/* PS4 memory calls on Windows. Virtual addresses stay below 1 TiB. Direct memory is only a counter of
 * "physical" offsets: mapping it commits ordinary memory (no aliasing yet; the GPU work may need it). */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "hle.h"
#include "voxgpu.h"

enum { PAGE_PS4 = 0x4000 };
#define MAP_START UINT64_C(0x2000000000)
#define MAP_END UINT64_C(0xE000000000)
#define DIRECT_MEMORY_SIZE (UINT64_C(5056) * 1024 * 1024)

static SRWLOCK lock = SRWLOCK_INIT;
static uint64_t next_address = MAP_START;
static uint64_t direct_next;

static DWORD win_protection(int prot) {
    /* PS4: 1 read, 2 write, 4 execute; 0x10 GPU read, 0x20 GPU write. Guest code never runs natively. */
    if (prot & (2 | 0x20)) return PAGE_READWRITE;
    if (prot & (1 | 4 | 0x10)) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

/* Maps at `wanted` when given (and free), else at the next free aligned address. */
static void *map_memory(uint64_t wanted, uint64_t size, int prot, uint64_t alignment) {
    if (alignment < 0x10000) alignment = 0x10000;
    const uint64_t span = (size + 0xffff) & ~UINT64_C(0xffff);
    const DWORD mode = win_protection(prot);
    if (wanted) {
        const uint64_t start = wanted & ~UINT64_C(0xffff);
        void *p = VirtualAlloc((void *)(uintptr_t)start, span, MEM_RESERVE | MEM_COMMIT, mode);
        if (p) {
            voxgpu_notify_map(start, span);
            return (void *)(uintptr_t)wanted;
        }
    }
    AcquireSRWLockExclusive(&lock);
    void *result = NULL;
    uint64_t at = (next_address + alignment - 1) & ~(alignment - 1);
    while (at + span < MAP_END) {
        result = VirtualAlloc((void *)(uintptr_t)at, span, MEM_RESERVE | MEM_COMMIT, mode);
        if (result) {
            next_address = at + span;
            break;
        }
        at = (at + span + alignment - 1) & ~(alignment - 1);
    }
    ReleaseSRWLockExclusive(&lock);
    if (result) voxgpu_notify_map((uint64_t)(uintptr_t)result, span);
    return result;
}

static ABI uint64_t direct_size(void) { return DIRECT_MEMORY_SIZE; }

static ABI int32_t direct_allocate(int64_t start, int64_t end, uint64_t size, uint64_t alignment, int32_t type, int64_t *out) {
    (void)end; (void)type;
    if (!out || !size) return ORBIS_ERR(E_INVAL);
    if (!alignment) alignment = PAGE_PS4;
    AcquireSRWLockExclusive(&lock);
    uint64_t at = direct_next > (uint64_t)start ? direct_next : (uint64_t)start;
    at = (at + alignment - 1) & ~(alignment - 1);
    const int fits = at + size <= DIRECT_MEMORY_SIZE;
    if (fits) direct_next = at + size;
    ReleaseSRWLockExclusive(&lock);
    if (!fits) return ORBIS_ERR(E_AGAIN);
    *out = (int64_t)at;
    printf("HLE: direct memory %#llx bytes at %#llx\n", (unsigned long long)size, (unsigned long long)at);
    return 0;
}

static ABI int32_t direct_map(void **address, uint64_t size, int32_t prot, int32_t flags, int64_t physical, uint64_t alignment) {
    (void)flags; (void)physical;
    if (!address || !size) return ORBIS_ERR(E_INVAL);
    void *p = map_memory((uint64_t)(uintptr_t)*address, size, prot, alignment);
    if (!p) return ORBIS_ERR(E_NOMEM);
    *address = p;
    return 0;
}

static ABI int32_t direct_release(int64_t start, uint64_t size) { (void)start; (void)size; return 0; }

static ABI int32_t flexible_map(void **address, uint64_t size, int32_t prot, int32_t flags, const char *name) {
    (void)flags;
    if (!address || !size) return ORBIS_ERR(E_INVAL);
    void *p = map_memory((uint64_t)(uintptr_t)*address, size, prot, 0);
    if (!p) return ORBIS_ERR(E_NOMEM);
    *address = p;
    printf("HLE: flexible memory \"%s\" %#llx bytes at %p\n", name ? name : "", (unsigned long long)size, p);
    return 0;
}

/* POSIX mmap: anonymous memory only (flag 0x1000); -1 with errno otherwise. */
static ABI void *posix_mmap(void *address, uint64_t size, int32_t prot, int32_t flags, int32_t fd, int64_t offset) {
    (void)offset;
    if (!size || (fd != -1 && !(flags & 0x1000))) return (void *)-1;
    void *p = map_memory((uint64_t)(uintptr_t)address, size, prot, 0);
    return p ? p : (void *)-1;
}

static ABI int32_t kernel_munmap(void *address, uint64_t size) {
    MEMORY_BASIC_INFORMATION info;
    if (!address || !VirtualQuery(address, &info, sizeof(info))) return ORBIS_ERR(E_INVAL);
    if (info.State == MEM_FREE) return 0;
    voxgpu_notify_unmap((uint64_t)(uintptr_t)address, size);
    if (info.AllocationBase == address && size >= info.RegionSize)
        return VirtualFree(address, 0, MEM_RELEASE) ? 0 : ORBIS_ERR(E_INVAL);
    return VirtualFree(address, size, MEM_DECOMMIT) ? 0 : ORBIS_ERR(E_INVAL);
}

static ABI int32_t kernel_mlock(const void *address, uint64_t size) { (void)address; (void)size; return 0; }
static ABI int32_t posix_msync(void *address, uint64_t size, int32_t flags) { (void)address; (void)size; (void)flags; return 0; }

/* OrbisVirtualQueryInfo */
typedef struct {
    uint64_t start, end, offset;
    int32_t protection, memory_type;
    uint32_t flags; /* bit 0 flexible, 1 direct, 2 stack, 3 pooled, 4 committed */
    char name[32];
} QueryInfo;

static ABI int32_t virtual_query(const void *address, int32_t flags, QueryInfo *out, uint64_t size) {
    if (!out || size < sizeof(QueryInfo)) return ORBIS_ERR(E_INVAL);
    MEMORY_BASIC_INFORMATION info;
    const uint8_t *at = address;
    while (VirtualQuery(at, &info, sizeof(info)) && info.State == MEM_FREE) {
        if (!(flags & 1)) return ORBIS_ERR(E_ACCES); /* SCE_KERNEL_VQ_FIND_NEXT */
        at = (const uint8_t *)info.BaseAddress + info.RegionSize;
        if ((uint64_t)(uintptr_t)at >= MAP_END) return ORBIS_ERR(E_ACCES);
    }
    memset(out, 0, sizeof(*out));
    out->start = (uint64_t)(uintptr_t)info.BaseAddress;
    out->end = out->start + info.RegionSize;
    out->protection = info.Protect == PAGE_READWRITE ? 3 : info.Protect == PAGE_READONLY ? 1 : 0;
    out->flags = 1 | (info.State == MEM_COMMIT ? 16 : 0);
    return 0;
}

static ABI int32_t page_size(void) { return PAGE_PS4; }

const RtExport rt_memory_exports[] = {
    {"sceKernelGetDirectMemorySize", direct_size}, {"sceKernelAllocateDirectMemory", direct_allocate},
    {"sceKernelMapDirectMemory", direct_map}, {"sceKernelReleaseDirectMemory", direct_release},
    {"sceKernelMapNamedFlexibleMemory", flexible_map}, {"mmap", posix_mmap}, {"sceKernelMunmap", kernel_munmap},
    {"sceKernelMlock", kernel_mlock}, {"msync", posix_msync}, {"sceKernelVirtualQuery", virtual_query},
    {"getpagesize", page_size},
};
const size_t rt_memory_export_count = sizeof(rt_memory_exports) / sizeof(*rt_memory_exports);
