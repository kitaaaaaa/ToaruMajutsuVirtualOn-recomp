/* The Windows host for the recompiled game: the loader (loader.c) and the PS4 system functions (hle*.c). */
#ifndef VOX_RUNTIME_H
#define VOX_RUNTIME_H
#include <stddef.h>
#include <stdint.h>

/* PS4 code calls with the System V convention; every function it reaches must take it. */
#define ABI __attribute__((sysv_abi))

/* What a system function's name resolves to. */
typedef struct { const char *name; const void *function; } RtExport;

/* hle.c: the host function or variable for an import, or 0. */
uintptr_t rt_resolve(const char *nid, const char *symbol, int is_data);
void rt_runtime_init(void);

/* Low addresses (guest pointers may be packed into 40 bits), zeroed, never freed. */
void *rt_low_alloc(size_t size);

/* Guest thread state: call on each thread before it runs guest code. */
void rt_thread_attach(void);
void rt_set_tls_module(uint64_t id, const void *data, uint64_t filesz, uint64_t memsz);
void rt_set_procparam(void *param);
void rt_set_folders(const char *app0, const char *user);

/* Calls a guest function on the current thread. */
uint64_t rt_guest_call(const void *fn, int count, const uint64_t *args);

/* Prints what was called, then ends the process. */
_Noreturn void rt_finish(int code, const char *why);
#endif
