/* Links voxgpu, opens the window and Vulkan device, lets the present thread run, exits. */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <windows.h>
#include "voxgpu.h"

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    voxgpu_register_kernel();
    printf("sceKernelCreateEqueue = %p\n", (void*)voxgpu_resolve("D0OdFMjp46I#A#B"));
    printf("sceGnmAddEqEvent before init = %p\n", (void*)voxgpu_resolve("b0xyllnVY-I"));

    VoxGpuConfig config = {
        .title = "voxgpu test", .serial = "CUSA00000", .user_dir = NULL,
        .sdk_version = 0x4508101, .psf_attributes = 0, .width = 1280, .height = 720,
    };
    int result = voxgpu_init(&config);
    printf("voxgpu_init = %d\n", result);
    if (result != 0) {
        return 1;
    }
    printf("sceGnmAddEqEvent = %p\n", (void*)voxgpu_resolve("b0xyllnVY-I"));
    printf("sceVideoOutGetFlipStatus = %p\n", (void*)voxgpu_resolve("SbU3dwp80lQ"));
    printf("voxgpu_handle_fault(untracked) = %d\n", voxgpu_handle_fault(NULL, (void*)0x10));

    /* A real flip through the GPU thread and the present thread: the blit to the swapchain. */
    typedef struct { uint64_t count, process_time, tsc; int64_t flip_arg; uint64_t rest[4]; } FlipStatus;
    int (__attribute__((sysv_abi)) *video_out_open)(int, int, int, const void*) = (void*)voxgpu_resolve("Up36PTk687E");
    int (__attribute__((sysv_abi)) *submit_flip)(int, int, int, int64_t) = (void*)voxgpu_resolve("U46NwOiJpys");
    int (__attribute__((sysv_abi)) *flip_status)(int, FlipStatus*) = (void*)voxgpu_resolve("SbU3dwp80lQ");
    int handle = video_out_open(255, 0, 0, NULL);
    printf("sceVideoOutOpen = %d\n", handle);
    for (int flip = 1; flip <= 60; ++flip) {
        int submitted = submit_flip(handle, -1, 1, flip);
        if (submitted != 0) {
            printf("sceVideoOutSubmitFlip = %#x\n", submitted);
            break;
        }
        Sleep(50);
    }
    Sleep(500);
    FlipStatus status = {0};
    int got = flip_status(handle, &status);
    printf("sceVideoOutGetFlipStatus = %d, flips presented = %llu, last flip_arg = %lld\n", got,
           (unsigned long long)status.count, (long long)status.flip_arg);

    Sleep(1000);
    printf("exiting\n");
    /* No shutdown in the API: the renderer's threads and static destructors don't get to run. */
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
