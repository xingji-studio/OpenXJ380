#include "xhci.h"

static xhci_controller *g_xhci = NULL;
static const uint32_t XHCI_INVALID_INDEX = (uint32_t)-1;

static void xhci_process_events(xhci_controller *xhci);

#define XHCI_DIAG_PENDING(slot) ((void)0)
#define XHCI_DIAG_OK(slot) ((void)0)
#define XHCI_DIAG_FAIL(slot) ((void)0)
#define XHCI_DIAG_INFO(slot) ((void)0)
#define XHCI_DIAG_WARN(slot) ((void)0)
#define xhci_diag_byte(first_slot, value) ((void)0)
#define xhci_diag_portsc(portsc) ((void)0)

static void *xhci_zero_alloc(size_t bytes) {
    void *ptr = malloc(bytes);
    if (!ptr) {
        return NULL;
    }
    memset(ptr, 0, bytes);
    return ptr;
}

static inline void xhci_cpu_relax(void) {
    __asm__ volatile("pause");
}

static inline void xhci_lock_u32(volatile uint32_t *lock) {
    while (__sync_lock_test_and_set(lock, 1U) != 0U) {
        xhci_cpu_relax();
    }
}

static inline void xhci_unlock_u32(volatile uint32_t *lock) {
    __sync_lock_release(lock);
}

static inline uint32_t xhci_read32(volatile const uint32_t *reg) {
    uint32_t value = *reg;
    __sync_synchronize();
    return value;
}

static inline void xhci_write32(volatile uint32_t *reg, uint32_t value) {
    __sync_synchronize();
    *reg = value;
    __sync_synchronize();
}

static inline uint32_t xhci_portsc_neutral(uint32_t portsc) {
    return portsc & (XHCI_PORTSC_RO_BITS | XHCI_PORTSC_RWS_BITS);
}

static inline uint32_t xhci_portsc_write_value(uint32_t portsc,
                                               uint32_t set_bits) {
    return xhci_portsc_neutral(portsc) | set_bits;
}

static inline uint64_t xhci_trb_ptr(const xhci_trb *trb) {
    return ((uint64_t)trb->ptr_high << 32) | trb->ptr_low;
}

static inline uint32_t xhci_trb_type(uint32_t control) {
    return (control >> XHCI_TRB_TYPE_SHIFT) & XHCI_TRB_TYPE_MASK;
}

static inline uint32_t xhci_trb_completion_code(uint32_t status) {
    return (status >> 24) & 0xFFU;
}

static inline uint32_t xhci_trb_slot_id(uint32_t control) {
    return (control >> XHCI_TRB_SLOT_ID_SHIFT) & XHCI_TRB_SLOT_ID_MASK;
}

static inline uint32_t xhci_trb_epid(uint32_t control) {
    return (control >> XHCI_TRB_EPID_SHIFT) & XHCI_TRB_EPID_MASK;
}

static inline uint32_t xhci_trb_status_field(uint32_t transfer_len,
                                             uint16_t intr_target) {
    return (transfer_len & 0x1FFFFU) |
           (((uint32_t)intr_target & XHCI_TRB_INTR_TARGET_MASK)
            << XHCI_TRB_INTR_TARGET_SHIFT);
}

static inline uint32_t xhci_field(uint32_t value, uint32_t shift,
                                  uint32_t mask) {
    return (value >> shift) & mask;
}

static inline uint16_t xhci_get_be16(const void *ptr) {
    uint16_t value = 0;
    memcpy(&value, ptr, sizeof(value));
    return __builtin_bswap16(value);
}

static inline uint32_t xhci_get_be32(const void *ptr) {
    uint32_t value = 0;
    memcpy(&value, ptr, sizeof(value));
    return __builtin_bswap32(value);
}

static inline uint64_t xhci_get_be64(const void *ptr) {
    uint64_t value = 0;
    memcpy(&value, ptr, sizeof(value));
    return __builtin_bswap64(value);
}

static inline void xhci_put_be16(void *ptr, uint16_t value) {
    value = __builtin_bswap16(value);
    memcpy(ptr, &value, sizeof(value));
}

static inline void xhci_put_be32(void *ptr, uint32_t value) {
    value = __builtin_bswap32(value);
    memcpy(ptr, &value, sizeof(value));
}

static inline void xhci_put_be64(void *ptr, uint64_t value) {
    value = __builtin_bswap64(value);
    memcpy(ptr, &value, sizeof(value));
}

static const char *xhci_speed_name(uint8_t speed) {
    switch (speed) {
    case XHCI_SPEED_FULL: return "full";
    case XHCI_SPEED_LOW: return "low";
    case XHCI_SPEED_HIGH: return "high";
    case XHCI_SPEED_SUPER: return "super";
    default: return "unknown";
    }
}

static uint16_t xhci_default_ep0_packet(uint8_t speed) {
    switch (speed) {
    case XHCI_SPEED_LOW:
    case XHCI_SPEED_FULL: return 8;
    case XHCI_SPEED_HIGH: return 64;
    case XHCI_SPEED_SUPER: return 512;
    default: return 8;
    }
}

static uint16_t xhci_descriptor_ep0_packet(uint8_t speed,
                                           const usb_device_descriptor *desc) {
    if (speed == XHCI_SPEED_SUPER) {
        if (desc->bMaxPacketSize0 < 16) {
            return (uint16_t)(1U << desc->bMaxPacketSize0);
        }
        return 512;
    }
    if (desc->bMaxPacketSize0 == 0) {
        return 8;
    }
    return desc->bMaxPacketSize0;
}

static inline int xhci_fls_u32(unsigned int value) {
    if (value == 0) {
        return 0;
    }
    return 32 - __builtin_clz(value);
}

static uint8_t xhci_last_ctx_entry(uint8_t epid) {
    return (epid == 0) ? 1U : epid;
}

static uint8_t xhci_interrupt_interval_reg(uint8_t speed_id,
                                           uint8_t interval) {
    if (interval == 0) {
        return 0;
    }

    int period = interval;
    if (speed_id != XHCI_SPEED_HIGH) {
        return (uint8_t)(xhci_fls_u32((unsigned int)period) + 3);
    }
    return (uint8_t)(((period <= 4) ? 0 : (period - 4)) + 3);
}

static const char *xhci_hid_kind_name(uint8_t kind) {
    switch (kind) {
    case XHCI_HID_KEYBOARD: return "keyboard";
    case XHCI_HID_MOUSE: return "mouse";
    default: return "unknown";
    }
}

static void xhci_ascii_field(char *dst, size_t dst_len, const uint8_t *src,
                             size_t src_len) {
    if (!dst || dst_len == 0) {
        return;
    }

    size_t copy_len = (src_len < (dst_len - 1)) ? src_len : (dst_len - 1);
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';

    while (copy_len != 0) {
        char ch = dst[copy_len - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        dst[copy_len - 1] = '\0';
        copy_len--;
    }
}

static bool xhci_keyboard_usage_present(const uint8_t *report, uint8_t usage) {
    for (size_t i = 2; i < 8; ++i) {
        if (report[i] == usage) {
            return true;
        }
    }
    return false;
}

static uint8_t xhci_keyboard_translate_usage(uint8_t usage, bool shift,
                                             bool caps_lock) {
    if (usage >= 0x04 && usage <= 0x1DU) {
        uint8_t letter = (uint8_t)('a' + (usage - 0x04));
        if (shift ^ caps_lock) {
            letter = (uint8_t)('A' + (usage - 0x04));
        }
        return letter;
    }

    switch (usage) {
    case 0x1E: return shift ? '!' : '1';
    case 0x1F: return shift ? '@' : '2';
    case 0x20: return shift ? '#' : '3';
    case 0x21: return shift ? '$' : '4';
    case 0x22: return shift ? '%' : '5';
    case 0x23: return shift ? '^' : '6';
    case 0x24: return shift ? '&' : '7';
    case 0x25: return shift ? '*' : '8';
    case 0x26: return shift ? '(' : '9';
    case 0x27: return shift ? ')' : '0';
    case 0x28: return '\n';
    case 0x29: return KEY_ESC;
    case 0x2A: return '\b';
    case 0x2B: return KEY_TAB;
    case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    case 0x39: return KEY_CAPS;
    case 0x3A: return KEY_F1;
    case 0x3B: return KEY_F2;
    case 0x3C: return KEY_F3;
    case 0x3D: return KEY_F4;
    case 0x3E: return KEY_F5;
    case 0x3F: return KEY_F6;
    case 0x40: return KEY_F7;
    case 0x41: return KEY_F8;
    case 0x42: return KEY_F9;
    case 0x43: return KEY_F10;
    case 0x44: return KEY_F11;
    case 0x45: return KEY_F12;
    case 0x46: return KEY_SCROLL;
    case 0x49: return KEY_INSERT;
    case 0x4A: return KEY_HOME;
    case 0x4B: return KEY_PAGE_UP;
    case 0x4C: return KEY_DELETE;
    case 0x4D: return KEY_END;
    case 0x4E: return KEY_PAGE_DOWN;
    case 0x4F: return KEY_RIGHT;
    case 0x50: return KEY_LEFT;
    case 0x51: return KEY_DOWN;
    case 0x52: return KEY_UP;
    case 0x53: return KEY_NUML;
    case 0x54: return '/';
    case 0x55: return '*';
    case 0x56: return '-';
    case 0x57: return '+';
    case 0x58: return '\n';
    case 0x59: return '1';
    case 0x5A: return '2';
    case 0x5B: return '3';
    case 0x5C: return '4';
    case 0x5D: return '5';
    case 0x5E: return '6';
    case 0x5F: return '7';
    case 0x60: return '8';
    case 0x61: return '9';
    case 0x62: return '0';
    case 0x63: return '.';
    default: return 0;
    }
}

static void xhci_keyboard_update_modifier(bool pressed, uint8_t value) {
    if (value == 0) {
        return;
    }

    keyboard_usb_key_event(value, value, pressed ? 1U : 0U);
}

static void xhci_handle_keyboard_report(xhci_slot_state *slot,
                                        const uint8_t *report,
                                        uint32_t report_len) {
    xhci_hid_state *hid = &slot->hid;
    uint8_t current[8];
    memset(current, 0, sizeof(current));
    if (report_len != 0) {
        uint32_t copy_len = report_len;
        if (copy_len > sizeof(current)) {
            copy_len = sizeof(current);
        }
        memcpy(current, report, copy_len);
    }

    uint8_t modifier_changes = (uint8_t)(current[0] ^ hid->prev_report[0]);
    if (modifier_changes != 0)
    {
        const struct
        {
            uint8_t mask;
            uint8_t value;
        } modifier_map[] = {
            {0x11U, KEY_CTRL},
            {0x22U, KEY_SHIFT},
            {0x44U, KEY_ALT},
            {0x08U, 0xE3U},
            {0x80U, 0xE7U},
        };

        for (size_t i = 0; i < sizeof(modifier_map) / sizeof(modifier_map[0]); ++i)
        {
            if ((modifier_changes & modifier_map[i].mask) == 0) {
                continue;
            }
            bool pressed = (current[0] & modifier_map[i].mask) != 0;
            xhci_keyboard_update_modifier(pressed, modifier_map[i].value);
        }
    }

    bool shift = (current[0] & 0x22U) != 0;
    for (size_t i = 2; i < sizeof(hid->prev_report); ++i) {
        uint8_t usage = hid->prev_report[i];
        if (usage == 0 || xhci_keyboard_usage_present(current, usage)) {
            continue;
        }

        keyboard_usb_key_event(usage, 0, 0);
    }

    for (size_t i = 2; i < sizeof(current); ++i) {
        uint8_t usage = current[i];
        if (usage == 0 || xhci_keyboard_usage_present(hid->prev_report, usage)) {
            continue;
        }

        if (usage == 0x39U) {
            hid->caps_lock = !hid->caps_lock;
        }

        uint8_t value =
            xhci_keyboard_translate_usage(usage, shift, hid->caps_lock);
        if (value != 0) {
            keyboard_usb_key_event(usage, value, 1);
        }
    }

    memcpy(hid->prev_report, current, sizeof(hid->prev_report));
}

static void xhci_handle_mouse_report(xhci_slot_state *slot, const uint8_t *report,
                                     uint32_t report_len) {
    (void)slot;
    if (report_len < 3) {
        return;
    }

    int dx = (int)(int8_t)report[1];
    int dy = (int)(int8_t)report[2];
    int wheel = 0;
    if (report_len >= 4) {
        wheel = -(int)(int8_t)report[3];
    }

    mouse_inject_report(dx, dy, report[0] & 0x07U, wheel);
}

static void xhci_handle_hid_report(xhci_slot_state *slot, const uint8_t *report,
                                   uint32_t report_len) {
    if (!slot) {
        return;
    }

    switch (slot->hid.kind) {
    case XHCI_HID_KEYBOARD:
        xhci_handle_keyboard_report(slot, report, report_len);
        break;
    case XHCI_HID_MOUSE:
        xhci_handle_mouse_report(slot, report, report_len);
        break;
    default:
        break;
    }
}

static int xhci_wait_bit(volatile uint32_t *reg, uint32_t mask,
                         uint32_t expected, uint32_t timeout_ms) {
    for (uint32_t waited = 0; waited < timeout_ms; ++waited) {
        if ((xhci_read32(reg) & mask) == expected) {
            return 0;
        }
        delay_ms_hp(1);
    }
    return -ETIMEDOUT;
}

static int xhci_dma_alloc(size_t bytes, void **virt_out, uint64_t *phys_out) {
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) {
        pages = 1;
    }

    uint64_t phys = alloc_frames_dma32(pages);
    if (phys == 0) {
        return -ENOMEM;
    }

    void *virt = phys_to_virt(phys);
    if (!virt) {
        free_frames(phys, pages);
        return -ENOMEM;
    }

    memset(virt, 0, pages * PAGE_SIZE);
    *virt_out = virt;
    *phys_out = phys;
    return 0;
}

static void xhci_dma_free(void *virt, uint64_t phys, size_t bytes) {
    (void)virt;
    if (phys == 0) {
        return;
    }

    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages == 0) {
        pages = 1;
    }
    free_frames(phys, pages);
}

static int xhci_ring_alloc(xhci_ring *ring, bool link_trb) {
    memset(ring, 0, sizeof(*ring));
    ring->link_trb = link_trb;
    ring->count = XHCI_RING_TRBS;
    ring->cycle_state = 1;
    return xhci_dma_alloc(sizeof(xhci_trb) * ring->count, (void **)&ring->trbs,
                          &ring->phys);
}

static void xhci_ring_free(xhci_ring *ring) {
    if (!ring || !ring->trbs) {
        return;
    }
    xhci_dma_free(ring->trbs, ring->phys, sizeof(xhci_trb) * ring->count);
    memset(ring, 0, sizeof(*ring));
}

static int xhci_event_ring_alloc(xhci_event_ring *ring) {
    memset(ring, 0, sizeof(*ring));
    ring->count = XHCI_RING_TRBS;
    ring->cycle_state = 1;
    return xhci_dma_alloc(sizeof(xhci_trb) * ring->count, (void **)&ring->trbs,
                          &ring->phys);
}

static void xhci_event_ring_free(xhci_event_ring *ring) {
    if (!ring || !ring->trbs) {
        return;
    }
    xhci_dma_free(ring->trbs, ring->phys, sizeof(xhci_trb) * ring->count);
    memset(ring, 0, sizeof(*ring));
}

static inline bool xhci_ring_busy(const xhci_ring *ring) {
    return ring->completion_idx != ring->enqueue_idx;
}

