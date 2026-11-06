/* libSceGnmDriver with no renderer yet. Functions that build commands write NOP packets of the size
 * the caller reserved: valid to walk, and Phase 5 replaces them with the real packets (shadPS4's
 * gnmdriver.cpp). Submitted buffers are walked at once, which is all a game can observe of a GPU that
 * draws nothing: labels are written and end-of-pipe events fire. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "hle.h"

enum { PM4_NOP = 0x10, PM4_INDIRECT_BUFFER_CNST = 0x33, PM4_WRITE_DATA = 0x37, PM4_WAIT_REG_MEM = 0x3c, PM4_INDIRECT_BUFFER = 0x3f,
       PM4_EVENT_WRITE_EOP = 0x47, PM4_EVENT_WRITE_EOS = 0x48, PM4_RELEASE_MEM = 0x49 };
enum { FILTER_GRAPHICS_CORE = -14, EVENT_EOP = 0x40 };
#define GNM_FAILURE ((int32_t)0x8eee00ff)

static void write_nops(uint32_t *cmd, uint32_t size) {
    if (!cmd || !size) return;
    if (size == 1) { cmd[0] = 0x80000000u; return; } /* type 2: one dword */
    cmd[0] = 0xc0000000u | ((size - 2) << 16) | (PM4_NOP << 8);
    memset(cmd + 1, 0, (size - 1) * 4);
}

static uint64_t gpu_clock(void) {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000000 + (uint64_t)(c.QuadPart % f.QuadPart) * 1000000000 / (uint64_t)f.QuadPart;
}

static void write_fence(uint64_t address, uint32_t select, uint32_t lo, uint32_t hi) {
    if (!address) return;
    switch (select) {
    case 1: *(volatile uint32_t *)(uintptr_t)address = lo; break;
    case 2: *(volatile uint64_t *)(uintptr_t)address = lo | ((uint64_t)hi << 32); break;
    case 3: case 4: *(volatile uint64_t *)(uintptr_t)address = gpu_clock(); break;
    default: break;
    }
}

/* ---- walking a command buffer ---- */

static uint64_t packets_seen[256];
static int bad_reported;
static volatile uint64_t submit_count, builder_calls;

/* The heartbeat's line: what the game asked the GPU to do so far. */
void rt_gnm_report(char *out, size_t size) {
    uint64_t total = 0, top[3] = {0, 0, 0};
    int top_op[3] = {0, 0, 0};
    for (int i = 0; i < 256; ++i) {
        total += packets_seen[i];
        for (int k = 0; k < 3; ++k)
            if (packets_seen[i] > top[k]) {
                for (int m = 2; m > k; --m) { top[m] = top[m - 1]; top_op[m] = top_op[m - 1]; }
                top[k] = packets_seen[i];
                top_op[k] = i;
                break;
            }
    }
    snprintf(out, size, "%llu submits, %llu packets (top %#x:%llu %#x:%llu %#x:%llu), %llu builder calls", (unsigned long long)submit_count,
             (unsigned long long)total, top_op[0], (unsigned long long)top[0], top_op[1], (unsigned long long)top[1], top_op[2],
             (unsigned long long)top[2], (unsigned long long)builder_calls);
}

static void walk(const uint32_t *cmd, uint64_t dwords, int depth) {
    const uint32_t *end = cmd + dwords;
    while (cmd < end) {
        const uint32_t header = *cmd;
        const uint32_t type = header >> 30;
        if (type == 2) { ++cmd; continue; }
        const uint32_t count = (header >> 16) & 0x3fff;
        const uint32_t size = count + 2;
        if ((type != 3 && type != 0) || cmd + size > end) {
            if (!bad_reported++) printf("HLE: malformed PM4 packet %#x with %lld dwords left\n", header, (long long)(end - cmd));
            return;
        }
        if (type == 3) {
            const uint32_t op = (header >> 8) & 0xff;
            ++packets_seen[op];
            switch (op) {
            case PM4_EVENT_WRITE_EOP: {
                const uint64_t address = cmd[2] | ((uint64_t)(cmd[3] & 0xffff) << 32);
                write_fence(address, cmd[3] >> 29, cmd[4], cmd[5]);
                if ((cmd[3] >> 24) & 3) rt_equeue_trigger(EVENT_EOP, FILTER_GRAPHICS_CORE, EVENT_EOP);
                break;
            }
            case PM4_EVENT_WRITE_EOS:
                if ((cmd[3] >> 29) == 2) write_fence(cmd[2] | ((uint64_t)(cmd[3] & 0xffff) << 32), 1, cmd[4], 0);
                break;
            case PM4_RELEASE_MEM: {
                const uint64_t address = cmd[3] | ((uint64_t)cmd[4] << 32);
                write_fence(address, cmd[2] >> 29, cmd[5], cmd[6]);
                if ((cmd[2] >> 24) & 7) rt_equeue_trigger(EVENT_EOP, FILTER_GRAPHICS_CORE, EVENT_EOP);
                break;
            }
            case PM4_WRITE_DATA: {
                const uint32_t destination = (cmd[1] >> 8) & 15;
                if (destination == 2 || destination == 5) {
                    const uint64_t address = cmd[2] | ((uint64_t)cmd[3] << 32);
                    if (address) memcpy((void *)(uintptr_t)address, cmd + 4, (size_t)(count - 2) * 4);
                }
                break;
            }
            case PM4_INDIRECT_BUFFER: case PM4_INDIRECT_BUFFER_CNST:
                if (depth < 4) walk((const uint32_t *)(uintptr_t)(cmd[1] | ((uint64_t)(cmd[2] & 0xffff) << 32)), cmd[3] & 0xfffff, depth + 1);
                break;
            default: break; /* draws, state, waits: nothing to do until there is a renderer */
            }
        }
        cmd += size;
    }
}

