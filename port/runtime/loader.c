/* vox: loads the game's executable and modules into one flat image, resolves imports, and starts the
 * game on the recompiled code.
 *
 * Layout and start order follow bbport's and bloodborne-runtime's loader (src/probe.c,
 * scripts/link_modules.py, GPL-2.0-or-later): each module 64 KiB-aligned after the previous one, so a
 * recompiled library made with the same layout finds its functions; modules start in order, then
 * the game's entry runs on a low stack.
 *
 * Usage: vox ELF_DIR NAMES.TSV [--timeout seconds] [--english | --user-tl] */
#include <windows.h>
#include <tlhelp32.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/bbcpu.h"
#include "runtime.h"
#include <signal.h>
#include "voxgpu.h"

uint64_t bb_image_base;

/* VOX_NO_GPU=1 runs with no window: hle_gnm.c and hle_video.c answer instead of the renderer. */
static int gpu_enabled;

enum { IMAGE_MAX = 512 * 1024 * 1024, MAX_MODULES = 16 };
#define IMAGE_AT UINT64_C(0x100000000)
#define LOW_START UINT64_C(0x1000000000)
#define LOW_END UINT64_C(0xF000000000)

static void fail(const char *format, const char *argument) {
    fprintf(stderr, "ERROR: ");
    fprintf(stderr, format, argument);
    fprintf(stderr, "\n");
    exit(1);
}

/* ---- low memory ---- */

void *runtime_low_map(size_t size, int prot) {
    static volatile int64_t next = (int64_t)LOW_START;
    const DWORD mode = (prot & 2) ? PAGE_READWRITE : (prot & 1) ? PAGE_READONLY : PAGE_NOACCESS;
    const size_t span = (size + 0xffff) & ~(size_t)0xffff;
    while ((uint64_t)next + span < LOW_END) {
        const int64_t at = InterlockedExchangeAdd64(&next, (int64_t)span);
        void *p = VirtualAlloc((void *)(uintptr_t)at, span, MEM_RESERVE | MEM_COMMIT, mode);
        if (p) {
            voxgpu_notify_map((uint64_t)at, span);
            return p;
        }
    }
    return NULL;
}

void *rt_low_alloc(size_t size) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static uint8_t *chunk;
    static size_t left;
    size = (size + 63) & ~(size_t)63;
    if (size >= 0x10000) {
        void *p = runtime_low_map(size, 3);
        if (!p) fail("%s", "out of low memory");
        return p;
    }
    AcquireSRWLockExclusive(&lock);
    if (left < size) {
        chunk = runtime_low_map(0x100000, 3);
        if (!chunk) fail("%s", "out of low memory");
        left = 0x100000;
    }
    void *p = chunk;
    chunk += size;
    left -= size;
    ReleaseSRWLockExclusive(&lock);
    return p;
}

/* ---- names ---- */

typedef struct { char *nid, *symbol; } Name;
static Name *names;
static size_t name_count;

static int name_order(const void *a, const void *b) { return strcmp(((const Name *)a)->nid, ((const Name *)b)->nid); }

static void load_names(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) fail("cannot open %s", path);
    char line[1024];
    size_t capacity = 0;
    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *end = tab + 1 + strcspn(tab + 1, "\r\n");
        *end = 0;
        if (name_count == capacity) {
            capacity = capacity ? capacity * 2 : 4096;
            names = realloc(names, capacity * sizeof(*names));
        }
        names[name_count++] = (Name){_strdup(line), _strdup(tab + 1)};
    }
    fclose(f);
    qsort(names, name_count, sizeof(*names), name_order);
}

static const char *symbol_name(const char *nid) {
    const Name key = {(char *)nid, NULL};
    const Name *found = bsearch(&key, names, name_count, sizeof(*names), name_order);
    return found ? found->symbol : NULL;
}

/* ---- imports with no implementation: a stub that logs and returns 0 ---- */

typedef struct { char nid[32]; const char *symbol; uint64_t calls; } Unimplemented;
static Unimplemented *stubs;
static size_t stub_count, stub_capacity;
static uint8_t *stub_code;
enum { STUB_BYTES = 32, STUB_MAX = 8192 };
extern void rt_import_shim(void);

