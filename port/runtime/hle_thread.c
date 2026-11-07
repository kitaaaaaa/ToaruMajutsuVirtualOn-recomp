/* PS4 threads and synchronization on Windows: scePthread*, semaphores. Handles are pointers in low
 * memory stored in the guest's slot; a slot of 0 or 1 is a static initializer, created on first use;
 * a destroyed one holds 2 (behaviours from bloodborne-runtime's runtime_mutex.c and shadPS4). */
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string.h>

#include "cpu/bbcpu.h"
#include "hle.h"

/* ---- mutexes ---- */

enum { MUTEX_ERRORCHECK = 1, MUTEX_RECURSIVE = 2, MUTEX_NORMAL = 3, MUTEX_ADAPTIVE = 4 };
typedef struct { int type; } MutexAttr;
typedef struct { SRWLOCK lock; volatile DWORD owner; int depth, type; } Mutex;

static SRWLOCK static_lock = SRWLOCK_INIT;

static ABI int32_t mutexattr_init(MutexAttr **out) {
    if (!out) return ORBIS_ERR(E_INVAL);
    *out = rt_low_alloc(sizeof(MutexAttr));
    (*out)->type = MUTEX_ERRORCHECK;
    return 0;
}
static ABI int32_t mutexattr_settype(MutexAttr **attr, int type) {
    if (!attr || !*attr || type < 1 || type > 4) return ORBIS_ERR(E_INVAL);
    (*attr)->type = type;
    return 0;
}
static ABI int32_t mutexattr_setprotocol(MutexAttr **attr, int protocol) {
    return !attr || !*attr || protocol < 0 || protocol > 2 ? ORBIS_ERR(E_INVAL) : 0;
}
static ABI int32_t mutexattr_destroy(MutexAttr **attr) {
    if (!attr || !*attr) return ORBIS_ERR(E_INVAL);
    *attr = NULL;
    return 0;
}

static Mutex *new_mutex(int type) {
    Mutex *m = rt_low_alloc(sizeof(Mutex));
    InitializeSRWLock(&m->lock);
    m->type = type;
    return m;
}
static ABI int32_t mutex_init(Mutex **out, MutexAttr **attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return ORBIS_ERR(E_INVAL);
    *out = new_mutex(attr ? (*attr)->type : MUTEX_ERRORCHECK);
    return 0;
}
static Mutex *ensure_mutex(Mutex **slot) {
    if (!slot) return NULL;
    uintptr_t v = __atomic_load_n((uintptr_t *)slot, __ATOMIC_ACQUIRE);
    if (v == 2) return NULL;
    if (v > 2) return (Mutex *)v;
    AcquireSRWLockExclusive(&static_lock);
    if ((uintptr_t)*slot < 2) __atomic_store_n(slot, new_mutex(MUTEX_ERRORCHECK), __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&static_lock);
    return *slot;
}
static ABI int32_t mutex_lock(Mutex **slot) {
    Mutex *m = ensure_mutex(slot);
    if (!m) return ORBIS_ERR(E_INVAL);
    const DWORD self = GetCurrentThreadId();
    if (m->owner == self) {
        if (m->type == MUTEX_RECURSIVE) { ++m->depth; return 0; }
        if (m->type == MUTEX_ERRORCHECK) return ORBIS_ERR(E_DEADLK);
    }
    AcquireSRWLockExclusive(&m->lock);
    m->owner = self;
    m->depth = 1;
    return 0;
}
static ABI int32_t mutex_trylock(Mutex **slot) {
    Mutex *m = ensure_mutex(slot);
    if (!m) return ORBIS_ERR(E_INVAL);
    const DWORD self = GetCurrentThreadId();
    if (m->owner == self) {
        if (m->type == MUTEX_RECURSIVE) { ++m->depth; return 0; }
        return ORBIS_ERR(E_BUSY);
    }
    if (!TryAcquireSRWLockExclusive(&m->lock)) return ORBIS_ERR(E_BUSY);
    m->owner = self;
    m->depth = 1;
    return 0;
}
static ABI int32_t mutex_unlock(Mutex **slot) {
    Mutex *m = ensure_mutex(slot);
    if (!m) return ORBIS_ERR(E_INVAL);
    if (m->owner != GetCurrentThreadId()) return ORBIS_ERR(E_PERM);
    if (--m->depth) return 0;
    m->owner = 0;
    ReleaseSRWLockExclusive(&m->lock);
    return 0;
}
static ABI int32_t mutex_destroy(Mutex **slot) {
    if (!slot) return ORBIS_ERR(E_INVAL);
    __atomic_store_n((uintptr_t *)slot, (uintptr_t)2, __ATOMIC_RELEASE);
    return 0;
}

