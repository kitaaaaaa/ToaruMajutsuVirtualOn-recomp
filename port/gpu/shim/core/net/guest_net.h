// vox: no network here; equeue.cpp's socket events never see a socket (core/file_sys/fs.h).
#pragma once
#include <span>
#include "common/types.h"

namespace Core::Net {

namespace Host {
enum class Error { Ok, BadF };
constexpr u32 EvIn = 1;
constexpr u32 EvOut = 2;
} // namespace Host

struct SelectEntry {
    s32 id;
    u32 interest;
    u32 ready;
};

struct NetResult {
    Host::Error error;
    s64 value;
};

inline NetResult SocketBytesReadable(s32) {
    return {Host::Error::BadF, 0};
}
inline NetResult SocketSelect(std::span<SelectEntry>, int) {
    return {Host::Error::BadF, 0};
}

} // namespace Core::Net
