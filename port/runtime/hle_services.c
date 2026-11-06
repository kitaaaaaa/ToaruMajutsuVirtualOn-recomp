/* PS4 system services, offline: one local user, no network. Each answer here was looked at; an import
 * not listed anywhere stops the run (loader.c), so a made-up success can't hide. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "cpu/bbcpu.h"
#include "hle.h"

static ABI int32_t ok(void) { return 0; }

/* The modules the game asks to load are implemented here. */
static ABI int32_t sysmodule_load(uint32_t id) {
    printf("HLE: system module %#x requested\n", id);
    return 0;
}

enum { USER_ID = 1 };
static ABI int32_t user_initial(int32_t *out) {
    if (!out) return (int32_t)0x80960002;
    *out = USER_ID;
    return 0;
}
static ABI int32_t user_name(int32_t user, char *out, uint64_t size) {
    (void)user;
    if (!out || !size) return (int32_t)0x80960002;
    snprintf(out, size, "Player");
    return 0;
}
static ABI int32_t user_color(int32_t user, int32_t *out) {
    (void)user;
    if (out) *out = 0;
    return 0;
}

/* SceNpId: a 16-character online id first, then reserved fields; 36 bytes. */
static ABI int32_t np_get_id(int32_t user, char *out) {
    (void)user;
    if (!out) return (int32_t)0x80550003;
    memset(out, 0, 36);
    snprintf(out, 17, "Player");
    return 0;
}

static ABI int32_t system_status(void *out) {
    if (out) memset(out, 0, 12);
    return 0;
}
/* ORBIS_SYSTEM_SERVICE_ERROR_NO_EVENT: nothing has happened. */
static ABI int32_t system_receive_event(void *out) { (void)out; return (int32_t)0x80a10004; }

/* PlayGo: the whole game is installed. Locus 3: "fast local storage" (shadPS4's playgo.cpp). */
static ABI int32_t playgo_open(int32_t *handle, const void *param) {
    (void)param;
    if (!handle) return (int32_t)0x80b20002;
    *handle = 1;
    return 0;
}
static ABI int32_t playgo_locus(int32_t handle, const uint16_t *chunks, uint32_t count, int8_t *out) {
    (void)handle; (void)chunks;
    if (!out) return (int32_t)0x80b20002;
    for (uint32_t i = 0; i < count; ++i) out[i] = 3;
    return 0;
}

/* RTC: ticks are microseconds since year 1. */
#define TICKS_TO_UNIX_EPOCH UINT64_C(62135596800000000)
typedef struct { uint16_t year, month, day, hour, minute, second; uint32_t microsecond; } RtcDateTime;
static uint64_t filetime_to_tick(const FILETIME *t) {
    return (((uint64_t)t->dwHighDateTime << 32) | t->dwLowDateTime) / 10 + UINT64_C(50491123200000000);
}
static ABI int32_t rtc_tick(uint64_t *out) {
    if (!out) return (int32_t)0x80b50002;
    FILETIME t;
    GetSystemTimePreciseAsFileTime(&t);
    *out = filetime_to_tick(&t);
    return 0;
}
static ABI int32_t rtc_local(RtcDateTime *out, int32_t zone_minutes) {
    (void)zone_minutes;
    if (!out) return (int32_t)0x80b50002;
    SYSTEMTIME s;
    GetLocalTime(&s);
    out->year = s.wYear;
    out->month = s.wMonth;
    out->day = s.wDay;
    out->hour = s.wHour;
    out->minute = s.wMinute;
    out->second = s.wSecond;
    out->microsecond = (uint32_t)s.wMilliseconds * 1000;
    return 0;
}
static ABI int32_t rtc_set_tick(RtcDateTime *out, const uint64_t *tick) {
    if (!out || !tick) return (int32_t)0x80b50002;
    const uint64_t filetime = (*tick - UINT64_C(50491123200000000)) * 10;
    const FILETIME t = {(DWORD)filetime, (DWORD)(filetime >> 32)};
    SYSTEMTIME s;
    if (!FileTimeToSystemTime(&t, &s)) return (int32_t)0x80b50002;
    out->year = s.wYear;
    out->month = s.wMonth;
    out->day = s.wDay;
    out->hour = s.wHour;
    out->minute = s.wMinute;
    out->second = s.wSecond;
    out->microsecond = (uint32_t)(*tick % 1000000);
    return 0;
}

