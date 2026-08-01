#include <graphics/GOP.hpp>
#include <graphics/sheet.h>
#include <fbdev.h>
#include <proto.hpp>
#include <cpu/lock.h>
bool SheetIsRefreshing = false;
bool SheetIsCreating   = false;
extern SHEET *mouse_ct_sheet_img;

typedef struct
{
    int x1;
    int y1;
    int x2;
    int y2;
} SheetDamageRect;

enum
{
    SHEET_DAMAGE_QUEUE_CAPACITY = 32,
    SHEET_DAMAGE_MERGE_GAP = 24,
    SHEET_DAMAGE_MERGE_MAX_WASTE_PERCENT = 45,
    SHEET_DAMAGE_LARGE_RECT_PERCENT = 70,
    SHEET_FRAME_INTERVAL_NS = 16666666ULL,
};

typedef struct
{
    SHEET_INFO      *sht;
    SheetDamageRect  rects[SHEET_DAMAGE_QUEUE_CAPACITY];
    uint8_t          count;
    bool             pending;
} SheetDamageQueueState;

static spin_t               g_sheet_damage_lock = SPIN_INIT;
static volatile long        g_sheet_flush_busy  = 0;
static spin_t               g_sheet_manager_lock = SPIN_INIT;
static SheetDamageQueueState g_sheet_damage_queue = {NULL, {}, 0, false};
static SheetDamageStats      g_sheet_damage_stats = {};
static uint64_t              g_sheet_next_frame_ns = 0;

void lock_sheet_manager()
{
    spin_lock(&g_sheet_manager_lock);
}

void unlock_sheet_manager()
{
    spin_unlock(&g_sheet_manager_lock);
}

static void keep_mouse_sheet_on_top_locked(SHEET_INFO *sht, SHEET *new_sheet)
{
    if (sht == NULL || mouse_ct_sheet_img == NULL || mouse_ct_sheet_img == new_sheet ||
        sht->start == NULL || mouse_ct_sheet_img->next == NULL)
    {
        return;
    }

    if (mouse_ct_sheet_img->front != NULL)
    {
        mouse_ct_sheet_img->front->next = mouse_ct_sheet_img->next;
    }
    else
    {
        sht->start = mouse_ct_sheet_img->next;
    }
    mouse_ct_sheet_img->next->front = mouse_ct_sheet_img->front;

    SHEET *tail = sht->start;
    while (tail != NULL && tail->next != NULL)
    {
        tail = tail->next;
    }
    if (tail == NULL)
    {
        sht->start = mouse_ct_sheet_img;
        mouse_ct_sheet_img->front = NULL;
        mouse_ct_sheet_img->next = NULL;
        return;
    }

    tail->next = mouse_ct_sheet_img;
    mouse_ct_sheet_img->front = tail;
    mouse_ct_sheet_img->next = NULL;
}

static bool sheet_flush_try_begin()
{
    unsigned char busy;
    asm volatile("lock btsq $0, %1\n\t"
                 "setc %0\n\t"
                 : "=q"(busy), "+m"(g_sheet_flush_busy)
                 :
                 : "memory", "cc");
    return !busy;
}

static void sheet_flush_end()
{
    asm volatile("lock btrq $0, %0\n\t"
                 : "+m"(g_sheet_flush_busy)
                 :
                 : "memory", "cc");
}

static inline int rect_min_int(int a, int b)
{
    return a < b ? a : b;
}

static inline int rect_max_int(int a, int b)
{
    return a > b ? a : b;
}

static inline void sheet_damage_rect_set(SheetDamageRect *rect, int x1, int y1, int x2, int y2)
{
    rect->x1 = x1;
    rect->y1 = y1;
    rect->x2 = x2;
    rect->y2 = y2;
}

