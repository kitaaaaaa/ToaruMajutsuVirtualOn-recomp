// vox: the address-space operations the video core needs, on plain VirtualAlloc'd guest memory
// (guest pointers are host pointers, identity mapped below 1 TiB).
#pragma once
#include "common/enum.h"
#include "common/types.h"

namespace Core {

static constexpr u64 GUEST_PAGE_BITS = 14;
static constexpr u64 GUEST_PAGE_SIZE = 1ULL << GUEST_PAGE_BITS;

enum class MemoryPermission : u32 {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
    ReadWriteExecute = Read | Write | Execute,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

/// Stands in for upstream's table of backing pages: guest memory has no separate backing, so the
/// entry for a guest page is its own address, or null while the page can't be read in full.
class BackingPageTable {
public:
    u8* const* find(u64 page) const;
};

class AddressSpace {
public:
    /// VirtualProtect. Page tracking's permission changes; parts not committed are skipped.
    void Protect(VAddr virtual_addr, u64 size, MemoryPermission perms);
    const BackingPageTable& BackingPages() const noexcept {
        return backing_pages;
    }
    /// Only an address for the tessellation factor ring (sceGnmGetTheTessellationFactorRing...).
    VAddr SystemReservedVirtualBase() const noexcept {
        return 0x7'0000'0000ULL;
    }
    u64 SystemReservedVirtualSize() const noexcept {
        return 0x1'0000'0000ULL;
    }

private:
    BackingPageTable backing_pages;
};

} // namespace Core