static int32_t submit(uint32_t count, const uint32_t **dcb, const uint32_t *dcb_sizes, const uint32_t **ccb, const uint32_t *ccb_sizes) {
    InterlockedIncrement64((volatile int64_t *)&submit_count);
    for (uint32_t i = 0; i < count; ++i) {
        if (dcb && dcb_sizes && dcb[i]) walk(dcb[i], dcb_sizes[i] / 4, 0);
        if (ccb && ccb_sizes && ccb[i]) walk(ccb[i], ccb_sizes[i] / 4, 0);
    }
    return 0;
}

static ABI int32_t submit_buffers(uint32_t count, const uint32_t **dcb, const uint32_t *dcb_sizes, const uint32_t **ccb, const uint32_t *ccb_sizes) {
    return submit(count, dcb, dcb_sizes, ccb, ccb_sizes);
}
static ABI int32_t submit_workload(uint32_t workload, uint32_t count, const uint32_t **dcb, const uint32_t *dcb_sizes, const uint32_t **ccb, const uint32_t *ccb_sizes) {
    (void)workload;
    return submit(count, dcb, dcb_sizes, ccb, ccb_sizes);
}
static ABI int32_t submit_and_flip(uint32_t count, const uint32_t **dcb, const uint32_t *dcb_sizes, const uint32_t **ccb, const uint32_t *ccb_sizes, uint32_t handle, uint32_t buffer, uint32_t mode, int64_t argument) {
    submit(count, dcb, dcb_sizes, ccb, ccb_sizes);
    return rt_video_flip((int32_t)handle, (int32_t)buffer, (int32_t)mode, argument);
}
static ABI int32_t submit_and_flip_workload(uint32_t workload, uint32_t count, const uint32_t **dcb, const uint32_t *dcb_sizes, const uint32_t **ccb, const uint32_t *ccb_sizes, uint32_t handle, uint32_t buffer, uint32_t mode, int64_t argument) {
    (void)workload;
    submit(count, dcb, dcb_sizes, ccb, ccb_sizes);
    return rt_video_flip((int32_t)handle, (int32_t)buffer, (int32_t)mode, argument);
}
static ABI int32_t request_flip(uint32_t *cmd, uint32_t bytes, uint32_t handle, uint32_t buffer, uint32_t mode, int64_t argument) {
    write_nops(cmd, bytes / 4);
    return rt_video_flip((int32_t)handle, (int32_t)buffer, (int32_t)mode, argument);
}
static ABI int32_t request_flip_workload(uint64_t workload, uint32_t *cmd, uint32_t bytes, uint32_t handle, uint32_t buffer, uint32_t mode, int64_t argument) {
    (void)workload;
    return request_flip(cmd, bytes, handle, buffer, mode, argument);
}

/* ---- compute queues ---- */

enum { COMPUTE_QUEUES = 64 };
static struct { const uint32_t *ring; uint32_t size_dw, offset; uint32_t *read_pointer; } compute[COMPUTE_QUEUES];
static SRWLOCK compute_lock = SRWLOCK_INIT;

