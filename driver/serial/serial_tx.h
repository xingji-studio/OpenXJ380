#pragma once

#include <task/pcb.h>
#include <task/scheduler.h>

// Every accepted record owns bounded static storage. A normal task streams
// through that storage; its producer never waits while holding metadata locks.
static constexpr size_t kSerialTxRecords = 32;
static constexpr size_t kSerialTxRecordBytes = 1024;
static constexpr size_t kSerialTxChunkBytes = 128;
static constexpr size_t kSerialTxServiceBytes = 16;
static constexpr size_t kSerialTxBootstrapPollLimit = 4096;

struct SerialTxRecord
{
    char bytes[kSerialTxRecordBytes];
    size_t read, write, count;
    uint64_t generation;
    tcb_t owner;
    bool used, closed, log, raw_prompt, newline, truncated, uart_only;
};

static SerialTxRecord serial_tx_records[kSerialTxRecords];
static spin_t serial_tx_lock = SPIN_INIT;
// Only recovery and the before-TCB-free hook acquire this lifetime pin.
// Producers never acquire it, including producers holding scheduler locks.
static spin_t serial_tx_owner_lock = SPIN_INIT;
static size_t serial_tx_head, serial_tx_tail, serial_tx_count;
static uint64_t serial_tx_next_generation;
// Read-only diagnostic counters; publication/mutation remains under the queue
// lock. Normal acceptance checks can inspect these symbols without UART logs.
uint64_t serial_tx_dropped_records, serial_tx_dropped_bytes;
static uint64_t serial_tx_reported_records, serial_tx_reported_bytes;
static bool serial_tx_started, serial_tx_emergency, serial_tx_consumer_busy;
static tcb_t serial_tx_worker_task, serial_tx_consumer_task;

struct SerialTxConsumer
{
    char chunk[kSerialTxChunkBytes + 1];
    size_t length, sent;
    size_t slot;
    uint64_t generation;
    bool active, restore_prompt, suffix_done, console_done, notify;
    char suffix[96];
    size_t suffix_length, suffix_offset;
};
static SerialTxConsumer serial_tx_consumer;

static bool serial_tx_service_once();
static void serial_runtime_worker(void *);

extern bool no_interrupt;

extern "C" void serial_runtime_panic()
{
    // scheduler_stop() may run while any formatter/consumer is interrupted.
    // Notification must never acquire a lock, print, or depend on a worker.
    __atomic_store_n(&serial_tx_emergency, true, __ATOMIC_RELEASE);
}

static bool serial_tx_runtime()
{
    return __atomic_load_n(&serial_tx_started, __ATOMIC_ACQUIRE) && scheduler_global_enabled();
}

static bool serial_tx_can_wait(tcb_t task)
{
    if (!serial_tx_runtime()) return false;
    return task && task->task_level != TASK_IDLE_LEVEL &&
           task != __atomic_load_n(&serial_tx_worker_task, __ATOMIC_ACQUIRE) &&
           task != __atomic_load_n(&serial_tx_consumer_task, __ATOMIC_ACQUIRE) &&
           !no_interrupt && are_interrupts_enabled() && scheduler_is_enabled();
}

static void serial_tx_reap_head()
{
    // The lifetime pin blocks the mandatory before-free hook, while releasing
    // the queue lock before scanning CPU queues avoids queue->serial inversion.
    spin_lock(&serial_tx_owner_lock);
    tcb_t owner = nullptr;
    size_t slot = 0;
    uint64_t generation = 0;
    spin_lock(&serial_tx_lock);
    if (serial_tx_count)
    {
        slot = serial_tx_head;
        SerialTxRecord &record = serial_tx_records[slot];
        if (!record.closed) { owner = record.owner; generation = record.generation; }
    }
    spin_unlock(&serial_tx_lock);
    if (owner && (__atomic_load_n(&owner->status, __ATOMIC_ACQUIRE) == DEATH ||
                  __atomic_load_n(&owner->status, __ATOMIC_ACQUIRE) == OUT) &&
        !scheduler_task_referenced(owner))
    {
        spin_lock(&serial_tx_lock);
        SerialTxRecord &record = serial_tx_records[slot];
        if (record.used && record.generation == generation && record.owner == owner && !record.closed)
        {
            record.closed = record.truncated = true;
            record.owner = nullptr;
            ++serial_tx_dropped_records;
        }
        spin_unlock(&serial_tx_lock);
    }
    spin_unlock(&serial_tx_owner_lock);
}

