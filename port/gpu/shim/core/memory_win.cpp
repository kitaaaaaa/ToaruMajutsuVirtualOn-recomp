// vox: Core::MemoryManager and Core::AddressSpace for Windows.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <windows.h>
#include "common/logging/log.h"
#include "core/memory.h"

namespace Core {
namespace {

// Serializes page tracking's protection changes with TryWriteBacking's temporary ones: a change
// landing between its unprotect and its restore would be undone by the restore.
std::mutex protection_mutex;

constexpr DWORD ToWindows(MemoryPermission perms) {
    const bool read = True(perms & MemoryPermission::Read);
    const bool write = True(perms & MemoryPermission::Write);
    if (True(perms & MemoryPermission::Execute)) {
        return write ? PAGE_EXECUTE_READWRITE : read ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    }
    return write ? PAGE_READWRITE : read ? PAGE_READONLY : PAGE_NOACCESS;
}

/// Guest mappings, as the runtime reports them (voxgpu_notify_map): start to end, not overlapping.
/// VirtualQuery can't answer these on hot paths: it scans page tables to find a region's extent,
/// milliseconds per call on a mapping of gigabytes, which held the game at about 1 frame a second.
std::map<VAddr, VAddr> mappings;
std::shared_mutex mappings_mutex;

/// How far the mapped range holding `address` reaches, or 0 when it isn't mapped. Lock held.
VAddr MappedEnd(VAddr address) {
    auto it = mappings.upper_bound(address);
    if (it == mappings.begin()) {
        return 0;
    }
    --it;
    return address < it->second ? it->second : 0;
}

/// The next mapped address after `address`, or 0.
VAddr NextMapped(VAddr address) {
    const auto it = mappings.upper_bound(address);
    return it == mappings.end() ? 0 : it->first;
}

void RemoveMapping(VAddr start, VAddr end) {
    auto it = mappings.upper_bound(start);
    if (it != mappings.begin()) {
        --it;
    }
    while (it != mappings.end() && it->first < end) {
        const VAddr a = it->first, b = it->second;
        if (b <= start) {
            ++it;
            continue;
        }
        it = mappings.erase(it);
        if (a < start) {
            mappings[a] = start;
        }
        if (b > end) {
            mappings[end] = b;
            break;
        }
    }
}

} // namespace

void NotifyMap(VAddr address, u64 size) {
    std::unique_lock lock{mappings_mutex};
    RemoveMapping(address, address + size);
    mappings[address] = address + size;
}

void NotifyUnmap(VAddr address, u64 size) {
    std::unique_lock lock{mappings_mutex};
    RemoveMapping(address, address + size);
}

bool MemoryManager::IsValidMapping(VAddr virtual_addr, u64 size) const {
    const u64 wanted = std::max<u64>(size, 1);
    return ClampRangeSize(virtual_addr, wanted) == wanted;
}

u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) const {
    std::shared_lock lock{mappings_mutex};
    u64 found = 0;
    while (found < size) {
        const VAddr end = MappedEnd(virtual_addr + found);
        if (!end) {
            break;
        }
        found = std::min<u64>(size, end - virtual_addr);
    }
    return found;
}

void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) const {
    std::shared_lock lock{mappings_mutex};
    while (size) {
        const VAddr end = MappedEnd(source);
        if (end) {
            const u64 chunk = std::min<u64>(size, end - source);
            std::memcpy(dest, reinterpret_cast<const void*>(source), chunk);
            source += chunk;
            dest += chunk;
            size -= chunk;
        } else {
            const VAddr next = NextMapped(source);
            const u64 chunk = next ? std::min<u64>(size, next - source) : size;
            std::memset(dest, 0, chunk);
            source += chunk;
            dest += chunk;
            size -= chunk;
        }
    }
}

bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    constexpr u64 HostPageSize = 0x1000;
    auto current = reinterpret_cast<VAddr>(address);
    auto* from = static_cast<const u8*>(data);

    if (!IsValidMapping(current, size)) {
        return false;
    }
    std::scoped_lock lock{protection_mutex};
    while (size) {
        // A page at a time: its old protection comes back exactly.
        const VAddr page = current & ~(HostPageSize - 1);
        const u64 chunk = std::min<u64>(size, page + HostPageSize - current);
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(page), HostPageSize, PAGE_READWRITE, &old)) {
            LOG_ERROR(Common_Memory, "TryWriteBacking: can't unprotect {:#x}: {}", page, GetLastError());
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(current), from, chunk);
        if (old != PAGE_READWRITE) {
            DWORD ignored;
            VirtualProtect(reinterpret_cast<void*>(page), HostPageSize, old, &ignored);
        }
        current += chunk;
        from += chunk;
        size -= chunk;
    }
    return true;
}

void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    const DWORD protect = ToWindows(perms);
    std::scoped_lock lock{protection_mutex};
    DWORD old;
    if (VirtualProtect(reinterpret_cast<void*>(virtual_addr), size, protect, &old)) {
        return;
    }
    // One call can't span separate allocations (the runtime's guest mappings may be adjacent ones)
    // or anything unmapped.
    std::shared_lock map_lock{mappings_mutex};
    const VAddr end = virtual_addr + size;
    while (virtual_addr < end) {
        const VAddr mapped_end = MappedEnd(virtual_addr);
        if (!mapped_end) {
            const VAddr next = NextMapped(virtual_addr);
            if (!next) {
                return;
            }
            virtual_addr = next;
            continue;
        }
        const VAddr next = std::min<VAddr>(end, mapped_end);
        if (!VirtualProtect(reinterpret_cast<void*>(virtual_addr), next - virtual_addr, protect, &old)) {
            LOG_ERROR(Common_Memory, "Protect {:#x}+{:#x} failed: {}", virtual_addr,
                      next - virtual_addr, GetLastError());
        }
        virtual_addr = next;
    }
}

u8* const* BackingPageTable::find(u64 page) const {
    thread_local u8* entry;
    const VAddr begin = page << GUEST_PAGE_BITS;
    const VAddr end = begin + GUEST_PAGE_SIZE;
    std::shared_lock lock{mappings_mutex};
    entry = reinterpret_cast<u8*>(begin);
    for (VAddr current = begin; current < end;) {
        const VAddr mapped_end = MappedEnd(current);
        if (!mapped_end) {
            entry = nullptr;
            break;
        }
        current = mapped_end;
    }
    return &entry;
}

} // namespace Core

extern "C" void voxgpu_notify_map(uint64_t address, uint64_t size) {
    Core::NotifyMap(address, size);
}

extern "C" void voxgpu_notify_unmap(uint64_t address, uint64_t size) {
    Core::NotifyUnmap(address, size);
}