/* ---- condition variables ---- */

typedef struct { CONDITION_VARIABLE cond; } Cond;

static Cond *new_cond(void) {
    Cond *c = rt_low_alloc(sizeof(Cond));
    InitializeConditionVariable(&c->cond);
    return c;
}
static ABI int32_t cond_init(Cond **out, const void *attr, const char *name) {
    (void)attr; (void)name;
    if (!out) return ORBIS_ERR(E_INVAL);
    *out = new_cond();
    return 0;
}
static Cond *ensure_cond(Cond **slot) {
    if (!slot) return NULL;
    uintptr_t v = __atomic_load_n((uintptr_t *)slot, __ATOMIC_ACQUIRE);
    if (v == 2) return NULL;
    if (v > 2) return (Cond *)v;
    AcquireSRWLockExclusive(&static_lock);
    if ((uintptr_t)*slot < 2) __atomic_store_n(slot, new_cond(), __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&static_lock);
    return *slot;
}
static int32_t cond_wait_ms(Cond **slot, Mutex **mutex_slot, DWORD ms) {
    Cond *c = ensure_cond(slot);
    Mutex *m = ensure_mutex(mutex_slot);
    if (!c || !m) return ORBIS_ERR(E_INVAL);
    const DWORD self = GetCurrentThreadId();
    if (m->owner != self) return ORBIS_ERR(E_PERM);
    const int depth = m->depth;
    m->owner = 0;
    m->depth = 0;
    const BOOL ok = SleepConditionVariableSRW(&c->cond, &m->lock, ms, 0);
    const DWORD error = ok ? 0 : GetLastError();
    m->owner = self;
    m->depth = depth;
    if (ok) return 0;
    return error == ERROR_TIMEOUT ? ORBIS_ERR(E_TIMEDOUT) : ORBIS_ERR(E_INVAL);
}
static ABI int32_t cond_wait(Cond **slot, Mutex **mutex) { return cond_wait_ms(slot, mutex, INFINITE); }
static ABI int32_t cond_timedwait(Cond **slot, Mutex **mutex, uint64_t usec) {
    const uint64_t ms = (usec + 999) / 1000;
    return cond_wait_ms(slot, mutex, ms >= INFINITE ? INFINITE - 1 : (DWORD)ms);
}
static ABI int32_t cond_signal(Cond **slot) {
    Cond *c = ensure_cond(slot);
    if (!c) return ORBIS_ERR(E_INVAL);
    WakeConditionVariable(&c->cond);
    return 0;
}
static ABI int32_t cond_broadcast(Cond **slot) {
    Cond *c = ensure_cond(slot);
    if (!c) return ORBIS_ERR(E_INVAL);
    WakeAllConditionVariable(&c->cond);
    return 0;
}
static ABI int32_t cond_destroy(Cond **slot) {
    if (!slot) return ORBIS_ERR(E_INVAL);
    __atomic_store_n((uintptr_t *)slot, (uintptr_t)2, __ATOMIC_RELEASE);
    return 0;
}

/* ---- read-write locks ---- */

typedef struct { SRWLOCK lock; volatile DWORD writer; } Rwlock;