static bool xhci_ring_index_before_or_at_target(const xhci_ring *ring,
                                                uint32_t index) {
    if (!ring->target_waiting || index >= ring->count ||
        ring->wait_target_idx >= ring->count) {
        return false;
    }

    uint32_t start = ring->completion_idx;
    uint32_t target = ring->wait_target_idx;
    if (start <= target) {
        return index >= start && index <= target;
    }
    return index >= start || index <= target;
}

static void xhci_ring_write_trb(xhci_ring *ring, uint32_t index, uint64_t ptr,
                                uint32_t status, uint32_t control,
                                bool inline_data, const void *data) {
    xhci_trb *dst = &ring->trbs[index];
    memset(dst, 0, sizeof(*dst));

    if (inline_data && data) {
        memcpy(&dst->ptr_low, data, sizeof(uint64_t));
    } else {
        dst->ptr_low = (uint32_t)ptr;
        dst->ptr_high = (uint32_t)(ptr >> 32);
    }

    dst->status = status;
    __sync_synchronize();
    dst->control = control | (ring->cycle_state ? XHCI_TRB_C : 0);
    __sync_synchronize();
}

static void xhci_ring_wrap(xhci_ring *ring) {
    xhci_ring_write_trb(ring, ring->count - 1, ring->phys, 0,
                        (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) |
                            XHCI_TRB_LINK_TC,
                        false, NULL);
    ring->enqueue_idx = 0;
    ring->cycle_state ^= 1U;
}

static void xhci_ring_ensure_contiguous(xhci_ring *ring, uint32_t trb_count) {
    if (!ring->link_trb || trb_count == 0) {
        return;
    }

    if (ring->enqueue_idx + trb_count > ring->count - 1) {
        xhci_ring_wrap(ring);
    }
}

static void xhci_ring_enqueue_ptr(xhci_ring *ring, uint64_t ptr,
                                  uint32_t status, uint32_t control) {
    if (ring->link_trb && ring->enqueue_idx >= ring->count - 1) {
        xhci_ring_wrap(ring);
    }

    xhci_ring_write_trb(ring, ring->enqueue_idx, ptr, status, control, false,
                        NULL);
    ring->enqueue_idx++;
}

static void xhci_ring_enqueue_inline(xhci_ring *ring, const void *data,
                                     uint32_t status, uint32_t control) {
    if (ring->link_trb && ring->enqueue_idx >= ring->count - 1) {
        xhci_ring_wrap(ring);
    }

    xhci_ring_write_trb(ring, ring->enqueue_idx, 0, status, control, true,
                        data);
    ring->enqueue_idx++;
}

static uint32_t xhci_ring_index_from_phys(const xhci_ring *ring,
                                          uint64_t phys_addr) {
    if (phys_addr < ring->phys) {
        return XHCI_INVALID_INDEX;
    }

    uint64_t delta = phys_addr - ring->phys;
    uint64_t ring_bytes = (uint64_t)ring->count * sizeof(xhci_trb);
    if (delta >= ring_bytes) {
        return XHCI_INVALID_INDEX;
    }

    return (uint32_t)(delta / sizeof(xhci_trb));
}

static void xhci_ring_complete(xhci_ring *ring, uint32_t trb_index,
                               const xhci_trb *evt) {
    ring->event = *evt;
    if (ring->target_waiting &&
        xhci_ring_index_before_or_at_target(ring, trb_index)) {
        uint32_t cc = xhci_trb_completion_code(evt->status);
        if (trb_index != ring->wait_target_idx &&
            cc == XHCI_CC_SUCCESS) {
            return;
        }
        trb_index = ring->wait_target_idx;
    }

    ring->completion_idx = trb_index + 1;
    if (ring->completion_idx >= ring->count) {
        ring->completion_idx = 0;
    }
}

static uint32_t xhci_ring_enqueue_inline_deferred(xhci_ring *ring,
                                                  const void *data,
                                                  uint32_t status,
                                                  uint32_t control) {
    if (ring->link_trb && ring->enqueue_idx >= ring->count - 1) {
        xhci_ring_wrap(ring);
    }

    uint32_t index = ring->enqueue_idx;
    xhci_trb *dst = &ring->trbs[index];
    memset(dst, 0, sizeof(*dst));
    if (data) {
        memcpy(&dst->ptr_low, data, sizeof(uint64_t));
    }
    dst->status = status;
    __sync_synchronize();
    dst->control = control | (ring->cycle_state ? 0 : XHCI_TRB_C);
    __sync_synchronize();
    ring->enqueue_idx++;
    return index;
}

static void xhci_ring_give_deferred(xhci_ring *ring, uint32_t index) {
    __sync_synchronize();
    if (ring->cycle_state) {
        ring->trbs[index].control |= XHCI_TRB_C;
    } else {
        ring->trbs[index].control &= ~XHCI_TRB_C;
    }
    __sync_synchronize();
}

static void xhci_doorbell(xhci_controller *xhci, uint32_t slot_id,
                          uint32_t value) {
    __sync_synchronize();
    xhci_write32(&xhci->db[slot_id].doorbell, value);
    __sync_synchronize();
}

static uint32_t xhci_max_scratchpad(uint32_t hcs_params2) {
    return ((hcs_params2 >> 27) & 0x1FU) | (((hcs_params2 >> 21) & 0x1FU) << 5);
}

static void xhci_clear_port_change_bits(xhci_controller *xhci, uint32_t port) {
    uint32_t portsc = xhci_read32(&xhci->ports[port].portsc);
    uint32_t change = portsc & XHCI_PORTSC_CHANGE_BITS;
    if (change != 0) {
        xhci_write32(&xhci->ports[port].portsc, xhci_portsc_write_value(portsc, change));
    }
}

static void xhci_event_ring_commit(xhci_controller *xhci) {
    uint64_t erdp = xhci->event_ring.phys +
                    ((uint64_t)xhci->event_ring.dequeue_idx * sizeof(xhci_trb));
    xhci_write32(&xhci->ir0->erdp_low,
                 (uint32_t)(erdp & ~0xFULL) | (1U << 3));
    xhci_write32(&xhci->ir0->erdp_high, (uint32_t)(erdp >> 32));
}

static void xhci_process_events(xhci_controller *xhci) {
    if (!xhci) {
        return;
    }

    xhci_lock_u32(&xhci->event_lock);
    bool processed = false;

    for (;;) {
        xhci_trb *evt = &xhci->event_ring.trbs[xhci->event_ring.dequeue_idx];
        uint32_t control = evt->control;
        __sync_synchronize();
        uint32_t cycle = (control & XHCI_TRB_C) ? 1U : 0U;
        if (cycle != xhci->event_ring.cycle_state) {
            break;
        }

        uint32_t type = xhci_trb_type(control);
        switch (type) {
        case XHCI_TRB_TYPE_TRANSFER_EVENT: {
            uint32_t slot_id = xhci_trb_slot_id(control);
            uint32_t epid = xhci_trb_epid(control);
            if (slot_id != 0 && slot_id <= xhci->num_slots &&
                xhci->slots[slot_id].used) {
                xhci_slot_state *slot = &xhci->slots[slot_id];
                if (epid == 1) {
                    uint32_t index = xhci_ring_index_from_phys(
                        &slot->ep0_ring, xhci_trb_ptr(evt));
                    if (index != XHCI_INVALID_INDEX) {
                        xhci_ring_complete(&slot->ep0_ring, index, evt);
                    }
                } else if (slot->hid.ready &&
                           epid == slot->hid.endpoint_epid) {
                    uint32_t index = xhci_ring_index_from_phys(
                        &slot->hid.ring, xhci_trb_ptr(evt));
                    if (index != XHCI_INVALID_INDEX) {
                        xhci_ring_complete(&slot->hid.ring, index, evt);
                        slot->hid.pending = false;

                        uint32_t cc = xhci_trb_completion_code(evt->status);
                        if (cc == XHCI_CC_SUCCESS ||
                            cc == XHCI_CC_SHORT_PACKET) {
                            XHCI_DIAG_OK(XHCI_DIAG_HID_REPORT);
                            uint32_t residue =
                                evt->status & 0x00FFFFFFU;
                            uint32_t actual =
                                (slot->hid.report_buffer_len >= residue)
                                    ? (uint32_t)(slot->hid.report_buffer_len -
                                                 residue)
                                    : 0;
                            xhci_handle_hid_report(
                                slot, (const uint8_t *)slot->hid.report_buffer,
                                actual);
                        } else {
                            XHCI_DIAG_FAIL(XHCI_DIAG_HID_REPORT);
                            pr_info("xhci: hid transfer failed slot=%u kind=%s cc=%u status=0x%x control=0x%x\n",
                                   slot->slot_id,
                                   xhci_hid_kind_name(slot->hid.kind), cc,
                                   evt->status, evt->control);
                        }
                    }
                } else if (slot->msc.ready &&
                           epid == slot->msc.bulk_in_epid) {
                    uint32_t index = xhci_ring_index_from_phys(
                        &slot->msc.bulk_in_ring, xhci_trb_ptr(evt));
                    if (index != XHCI_INVALID_INDEX) {
                        xhci_ring_complete(&slot->msc.bulk_in_ring, index,
                                           evt);
                    }
                } else if (slot->msc.ready &&
                           epid == slot->msc.bulk_out_epid) {
                    uint32_t index = xhci_ring_index_from_phys(
                        &slot->msc.bulk_out_ring, xhci_trb_ptr(evt));
                    if (index != XHCI_INVALID_INDEX) {
                        xhci_ring_complete(&slot->msc.bulk_out_ring, index,
                                           evt);
                    }
                }
            }
            break;
        }
        case XHCI_TRB_TYPE_COMMAND_COMPLETION_EVENT: {
            uint32_t index =
                xhci_ring_index_from_phys(&xhci->cmd_ring, xhci_trb_ptr(evt));
            if (index != XHCI_INVALID_INDEX) {
                xhci_ring_complete(&xhci->cmd_ring, index, evt);
            }
            break;
        }
        case XHCI_TRB_TYPE_PORT_STATUS_EVENT: {
            uint32_t port = ((evt->ptr_low >> 24) & 0xFFU);
            if (port > 0 && port <= xhci->num_ports) {
                xhci_clear_port_change_bits(xhci, port - 1);
                if (xhci->root_hub) {
                    usb_hub_mark_port_changed(xhci->root_hub, port - 1);
                }
            }
            break;
        }
        case XHCI_TRB_TYPE_HOST_CONTROLLER_EVENT:
            pr_info("xhci: host controller event status=0x%x control=0x%x\n",
                   evt->status, evt->control);
            break;
        default:
            pr_info("xhci: unhandled event type=%u status=0x%x control=0x%x\n",
                   type, evt->status, evt->control);
            break;
        }

        processed = true;
        xhci->event_ring.dequeue_idx++;
        if (xhci->event_ring.dequeue_idx >= xhci->event_ring.count) {
            xhci->event_ring.dequeue_idx = 0;
            xhci->event_ring.cycle_state ^= 1U;
        }
    }

    if (processed) {
        xhci_event_ring_commit(xhci);
    }
    xhci_unlock_u32(&xhci->event_lock);
}

static int xhci_wait_for_ring(xhci_controller *xhci, xhci_ring *ring,
                              uint32_t timeout_ms) {
    for (uint32_t waited = 0; waited < timeout_ms; ++waited) {
        xhci_process_events(xhci);
        if (!xhci_ring_busy(ring)) {
            return (int)xhci_trb_completion_code(ring->event.status);
        }
        delay_ms_hp(1);
    }

    ring->completion_idx = ring->enqueue_idx;
    XHCI_DIAG_FAIL(XHCI_DIAG_CMD_TIMEOUT);
    return XHCI_CC_USB_TRANSACTION_ERROR;
}

static int xhci_wait_for_ring_target(xhci_controller *xhci, xhci_ring *ring,
                                     uint32_t target_idx,
                                     uint32_t timeout_ms) {
    ring->wait_target_idx = target_idx;
    ring->target_waiting = true;
    int cc = xhci_wait_for_ring(xhci, ring, timeout_ms);
    ring->target_waiting = false;
    ring->wait_target_idx = 0;
    return cc;
}

static int xhci_cmd_submit(xhci_controller *xhci, uint64_t input_ctx_phys,
                           uint32_t control) {
    xhci_ring_enqueue_ptr(&xhci->cmd_ring, input_ctx_phys, 0, control);
    xhci_doorbell(xhci, 0, 0);
    return xhci_wait_for_ring(xhci, &xhci->cmd_ring, 1000);
}

static int xhci_cmd_enable_slot(xhci_controller *xhci) {
    int cc = xhci_cmd_submit(
        xhci, 0, XHCI_TRB_TYPE_ENABLE_SLOT_CMD << XHCI_TRB_TYPE_SHIFT);
    if (cc != XHCI_CC_SUCCESS) {
        pr_info("xhci: enable slot failed cc=%d status=0x%x control=0x%x\n",
               cc, xhci->cmd_ring.event.status, xhci->cmd_ring.event.control);
        return -EIO;
    }
    return (int)xhci_trb_slot_id(xhci->cmd_ring.event.control);
}

static int xhci_cmd_disable_slot(xhci_controller *xhci, uint32_t slot_id) {
    int cc = xhci_cmd_submit(
        xhci, 0,
        (XHCI_TRB_TYPE_DISABLE_SLOT_CMD << XHCI_TRB_TYPE_SHIFT) |
            (slot_id << XHCI_TRB_SLOT_ID_SHIFT));
    return (cc == XHCI_CC_SUCCESS) ? 0 : -EIO;
}

static int xhci_cmd_address_device(xhci_controller *xhci, uint32_t slot_id,
                                   uint64_t input_ctx_phys, bool block_set_address) {
    uint32_t control = (XHCI_TRB_TYPE_ADDRESS_DEVICE_CMD << XHCI_TRB_TYPE_SHIFT) |
                       (slot_id << XHCI_TRB_SLOT_ID_SHIFT);
    if (block_set_address) {
        control |= XHCI_TRB_BSR;
    }

    int cc = xhci_cmd_submit(
        xhci, input_ctx_phys, control);
    if (cc != XHCI_CC_SUCCESS) {
        xhci_diag_byte(XHCI_DIAG_ADDRESS_CC0, (uint8_t)cc);
        pr_info("xhci: address device failed slot=%u bsr=%u cc=%d status=0x%x control=0x%x\n",
               slot_id, block_set_address ? 1U : 0U, cc,
               xhci->cmd_ring.event.status,
               xhci->cmd_ring.event.control);
    }
    return (cc == XHCI_CC_SUCCESS) ? 0 : -EIO;
}

static int xhci_cmd_configure_endpoint(xhci_controller *xhci, uint32_t slot_id,
                                       uint64_t input_ctx_phys) {
    int cc = xhci_cmd_submit(
        xhci, input_ctx_phys,
        (XHCI_TRB_TYPE_CONFIGURE_ENDPOINT_CMD << XHCI_TRB_TYPE_SHIFT) |
            (slot_id << XHCI_TRB_SLOT_ID_SHIFT));
    if (cc != XHCI_CC_SUCCESS) {
        pr_info("xhci: configure endpoint failed slot=%u cc=%d status=0x%x control=0x%x\n",
               slot_id, cc, xhci->cmd_ring.event.status,
               xhci->cmd_ring.event.control);
    }
    return (cc == XHCI_CC_SUCCESS) ? 0 : -EIO;
}

