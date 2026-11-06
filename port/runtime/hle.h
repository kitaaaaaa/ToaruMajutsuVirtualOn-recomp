/* What the hle*.c files share. */
#ifndef VOX_HLE_H
#define VOX_HLE_H
#include "runtime.h"

extern uint64_t bb_image_base;
void *runtime_low_map(size_t size, int prot);

/* PS4 (FreeBSD) errno values; sce* functions return them with this prefix. */
enum {
    E_PERM = 1, E_NOENT = 2, E_INTR = 4, E_IO = 5, E_BADF = 9, E_DEADLK = 11, E_NOMEM = 12, E_ACCES = 13, E_FAULT = 14,
    E_BUSY = 16, E_EXIST = 17, E_NOTDIR = 20, E_ISDIR = 21, E_INVAL = 22, E_MFILE = 24, E_NOSPC = 28, E_AGAIN = 35,
    E_TIMEDOUT = 60, E_NOTEMPTY = 66, E_NOSYS = 78,
};
#define ORBIS_ERR(e) ((int32_t)(UINT32_C(0x80020000) | (uint32_t)(e)))

extern const RtExport rt_thread_exports[];
extern const size_t rt_thread_export_count;
extern const RtExport rt_memory_exports[];
extern const size_t rt_memory_export_count;
extern const RtExport rt_kernel_exports[];
extern const size_t rt_kernel_export_count;
extern const RtExport rt_video_exports[];
extern const size_t rt_video_export_count;
extern const RtExport rt_gnm_exports[];
extern const size_t rt_gnm_export_count;
extern const RtExport rt_service_exports[];
extern const size_t rt_service_export_count;
extern const RtExport rt_io_exports[];
extern const size_t rt_io_export_count;

int32_t rt_equeue_add(void *queue, uint64_t ident, int16_t filter, void *udata);
int32_t rt_equeue_remove(void *queue, uint64_t ident, int16_t filter);
void rt_equeue_trigger(uint64_t ident, int16_t filter, uint64_t data);
int32_t rt_video_flip(int32_t handle, int32_t buffer, int32_t mode, int64_t argument);
void rt_video_init(void);
#endif