ABI uint64_t rt_unimplemented(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t index) {
    Unimplemented *s = &stubs[index];
    const uint64_t n = InterlockedIncrement64((volatile int64_t *)&s->calls);
    /* PSN isn't part of this port: its calls get what a console not signed in gets (ORBIS_NP_ERROR_SIGNED_OUT). */
    if (s->symbol && !strncmp(s->symbol, "sceNp", 5)) {
        if (n == 1) printf("HLE: %s answered signed out, from +%" PRIx64 "\n", s->symbol, bbcpu_import_return() - bb_image_base);
        return UINT32_C(0x80550006);
    }
    /* Nor is its game networking. The code is "not initialized" from memory, not checked against a
     * table; the game's NetworkManager only logs a negative result from these and carries on. */
    if (s->symbol && !strncmp(s->symbol, "sceRudp", 7)) {
        if (n == 1) printf("HLE: %s answered not initialized, from +%" PRIx64 "\n", s->symbol, bbcpu_import_return() - bb_image_base);
        return UINT32_C(0x80770001);
    }
    /* By default an import nobody has looked at ends the run: answering it with a made-up success
     * would send the game down a wrong path. VOX_LENIENT=1 logs and returns 0 instead. */
    static int lenient = -1;
    if (lenient < 0) lenient = getenv("VOX_LENIENT") && getenv("VOX_LENIENT")[0] == '1';
    if (!lenient) {
        printf("STOP: unimplemented %s (%s) args %" PRIx64 " %" PRIx64 " %" PRIx64 " %" PRIx64 " %" PRIx64 " %" PRIx64 " from +%" PRIx64 "\n",
               s->symbol ? s->symbol : "?", s->nid, a0, a1, a2, a3, a4, a5, bbcpu_import_return() - bb_image_base);
        rt_finish(44, "unimplemented import");
    }
    if (n <= 8)
        printf("UNIMPLEMENTED %s (%s) #%" PRIu64 " args %" PRIx64 " %" PRIx64 " %" PRIx64 " %" PRIx64 " %" PRIx64 " %" PRIx64
               " from +%" PRIx64 "\n", s->symbol ? s->symbol : "?", s->nid, n, a0, a1, a2, a3, a4, a5,
               bbcpu_import_return() - bb_image_base);
    return 0;
}

static uintptr_t make_stub(const char *nid, const char *symbol) {
    for (size_t i = 0; i < stub_count; ++i)
        if (!strcmp(stubs[i].nid, nid)) return (uintptr_t)(stub_code + i * STUB_BYTES);
    if (stub_count == STUB_MAX) fail("%s", "too many unresolved imports");
    if (stub_count == stub_capacity) {
        stub_capacity = stub_capacity ? stub_capacity * 2 : 1024;
        stubs = realloc(stubs, stub_capacity * sizeof(*stubs));
    }
    Unimplemented *s = &stubs[stub_count];
    memset(s, 0, sizeof(*s));
    snprintf(s->nid, sizeof(s->nid), "%s", nid);
    s->symbol = symbol;
    uint8_t *t = stub_code + stub_count * STUB_BYTES;
    const uint64_t index = stub_count, shim = (uint64_t)(uintptr_t)rt_import_shim;
    t[0] = 0x49; t[1] = 0xba; memcpy(t + 2, &index, 8); /* mov r10, index */
    t[10] = 0x49; t[11] = 0xbb; memcpy(t + 12, &shim, 8); /* mov r11, shim */
    t[20] = 0x41; t[21] = 0xff; t[22] = 0xe3; /* jmp r11 */
    ++stub_count;
    return (uintptr_t)t;
}

/* ---- modules ---- */

typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;
typedef struct { uint32_t name; uint8_t info, other; uint16_t section; uint64_t value, size; } Sym;
typedef struct { uint64_t offset, info; int64_t addend; } Rela;

typedef struct {
    char name[64];
    uint8_t *file;
    size_t file_size;
    uint64_t base, size, init, entry;
    uint64_t tls_id;
    const Phdr *ph;
    int phnum;
    const char *strings;
    uint64_t strings_size;
    const Sym *syms;
    uint64_t sym_count;
    const Rela *rela[2];
    uint64_t rela_count[2];
} Module;

static Module modules[MAX_MODULES];
static int module_count;
static uint8_t *image;
static uint64_t image_used;

typedef struct { const char *nid; uint64_t address; int is_data; } Export;
static Export *exports;
static size_t export_count, export_capacity;

static int export_order(const void *a, const void *b) { return strcmp(((const Export *)a)->nid, ((const Export *)b)->nid); }

/* A symbol's NID: its name up to the first '#'. */
static const char *nid_of(const char *name, char *out, size_t size) {
    size_t n = strcspn(name, "#");
    if (n >= size) n = size - 1;
    memcpy(out, name, n);
    out[n] = 0;
    return out;
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) fail("cannot open %s (run the pipeline's ps4elf step)", path);
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) fail("cannot read %s", path);
    fclose(f);
    *size = (size_t)n;
    return data;
}