static int xhci_cmd_evaluate_context(xhci_controller *xhci, uint32_t slot_id,
                                     uint64_t input_ctx_phys) {
    int cc = xhci_cmd_submit(
        xhci, input_ctx_phys,
        (XHCI_TRB_TYPE_EVALUATE_CONTEXT_CMD << XHCI_TRB_TYPE_SHIFT) |
            (slot_id << XHCI_TRB_SLOT_ID_SHIFT));
    if (cc != XHCI_CC_SUCCESS) {
        pr_info("xhci: evaluate context failed slot=%u cc=%d status=0x%x control=0x%x\n",
               slot_id, cc, xhci->cmd_ring.event.status,
               xhci->cmd_ring.event.control);
    }
    return (cc == XHCI_CC_SUCCESS) ? 0 : -EIO;
}

static void *xhci_alloc_input_ctx(xhci_controller *xhci, uint64_t *phys_out) {
    void *ctx = NULL;
    int rc = xhci_dma_alloc(xhci->context_size * (XHCI_CONTEXTS + 1), &ctx,
                            phys_out);
    return rc == 0 ? ctx : NULL;
}

static void xhci_free_input_ctx(xhci_controller *xhci, void *ctx,
                                uint64_t phys_addr) {
    if (!ctx) {
        return;
    }
    xhci_dma_free(ctx, phys_addr,
                  xhci->context_size * (XHCI_CONTEXTS + 1));
}

static xhci_input_control_ctx *xhci_input_ctrl_ctx(void *input_ctx) {
    return (xhci_input_control_ctx *)input_ctx;
}

static xhci_slot_ctx *xhci_input_slot_ctx(xhci_controller *xhci,
                                          void *input_ctx) {
    return (xhci_slot_ctx *)((uint8_t *)input_ctx + xhci->context_size);
}

static xhci_ep_ctx *xhci_input_ep_ctx(xhci_controller *xhci, void *input_ctx,
                                      uint32_t epid) {
    if (epid == 0 || epid >= XHCI_CONTEXTS) {
        return NULL;
    }
    return (xhci_ep_ctx *)((uint8_t *)input_ctx +
                           xhci->context_size * (1U + epid));
}

static xhci_ep_ctx *xhci_input_ep0_ctx(xhci_controller *xhci, void *input_ctx) {
    return xhci_input_ep_ctx(xhci, input_ctx, 1);
}

static void xhci_fill_slot_ctx(xhci_slot_ctx *slot_ctx,
                               const xhci_slot_state *slot,
                               uint8_t context_entries) {
    if (context_entries == 0) {
        context_entries = 1;
    }
    slot_ctx->ctx[0] = (slot->route_string & 0xFFFFFU) |
                       ((uint32_t)slot->speed_id << XHCI_SLOT_SPEED_SHIFT) |
                       ((uint32_t)context_entries
                        << XHCI_SLOT_CTX_ENTRIES_SHIFT);
    slot_ctx->ctx[1] = ((uint32_t)slot->port_id << XHCI_SLOT_RHPORT_SHIFT);
    slot_ctx->ctx[2] = slot->tt_info;
}

static void xhci_fill_ep0_ctx(xhci_slot_state *slot, xhci_ep_ctx *ep0_ctx) {
    ep0_ctx->ctx[1] = (3U << XHCI_EP_CTX_CERR_SHIFT) |
                      (XHCI_EP_TYPE_CONTROL << XHCI_EP_CTX_TYPE_SHIFT) |
                      ((uint32_t)slot->max_packet0
                       << XHCI_EP_CTX_MAX_PACKET_SHIFT);
    ep0_ctx->deq_low = (uint32_t)(slot->ep0_ring.phys | 1U);
    ep0_ctx->deq_high = (uint32_t)(slot->ep0_ring.phys >> 32);
    ep0_ctx->tx_info = 8U & XHCI_EP_TX_AVG_TRB_LENGTH_MASK;
}

static int xhci_address_device(xhci_controller *xhci, xhci_slot_state *slot,
                               bool block_set_address) {
    uint64_t input_ctx_phys = 0;
    void *input_ctx = xhci_alloc_input_ctx(xhci, &input_ctx_phys);
    if (!input_ctx) {
        return -ENOMEM;
    }

    xhci_input_control_ctx *ctrl_ctx = xhci_input_ctrl_ctx(input_ctx);
    ctrl_ctx->add_flags = XHCI_INPUT_ADD_SLOT | XHCI_INPUT_ADD_EP0;
    xhci_fill_slot_ctx(xhci_input_slot_ctx(xhci, input_ctx), slot, 1);
    xhci_fill_ep0_ctx(slot, xhci_input_ep0_ctx(xhci, input_ctx));

    int rc = xhci_cmd_address_device(xhci, slot->slot_id, input_ctx_phys,
                                     block_set_address);
    xhci_free_input_ctx(xhci, input_ctx, input_ctx_phys);
    return rc;
}

static void xhci_fill_interrupt_ep_ctx(xhci_slot_state *slot,
                                       xhci_ep_ctx *ep_ctx) {
    xhci_hid_state *hid = &slot->hid;
    memset(ep_ctx, 0, sizeof(*ep_ctx));
    ep_ctx->ctx[0] = (uint32_t)hid->interval_reg << 16;
    ep_ctx->ctx[1] = (3U << XHCI_EP_CTX_CERR_SHIFT) |
                     (XHCI_EP_TYPE_INTERRUPT_IN << XHCI_EP_CTX_TYPE_SHIFT) |
                     ((uint32_t)hid->max_burst << XHCI_EP_CTX_MAX_BURST_SHIFT) |
                     ((uint32_t)hid->max_packet
                      << XHCI_EP_CTX_MAX_PACKET_SHIFT);
    ep_ctx->deq_low = (uint32_t)(hid->ring.phys | 1U);
    ep_ctx->deq_high = (uint32_t)(hid->ring.phys >> 32);
    ep_ctx->tx_info =
        ((uint32_t)hid->report_buffer_len & XHCI_EP_TX_AVG_TRB_LENGTH_MASK) |
        ((uint32_t)hid->max_packet << XHCI_EP_TX_MAX_ESIT_PAYLOAD_SHIFT);
}

static void xhci_fill_bulk_ep_ctx(xhci_msc_state *msc, bool in_dir,
                                  xhci_ep_ctx *ep_ctx) {
    memset(ep_ctx, 0, sizeof(*ep_ctx));

    uint8_t type = in_dir ? XHCI_EP_TYPE_BULK_IN : XHCI_EP_TYPE_BULK_OUT;
    uint8_t max_burst =
        in_dir ? msc->bulk_in_max_burst : msc->bulk_out_max_burst;
    uint16_t max_packet =
        in_dir ? msc->bulk_in_max_packet : msc->bulk_out_max_packet;
    xhci_ring *ring = in_dir ? &msc->bulk_in_ring : &msc->bulk_out_ring;

    ep_ctx->ctx[1] = (3U << XHCI_EP_CTX_CERR_SHIFT) |
                     ((uint32_t)type << XHCI_EP_CTX_TYPE_SHIFT) |
                     ((uint32_t)max_burst << XHCI_EP_CTX_MAX_BURST_SHIFT) |
                     ((uint32_t)max_packet << XHCI_EP_CTX_MAX_PACKET_SHIFT);
    ep_ctx->deq_low = (uint32_t)(ring->phys | 1U);
    ep_ctx->deq_high = (uint32_t)(ring->phys >> 32);
    ep_ctx->tx_info = (uint32_t)max_packet & XHCI_EP_TX_AVG_TRB_LENGTH_MASK;
}

static int xhci_update_ep0_packet(xhci_controller *xhci, xhci_slot_state *slot,
                                  uint16_t packet_size) {
    uint64_t input_ctx_phys = 0;
    void *input_ctx = xhci_alloc_input_ctx(xhci, &input_ctx_phys);
    if (!input_ctx) {
        return -ENOMEM;
    }

    slot->max_packet0 = packet_size;
    xhci_input_ctrl_ctx(input_ctx)->add_flags = XHCI_INPUT_ADD_EP0;
    xhci_fill_ep0_ctx(slot, xhci_input_ep0_ctx(xhci, input_ctx));

    int rc = xhci_cmd_evaluate_context(xhci, slot->slot_id, input_ctx_phys);
    xhci_free_input_ctx(xhci, input_ctx, input_ctx_phys);
    return rc;
}

static uint32_t xhci_route_string_append(const xhci_slot_state *parent,
                                         uint8_t port) {
    if (!parent) {
        return 0;
    }
    if (port == 0 || port > 15 || parent->level == 0) {
        return XHCI_INVALID_INDEX;
    }

    uint32_t shift = (uint32_t)(parent->level - 1) * 4U;
    if (shift >= 20U) {
        return XHCI_INVALID_INDEX;
    }

    return parent->route_string | ((uint32_t)port << shift);
}

static int xhci_control_transfer(xhci_controller *xhci, xhci_slot_state *slot,
                                 const usb_setup_packet *setup, void *data,
                                 uint32_t data_len, bool in_dir,
                                 uint32_t *actual_len) {
    if (!slot || !slot->used) {
        return -ENODEV;
    }
    if (data_len != 0 && data == NULL) {
        return -EINVAL;
    }

    if (setup->bRequest == USB_REQ_SET_ADDRESS) {
        if (actual_len) {
            *actual_len = 0;
        }
        return 0;
    }

    uint32_t trt = 0;
    if (data_len != 0) {
        trt = in_dir ? 3U : 2U;
    }

    void *dma_buf = NULL;
    uint64_t dma_phys = 0;
    if (data_len != 0) {
        int rc = xhci_dma_alloc(data_len, &dma_buf, &dma_phys);
        if (rc != 0) {
            return rc;
        }
        if (!in_dir) {
            memcpy(dma_buf, data, data_len);
        }
    }

    uint32_t status_dir = (data_len == 0) ? 1U : (in_dir ? 0U : 1U);
    uint32_t trb_count = (data_len != 0) ? 3U : 2U;
    xhci_ring_ensure_contiguous(&slot->ep0_ring, trb_count);

    uint32_t setup_index = xhci_ring_enqueue_inline_deferred(
        &slot->ep0_ring, setup, xhci_trb_status_field(sizeof(*setup), 0),
        (XHCI_TRB_TYPE_SETUP << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IDT |
            (trt << 16));

    if (data_len != 0) {
        uint32_t data_control = XHCI_TRB_TYPE_DATA << XHCI_TRB_TYPE_SHIFT;
        if (in_dir) {
            data_control |= XHCI_TRB_DIR | XHCI_TRB_ISP;
        }
        xhci_ring_enqueue_ptr(
            &slot->ep0_ring, dma_phys, xhci_trb_status_field(data_len, 0),
            data_control);
    }

    uint32_t status_index = slot->ep0_ring.enqueue_idx;
    xhci_ring_enqueue_ptr(
        &slot->ep0_ring, 0, xhci_trb_status_field(0, 0),
        (XHCI_TRB_TYPE_STATUS << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC |
            (status_dir ? XHCI_TRB_DIR : 0));

    xhci_ring_give_deferred(&slot->ep0_ring, setup_index);
    xhci_doorbell(xhci, slot->slot_id, 1);

    int cc = xhci_wait_for_ring_target(xhci, &slot->ep0_ring, status_index,
                                       5000);
    if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET) {
        pr_info("xhci: control transfer failed slot=%u req=0x%x type=0x%x len=%u cc=%d status=0x%x control=0x%x\n",
               slot->slot_id, setup->bRequest, setup->bmRequestType, data_len,
               cc, slot->ep0_ring.event.status, slot->ep0_ring.event.control);
        if (dma_buf) {
            xhci_dma_free(dma_buf, dma_phys, data_len);
        }
        return -EIO;
    }

    uint32_t residue = slot->ep0_ring.event.status & 0x00FFFFFFU;
    uint32_t actual = (data_len >= residue) ? (data_len - residue) : 0;
    if (actual_len) {
        *actual_len = actual;
    }

    if (in_dir && data_len != 0) {
        uint32_t copy_len = actual;
        if (copy_len > data_len) {
            copy_len = data_len;
        }
        if (copy_len != 0) {
            memcpy(data, dma_buf, copy_len);
        }
    }

    if (dma_buf) {
        xhci_dma_free(dma_buf, dma_phys, data_len);
    }
    return 0;
}

int xhci_control_request(xhci_controller *xhci, xhci_slot_state *slot,
                         uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index, void *data,
                         uint16_t length, uint32_t *actual_len) {
    usb_setup_packet setup = {
        .bmRequestType = request_type,
        .bRequest = request,
        .wValue = value,
        .wIndex = index,
        .wLength = length,
    };
    bool in_dir = (request_type & USB_DIR_IN) != 0;
    return xhci_control_transfer(xhci, slot, &setup, data, length, in_dir,
                                 actual_len);
}

int xhci_set_configuration(xhci_controller *xhci, xhci_slot_state *slot,
                           uint8_t config_value) {
    return xhci_control_request(
        xhci, slot, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_SET_CONFIGURATION, config_value, 0, NULL, 0, NULL);
}

static int xhci_hid_set_protocol(xhci_controller *xhci, xhci_slot_state *slot,
                                 uint8_t protocol) {
    return xhci_control_request(
        xhci, slot, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        HID_REQ_SET_PROTOCOL, protocol, slot->hid.interface_number, NULL, 0,
        NULL);
}

static int xhci_hid_set_idle(xhci_controller *xhci, xhci_slot_state *slot,
                             uint8_t duration, uint8_t report_id) {
    uint16_t value = (uint16_t)(((uint16_t)duration << 8) | report_id);
    return xhci_control_request(
        xhci, slot, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        HID_REQ_SET_IDLE, value, slot->hid.interface_number, NULL, 0, NULL);
}

static int xhci_read_device_descriptor(xhci_controller *xhci,
                                       xhci_slot_state *slot,
                                       usb_device_descriptor *desc) {
    memset(desc, 0, sizeof(*desc));
    return xhci_control_request(
        xhci, slot, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DT_DEVICE << 8), 0, desc,
        sizeof(*desc), NULL);
}

static int xhci_read_config_descriptor(xhci_controller *xhci,
                                       xhci_slot_state *slot, void **buf_out,
                                       uint16_t *length_out) {
    usb_config_descriptor header;
    memset(&header, 0, sizeof(header));

    uint32_t actual = 0;
    int rc = xhci_control_request(
        xhci, slot, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DT_CONFIG << 8), 0, &header,
        sizeof(header), &actual);
    if (rc != 0 || actual < sizeof(header) ||
        header.wTotalLength < sizeof(header)) {
        return -EIO;
    }

    void *config = malloc(header.wTotalLength);
    if (!config) {
        return -ENOMEM;
    }
    memset(config, 0, header.wTotalLength);

    rc = xhci_control_request(
        xhci, slot, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
        USB_REQ_GET_DESCRIPTOR, (uint16_t)(USB_DT_CONFIG << 8), 0, config,
        header.wTotalLength, &actual);
    if (rc != 0 || actual < sizeof(header)) {
        free(config);
        return -EIO;
    }

    *buf_out = config;
    *length_out = header.wTotalLength;
    return 0;
}

