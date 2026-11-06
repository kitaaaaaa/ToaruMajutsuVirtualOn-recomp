// vox: the MemoryManager surface used by the video core and GnmDriver. Guest memory is plain
// VirtualAlloc'd memory owned by the runtime; the answers come from VirtualQuery.
#pragma once
#include "common/singleton.h"
#include "common/types.h"
#include "core/address_space.h"

namespace Vulkan {
class Rasterizer;
}

namespace Core {

class MemoryManager {
public:
    void SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }
    Vulkan::Rasterizer* GetRasterizer() const {
        return rasterizer;
    }
    AddressSpace& GetAddressSpace() {
        return impl;
    }
    bool IsValidGpuMapping(VAddr virtual_addr, u64 size = 0) const {
        // The PS4's GPU can only handle 40 bit addresses.
        return virtual_addr + size < 0x100'0000'0000ULL;
    }
    /// The whole range is committed memory.
    bool IsValidMapping(VAddr virtual_addr, u64 size = 0) const;
    /// How much of the range, from its start, is committed memory.
    u64 ClampRangeSize(VAddr virtual_addr, u64 size) const;
    /// Copies the range; what isn't readable reads as zeros (buffers extend past mappings).
    void CopySparseMemory(VAddr source, u8* dest, u64 size) const;
    /// Writes guest memory past page tracking's protection. False if it isn't all committed.
    bool TryWriteBacking(void* address, const void* data, u64 size);

private:
    AddressSpace impl;
    Vulkan::Rasterizer* rasterizer{};
};
using Memory = Common::Singleton<MemoryManager>;

} // namespace Core