static const Phdr *find_ph(const Module *m, uint32_t type) {
    for (int i = 0; i < m->phnum; ++i)
        if (m->ph[i].type == type) return &m->ph[i];
    return NULL;
}

static void place_module(const char *dir, const char *name) {
    if (module_count == MAX_MODULES) fail("%s", "too many modules");
    Module *m = &modules[module_count++];
    snprintf(m->name, sizeof(m->name), "%s", name);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.elf", dir, name);
    m->file = read_file(path, &m->file_size);

    uint64_t phoff, entry;
    uint16_t phnum;
    memcpy(&entry, m->file + 0x18, 8);
    memcpy(&phoff, m->file + 0x20, 8);
    memcpy(&phnum, m->file + 0x38, 2);
    m->ph = (const Phdr *)(m->file + phoff);
    m->phnum = phnum;

    m->base = (image_used + 0xffff) & ~UINT64_C(0xffff);
    uint64_t size = 0;
    for (int i = 0; i < m->phnum; ++i) {
        const Phdr *p = &m->ph[i];
        if ((p->type == 1 || p->type == 0x61000010) && p->vaddr + p->memsz > size) size = p->vaddr + p->memsz;
    }
    if (m->base + size > IMAGE_MAX) fail("%s", "image too large");
    m->size = size;
    m->entry = entry;
    for (int i = 0; i < m->phnum; ++i) {
        const Phdr *p = &m->ph[i];
        if (p->type != 1 && p->type != 0x61000010) continue;
        if (p->offset + p->filesz > m->file_size) fail("segment outside %s", name);
        memcpy(image + m->base + p->vaddr, m->file + p->offset, p->filesz);
    }
    image_used = m->base + size;

    const Phdr *dyn = find_ph(m, 2), *blob = find_ph(m, 0x61000000);
    if (!dyn || !blob) fail("no dynamic tables in %s", name);
    const uint8_t *data = m->file + blob->offset;
    uint64_t rela = 0, rela_size = 0, jmp = 0, jmp_size = 0, sym = 0, sym_size = 0, str = 0, str_size = 0;
    for (const uint64_t *d = (const uint64_t *)(m->file + dyn->offset); d[0]; d += 2) {
        switch (d[0]) {
        case 0x0c: m->init = d[1]; break;
        case 0x61000029: rela = d[1]; break;
        case 0x6100002d: rela_size = d[1]; break;
        case 0x6100002f: jmp = d[1]; break;
        case 0x61000031: jmp_size = d[1]; break;
        case 0x61000035: str = d[1]; break;
        case 0x61000037: str_size = d[1]; break;
        case 0x61000039: sym = d[1]; break;
        case 0x6100003f: sym_size = d[1]; break;
        default: break;
        }
    }
    m->strings = (const char *)data + str;
    m->strings_size = str_size;
    m->syms = (const Sym *)(data + sym);
    m->sym_count = sym_size / sizeof(Sym);
    m->rela[0] = (const Rela *)(data + rela);
    m->rela_count[0] = rela_size / sizeof(Rela);
    m->rela[1] = (const Rela *)(data + jmp);
    m->rela_count[1] = jmp_size / sizeof(Rela);

    int exported = 0;
    for (uint64_t i = 0; i < m->sym_count; ++i) {
        const Sym *s = &m->syms[i];
        const int binding = s->info >> 4, type = s->info & 15;
        if (!s->section || (binding != 1 && binding != 2) || (type != 1 && type != 2)) continue;
        const char *symbol = m->strings + s->name;
        if (!strchr(symbol, '#')) continue;
        if (export_count == export_capacity) {
            export_capacity = export_capacity ? export_capacity * 2 : 4096;
            exports = realloc(exports, export_capacity * sizeof(*exports));
        }
        char nid[32];
        nid_of(symbol, nid, sizeof(nid));
        exports[export_count++] = (Export){_strdup(nid), m->base + s->value, type == 1};
        ++exported;
    }
    printf("Module %-16s base +%#" PRIx64 " size %#" PRIx64 " init +%#" PRIx64 " exports %d\n", name, m->base, size, m->init, exported);
}

static uint64_t next_tls_id = 2;