static Rwlock *new_rwlock(void) {
    Rwlock *r = rt_low_alloc(sizeof(Rwlock));
    InitializeSRWLock(&r->lock);
    return r;
}
static ABI int32_t rwlock_init(Rwlock **out, const void *attr, const char *name) {
    (void)attr; (void)name;
    if (!out) return ORBIS_ERR(E_INVAL);
    *out = new_rwlock();
    return 0;
}
static Rwlock *ensure_rwlock(Rwlock **slot) {
    if (!slot) return NULL;
    uintptr_t v = __atomic_load_n((uintptr_t *)slot, __ATOMIC_ACQUIRE);
    if (v == 2) return NULL;
    if (v > 2) return (Rwlock *)v;
    AcquireSRWLockExclusive(&static_lock);
    if ((uintptr_t)*slot < 2) __atomic_store_n(slot, new_rwlock(), __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&static_lock);
    return *slot;
}
static ABI int32_t rwlock_rdlock(Rwlock **slot) {
    Rwlock *r = ensure_rwlock(slot);
    if (!r) return ORBIS_ERR(E_INVAL);
    AcquireSRWLockShared(&r->lock);
    return 0;
}
static ABI int32_t rwlock_tryrdlock(Rwlock **slot) {
    Rwlock *r = ensure_rwlock(slot);
    if (!r) return ORBIS_ERR(E_INVAL);
    return TryAcquireSRWLockShared(&r->lock) ? 0 : ORBIS_ERR(E_BUSY);
}
static ABI int32_t rwlock_wrlock(Rwlock **slot) {
    Rwlock *r = ensure_rwlock(slot);
    if (!r) return ORBIS_ERR(E_INVAL);
    AcquireSRWLockExclusive(&r->lock);
    r->writer = GetCurrentThreadId();
    return 0;
}
static ABI int32_t rwlock_trywrlock(Rwlock **slot) {
    Rwlock *r = ensure_rwlock(slot);
    if (!r) return ORBIS_ERR(E_INVAL);
    if (!TryAcquireSRWLockExclusive(&r->lock)) return ORBIS_ERR(E_BUSY);
    r->writer = GetCurrentThreadId();
    return 0;
}
static ABI int32_t rwlock_unlock(Rwlock **slot) {
    Rwlock *r = ensure_rwlock(slot);
    if (!r) return ORBIS_ERR(E_INVAL);
    if (r->writer == GetCurrentThreadId()) {
        r->writer = 0;
        ReleaseSRWLockExclusive(&r->lock);
    } else {
        ReleaseSRWLockShared(&r->lock);
    }
    return 0;
}
static ABI int32_t rwlock_destroy(Rwlock **slot) {
    if (!slot) return ORBIS_ERR(E_INVAL);
    __atomic_store_n((uintptr_t *)slot, (uintptr_t)2, __ATOMIC_RELEASE);
    return 0;
}

/* ---- once and thread-specific data ---- */

