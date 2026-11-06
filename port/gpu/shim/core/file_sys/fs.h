// vox: equeue.cpp's use of the file table. The emulator's file system isn't here (the runtime owns
// guest file descriptors), so the table holds only the event queues' own handles: read, write and
// vnode events on any other descriptor report EBADF.
#pragma once
#include <memory>
#include <mutex>
#include <unordered_map>
#include "common/types.h"

namespace Core::FileSys {

enum class FileType { Regular, Socket, Equeue };

struct HostFile {
    u64 Size() const { return 0; }
    u64 Tell() const { return 0; }
};

struct File {
    FileType type{FileType::Equeue};
    std::unique_ptr<HostFile> handle;
};

class HandleTable {
public:
    s32 CreateHandle() {
        std::scoped_lock lock{mutex};
        const s32 handle = next_handle++;
        files[handle] = std::make_unique<File>();
        return handle;
    }
    File* GetFile(s32 handle) {
        std::scoped_lock lock{mutex};
        const auto it = files.find(handle);
        return it == files.end() ? nullptr : it->second.get();
    }
    void DeleteHandle(s32 handle) {
        std::scoped_lock lock{mutex};
        files.erase(handle);
    }

private:
    std::mutex mutex;
    std::unordered_map<s32, std::unique_ptr<File>> files;
    s32 next_handle{0x4000}; // apart from the runtime's descriptors
};

} // namespace Core::FileSys
