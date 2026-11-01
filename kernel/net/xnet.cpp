// SPDX-License-Identifier: Apache-2.0
// Native networking: process/image handles and heap operation tickets. Only a
// kernel-owned worker enters the stack, so terminating an application cannot
// abandon lwIP's core mutex. Locks protect publication, never backend calls,
// user copies, allocation, destruction or sleeping.
#include <dlinker.h>
#include <net/xnet_provider.h>
#include <syscall/signal.h>
#include <syscall/xapi_user.h>
#include <task/scheduler.h>
#include <user/permissions.h>
#include <xnet_abi.h>

namespace {
enum Kind
{
    Empty,
    Socket,
    Cancel,
    Resolver,
    Watch
};
enum Phase
{
    Building,
    Queued,
    Running,
    Done
};
struct Object
{
    uint64_t          id;
    pcb_t             owner;
    page_directory_t *image;
    Kind              kind;
    uint32_t          refs, gates;
    bool              closed, signaled, destroying;
    void             *impl;
    XNetOpenDesc      desc;
    XNetSocketInfo    socket;
    Object           *cancel;
    uint64_t          deadline, seen;
    int64_t           result;
    bool              complete;
    uint16_t          port;
    XNetEndpoint      resolved;
};
union Payload
{
    XNetOpenDesc        open;
    XNetEndpoint        endpoint;
    XNetSocketInfo      socket;
    XNetCapabilities    caps;
    XNetTcpOptions      tcp;
    XNetUdpOptions      udp;
    XNetResolveDesc     resolve;
    XNetInterfaceInfo   interface;
    XNetInterfaceConfig config;
    XNetDnsConfig       dns;
    XNetChange          change;
};
struct Job
{
    Job              *next;
    tcb_t             owner;
    pcb_t             process;
    page_directory_t *image;
    XNetCall          call;
    Phase             phase;
    bool              abandoned, delivered, begun, revoked;
    uint32_t          gate;
    uint64_t          start, turn;
    int64_t           result;
    Object           *object, *cancel, *created;
    Object           *watched[XNET_MAX_WAIT_ITEMS];
    XNetWaitItem      items[XNET_MAX_WAIT_ITEMS];
    Payload           payload;
    XNetEndpoint      endpoint;
    XNetDatagramInfo  datagram;
    char              hostname[XNET_MAX_HOST_BYTES];
    void             *data;
    uint32_t          required;
};
spin_t               registry_lock = SPIN_INIT;
Object               objects[XNET_MAX_OBJECTS];
Job                 *jobs;
XNetProvider         provider;
bool                 registered;
int                  worker_state;
uint64_t             next_id, turn;
XNetInterfaceInfo    interface_info;
XNetInterfaceAddress interface_addresses[3];
XNetDnsConfig        dns_config;
uint32_t             address_count;
uint64_t             generation = 1;

bool is_closed(const Object *o)
{
    return __atomic_load_n(&o->closed, __ATOMIC_ACQUIRE);
}

bool cancelled(const Object *o)
{
    return o && (is_closed(o) || __atomic_load_n(&o->signaled, __ATOMIC_ACQUIRE));
}

Object *find_locked(pcb_t owner, page_directory_t *image, uint64_t id)
{
    if (!id) return NULL;
    for (Object &o : objects)
        if (o.id == id && o.owner == owner && o.image == image && !is_closed(&o)) return &o;
    return NULL;
}

void unpin_locked(Object *o)
{
    if (o && o->refs) --o->refs;
}

void retire_locked(Object *o)
{
    if (o && !is_closed(o))
    {
        __atomic_store_n(&o->closed, true, __ATOMIC_RELEASE);
        if (o->kind == Cancel) __atomic_store_n(&o->signaled, true, __ATOMIC_RELEASE);
        unpin_locked(o); // Drop the handle table's ownership, retain operations.
    }
}

Object *create_object(Job *job, Kind kind)
{
    Object *result = NULL;
    spin_lock(&registry_lock);
    for (Object &o : objects)
    {
        if (job->revoked) break;
        if (o.kind != Empty) continue;
        if (next_id == ~uint64_t(0)) break;
        o            = {};
        o.id         = ++next_id;
        o.owner      = job->process;
        o.image      = job->image;
        o.kind       = kind;
        o.refs       = 2; // Handle table and creator ticket until copyout finishes.
        job->created = result = &o;
        break;
    }
    spin_unlock(&registry_lock);
    return result;
}

void transfer_cancel(Job *job, Object *o)
{
    spin_lock(&registry_lock);
    o->cancel = job->cancel;
    if (o->cancel) ++o->cancel->refs;
    spin_unlock(&registry_lock);
}

void destroy_job(Job *job)
{
    spin_lock(&registry_lock);
    if (job->created && !job->delivered) retire_locked(job->created);
    unpin_locked(job->created);
    if (job->object) job->object->gates &= ~job->gate;
    unpin_locked(job->object);
    unpin_locked(job->cancel);
    for (uint32_t i = 0; i < job->call.count && i < XNET_MAX_WAIT_ITEMS; ++i)
        if (job->call.operation == XNET_OP_WAIT) unpin_locked(job->watched[i]);
    spin_unlock(&registry_lock);
    free(job->data);
    free(job);
}

void sweep()
{
    // Completed/orphan tickets are detached before any buffer/object release.
    for (;;)
    {
        Job *dead = NULL;
        spin_lock(&registry_lock);
        Job **link = &jobs;
        while (*link)
        {
            if ((*link)->abandoned && (*link)->phase != Running)
            {
                dead  = *link;
                *link = dead->next;
                break;
            }
            link = &(*link)->next;
        }
        spin_unlock(&registry_lock);
        if (!dead) break;
        destroy_job(dead);
    }
    for (Object &o : objects)
    {
        spin_lock(&registry_lock);
        bool destroy = o.kind != Empty && is_closed(&o) && !o.refs && !o.destroying;
        if (destroy) o.destroying = true;
        spin_unlock(&registry_lock);
        if (!destroy) continue;
        if (o.impl)
        {
            if (o.kind == Socket) provider.close(o.impl);
            if (o.kind == Resolver) provider.resolve_release(o.impl);
        }
        spin_lock(&registry_lock);
        unpin_locked(o.cancel);
        o = {};
        spin_unlock(&registry_lock);
    }
}

void refresh_interface()
{
    if (!__atomic_load_n(&registered, __ATOMIC_ACQUIRE)) return;
    XNetInterfaceInfo    info         = {};
    XNetInterfaceAddress addresses[3] = {};
    XNetDnsConfig        dns          = {};
    uint32_t             count        = 3;
    if (provider.interface_info(&info, addresses, &count, &dns) < 0 || count > 3) return;
    info.generation = 0;
    if (memcmp(&info, &interface_info, sizeof(info)) || count != address_count ||
        memcmp(addresses, interface_addresses, sizeof(addresses)) || memcmp(&dns, &dns_config, sizeof(dns)))
    {
        ++generation;
        interface_info = info;
        memcpy(interface_addresses, addresses, sizeof(addresses));
        dns_config    = dns;
        address_count = count;
    }
}

void fail_connect(Object *o, int error)
{
    if (o->impl) provider.close(o->impl);
    o->impl                     = NULL;
    o->socket.state             = XNET_FAILED;
    o->socket.connection_result = error;
    o->socket.events            = XNET_ERROR;
}

void refresh_objects()
{
    uint64_t now = nanoTime();
    for (Object &o : objects)
    {
        // This worker alone mutates backend state. Pins need only prevent slot
        // reuse, while the atomic closed flag also permits exec/reaper revoke.
        if (o.kind == Empty || is_closed(&o)) continue;
        if (o.kind == Socket && o.impl)
        {
            if (o.socket.state == XNET_CONNECTING && (cancelled(o.cancel) || (o.deadline && now >= o.deadline)))
                fail_connect(&o, cancelled(o.cancel) ? int(XNET_CANCELLED) : -ETIMEDOUT);
            else
            {
                XNetSocketInfo status = o.socket;
                if (provider.query(o.impl, &status) == 0) o.socket = status;
            }
            if (o.socket.state != XNET_CONNECTING && o.cancel)
            {
                spin_lock(&registry_lock);
                unpin_locked(o.cancel);
                o.cancel = NULL;
                spin_unlock(&registry_lock);
            }
        }
        if (o.kind == Resolver && !o.complete)
        {
            int result;
            if (cancelled(o.cancel))
                result = int(XNET_CANCELLED);
            else if (now >= o.deadline)
                result = -ETIMEDOUT;
            else
                result = provider.resolve_poll(o.impl, &o.resolved.address);
            if (result == -EINPROGRESS) continue;
            o.result        = result;
            o.complete      = true;
            o.resolved.port = o.port;
            provider.resolve_release(o.impl);
            o.impl = NULL;
            spin_lock(&registry_lock);
            unpin_locked(o.cancel);
            o.cancel = NULL;
            spin_unlock(&registry_lock);
        }
    }
}

int expired(const Job *job)
{
    if (cancelled(job->cancel)) return int(XNET_CANCELLED);
    if (job->call.io.timeout_ms > 0 && nanoTime() - job->start >= uint64_t(job->call.io.timeout_ms) * 1000000ULL)
        return -ETIMEDOUT;
    return 0;
}

uint32_t ready(Object *o)
{
    if (!o) return XNET_INVALID;
    if (is_closed(o)) return XNET_CLOSED;
    switch (o->kind)
    {
    case Socket: return o->socket.events;
    case Cancel: return cancelled(o) ? XNET_DONE : 0;
    case Resolver: return o->complete ? XNET_DONE | (o->result < 0 ? XNET_ERROR : 0) : 0;
    case Watch: return o->seen != generation ? XNET_CHANGED : 0;
    default: return XNET_INVALID;
    }
}

// Return true only when a result is final. Pending calls are revisited once
// per worker pass, using one absolute budget rather than resetting on retries.
bool execute(Job *j)
{
    const XNetCall &c       = j->call;
    Object         *o       = j->object;
    uint32_t        op      = c.operation;
    bool            present = __atomic_load_n(&registered, __ATOMIC_ACQUIRE);
    j->result               = 0;
    if (__atomic_load_n(&j->revoked, __ATOMIC_ACQUIRE))
    {
        j->result = XNET_CANCELLED;
        return true;
    }
    if (o && is_closed(o))
    {
        j->result = XNET_CANCELLED;
        return true;
    }
    if (op != XNET_OP_CLOSE && op != XNET_OP_CONNECT && cancelled(j->cancel))
    {
        j->result = XNET_CANCELLED;
        return true;
    }
    if (op != XNET_OP_WAIT && op != XNET_OP_CONNECT && c.io.timeout_ms > 0)
        if (int error = expired(j))
        {
            j->result = error;
            return true;
        }
    switch (op)
    {
    case XNET_OP_CAPS:
        j->payload.caps = {
            sizeof(XNetCapabilities),
            XNET_ABI_VERSION,
            __atomic_load_n(&registered, __ATOMIC_ACQUIRE) ? 1U : 0U,
            XNET_MAX_OBJECTS,
            present ? provider.features : 0,
            XNET_MAX_WAIT_ITEMS,
            XNET_MAX_TRANSFER_BYTES,
            present ? 1U : 0U,
            present ? 1U : 0U,
            {0, 0}
        };
        return true;
    case XNET_OP_CANCEL_CREATE:
    case XNET_OP_WATCH_CREATE:
        if (op == XNET_OP_WATCH_CREATE && c.argument > 1)
        {
            j->result = -ENODEV;
            return true;
        }
        o = create_object(j, op == XNET_OP_CANCEL_CREATE ? Cancel : Watch);
        if (!o)
            j->result = -ENFILE;
        else
            o->seen = generation;
        return true;
    case XNET_OP_CANCEL_SIGNAL: __atomic_store_n(&o->signaled, true, __ATOMIC_RELEASE); return true;
    case XNET_OP_CLOSE:
        spin_lock(&registry_lock);
        retire_locked(o);
        spin_unlock(&registry_lock);
        return true;
    case XNET_OP_WAIT: {
        int count = 0;
        for (uint32_t i = 0; i < c.count; ++i)
        {
            uint32_t events    = ready(j->watched[i]);
            j->items[i].ready  = events & (j->items[i].events | XNET_ERROR | XNET_CLOSED | XNET_INVALID);
            count             += j->items[i].ready != 0;
        }
        if (count || !c.io.timeout_ms || expired(j))
        {
            j->result = count;
            return true;
        }
        return false;
    }
    case XNET_OP_OPEN:
        if (!present)
        {
            j->result = -ENODEV;
            return true;
        }
        o = create_object(j, Socket);
        if (!o)
        {
            j->result = -ENFILE;
            return true;
        }
        o->desc                = j->payload.open;
        o->socket              = {};
        o->socket.size         = sizeof(XNetSocketInfo);
        o->socket.version      = XNET_ABI_VERSION;
        o->socket.family       = o->desc.family;
        o->socket.transport    = o->desc.transport;
        o->socket.interface_id = 1;
        o->socket.state        = XNET_CREATED;
        j->result              = provider.open(&o->desc, &o->impl);
        return true;
    case XNET_OP_BIND:
        j->result = provider.bind(o->impl, &j->payload.endpoint);
        if (!j->result) o->socket.state = XNET_BOUND;
        return true;
    case XNET_OP_LISTEN:
        j->result = provider.listen(o->impl, c.argument);
        if (!j->result) o->socket.state = XNET_LISTENING;
        return true;
    case XNET_OP_CONNECT:
        if (cancelled(j->cancel))
        {
            if (j->begun) fail_connect(o, int(XNET_CANCELLED));
            j->result = XNET_CANCELLED;
            return true;
        }
        if (int error = expired(j))
        {
            if (j->begun) fail_connect(o, error);
            j->result = error;
            return true;
        }
        if (!j->begun)
        {
            if (o->socket.state == XNET_CONNECTING)
            {
                j->result = -EALREADY;
                return true;
            }
            if (o->socket.state == XNET_CONNECTED)
            {
                j->result = -EISCONN;
                return true;
            }
            if (o->socket.state == XNET_FAILED)
            {
                j->result = o->socket.connection_result;
                return true;
            }
            j->result = provider.connect(o->impl, &j->payload.endpoint);
            if (j->result != -EINPROGRESS && j->result != -EALREADY)
            {
                o->socket.connection_result = j->result;
                o->socket.state             = j->result ? XNET_FAILED : XNET_CONNECTED;
                o->socket.events            = j->result ? XNET_ERROR : XNET_CONNECTION_READY;
                return true;
            }
            j->begun                    = true;
            o->socket.state             = XNET_CONNECTING;
            o->socket.connection_result = -EINPROGRESS;
            transfer_cancel(j, o);
            o->deadline = c.io.timeout_ms > 0 ? j->start + uint64_t(c.io.timeout_ms) * 1000000ULL : 0;
        }
        if (o->socket.state != XNET_CONNECTING)
        {
            j->result = o->socket.connection_result;
            return true;
        }
        if (!c.io.timeout_ms)
        {
            j->result = -EINPROGRESS;
            return true;
        }
        if (int error = expired(j))
        {
            fail_connect(o, error);
            j->result = error;
            return true;
        }
        return false;
    case XNET_OP_ACCEPT: {
        if (o->socket.state != XNET_LISTENING)
        {
            j->result = -EINVAL;
            return true;
        }
        void *impl = NULL;
        j->result  = provider.accept(o->impl, &impl, &j->endpoint);
        if (!j->result)
        {
            Object *accepted = create_object(j, Socket);
            if (!accepted)
            {
                provider.close(impl);
                j->result = -ENFILE;
                return true;
            }
            accepted->impl          = impl;
            accepted->desc          = o->desc;
            accepted->socket        = o->socket;
            accepted->socket.state  = XNET_CONNECTED;
            accepted->socket.events = XNET_CONNECTION_READY;
            accepted->socket.peer   = j->endpoint;
            provider.query(impl, &accepted->socket);
        }
        break;
    }
    case XNET_OP_SEND:
    case XNET_OP_RECEIVE:
    case XNET_OP_SEND_TO:
    case XNET_OP_RECEIVE_FROM: {
        bool send     = op == XNET_OP_SEND || op == XNET_OP_SEND_TO;
        bool datagram = o->desc.transport == XNET_UDP;
        if (o->socket.state == XNET_FAILED)
        {
            j->result = o->socket.connection_result;
            return true;
        }
        if (!o->impl)
        {
            j->result = -ENOTCONN;
            return true;
        }
        if ((op == XNET_OP_RECEIVE_FROM || op == XNET_OP_SEND_TO) && !datagram)
        {
            j->result = -EPROTOTYPE;
            return true;
        }
        if (!datagram && !c.count) return true;
        if (!datagram && o->socket.state != XNET_CONNECTED)
        {
            j->result = -ENOTCONN;
            return true;
        }
        if (send && (o->socket.shutdown & XNET_WRITE_CLOSED))
        {
            j->result = -ESHUTDOWN;
            return true;
        }
        if (!send && (o->socket.shutdown & XNET_READ_CLOSED)) return true;
        if (send)
            j->result = provider.send(o->impl, j->data, c.count, op == XNET_OP_SEND_TO ? &j->endpoint : NULL);
        else
        {
            j->datagram = {sizeof(XNetDatagramInfo), XNET_ABI_VERSION, 1, 0, 0, {}};
            j->result   = provider.receive(o->impl, j->data, c.count, &j->datagram);
        }
        break;
    }
    case XNET_OP_SHUTDOWN:
        if (!o->impl)
        {
            j->result = -ENOTCONN;
            return true;
        }
        j->result = provider.shutdown(o->impl, c.argument);
        if (!j->result) o->socket.shutdown |= c.argument;
        break;
    case XNET_OP_QUERY: j->payload.socket = o->socket; return true;
    case XNET_OP_TCP_GET:
    case XNET_OP_TCP_SET:
        if (o->desc.transport != XNET_TCP)
        {
            j->result = -EPROTOTYPE;
            return true;
        }
        if (!o->impl)
        {
            j->result = -ENOTCONN;
            return true;
        }
        j->result = provider.tcp_options(o->impl, &j->payload.tcp, op == XNET_OP_TCP_SET);
        return true;
    case XNET_OP_UDP_GET:
    case XNET_OP_UDP_SET:
        if (o->desc.transport != XNET_UDP)
        {
            j->result = -EPROTOTYPE;
            return true;
        }
        j->result = provider.udp_options(o->impl, &j->payload.udp, op == XNET_OP_UDP_SET);
        return true;
    case XNET_OP_RESOLVE_START:
        if (!present)
        {
            j->result = -ENODEV;
            return true;
        }
        o = create_object(j, Resolver);
        if (!o)
        {
            j->result = -ENFILE;
            return true;
        }
        o->deadline = j->start + uint64_t(j->payload.resolve.total_timeout_ms) * 1000000ULL;
        o->port     = j->payload.resolve.port;
        transfer_cancel(j, o);
        j->result = provider.resolve_start(j->hostname, j->payload.resolve.family, &o->impl);
        return true;
    case XNET_OP_RESOLVE_RESULT:
        if (!o->complete)
        {
            j->result = -EINPROGRESS;
            return true;
        }
        if (o->result)
        {
            j->result = o->result;
            return true;
        }
        j->required         = 1;
        j->payload.endpoint = o->resolved;
        j->result           = c.count && c.count < j->required ? -ENOBUFS : 0;
        return true;
    case XNET_OP_ENUM: j->required = present ? 1 : 0; return true;
    case XNET_OP_INTERFACE:
        if (!present)
        {
            j->result = -ENODEV;
            return true;
        }
        j->payload.interface            = interface_info;
        j->payload.interface.generation = generation;
        return true;
    case XNET_OP_ADDRESSES:
        if (!present)
        {
            j->result = -ENODEV;
            return true;
        }
        j->required = address_count;
        if (c.count && c.count < address_count)
            j->result = -ENOBUFS;
        else if (c.count)
            memcpy(j->data, interface_addresses, address_count * sizeof(XNetInterfaceAddress));
        return true;
    case XNET_OP_DNS_GET:
        if (!present)
        {
            j->result = -ENODEV;
            return true;
        }
        j->payload.dns = dns_config;
        return true;
    case XNET_OP_CONFIG_APPLY:
        j->result = present ? provider.apply_interface(&j->payload.config) : -ENODEV;
        refresh_interface();
        return true;
    case XNET_OP_DNS_APPLY:
        j->result = present ? provider.apply_dns(&j->payload.dns) : -ENODEV;
        refresh_interface();
        return true;
    case XNET_OP_WATCH_READ:
        if (o->seen == generation)
        {
            j->result = -EAGAIN;
            return true;
        }
        j->payload.change = {sizeof(XNetChange), XNET_ABI_VERSION, 1, generation - o->seen > 1 ? XNET_CHANGE_RESYNC : 0,
                             generation};
        o->seen           = generation;
        return true;
    default: j->result = -ENOSYS; return true;
    }
    if (j->result != -EAGAIN && j->result != -EWOULDBLOCK) return true;
    if (!c.io.timeout_ms) return true;
    if (int error = expired(j))
    {
        j->result = error;
        return true;
    }
    return false;
}

void pump()
{
    ++turn;
    sweep();
    refresh_interface();
    refresh_objects();
    for (;;)
    {
        Job *job = NULL;
        spin_lock(&registry_lock);
        for (Job *candidate = jobs; candidate; candidate = candidate->next)
        {
            if (candidate->phase == Queued && !candidate->abandoned && candidate->turn != turn)
            {
                candidate->phase = Running;
                candidate->turn  = turn;
                job              = candidate;
                break;
            }
        }
        spin_unlock(&registry_lock);
        if (!job) break;
        bool done = execute(job);
        spin_lock(&registry_lock);
        if (done && job->created && job->result < 0) retire_locked(job->created);
        if (done && job->object)
        {
            job->object->gates &= ~job->gate;
            job->gate           = 0;
        }
        job->phase = done ? Done : Queued;
        spin_unlock(&registry_lock);
        // Publication transfers ownership; do not access job after unlock.
    }
    sweep();
}

void worker(void *)
{
    for (;;)
    {
        pump();
        scheduler_sleep_ns(1000000ULL);
    }
}

int ensure_worker()
{
    int expected = 0;
    if (__atomic_compare_exchange_n(&worker_state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        int64_t id = int64_t(create_kernel_thread((void *)worker, NULL, (char *)"xnet", NULL));
        __atomic_store_n(&worker_state, id > 0 ? 2 : 0, __ATOMIC_RELEASE);
        return id > 0 ? 0 : -ENOMEM;
    }
    while (__atomic_load_n(&worker_state, __ATOMIC_ACQUIRE) == 1)
        scheduler_sleep_ns(1000000ULL);
    return __atomic_load_n(&worker_state, __ATOMIC_ACQUIRE) == 2 ? 0 : -ENOMEM;
}

template <typename T>
int input(Job *j, T &value, uint64_t address)
{
    if (!copy_from_user_pagedir(j->image, &value, (void *)address, sizeof(T))) return -EFAULT;
    if (value.size != sizeof(T) || value.version != XNET_ABI_VERSION) return -EINVAL;
    return 0;
}

int output_range(Job *j, uint64_t address, uint64_t bytes)
{
    if (!bytes) return 0;
    if (!user_range_mapped(j->image, (void *)address, bytes)) return -EFAULT;
    uint64_t end = address + bytes - 1;
    for (uint64_t page = address & PAGE_MASK;; page += PAGE_SIZE)
    {
        uint64_t flags;
        if (!page_table_get_flags(j->image, page, &flags) || !(flags & PTE_WRITEABLE)) return -EFAULT;
        if (page == (end & PAGE_MASK)) break;
    }
    return 0;
}

int endpoint_input(Job *j, XNetEndpoint &endpoint, uint64_t address)
{
    if (!copy_from_user_pagedir(j->image, &endpoint, (void *)address, sizeof(endpoint))) return -EFAULT;
    if (endpoint.reserved16 || endpoint.reserved32) return -EINVAL;
    if (endpoint.address.family == XNET_IPV4)
    {
        if (endpoint.address.scope_id) return -EINVAL;
        for (unsigned i = 4; i < 16; ++i)
            if (endpoint.address.bytes[i]) return -EINVAL;
    }
    else if (endpoint.address.family != XNET_IPV6)
        return -EAFNOSUPPORT;
    return 0;
}

int prepare(Job *j)
{
    XNetCall &c = j->call;
    if (c.size != sizeof(c) || c.version != XNET_ABI_VERSION || c.flags || c.operation > XNET_OP_UDP_GET)
        return -EINVAL;
    if (c.io.size != sizeof(c.io) || c.io.version != XNET_ABI_VERSION || c.io.reserved || c.io.timeout_ms < -1)
        return -EINVAL;
    int      error        = 0;
    uint64_t output_bytes = 0;
    switch (c.operation)
    {
    case XNET_OP_CAPS:
        error        = input(j, j->payload.caps, c.output);
        output_bytes = sizeof(XNetCapabilities);
        break;
    case XNET_OP_OPEN:
        error = input(j, j->payload.open, c.input);
        if (!error && (j->payload.open.flags || j->payload.open.reserved[0] || j->payload.open.reserved[1]))
            error = -EINVAL;
        if (!error && j->payload.open.interface_id > 1) error = -ENODEV;
        if (!error && j->payload.open.family != XNET_IPV4 && j->payload.open.family != XNET_IPV6) error = -EAFNOSUPPORT;
        if (!error && j->payload.open.transport != XNET_TCP && j->payload.open.transport != XNET_UDP)
            error = -EPROTONOSUPPORT;
        output_bytes = sizeof(uint64_t);
        break;
    case XNET_OP_BIND:
    case XNET_OP_CONNECT: error = endpoint_input(j, j->payload.endpoint, c.input); break;
    case XNET_OP_LISTEN:
        if (!c.argument || c.argument > 255) error = -EINVAL;
        break;
    case XNET_OP_ACCEPT:
        output_bytes = sizeof(uint64_t);
        if (c.auxiliary) error = output_range(j, c.auxiliary, sizeof(XNetEndpoint));
        break;
    case XNET_OP_SEND:
    case XNET_OP_RECEIVE:
    case XNET_OP_SEND_TO:
    case XNET_OP_RECEIVE_FROM:
        if (c.count > XNET_MAX_TRANSFER_BYTES) return -EMSGSIZE;
        j->data = malloc(c.count ? c.count : 1);
        if (!j->data) return -ENOMEM;
        if (c.operation == XNET_OP_SEND || c.operation == XNET_OP_SEND_TO)
        {
            if (!copy_from_user_pagedir(j->image, j->data, (void *)c.input, c.count)) error = -EFAULT;
            if (!error && c.operation == XNET_OP_SEND_TO) error = endpoint_input(j, j->endpoint, c.auxiliary);
        }
        else
        {
            output_bytes = c.count;
            if (c.operation == XNET_OP_RECEIVE_FROM)
            {
                error = input(j, j->datagram, c.auxiliary);
                if (!error) error = output_range(j, c.auxiliary, sizeof(XNetDatagramInfo));
            }
        }
        break;
    case XNET_OP_SHUTDOWN:
        if (!c.argument || c.argument > XNET_SHUT_BOTH) error = -EINVAL;
        break;
    case XNET_OP_QUERY:
        error        = input(j, j->payload.socket, c.output);
        output_bytes = sizeof(XNetSocketInfo);
        break;
    case XNET_OP_TCP_GET:
    case XNET_OP_TCP_SET:
        error = input(j, j->payload.tcp, c.operation == XNET_OP_TCP_GET ? c.output : c.input);
        if (!error && j->payload.tcp.reserved) error = -EINVAL;
        if (c.operation == XNET_OP_TCP_GET) output_bytes = sizeof(XNetTcpOptions);
        break;
    case XNET_OP_UDP_GET:
    case XNET_OP_UDP_SET:
        error = input(j, j->payload.udp, c.operation == XNET_OP_UDP_GET ? c.output : c.input);
        if (!error && j->payload.udp.reserved) error = -EINVAL;
        if (c.operation == XNET_OP_UDP_GET) output_bytes = sizeof(XNetUdpOptions);
        break;
    case XNET_OP_WAIT:
        if (c.count > XNET_MAX_WAIT_ITEMS) return -EINVAL;
        output_bytes = c.count * sizeof(XNetWaitItem);
        if (!copy_from_user_pagedir(j->image, j->items, (void *)c.output, output_bytes)) return -EFAULT;
        for (uint32_t i = 0; i < c.count; ++i)
            if (j->items[i].size != sizeof(XNetWaitItem) || j->items[i].version != XNET_ABI_VERSION ||
                (j->items[i].events & ~XNET_EVENTS_ALL))
                return -EINVAL;
        break;
    case XNET_OP_CANCEL_CREATE:
    case XNET_OP_WATCH_CREATE: output_bytes = sizeof(uint64_t); break;
    case XNET_OP_RESOLVE_START:
        error = input(j, j->payload.resolve, c.input);
        if (error) break;
        if (j->payload.resolve.reserved16 || j->payload.resolve.reserved[0] || j->payload.resolve.reserved[1] ||
            !j->payload.resolve.total_timeout_ms || j->payload.resolve.total_timeout_ms > 0x7fffffffU)
            return -EINVAL;
        if (j->payload.resolve.interface_id > 1) return -ENODEV;
        if (j->payload.resolve.family > XNET_IPV6) return -EAFNOSUPPORT;
        for (unsigned i = 0; i < sizeof(j->hostname); ++i)
        {
            if (!copy_from_user_pagedir(j->image, &j->hostname[i], j->payload.resolve.hostname + i, 1)) return -EFAULT;
            if (!j->hostname[i]) break;
        }
        if (!j->hostname[0] || j->hostname[sizeof(j->hostname) - 1]) return -EINVAL;
        c.io.cancel  = j->payload.resolve.cancel;
        output_bytes = sizeof(uint64_t);
        break;
    case XNET_OP_RESOLVE_RESULT:
    case XNET_OP_ENUM:
    case XNET_OP_ADDRESSES:
        if (c.count > XNET_MAX_TRANSFER_BYTES / sizeof(XNetEndpoint)) return -EINVAL;
        error        = output_range(j, c.auxiliary, sizeof(uint32_t));
        output_bytes = c.count * (c.operation == XNET_OP_ENUM        ? sizeof(uint32_t)
                                  : c.operation == XNET_OP_ADDRESSES ? sizeof(XNetInterfaceAddress)
                                                                     : sizeof(XNetEndpoint));
        if (c.operation == XNET_OP_ADDRESSES)
        {
            j->data = malloc(output_bytes ? output_bytes : 1);
            if (!j->data) return -ENOMEM;
        }
        break;
    case XNET_OP_INTERFACE:
        error        = input(j, j->payload.interface, c.output);
        output_bytes = sizeof(XNetInterfaceInfo);
        break;
    case XNET_OP_DNS_GET:
        error        = input(j, j->payload.dns, c.output);
        output_bytes = sizeof(XNetDnsConfig);
        break;
    case XNET_OP_CONFIG_APPLY:
        if (!permissions_have(XPERM_CS)) return -EACCES;
        error = input(j, j->payload.config, c.input);
        if (!error && j->payload.config.reserved) error = -EINVAL;
        break;
    case XNET_OP_DNS_APPLY:
        if (!permissions_have(XPERM_CS)) return -EACCES;
        error = input(j, j->payload.dns, c.input);
        if (!error && j->payload.dns.reserved) error = -EINVAL;
        break;
    case XNET_OP_WATCH_READ:
        error        = input(j, j->payload.change, c.output);
        output_bytes = sizeof(XNetChange);
        break;
    default: break;
    }
    if (!error && (c.operation == XNET_OP_INTERFACE || c.operation == XNET_OP_ADDRESSES) && c.argument > 1)
        error = -ENODEV;
    if (!error) error = output_range(j, c.output, output_bytes);
    return error;
}

int pin_job(Job *j)
{
    uint32_t op       = j->call.operation;
    bool     socket   = (op >= XNET_OP_BIND && op <= XNET_OP_QUERY && op != XNET_OP_CLOSE) || op >= XNET_OP_TCP_SET;
    Kind     expected = socket                         ? Socket
                        : op == XNET_OP_CANCEL_SIGNAL  ? Cancel
                        : op == XNET_OP_RESOLVE_RESULT ? Resolver
                        : op == XNET_OP_WATCH_READ     ? Watch
                                                       : Empty;
    int      error    = 0;
    spin_lock(&registry_lock);
    if (expected != Empty || op == XNET_OP_CLOSE)
    {
        j->object = find_locked(j->process, j->image, j->call.object);
        if (!j->object)
            error = -EBADF;
        else if (expected != Empty && j->object->kind != expected)
        {
            j->object = NULL;
            error     = -EINVAL;
        }
        else
            ++j->object->refs;
    }
    if (!error && j->call.io.cancel)
    {
        Object *cancel = find_locked(j->process, j->image, j->call.io.cancel);
        if (!cancel || cancel->kind != Cancel)
            error = -EBADF;
        else
        {
            j->cancel = cancel;
            ++cancel->refs;
        }
    }
    if (!error && op == XNET_OP_WAIT)
        for (uint32_t i = 0; i < j->call.count; ++i)
        {
            j->watched[i] = find_locked(j->process, j->image, j->items[i].object);
            if (j->watched[i]) ++j->watched[i]->refs;
        }
    if (!error && socket)
    {
        uint32_t gate = op == XNET_OP_RECEIVE || op == XNET_OP_RECEIVE_FROM || op == XNET_OP_ACCEPT ? 1
                        : op == XNET_OP_SEND || op == XNET_OP_SEND_TO                               ? 2
                        : op == XNET_OP_QUERY                                                       ? 0
                                                                                                    : 4;
        if (j->object->gates & gate)
            error = -EBUSY;
        else
        {
            j->gate           = gate;
            j->object->gates |= gate;
        }
        if (!error && (op == XNET_OP_CONNECT || op == XNET_OP_BIND || op == XNET_OP_SEND_TO) &&
            (op == XNET_OP_SEND_TO ? j->endpoint.address.family : j->payload.endpoint.address.family) !=
                j->object->desc.family)
            error = -EAFNOSUPPORT;
    }
    spin_unlock(&registry_lock);
    return error;
}

int copy_result(Job *j)
{
    XNetCall &c   = j->call;
    auto      put = [j](uint64_t destination, const void *source, size_t bytes) -> int {
        return copy_to_user_pagedir(j->image, (void *)destination, source, bytes) ? 0 : -EFAULT;
    };
    bool array =
        c.operation == XNET_OP_RESOLVE_RESULT || c.operation == XNET_OP_ENUM || c.operation == XNET_OP_ADDRESSES;
    if (array && (j->result == 0 || j->result == -ENOBUFS))
        if (int error = put(c.auxiliary, &j->required, sizeof(j->required))) return error;
    if (j->result < 0) return 0;
    switch (c.operation)
    {
    case XNET_OP_OPEN:
    case XNET_OP_ACCEPT:
    case XNET_OP_CANCEL_CREATE:
    case XNET_OP_RESOLVE_START:
    case XNET_OP_WATCH_CREATE:
        if (int error = put(c.output, &j->created->id, sizeof(uint64_t))) return error;
        if (c.operation == XNET_OP_ACCEPT && c.auxiliary)
            if (int error = put(c.auxiliary, &j->endpoint, sizeof(j->endpoint))) return error;
        break;
    case XNET_OP_RECEIVE:
    case XNET_OP_RECEIVE_FROM:
        if (uint64_t(j->result) > c.count) return -EIO;
        if (int error = put(c.output, j->data, j->result)) return error;
        if (c.operation == XNET_OP_RECEIVE_FROM) return put(c.auxiliary, &j->datagram, sizeof(j->datagram));
        break;
    case XNET_OP_WAIT: return put(c.output, j->items, c.count * sizeof(XNetWaitItem));
    case XNET_OP_RESOLVE_RESULT:
        if (c.count) return put(c.output, &j->payload.endpoint, sizeof(XNetEndpoint));
        break;
    case XNET_OP_ENUM:
        if (c.count && j->required)
        {
            uint32_t id = 1;
            return put(c.output, &id, sizeof(id));
        }
        break;
    case XNET_OP_ADDRESSES:
        if (c.count) return put(c.output, j->data, j->required * sizeof(XNetInterfaceAddress));
        break;
    case XNET_OP_CAPS: return put(c.output, &j->payload.caps, sizeof(XNetCapabilities));
    case XNET_OP_QUERY: return put(c.output, &j->payload.socket, sizeof(XNetSocketInfo));
    case XNET_OP_INTERFACE: return put(c.output, &j->payload.interface, sizeof(XNetInterfaceInfo));
    case XNET_OP_DNS_GET: return put(c.output, &j->payload.dns, sizeof(XNetDnsConfig));
    case XNET_OP_WATCH_READ: return put(c.output, &j->payload.change, sizeof(XNetChange));
    case XNET_OP_TCP_GET: return put(c.output, &j->payload.tcp, sizeof(XNetTcpOptions));
    case XNET_OP_UDP_GET: return put(c.output, &j->payload.udp, sizeof(XNetUdpOptions));
    default: break;
    }
    return 0;
}

} // namespace

extern "C" int xnet_register_provider(const XNetProvider *p)
{
    if (!p || p->size != sizeof(*p) || p->version != XNET_ABI_VERSION || !p->open || !p->close || !p->bind ||
        !p->listen || !p->accept || !p->connect || !p->shutdown || !p->query || !p->send || !p->receive ||
        !p->tcp_options || !p->udp_options || !p->resolve_start || !p->resolve_poll || !p->resolve_release ||
        !p->interface_info || !p->apply_interface || !p->apply_dns)
        return -EINVAL;
    spin_lock(&registry_lock);
    int result = registered ? -EEXIST : 0;
    if (!result)
    {
        provider = *p;
        __atomic_store_n(&registered, true, __ATOMIC_RELEASE);
    }
    spin_unlock(&registry_lock);
    return result;
}

EXPORT_SYMBOL(xnet_register_provider);

int xnet_init()
{
    return ensure_worker();
}

int64_t xnet_syscall(uint64_t address)
{
    tcb_t task    = get_current_task();
    pcb_t process = task ? task->parent_group : NULL;
    if (!process || !process->pagedir) return -EFAULT;
    XNetCall call;
    if (!copy_from_user_pagedir(process->pagedir, &call, (void *)address, sizeof(call))) return -EFAULT;
    if (__atomic_load_n(&worker_state, __ATOMIC_ACQUIRE) != 2) return -ENODEV;
    auto *job = static_cast<Job *>(calloc(1, sizeof(Job)));
    if (!job) return -ENOMEM;
    job->owner   = task;
    job->process = process;
    job->image   = process->pagedir;
    job->start   = nanoTime();
    job->call    = call;
    spin_lock(&registry_lock);
    job->next = jobs;
    jobs      = job;
    spin_unlock(&registry_lock);
    int error = prepare(job);
    if (!error) error = pin_job(job);
    spin_lock(&registry_lock);
    job->phase = error ? Done : Queued;
    if (error) job->result = error;
    spin_unlock(&registry_lock);
    for (;;)
    {
        spin_lock(&registry_lock);
        bool done = job->phase == Done;
        spin_unlock(&registry_lock);
        if (done) break;
        if (signals_pending_quick(task) || process->pagedir != job->image)
        {
            int64_t result = process->pagedir == job->image ? -EINTR : XNET_CANCELLED;
            spin_lock(&registry_lock);
            job->abandoned = true;
            spin_unlock(&registry_lock);
            return result;
        }
        scheduler_sleep_ns(1000000ULL);
    }
    int64_t result = job->result;
    if (process->pagedir != job->image || __atomic_load_n(&job->revoked, __ATOMIC_ACQUIRE))
        result = XNET_CANCELLED;
    else if (int copy_error = copy_result(job))
        result = copy_error;
    spin_lock(&registry_lock);
    job->delivered = result >= 0;
    job->abandoned = true;
    spin_unlock(&registry_lock);
    return result;
}

void xnet_process_release(pcb_t process)
{
    if (!process) return;
    spin_lock(&registry_lock);
    for (Job *job = jobs; job; job = job->next)
        if (job->process == process) __atomic_store_n(&job->revoked, true, __ATOMIC_RELEASE);
    for (Object &o : objects)
        if (o.kind != Empty && o.owner == process) retire_locked(&o);
    spin_unlock(&registry_lock);
}

void xnet_thread_release(tcb_t task)
{
    // Never retire a heap ticket while its old kernel stack can still publish
    // buffers or copy results, even if another CPU has marked the TCB dead.
    if (!task || (task->status != DEATH && task->status != OUT) || scheduler_task_referenced(task)) return;
    spin_lock(&registry_lock);
    for (Job *job = jobs; job; job = job->next)
        if (job->owner == task)
        {
            job->owner     = NULL;
            job->abandoned = true;
        }
    spin_unlock(&registry_lock);
}
