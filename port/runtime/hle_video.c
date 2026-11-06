/* Event queues and video out on Windows (behaviour from shadPS4's equeue.cpp, video_out.cpp and
 * driver.cpp, GPL-2.0-or-later). There is no display yet: a flip completes at once, and a timer
 * thread makes the 59.94 Hz vblanks. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "hle.h"

/* ---- event queues ---- */

typedef struct { uint64_t ident; int16_t filter; uint16_t flags; uint32_t fflags; uint64_t data; void *udata; } GuestEvent;

enum { QUEUE_EVENTS = 64 };
typedef struct {
    SRWLOCK lock;
    CONDITION_VARIABLE changed;
    struct { GuestEvent event; int triggered, used; } events[QUEUE_EVENTS];
    char name[32];
    int deleted;
} EventQueue;

enum { QUEUES = 256 };
static EventQueue *queues[QUEUES];
static SRWLOCK queues_lock = SRWLOCK_INIT;

static ABI int32_t equeue_create(EventQueue **out, const char *name) {
    if (!out) return ORBIS_ERR(E_INVAL);
    EventQueue *q = rt_low_alloc(sizeof(EventQueue));
    InitializeSRWLock(&q->lock);
    InitializeConditionVariable(&q->changed);
    snprintf(q->name, sizeof(q->name), "%s", name ? name : "");
    AcquireSRWLockExclusive(&queues_lock);
    int placed = 0;
    for (int i = 0; i < QUEUES && !placed; ++i)
        if (!queues[i]) { queues[i] = q; placed = 1; }
    ReleaseSRWLockExclusive(&queues_lock);
    if (!placed) return ORBIS_ERR(E_MFILE);
    *out = q;
    return 0;
}

static ABI int32_t equeue_delete(EventQueue *q) {
    if (!q) return ORBIS_ERR(E_BADF);
    AcquireSRWLockExclusive(&queues_lock);
    for (int i = 0; i < QUEUES; ++i)
        if (queues[i] == q) queues[i] = NULL;
    ReleaseSRWLockExclusive(&queues_lock);
    q->deleted = 1;
    WakeAllConditionVariable(&q->changed);
    return 0;
}

int32_t rt_equeue_add(void *queue, uint64_t ident, int16_t filter, void *udata) {
    EventQueue *q = queue;
    if (!q) return ORBIS_ERR(E_BADF);
    AcquireSRWLockExclusive(&q->lock);
    int done = 0;
    for (int i = 0; i < QUEUE_EVENTS && !done; ++i)
        if (q->events[i].used && q->events[i].event.ident == ident && q->events[i].event.filter == filter) {
            q->events[i].event.udata = udata;
            done = 1;
        }
    for (int i = 0; i < QUEUE_EVENTS && !done; ++i)
        if (!q->events[i].used) {
            memset(&q->events[i], 0, sizeof(q->events[i]));
            q->events[i].used = 1;
            q->events[i].event.ident = ident;
            q->events[i].event.filter = filter;
            q->events[i].event.flags = 1;
            q->events[i].event.udata = udata;
            done = 1;
        }
    ReleaseSRWLockExclusive(&q->lock);
    return done ? 0 : ORBIS_ERR(E_NOMEM);
}

int32_t rt_equeue_remove(void *queue, uint64_t ident, int16_t filter) {
    EventQueue *q = queue;
    if (!q) return ORBIS_ERR(E_BADF);
    AcquireSRWLockExclusive(&q->lock);
    for (int i = 0; i < QUEUE_EVENTS; ++i)
        if (q->events[i].used && q->events[i].event.ident == ident && q->events[i].event.filter == filter) q->events[i].used = 0;
    ReleaseSRWLockExclusive(&q->lock);
    return 0;
}

/* Fires an event in every queue that holds it. */
void rt_equeue_trigger(uint64_t ident, int16_t filter, uint64_t data) {
    AcquireSRWLockShared(&queues_lock);
    for (int k = 0; k < QUEUES; ++k) {
        EventQueue *q = queues[k];
        if (!q) continue;
        int hit = 0;
        AcquireSRWLockExclusive(&q->lock);
        for (int i = 0; i < QUEUE_EVENTS; ++i)
            if (q->events[i].used && q->events[i].event.ident == ident && q->events[i].event.filter == filter) {
                q->events[i].event.data = data;
                q->events[i].triggered = 1;
                hit = 1;
            }
        ReleaseSRWLockExclusive(&q->lock);
        if (hit) WakeAllConditionVariable(&q->changed);
    }
    ReleaseSRWLockShared(&queues_lock);
}

