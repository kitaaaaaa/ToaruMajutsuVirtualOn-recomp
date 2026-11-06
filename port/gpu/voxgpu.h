/* voxgpu: shadPS4 v0.21's GNM driver, VideoOut, event queues and Vulkan renderer for vox.exe. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *title, *serial, *user_dir;
    uint32_t sdk_version, psf_attributes;
    int32_t width, height;
} VoxGpuConfig;

/* Event queue functions: needed before imports resolve. */
void voxgpu_register_kernel(void);
/* Window on its own thread pumping events, Vulkan, presenter, GPU command processor; registers Gnm
   and VideoOut. 0 on success. */
int voxgpu_init(const VoxGpuConfig *c);
/* The function for a NID (the part before '#'), or 0. */
uintptr_t voxgpu_resolve(const char *nid);
/* 1 if page tracking handled it; context is a Windows CONTEXT*. */
int voxgpu_handle_fault(void *context, void *address);
/* Guest memory the GPU may read, as the runtime maps and unmaps it. Asking Windows instead is far too
   slow on the renderer's hot paths. */
void voxgpu_notify_map(uint64_t address, uint64_t size);
void voxgpu_notify_unmap(uint64_t address, uint64_t size);

#ifdef __cplusplus
}
#endif