void serial_task_quiescent_exit(struct thread_control_block *task)
{
    if (!task) return;
    spin_lock(&serial_tx_owner_lock);
    spin_lock(&serial_tx_lock);
    for (SerialTxRecord &record : serial_tx_records)
        if (record.used && record.owner == task)
        {
            record.owner = nullptr;
            if (!record.closed)
            {
                record.closed = record.truncated = true;
                ++serial_tx_dropped_records;
            }
        }
    spin_unlock(&serial_tx_lock);
    spin_unlock(&serial_tx_owner_lock);
}

struct SerialRecordWriter
{
    int slot = -1;
    uint64_t generation = 0;
    bool bootstrap = false, blocking = false, emergency = false, discarded = false, filtered = false;
    char chunk[kSerialTxChunkBytes + 1];
    size_t length = 0, lost_bytes = 0, bootstrap_stalls = 0;
    bool bootstrap_exhausted = false;
    bool prompt_enabled = false, prompt_possible = true;
    size_t prompt_length = 0;

    bool pause_producer()
    {
        if (!bootstrap) { scheduler_sleep_ns(1000000ULL); return true; }
        if (bootstrap_exhausted) return false;
        if (serial_tx_service_once()) { bootstrap_stalls = 0; return true; }
        if (++bootstrap_stalls >= kSerialTxBootstrapPollLimit)
        {
            bootstrap_exhausted = true;
            return false;
        }
        asm volatile("pause");
        return true;
    }

    SerialRecordWriter(bool log, bool raw_prompt = false, bool uart_only = false)
    {
        prompt_enabled = !log && !uart_only;
        emergency = __atomic_load_n(&serial_tx_emergency, __ATOMIC_ACQUIRE);
        if (log && !serial_logging_allowed()) { discarded = filtered = true; return; }
        if (emergency) return;
        bootstrap = !serial_tx_runtime();
        tcb_t owner = __atomic_load_n(&serial_tx_started, __ATOMIC_ACQUIRE) ? get_current_task() : nullptr;
        blocking = serial_tx_can_wait(owner);
        // Bootstrap callers help the bounded consumer themselves. A recursive
        // callback on the consumer must instead use nonwaiting admission.
        if (bootstrap && __atomic_load_n(&serial_tx_consumer_busy, __ATOMIC_ACQUIRE) &&
            owner == __atomic_load_n(&serial_tx_consumer_task, __ATOMIC_ACQUIRE)) bootstrap = false;
        for (;;)
        {
            spin_lock(&serial_tx_lock);
            if (serial_tx_count < kSerialTxRecords)
            {
                slot = (int)serial_tx_tail;
                SerialTxRecord &record = serial_tx_records[slot];
                record.read = record.write = record.count = 0;
                record.generation = generation = ++serial_tx_next_generation;
                record.owner = owner;
                record.used = true;
                record.closed = record.newline = record.truncated = false;
                record.log = log;
                record.raw_prompt = raw_prompt;
                record.uart_only = uart_only;
                serial_tx_tail = (serial_tx_tail + 1) % kSerialTxRecords;
                ++serial_tx_count;
                spin_unlock(&serial_tx_lock);
                return;
            }
            if (!blocking && !bootstrap)
            {
                ++serial_tx_dropped_records;
                spin_unlock(&serial_tx_lock);
                discarded = true;
                return;
            }
            spin_unlock(&serial_tx_lock);
            if (__atomic_load_n(&serial_tx_emergency, __ATOMIC_ACQUIRE))
            {
                emergency = true;
                return;
            }
            if (!pause_producer())
            {
                spin_lock(&serial_tx_lock);
                ++serial_tx_dropped_records;
                spin_unlock(&serial_tx_lock);
                discarded = true;
                return;
            }
        }
    }

