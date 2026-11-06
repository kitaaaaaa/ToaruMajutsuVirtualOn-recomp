// vox: the game window on SDL3. Created and pumped on voxgpu's window thread (voxgpu.cpp).
#pragma once
#include <atomic>
#include "common/types.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 { Headless, Windows, X11, Wayland, Metal };

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    WindowSDL(s32 width, s32 height, const char* title);
    ~WindowSDL();
    /// The window exists and has a Win32 handle.
    bool IsValid() const {
        return window != nullptr && window_info.render_surface != nullptr;
    }
    s32 GetWidth() const {
        return width.load(std::memory_order_relaxed);
    }
    s32 GetHeight() const {
        return height.load(std::memory_order_relaxed);
    }
    SDL_Window* GetSDLWindow() const {
        return window;
    }
    WindowSystemInfo GetWindowInfo() const {
        return window_info;
    }
    bool IsOpen() const {
        return is_open.load(std::memory_order_relaxed);
    }
    /// Processes pending window events. Returns false once the user closed the window.
    bool PollEvents();

private:
    std::atomic<s32> width, height;
    std::atomic<bool> is_open{true};
    SDL_Window* window{};
    WindowSystemInfo window_info{};
};

} // namespace Frontend