static uint64_t resolve_symbol(const Module *m, const Sym *s, int64_t addend, int *unresolved_data) {
    const char *symbol = m->strings + s->name;
    if (s->section) return (s->section == 0xfff1 ? 0 : (uint64_t)(uintptr_t)image + m->base) + s->value + (uint64_t)addend;
    if (!strchr(symbol, '#')) {
        if ((s->info >> 4) == 2) return 0; /* weak undefined */
        fail("undefined plain symbol %s", symbol);
    }
    const int is_data = (s->info & 15) == 1;
    char nid[32];
    nid_of(symbol, nid, sizeof(nid));
    const char *name = symbol_name(nid);
    if (gpu_enabled && !is_data) {
        const uintptr_t renderer = voxgpu_resolve(nid);
        if (renderer) return renderer + (uint64_t)addend;
    }
    const uintptr_t host = rt_resolve(nid, name, is_data);
    if (host) return host + (uint64_t)addend;
    const Export key = {nid, 0, 0};
    const Export *found = bsearch(&key, exports, export_count, sizeof(*exports), export_order);
    if (found) return (uint64_t)(uintptr_t)image + found->address + (uint64_t)addend;
    if (is_data) {
        printf("UNRESOLVED DATA %s (%s): zero page\n", name ? name : "?", nid);
        ++*unresolved_data;
        return (uint64_t)(uintptr_t)rt_low_alloc(0x10000) + (uint64_t)addend;
    }
    return make_stub(nid, name);
}

static void relocate(Module *m) {
    const Phdr *tls = find_ph(m, 7);
    if (tls && tls->memsz) {
        m->tls_id = next_tls_id++;
        rt_set_tls_module(m->tls_id, image + m->base + tls->vaddr, tls->filesz, tls->memsz);
    }
    int data = 0;
    for (int table = 0; table < 2; ++table) {
        for (uint64_t i = 0; i < m->rela_count[table]; ++i) {
            const Rela *r = &m->rela[table][i];
            const uint32_t type = (uint32_t)r->info, sym = (uint32_t)(r->info >> 32);
            uint64_t value;
            switch (type) {
            case 8: value = (uint64_t)(uintptr_t)image + m->base + (uint64_t)r->addend; break;
            case 1: case 6: case 7: value = resolve_symbol(m, &m->syms[sym], r->addend, &data); break;
            case 16:
                if (!m->tls_id) fail("TLS relocation with no TLS in %s", m->name);
                value = m->tls_id;
                break;
            default: {
                char text[32];
                snprintf(text, sizeof(text), "%u", type);
                fail("unsupported relocation type %s", text);
                return;
            }
            }
            memcpy(image + m->base + r->offset, &value, 8);
        }
    }
}

/* ---- faults and the watchdog ---- */

/* Words on the stack that point into code, for profile-style symbolizing by hand after a crash. */
static void print_stack_code(uint64_t rsp) {
    MEMORY_BASIC_INFORMATION region;
    if (!VirtualQuery((void *)rsp, &region, sizeof(region))) return;
    const uint64_t *stack = (const uint64_t *)rsp;
    size_t words = ((uint64_t)region.BaseAddress + region.RegionSize - rsp) / 8;
    if (words > 8192) words = 8192;
    int shown = 0;
    for (size_t w = 0; w < words && shown < 40; ++w) {
        HMODULE module = NULL;
        char path[260];
        if (stack[w] < 0x10000 || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)stack[w], &module)) continue;
        if (!GetModuleFileNameA(module, path, sizeof(path))) continue;
        const char *base = strrchr(path, '\\');
        printf("  stack %s+%llx\n", base ? base + 1 : path, (unsigned long long)(stack[w] - (uint64_t)module));
        ++shown;
    }
}

/* abort(): std::terminate and the renderer's failed checks end here, without an exception. */
static void on_abort(int signal_number) {
    (void)signal_number;
    uint64_t marker = 0;
    printf("ABORT on thread %lu, guest rip +%#llx\n", GetCurrentThreadId(), (unsigned long long)(bbcpu_guest_rip() - bb_image_base));
    print_stack_code((uint64_t)&marker);
    rt_finish(45, "abort");
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;
    /* The renderer's page tracking write-protects guest pages: these faults are normal. */
    if (gpu_enabled && code == EXCEPTION_ACCESS_VIOLATION &&
        voxgpu_handle_fault(info->ContextRecord, (void *)info->ExceptionRecord->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_EXECUTION;
    const uintptr_t rip = (uintptr_t)info->ContextRecord->Rip;
    HMODULE module = NULL;
    char path[260] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &module))
        GetModuleFileNameA(module, path, sizeof(path));
    printf("FAULT %#lx at host %#llx (%s +%#llx), address %#llx (%s), guest rip +%#llx, thread %lu\n", code,
           (unsigned long long)rip, path, (unsigned long long)(rip - (uintptr_t)module),
           (unsigned long long)info->ExceptionRecord->ExceptionInformation[1],
           info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
           (unsigned long long)(bbcpu_guest_rip() - bb_image_base), GetCurrentThreadId());
    print_stack_code(info->ContextRecord->Rsp);
    rt_finish(40, "fault");
}