/* Trophies: accepted and logged, nothing stored yet. */
static ABI int32_t trophy_create(int32_t *out) {
    if (out) *out = 1;
    return 0;
}
static ABI int32_t trophy_create_context(int32_t *out, int32_t user, uint32_t service, uint64_t options) {
    (void)user; (void)service; (void)options;
    if (out) *out = 1;
    return 0;
}
static ABI int32_t trophy_unlock(int32_t context, int32_t handle, int32_t id, int32_t *platinum) {
    (void)context; (void)handle;
    printf("HLE: trophy %d unlocked\n", id);
    if (platinum) *platinum = -1;
    return 0;
}

/* Common dialogs, with no UI yet: each opens and is finished by the next status check, answering OK.
 * Status values (shadPS4's commondialog.h): 0 none, 1 initialized, 2 running, 3 finished. */
enum { DIALOG_NONE, DIALOG_INITIALIZED, DIALOG_RUNNING, DIALOG_FINISHED };
#define DIALOG(prefix, label)                                                                              \
    static volatile int prefix##_status;                                                                   \
    static ABI int32_t prefix##_initialize(void) { prefix##_status = DIALOG_INITIALIZED; return 0; }       \
    static ABI int32_t prefix##_terminate(void) { prefix##_status = DIALOG_NONE; return 0; }               \
    static ABI int32_t prefix##_close(void) { prefix##_status = DIALOG_FINISHED; return 0; }               \
    static ABI int32_t prefix##_open(const void *param) {                                                  \
        (void)param;                                                                                       \
        printf("HLE: %s dialog opened, answering OK\n", label);                                            \
        prefix##_status = DIALOG_RUNNING;                                                                  \
        return 0;                                                                                          \
    }                                                                                                      \
    static ABI int32_t prefix##_update(void) {                                                             \
        if (prefix##_status == DIALOG_RUNNING) prefix##_status = DIALOG_FINISHED;                          \
        return prefix##_status;                                                                            \
    }
DIALOG(save_dialog, "save data")
DIALOG(msg_dialog, "message")
DIALOG(error_dialog, "error")
DIALOG(commerce_dialog, "store")
DIALOG(invitation_dialog, "invitation")

static volatile int save_dialog_progress;

/* The save dialog's request, as ErrorSaveDialog.cpp in the game fills it: mode +0x34, display type +0x38,
 * the system message's parameter pointer +0x58 (its type first). */
static ABI int32_t save_dialog_open_logged(const uint8_t *param) {
    static volatile long logged;
    if (param && InterlockedIncrement(&logged) <= 12) {
        const uint32_t *system_message = *(const uint32_t *const *)(param + 0x58);
        printf("HLE: save dialog mode %u display %u message type %d from +%llx\n", *(const uint32_t *)(param + 0x34), *(const uint32_t *)(param + 0x38),
               system_message ? (int)*system_message : -1, (unsigned long long)(bbcpu_import_return() - bb_image_base));
    }
    /* System message type 5 is a progress dialog: it stays up until the game closes it. */
    const uint32_t *system_message = param ? *(const uint32_t *const *)(param + 0x58) : NULL;
    save_dialog_progress = system_message && *system_message == 5;
    save_dialog_status = DIALOG_RUNNING;
    return 0;
}
static ABI int32_t save_dialog_update_status(void) {
    if (save_dialog_status == DIALOG_RUNNING && !save_dialog_progress) save_dialog_status = DIALOG_FINISHED;
    return save_dialog_status;
}

/* OrbisSaveDataDialogResult: mode, result, button (1: OK or yes), the rest unused here. */
static ABI int32_t save_dialog_result(uint32_t *out) {
    if (!out) return (int32_t)0x80b80006;
    out[1] = 0;
    out[2] = 1;
    return 0;
}
static ABI int32_t save_dialog_ready(void) { return 1; }
static ABI int32_t commerce_result(void *out) {
    if (out) memset(out, 0, 8);
    return 0;
}

/* AvPlayer: hle_movie.c. */
ABI void *rt_avplayer_init(const void *data);
ABI int32_t rt_avplayer_add_source(void *player, const char *path);
ABI int32_t rt_avplayer_set_looping(void *player, uint8_t looping);
ABI int32_t rt_avplayer_stop(void *player);
ABI int32_t rt_avplayer_close(void *player);
ABI uint8_t rt_avplayer_is_active(const void *player);
ABI uint8_t rt_avplayer_video_data(void *player, uint8_t *frame);
ABI uint8_t rt_avplayer_audio_data(void *player, uint8_t *frame);

/* The mouse is there and never moves. */
static ABI int32_t mouse_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)user; (void)type; (void)index; (void)param;
    return 1;
}
static ABI int32_t mouse_read(int32_t handle, void *out, int32_t count) { (void)handle; (void)out; (void)count; return 0; }