static ABI int32_t map_compute(uint32_t pipe, uint32_t queue, const uint32_t *ring, uint32_t size_dw, uint32_t *read_pointer) {
    const uint32_t id = pipe * 8 + queue;
    if (id >= COMPUTE_QUEUES) return (int32_t)0x8eee00ff;
    AcquireSRWLockExclusive(&compute_lock);
    compute[id].ring = ring;
    compute[id].size_dw = size_dw;
    compute[id].offset = 0;
    compute[id].read_pointer = read_pointer;
    ReleaseSRWLockExclusive(&compute_lock);
    if (read_pointer) *read_pointer = 0;
    return (int32_t)id + 1;
}
static ABI int32_t map_compute_priority(uint32_t pipe, uint32_t queue, const uint32_t *ring, uint32_t size_dw, uint32_t *read_pointer, uint32_t priority) {
    (void)priority;
    return map_compute(pipe, queue, ring, size_dw, read_pointer);
}
static ABI int32_t unmap_compute(uint32_t vqid) {
    if (vqid && vqid <= COMPUTE_QUEUES) compute[vqid - 1].ring = NULL;
    return 0;
}
static ABI void ding_dong(uint32_t vqid, uint32_t next) {
    if (!vqid || vqid > COMPUTE_QUEUES) return;
    AcquireSRWLockExclusive(&compute_lock);
    const uint32_t id = vqid - 1;
    if (compute[id].ring && next != compute[id].offset) {
        if (next < compute[id].offset) { /* wrapped */
            walk(compute[id].ring + compute[id].offset, compute[id].size_dw - compute[id].offset, 0);
            compute[id].offset = 0;
        }
        walk(compute[id].ring + compute[id].offset, next - compute[id].offset, 0);
        compute[id].offset = next;
        if (compute[id].read_pointer) *compute[id].read_pointer = next;
    }
    ReleaseSRWLockExclusive(&compute_lock);
}
static ABI void ding_dong_workload(uint32_t vqid, uint32_t next, uint64_t workload) { (void)workload; ding_dong(vqid, next); }

/* ---- what the game asks for ---- */

static ABI int32_t add_eq_event(void *queue, uint64_t id, void *udata) { return rt_equeue_add(queue, id, FILTER_GRAPHICS_CORE, udata); }
static ABI int32_t delete_eq_event(void *queue, uint64_t id) { return rt_equeue_remove(queue, id, FILTER_GRAPHICS_CORE); }
static ABI int32_t eq_event_type(const uint64_t *event) { return event ? (int32_t)event[2] : 0; }
static ABI int32_t eq_timestamp(void) { return 0; }
static ABI int32_t ok(void) { return 0; }
static ABI int32_t failure(void) { return GNM_FAILURE; }
static ABI int32_t submits_allowed(void) { return 1; }
static ABI int32_t cu_mask(int64_t unused, int32_t mask) { (void)unused; return mask; }
static ABI uint64_t tessellation_ring(void) {
    static void *ring;
    if (!ring) ring = rt_low_alloc(0x40000);
    return (uint64_t)(uintptr_t)ring;
}
static ABI uint32_t offchip_size(void) { return 0x800000; }

static ABI int32_t begin_workload(uint32_t stream, uint64_t *workload) {
    if (!workload) return 3;
    *workload = stream < 16 ? 1 : 0;
    return stream > 15;
}
static ABI int32_t end_workload(uint64_t workload) { return (((workload >> 56) & 0xff) > 15) * 2; }
static ABI int32_t create_workload_stream(uint64_t unused, uint32_t *stream) {
    (void)unused;
    if (!stream) return 3;
    *stream = 1;
    return 0;
}

/* Builders: NOPs of the reserved size, 0 back. */
static ABI int32_t nop_builder(uint32_t *cmd, uint32_t size) {
    InterlockedIncrement64((volatile int64_t *)&builder_calls);
    write_nops(cmd, size);
    return 0;
}
/* The default state builders return how much they wrote (shadPS4's HwInitPacketSize, CtxInitPacketSize). */
static ABI uint32_t init_hardware(uint32_t *cmd, uint32_t size) {
    if (size < 0x100) return 0;
    write_nops(cmd, 0x100);
    return 0x100;
}