/* VOX_PROFILE=seconds: samples every thread's instruction pointer every 5 ms after a short delay, and
 * writes profile.tsv (thread, module, offset) for analysis\scripts\profile_report.py. */
enum { PROFILE_FRAMES = 24 };
typedef struct { uint32_t thread; uint64_t frames[PROFILE_FRAMES]; } ProfileSample;
static HANDLE stall_event; /* set by the heartbeat when flips stop */

/* CPU time of every thread in this process, in 100 ns units, with its name. */
enum { PROFILE_THREADS = 256 };
typedef struct { uint32_t id; uint64_t user, kernel; char name[48]; } ThreadTime;
static int thread_ids(uint32_t *ids, DWORD self) {
    int n = 0;
    const DWORD process = GetCurrentProcessId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    if (snapshot != INVALID_HANDLE_VALUE && Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self && n < PROFILE_THREADS) ids[n++] = entry.th32ThreadID;
        } while (Thread32Next(snapshot, &entry));
    }
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    return n;
}
static int read_times(ThreadTime *out, DWORD self) {
    static uint32_t ids[PROFILE_THREADS];
    const int n = thread_ids(ids, self);
    for (int i = 0; i < n; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].id = ids[i];
        const HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, ids[i]);
        if (!h) continue;
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes(h, &created, &exited, &kernel, &user)) {
            out[i].user = ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
            out[i].kernel = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
        }
        PWSTR name = NULL;
        if (SUCCEEDED(GetThreadDescription(h, &name)) && name) {
            WideCharToMultiByte(CP_UTF8, 0, name, -1, out[i].name, sizeof(out[i].name), NULL, NULL);
            LocalFree(name);
        }
        CloseHandle(h);
    }
    return n;
}

static DWORD WINAPI profiler(void *argument) {
    const unsigned seconds = (unsigned)(uintptr_t)argument;
    enum { MAX_SAMPLES = 400000 };
    ProfileSample *samples = malloc(MAX_SAMPLES * sizeof(*samples));
    static ThreadTime before[PROFILE_THREADS], after[PROFILE_THREADS];
    if (!samples) return 0;
    size_t count = 0;
    const DWORD self = GetCurrentThreadId();
    /* VOX_PROFILE_DELAY: seconds to wait first, or "stall" for when flips stop (then exit when done). */
    const char *delay = getenv("VOX_PROFILE_DELAY");
    const int on_stall = delay && !strcmp(delay, "stall");
    if (on_stall) WaitForSingleObject(stall_event, INFINITE);
    else Sleep(delay ? (DWORD)atoi(delay) * 1000 : 15000);
    puts("PROFILE: thread times");
    const int before_count = read_times(before, self);
    const ULONGLONG times_start = GetTickCount64();
    Sleep(seconds * 1000);
    const ULONGLONG times_elapsed = GetTickCount64() - times_start;
    const int after_count = read_times(after, self);
    FILE *f = fopen("profile.tsv", "w");
    if (!f) return 0;
    fprintf(f, "#rounds\tthread times over\t%llu ms\n", (unsigned long long)times_elapsed);
    for (int i = 0; i < after_count; ++i) {
        uint64_t user = after[i].user, kernel = after[i].kernel;
        for (int k = 0; k < before_count; ++k)
            if (before[k].id == after[i].id) { user -= before[k].user; kernel -= before[k].kernel; }
        fprintf(f, "#thread\t%u\t%s\t%.1f\t%.1f\n", after[i].id, after[i].name, (double)user / 1e4, (double)kernel / 1e4);
    }
    fflush(f);

    /* Samples: registers and a copy of the top of the stack while paused; nothing that takes a lock
     * (unwinding does, and a paused thread may hold it). Return addresses are picked out afterwards. */
    puts("PROFILE: sampling");
    const ULONGLONG start = GetTickCount64(), end = start + seconds * 1000;
    static uint32_t ids[PROFILE_THREADS];
    uint32_t rounds = 0;
    const uint64_t exe_start = (uint64_t)GetModuleHandleA(NULL);
    const uint64_t game_start = (uint64_t)GetModuleHandleA("librecomp.dll");
    const uint64_t game_end = game_start ? game_start + ((IMAGE_NT_HEADERS *)(game_start + ((IMAGE_DOS_HEADER *)game_start)->e_lfanew))->OptionalHeader.SizeOfImage : 0;
    while (GetTickCount64() < end && count + PROFILE_THREADS < MAX_SAMPLES) {
        const int n = thread_ids(ids, self);
        ++rounds;
        for (int i = 0; i < n; ++i) {
            const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, ids[i]);
            if (!h) continue;
            if (SuspendThread(h) != (DWORD)-1) {
                CONTEXT context;
                context.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &context)) {
                    ProfileSample *s = &samples[count++];
                    memset(s, 0, sizeof(*s));
                    s->thread = ids[i];
                    s->frames[0] = context.Rip;
                    /* Words on the stack that point into the executable: callers, and a little noise. */
                    MEMORY_BASIC_INFORMATION region;
                    if (VirtualQuery((void *)context.Rsp, &region, sizeof(region))) {
                        const uint64_t *stack = (const uint64_t *)context.Rsp;
                        const size_t got = (uint64_t)region.BaseAddress + region.RegionSize - context.Rsp;
                        int f = 1;
                        for (size_t w = 0; w < got / 8 && f < PROFILE_FRAMES; ++w)
                            if ((stack[w] >= exe_start + 0x1000 && stack[w] < exe_start + 0x4000000) || (stack[w] >= game_start && stack[w] < game_end)) s->frames[f++] = stack[w];
                    }
                }
                ResumeThread(h);
            }
            CloseHandle(h);
        }
        Sleep(5);
    }
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (f) {
        fprintf(f, "#rounds\tsamples\t%u rounds in %llu ms\n", rounds, (unsigned long long)elapsed);
        for (size_t i = 0; i < count; ++i) {
            fprintf(f, "%u", samples[i].thread);
            for (int k = 0; k < PROFILE_FRAMES && samples[i].frames[k]; ++k) {
                HMODULE module = NULL;
                char path[260] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)samples[i].frames[k], &module))
                    GetModuleFileNameA(module, path, sizeof(path));
                const char *base = strrchr(path, '\\');
                fprintf(f, "\t%s+%llx", base ? base + 1 : path, (unsigned long long)(samples[i].frames[k] - (uint64_t)module));
            }
            fputc('\n', f);
        }
        fclose(f);
    }
    printf("PROFILE: %zu samples, %u rounds over %llu ms written\n", count, rounds, (unsigned long long)elapsed);
    if (on_stall) rt_finish(42, "profiled a stall");
    return 0;
}