static void xhci_parse_config(xhci_slot_state *slot) {
    if (!slot || !slot->config_buffer ||
        slot->config_length < sizeof(usb_config_descriptor)) {
        return;
    }

    memcpy(&slot->config_desc, slot->config_buffer, sizeof(slot->config_desc));
    slot->config_value = slot->config_desc.bConfigurationValue;
    memset(&slot->hid, 0, sizeof(slot->hid));
    memset(&slot->msc, 0, sizeof(slot->msc));
    slot->msc.device_id = -1;

    uint8_t *ptr = (uint8_t *)slot->config_buffer;
    uint8_t *end = ptr + slot->config_length;
    bool hid_candidate = false;
    uint8_t hid_kind = XHCI_HID_NONE;
    uint8_t hid_interface = 0;
    bool msc_candidate = false;
    uint8_t msc_interface = 0;
    uint8_t msc_last_bulk = 0;
    while (ptr + sizeof(usb_descriptor_header) <= end) {
        usb_descriptor_header *hdr = (usb_descriptor_header *)ptr;
        if (hdr->bLength == 0 || ptr + hdr->bLength > end) {
            break;
        }

        if (hdr->bDescriptorType == USB_DT_INTERFACE &&
            hdr->bLength >= sizeof(usb_interface_descriptor)) {
            usb_interface_descriptor *iface = (usb_interface_descriptor *)ptr;
            if (iface->bAlternateSetting == 0 &&
                slot->active_iface_class == 0) {
                slot->active_iface_class = iface->bInterfaceClass;
                slot->active_iface_subclass = iface->bInterfaceSubClass;
                slot->active_iface_protocol = iface->bInterfaceProtocol;
            }

            hid_candidate = false;
            hid_kind = XHCI_HID_NONE;
            hid_interface = iface->bInterfaceNumber;
            msc_candidate = false;
            msc_interface = iface->bInterfaceNumber;
            msc_last_bulk = 0;
            if (!slot->hid.supported && iface->bAlternateSetting == 0 &&
                iface->bInterfaceClass == USB_CLASS_HID &&
                iface->bInterfaceSubClass == USB_INTERFACE_SUBCLASS_BOOT) {
                if (iface->bInterfaceProtocol ==
                    USB_INTERFACE_PROTOCOL_KEYBOARD) {
                    hid_kind = XHCI_HID_KEYBOARD;
                    hid_candidate = true;
                } else if (iface->bInterfaceProtocol ==
                           USB_INTERFACE_PROTOCOL_MOUSE) {
                    hid_kind = XHCI_HID_MOUSE;
                    hid_candidate = true;
                }
            }
            if (!slot->msc.supported && iface->bAlternateSetting == 0 &&
                iface->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
                iface->bInterfaceSubClass == USB_MSC_SUBCLASS_SCSI &&
                iface->bInterfaceProtocol == USB_MSC_PROTOCOL_BBB) {
                msc_candidate = true;
                slot->msc.interface_number = iface->bInterfaceNumber;
            }
        } else if (hid_candidate && hdr->bDescriptorType == USB_DT_ENDPOINT &&
                   hdr->bLength >= sizeof(usb_endpoint_descriptor)) {
            usb_endpoint_descriptor *ep = (usb_endpoint_descriptor *)ptr;
            uint16_t max_packet = ep->wMaxPacketSize & 0x07FFU;
            if ((ep->bEndpointAddress & USB_DIR_IN) != 0 &&
                (ep->bmAttributes & 0x03U) == USB_ENDPOINT_XFER_INT &&
                max_packet != 0) {
                slot->hid.supported = true;
                slot->hid.kind = hid_kind;
                slot->hid.interface_number = hid_interface;
                slot->hid.endpoint_address = ep->bEndpointAddress;
                slot->hid.endpoint_interval = ep->bInterval;
                slot->hid.endpoint_epid =
                    xhci_endpoint_id_from_address(ep->bEndpointAddress);
                slot->hid.max_packet = max_packet;
                slot->hid.report_buffer_len = max_packet;
                if (slot->speed_id == XHCI_SPEED_HIGH) {
                    slot->hid.max_burst =
                        (uint8_t)((ep->wMaxPacketSize >> 11) & 0x03U);
                }
                slot->hid.interval_reg =
                    xhci_interrupt_interval_reg(slot->speed_id, ep->bInterval);
                hid_candidate = false;
            }
        } else if (msc_candidate && hdr->bDescriptorType == USB_DT_ENDPOINT &&
                   hdr->bLength >= sizeof(usb_endpoint_descriptor)) {
            usb_endpoint_descriptor *ep = (usb_endpoint_descriptor *)ptr;
            uint16_t max_packet = ep->wMaxPacketSize & 0x07FFU;
            if ((ep->bmAttributes & 0x03U) == USB_ENDPOINT_XFER_BULK &&
                max_packet != 0) {
                if ((ep->bEndpointAddress & USB_DIR_IN) != 0 &&
                    slot->msc.bulk_in_epid == 0) {
                    slot->msc.bulk_in_address = ep->bEndpointAddress;
                    slot->msc.bulk_in_epid =
                        xhci_endpoint_id_from_address(ep->bEndpointAddress);
                    slot->msc.bulk_in_max_packet = max_packet;
                    if (slot->speed_id == XHCI_SPEED_HIGH) {
                        slot->msc.bulk_in_max_burst =
                            (uint8_t)((ep->wMaxPacketSize >> 11) & 0x03U);
                    }
                    msc_last_bulk = USB_DIR_IN;
                } else if ((ep->bEndpointAddress & USB_DIR_IN) == 0 &&
                           slot->msc.bulk_out_epid == 0) {
                    slot->msc.bulk_out_address = ep->bEndpointAddress;
                    slot->msc.bulk_out_epid =
                        xhci_endpoint_id_from_address(ep->bEndpointAddress);
                    slot->msc.bulk_out_max_packet = max_packet;
                    if (slot->speed_id == XHCI_SPEED_HIGH) {
                        slot->msc.bulk_out_max_burst =
                            (uint8_t)((ep->wMaxPacketSize >> 11) & 0x03U);
                    }
                    msc_last_bulk = USB_DIR_OUT;
                } else {
                    msc_last_bulk = 0;
                }

                if (slot->msc.bulk_in_epid != 0 &&
                    slot->msc.bulk_out_epid != 0) {
                    slot->msc.supported = true;
                    slot->msc.interface_number = msc_interface;
                }
            }
        } else if (msc_candidate &&
                   hdr->bDescriptorType == USB_DT_SS_ENDPOINT_COMPANION &&
                   hdr->bLength >= sizeof(usb_ss_ep_comp_descriptor)) {
            usb_ss_ep_comp_descriptor *comp =
                (usb_ss_ep_comp_descriptor *)ptr;
            if (msc_last_bulk == USB_DIR_IN) {
                slot->msc.bulk_in_max_burst = comp->bMaxBurst;
            } else if (msc_last_bulk == USB_DIR_OUT) {
                slot->msc.bulk_out_max_burst = comp->bMaxBurst;
            }
            msc_last_bulk = 0;
        } else {
            msc_last_bulk = 0;
        }

        ptr += hdr->bLength;
    }

    if (slot->hid.supported &&
        (slot->hid.endpoint_epid == 0 || slot->hid.report_buffer_len == 0)) {
        memset(&slot->hid, 0, sizeof(slot->hid));
    }
    if (slot->msc.supported &&
        (slot->msc.bulk_in_epid == 0 || slot->msc.bulk_out_epid == 0 ||
         slot->msc.bulk_in_max_packet == 0 ||
         slot->msc.bulk_out_max_packet == 0)) {
        memset(&slot->msc, 0, sizeof(slot->msc));
        slot->msc.device_id = -1;
    }

    slot->is_hub = (slot->device_desc.bDeviceClass == USB_CLASS_HUB) ||
                   (slot->active_iface_class == USB_CLASS_HUB);
}

static void xhci_release_hid(xhci_slot_state *slot) {
    if (!slot) {
        return;
    }

    if (slot->hid.report_buffer) {
        xhci_dma_free(slot->hid.report_buffer, slot->hid.report_buffer_phys,
                      slot->hid.report_buffer_len);
    }
    xhci_ring_free(&slot->hid.ring);
    memset(&slot->hid, 0, sizeof(slot->hid));
}

void xhci_release_msc(xhci_slot_state *slot) {
    if (!slot) {
        return;
    }

    xhci_lock_u32(&slot->msc.io_lock);
    xhci_ring_free(&slot->msc.bulk_in_ring);
    xhci_ring_free(&slot->msc.bulk_out_ring);
    memset(&slot->msc, 0, sizeof(slot->msc));
    slot->msc.device_id = -1;
    xhci_unlock_u32(&slot->msc.io_lock);
}

static int xhci_configure_hid_endpoint(xhci_controller *xhci,
                                       xhci_slot_state *slot) {
    if (!slot->configured || !slot->hid.supported || slot->hid.ready) {
        return 0;
    }

    xhci_hid_state *hid = &slot->hid;
    if (hid->endpoint_epid <= 1 || hid->endpoint_epid >= XHCI_CONTEXTS ||
        hid->report_buffer_len == 0) {
        return -EINVAL;
    }

    int rc = xhci_ring_alloc(&hid->ring, true);
    if (rc != 0) {
        return rc;
    }

    rc = xhci_dma_alloc(hid->report_buffer_len, &hid->report_buffer,
                        &hid->report_buffer_phys);
    if (rc != 0) {
        xhci_ring_free(&hid->ring);
        return rc;
    }

    uint64_t input_ctx_phys = 0;
    void *input_ctx = xhci_alloc_input_ctx(xhci, &input_ctx_phys);
    if (!input_ctx) {
        xhci_release_hid(slot);
        return -ENOMEM;
    }

    xhci_input_control_ctx *ctrl_ctx = xhci_input_ctrl_ctx(input_ctx);
    ctrl_ctx->add_flags =
        XHCI_INPUT_ADD_SLOT | (1U << hid->endpoint_epid);

    xhci_fill_slot_ctx(xhci_input_slot_ctx(xhci, input_ctx), slot,
                       xhci_last_ctx_entry(hid->endpoint_epid));

    xhci_ep_ctx *ep_ctx =
        xhci_input_ep_ctx(xhci, input_ctx, hid->endpoint_epid);
    if (!ep_ctx) {
        xhci_free_input_ctx(xhci, input_ctx, input_ctx_phys);
        xhci_release_hid(slot);
        return -EINVAL;
    }
    xhci_fill_interrupt_ep_ctx(slot, ep_ctx);

    rc = xhci_cmd_configure_endpoint(xhci, slot->slot_id, input_ctx_phys);
    xhci_free_input_ctx(xhci, input_ctx, input_ctx_phys);
    if (rc != 0) {
        xhci_release_hid(slot);
        return rc;
    }

    hid->ready = true;
    hid->pending = false;
    memset(hid->prev_report, 0, sizeof(hid->prev_report));
    return 0;
}

int xhci_configure_msc_endpoints(xhci_controller *xhci,
                                 xhci_slot_state *slot) {
    if (!slot->configured || !slot->msc.supported || slot->msc.ready) {
        return 0;
    }

    xhci_msc_state *msc = &slot->msc;
    if (msc->bulk_in_epid <= 1 || msc->bulk_in_epid >= XHCI_CONTEXTS ||
        msc->bulk_out_epid <= 1 || msc->bulk_out_epid >= XHCI_CONTEXTS ||
        msc->bulk_in_max_packet == 0 || msc->bulk_out_max_packet == 0) {
        return -EINVAL;
    }

    int rc = xhci_ring_alloc(&msc->bulk_in_ring, true);
    if (rc != 0) {
        return rc;
    }

    rc = xhci_ring_alloc(&msc->bulk_out_ring, true);
    if (rc != 0) {
        xhci_ring_free(&msc->bulk_in_ring);
        return rc;
    }

    uint64_t input_ctx_phys = 0;
    void *input_ctx = xhci_alloc_input_ctx(xhci, &input_ctx_phys);
    if (!input_ctx) {
        xhci_release_msc(slot);
        return -ENOMEM;
    }

    xhci_input_control_ctx *ctrl_ctx = xhci_input_ctrl_ctx(input_ctx);
    ctrl_ctx->add_flags = XHCI_INPUT_ADD_SLOT | (1U << msc->bulk_in_epid) |
                          (1U << msc->bulk_out_epid);

    uint8_t last_ctx = xhci_last_ctx_entry(
        (msc->bulk_in_epid > msc->bulk_out_epid) ? msc->bulk_in_epid
                                                 : msc->bulk_out_epid);
    xhci_fill_slot_ctx(xhci_input_slot_ctx(xhci, input_ctx), slot, last_ctx);

    xhci_ep_ctx *in_ctx = xhci_input_ep_ctx(xhci, input_ctx, msc->bulk_in_epid);
    xhci_ep_ctx *out_ctx =
        xhci_input_ep_ctx(xhci, input_ctx, msc->bulk_out_epid);
    if (!in_ctx || !out_ctx) {
        xhci_free_input_ctx(xhci, input_ctx, input_ctx_phys);
        xhci_release_msc(slot);
        return -EINVAL;
    }

    xhci_fill_bulk_ep_ctx(msc, true, in_ctx);
    xhci_fill_bulk_ep_ctx(msc, false, out_ctx);

    rc = xhci_cmd_configure_endpoint(xhci, slot->slot_id, input_ctx_phys);
    xhci_free_input_ctx(xhci, input_ctx, input_ctx_phys);
    if (rc != 0) {
        xhci_release_msc(slot);
        return rc;
    }

    msc->ready = true;
    msc->slot = slot;
    msc->controller = xhci;
    msc->tag = 1;
    msc->io_lock = 0;
    return 0;
}

