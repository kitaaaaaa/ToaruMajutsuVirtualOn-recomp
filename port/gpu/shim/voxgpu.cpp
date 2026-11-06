// vox: glue between the C runtime and the vendored shadPS4 video core (the C API in voxgpu.h, the
// kernel services the vendored libraries call directly, and what else lives outside them upstream).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <boost/asio/io_context.hpp>
#include <SDL3/SDL.h>
#include <windows.h>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/polyfill_thread.h"
#include "common/rdtsc.h"
#include "common/thread.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderdoc.h"
#include "voxgpu.h"

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
}
namespace Libraries::VideoOut {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
}
namespace Libraries::Kernel {
void RegisterEventQueue(Core::Loader::SymbolsResolver* sym);
}

namespace {
std::unordered_map<std::string, u64> g_symbols;
std::mutex g_symbols_mutex;
s32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char*, const char*, SymbolType,
                                u64 address) {
    std::scoped_lock lock{g_symbols_mutex};
    g_symbols.try_emplace(nid, address);
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; they're filled here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const VoxGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};
} // namespace Core

// Kernel services the vendored libraries call directly: the emulator's kernel library isn't here.
namespace Libraries::Kernel {
namespace {
u64 CounterNow() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}
u64 CounterFrequency() {
    static const u64 frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<u64>(value.QuadPart);
    }();
    return frequency;
}
const u64 g_process_start = CounterNow();
} // namespace

u64 PS4_SYSV_ABI sceKernelGetTscFrequency() {
    static const u64 frequency = Common::EstimateRDTSCFrequency();
    return frequency;
}
u64 PS4_SYSV_ABI sceKernelReadTsc() {
    return Common::FencedRDTSC();
}
u64 PS4_SYSV_ABI sceKernelGetProcessTime() {
    const u64 elapsed = CounterNow() - g_process_start;
    const u64 frequency = CounterFrequency();
    return elapsed / frequency * 1'000'000 + elapsed % frequency * 1'000'000 / frequency;
}
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() {
    return CounterNow() - g_process_start;
}
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() {
    return CounterFrequency();
}
s32 PS4_SYSV_ABI sceKernelIsNeoMode() {
    return 0; // base PS4
}
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    *ver = g_sdk_version;
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
// The vendored libraries' own errno, apart from the runtime's guest errno.
int* PS4_SYSV_ABI __Error() {
    thread_local int error = 0;
    return &error;
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("vox:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) {
            break;
        }
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks (renderdoc.cpp isn't built) and screenshots (no hotkey to ask for one).
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() {
    return false;
}
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() {
    return 0;
}
u32 ConsumeWithOverlaysScreenshotRequests() {
    return 0;
}
ScreenshotRequests ConsumeScreenshotRequests() {
    return {};
}
} // namespace VideoCore

extern "C" void voxgpu_register_kernel(void) {
    using namespace Libraries::Kernel;
    if (!service_thread.joinable()) {
        service_thread = std::jthread{KernelServiceThread};
    }
    Core::Loader::SymbolsResolver resolver;
    RegisterEventQueue(&resolver);
}

static void SetUserPaths(const char* user_dir) {
    using namespace Common::FS;
    const std::filesystem::path base{user_dir};
    std::error_code error;
    const auto set_path = [&](PathType type, const std::filesystem::path& path) {
        std::filesystem::create_directories(path, error);
        SetUserPath(type, path);
    };
    set_path(PathType::UserDir, base);
    set_path(PathType::ShaderDir, base / SHADER_DIR);
    set_path(PathType::CacheDir, base / CACHE_DIR);
}

extern "C" int voxgpu_init(const VoxGpuConfig* config) {
    if (!config) {
        return -1;
    }
    g_sdk_version = static_cast<s32>(config->sdk_version);
    if (config->user_dir) {
        SetUserPaths(config->user_dir);
    }
    Core::Emulator::FillElfInfo(*config);

    const std::string title = config->title ? config->title : "vox";
    const s32 width = config->width > 0 ? config->width : 1280;
    const s32 height = config->height > 0 ? config->height : 720;

    // SDL video must be driven from one thread: the window lives on its own host thread.
    static std::mutex mutex;
    static std::condition_variable cv;
    static bool ready = false;
    static bool valid = false;
    std::thread window_thread([title, width, height] {
        Common::SetCurrentThreadName("vox:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        const bool window_valid = window->IsValid();
        {
            std::scoped_lock lock{mutex};
            g_window = window;
            valid = window_valid;
            ready = true;
        }
        cv.notify_all();
        if (!window_valid) {
            return;
        }
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        LOG_INFO(Frontend, "Window closed by the user");
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 0);
    });
    window_thread.detach();
    {
        std::unique_lock lock{mutex};
        cv.wait(lock, [&] { return ready; });
    }
    if (!valid) {
        return -2;
    }

    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

extern "C" uintptr_t voxgpu_resolve(const char* nid) {
    const char* hash = std::strchr(nid, '#');
    const std::string key = hash ? std::string{nid, hash} : std::string{nid};
    std::scoped_lock lock{g_symbols_mutex};
    const auto it = g_symbols.find(key);
    return it == g_symbols.end() ? 0 : static_cast<uintptr_t>(it->second);
}

extern "C" int voxgpu_handle_fault(void* context, void* address) {
    return Core::Signals::Instance()->DispatchAccessViolation(context, address) ? 1 : 0;
}