/* Words on a stack copy that point into code, innermost first, one line each. System modules are kept (they show
 * where a thread is blocked) but repeats of them are dropped. analysis\scripts\stall_report.py makes this readable. */
static void print_stall_stack(const uint64_t *stack, size_t words) {
    int shown = 0;
    HMODULE previous = NULL;
    const uint64_t exe = (uint64_t)GetModuleHandleA(NULL);
    const HMODULE game = GetModuleHandleA("librecomp.dll");
    for (size_t w = 0; w < words && shown < 80; ++w) {
        HMODULE module = NULL;
        char path[260];
        if (stack[w] < 0x10000 || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)stack[w], &module)) continue;
        const int ours = (uint64_t)module == exe || module == game;
        if (!ours && module == previous) continue;
        previous = module;
        if (!GetModuleFileNameA(module, path, sizeof(path))) continue;
        const char *base = strrchr(path, '\\');
        printf("STALL  %s+%llx\n", base ? base + 1 : path, (unsigned long long)(stack[w] - (uint64_t)module));
        ++shown;
    }
}

/* Everything a stalled game can say about itself: flip state, then every thread's name, CPU use and stack. Each
 * thread is paused only to copy its stack; nothing that takes a lock happens until it runs again. */
static void report_stall(uint64_t flips, const char *why) {
    enum { STACK_WORDS = 128 * 1024 };
    static uint64_t stack[STACK_WORDS];
    static ThreadTime before[PROFILE_THREADS], after[PROFILE_THREADS];
    const DWORD self = GetCurrentThreadId();
    printf("STALL begin: %s, at flip %llu\n", why, (unsigned long long)flips);
    voxgpu_print_video_state();
    const int before_count = read_times(before, self);
    Sleep(2000);
    const int count = read_times(after, self);
    for (int i = 0; i < count; ++i) {
        uint64_t used = after[i].user + after[i].kernel;
        for (int k = 0; k < before_count; ++k)
            if (before[k].id == after[i].id) used -= before[k].user + before[k].kernel;
        const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, after[i].id);
        if (!h) continue;
        size_t words = 0;
        CONTEXT context;
        context.ContextFlags = CONTEXT_CONTROL;
        BOOL got = FALSE;
        if (SuspendThread(h) != (DWORD)-1) {
            got = GetThreadContext(h, &context);
            MEMORY_BASIC_INFORMATION region;
            if (got && VirtualQuery((void *)context.Rsp, &region, sizeof(region))) {
                words = ((uint64_t)region.BaseAddress + region.RegionSize - context.Rsp) / 8;
                if (words > STACK_WORDS) words = STACK_WORDS;
                memcpy(stack, (const void *)context.Rsp, words * 8);
            }
            ResumeThread(h);
        }
        CloseHandle(h);
        printf("STALL thread %u \"%s\" cpu %.1f ms\n", after[i].id, after[i].name, (double)used / 1e4);
        if (got) {
            const uint64_t rip = context.Rip;
            print_stall_stack(&rip, 1);
            print_stall_stack(stack, words);
        }
    }
    puts("STALL end");
}