static int xhci_submit_hid_transfer(xhci_controller *xhci,
                                    xhci_slot_state *slot) {
    xhci_hid_state *hid = &slot->hid;
    if (!hid->ready || hid->pending || !hid->report_buffer) {
        return 0;
    }

    memset(hid->report_buffer, 0, hid->report_buffer_len);
    xhci_ring_enqueue_ptr(
        &hid->ring, hid->report_buffer_phys,
        xhci_trb_status_field(hid->report_buffer_len, 0),
        (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC);
    hid->pending = true;
    xhci_doorbell(xhci, slot->slot_id, hid->endpoint_epid);
    return 0;
}

static int xhci_submit_bulk_transfer(xhci_controller *xhci,
                                     xhci_slot_state *slot, bool in_dir,
                                     uint64_t phys, uint32_t len,
                                     uint32_t *actual_len) {
    xhci_msc_state *msc = &slot->msc;
    xhci_ring *ring = in_dir ? &msc->bulk_in_ring : &msc->bulk_out_ring;
    uint8_t epid = in_dir ? msc->bulk_in_epid : msc->bulk_out_epid;

    if (!msc->ready || epid <= 1 || epid >= XHCI_CONTEXTS) {
        return -ENODEV;
    }
    if (len > 0x1FFFFU) {
        return -E2BIG;
    }

    xhci_ring_enqueue_ptr(
        ring, phys, xhci_trb_status_field(len, 0),
        (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC);
    xhci_doorbell(xhci, slot->slot_id, epid);

    int cc = xhci_wait_for_ring(xhci, ring, 1000);
    if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET) {
        return -EIO;
    }

    uint32_t residue = ring->event.status & 0x00FFFFFFU;
    if (actual_len) {
        *actual_len = (len >= residue) ? (len - residue) : 0;
    }
    return 0;
}

static int xhci_msc_reset_recovery(xhci_controller *xhci, xhci_slot_state *slot) {
    int rc = xhci_control_request(
        xhci, slot, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        USB_MSC_REQ_RESET, 0, slot->msc.interface_number, NULL, 0, NULL);
    if (rc != 0) {
        return rc;
    }

    (void)xhci_control_request(
        xhci, slot, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
        USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT,
        slot->msc.bulk_in_address, NULL, 0, NULL);
    (void)xhci_control_request(
        xhci, slot, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
        USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT,
        slot->msc.bulk_out_address, NULL, 0, NULL);
    return 0;
}

static uint64_t xhci_buffer_phys_if_contiguous(const void *data,
                                               uint32_t data_len) {
    if (!data || data_len == 0) {
        return 0;
    }

    uint64_t base_virt = (uint64_t)data;
    uint64_t base_phys = page_virt_to_phys(base_virt);
    if (base_phys == 0) {
        return 0;
    }

    uint32_t total = 0;
    while (total < data_len) {
        uint64_t cur_virt = base_virt + total;
        uint64_t cur_phys = page_virt_to_phys(cur_virt);
        if (cur_phys == 0 || cur_phys != base_phys + total) {
            return 0;
        }

        uint32_t page_left = (uint32_t)(PAGE_SIZE - (cur_virt & (PAGE_SIZE - 1)));
        uint32_t chunk = MIN(page_left, data_len - total);
        total += chunk;
    }

    return base_phys;
}

static int xhci_msc_prepare_data_buffer(void *data, uint32_t data_len,
                                        bool in_dir, bool direct_data,
                                        void **dma_buf_out,
                                        uint64_t *dma_phys_out,
                                        bool *allocated_out) {
    *dma_buf_out = NULL;
    *dma_phys_out = 0;
    *allocated_out = false;

    if (data_len == 0) {
        return 0;
    }
    if (!data) {
        return -EINVAL;
    }

    if (direct_data) {
        uint64_t phys = xhci_buffer_phys_if_contiguous(data, data_len);
        if (phys != 0) {
            *dma_buf_out = data;
            *dma_phys_out = phys;
            return 0;
        }
    }

    int rc = xhci_dma_alloc(data_len, dma_buf_out, dma_phys_out);
    if (rc != 0) {
        return rc;
    }
    if (!in_dir) {
        memcpy(*dma_buf_out, data, data_len);
    }
    *allocated_out = true;
    return 0;
}

static void xhci_msc_finish_data_buffer(void *data, uint32_t data_len,
                                        bool in_dir, void *dma_buf,
                                        uint64_t dma_phys, bool allocated,
                                        uint32_t actual_len) {
    if (allocated) {
        if (in_dir && data && actual_len != 0) {
            uint32_t copy_len = (actual_len < data_len) ? actual_len : data_len;
            memcpy(data, dma_buf, copy_len);
        }
        xhci_dma_free(dma_buf, dma_phys, data_len);
    }
}

static int xhci_msc_bot_command(xhci_controller *xhci, xhci_slot_state *slot,
                                const void *cdb, uint8_t cdb_len, void *data,
                                uint32_t data_len, bool in_dir,
                                bool direct_data, uint32_t *actual_len) {
    if (!xhci || !slot || !slot->msc.ready || !cdb || cdb_len == 0 ||
        cdb_len > sizeof(((xhci_msc_cbw *)0)->cdb)) {
        return -EINVAL;
    }

    xhci_msc_cbw *cbw = NULL;
    xhci_msc_csw *csw = NULL;
    uint64_t cbw_phys = 0;
    uint64_t csw_phys = 0;
    void *dma_buf = NULL;
    uint64_t dma_phys = 0;
    bool allocated = false;
    uint32_t data_actual = 0;
    uint32_t csw_actual = 0;

    int rc = xhci_dma_alloc(sizeof(*cbw), (void **)&cbw, &cbw_phys);
    if (rc != 0) {
        return rc;
    }

    rc = xhci_dma_alloc(sizeof(*csw), (void **)&csw, &csw_phys);
    if (rc != 0) {
        xhci_dma_free(cbw, cbw_phys, sizeof(*cbw));
        return rc;
    }

    rc = xhci_msc_prepare_data_buffer(data, data_len, in_dir, direct_data,
                                      &dma_buf, &dma_phys, &allocated);
    if (rc != 0) {
        xhci_dma_free(csw, csw_phys, sizeof(*csw));
        xhci_dma_free(cbw, cbw_phys, sizeof(*cbw));
        return rc;
    }

    memset(cbw, 0, sizeof(*cbw));
    memset(csw, 0, sizeof(*csw));

    uint32_t tag = slot->msc.tag++;
    if (tag == 0) {
        tag = slot->msc.tag++;
    }

    cbw->signature = XHCI_MSC_CBW_SIGNATURE;
    cbw->tag = tag;
    cbw->data_transfer_length = data_len;
    cbw->flags = in_dir ? USB_DIR_IN : USB_DIR_OUT;
    cbw->lun = 0;
    cbw->cb_length = cdb_len;
    memcpy(cbw->cdb, cdb, cdb_len);

    rc = xhci_submit_bulk_transfer(xhci, slot, false, cbw_phys, sizeof(*cbw),
                                   NULL);
    if (rc == 0 && data_len != 0) {
        rc = xhci_submit_bulk_transfer(xhci, slot, in_dir, dma_phys, data_len,
                                       &data_actual);
    }
    if (rc == 0) {
        rc = xhci_submit_bulk_transfer(xhci, slot, true, csw_phys, sizeof(*csw),
                                       &csw_actual);
    }

    if (rc != 0) {
        (void)xhci_msc_reset_recovery(xhci, slot);
        xhci_msc_finish_data_buffer(data, data_len, in_dir, dma_buf, dma_phys,
                                    allocated, data_actual);
        xhci_dma_free(csw, csw_phys, sizeof(*csw));
        xhci_dma_free(cbw, cbw_phys, sizeof(*cbw));
        return rc;
    }

    if (csw_actual < sizeof(*csw) || csw->signature != XHCI_MSC_CSW_SIGNATURE ||
        csw->tag != tag || csw->status != 0) {
        (void)xhci_msc_reset_recovery(xhci, slot);
        xhci_msc_finish_data_buffer(data, data_len, in_dir, dma_buf, dma_phys,
                                    allocated, data_actual);
        xhci_dma_free(csw, csw_phys, sizeof(*csw));
        xhci_dma_free(cbw, cbw_phys, sizeof(*cbw));
        return -EIO;
    }

    xhci_msc_finish_data_buffer(data, data_len, in_dir, dma_buf, dma_phys,
                                allocated, data_actual);
    xhci_dma_free(csw, csw_phys, sizeof(*csw));
    xhci_dma_free(cbw, cbw_phys, sizeof(*cbw));
    if (actual_len) {
        *actual_len = data_actual;
    }
    return 0;
}

int xhci_msc_scsi_test_unit_ready(xhci_controller *xhci,
                                  xhci_slot_state *slot) {
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_TEST_UNIT_READY;
    return xhci_msc_bot_command(xhci, slot, cdb, sizeof(cdb), NULL, 0, true,
                                false, NULL);
}

int xhci_msc_scsi_request_sense(xhci_controller *xhci,
                                xhci_slot_state *slot, void *data,
                                uint8_t alloc_len) {
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = alloc_len;
    return xhci_msc_bot_command(xhci, slot, cdb, sizeof(cdb), data, alloc_len,
                                true, false, NULL);
}

int xhci_msc_scsi_inquiry(xhci_controller *xhci, xhci_slot_state *slot,
                          void *data, uint8_t alloc_len) {
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_INQUIRY;
    cdb[4] = alloc_len;
    return xhci_msc_bot_command(xhci, slot, cdb, sizeof(cdb), data, alloc_len,
                                true, false, NULL);
}

int xhci_msc_scsi_read_capacity10(xhci_controller *xhci,
                                  xhci_slot_state *slot, void *data) {
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_READ_CAPACITY_10;
    return xhci_msc_bot_command(xhci, slot, cdb, sizeof(cdb), data, 8, true,
                                false, NULL);
}

int xhci_msc_scsi_read_capacity16(xhci_controller *xhci,
                                  xhci_slot_state *slot, void *data) {
    uint8_t cdb[16];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_SERVICE_ACTION_IN_16;
    cdb[1] = 0x10;
    xhci_put_be32(&cdb[10], 32);
    return xhci_msc_bot_command(xhci, slot, cdb, sizeof(cdb), data, 32, true,
                                false, NULL);
}

int xhci_msc_scsi_rw(xhci_controller *xhci, xhci_slot_state *slot,
                     void *buffer, uint64_t lba, uint32_t blocks, bool write) {
    if (blocks == 0) {
        return 0;
    }

    bool use16 = slot->msc.use_read16 || lba > 0xFFFFFFFFULL ||
                 blocks > 0xFFFFU;
    if (use16) {
        uint8_t cdb[16];
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = write ? SCSI_WRITE_16 : SCSI_READ_16;
        xhci_put_be64(&cdb[2], lba);
        xhci_put_be32(&cdb[10], blocks);
        return xhci_msc_bot_command(
            xhci, slot, cdb, sizeof(cdb), buffer,
            blocks * slot->msc.block_size, !write, true, NULL);
    }

    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
    xhci_put_be32(&cdb[2], (uint32_t)lba);
    xhci_put_be16(&cdb[7], (uint16_t)blocks);
    return xhci_msc_bot_command(xhci, slot, cdb, sizeof(cdb), buffer,
                                blocks * slot->msc.block_size, !write, true,
                                NULL);
}

static bool xhci_has_active_hid(const xhci_controller *xhci) {
    if (!xhci || !xhci->slots) {
        return false;
    }

    for (uint32_t i = 1; i <= xhci->num_slots; ++i) {
        if (xhci->slots[i].used && xhci->slots[i].hid.ready) {
            return true;
        }
    }
    return false;
}

static bool xhci_is_registered_controller(const xhci_controller *xhci) {
    for (xhci_controller *cur = g_xhci; cur; cur = cur->next) {
        if (cur == xhci) {
            return true;
        }
    }
    return false;
}

static void xhci_hid_worker(void *arg) {
    xhci_controller *xhci = (xhci_controller *)arg;
    for (;;) {
        if (!xhci || !xhci_is_registered_controller(xhci)) {
            delay_ms_hp(10);
            continue;
        }

        xhci_process_events(xhci);

        for (uint32_t i = 1; i <= xhci->num_slots; ++i) {
            xhci_slot_state *slot = &xhci->slots[i];
            if (!slot->used || !slot->hid.ready) {
                continue;
            }
            (void)xhci_submit_hid_transfer(xhci, slot);
        }

        delay_ms_hp(1);
    }
}

int xhci_hub_get_descriptor(xhci_controller *xhci, xhci_slot_state *slot,
                            usb_hub_descriptor_prefix *desc) {
    uint16_t desc_type =
        (slot->speed_id == XHCI_SPEED_SUPER) ? USB_DT_HUB3 : USB_DT_HUB;
    memset(desc, 0, sizeof(*desc));
    return xhci_control_request(
        xhci, slot, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE,
        USB_REQ_GET_DESCRIPTOR, (uint16_t)(desc_type << 8), 0, desc,
        sizeof(*desc), NULL);
}

int xhci_hub_set_depth(xhci_controller *xhci, xhci_slot_state *slot) {
    if (slot->speed_id != XHCI_SPEED_SUPER) {
        return 0;
    }

    uint16_t depth = (slot->level > 0) ? (uint16_t)(slot->level - 1) : 0;
    return xhci_control_request(
        xhci, slot, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_DEVICE,
        HUB_REQ_SET_HUB_DEPTH, depth, 0, NULL, 0, NULL);
}

int xhci_hub_set_port_feature(xhci_controller *xhci, xhci_slot_state *slot,
                              uint8_t port, uint16_t feature) {
    return xhci_control_request(
        xhci, slot, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
        USB_REQ_SET_FEATURE, feature, port, NULL, 0, NULL);
}

int xhci_hub_clear_port_feature(xhci_controller *xhci, xhci_slot_state *slot,
                                uint8_t port, uint16_t feature) {
    return xhci_control_request(
        xhci, slot, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER,
        USB_REQ_CLEAR_FEATURE, feature, port, NULL, 0, NULL);
}

int xhci_hub_get_port_status(xhci_controller *xhci, xhci_slot_state *slot,
                             uint8_t port, usb_port_status *status) {
    memset(status, 0, sizeof(*status));
    return xhci_control_request(
        xhci, slot, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER,
        USB_REQ_GET_STATUS, 0, port, status, sizeof(*status), NULL);
}

void xhci_hub_ack_port_change(xhci_controller *xhci, xhci_slot_state *slot,
                              uint8_t port, const usb_port_status *status) {
    if (status->wPortChange & (1U << 0)) {
        (void)xhci_hub_clear_port_feature(xhci, slot, port,
                                          USB_PORT_FEAT_C_CONNECTION);
    }
    if (status->wPortChange & (1U << 1)) {
        (void)xhci_hub_clear_port_feature(xhci, slot, port,
                                          USB_PORT_FEAT_C_ENABLE);
    }
    if (status->wPortChange & (1U << 3)) {
        (void)xhci_hub_clear_port_feature(xhci, slot, port,
                                          USB_PORT_FEAT_C_OVER_CURRENT);
    }
    if (status->wPortChange & (1U << 4)) {
        (void)xhci_hub_clear_port_feature(xhci, slot, port,
                                          USB_PORT_FEAT_C_RESET);
    }
}

int xhci_hub_detect_port(xhci_controller *xhci, xhci_slot_state *slot,
                         uint8_t port, usb_port_status *status) {
    int rc = xhci_hub_get_port_status(xhci, slot, port, status);
    if (rc != 0) {
        return rc;
    }
    return (status->wPortStatus & USB_PORT_STAT_CONNECTION) ? 1 : 0;
}

int xhci_hub_reset_port(xhci_controller *xhci, xhci_slot_state *slot,
                        uint8_t port, uint8_t *speed_out) {
    int rc = xhci_hub_set_port_feature(xhci, slot, port, USB_PORT_FEAT_RESET);
    if (rc != 0) {
        return rc;
    }

    usb_port_status status;
    for (uint32_t waited = 0; waited < 2000; waited += 5) {
        delay_ms_hp(5);
        rc = xhci_hub_get_port_status(xhci, slot, port, &status);
        if (rc != 0) {
            return rc;
        }

        if ((status.wPortStatus & USB_PORT_STAT_RESET) == 0 &&
            (slot->speed_id != XHCI_SPEED_SUPER ||
             (status.wPortStatus & USB_PORT_STAT_LINK_MASK) == 0)) {
            if ((status.wPortStatus & USB_PORT_STAT_CONNECTION) == 0) {
                return -ENODEV;
            }

            if (slot->speed_id == XHCI_SPEED_SUPER) {
                *speed_out = XHCI_SPEED_SUPER;
            } else if (status.wPortStatus & USB_PORT_STAT_HIGH_SPEED) {
                *speed_out = XHCI_SPEED_HIGH;
            } else if (status.wPortStatus & USB_PORT_STAT_LOW_SPEED) {
                *speed_out = XHCI_SPEED_LOW;
            } else {
                *speed_out = XHCI_SPEED_FULL;
            }

            xhci_hub_ack_port_change(xhci, slot, port, &status);
            return 0;
        }
    }

    return -ETIMEDOUT;
}

static int xhci_configure_hub_context(xhci_controller *xhci,
                                      xhci_slot_state *slot) {
    if (!slot->is_hub || slot->hub_context_configured) {
        return 0;
    }

    uint64_t input_ctx_phys = 0;
    void *input_ctx = xhci_alloc_input_ctx(xhci, &input_ctx_phys);
    if (!input_ctx) {
        return -ENOMEM;
    }

    xhci_input_ctrl_ctx(input_ctx)->add_flags = XHCI_INPUT_ADD_SLOT;
    xhci_slot_ctx *slot_ctx = xhci_input_slot_ctx(xhci, input_ctx);
    slot_ctx->ctx[0] = XHCI_SLOT_CTX_HUB;
    slot_ctx->ctx[1] =
        (uint32_t)slot->hub_port_count << XHCI_SLOT_CTX_NUM_PORTS_SHIFT;

    if (slot->speed_id == XHCI_SPEED_HIGH) {
        slot->child_tt_info_base =
            (((uint32_t)(slot->hub_characteristics & USB_HUB_CHAR_TTT_MASK)) >>
             USB_HUB_CHAR_TTT_SHIFT)
            << XHCI_SLOT_TTT_SHIFT;
        slot_ctx->ctx[2] = slot->child_tt_info_base;
    } else {
        slot->child_tt_info_base = slot->tt_info;
    }

    int rc = xhci_cmd_configure_endpoint(xhci, slot->slot_id, input_ctx_phys);
    xhci_free_input_ctx(xhci, input_ctx, input_ctx_phys);
    if (rc == 0) {
        slot->hub_context_configured = true;
    }
    return rc;
}

int xhci_setup_hub(xhci_controller *xhci, xhci_slot_state *slot) {
    usb_hub_descriptor_prefix hub_desc;
    int rc = xhci_hub_get_descriptor(xhci, slot, &hub_desc);
    if (rc != 0) {
        return rc;
    }

    slot->hub_port_count = hub_desc.bNbrPorts;
    slot->hub_characteristics = hub_desc.wHubCharacteristics;

    rc = xhci_configure_hub_context(xhci, slot);
    if (rc != 0) {
        return rc;
    }

    rc = xhci_hub_set_depth(xhci, slot);
    if (rc != 0) {
        return rc;
    }

    for (uint8_t port = 1; port <= slot->hub_port_count; ++port) {
        rc = xhci_hub_set_port_feature(xhci, slot, port, USB_PORT_FEAT_POWER);
        if (rc != 0) {
            return rc;
        }
    }

    delay_ms_hp((uint64_t)hub_desc.bPwrOn2PwrGood * 2U);
    return 0;
}

static int xhci_prepare_slot(xhci_controller *xhci,
                             const xhci_slot_state *parent_slot,
                             uint8_t parent_port, uint8_t speed_id,
                             bool block_set_address, xhci_slot_state *slot) {
    memset(slot, 0, sizeof(*slot));
    slot->port_id = parent_slot ? parent_slot->port_id : parent_port;
    slot->parent_port = parent_port;
    slot->speed_id = speed_id;
    slot->level = parent_slot ? (uint8_t)(parent_slot->level + 1) : 1;
    slot->parent_slot_id = parent_slot ? (uint8_t)parent_slot->slot_id : 0;
    slot->max_packet0 = xhci_default_ep0_packet(speed_id);
    slot->child_tt_info_base = slot->tt_info;

    if (parent_slot) {
        slot->route_string = xhci_route_string_append(parent_slot, parent_port);
        if (slot->route_string == XHCI_INVALID_INDEX) {
            return -ENOSPC;
        }

        if (speed_id == XHCI_SPEED_LOW || speed_id == XHCI_SPEED_FULL) {
            if (parent_slot->speed_id == XHCI_SPEED_HIGH) {
                slot->tt_info = (parent_slot->slot_id & 0xFFU) |
                                ((uint32_t)parent_port << 8) |
                                parent_slot->child_tt_info_base;
            } else {
                slot->tt_info = parent_slot->child_tt_info_base;
            }
        }
    }

    int rc = xhci_ring_alloc(&slot->ep0_ring, true);
    if (rc != 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_EP0_RING);
        return rc;
    }
    XHCI_DIAG_OK(XHCI_DIAG_EP0_RING);

    rc = xhci_dma_alloc(xhci->context_size * XHCI_CONTEXTS, &slot->output_ctx,
                        &slot->output_ctx_phys);
    if (rc != 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_OUTPUT_CTX);
        xhci_ring_free(&slot->ep0_ring);
        return rc;
    }
    XHCI_DIAG_OK(XHCI_DIAG_OUTPUT_CTX);

    XHCI_DIAG_PENDING(XHCI_DIAG_ENABLE_SLOT);
    int slot_id = xhci_cmd_enable_slot(xhci);
    if (slot_id <= 0 || (uint32_t)slot_id > xhci->num_slots) {
        XHCI_DIAG_FAIL(XHCI_DIAG_ENABLE_SLOT);
        xhci_dma_free(slot->output_ctx, slot->output_ctx_phys,
                      xhci->context_size * XHCI_CONTEXTS);
        xhci_ring_free(&slot->ep0_ring);
        return -EIO;
    }
    XHCI_DIAG_OK(XHCI_DIAG_ENABLE_SLOT);

    slot->slot_id = (uint32_t)slot_id;
    slot->used = true;

    xhci->dcbaa[slot_id].ptr_low = (uint32_t)slot->output_ctx_phys;
    xhci->dcbaa[slot_id].ptr_high = (uint32_t)(slot->output_ctx_phys >> 32);

    XHCI_DIAG_PENDING(XHCI_DIAG_ADDRESS_DEVICE);
    XHCI_DIAG_PENDING(XHCI_DIAG_INPUT_CTX);
    rc = xhci_address_device(xhci, slot, block_set_address);
    if (rc != 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_INPUT_CTX);
        XHCI_DIAG_FAIL(XHCI_DIAG_ADDRESS_DEVICE);
        xhci_cmd_disable_slot(xhci, slot->slot_id);
        xhci_dma_free(slot->output_ctx, slot->output_ctx_phys,
                      xhci->context_size * XHCI_CONTEXTS);
        xhci_ring_free(&slot->ep0_ring);
        memset(slot, 0, sizeof(*slot));
        return rc;
    }
    XHCI_DIAG_OK(XHCI_DIAG_INPUT_CTX);
    XHCI_DIAG_OK(XHCI_DIAG_ADDRESS_DEVICE);

    return 0;
}