const RtExport rt_service_exports[] = {
    {"sceSaveDataDialogInitialize", save_dialog_initialize}, {"sceSaveDataDialogTerminate", save_dialog_terminate}, {"sceSaveDataDialogOpen", save_dialog_open_logged},
    {"sceSaveDataDialogClose", save_dialog_close}, {"sceSaveDataDialogUpdateStatus", save_dialog_update_status}, {"sceSaveDataDialogGetResult", save_dialog_result},
    {"sceSaveDataDialogIsReadyToDisplay", save_dialog_ready},
    {"sceMsgDialogInitialize", msg_dialog_initialize}, {"sceMsgDialogTerminate", msg_dialog_terminate}, {"sceMsgDialogOpen", msg_dialog_open},
    {"sceMsgDialogClose", msg_dialog_close}, {"sceMsgDialogUpdateStatus", msg_dialog_update},
    {"sceErrorDialogInitialize", error_dialog_initialize}, {"sceErrorDialogTerminate", error_dialog_terminate}, {"sceErrorDialogOpen", error_dialog_open},
    {"sceErrorDialogUpdateStatus", error_dialog_update},
    {"sceNpCommerceDialogInitialize", commerce_dialog_initialize}, {"sceNpCommerceDialogTerminate", commerce_dialog_terminate}, {"sceNpCommerceDialogOpen", commerce_dialog_open},
    {"sceNpCommerceDialogUpdateStatus", commerce_dialog_update}, {"sceNpCommerceDialogGetResult", commerce_result},
    {"sceInvitationDialogInitialize", invitation_dialog_initialize}, {"sceInvitationDialogTerminate", invitation_dialog_terminate}, {"sceInvitationDialogOpenA", invitation_dialog_open},
    {"sceInvitationDialogClose", invitation_dialog_close}, {"sceInvitationDialogUpdateStatus", invitation_dialog_update},
    {"sceAvPlayerInit", rt_avplayer_init}, {"sceAvPlayerAddSource", rt_avplayer_add_source}, {"sceAvPlayerClose", rt_avplayer_close}, {"sceAvPlayerStop", rt_avplayer_stop},
    {"sceAvPlayerPause", ok}, {"sceAvPlayerResume", ok}, {"sceAvPlayerSetLooping", rt_avplayer_set_looping}, {"sceAvPlayerIsActive", rt_avplayer_is_active},
    {"sceAvPlayerGetVideoDataEx", rt_avplayer_video_data}, {"sceAvPlayerGetAudioData", rt_avplayer_audio_data},
    {"sceScreenShotSetParam", ok}, {"sceScreenShotSetOverlayImage", ok},
    {"sceNpTrophyCreateContext", trophy_create_context}, {"sceNpTrophyCreateHandle", trophy_create}, {"sceNpTrophyRegisterContext", ok},
    {"sceNpTrophyDestroyContext", ok}, {"sceNpTrophyDestroyHandle", ok}, {"sceNpTrophyUnlockTrophy", trophy_unlock},
    {"sceMouseInit", ok}, {"sceMouseOpen", mouse_open}, {"sceMouseRead", mouse_read}, {"sceMouseClose", ok},
    {"scePlayGoInitialize", ok}, {"scePlayGoTerminate", ok}, {"scePlayGoOpen", playgo_open}, {"scePlayGoClose", ok}, {"scePlayGoGetLocus", playgo_locus},
    {"sceRtcGetCurrentTick", rtc_tick}, {"sceRtcGetCurrentClockLocalTime", rtc_local}, {"sceRtcSetTick", rtc_set_tick},
    {"sceSysmoduleLoadModule", sysmodule_load}, {"sceSysmoduleUnloadModule", ok}, {"sceSysmoduleLoadModuleInternalWithArg", ok},
    {"sceUserServiceInitialize", ok}, {"sceUserServiceTerminate", ok}, {"sceUserServiceGetInitialUser", user_initial},
    {"sceUserServiceGetUserName", user_name}, {"sceUserServiceGetUserColor", user_color},
    {"sceNpSetNpTitleId", ok}, {"sceNpSetContentRestriction", ok}, {"sceNpGetNpId", np_get_id},
    {"sceCommonDialogInitialize", ok},
    {"sceSystemServiceGetStatus", system_status}, {"sceSystemServiceHideSplashScreen", ok}, {"sceSystemServiceReceiveEvent", system_receive_event},
};
const size_t rt_service_export_count = sizeof(rt_service_exports) / sizeof(*rt_service_exports);