/* A heartbeat with the renderer's flip count, every five seconds. Flips stopping for two beats gets a stall report,
 * once for each stall. VOX_TEST_STALL=seconds asks for one at that time anyway, to test the report. */
static DWORD WINAPI flip_heartbeat(void *argument) {
    typedef ABI int32_t (*GetFlipStatus)(int32_t handle, uint64_t *status);
    const GetFlipStatus get_status = (GetFlipStatus)voxgpu_resolve("SbU3dwp80lQ");
    if (!get_status) return 0;
    uint64_t last = 0;
    int quiet = 0, reported = 0;
    const int test_at = getenv("VOX_TEST_STALL") ? atoi(getenv("VOX_TEST_STALL")) : 0;
    int elapsed = 0;
    for (;;) {
        Sleep(5000);
        elapsed += 5;
        uint64_t status[8] = {0};
        if (get_status(1, status) != 0) continue;
        printf("BEAT flips %llu, %.1f per second\n", (unsigned long long)status[0], (double)(status[0] - last) / 5.0);
        if (last && status[0] == last) {
            if (stall_event) SetEvent(stall_event);
            ++quiet;
        } else {
            quiet = reported = 0;
        }
        if (quiet >= 2 && !reported) {
            reported = 1;
            report_stall(status[0], "flips stopped");
        }
        if (test_at && elapsed >= test_at && elapsed - 5 < test_at) report_stall(status[0], "VOX_TEST_STALL");
        last = status[0];
    }
    (void)argument;
}

static DWORD WINAPI watchdog(void *argument) {
    Sleep((DWORD)(uintptr_t)argument * 1000);
    rt_finish(41, "watchdog timeout");
}

static SRWLOCK finish_lock = SRWLOCK_INIT;
_Noreturn void rt_finish(int code, const char *why) {
    AcquireSRWLockExclusive(&finish_lock); /* one report; other threads stop here */
    fflush(stdout);
    printf("\n=== STOP: %s\n", why);
    size_t called = 0;
    for (size_t i = 0; i < stub_count; ++i)
        if (stubs[i].calls) ++called;
    printf("unimplemented imports: %zu known, %zu called:\n", stub_count, called);
    for (size_t i = 0; i < stub_count; ++i)
        if (stubs[i].calls) printf("  %8" PRIu64 " %s (%s)\n", stubs[i].calls, stubs[i].symbol ? stubs[i].symbol : "?", stubs[i].nid);
    bbcpu_report();
    fflush(NULL);
    _exit(code);
}

/* ---- start ---- */

#define MAIN_OFFSET 0xd80
#define MAIN_OFFSET_TEXT "d80"

static void reached_main(const uint64_t regs[16], uint64_t address, uint32_t tag) {
    (void)address;
    (void)tag;
    printf("REACHED main: argc=%" PRIu64 " argv[0]=%s\n", regs[7], *(const char **)(uintptr_t)regs[6]);
}

static ABI void guest_exit_callback(void) { puts("Guest: process finalizer callback reached"); }

uint64_t rt_guest_call(const void *fn, int count, const uint64_t *args) {
    return bbcpu_call((uintptr_t)fn, count, args);
}