const RtExport rt_gnm_exports[] = {
    {"sceGnmSubmitCommandBuffers", submit_buffers}, {"sceGnmSubmitCommandBuffersForWorkload", submit_workload},
    {"sceGnmSubmitAndFlipCommandBuffers", submit_and_flip}, {"sceGnmSubmitAndFlipCommandBuffersForWorkload", submit_and_flip_workload},
    {"sceGnmRequestFlipAndSubmitDone", request_flip}, {"sceGnmRequestFlipAndSubmitDoneForWorkload", request_flip_workload},
    {"sceGnmSubmitDone", ok}, {"sceGnmAreSubmitsAllowed", submits_allowed}, {"sceGnmFlushGarlic", ok},
    {"sceGnmMapComputeQueue", map_compute}, {"sceGnmMapComputeQueueWithPriority", map_compute_priority}, {"sceGnmUnmapComputeQueue", unmap_compute},
    {"sceGnmDingDong", ding_dong}, {"sceGnmDingDongForWorkload", ding_dong_workload},
    {"sceGnmAddEqEvent", add_eq_event}, {"sceGnmDeleteEqEvent", delete_eq_event}, {"sceGnmGetEqEventType", eq_event_type}, {"sceGnmGetEqTimeStamp", eq_timestamp},
    {"sceGnmBeginWorkload", begin_workload}, {"sceGnmEndWorkload", end_workload}, {"sceGnmCreateWorkloadStream", create_workload_stream}, {"sceGnmDestroyWorkloadStream", ok},
    {"sceGnmLogicalCuMaskToPhysicalCuMask", cu_mask}, {"sceGnmGetTheTessellationFactorRingBufferBaseAddress", tessellation_ring},
    {"sceGnmGetOffChipTessellationBufferSize", offchip_size},
    /* Debug and registration: what retail firmware answers, per shadPS4. */
    {"sceGnmRegisterOwner", failure}, {"sceGnmRegisterResource", failure}, {"sceGnmRegisterGdsResource", failure},
    {"sceGnmSetResourceRegistrationUserMemory", failure}, {"sceGnmUnregisterOwnerAndResources", failure}, {"sceGnmUnregisterResource", failure},
    {"sceGnmUnregisterAllResourcesForOwner", failure},
    {"sceGnmIsUserPaEnabled", ok}, {"sceGnmDriverCaptureInProgress", ok}, {"sceGnmDriverTraceInProgress", ok}, {"sceGnmDriverTriggerCapture", ok},
    {"sceRazorIsLoaded", ok}, {"sceRazorCaptureImmediate", ok}, {"sceGnmValidateOnSubmitEnabled", ok}, {"sceGnmValidateDisableDiagnostics", ok},
    {"sceGnmValidateDisableDiagnostics2", ok}, {"sceGnmValidateResetState", ok}, {"sceGnmDebugHardwareStatus", ok}, {"sceGnmGetGpuInfoStatus", ok},
    {"sceGnmGetGpuBlockStatus", ok}, {"sceGnmGetLastWaitedAddress", ok}, {"sceGnmGetNumTcaUnits", ok}, {"sceGnmSetupMipStatsReport", ok},
    {"sceGnmDisableMipStatsReport", ok}, {"sceGnmRequestMipStatsReportAndReset", ok}, {"sceGnmSetWaveLimitMultipliers", ok}, {"sceGnmSetGsRingSizes", ok},
    {"sceGnmInsertThreadTraceMarker", ok},
    /* State init: real packets in Phase 5. */
    {"sceGnmDrawInitDefaultHardwareState350", init_hardware}, {"sceGnmDrawInitToDefaultContextState400", init_hardware}, {"sceGnmDispatchInitDefaultHardwareState", init_hardware},
    /* Draws, dispatches, shaders, markers: real packets in Phase 5. */
    {"sceGnmDrawIndex", nop_builder}, {"sceGnmDrawIndexAuto", nop_builder}, {"sceGnmDrawIndexOffset", nop_builder}, {"sceGnmDrawIndexIndirect", nop_builder},
    {"sceGnmDrawIndexIndirectCountMulti", nop_builder}, {"sceGnmDrawIndexMultiInstanced", nop_builder}, {"sceGnmDrawIndirect", nop_builder},
    {"sceGnmDrawIndirectCountMulti", nop_builder}, {"sceGnmDrawOpaqueAuto", nop_builder}, {"sceGnmDispatchDirect", nop_builder}, {"sceGnmDispatchIndirect", nop_builder},
    {"sceGnmDispatchIndirectOnMec", nop_builder}, {"sceGnmComputeWaitOnAddress", nop_builder}, {"sceGnmComputeWaitSemaphore", nop_builder},
    {"sceGnmSetVsShader", nop_builder}, {"sceGnmSetPsShader350", nop_builder}, {"sceGnmSetCsShaderWithModifier", nop_builder}, {"sceGnmSetEsShader", nop_builder},
    {"sceGnmSetGsShader", nop_builder}, {"sceGnmSetHsShader", nop_builder}, {"sceGnmSetLsShader", nop_builder}, {"sceGnmSetEmbeddedVsShader", nop_builder},
    {"sceGnmSetEmbeddedPsShader", nop_builder}, {"sceGnmUpdateVsShader", nop_builder}, {"sceGnmUpdatePsShader350", nop_builder}, {"sceGnmUpdateGsShader", nop_builder},
    {"sceGnmUpdateHsShader", nop_builder}, {"sceGnmSetVgtControl", nop_builder}, {"sceGnmResetVgtControl", nop_builder}, {"sceGnmInsertPushMarker", nop_builder},
    {"sceGnmInsertPushColorMarker", nop_builder}, {"sceGnmInsertPopMarker", nop_builder}, {"sceGnmInsertSetMarker", nop_builder}, {"sceGnmInsertSetColorMarker", nop_builder},
    {"sceGnmInsertDingDongMarker", nop_builder}, {"sceGnmInsertWaitFlipDone", nop_builder},
};
const size_t rt_gnm_export_count = sizeof(rt_gnm_exports) / sizeof(*rt_gnm_exports);