static void xhci_release_slot(xhci_controller *xhci, xhci_slot_state *slot) {
    if (!slot || !slot->used) {
        return;
    }

    xhci_release_hid(slot);
    xhci_release_msc(slot);

    if (slot->config_buffer) {
        free(slot->config_buffer);
        slot->config_buffer = NULL;
    }

    if (slot->slot_id != 0) {
        xhci_cmd_disable_slot(xhci, slot->slot_id);
        xhci->dcbaa[slot->slot_id].ptr_low = 0;
        xhci->dcbaa[slot->slot_id].ptr_high = 0;
    }

    xhci_ring_free(&slot->ep0_ring);
    xhci_dma_free(slot->output_ctx, slot->output_ctx_phys,
                  xhci->context_size * XHCI_CONTEXTS);
    memset(slot, 0, sizeof(*slot));
}

static xhci_slot_state *xhci_find_root_slot_by_port(xhci_controller *xhci,
                                                    uint8_t port_id) {
    if (!xhci || !xhci->slots || port_id == 0) {
        return NULL;
    }

    for (uint32_t i = 1; i <= xhci->num_slots; ++i) {
        xhci_slot_state *slot = &xhci->slots[i];
        if (!slot->used || slot->parent_slot_id != 0) {
            continue;
        }
        if (slot->port_id == port_id) {
            return slot;
        }
    }
    return NULL;
}

static void xhci_release_slot_tree(xhci_controller *xhci, uint32_t slot_id) {
    if (!xhci || !xhci->slots || slot_id == 0 || slot_id > xhci->num_slots) {
        return;
    }

    for (uint32_t i = 1; i <= xhci->num_slots; ++i) {
        if (xhci->slots[i].used && xhci->slots[i].parent_slot_id == slot_id) {
            xhci_release_slot_tree(xhci, i);
        }
    }

    if (xhci->slots[slot_id].used) {
        xhci_release_slot(xhci, &xhci->slots[slot_id]);
    }
}

static int xhci_reset_port(xhci_controller *xhci, uint32_t port_idx,
                           uint8_t *speed_out) {
    XHCI_DIAG_PENDING(XHCI_DIAG_PORT_RESET);
    volatile uint32_t *portsc_reg = &xhci->ports[port_idx].portsc;
    uint32_t portsc = xhci_read32(portsc_reg);
    if ((portsc & XHCI_PORTSC_CCS) == 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_PORT_PRESENT);
        return -ENODEV;
    }
    XHCI_DIAG_OK(XHCI_DIAG_PORT_PRESENT);

    uint32_t port_number = port_idx + 1;
    bool is_usb2_port =
        (xhci->usb2.start != 0 && port_number >= xhci->usb2.start &&
         port_number < (uint32_t)xhci->usb2.start + xhci->usb2.count);

    if (is_usb2_port || (portsc & XHCI_PORTSC_PED) == 0) {
        xhci_clear_port_change_bits(xhci, port_idx);
        portsc = xhci_read32(portsc_reg);
        xhci_write32(portsc_reg, xhci_portsc_write_value(portsc, XHCI_PORTSC_PR));

        for (uint32_t waited = 0; waited < 2000; ++waited) {
            portsc = xhci_read32(portsc_reg);
            if ((portsc & XHCI_PORTSC_CCS) == 0) {
                xhci_diag_portsc(portsc);
                XHCI_DIAG_FAIL(XHCI_DIAG_PORT_RESET);
                return -ENODEV;
            }
            if ((portsc & XHCI_PORTSC_PR) == 0 &&
                (portsc & XHCI_PORTSC_PRC) != 0) {
                break;
            }
            delay_ms_hp(1);
        }
    }

    portsc = xhci_read32(portsc_reg);
    if (is_usb2_port) {
        for (uint32_t waited = 0; waited < 500; ++waited) {
            uint32_t pls = (portsc >> XHCI_PORTSC_PLS_SHIFT) &
                           XHCI_PORTSC_PLS_MASK;
            if (pls == XHCI_PLS_U0) {
                break;
            }
            delay_ms_hp(1);
            portsc = xhci_read32(portsc_reg);
        }
    }
    if ((portsc & XHCI_PORTSC_PED) == 0) {
        xhci_diag_portsc(portsc);
        XHCI_DIAG_FAIL(XHCI_DIAG_PORT_RESET);
        return -ETIMEDOUT;
    }

    xhci_clear_port_change_bits(xhci, port_idx);
    delay_ms_hp(10);
    portsc = xhci_read32(portsc_reg);
    xhci_diag_portsc(portsc);
    if ((portsc & XHCI_PORTSC_CCS) == 0 ||
        (portsc & XHCI_PORTSC_PED) == 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_PORT_RESET);
        return -ENODEV;
    }

    *speed_out = (uint8_t)xhci_field(portsc, XHCI_PORTSC_SPEED_SHIFT,
                                     XHCI_PORTSC_SPEED_MASK);
    XHCI_DIAG_OK(XHCI_DIAG_PORT_RESET);
    return 0;
}