typedef struct { volatile int32_t state; int32_t reserved; void *mutex; } Once;
static ABI int32_t once_run(Once *once, const void *fn) {
    if (!once || !fn) return ORBIS_ERR(E_INVAL);
    int32_t expected = 0;
    if (__atomic_compare_exchange_n(&once->state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        rt_guest_call(fn, 0, NULL);
        __atomic_store_n(&once->state, 2, __ATOMIC_RELEASE);
        return 0;
    }
    while (__atomic_load_n(&once->state, __ATOMIC_ACQUIRE) != 2) SwitchToThread();
    return 0;
}

enum { KEYS = 512 };
static const void *volatile key_destructors[KEYS];
static volatile uint8_t key_used[KEYS];
static _Thread_local void *key_values[KEYS];

static ABI int32_t key_create(int32_t *out, const void *destructor) {
    if (!out) return ORBIS_ERR(E_INVAL);
    for (int i = 0; i < KEYS; ++i) {
        uint8_t expected = 0;
        if (__atomic_compare_exchange_n(&key_used[i], &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            key_destructors[i] = destructor;
            *out = i;
            return 0;
        }
    }
    return ORBIS_ERR(E_AGAIN);
}
static ABI int32_t key_delete(int32_t key) {
    if (key < 0 || key >= KEYS || !key_used[key]) return ORBIS_ERR(E_INVAL);
    key_destructors[key] = NULL;
    __atomic_store_n(&key_used[key], 0, __ATOMIC_RELEASE);
    return 0;
}
static ABI int32_t set_specific(int32_t key, void *value) {
    if (key < 0 || key >= KEYS || !key_used[key]) return ORBIS_ERR(E_INVAL);
    key_values[key] = value;
    return 0;
}
static ABI void *get_specific(int32_t key) { return key < 0 || key >= KEYS ? NULL : key_values[key]; }

/* ---- threads ---- */

typedef struct { uint64_t stack_size, guard_size, affinity; int32_t detached, policy, priority, inherit; } ThreadAttr;
typedef struct Thread {
    const void *entry;
    void *argument, *result, *stack;
    uint64_t stack_size, affinity;
    HANDLE handle;
    int32_t detached, priority;
    jmp_buf exit_jump;
    uint32_t fp_mxcsr;
    uint16_t fp_fcw;
    char name[32];
} Thread;

static _Thread_local Thread *current_thread;
static Thread main_thread_record;

void rt_thread_attach(void) {
    uint64_t *tcb = rt_low_alloc(256);
    tcb[0] = (uint64_t)(uintptr_t)tcb; /* tcb_self: what `mov rax, fs:[0]` reads */
    tcb[2] = (uint64_t)(uintptr_t)current_thread;
    bbcpu_set_thread_pointer((uintptr_t)tcb);
    if (!current_thread) {
        current_thread = &main_thread_record;
        snprintf(main_thread_record.name, sizeof(main_thread_record.name), "main");
        main_thread_record.handle = NULL;
        main_thread_record.priority = 700;
        tcb[2] = (uint64_t)(uintptr_t)current_thread;
    }
}

static void run_key_destructors(void) {
    for (int i = 0; i < KEYS; ++i) {
        void *value = key_values[i];
        const void *destructor = key_destructors[i];
        if (!value || !destructor) continue;
        key_values[i] = NULL;
        const uint64_t argument = (uint64_t)(uintptr_t)value;
        rt_guest_call(destructor, 1, &argument);
    }
}

static unsigned __stdcall thread_start(void *p) {
    Thread *t = p;
    current_thread = t;
    /* The game's name for it, for stall reports and debuggers. */
    WCHAR thread_name[64];
    if (t->name[0] && MultiByteToWideChar(CP_UTF8, 0, t->name, -1, thread_name, 64)) SetThreadDescription(GetCurrentThread(), thread_name);
    bbcpu_set_fp_control(t->fp_mxcsr, t->fp_fcw);
    rt_thread_attach();
    if (!_setjmp(t->exit_jump)) {
        const uint64_t argument = (uint64_t)(uintptr_t)t->argument;
        t->result = (void *)(uintptr_t)bbcpu_call_on_stack((uintptr_t)t->entry, 1, &argument, t->stack, t->stack_size - 64);
    }
    run_key_destructors();
    return 0;
}

static ABI int32_t attr_init(ThreadAttr **out) {
    if (!out) return ORBIS_ERR(E_INVAL);
    ThreadAttr *a = rt_low_alloc(sizeof(ThreadAttr));
    a->stack_size = 0x100000;
    a->guard_size = 0x4000;
    a->affinity = 0x7f;
    a->priority = 700;
    *out = a;
    return 0;
}
static ABI int32_t attr_destroy(ThreadAttr **a) {
    if (!a || !*a) return ORBIS_ERR(E_INVAL);
    *a = NULL;
    return 0;
}
#define ATTR_SETTER(name, field, type)                                   \
    static ABI int32_t name(ThreadAttr **a, type value) {                \
        if (!a || !*a) return ORBIS_ERR(E_INVAL);                        \
        (*a)->field = value;                                             \
        return 0;                                                        \
    }
ATTR_SETTER(attr_setstacksize, stack_size, uint64_t)
ATTR_SETTER(attr_setguardsize, guard_size, uint64_t)
ATTR_SETTER(attr_setaffinity, affinity, uint64_t)
ATTR_SETTER(attr_setdetachstate, detached, int32_t)
ATTR_SETTER(attr_setschedpolicy, policy, int32_t)
ATTR_SETTER(attr_setinheritsched, inherit, int32_t)
static ABI int32_t attr_setschedparam(ThreadAttr **a, const int32_t *param) {
    if (!a || !*a || !param) return ORBIS_ERR(E_INVAL);
    (*a)->priority = *param;
    return 0;
}
static ABI int32_t attr_getaffinity(ThreadAttr **a, uint64_t *out) {
    if (!a || !*a || !out) return ORBIS_ERR(E_INVAL);
    *out = (*a)->affinity;
    return 0;
}
static ABI int32_t attr_getdetachstate(ThreadAttr **a, int32_t *out) {
    if (!a || !*a || !out) return ORBIS_ERR(E_INVAL);
    *out = (*a)->detached;
    return 0;
}
static ABI int32_t attr_get(Thread *t, ThreadAttr **a) {
    if (!t || !a || !*a) return ORBIS_ERR(E_INVAL);
    (*a)->stack_size = t->stack_size;
    (*a)->affinity = t->affinity;
    (*a)->detached = t->detached;
    (*a)->priority = t->priority;
    return 0;
}

static ABI int32_t thread_create(Thread **out, ThreadAttr **attr, const void *entry, void *argument, const char *name) {
    if (!out || !entry) return ORBIS_ERR(E_INVAL);
    Thread *t = rt_low_alloc(sizeof(Thread));
    t->entry = entry;
    t->argument = argument;
    t->stack_size = attr && *attr ? (*attr)->stack_size : 0x100000;
    t->affinity = attr && *attr ? (*attr)->affinity : 0x7f;
    t->detached = attr && *attr ? (*attr)->detached : 0;
    t->priority = attr && *attr ? (*attr)->priority : 700;
    if (t->stack_size < 0x10000) t->stack_size = 0x10000;
    t->stack = runtime_low_map(t->stack_size, 3);
    if (!t->stack) return ORBIS_ERR(E_NOMEM);
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "");
    bbcpu_get_fp_control(&t->fp_mxcsr, &t->fp_fcw);
    *out = t;
    printf("HLE: thread \"%s\" entry +%llx stack %#llx\n", t->name, (unsigned long long)((uintptr_t)entry - bb_image_base),
           (unsigned long long)t->stack_size);
    /* The host stack holds the C frames of recompiled guest calls: much larger than the guest's. */
    const uintptr_t handle = _beginthreadex(NULL, 64 * 1024 * 1024, thread_start, t, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!handle) return ORBIS_ERR(E_AGAIN);
    t->handle = (HANDLE)handle;
    return 0;
}
static ABI Thread *thread_self(void) { return current_thread; }
static ABI int32_t thread_equal(Thread *a, Thread *b) { return a == b; }
static ABI int32_t thread_detach(Thread *t) {
    if (!t) return ORBIS_ERR(E_INVAL);
    t->detached = 1;
    return 0;
}
static ABI int32_t thread_join(Thread *t, void **result) {
    if (!t || !t->handle) return ORBIS_ERR(E_INVAL);
    WaitForSingleObject(t->handle, INFINITE);
    if (result) *result = t->result;
    return 0;
}
static ABI _Noreturn void thread_exit(void *result) {
    Thread *t = current_thread;
    if (!t || t == &main_thread_record) rt_finish(0, "main thread called scePthreadExit");
    t->result = result;
    _longjmp(t->exit_jump, 1);
}
static ABI int32_t thread_yield(void) {
    SwitchToThread();
    return 0;
}
static ABI int32_t thread_getaffinity(Thread *t, uint64_t *out) {
    if (!t || !out) return ORBIS_ERR(E_INVAL);
    *out = t->affinity;
    return 0;
}
static ABI int32_t thread_setaffinity(Thread *t, uint64_t mask) {
    if (!t) return ORBIS_ERR(E_INVAL);
    t->affinity = mask;
    return 0;
}
static ABI int32_t thread_getprio(Thread *t, int32_t *out) {
    if (!t || !out) return ORBIS_ERR(E_INVAL);
    *out = t->priority;
    return 0;
}
static ABI int32_t thread_setprio(Thread *t, int32_t priority) {
    if (!t) return ORBIS_ERR(E_INVAL);
    t->priority = priority;
    return 0;
}
static ABI int32_t thread_rename(Thread *t, const char *name) {
    if (!t || !name) return ORBIS_ERR(E_INVAL);
    snprintf(t->name, sizeof(t->name), "%s", name);
    return 0;
}
static ABI int32_t thread_getname(Thread *t, char *out) {
    if (!t || !out) return ORBIS_ERR(E_INVAL);
    memcpy(out, t->name, sizeof(t->name));
    return 0;
}

/* ---- semaphores ---- */

typedef struct { SRWLOCK lock; CONDITION_VARIABLE changed; int32_t count, max; volatile int deleted; } Sema;

static ABI int32_t sema_create(Sema **out, const char *name, uint32_t attr, int32_t initial, int32_t max, const void *option) {
    (void)name; (void)attr; (void)option;
    if (!out || initial < 0 || max <= 0 || initial > max) return ORBIS_ERR(E_INVAL);
    Sema *s = rt_low_alloc(sizeof(Sema));
    InitializeSRWLock(&s->lock);
    InitializeConditionVariable(&s->changed);
    s->count = initial;
    s->max = max;
    *out = s;
    return 0;
}
static ABI int32_t sema_wait(Sema *s, int32_t need, uint32_t *timeout_us) {
    if (!s || need <= 0 || need > s->max) return ORBIS_ERR(E_INVAL);
    AcquireSRWLockExclusive(&s->lock);
    const ULONGLONG start = GetTickCount64();
    while (s->count < need && !s->deleted) {
        DWORD ms = INFINITE;
        if (timeout_us) {
            const ULONGLONG total = (*timeout_us + 999) / 1000, gone = GetTickCount64() - start;
            if (gone >= total) {
                *timeout_us = 0;
                ReleaseSRWLockExclusive(&s->lock);
                return ORBIS_ERR(E_TIMEDOUT);
            }
            ms = (DWORD)(total - gone);
        }
        SleepConditionVariableSRW(&s->changed, &s->lock, ms, 0);
    }
    const int deleted = s->deleted;
    if (!deleted) s->count -= need;
    ReleaseSRWLockExclusive(&s->lock);
    return deleted ? ORBIS_ERR(E_ACCES) : 0;
}
static ABI int32_t sema_signal(Sema *s, int32_t count) {
    if (!s || count <= 0) return ORBIS_ERR(E_INVAL);
    AcquireSRWLockExclusive(&s->lock);
    if (s->count + count > s->max) {
        ReleaseSRWLockExclusive(&s->lock);
        return ORBIS_ERR(E_INVAL);
    }
    s->count += count;
    ReleaseSRWLockExclusive(&s->lock);
    WakeAllConditionVariable(&s->changed);
    return 0;
}
static ABI int32_t sema_delete(Sema *s) {
    if (!s) return ORBIS_ERR(E_INVAL);
    s->deleted = 1;
    WakeAllConditionVariable(&s->changed);
    return 0;
}

const RtExport rt_thread_exports[] = {
    {"scePthreadMutexattrInit", mutexattr_init}, {"scePthreadMutexattrSettype", mutexattr_settype},
    {"scePthreadMutexattrSetprotocol", mutexattr_setprotocol}, {"scePthreadMutexattrDestroy", mutexattr_destroy},
    {"scePthreadMutexInit", mutex_init}, {"scePthreadMutexLock", mutex_lock}, {"scePthreadMutexTrylock", mutex_trylock},
    {"scePthreadMutexUnlock", mutex_unlock}, {"scePthreadMutexDestroy", mutex_destroy},
    {"scePthreadCondInit", cond_init}, {"scePthreadCondWait", cond_wait}, {"scePthreadCondTimedwait", cond_timedwait},
    {"scePthreadCondSignal", cond_signal}, {"scePthreadCondBroadcast", cond_broadcast}, {"scePthreadCondDestroy", cond_destroy},
    {"scePthreadRwlockInit", rwlock_init}, {"scePthreadRwlockRdlock", rwlock_rdlock}, {"scePthreadRwlockTryrdlock", rwlock_tryrdlock},
    {"scePthreadRwlockWrlock", rwlock_wrlock}, {"scePthreadRwlockTrywrlock", rwlock_trywrlock},
    {"scePthreadRwlockUnlock", rwlock_unlock}, {"scePthreadRwlockDestroy", rwlock_destroy},
    {"scePthreadOnce", once_run}, {"scePthreadKeyCreate", key_create}, {"scePthreadKeyDelete", key_delete},
    {"scePthreadSetspecific", set_specific}, {"scePthreadGetspecific", get_specific},
    {"scePthreadAttrInit", attr_init}, {"scePthreadAttrDestroy", attr_destroy}, {"scePthreadAttrSetstacksize", attr_setstacksize},
    {"scePthreadAttrSetguardsize", attr_setguardsize}, {"scePthreadAttrSetaffinity", attr_setaffinity},
    {"scePthreadAttrSetdetachstate", attr_setdetachstate}, {"scePthreadAttrSetschedpolicy", attr_setschedpolicy},
    {"scePthreadAttrSetinheritsched", attr_setinheritsched}, {"scePthreadAttrSetschedparam", attr_setschedparam},
    {"scePthreadAttrGetaffinity", attr_getaffinity}, {"scePthreadAttrGetdetachstate", attr_getdetachstate}, {"scePthreadAttrGet", attr_get},
    {"scePthreadCreate", thread_create}, {"scePthreadSelf", thread_self}, {"scePthreadEqual", thread_equal},
    {"scePthreadDetach", thread_detach}, {"scePthreadJoin", thread_join}, {"scePthreadExit", thread_exit},
    {"scePthreadYield", thread_yield}, {"sched_yield", thread_yield}, {"scePthreadGetaffinity", thread_getaffinity},
    {"scePthreadSetaffinity", thread_setaffinity}, {"scePthreadGetprio", thread_getprio}, {"scePthreadSetprio", thread_setprio},
    {"scePthreadRename", thread_rename}, {"scePthreadGetname", thread_getname},
    {"sceKernelCreateSema", sema_create}, {"sceKernelWaitSema", sema_wait}, {"sceKernelSignalSema", sema_signal},
    {"sceKernelDeleteSema", sema_delete},
};
const size_t rt_thread_export_count = sizeof(rt_thread_exports) / sizeof(*rt_thread_exports);