static inline bool sheet_damage_rect_intersects_or_touches(const SheetDamageRect *a, const SheetDamageRect *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

static inline void sheet_damage_rect_union_into(SheetDamageRect *dst, const SheetDamageRect *src)
{
    dst->x1 = rect_min_int(dst->x1, src->x1);
    dst->y1 = rect_min_int(dst->y1, src->y1);
    dst->x2 = rect_max_int(dst->x2, src->x2);
    dst->y2 = rect_max_int(dst->y2, src->y2);
}

static inline uint64_t sheet_damage_rect_area(const SheetDamageRect *rect)
{
    return (uint64_t)(rect->x2 - rect->x1) * (uint64_t)(rect->y2 - rect->y1);
}

static inline bool sheet_damage_rect_empty(const SheetDamageRect *rect)
{
    return rect == NULL || rect->x1 >= rect->x2 || rect->y1 >= rect->y2;
}

static inline bool sheet_damage_rect_close_enough(const SheetDamageRect *a, const SheetDamageRect *b)
{
    return a->x1 <= b->x2 + SHEET_DAMAGE_MERGE_GAP && b->x1 <= a->x2 + SHEET_DAMAGE_MERGE_GAP &&
           a->y1 <= b->y2 + SHEET_DAMAGE_MERGE_GAP && b->y1 <= a->y2 + SHEET_DAMAGE_MERGE_GAP;
}

static inline bool sheet_damage_should_merge(const SheetDamageRect *a, const SheetDamageRect *b)
{
    if (sheet_damage_rect_intersects_or_touches(a, b)) return true;
    if (!sheet_damage_rect_close_enough(a, b)) return false;

    SheetDamageRect merged = *a;
    sheet_damage_rect_union_into(&merged, b);

    uint64_t area_a = sheet_damage_rect_area(a);
    uint64_t area_b = sheet_damage_rect_area(b);
    uint64_t merged_area = sheet_damage_rect_area(&merged);
    uint64_t source_area = area_a + area_b;
    if (merged_area <= source_area) return true;

    uint64_t waste = merged_area - source_area;
    return waste * 100 <= source_area * SHEET_DAMAGE_MERGE_MAX_WASTE_PERCENT;
}

static void sheet_damage_queue_compact_locked(SheetDamageQueueState *queue)
{
    for (uint8_t i = 0; i < queue->count; ++i)
    {
        for (uint8_t j = i + 1; j < queue->count;)
        {
            if (!sheet_damage_should_merge(&queue->rects[i], &queue->rects[j]))
            {
                ++j;
                continue;
            }

            sheet_damage_rect_union_into(&queue->rects[i], &queue->rects[j]);
            g_sheet_damage_stats.merged_rects++;
            queue->rects[j] = queue->rects[queue->count - 1];
            queue->count--;
        }
    }

    queue->pending = queue->count != 0;
    if (!queue->pending) {
        queue->sht = NULL;
    }
}

static void sheet_damage_queue_push_locked(SheetDamageQueueState *queue, SHEET_INFO *sht, int x1, int y1, int x2, int y2)
{
    if (queue->pending && queue->sht != sht)
    {
        // Current callers use a single compositor. If another sheet manager
        // starts sharing this queue, keep correctness by falling back to
        // immediate coalescing on the existing owner first.
        SheetDamageRect merged = queue->rects[0];
        for (uint8_t i = 1; i < queue->count; ++i)
        {
            sheet_damage_rect_union_into(&merged, &queue->rects[i]);
        }
        queue->rects[0] = merged;
        queue->count    = queue->count ? 1 : 0;
        queue->pending  = queue->count != 0;
    }

    queue->sht = sht;
    g_sheet_damage_stats.queued_rects++;

    SheetDamageRect rect;
    sheet_damage_rect_set(&rect, x1, y1, x2, y2);

    uint64_t screen_area = (uint64_t)sht->scrx * (uint64_t)sht->scry;
    if (screen_area != 0 && sheet_damage_rect_area(&rect) * 100 >= screen_area * SHEET_DAMAGE_LARGE_RECT_PERCENT)
    {
        sheet_damage_rect_set(&rect, 0, 0, (int)sht->scrx, (int)sht->scry);
        queue->count = 0;
    }

    for (uint8_t i = 0; i < queue->count;)
    {
        if (!sheet_damage_should_merge(&queue->rects[i], &rect))
        {
            ++i;
            continue;
        }

        sheet_damage_rect_union_into(&rect, &queue->rects[i]);
        g_sheet_damage_stats.merged_rects++;
        queue->rects[i] = queue->rects[queue->count - 1];
        queue->count--;
    }

    if (queue->count < SHEET_DAMAGE_QUEUE_CAPACITY)
    {
        queue->rects[queue->count++] = rect;
        queue->pending = true;
        if (queue->count > g_sheet_damage_stats.max_queue_depth) g_sheet_damage_stats.max_queue_depth = queue->count;
        return;
    }

    uint8_t  best_index  = 0;
    uint64_t best_growth = (uint64_t)-1;
    uint64_t best_area = (uint64_t)-1;
    for (uint8_t i = 0; i < queue->count; ++i)
    {
        SheetDamageRect merged = queue->rects[i];
        sheet_damage_rect_union_into(&merged, &rect);
        uint64_t growth = sheet_damage_rect_area(&merged) - sheet_damage_rect_area(&queue->rects[i]);
        uint64_t area = sheet_damage_rect_area(&merged);
        if (growth < best_growth || (growth == best_growth && area < best_area))
        {
            best_growth = growth;
            best_area = area;
            best_index  = i;
        }
    }

    sheet_damage_rect_union_into(&queue->rects[best_index], &rect);
    g_sheet_damage_stats.merged_rects++;
    sheet_damage_queue_compact_locked(queue);
    if (queue->count > g_sheet_damage_stats.max_queue_depth) g_sheet_damage_stats.max_queue_depth = queue->count;
}

static size_t sheet_buffer_bytes(uint32_t width, uint32_t height)
{
    return (size_t)width * (size_t)height * sizeof(SHEET_BUFFER);
}

static void *alloc_shared_sheet_buffer(size_t bytes)
{
    // Sheet buffers are shared by the desktop flusher and syscall paths across
    // different CR3 contexts, so they must live in the shared kernel heap.
    void *buffer = calloc(1, bytes);
    if (buffer != NULL) return buffer;

    write_serial_string("Cannot allocate memory for shared sheet buffer. Requested bytes: ");
    write_serial_dec(bytes);
    write_serial_string("\n");
    return NULL;
}

static bool is_valid_sheet_buffer(const void *buffer)
{
    return buffer != NULL && (uint64_t)buffer >= get_physical_memory_offset();
}

static bool clip_sheet_copy_region(const SHEET *dst_sheet, int *dst_x, int *dst_y,
                                   int *src_x, int *src_y, int *width, int *height)
{
    if (!dst_sheet || !is_valid_sheet_buffer(dst_sheet->buffer)) return false;

    int dx = *dst_x;
    int dy = *dst_y;
    int sx = *src_x;
    int sy = *src_y;
    int w  = *width;
    int h  = *height;

    if (w <= 0 || h <= 0) return false;

    if (dx < 0)
    {
        sx += -dx;
        w += dx;
        dx = 0;
    }
    if (dy < 0)
    {
        sy += -dy;
        h += dy;
        dy = 0;
    }

    if (dx >= (int)dst_sheet->width || dy >= (int)dst_sheet->height) return false;

    if (dx + w > (int)dst_sheet->width) w = (int)dst_sheet->width - dx;
    if (dy + h > (int)dst_sheet->height) h = (int)dst_sheet->height - dy;

    if (sx < 0 || sy < 0 || w <= 0 || h <= 0) return false;

    *dst_x  = dx;
    *dst_y  = dy;
    *src_x  = sx;
    *src_y  = sy;
    *width  = w;
    *height = h;
    return true;
}

static inline bool is_point_in_sheet_rect(const SHEET *sheet, int x, int y)
{
    if (sheet == NULL) return false;
    return sheet->bx <= x && sheet->bx + (int)sheet->width >= x && sheet->by <= y &&
           sheet->by + (int)sheet->height >= y;
}

static inline bool is_sheet_pixel_opaque_at(const SHEET *sheet, int x, int y)
{
    if (sheet == NULL || !is_valid_sheet_buffer(sheet->buffer)) return false;
    int lx = x - sheet->bx;
    int ly = y - sheet->by;
    if (lx < 0 || ly < 0 || lx >= (int)sheet->width || ly >= (int)sheet->height) return false;
    SHEET_BUFFER *buf = (SHEET_BUFFER *)sheet->buffer;
    return buf[ly * (int)sheet->width + lx].a != 0;
}

void init_foxland(SHEET_INFO *sht)
{
    sht->foxland.enabled        = false;
    sht->foxland.fl_peek_buffer = (SHEET_BUFFER *)calloc((size_t)sht->scrx * sht->scry, sizeof(SHEET_BUFFER));//不要乱用pagealloc
    sht->foxland.peek_sign      = (uint8_t *)calloc(sht->scrx * sht->scry, sizeof(uint8_t));
    sht->foxland.opaque_layer   = (uint16_t *)calloc(sht->scrx * sht->scry, sizeof(uint16_t));
    if (sht->foxland.fl_peek_buffer == NULL || sht->foxland.peek_sign == NULL || sht->foxland.opaque_layer == NULL)
    {
        write_serial_string("Foxland Peek disabled: allocation failed.\n");
        return;
    }
    sht->foxland.enabled =
        LoadPicture((SHEET_BUFFER *)sht->foxland.fl_peek_buffer, sht->scrx, sht->scry, "/system/foxland/peek.png");
    if (!sht->foxland.enabled) write_serial_string("Foxland Peek disabled: /system/foxland/peek.png unavailable.\n");
}
void init_sheet(const FrameBufferConfig &fbc, SHEET_INFO *sht)
{
    write_serial_string("Initializing Sheet Manager...\n");
    sht->fbc         = &fbc;
    sht->scrx        = fbc.horizontal_resolution;
    sht->scry        = fbc.vertical_resolution;
    sht->start       = NULL;
    size_t temp_buffer_bytes = sheet_buffer_bytes(sht->scrx, sht->scry);
    sht->temp_buffer = alloc_shared_sheet_buffer(temp_buffer_bytes);
    if (sht->temp_buffer == NULL)
    {
        write_serial_string("Sheet Manager critical buffer allocation failed. System halted.\n");
        while (1)
        {
            __asm__ __volatile__("hlt");
        }
    }
    sht->sheet_num            = -1;

    init_foxland(sht);

    write_serial_string("Sheet Manager Initialize Success.\n");
}

bool create_sheet(SHEET_INFO *sht, uint32_t bx, uint32_t by, uint32_t x, uint32_t y, uint32_t type, int16_t index,
                  SHEET **sheet)
{
    if (sheet != NULL) *sheet = NULL;
    if (sht == NULL || sheet == NULL || x == 0 || y == 0) return false;

    SHEET *new_sheet = (SHEET *)(malloc(sizeof(SHEET))); // 开辟内存空间
    if (new_sheet == NULL)
    {
        return false;
    }
    memset(new_sheet, 0, sizeof(SHEET));
    size_t buffer_bytes = sheet_buffer_bytes(x, y);
    new_sheet->buffer = alloc_shared_sheet_buffer(buffer_bytes);
    if (new_sheet->buffer == NULL)
    {
        free(new_sheet);
        return false;
    }

    new_sheet->bx        = bx;
    new_sheet->by        = by;
    new_sheet->width     = x;
    new_sheet->height    = y;
    new_sheet->type      = type;
    new_sheet->is_change = true;

    lock_sheet_manager();
    SheetIsCreating = true;

    SHEET *front_p = sht->start;
    if (index > 0)
    {
        // 指定图层高度
        if (front_p != NULL)
        {
            SHEET *record = front_p;
            int count = -1;
            while (true)
            {
                count++;
                if (front_p->next == NULL) { break; }
                else if (count + 1 >= index) { break; }
                else if (record == front_p->next || front_p == front_p->next)
                {
                    front_p->next = NULL;
                    break;
                }
                front_p = front_p->next;
            }

            if (front_p->next != NULL)
            {
                // 后继有人，给他踢出去换成自己
                new_sheet->next      = front_p->next;   // 把自己的下一个标记为曾经的
                new_sheet->front     = front_p;         // 把自己的上一个标记为上一个
                front_p->next->front = new_sheet;       // 把曾经的上一个标记为自己
                front_p->next        = new_sheet;       // 把自己的上一个的下一个标记为自己
                front_p              = new_sheet->next; // 指针指向自己的下一个（曾经的）
                record               = front_p;
                // // 给后面的全++
                // while (true)
                // {
                //     front_p->index++;
                //     if (front_p->next == NULL) { break; }
                //     else if (record == front_p->next || front_p == front_p->next)
                //     {
                //         front_p->next = NULL;
                //         break;
                //     }
                //     front_p = front_p->next;
                // }

                // 分配
                *sheet = new_sheet;
                sht->sheet_num++;
            }
            else
            {
                // 哦亲爱的孩子你是第一个分配的
                new_sheet->front = front_p;
                front_p->next    = new_sheet;

                // 分配
                *sheet = new_sheet;
                sht->sheet_num++;
            }
        }
        else
        {
            sht->start = new_sheet;

            *sheet = new_sheet;
            sht->sheet_num++;
        }
    }
    else
    {
        // 随便分配
        // 直接分配下一个
        if (front_p != NULL)
        {
            for (int i = 0; i < sht->sheet_num - 4; i++)
            {
                front_p = front_p->next;
            }

            if (front_p->next == NULL)
            {
                front_p->next    = new_sheet;
                new_sheet->front = front_p;
            }
            else
            {
                new_sheet->next      = front_p->next;
                front_p->next->front = new_sheet;
                front_p->next        = new_sheet;
                new_sheet->front     = front_p;
            }

            *sheet = new_sheet;
            sht->sheet_num++;
        }
        else
        {
            sht->start = new_sheet;

            *sheet = new_sheet;
            sht->sheet_num++;
        }
    }
    // write_serial_fmt("Sheet Create Success. SheetID: %d\n", *ct_sheet);
    keep_mouse_sheet_on_top_locked(sht, new_sheet);
    SheetIsCreating = false;
    unlock_sheet_manager();
    return *sheet != NULL;
}

uint32_t getBX(SHEET_INFO *sht, SHEET *csheet)
{
    if (!csheet) return NULL;
    return csheet->bx;
}

uint32_t getBY(SHEET_INFO *sht, SHEET *csheet)
{
    if (!csheet) return NULL;
    return csheet->by;
}

uint32_t getXsize(SHEET_INFO *sht, SHEET *csheet)
{
    if (!csheet) return NULL;
    return csheet->width;
}

uint32_t getYsize(SHEET_INFO *sht, SHEET *csheet)
{
    if (!csheet) return NULL;
    return csheet->height;
}

SHEET *found_sheet(SHEET_INFO *sht, uint32_t x, uint32_t y)
{
    if (sht == NULL || sht->start == NULL) {
        return NULL;
    }

    SHEET *front_p = sht->start;
    while (true)
    {
        if (front_p->next == NULL)
        {
            while (true)
            {
                if (front_p->bx <= x && front_p->bx + front_p->width >= x && front_p->by <= y &&
                    front_p->by + front_p->height >= y)
                {
                    return front_p;
                }
                else if (front_p->front == NULL) { return NULL; }
                front_p = front_p->front;
            }
            return NULL;
        }
        front_p = front_p->next;
    }
}

SHEET *found_sheetmb(SHEET_INFO *sht, uint32_t x, uint32_t y)
{
    if (sht == NULL || sht->start == NULL) {
        return NULL;
    }

    SHEET *front_p = sht->start;
    while (front_p->next != NULL) {
        front_p = front_p->next;
    }

    while (front_p != NULL)
    {
        if (!is_point_in_sheet_rect(front_p, (int)x, (int)y)) {
            front_p = front_p->front;
            continue;
        }
        if (front_p == mouse_ct_sheet_img) {
            front_p = front_p->front;
            continue;
        }
        if (!is_sheet_pixel_opaque_at(front_p, (int)x, (int)y)) {
            front_p = front_p->front;
            continue;
        }

        if (front_p->type == MovableSheetType || front_p->type == NoEdgeWindowSheetType ||
            front_p->type == TopWindowSheetType) {
            return front_p;
        }
        // 顶层非窗口图层（例如底栏）已经遮挡住该点，不应继续穿透到底下窗口。
        return NULL;
    }

    return NULL;
}

SHEET *found_sheet_movable(SHEET_INFO *sht, uint32_t x, uint32_t y)
{
    if (sht == NULL || sht->start == NULL) {
        return NULL;
    }

    int px = (int)x;
    int py = (int)y;
    SHEET *front_p = sht->start;
    while (front_p->next != NULL) {
        front_p = front_p->next;
    }

    while (front_p != NULL)
    {
        if (!is_point_in_sheet_rect(front_p, px, py)) {
            front_p = front_p->front;
            continue;
        }
        if (front_p == mouse_ct_sheet_img) {
            front_p = front_p->front;
            continue;
        }
        if (!is_sheet_pixel_opaque_at(front_p, px, py)) {
            front_p = front_p->front;
            continue;
        }

        if (front_p->type == MovableSheetType)
        {
            if (front_p->bx + 22 <= px && front_p->bx + (int)front_p->width - 32 >= px &&
                front_p->by <= py && front_p->by + 22 >= py)
            {
                return front_p;
            }
            return NULL;
        }
        if (front_p->type == NoEdgeWindowSheetType)
        {
            return front_p;
        }
        if (front_p->type == TopWindowSheetType) {
            return NULL;
        }

        // 被其他可见图层（比如底栏）挡住时，不允许拖动其下层窗口。
        return NULL;
    }

    return NULL;
}

void change_sheet_type(SHEET_INFO *sht, SHEET *csheet, uint32_t new_type)
{
    if (!csheet) return;
    csheet->type = new_type;
}

SHEET *get_sheet(SHEET_INFO *sht, SHEET *csheet)
{
    return csheet;
}

bool sheet_contains(SHEET_INFO *sht, SHEET *csheet)
{
    if (sht == NULL || csheet == NULL) return false;

    SHEET *front_p = sht->start;
    while (front_p != NULL)
    {
        if (front_p == csheet) return true;
        front_p = front_p->next;
    }
    return false;
}

void setBX(SHEET_INFO *sht, SHEET *csheet, uint32_t num)
{
    if (!csheet) return;
    csheet->bx = num;
}

void setBY(SHEET_INFO *sht, SHEET *csheet, uint32_t num)
{
    if (!csheet) return;
    csheet->by = num;
}

void delete_sheet(SHEET_INFO *sht, SHEET *csheet)
{
    if (!csheet) return;
    lock_sheet_manager();
    SheetIsCreating = true;
    
    if (csheet->front == NULL) {
        sht->start = csheet->next;
        if (csheet->next) {
            csheet->next->front = NULL;
        }
    } else if (csheet->next == NULL) {
        csheet->front->next = NULL;
    } else {
        csheet->front->next = csheet->next;
        csheet->next->front = csheet->front;
    }
    
    // 释放内存
    free(csheet->buffer);
    memset(csheet, 0, sizeof(SHEET));
    free((void *)(csheet));
    sht->sheet_num--;

    SheetIsCreating = false;
    unlock_sheet_manager();
}

static bool is_raiseable_window_sheet(const SHEET *sheet)
{
    return sheet != NULL && (sheet->type == MovableSheetType || sheet->type == NoEdgeWindowSheetType);
}

void lift_sheet(SHEET_INFO *sht, SHEET *csheet)
{
    if (sht == NULL || csheet == NULL) return;
    lock_sheet_manager();
    SheetIsCreating = true;
    if (!sheet_contains(sht, csheet) || csheet->front == NULL || !is_raiseable_window_sheet(csheet))
    {
        SheetIsCreating = false;
        unlock_sheet_manager();
        return;
    }

    SHEET *target = sht->start;
    for (SHEET *front_p = sht->start; front_p != NULL; front_p = front_p->next)
    {
        if (front_p != csheet && is_raiseable_window_sheet(front_p)) target = front_p;
    }

    if (target == NULL || target == csheet || target->next == csheet)
    {
        SheetIsCreating = false;
        unlock_sheet_manager();
        return;
    }

    csheet->front->next = csheet->next;
    if (csheet->next != NULL) csheet->next->front = csheet->front;

    csheet->next = target->next;
    if (csheet->next != NULL) csheet->next->front = csheet;
    target->next  = csheet;
    csheet->front = target;

    SheetIsCreating = false;
    unlock_sheet_manager();
}

bool allow_to_flush = false;

void get_sheet_damage_stats(SheetDamageStats *stats)
{
    if (stats == NULL) return;
    spin_lock(&g_sheet_damage_lock);
    *stats = g_sheet_damage_stats;
    spin_unlock(&g_sheet_damage_lock);
}

static bool clip_refresh_region(SHEET_INFO *sht, int *bex1, int *bey1, int *bex2, int *bey2)
{
    if (sht == NULL) return false;
    if (*bex1 < 0) *bex1 = 0;
    if (*bey1 < 0) *bey1 = 0;
    if (*bex2 > (int)sht->scrx) *bex2 = sht->scrx;
    if (*bey2 > (int)sht->scry) *bey2 = sht->scry;
    if (*bex1 >= *bex2 || *bey1 >= *bey2) return false;
    return true;
}

static void refresh_part_sheet_compose(SHEET_INFO *sht, int bex1, int bey1, int bex2, int bey2)
{
    uint64_t flush_start_ns = nanoTime();
    lock_sheet_manager();
    SheetIsRefreshing = true;

    SHEET_BUFFER *temp_buf_base = (SHEET_BUFFER *)sht->temp_buffer;
    uint8_t *temp_peek_sign = sht->foxland.peek_sign;
    bool foxland_enabled = sht->foxland.enabled && temp_peek_sign != NULL && sht->foxland.fl_peek_buffer != NULL;
    bool need_peek_overlay = false;
    int refresh_width = bex2 - bex1;
    size_t refresh_row_bytes = (size_t)refresh_width * sizeof(SHEET_BUFFER);

    for (int y = bey1; y < bey2; y++) {
        int offset = y * sht->scrx + bex1;
        memset(&temp_buf_base[offset], 0, refresh_row_bytes);
        if (foxland_enabled) {
            memset(&temp_peek_sign[offset], 0, (size_t)refresh_width);
        }
    }

    for (SHEET *st = sht->start; st != nullptr; st = st->next) {
        int vx0 = (bex1 > (int)st->bx) ? bex1 : (int)st->bx;
        int vy0 = (bey1 > (int)st->by) ? bey1 : (int)st->by;
        int vx1 = (bex2 < (int)(st->bx + st->width)) ? bex2 : (int)(st->bx + st->width);
        int vy1 = (bey2 < (int)(st->by + st->height)) ? bey2 : (int)(st->by + st->height);

        if (vx0 >= vx1 || vy0 >= vy1 || !is_valid_sheet_buffer(st->buffer)) {
            continue;
        }

        SHEET_BUFFER *st_buf = (SHEET_BUFFER *)st->buffer;
        int width = vx1 - vx0;
        
        for (int y = vy0; y < vy1; y++) {
            int screen_off = y * sht->scrx + vx0;
            int st_off = (y - st->by) * st->width + (vx0 - st->bx);
            
            SHEET_BUFFER *p_dst = &temp_buf_base[screen_off];
            SHEET_BUFFER *p_src = &st_buf[st_off];
            uint8_t *p_sign = foxland_enabled ? &temp_peek_sign[screen_off] : NULL;

            for (int x = 0; x < width; ) {
                uint8_t alpha = p_src[x].a;
                if (alpha == 0) {
                    int run = 1;
                    while (x + run < width && p_src[x + run].a == 0) {
                        run++;
                    }
                    x += run;
                    continue;
                }

                if (alpha == 255) {
                    int run = 1;
                    while (x + run < width && p_src[x + run].a == 255) {
                        run++;
                    }
                    memcpy(&p_dst[x], &p_src[x], (size_t)run * sizeof(SHEET_BUFFER));
                    if (foxland_enabled) {
                        memset(&p_sign[x], 0, (size_t)run);
                    }
                    x += run;
                    continue;
                }

                {
                    uint32_t inv_alpha = 255 - alpha;

                    p_dst[x].r = (p_src[x].r * alpha + p_dst[x].r * inv_alpha) >> 8;
                    p_dst[x].g = (p_src[x].g * alpha + p_dst[x].g * inv_alpha) >> 8;
                    p_dst[x].b = (p_src[x].b * alpha + p_dst[x].b * inv_alpha) >> 8;

                    if (foxland_enabled && st->type != FixedSheetType)
                    {
                        p_sign[x] = 1;
                        need_peek_overlay = true;
                    }
                }
                x++;
            }
        }
    }

    if (need_peek_overlay && foxland_enabled) {
        SHEET_BUFFER *temp_peek = (SHEET_BUFFER *)sht->foxland.fl_peek_buffer;
        int peek_width = bex2 - bex1;

        for (int y = bey1; y < bey2; y++) {
            int offset = y * sht->scrx + bex1;
            SHEET_BUFFER *p_dst = &temp_buf_base[offset];
            uint8_t *p_sign = &temp_peek_sign[offset];
            SHEET_BUFFER *p_peek = &temp_peek[offset];

            for (int x = 0; x < peek_width; x++) {
                if (p_sign[x] == 1) {
                    uint32_t alpha = p_peek[x].a;
                    uint32_t inv_alpha = 255 - alpha;
                    p_dst[x].r = (p_peek[x].r * alpha + p_dst[x].r * inv_alpha) >> 8;
                    p_dst[x].g = (p_peek[x].g * alpha + p_dst[x].g * inv_alpha) >> 8;
                    p_dst[x].b = (p_peek[x].b * alpha + p_dst[x].b * inv_alpha) >> 8;
                    p_sign[x] = 0;
                }
            }
        }
    }

    // 写入后备 framebuffer，帧调度器统一 present 到 GOP。
    uint8_t *frame_buffer = fbdev_ready() ? fbdev_kernel_framebuffer() : (uint8_t *)sht->fbc->frame_buffer;
    uint32_t pixels_per_scan_line = sht->fbc->pixels_per_scan_line;
    size_t row_bytes = (size_t)(bex2 - bex1) * sizeof(SHEET_BUFFER);
    
    if (sht->fbc->pixel_format == PixelFormat::kRGBR) {
        for (int y = bey1; y < bey2; y++) {
            SHEET_BUFFER *src_line = &temp_buf_base[y * sht->scrx + bex1];
            uint8_t *dst_line = frame_buffer + (y * pixels_per_scan_line + bex1) * 4;

            memcpy(dst_line, src_line, row_bytes);
        }
    } else {
        for (int y = bey1; y < bey2; y++) {
            SHEET_BUFFER *src_line = &temp_buf_base[y * sht->scrx + bex1];
            uint8_t *dst_line = frame_buffer + (y * pixels_per_scan_line + bex1) * 4;
            
            // 转换并复制整行
            for (int x = 0; x < (bex2 - bex1); x++) {
                dst_line[x * 4 + 0] = src_line[x].b;
                dst_line[x * 4 + 1] = src_line[x].g;
                dst_line[x * 4 + 2] = src_line[x].r;
                dst_line[x * 4 + 3] = 0;
            }
        }
    }

    SheetIsRefreshing = false;
    unlock_sheet_manager();

    uint64_t elapsed_ns = nanoTime() - flush_start_ns;
    spin_lock(&g_sheet_damage_lock);
    g_sheet_damage_stats.flushed_rects++;
    g_sheet_damage_stats.flushed_pixels +=
        (uint64_t)(bex2 - bex1) * (uint64_t)(bey2 - bey1);
    g_sheet_damage_stats.last_flush_ns = elapsed_ns;
    if (elapsed_ns > g_sheet_damage_stats.max_flush_ns) g_sheet_damage_stats.max_flush_ns = elapsed_ns;
    spin_unlock(&g_sheet_damage_lock);
}

void refresh_part_sheet(SHEET_INFO *sht, int bex1, int bey1, int bex2, int bey2)
{
    if (!allow_to_flush) return;
    if (!clip_refresh_region(sht, &bex1, &bey1, &bex2, &bey2)) return;

    spin_lock(&g_sheet_damage_lock);
    sheet_damage_queue_push_locked(&g_sheet_damage_queue, sht, bex1, bey1, bex2, bey2);
    spin_unlock(&g_sheet_damage_lock);
}

void refresh_sheet(SHEET_INFO *sht)
{
    refresh_part_sheet(sht, 0, 0, sht->scrx, sht->scry);
}

static int sheet_damage_queue_take_frame_locked(SHEET_INFO *sht, SheetDamageRect *rects, int max_count)
{
    if (rects == NULL || max_count <= 0 || !g_sheet_damage_queue.pending ||
        g_sheet_damage_queue.sht != sht || g_sheet_damage_queue.count == 0)
    {
        return 0;
    }

    int count = g_sheet_damage_queue.count;
    if (count > max_count) count = max_count;
    for (int i = 0; i < count; ++i)
    {
        rects[i] = g_sheet_damage_queue.rects[i];
    }

    g_sheet_damage_queue.count = 0;
    g_sheet_damage_queue.pending = false;
    g_sheet_damage_queue.sht = NULL;
    return count;
}

static bool sheet_damage_frame_union(SheetDamageRect *rects, int count, SheetDamageRect *frame)
{
    if (rects == NULL || count <= 0 || frame == NULL) return false;

    *frame = rects[0];
    for (int i = 1; i < count; ++i)
    {
        sheet_damage_rect_union_into(frame, &rects[i]);
    }
    return !sheet_damage_rect_empty(frame);
}

static void sheet_present_frame(SHEET_INFO *sht, const SheetDamageRect *frame)
{
    if (sht == NULL || sheet_damage_rect_empty(frame)) return;

    if (fbdev_ready()) {
        fbdev_present_kernel_region((uint32_t)frame->x1, (uint32_t)frame->y1,
                                    (uint32_t)frame->x2, (uint32_t)frame->y2);
    }
}

static void flush_sheet_damage_queue_frame(SHEET_INFO *sht, bool force)
{
    if (!allow_to_flush || sht == NULL) return;
    if (!sheet_flush_try_begin()) return;

    uint64_t now = nanoTime();
    if (!force && g_sheet_next_frame_ns != 0 && now < g_sheet_next_frame_ns)
    {
        sheet_flush_end();
        return;
    }
    g_sheet_next_frame_ns = now + SHEET_FRAME_INTERVAL_NS;

    SheetDamageRect frame_rects[SHEET_DAMAGE_QUEUE_CAPACITY];
    spin_lock(&g_sheet_damage_lock);
    int frame_rect_count =
        sheet_damage_queue_take_frame_locked(sht, frame_rects, SHEET_DAMAGE_QUEUE_CAPACITY);
    spin_unlock(&g_sheet_damage_lock);

    SheetDamageRect frame;
    if (sheet_damage_frame_union(frame_rects, frame_rect_count, &frame))
    {
        refresh_part_sheet_compose(sht, frame.x1, frame.y1, frame.x2, frame.y2);
        sheet_present_frame(sht, &frame);
    }
    sheet_flush_end();
}

void flush_sheet_damage_queue(SHEET_INFO *sht)
{
    flush_sheet_damage_queue_frame(sht, false);
}

void flush_sheet_damage_queue_now(SHEET_INFO *sht)
{
    flush_sheet_damage_queue_frame(sht, true);
}

uint32_t get_hor()
{
    return fbc_addr->horizontal_resolution;
}

uint32_t get_ver()
{
    return fbc_addr->vertical_resolution;
}

void copy_buffer_by_id(SHEET_INFO *sht, SHEET *dst_sheet, SHEET_BUFFER *src, uint32_t dst_x, uint32_t dst_y,
                       uint32_t width, uint32_t height)
{
    if (!dst_sheet || src == NULL) return;

    int dst_x_i = (int)dst_x;
    int dst_y_i = (int)dst_y;
    int src_x_i = 0;
    int src_y_i = 0;
    int width_i = (int)width;
    int height_i = (int)height;
    int src_stride = (int)width;
    if (!clip_sheet_copy_region(dst_sheet, &dst_x_i, &dst_y_i, &src_x_i, &src_y_i, &width_i, &height_i)) return;

    SHEET_BUFFER *dst = (SHEET_BUFFER *)dst_sheet->buffer;
    for (int y = 0; y < height_i; y++)
    {
        SHEET_BUFFER *dst_row = &dst[(dst_y_i + y) * dst_sheet->width + dst_x_i];
        SHEET_BUFFER *src_row = &src[(src_y_i + y) * src_stride + src_x_i];
        memcpy(dst_row, src_row, (size_t)width_i * sizeof(SHEET_BUFFER));
    }
}

void copy_buffer_by_id_without_alpha(SHEET_INFO *sht, SHEET *dst_sheet, SHEET_BUFFER *src, uint32_t dst_x,
                                     uint32_t dst_y, uint32_t width, uint32_t height)
{
    if (!dst_sheet || src == NULL) return;

    int dst_x_i = (int)dst_x;
    int dst_y_i = (int)dst_y;
    int src_x_i = 0;
    int src_y_i = 0;
    int width_i = (int)width;
    int height_i = (int)height;
    int src_stride = (int)width;
    if (!clip_sheet_copy_region(dst_sheet, &dst_x_i, &dst_y_i, &src_x_i, &src_y_i, &width_i, &height_i)) return;

    SHEET_BUFFER *dst = (SHEET_BUFFER *)dst_sheet->buffer;
    for (int y = 0; y < height_i; y++)
    {
        SHEET_BUFFER *dst_row = &dst[(dst_y_i + y) * dst_sheet->width + dst_x_i];
        SHEET_BUFFER *src_row = &src[(src_y_i + y) * src_stride + src_x_i];
        for (int x = 0; x < width_i; x++)
        {
            SHEET_BUFFER pixel = src_row[x];
            if (pixel.a > 10) dst_row[x] = pixel;
        }
    }
}

SHEET_BUFFER LCD_AlphaBlend(SHEET_BUFFER foreground_color, SHEET_BUFFER background_color, uint8_t alpha);

void copy_buffer_blend_by_id(SHEET_INFO *sht, SHEET *dst_sheet, SHEET_BUFFER *src, uint32_t dst_x, uint32_t dst_y,
                             uint32_t width, uint32_t height)
{
    if (!dst_sheet || src == NULL) return;

    int dst_x_i = (int)dst_x;
    int dst_y_i = (int)dst_y;
    int src_x_i = 0;
    int src_y_i = 0;
    int width_i = (int)width;
    int height_i = (int)height;
    int src_stride = (int)width;
    if (!clip_sheet_copy_region(dst_sheet, &dst_x_i, &dst_y_i, &src_x_i, &src_y_i, &width_i, &height_i)) return;

    SHEET_BUFFER *dst = (SHEET_BUFFER *)dst_sheet->buffer;
    for (int y = 0; y < height_i; y++)
    {
        SHEET_BUFFER *dst_row = &dst[(dst_y_i + y) * dst_sheet->width + dst_x_i];
        SHEET_BUFFER *src_row = &src[(src_y_i + y) * src_stride + src_x_i];
        for (int x = 0; x < width_i; x++)
        {
            SHEET_BUFFER pixel = src_row[x];
            if (pixel.a == 0) continue;
            if (pixel.a == 255)
            {
                dst_row[x] = pixel;
                continue;
            }
            dst_row[x] = LCD_AlphaBlend(pixel, dst_row[x], pixel.a);
        }
    }
}

void *get_sheet_buffer(SHEET_INFO *sht, SHEET *csheet)
{
    if (!csheet) return NULL;
    return csheet->buffer;
}

SHEET *found_sheet_byid(SHEET_INFO *sht, SHEET *csheet)
{
    return sheet_contains(sht, csheet) ? csheet : NULL;
}
