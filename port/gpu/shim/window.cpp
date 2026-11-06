// vox: Frontend::WindowSDL on SDL3 for Windows: a window, the Win32 handle for the Vulkan surface,
// resizes and quitting. No input yet.
#include <SDL3/SDL.h>
#include "common/logging/log.h"
#include "sdl_window.h"

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title) : width{width_}, height{height_} {
    SDL_SetHint(SDL_HINT_APP_NAME, "vox");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_CRITICAL(Frontend, "SDL_Init: {}", SDL_GetError());
        return;
    }
    window = SDL_CreateWindow(title, width_, height_, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        LOG_CRITICAL(Frontend, "SDL_CreateWindow: {}", SDL_GetError());
        return;
    }
    SDL_SetWindowMinimumSize(window, 640, 360);

    window_info.type = WindowSystemType::Windows;
    window_info.render_surface = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    window_info.render_surface_scale = SDL_GetWindowDisplayScale(window);

    int pixel_width = 0, pixel_height = 0;
    if (SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height) && pixel_width > 0 &&
        pixel_height > 0) {
        width = pixel_width;
        height = pixel_height;
    }
}

WindowSDL::~WindowSDL() {
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
}

bool WindowSDL::PollEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            // A minimized window reports nothing usable: the swapchain keeps its size.
            if (event.window.data1 > 0 && event.window.data2 > 0) {
                width = event.window.data1;
                height = event.window.data2;
            }
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            is_open = false;
            break;
        default:
            break;
        }
    }
    return IsOpen();
}

} // namespace Frontend