    void flush()
    {
        if (!length) return;
        if (discarded) { length = 0; return; }
        if (emergency)
        {
            // Panic never waits for an interrupted metadata owner/consumer.
            // A broken UART has a finite polling fallback, independent of HPET.
            for (size_t index = 0; index < length; ++index)
            {
                unsigned polls = 0;
                while (!is_transmit_empty() && ++polls < 4096) asm volatile("pause");
                if (polls < 4096) outb(PORT, chunk[index]);
            }
            // Console/observer callbacks can own locks in the interrupted
            // context. Emergency diagnostics must not reenter those locks.
            length = 0;
            return;
        }
        size_t offset = 0;
        while (offset < length)
        {
            spin_lock(&serial_tx_lock);
            SerialTxRecord &record = serial_tx_records[slot];
            if (!record.used || record.generation != generation || record.closed)
            {
                spin_unlock(&serial_tx_lock);
                break;
            }
            if (record.truncated && !blocking && !bootstrap)
            {
                serial_tx_dropped_bytes += length - offset;
                spin_unlock(&serial_tx_lock);
                break;
            }
            size_t available = kSerialTxRecordBytes - record.count;
            size_t count = length - offset;
            if (count > available) count = available;
            for (size_t index = 0; index < count; ++index)
            {
                char byte = chunk[offset + index];
                record.bytes[record.write] = byte;
                record.write = (record.write + 1) % kSerialTxRecordBytes;
                if (byte == '\n') record.newline = true;
            }
            record.count += count;
            offset += count;
            if (offset < length && !blocking && !bootstrap)
            {
                if (!record.truncated) ++serial_tx_dropped_records;
                record.truncated = true;
                serial_tx_dropped_bytes += length - offset;
                spin_unlock(&serial_tx_lock);
                break;
            }
            spin_unlock(&serial_tx_lock);
            if (offset < length)
            {
                if (__atomic_load_n(&serial_tx_emergency, __ATOMIC_ACQUIRE)) break;
                if (!pause_producer())
                {
                    spin_lock(&serial_tx_lock);
                    if (!record.truncated) ++serial_tx_dropped_records;
                    record.truncated = true;
                    serial_tx_dropped_bytes += length - offset;
                    spin_unlock(&serial_tx_lock);
                    break;
                }
            }
        }
        length = 0;
    }

    void put(char byte)
    {
        if (discarded) { if (!filtered) ++lost_bytes; return; }
        if (prompt_enabled && prompt_possible)
        {
            if (prompt_length < sizeof(serial_shell_prompt) - 1 && byte == serial_shell_prompt[prompt_length])
                ++prompt_length;
            else prompt_possible = false;
        }
        chunk[length++] = byte;
        if (length == kSerialTxChunkBytes) flush();
    }

    void append(const char *text)
    {
        if (text) while (*text) put(*text++);
    }

    ~SerialRecordWriter()
    {
        flush();
        if (lost_bytes)
        {
            spin_lock(&serial_tx_lock);
            serial_tx_dropped_bytes += lost_bytes;
            spin_unlock(&serial_tx_lock);
        }
        if (slot < 0) return;
        spin_lock(&serial_tx_lock);
        SerialTxRecord &record = serial_tx_records[slot];
        if (record.used && record.generation == generation)
        {
            if (prompt_enabled && prompt_possible && prompt_length == sizeof(serial_shell_prompt) - 1)
                record.raw_prompt = true;
            record.closed = true;
            record.owner = nullptr;
        }
        spin_unlock(&serial_tx_lock);
        if (bootstrap)
        {
            for (;;)
            {
                spin_lock(&serial_tx_lock);
                bool pending = record.used && record.generation == generation;
                spin_unlock(&serial_tx_lock);
                if (!pending || __atomic_load_n(&serial_tx_emergency, __ATOMIC_ACQUIRE)) break;
                if (!pause_producer()) break;
            }
        }
    }
};

