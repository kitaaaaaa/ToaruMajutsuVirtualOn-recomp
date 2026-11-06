/* PS4 system functions on Windows: the imports nothing in the game's own modules provides. Only what
 * has been checked to be needed, each from what the PS4 function is known to do (shadPS4 and
 * bloodborne-runtime, both GPL-2.0-or-later, as references). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/bbcpu.h"
#include "hle.h"

void rt_kernel_init(void);

/* ---- variables the game imports ---- */

typedef struct { uint64_t stack_guard; int32_t need_libc_internal; } ImportedData;
static ImportedData *imported;

/* ---- TLS: static TLS below the TCB (the eboot has none), dynamic TLS by module id ---- */

enum { TLS_MODULES = 16 };
static struct { const void *data; uint64_t filesz, memsz; } tls_modules[TLS_MODULES];
static _Thread_local uint8_t *tls_blocks[TLS_MODULES];

void rt_set_tls_module(uint64_t id, const void *data, uint64_t filesz, uint64_t memsz) {
    if (id >= TLS_MODULES) rt_finish(21, "too many TLS modules");
    tls_modules[id].data = data;
    tls_modules[id].filesz = filesz;
    tls_modules[id].memsz = memsz;
}

static ABI void *guest_tls_get_addr(const uint64_t *index) {
    if (!index || index[0] >= TLS_MODULES || !tls_modules[index[0]].data || index[1] >= tls_modules[index[0]].memsz)
        rt_finish(21, "__tls_get_addr: unknown module or offset");
    uint8_t **block = &tls_blocks[index[0]];
    if (!*block) {
        *block = rt_low_alloc(tls_modules[index[0]].memsz);
        memcpy(*block, tls_modules[index[0]].data, tls_modules[index[0]].filesz);
    }
    return *block + index[1];
}

/* ---- process ---- */

static void *process_param;
void rt_set_procparam(void *param) { process_param = param; }
static ABI void *guest_get_procparam(void) { return process_param; }

static void **application_heap_api;
static ABI void guest_set_heap_api(void **api) {
    application_heap_api = api;
    puts("HLE: application heap API registered");
}

static ABI _Noreturn void guest_stack_chk_fail(void) { rt_finish(22, "guest stack protector found corruption"); }

/* libc's heap asks where to keep its tracing state (shadPS4's libc_internal_memory.cpp). */
typedef struct { uint64_t size; uint32_t flag, get_segment_info; uint64_t *atomic_id_mask, *state_table; } HeapTraceInfo;
static ABI void heap_get_trace_info(HeapTraceInfo *info) {
    static uint64_t *mask, *table;
    if (!mask) {
        mask = rt_low_alloc(sizeof(uint64_t));
        table = rt_low_alloc(64 * sizeof(uint64_t));
    }
    info->atomic_id_mask = mask;
    info->state_table = table;
    info->get_segment_info = 0;
}

/* ---- the table ---- */

static const RtExport functions[] = {
    {"__stack_chk_fail", guest_stack_chk_fail},
    {"__tls_get_addr", guest_tls_get_addr},
    {"sceKernelGetProcParam", guest_get_procparam},
    {"_sceKernelRtldSetApplicationHeapAPI", guest_set_heap_api},
    {"sceLibcHeapGetTraceInfo", heap_get_trace_info},
};

void rt_runtime_init(void) {
    rt_kernel_init();
    rt_video_init();
    imported = rt_low_alloc(sizeof(*imported));
    unsigned halves[2];
    if (rand_s(&halves[0]) || rand_s(&halves[1])) rt_finish(1, "cannot make a stack guard");
    memcpy(&imported->stack_guard, halves, 8);
    imported->stack_guard &= ~UINT64_C(255);
    imported->need_libc_internal = 1;
}

uintptr_t rt_resolve(const char *nid, const char *symbol, int is_data) {
    if (is_data) {
        if (!strcmp(nid, "f7uOxY9mM1U")) return (uintptr_t)&imported->stack_guard;
        if (!strcmp(nid, "ZT4ODD2Ts9o")) return (uintptr_t)&imported->need_libc_internal;
        return 0;
    }
    if (!symbol) return 0;
    for (size_t i = 0; i < sizeof(functions) / sizeof(*functions); ++i)
        if (!strcmp(symbol, functions[i].name)) return (uintptr_t)functions[i].function;
    const struct { const RtExport *table; size_t count; } tables[] = {
        {rt_thread_exports, rt_thread_export_count},
        {rt_memory_exports, rt_memory_export_count},
        {rt_kernel_exports, rt_kernel_export_count},
        {rt_video_exports, rt_video_export_count},
        {rt_gnm_exports, rt_gnm_export_count},
        {rt_service_exports, rt_service_export_count},
        {rt_io_exports, rt_io_export_count},
    };
    for (size_t t = 0; t < sizeof(tables) / sizeof(*tables); ++t)
        for (size_t i = 0; i < tables[t].count; ++i)
            if (!strcmp(symbol, tables[t].table[i].name)) return (uintptr_t)tables[t].table[i].function;
    return 0;
}