int main(int argc, char **argv) {
    SetThreadDescription(GetCurrentThread(), L"vox:main");
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGABRT, on_abort);
    if (argc < 3) {
        fprintf(stderr, "usage: vox ELF_DIR NAMES.TSV [--timeout seconds] [--english | --user-tl]\n");
        return 2;
    }
    unsigned timeout = 60;
    const char *app0 = ".", *user = "user";
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--app0") && i + 1 < argc) app0 = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) user = argv[++i];
        else if (!strcmp(argv[i], "--english")) rt_overlay_folder = "translation";
        else if (!strcmp(argv[i], "--user-tl")) rt_overlay_folder = "translation_user";
    }
    rt_set_folders(app0, user);
    AddVectoredExceptionHandler(1, on_fault);
    load_names(argv[2]);
    printf("Names: %zu\n", name_count);

    image = VirtualAlloc((void *)(uintptr_t)IMAGE_AT, IMAGE_MAX, MEM_RESERVE, PAGE_READWRITE);
    if (!image) fail("%s", "cannot reserve the image");
    if (!VirtualAlloc(image, IMAGE_MAX, MEM_COMMIT, PAGE_READWRITE)) fail("%s", "cannot commit the image");
    bb_image_base = (uint64_t)(uintptr_t)image;
    voxgpu_notify_map(bb_image_base, IMAGE_MAX);
    stub_code = VirtualAlloc(NULL, (size_t)STUB_MAX * STUB_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!stub_code) fail("%s", "cannot allocate import stubs");

    rt_runtime_init();
    place_module(argv[1], "eboot");
    place_module(argv[1], "libc");
    place_module(argv[1], "libSceFios2");
    qsort(exports, export_count, sizeof(*exports), export_order);

    gpu_enabled = !(getenv("VOX_NO_GPU") && getenv("VOX_NO_GPU")[0] == '1');
    if (gpu_enabled) {
        /* Before relocation: this registers the functions imports resolve to. SDK version and
         * attributes: the game's procparam (+0x10) and param.sfo's ATTRIBUTE. */
        const Phdr *proc = find_ph(&modules[0], 0x61000001);
        uint64_t sdk = 0;
        if (proc) memcpy(&sdk, image + proc->vaddr + 16, 8);
        voxgpu_register_kernel();
        const VoxGpuConfig config = {"A Certain Magical Virtual-On", "CUSA08425", user, (uint32_t)sdk, 0x00800000, 1920, 1080};
        const int result = voxgpu_init(&config);
        printf("Renderer: SDK %#x, voxgpu_init %d\n", (unsigned)sdk, result);
        if (result) fail("%s", "renderer initialization failed");
    }
    for (int i = 0; i < module_count; ++i) relocate(&modules[i]);
    printf("Image at %#" PRIx64 ", %#" PRIx64 " bytes; %zu exports; %zu imports have no implementation\n", bb_image_base, image_used,
           export_count, stub_count);

    for (int i = 0; i < module_count; ++i)
        for (int k = 0; k < modules[i].phnum; ++k) {
            const Phdr *p = &modules[i].ph[k];
            if (p->type == 1 && (p->flags & 1)) bbcpu_add_guest_code((uintptr_t)(image + modules[i].base + p->vaddr), p->memsz);
        }

    const Phdr *param = find_ph(&modules[0], 0x61000001);
    if (!param) fail("%s", "no process parameters");
    rt_set_procparam(image + param->vaddr);

    /* The game's main (from _start's code): left to the interpreter so that this probe sees the call,
     * which a recompiled caller would make directly. */
    if (!getenv("BB_RECOMP_OFF")) _putenv("BB_RECOMP_OFF=0x" MAIN_OFFSET_TEXT);
    bbcpu_set_probe(bb_image_base + MAIN_OFFSET, reached_main, 0);
    if (!getenv("BB_RECOMP_LIB")) puts("BB_RECOMP_LIB not set: everything runs in the interpreter");
    bbcpu_recomp_load(bb_image_base);
    if (timeout) CreateThread(NULL, 0, watchdog, (void *)(uintptr_t)timeout, 0, NULL);
    stall_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (gpu_enabled) CreateThread(NULL, 0, flip_heartbeat, NULL, 0, NULL);
    if (getenv("VOX_PROFILE")) CreateThread(NULL, 0, profiler, (void *)(uintptr_t)atoi(getenv("VOX_PROFILE")), 0, NULL);

    rt_thread_attach();
    for (int i = 1; i < module_count; ++i) {
        printf("Starting %s at +%#" PRIx64 "\n", modules[i].name, modules[i].base + modules[i].init);
        const uint64_t args[3] = {0, 0, 0};
        const int result = (int)rt_guest_call(image + modules[i].base + modules[i].init, 3, args);
        printf("%s initializer returned %d\n", modules[i].name, result);
        if (result) rt_finish(42, "module initializer failed");
    }

    enum { MAIN_STACK = 8 * 1024 * 1024 };
    uint8_t *stack = runtime_low_map(MAIN_STACK, 3);
    if (!stack) fail("%s", "cannot allocate the guest stack");
    static const char arg0[] = "/app0/eboot.bin";
    struct { uint64_t argc; const char *argv[2]; } *params = rt_low_alloc(sizeof(*params));
    params->argc = 1;
    params->argv[0] = arg0;
    params->argv[1] = NULL;
    printf("Entering the game at +%#" PRIx64 "\n", modules[0].entry);
    const uint64_t args[2] = {(uint64_t)(uintptr_t)params, (uint64_t)(uintptr_t)guest_exit_callback};
    bbcpu_call_on_stack((uintptr_t)(image + modules[0].entry), 2, args, stack, MAIN_STACK - 64);
    rt_finish(0, "the game's entry returned");
}