static uint8_t serial_record_write_handler(Writer *writer, char byte)
{
    static_cast<SerialRecordWriter *>(writer->data)->put(byte);
    return 0;
}

static bool serial_tx_service_once()
{
    if (__atomic_load_n(&serial_tx_emergency, __ATOMIC_ACQUIRE)) return false;
    bool runtime = serial_tx_runtime();
    if (runtime && (!are_interrupts_enabled() || !scheduler_is_enabled() || no_interrupt)) return false;
    bool expected = false;
    if (!__atomic_compare_exchange_n(&serial_tx_consumer_busy, &expected, true, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return false;
    // Started-but-not-yet-scheduled bootstrap already has valid TCB identity.
    // A recursive observer must recognize this consumer and never wait on it.
    __atomic_store_n(&serial_tx_consumer_task,
                     __atomic_load_n(&serial_tx_started, __ATOMIC_ACQUIRE) ? get_current_task() : nullptr,
                     __ATOMIC_RELEASE);
    serial_tx_reap_head();
    SerialTxConsumer &consumer = serial_tx_consumer;
    bool progress = false;
    bool record_finished = false;
    if (!consumer.active && consumer.length == 0)
    {
        spin_lock(&serial_tx_lock);
        if (serial_tx_count)
        {
            SerialTxRecord &record = serial_tx_records[serial_tx_head];
            consumer = {};
            consumer.active = true;
            consumer.slot = serial_tx_head;
            consumer.generation = record.generation;
            consumer.notify = !record.uart_only;
            consumer.restore_prompt = record.log && serial_prompt_active;
            if (record.log) serial_prompt_active = false;
            if (consumer.restore_prompt && !serial_line_start)
            {
                consumer.chunk[0] = '\n';
                consumer.length = 1;
            }
        }
        spin_unlock(&serial_tx_lock);
    }
    if (consumer.active && consumer.length == 0)
    {
        spin_lock(&serial_tx_lock);
        SerialTxRecord &record = serial_tx_records[consumer.slot];
        size_t count = record.count;
        if (count > kSerialTxChunkBytes) count = kSerialTxChunkBytes;
        for (size_t index = 0; index < count; ++index)
        {
            consumer.chunk[index] = record.bytes[record.read];
            record.read = (record.read + 1) % kSerialTxRecordBytes;
        }
        record.count -= count;
        consumer.length = count;
        if (!count && record.closed)
        {
            if (!consumer.suffix_done)
            {
                // Prepare only bounded control text while protected; no
                // formatter or callback runs under the queue lock.
                size_t n = 0;
                if (record.truncated)
                {
                    const char *notice = "\n[serial: record truncated]\n";
                    while (*notice) consumer.chunk[n++] = *notice++;
                }
                if (consumer.restore_prompt)
                {
                    if (!serial_line_start && n == 0) consumer.chunk[n++] = '\n';
                    const char *prompt = serial_shell_prompt;
                    while (*prompt) consumer.chunk[n++] = *prompt++;
                    serial_prompt_active = true;
                }
                else if (!record.uart_only && record.raw_prompt) serial_prompt_active = true;
                else if (!record.uart_only && serial_prompt_active && record.newline) serial_prompt_active = false;
                consumer.length = n;
                consumer.suffix_done = true;
            }
            if (!consumer.length)
            {
                record.used = false;
                serial_tx_head = (serial_tx_head + 1) % kSerialTxRecords;
                --serial_tx_count;
                consumer.active = false;
                progress = true;
                record_finished = true;
            }
        }
        spin_unlock(&serial_tx_lock);
    }
    if (!consumer.active && !consumer.length)
    {
        uint64_t records, bytes;
        spin_lock(&serial_tx_lock);
        records = serial_tx_dropped_records - serial_tx_reported_records;
        bytes = serial_tx_dropped_bytes - serial_tx_reported_bytes;
        serial_tx_reported_records = serial_tx_dropped_records;
        serial_tx_reported_bytes = serial_tx_dropped_bytes;
        spin_unlock(&serial_tx_lock);
        if (records || bytes)
        {
            int size = snprintf(consumer.chunk, sizeof(consumer.chunk),
                                "\n[serial: dropped records=%llu bytes=%llu]\n", records, bytes);
            consumer.length = size > 0 ? (size_t)size : 0;
            if (consumer.length > kSerialTxChunkBytes) consumer.length = kSerialTxChunkBytes;
            consumer.sent = 0;
            consumer.console_done = false;
            consumer.notify = true;
        }
    }
    if (consumer.length)
    {
        consumer.chunk[consumer.length] = '\0';
        if (!consumer.console_done)
        {
#if !OPENXJ380_INPUT_OUTPUT_DISABLED
            if (consumer.notify) console_write(consumer.chunk);
#endif
            consumer.console_done = true;
        }
        if (is_transmit_empty())
        {
            size_t count = consumer.length - consumer.sent;
            if (count > kSerialTxServiceBytes) count = kSerialTxServiceBytes;
            if (count > serial_tx_fifo_bytes) count = serial_tx_fifo_bytes;
            while (count--)
            {
                char byte = consumer.chunk[consumer.sent++];
                outb(PORT, byte);
                if (consumer.notify) serial_line_start = byte == '\n';
                progress = true;
            }
        }
        if (consumer.sent == consumer.length)
        {
            if (consumer.notify) notify_serial_output_observer(consumer.chunk);
            consumer.length = consumer.sent = 0;
            consumer.console_done = false;
            if (!consumer.active) record_finished = true;
        }
    }
    if (record_finished && consumer.notify) notify_serial_output_observer(nullptr);
    __atomic_store_n(&serial_tx_consumer_task, (tcb_t)nullptr, __ATOMIC_RELEASE);
    __atomic_store_n(&serial_tx_consumer_busy, false, __ATOMIC_RELEASE);
    return progress;
}

static void serial_runtime_worker(void *)
{
    __atomic_store_n(&serial_tx_worker_task, get_current_task(), __ATOMIC_RELEASE);
    for (;;)
    {
        serial_tx_service_once();
        scheduler_sleep_ns(1000000ULL);
    }
}

bool serial_runtime_start()
{
    if (__atomic_load_n(&serial_tx_started, __ATOMIC_ACQUIRE)) return true;
    int64_t tid = (int64_t)create_kernel_thread((void *)serial_runtime_worker, nullptr, (char *)"serial-tx", nullptr);
    if (tid < 0) return false;
    __atomic_store_n(&serial_tx_started, true, __ATOMIC_RELEASE);
    return true;
}

static void serial_tx_reset()
{
    serial_tx_lock = serial_tx_owner_lock = SPIN_INIT;
    serial_tx_head = serial_tx_tail = serial_tx_count = 0;
    serial_tx_next_generation = 0;
    serial_tx_dropped_records = serial_tx_dropped_bytes = 0;
    serial_tx_reported_records = serial_tx_reported_bytes = 0;
    serial_tx_started = serial_tx_emergency = serial_tx_consumer_busy = false;
    serial_tx_worker_task = serial_tx_consumer_task = nullptr;
    serial_tx_consumer = {};
    for (SerialTxRecord &record : serial_tx_records)
    {
        record.used = record.closed = false;
        record.owner = nullptr;
    }
}