int xhci_usb_enumerate_device(xhci_controller *xhci, usb_hub *parent_hub,
                              uint8_t parent_port, uint8_t speed_id,
                              usb_device **device_out) {
    XHCI_DIAG_PENDING(XHCI_DIAG_SLOT);
    const xhci_slot_state *parent_slot =
        (parent_hub && parent_hub->usbdev) ? parent_hub->usbdev->slot : NULL;
    xhci_slot_state temp_slot;
    bool use_bsr_address = (speed_id == XHCI_SPEED_LOW);
    int rc = xhci_prepare_slot(xhci, parent_slot, parent_port, speed_id,
                               use_bsr_address, &temp_slot);
    if (rc != 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_SLOT);
        pr_info("xhci: prepare slot failed root-port=%u parent-slot=%u parent-port=%u speed=%u rc=%d\n",
               parent_slot ? parent_slot->port_id : parent_port,
               parent_slot ? parent_slot->slot_id : 0, parent_port, speed_id,
               rc);
        return rc;
    }
    XHCI_DIAG_OK(XHCI_DIAG_SLOT);

    xhci_slot_state *slot = &xhci->slots[temp_slot.slot_id];
    *slot = temp_slot;

    usb_device_descriptor first_desc;
    memset(&first_desc, 0, sizeof(first_desc));

    usb_setup_packet first_read = {
        .bmRequestType = USB_DIR_IN,
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = (uint16_t)(USB_DT_DEVICE << 8),
        .wIndex = 0,
        .wLength = 8,
    };

    uint32_t actual = 0;
    XHCI_DIAG_PENDING(XHCI_DIAG_DESCRIPTOR);
    rc = xhci_control_transfer(xhci, slot, &first_read, &first_desc, 8,
                               true, &actual);
    if (rc != 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_DESCRIPTOR);
        pr_info("xhci: initial device descriptor read failed slot=%u root-port=%u rc=%d\n",
               slot->slot_id, slot->port_id, rc);
        xhci_release_slot(xhci, slot);
        return rc;
    }

    uint16_t desc_ep0 = xhci_descriptor_ep0_packet(speed_id, &first_desc);
    if (desc_ep0 != 0 && desc_ep0 != slot->max_packet0 &&
        speed_id != XHCI_SPEED_SUPER) {
        if (use_bsr_address) {
            slot->max_packet0 = desc_ep0;
        } else {
            rc = xhci_update_ep0_packet(xhci, slot, desc_ep0);
            if (rc != 0) {
                pr_info("xhci: update ep0 packet failed slot=%u root-port=%u mps=%u rc=%d\n",
                       slot->slot_id, slot->port_id, desc_ep0, rc);
                xhci_release_slot(xhci, slot);
                return rc;
            }
        }
    }

    if (use_bsr_address) {
        xhci_ring_free(&slot->ep0_ring);
        rc = xhci_ring_alloc(&slot->ep0_ring, true);
        if (rc != 0) {
            XHCI_DIAG_FAIL(XHCI_DIAG_EP0_RING);
            pr_info("xhci: reset ep0 ring failed slot=%u root-port=%u rc=%d\n",
                   slot->slot_id, slot->port_id, rc);
            xhci_release_slot(xhci, slot);
            return rc;
        }
        XHCI_DIAG_OK(XHCI_DIAG_EP0_RING);

        XHCI_DIAG_PENDING(XHCI_DIAG_ADDRESS_DEVICE);
        rc = xhci_address_device(xhci, slot, false);
        if (rc != 0) {
            XHCI_DIAG_FAIL(XHCI_DIAG_ADDRESS_DEVICE);
            pr_info("xhci: finalize address failed slot=%u root-port=%u rc=%d\n",
                   slot->slot_id, slot->port_id, rc);
            xhci_release_slot(xhci, slot);
            return rc;
        }
        XHCI_DIAG_OK(XHCI_DIAG_ADDRESS_DEVICE);
        delay_ms_hp(10);
    }

    rc = xhci_read_device_descriptor(xhci, slot, &slot->device_desc);
    if (rc != 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_DESCRIPTOR);
        pr_info("xhci: full device descriptor read failed slot=%u root-port=%u rc=%d\n",
               slot->slot_id, slot->port_id, rc);
        xhci_release_slot(xhci, slot);
        return rc;
    }
    XHCI_DIAG_OK(XHCI_DIAG_DESCRIPTOR);
    slot->is_hub = (slot->device_desc.bDeviceClass == USB_CLASS_HUB);

    if (slot->device_desc.bNumConfigurations != 0) {
        XHCI_DIAG_PENDING(XHCI_DIAG_CONFIG);
        void *config_buffer = NULL;
        uint16_t config_length = 0;
        rc = xhci_read_config_descriptor(xhci, slot, &config_buffer,
                                         &config_length);
        if (rc == 0) {
            slot->config_buffer = config_buffer;
            slot->config_length = config_length;
            xhci_parse_config(slot);

            if (slot->config_value != 0) {
                rc = xhci_set_configuration(xhci, slot, slot->config_value);
                if (rc == 0) {
                    slot->configured = true;
                } else {
                    pr_info("xhci: set configuration failed slot=%u root-port=%u value=%u rc=%d\n",
                           slot->slot_id, slot->port_id, slot->config_value,
                           rc);
                }
            }
        } else {
            XHCI_DIAG_FAIL(XHCI_DIAG_CONFIG);
            pr_info("xhci: config descriptor read failed slot=%u root-port=%u rc=%d\n",
                   slot->slot_id, slot->port_id, rc);
        }
    }
    if (slot->configured) {
        XHCI_DIAG_OK(XHCI_DIAG_CONFIG);
    }

    if (slot->configured && slot->hid.supported) {
        XHCI_DIAG_PENDING(XHCI_DIAG_HID);
        rc = xhci_hid_set_protocol(xhci, slot, 0);
        if (rc != 0) {
            XHCI_DIAG_WARN(XHCI_DIAG_HID);
            pr_info("xhci: slot %u hid set protocol failed iface=%u rc=%d, continuing\n",
                   slot->slot_id, slot->hid.interface_number, rc);
        } else {
            if (slot->hid.kind == XHCI_HID_KEYBOARD) {
                rc = xhci_hid_set_idle(xhci, slot, 0, 0);
                if (rc != 0) {
                    XHCI_DIAG_WARN(XHCI_DIAG_HID);
                    pr_info("xhci: slot %u hid set idle failed iface=%u rc=%d, continuing\n",
                           slot->slot_id, slot->hid.interface_number, rc);
                }
            }
        }

        if (slot->hid.supported) {
            rc = xhci_configure_hid_endpoint(xhci, slot);
            if (rc != 0) {
                XHCI_DIAG_FAIL(XHCI_DIAG_HID);
                pr_info("xhci: slot %u hid endpoint configure failed ep=0x%02x rc=%d\n",
                       slot->slot_id, slot->hid.endpoint_address, rc);
                xhci_release_hid(slot);
            } else {
                XHCI_DIAG_OK(XHCI_DIAG_HID);
            }
        }
    }

    usb_device *usbdev = (usb_device *)malloc(sizeof(*usbdev));
    if (!usbdev) {
        xhci_release_slot(xhci, slot);
        return -ENOMEM;
    }
    memset(usbdev, 0, sizeof(*usbdev));
    usbdev->controller = xhci;
    usbdev->slot = slot;
    usbdev->parent_hub = parent_hub;
    usbdev->online = true;
    usbdev->port = parent_port;
    usbdev->speed_id = speed_id;
    usbdev->vendor_id = slot->device_desc.idVendor;
    usbdev->product_id = slot->device_desc.idProduct;
    usbdev->device_desc = slot->device_desc;
    usbdev->config_desc = slot->config_desc;
    usbdev->config_buffer = slot->config_buffer;
    usbdev->config_length = slot->config_length;
    usbdev->config_value = slot->config_value;
    slot->usbdev = usbdev;

    usb_fill_topology(usbdev);
    int parse_rc = usb_parse_configuration(usbdev);
    for (uint8_t i = 0; i < usbdev->interface_count; ++i) {
        if (usbdev->interfaces[i].interface_class == USB_CLASS_HUB) {
            slot->is_hub = true;
            break;
        }
    }

    pr_debug("xhci: probe topology=%s slot=%u parse_rc=%d ifaces=%u parsed-hub=%u parsed-msc=%u active=%02x/%02x/%02x configured=%u\n",
             usbdev->topology, slot->slot_id, parse_rc,
             usbdev->interface_count,
             slot->is_hub ? 1U : 0U, slot->msc.supported ? 1U : 0U,
             slot->active_iface_class, slot->active_iface_subclass,
             slot->active_iface_protocol, slot->configured ? 1U : 0U);

    int probe_rc = usb_probe_device(usbdev);
    if (probe_rc != 0 && slot->configured &&
        (slot->msc.supported ||
         (slot->active_iface_class == USB_CLASS_MASS_STORAGE &&
          slot->active_iface_subclass == USB_MSC_SUBCLASS_SCSI &&
          slot->active_iface_protocol == USB_MSC_PROTOCOL_BBB))) {
        probe_rc = usb_fallback_probe_msc(usbdev);
    }
    if (probe_rc != 0 && slot->configured &&
        (slot->is_hub || slot->active_iface_class == USB_CLASS_HUB)) {
        probe_rc = usb_fallback_probe_hub(usbdev);
    }
    if (probe_rc != 0) {
        pr_info("xhci: probe failed topology=%s slot=%u rc=%d active=%02x/%02x/%02x ifaces=%u\n",
               usbdev->topology, slot->slot_id, probe_rc,
               slot->active_iface_class, slot->active_iface_subclass,
               slot->active_iface_protocol, usbdev->interface_count);
    }

    uint16_t max_packet =
        xhci_descriptor_ep0_packet(speed_id, &slot->device_desc);
    if (max_packet == 0) {
        max_packet = slot->max_packet0;
    }

    pr_info("xhci: slot %u root-port=%u parent-slot=%u parent-port=%u level=%u route=0x%x %s-speed device VID=%04x PID=%04x class=%02x/%02x/%02x mps0=%u cfg=%u/%u\n",
           slot->slot_id, slot->port_id, slot->parent_slot_id,
           slot->parent_port, slot->level, slot->route_string,
           xhci_speed_name(speed_id),
           slot->device_desc.idVendor, slot->device_desc.idProduct,
           slot->device_desc.bDeviceClass, slot->device_desc.bDeviceSubClass,
           slot->device_desc.bDeviceProtocol, max_packet, slot->config_value,
           slot->device_desc.bNumConfigurations);

    if (slot->config_desc.bLength >= sizeof(usb_config_descriptor)) {
        pr_info("xhci: slot %u config total_len=%u interfaces=%u value=%u max_power=%u iface=%02x/%02x/%02x\n",
               slot->slot_id, slot->config_desc.wTotalLength,
               slot->config_desc.bNumInterfaces,
               slot->config_desc.bConfigurationValue,
               slot->config_desc.bMaxPower, slot->active_iface_class,
               slot->active_iface_subclass, slot->active_iface_protocol);
    }

    if (slot->hid.ready) {
        XHCI_DIAG_OK(XHCI_DIAG_HID_ARMED);
        pr_info("xhci: slot %u boot %s armed iface=%u ep=0x%02x epid=%u mps=%u interval=%u\n",
               slot->slot_id, xhci_hid_kind_name(slot->hid.kind),
               slot->hid.interface_number, slot->hid.endpoint_address,
               slot->hid.endpoint_epid, slot->hid.max_packet,
               slot->hid.endpoint_interval);
        xhci->hid_devices_present = true;
    }

    if (device_out) {
        *device_out = usbdev;
    }

    return 0;
}

void xhci_usb_release_device(usb_device *usbdev) {
    if (!usbdev || !usbdev->controller || !usbdev->slot) {
        return;
    }
    xhci_release_slot(usbdev->controller, usbdev->slot);
}

static int xhci_root_hub_detect(usb_hub *hub, uint8_t port,
                                usb_port_status *status) {
    if (!hub || !hub->controller || port >= hub->controller->num_ports) {
        return -ENODEV;
    }

    uint32_t portsc = xhci_read32(&hub->controller->ports[port].portsc);
    if (status) {
        memset(status, 0, sizeof(*status));
        if (portsc & XHCI_PORTSC_CCS) {
            status->wPortStatus |= USB_PORT_STAT_CONNECTION;
        }
        if (portsc & XHCI_PORTSC_PED) {
            status->wPortStatus |= USB_PORT_STAT_ENABLE;
        }
        uint8_t speed = (uint8_t)xhci_field(portsc, XHCI_PORTSC_SPEED_SHIFT,
                                            XHCI_PORTSC_SPEED_MASK);
        if (speed == XHCI_SPEED_LOW) {
            status->wPortStatus |= USB_PORT_STAT_LOW_SPEED;
        } else if (speed == XHCI_SPEED_HIGH) {
            status->wPortStatus |= USB_PORT_STAT_HIGH_SPEED;
        }
    }
    return (portsc & XHCI_PORTSC_CCS) ? 1 : 0;
}

static int xhci_root_hub_reset(usb_hub *hub, uint8_t port,
                               uint8_t *speed_out) {
    if (!hub || !hub->controller) {
        return -ENODEV;
    }
    return xhci_reset_port(hub->controller, port, speed_out);
}

static void xhci_root_hub_disconnect(usb_hub *hub, uint8_t port) {
    (void)hub;
    (void)port;
}

static void xhci_root_hub_ack(usb_hub *hub, uint8_t port,
                              const usb_port_status *status) {
    (void)hub;
    (void)port;
    (void)status;
}

static const usb_hub_ops g_xhci_root_hub_ops = {
    .detect = xhci_root_hub_detect,
    .reset = xhci_root_hub_reset,
    .disconnect = xhci_root_hub_disconnect,
    .ack = xhci_root_hub_ack,
};

static void xhci_service_worker(void *arg) {
    xhci_controller *xhci = (xhci_controller *)arg;
    for (;;) {
        if (!xhci || !xhci_is_registered_controller(xhci)) {
            delay_ms_hp(10);
            continue;
        }

        xhci_process_events(xhci);

        for (uint32_t i = 1; i <= xhci->num_slots; ++i) {
            xhci_slot_state *slot = &xhci->slots[i];
            if (!slot->used || !slot->hid.ready) {
                continue;
            }
            (void)xhci_submit_hid_transfer(xhci, slot);
        }

        cpu_relax();
        scheduler_yield();
    }
}

static void xhci_parse_protocol_caps(xhci_controller *xhci) {
    if (xhci->ext_caps_off == 0) {
        return;
    }

    uint8_t *addr = (uint8_t *)xhci->mmio + xhci->ext_caps_off;
    for (;;) {
        xhci_ext_cap *cap = (xhci_ext_cap *)addr;
        uint32_t header = cap->cap;
        __sync_synchronize();
        uint32_t cap_id = header & 0xFFU;
        uint32_t next = (header >> 8) & 0xFFU;

        if (cap_id == XHCI_PROTOCOL_CAP_ID) {
            uint32_t name = xhci_read32(&cap->data[0]);
            uint32_t ports = xhci_read32(&cap->data[1]);
            uint8_t major = (header >> 24) & 0xFFU;
            uint8_t minor = (header >> 16) & 0xFFU;
            uint8_t count = (ports >> 8) & 0xFFU;
            uint8_t start = ports & 0xFFU;

            pr_info("xhci: supported protocol capability discovered\n");

            if (name == XHCI_PROTOCOL_USB_NAME) {
                if (major == 2) {
                    xhci->usb2.start = start;
                    xhci->usb2.count = count;
                } else if (major == 3) {
                    xhci->usb3.start = start;
                    xhci->usb3.count = count;
                }
            }
        }

        if (next == 0) {
            break;
        }
        addr += (next << 2);
    }
}

static void xhci_bios_handoff(xhci_controller *xhci) {
    if (xhci->ext_caps_off == 0) {
        return;
    }

    uint8_t *addr = (uint8_t *)xhci->mmio + xhci->ext_caps_off;
    for (;;) {
        uint32_t header = xhci_read32((uint32_t *)addr);
        uint32_t cap_id = header & 0xFFU;
        uint32_t next = (header >> 8) & 0xFFU;

        if (cap_id == XHCI_LEGACY_CAP_ID) {
            volatile uint32_t *legacy_sup = (volatile uint32_t *)(addr + 4);
            volatile uint32_t *legacy_ctl = (volatile uint32_t *)(addr + 8);
            uint32_t value = xhci_read32(legacy_sup);

            if (value & (1U << 16)) {
                pr_info("xhci: BIOS owns controller, requesting handoff\n");
                xhci_write32(legacy_sup, value | (1U << 24));

                for (uint32_t waited = 0; waited < 1000; ++waited) {
                    value = xhci_read32(legacy_sup);
                    if ((value & (1U << 16)) == 0) {
                        break;
                    }
                    delay_ms_hp(1);
                }

                value = xhci_read32(legacy_sup);
                if (value & (1U << 16)) {
                    pr_info("xhci: BIOS handoff timed out\n");
                } else {
                    pr_info("xhci: BIOS handoff complete\n");
                }
            }

            xhci_write32(legacy_ctl, 0);
        }

        if (next == 0) {
            break;
        }
        addr += (next << 2);
    }
}

static int xhci_setup_scratchpad(xhci_controller *xhci) {
    xhci->scratchpad_count =
        xhci_max_scratchpad(xhci_read32(&xhci->caps->hcs_params2));
    if (xhci->scratchpad_count == 0) {
        return 0;
    }

    int rc = xhci_dma_alloc(sizeof(uint64_t) * xhci->scratchpad_count,
                            (void **)&xhci->scratchpad_array,
                            &xhci->scratchpad_array_phys);
    if (rc != 0) {
        return rc;
    }

    rc = xhci_dma_alloc((size_t)xhci->scratchpad_count * PAGE_SIZE,
                        &xhci->scratchpad_pages, &xhci->scratchpad_pages_phys);
    if (rc != 0) {
        xhci_dma_free(xhci->scratchpad_array, xhci->scratchpad_array_phys,
                      sizeof(uint64_t) * xhci->scratchpad_count);
        xhci->scratchpad_array = NULL;
        xhci->scratchpad_array_phys = 0;
        return rc;
    }

    for (uint32_t i = 0; i < xhci->scratchpad_count; ++i) {
        xhci->scratchpad_array[i] =
            xhci->scratchpad_pages_phys + (uint64_t)i * PAGE_SIZE;
    }

    xhci->dcbaa[0].ptr_low = (uint32_t)xhci->scratchpad_array_phys;
    xhci->dcbaa[0].ptr_high = (uint32_t)(xhci->scratchpad_array_phys >> 32);
    return 0;
}

static int xhci_runtime_alloc(xhci_controller *xhci) {
    int rc = xhci_dma_alloc(sizeof(xhci_dcbaa_entry) * (xhci->num_slots + 1),
                            (void **)&xhci->dcbaa, &xhci->dcbaa_phys);
    if (rc != 0) {
        return rc;
    }

    rc = xhci_ring_alloc(&xhci->cmd_ring, true);
    if (rc != 0) {
        return rc;
    }

    rc = xhci_event_ring_alloc(&xhci->event_ring);
    if (rc != 0) {
        return rc;
    }

    rc = xhci_dma_alloc(sizeof(xhci_erst_entry), (void **)&xhci->erst,
                        &xhci->erst_phys);
    if (rc != 0) {
        return rc;
    }

    xhci->slots = (xhci_slot_state *)xhci_zero_alloc(
        sizeof(xhci_slot_state) * (xhci->num_slots + 1));
    if (!xhci->slots) {
        return -ENOMEM;
    }

    xhci->root_port_pending =
        (uint8_t *)xhci_zero_alloc((size_t)xhci->num_ports + 1);
    if (!xhci->root_port_pending) {
        free(xhci->slots);
        xhci->slots = NULL;
        return -ENOMEM;
    }

    xhci->erst[0].ptr_low = (uint32_t)xhci->event_ring.phys;
    xhci->erst[0].ptr_high = (uint32_t)(xhci->event_ring.phys >> 32);
    xhci->erst[0].size = xhci->event_ring.count;

    return 0;
}