static ABI int32_t equeue_wait(EventQueue *q, GuestEvent *out, int32_t size, int32_t *count, const uint32_t *timeout_us) {
    if (!q) return ORBIS_ERR(E_BADF);
    if (!out || size < 1) return ORBIS_ERR(E_INVAL);
    if (count) *count = 0;
    AcquireSRWLockExclusive(&q->lock);
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        int n = 0;
        for (int i = 0; i < QUEUE_EVENTS && n < size; ++i)
            if (q->events[i].used && q->events[i].triggered) {
                out[n++] = q->events[i].event;
                q->events[i].triggered = 0;
            }
        if (n) {
            ReleaseSRWLockExclusive(&q->lock);
            if (count) *count = n;
            return 0;
        }
        if (q->deleted) {
            ReleaseSRWLockExclusive(&q->lock);
            return ORBIS_ERR(E_BADF);
        }
        DWORD ms = INFINITE;
        if (timeout_us) {
            const ULONGLONG total = (*timeout_us + 999) / 1000, gone = GetTickCount64() - start;
            if (gone >= total) {
                ReleaseSRWLockExclusive(&q->lock);
                return ORBIS_ERR(E_TIMEDOUT);
            }
            ms = (DWORD)(total - gone);
        }
        SleepConditionVariableSRW(&q->changed, &q->lock, ms, 0);
    }
}

/* ---- video out ---- */

enum { FILTER_VIDEO_OUT = -13, EVENT_FLIP = 6, EVENT_VBLANK = 7 };

typedef struct { uint64_t count, process_time, tsc; int64_t flip_arg; uint64_t submit_tsc, reserved0; int32_t gc_queue_num, flip_pending_num, current_buffer; uint32_t reserved1; } FlipStatus;
typedef struct { int32_t full_width, full_height, pane_width, pane_height; uint64_t refresh_rate; float screen_size_in_inch; uint16_t flags, reserved0; uint32_t reserved1[3]; } ResolutionStatus;
typedef struct { uint32_t pixel_format, tiling_mode, aspect_ratio, width, height, pitch_in_pixel, option, reserved0; uint64_t reserved1; } BufferAttribute;

static SRWLOCK vo_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE vo_vblank;
static FlipStatus flip_status = {.flip_arg = -1, .current_buffer = -1};
static volatile uint64_t vblank_count;
static int vo_open;
static void *buffers[16];
static LONGLONG qpc_frequency;

static uint64_t now_counter(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
}

void rt_gnm_report(char *out, size_t size);

static DWORD WINAPI vblank_thread(void *unused) {
    (void)unused;
    for (;;) {
        Sleep(16);
        AcquireSRWLockExclusive(&vo_lock);
        const uint64_t count = ++vblank_count;
        const uint64_t flips = flip_status.count;
        ReleaseSRWLockExclusive(&vo_lock);
        if (count % 300 == 0) { /* a heartbeat every few seconds */
            char gnm[200];
            rt_gnm_report(gnm, sizeof(gnm));
            printf("BEAT vblanks %llu, flips %llu, %s\n", (unsigned long long)count, (unsigned long long)flips, gnm);
        }
        WakeAllConditionVariable(&vo_vblank);
        /* Event data: 12 bits time, 4 bits count, the rest flip argument (0 for a vblank). */
        rt_equeue_trigger(EVENT_VBLANK, FILTER_VIDEO_OUT, (count & 15) << 12);
    }
    return 0;
}

static ABI int32_t vo_open_port(int32_t user, int32_t bus, int32_t index, const void *param) {
    (void)user; (void)param;
    if (bus != 0 || index != 0) return (int32_t)0x80290004;
    AcquireSRWLockExclusive(&vo_lock);
    const int first = !vo_open;
    vo_open = 1;
    ReleaseSRWLockExclusive(&vo_lock);
    if (first) CreateThread(NULL, 0, vblank_thread, NULL, 0, NULL);
    puts("HLE: video out opened, handle 1");
    return 1;
}
static ABI int32_t vo_close(int32_t handle) { (void)handle; return 0; }
static ABI int32_t vo_resolution(int32_t handle, ResolutionStatus *out) {
    if (handle != 1) return (int32_t)0x80290008;
    if (!out) return (int32_t)0x80290002;
    memset(out, 0, sizeof(*out));
    out->full_width = out->pane_width = 1920;
    out->full_height = out->pane_height = 1080;
    out->refresh_rate = 3; /* 59.94 Hz */
    out->screen_size_in_inch = 50;
    return 0;
}
static ABI void vo_set_attribute(BufferAttribute *a, uint32_t format, uint32_t tiling, uint32_t aspect, uint32_t width, uint32_t height, uint32_t pitch) {
    memset(a, 0, sizeof(*a));
    a->pixel_format = format;
    a->tiling_mode = tiling;
    a->aspect_ratio = aspect;
    a->width = width;
    a->height = height;
    a->pitch_in_pixel = pitch;
}
static ABI int32_t vo_register(int32_t handle, int32_t start, void *const *addresses, int32_t count, const BufferAttribute *attribute) {
    if (handle != 1) return (int32_t)0x80290008;
    if (!addresses || !attribute || start < 0 || count < 1 || start + count > 16) return (int32_t)0x80290002;
    for (int i = 0; i < count; ++i) buffers[start + i] = addresses[i];
    printf("HLE: video out registered %d buffers at %d, %ux%u format %#x\n", count, start, attribute->width, attribute->height, attribute->pixel_format);
    return 0;
}
static ABI int32_t vo_unregister(int32_t handle, int32_t attribute_index) { (void)handle; (void)attribute_index; return 0; }
static ABI int32_t vo_ok_mode(void *a, uint64_t b, void *c, uint64_t d, uint64_t e, uint64_t f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    return 0;
}
static ABI int32_t vo_flip_rate(int32_t handle, int32_t rate) { (void)handle; (void)rate; return 0; }

