// vox: the settings the video core reads, fixed: no validation, no dumps, pipeline cache off.
// Otherwise upstream's defaults, except the VideoOut resolution, which is the console's.
#pragma once
#include <string>
#include "common/types.h"

enum GpuReadbacksMode : int { Disabled, Relaxed, Precise };

class EmulatorSettingsImpl {
public:
    static EmulatorSettingsImpl* GetInstance() {
        static EmulatorSettingsImpl instance;
        return &instance;
    }
    s32 GetGpuId() const { return -1; } // auto
    u32 GetInternalScreenWidth() const { return 1920; }
    u32 GetInternalScreenHeight() const { return 1080; }
    std::string GetPresentMode() const { return "Mailbox"; }
    int GetRcasAttenuation() const { return 250; }
    u32 GetReadbacksMode() const { return GpuReadbacksMode::Disabled; }
    u32 GetVblankFrequency() const { return 60; }
    // vox: on. The CP runs packets in order and signals an EOP at once; a real one has read ahead by
    // then. Without a copy the game reuses a buffer the CP hasn't finished, as soon as it hears "done".
    bool IsCopyGpuBuffers() const { return true; }
    bool IsDirectMemoryAccessEnabled() const { return false; }
    bool IsDumpShaders() const { return false; }
    bool IsFastShaderMath() const { return false; }
    bool IsFsrEnabled() const { return false; }
    bool IsHdrAllowed() const { return false; }
    bool IsInlineFetchShader() const { return false; }
    bool IsNullGPU() const { return false; }
    bool IsPatchShaders() const { return false; }
    bool IsPipelineCacheArchived() const { return false; }
    bool IsPipelineCacheEnabled() const { return false; }
    bool IsRcasEnabled() const { return true; }
    bool IsReadbackLinearImagesEnabled() const { return false; }
    bool IsRenderdocEnabled() const { return false; }
    bool IsShaderCollect() const { return false; }
    bool IsUserfaultfdTracking() const { return false; }
    bool IsVkCrashDiagnosticEnabled() const { return false; }
    bool IsVkGuestMarkersEnabled() const { return false; }
    bool IsVkHostMarkersEnabled() const { return false; }
    bool IsVkValidationCoreEnabled() const { return false; }
    bool IsVkValidationEnabled() const { return false; }
    bool IsVkValidationGpuEnabled() const { return false; }
    bool IsVkValidationSyncEnabled() const { return false; }
};
#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())