static void xhci_runtime_free(xhci_controller *xhci) {
    if (!xhci) {
        return;
    }

    if (xhci->slots) {
        for (uint32_t i = 1; i <= xhci->num_slots; ++i) {
            if (xhci->slots[i].used) {
                xhci_release_slot(xhci, &xhci->slots[i]);
            }
        }
        free(xhci->slots);
        xhci->slots = NULL;
    }

    if (xhci->root_port_pending) {
        free(xhci->root_port_pending);
        xhci->root_port_pending = NULL;
    }

    if (xhci->scratchpad_pages) {
        xhci_dma_free(xhci->scratchpad_pages, xhci->scratchpad_pages_phys,
                      (size_t)xhci->scratchpad_count * PAGE_SIZE);
        xhci->scratchpad_pages = NULL;
        xhci->scratchpad_pages_phys = 0;
    }

    if (xhci->scratchpad_array) {
        xhci_dma_free(xhci->scratchpad_array, xhci->scratchpad_array_phys,
                      sizeof(uint64_t) * xhci->scratchpad_count);
        xhci->scratchpad_array = NULL;
        xhci->scratchpad_array_phys = 0;
    }
    xhci->scratchpad_count = 0;

    if (xhci->erst) {
        xhci_dma_free(xhci->erst, xhci->erst_phys, sizeof(xhci_erst_entry));
        xhci->erst = NULL;
        xhci->erst_phys = 0;
    }

    xhci_event_ring_free(&xhci->event_ring);
    xhci_ring_free(&xhci->cmd_ring);

    if (xhci->dcbaa) {
        xhci_dma_free(xhci->dcbaa, xhci->dcbaa_phys,
                      sizeof(xhci_dcbaa_entry) * (xhci->num_slots + 1));
        xhci->dcbaa = NULL;
        xhci->dcbaa_phys = 0;
    }
}

static int xhci_start_controller(xhci_controller *xhci) {
    XHCI_DIAG_PENDING(XHCI_DIAG_CONTROLLER);
    uint32_t cmd = xhci_read32(&xhci->op->usb_cmd);
    if (cmd & XHCI_CMD_RS) {
        xhci_write32(&xhci->op->usb_cmd, cmd & ~XHCI_CMD_RS);
        if (xhci_wait_bit(&xhci->op->usb_sts, XHCI_STS_HCH, XHCI_STS_HCH,
                          3000) != 0) {
            XHCI_DIAG_FAIL(XHCI_DIAG_CONTROLLER);
            return -EIO;
        }
    }

    xhci_write32(&xhci->op->usb_cmd, XHCI_CMD_HCRST);
    if (xhci_wait_bit(&xhci->op->usb_cmd, XHCI_CMD_HCRST, 0, 3000) != 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_CONTROLLER);
        return -EIO;
    }
    if (xhci_wait_bit(&xhci->op->usb_sts, XHCI_STS_CNR, 0, 3000) != 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_CONTROLLER);
        return -EIO;
    }

    int rc = xhci_runtime_alloc(xhci);
    if (rc != 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_CONTROLLER);
        return rc;
    }

    xhci_write32(&xhci->op->config, xhci->num_slots);
    xhci_write32(&xhci->op->dcbaap_low, (uint32_t)xhci->dcbaa_phys);
    xhci_write32(&xhci->op->dcbaap_high,
                 (uint32_t)(xhci->dcbaa_phys >> 32));

    uint64_t crcr = xhci->cmd_ring.phys;
    xhci_write32(&xhci->op->crcr_low, (uint32_t)(crcr & ~0x3FULL) | 1U);
    xhci_write32(&xhci->op->crcr_high, (uint32_t)(crcr >> 32));

    xhci_write32(&xhci->ir0->erstsz, 1);
    xhci_write32(&xhci->ir0->erstba_low, (uint32_t)xhci->erst_phys);
    xhci_write32(&xhci->ir0->erstba_high, (uint32_t)(xhci->erst_phys >> 32));
    xhci_event_ring_commit(xhci);
    xhci_write32(&xhci->ir0->imod, 0);
    xhci_write32(&xhci->ir0->iman, 0);

    rc = xhci_setup_scratchpad(xhci);
    if (rc != 0) {
        xhci_runtime_free(xhci);
        XHCI_DIAG_FAIL(XHCI_DIAG_CONTROLLER);
        return rc;
    }

    xhci_write32(&xhci->op->usb_sts, XHCI_STS_EINT);

    cmd = xhci_read32(&xhci->op->usb_cmd);
    cmd &= ~XHCI_CMD_INTE;
    cmd |= XHCI_CMD_RS;
    xhci_write32(&xhci->op->usb_cmd, cmd);

    if (xhci_wait_bit(&xhci->op->usb_sts, XHCI_STS_HCH, 0, 1000) != 0) {
        xhci_runtime_free(xhci);
        XHCI_DIAG_FAIL(XHCI_DIAG_CONTROLLER);
        return -EIO;
    }

    for (uint32_t port = 0; port < xhci->num_ports; ++port) {
        uint32_t portsc = xhci_read32(&xhci->ports[port].portsc);
        if ((portsc & XHCI_PORTSC_PP) == 0) {
            xhci_write32(&xhci->ports[port].portsc,
                         xhci_portsc_write_value(portsc, XHCI_PORTSC_PP));
        }
    }

    delay_ms_hp(20);
    XHCI_DIAG_OK(XHCI_DIAG_CONTROLLER);
    return 0;
}

static void xhci_stop_controller(xhci_controller *xhci) {
    if (!xhci) {
        return;
    }

    uint32_t cmd = xhci_read32(&xhci->op->usb_cmd);
    if (cmd & XHCI_CMD_RS) {
        xhci_write32(&xhci->op->usb_cmd, cmd & ~XHCI_CMD_RS);
        (void)xhci_wait_bit(&xhci->op->usb_sts, XHCI_STS_HCH, XHCI_STS_HCH,
                            1000);
    }
}

static void xhci_free_controller(xhci_controller *xhci) {
    if (!xhci) {
        return;
    }
    if (xhci->root_hub) {
        free(xhci->root_hub->port_changed);
        free(xhci->root_hub->children);
        free(xhci->root_hub);
        xhci->root_hub = NULL;
    }
    if (xhci->root_usbdev) {
        free(xhci->root_usbdev);
        xhci->root_usbdev = NULL;
    }
    xhci_stop_controller(xhci);
    xhci_runtime_free(xhci);
    free(xhci);
}

static int xhci_enable_pci_device(pci_device_t *pci_dev) {
    uint32_t cmd = pci_dev->op->read(pci_dev->bus, pci_dev->slot, pci_dev->func,
                                     pci_dev->segment, PCI_CONF_COMMAND);
    cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    pci_dev->op->write(pci_dev->bus, pci_dev->slot, pci_dev->func,
                       pci_dev->segment, PCI_CONF_COMMAND, cmd);
    return 0;
}

static int xhci_find_mmio_bar(const pci_device_t *pci_dev, uint64_t *base_out,
                              uint64_t *size_out) {
    for (int i = 0; i < 6; ++i) {
        if (pci_dev->bars[i].mmio && pci_dev->bars[i].size != 0) {
            *base_out = pci_dev->bars[i].address;
            *size_out = pci_dev->bars[i].size;
            return 0;
        }
    }
    return -ENODEV;
}

static xhci_controller *xhci_create_controller(pci_device_t *pci_dev,
                                               void *mmio_base) {
    xhci_controller *xhci =
        (xhci_controller *)xhci_zero_alloc(sizeof(xhci_controller));
    if (!xhci) {
        return NULL;
    }

    xhci->pci_dev = pci_dev;
    xhci->mmio = mmio_base;
    xhci->caps = (xhci_cap_regs *)mmio_base;
    xhci->op = (xhci_op_regs *)((uint8_t *)mmio_base +
                                xhci->caps->cap_length);
    xhci->ports = (xhci_port_regs *)((uint8_t *)xhci->op + 0x400);
    xhci->db = (xhci_db_reg *)((uint8_t *)mmio_base +
                               xhci_read32(&xhci->caps->db_offset));
    xhci->ir0 = (xhci_intr_regs *)((uint8_t *)mmio_base +
                                   xhci_read32(&xhci->caps->runtime_offset) +
                                   0x20);

    write_serial_fmt("Version %d \n", xhci->caps->hci_version);

    uint32_t hcs1 = xhci_read32(&xhci->caps->hcs_params1);
    uint32_t hcc1 = xhci_read32(&xhci->caps->hcc_params1);
    uint16_t hci_version = xhci->caps->hci_version;

    xhci->num_ports = xhci_field(hcs1, XHCI_HCS1_MAX_PORTS_SHIFT,
                                 XHCI_HCS1_MAX_PORTS_MASK);
    xhci->num_slots = hcs1 & XHCI_HCS1_MAX_SLOTS_MASK;
    xhci->num_irqs = xhci_field(hcs1, XHCI_HCS1_MAX_INTRS_SHIFT,
                                XHCI_HCS1_MAX_INTRS_MASK);
    xhci->ext_caps_off = ((hcc1 >> 16) & 0xFFFFU) << 2;
    xhci->context_size = (hcc1 & XHCI_HCC_CSZ) ? 64U : 32U;

    if ((xhci_read32(&xhci->op->page_size) & 1U) == 0) {
        pr_info("xhci: controller does not support 4K pages\n");
        free(xhci);
        return NULL;
    }

    xhci_bios_handoff(xhci);
    xhci_parse_protocol_caps(xhci);
    pr_info("xhci: spec version %u.%u (raw=0x%04x)\n",
           (unsigned int)((hci_version >> 8) & 0xFFU),
           (unsigned int)(hci_version & 0xFFU), hci_version);
    return xhci;
}

static int xhci_probe(pci_device_t *pci_dev) {
    XHCI_DIAG_PENDING(XHCI_DIAG_PROBE);
    uint64_t mmio_base = 0;
    uint64_t mmio_size = 0;
    int rc = xhci_find_mmio_bar(pci_dev, &mmio_base, &mmio_size);
    if (rc != 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_MMIO);
        pr_info("xhci: no MMIO BAR found\n");
        return rc;
    }
    XHCI_DIAG_OK(XHCI_DIAG_MMIO);

    xhci_enable_pci_device(pci_dev);

    void *mmio_vaddr = driver_phys_to_virt(mmio_base);
    page_map_range(get_current_directory(), (uint64_t)mmio_vaddr, mmio_base,
                   mmio_size, XHCI_MMIO_PTE_FLAGS);

    xhci_controller *xhci = xhci_create_controller(pci_dev, mmio_vaddr);
    if (!xhci) {
        XHCI_DIAG_FAIL(XHCI_DIAG_PROBE);
        return -ENOMEM;
    }

    rc = xhci_start_controller(xhci);
    if (rc != 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_PROBE);
        pr_info("xhci: failed to start controller (%d)\n", rc);
        xhci_free_controller(xhci);
        return rc;
    }

    pr_info("xhci: controller ready ports=%u slots=%u intrs=%u ctx=%u\n",
           xhci->num_ports, xhci->num_slots, xhci->num_irqs,
           xhci->context_size);

    usb_core_init();
    usb_register_builtin_hub_driver();
    usb_register_builtin_msc_driver();

    usb_hub *root_hub = (usb_hub *)malloc(sizeof(*root_hub));
    usb_device *root_dev = (usb_device *)malloc(sizeof(*root_dev));
    if (!root_hub || !root_dev) {
        XHCI_DIAG_FAIL(XHCI_DIAG_ROOT_HUB);
        free(root_hub);
        free(root_dev);
        xhci_free_controller(xhci);
        return -ENOMEM;
    }
    memset(root_hub, 0, sizeof(*root_hub));
    memset(root_dev, 0, sizeof(*root_dev));

    root_hub->controller = xhci;
    root_hub->usbdev = root_dev;
    root_hub->ops = &g_xhci_root_hub_ops;
    root_hub->port_count = (uint8_t)xhci->num_ports;

    root_dev->controller = xhci;
    root_dev->child_hub = root_hub;
    root_dev->is_root_hub = true;
    root_dev->online = true;
    snprintf(root_dev->topology, sizeof(root_dev->topology), "usb-root");

    xhci->root_hub = root_hub;
    xhci->root_usbdev = root_dev;
    xhci->next = g_xhci;
    g_xhci = xhci;
    XHCI_DIAG_OK(XHCI_DIAG_ROOT_HUB);
    usb_register_hub(root_hub);
    XHCI_DIAG_PENDING(XHCI_DIAG_SCAN);
    usb_scan_all_hubs();
    XHCI_DIAG_OK(XHCI_DIAG_SCAN);

    XHCI_DIAG_OK(XHCI_DIAG_PROBE);
    return 0;
}

void xhci_start_workers(void) {
    usb_core_start_workers();

    for (xhci_controller *xhci = g_xhci; xhci; xhci = xhci->next) {
        if (xhci->service_worker_started) {
            continue;
        }

        size_t tid = create_kernel_thread((void *)xhci_service_worker, xhci,
                                          (char *)"xhci-svc", NULL);
        if ((int64_t)tid < 0) {
            continue;
        }

        xhci->service_worker_started = true;
        xhci->hid_worker_started = true;
        XHCI_DIAG_OK(XHCI_DIAG_WORKER);
        pr_info("xhci: service worker started tid=%u hid=%s\n",
                (unsigned int)tid, xhci_has_active_hid(xhci) ? "yes" : "no");
    }
}

int xhci_setup(void) {
    XHCI_DIAG_PENDING(XHCI_DIAG_SETUP);
    if (g_xhci) {
        XHCI_DIAG_OK(XHCI_DIAG_SETUP);
        return 0;
    }

    pci_device_t *xhci_devs[PCI_DEVICE_MAX];
    uint32_t xhci_count = 0;
    XHCI_DIAG_PENDING(XHCI_DIAG_PCI_SEARCH);
    pci_find_class_n(xhci_devs, &xhci_count, XHCI_PCI_CLASS);
    if (xhci_count == 0) {
        XHCI_DIAG_FAIL(XHCI_DIAG_PCI_SEARCH);
        XHCI_DIAG_FAIL(XHCI_DIAG_SETUP);
        pr_info("xhci: no xHCI controller found\n");
        return -ENODEV;
    }
    XHCI_DIAG_OK(XHCI_DIAG_PCI_SEARCH);

    int first_rc = -ENODEV;
    uint32_t ready_count = 0;
    for (uint32_t i = 0; i < xhci_count; ++i) {
        int rc = xhci_probe(xhci_devs[i]);
        if (rc == 0) {
            ready_count++;
        } else if (first_rc == -ENODEV) {
            first_rc = rc;
        }
    }

    if (ready_count != 0) {
        XHCI_DIAG_OK(XHCI_DIAG_SETUP);
        pr_info("xhci: initialized %u/%u controller(s)\n", ready_count, xhci_count);
        return 0;
    }
    XHCI_DIAG_FAIL(XHCI_DIAG_SETUP);
    return first_rc;
}

extern "C" __attribute__((used)) __attribute__((visibility("default"))) int
dlstart(void) {
    xhci_start_workers();
    return 0;
}

extern "C" __attribute__((used)) __attribute__((visibility("default"))) int
dlmain(void) {
    int rc = xhci_setup();
    if (rc == 0) {
        xhci_start_workers();
    }
    return rc;
}