/* A flip finishes at once: there is nothing to show yet. */
int32_t rt_video_flip(int32_t handle, int32_t buffer, int32_t mode, int64_t argument) {
    (void)mode;
    if (handle != 1) return (int32_t)0x80290008;
    if (buffer < -1 || buffer > 15) return (int32_t)0x80290003;
    AcquireSRWLockExclusive(&vo_lock);
    ++flip_status.count;
    flip_status.flip_arg = argument;
    flip_status.current_buffer = buffer;
    flip_status.submit_tsc = flip_status.tsc = now_counter();
    flip_status.process_time = flip_status.tsc / (uint64_t)(qpc_frequency ? qpc_frequency : 1);
    const uint64_t count = flip_status.count;
    ReleaseSRWLockExclusive(&vo_lock);
    rt_equeue_trigger(EVENT_FLIP, FILTER_VIDEO_OUT, ((uint64_t)argument << 16) | ((count & 15) << 12));
    return 0;
}
static ABI int32_t vo_submit_flip(int32_t handle, int32_t buffer, int32_t mode, int64_t argument) { return rt_video_flip(handle, buffer, mode, argument); }
static ABI int32_t vo_flip_status(int32_t handle, FlipStatus *out) {
    if (handle != 1) return (int32_t)0x80290008;
    if (!out) return (int32_t)0x80290002;
    AcquireSRWLockShared(&vo_lock);
    *out = flip_status;
    ReleaseSRWLockShared(&vo_lock);
    return 0;
}
static ABI int32_t vo_flip_pending(int32_t handle) { (void)handle; return 0; }
static ABI int32_t vo_wait_vblank(int32_t handle) {
    if (handle != 1) return (int32_t)0x80290008;
    AcquireSRWLockExclusive(&vo_lock);
    const uint64_t seen = vblank_count;
    while (vblank_count == seen) SleepConditionVariableSRW(&vo_vblank, &vo_lock, 100, 0);
    ReleaseSRWLockExclusive(&vo_lock);
    return 0;
}
static ABI int32_t vo_add_vblank(void *queue, int32_t handle, void *udata) {
    if (handle != 1) return (int32_t)0x80290008;
    return rt_equeue_add(queue, EVENT_VBLANK, FILTER_VIDEO_OUT, udata);
}
static ABI int32_t vo_delete_vblank(void *queue, int32_t handle) {
    if (handle != 1) return (int32_t)0x80290008;
    return rt_equeue_remove(queue, EVENT_VBLANK, FILTER_VIDEO_OUT);
}
static ABI int32_t vo_gamma(void *settings, float gamma) { (void)settings; (void)gamma; return 0; }
static ABI int32_t vo_adjust(int32_t handle, const void *settings) { (void)handle; (void)settings; return 0; }

void rt_video_init(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpc_frequency = f.QuadPart;
    InitializeConditionVariable(&vo_vblank);
}

const RtExport rt_video_exports[] = {
    {"sceKernelCreateEqueue", equeue_create}, {"sceKernelDeleteEqueue", equeue_delete}, {"sceKernelWaitEqueue", equeue_wait},
    {"sceVideoOutOpen", vo_open_port}, {"sceVideoOutClose", vo_close}, {"sceVideoOutGetResolutionStatus", vo_resolution},
    {"sceVideoOutSetBufferAttribute", vo_set_attribute}, {"sceVideoOutRegisterBuffers", vo_register},
    {"sceVideoOutUnregisterBuffers", vo_unregister}, {"sceVideoOutModeSetAny_", vo_ok_mode},
    {"sceVideoOutConfigureOutputMode_", vo_ok_mode}, {"sceVideoOutSetFlipRate", vo_flip_rate},
    {"sceVideoOutSubmitFlip", vo_submit_flip}, {"sceVideoOutGetFlipStatus", vo_flip_status},
    {"sceVideoOutIsFlipPending", vo_flip_pending}, {"sceVideoOutWaitVblank", vo_wait_vblank},
    {"sceVideoOutAddVblankEvent", vo_add_vblank}, {"sceVideoOutDeleteVblankEvent", vo_delete_vblank},
    {"sceVideoOutColorSettingsSetGamma_", vo_gamma}, {"sceVideoOutAdjustColor_", vo_adjust},
};
const size_t rt_video_export_count = sizeof(rt_video_exports) / sizeof(*rt_video_exports);
