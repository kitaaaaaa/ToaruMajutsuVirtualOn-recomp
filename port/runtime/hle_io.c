/* The pad, from an Xbox controller through XInput (idle and still connected when there is none), and
 * audio, through SDL. */
#include <windows.h>
#include <xinput.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

/* ---- pad (OrbisPadData from shadPS4's pad.h, 0x78 bytes) ---- */

typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_time;
    struct { uint16_t x, y; uint8_t id, reserve[3]; } touch[2];
    uint8_t connected, connected_pad[3];
    uint64_t timestamp;
    uint32_t extension_id;
    uint8_t extension_reserve, extension_length, extension_data[10];
    uint8_t connected_count, reserve[2], unique_length, unique_data[12];
} PadData;
_Static_assert(sizeof(PadData) == 0x78, "OrbisPadData");

static int pad_ready;
static ABI int32_t pad_init(void) { pad_ready = 1; return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)user; (void)type; (void)index; (void)param;
    if (!pad_ready) return (int32_t)0x80920001;
    puts("HLE: pad opened, handle 1");
    return 1;
}
/* VOX_PAD_AUTO (testing): a short press every two seconds, to get past menus. 1 taps Options and Cross
 * in turn; 2 taps Circle, this version's confirm button. */
static uint32_t automatic_buttons(void) {
    static int mode = -1;
    if (mode < 0) mode = getenv("VOX_PAD_AUTO") ? atoi(getenv("VOX_PAD_AUTO")) : 0;
    if (!mode) return 0;
    const ULONGLONG t = GetTickCount64() / 100;
    if (t % 20 >= 2) return 0;
    if (mode == 2) return 0x2000u;
    return (t / 20) % 2 ? 0x8u : 0x4000u; /* Options, Cross */
}
/* The first connected controller. Asking XInput about an empty slot is slow, so a search for one runs
 * at most once a second. */
static int controller_state(XINPUT_STATE *state) {
    static int slot = -1;
    static ULONGLONG next_search;
    if (slot >= 0) {
        if (XInputGetState((DWORD)slot, state) == ERROR_SUCCESS) return 1;
        printf("HLE: controller %d disconnected\n", slot);
        slot = -1;
    }
    const ULONGLONG now = GetTickCount64();
    if (now < next_search) return 0;
    next_search = now + 1000;
    for (int i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (XInputGetState((DWORD)i, state) == ERROR_SUCCESS) {
            printf("HLE: controller %d connected\n", i);
            slot = i;
            return 1;
        }
    }
    return 0;
}
/* XInput's stick (-32768 to 32767, up positive) as a PS4 one (0 to 255, up 0), centered inside the
 * dead zone XInput's documentation gives for that stick. */
static void convert_stick(int x, int y, int dead_zone, uint8_t *out_x, uint8_t *out_y) {
    if ((int64_t)x * x + (int64_t)y * y < (int64_t)dead_zone * dead_zone) x = y = 0;
    *out_x = (uint8_t)((x + 32768) * 255 / 65535 + (x == 0));
    *out_y = (uint8_t)((32767 - y) * 255 / 65535 + (y == 0));
}
static void apply_controller(PadData *out) {
    XINPUT_STATE state;
    if (!controller_state(&state)) return;
    static const struct { WORD from; uint32_t to; } buttons[] = {
        {XINPUT_GAMEPAD_A, 0x4000}, {XINPUT_GAMEPAD_B, 0x2000}, {XINPUT_GAMEPAD_X, 0x8000}, {XINPUT_GAMEPAD_Y, 0x1000},
        {XINPUT_GAMEPAD_LEFT_SHOULDER, 0x400}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, 0x800},
        {XINPUT_GAMEPAD_LEFT_THUMB, 0x2}, {XINPUT_GAMEPAD_RIGHT_THUMB, 0x4},
        {XINPUT_GAMEPAD_START, 0x8}, {XINPUT_GAMEPAD_BACK, 0x100000},
        {XINPUT_GAMEPAD_DPAD_UP, 0x10}, {XINPUT_GAMEPAD_DPAD_RIGHT, 0x20}, {XINPUT_GAMEPAD_DPAD_DOWN, 0x40}, {XINPUT_GAMEPAD_DPAD_LEFT, 0x80},
    };
    const XINPUT_GAMEPAD *pad = &state.Gamepad;
    for (size_t i = 0; i < sizeof(buttons) / sizeof(*buttons); ++i)
        if (pad->wButtons & buttons[i].from) out->buttons |= buttons[i].to;
    out->l2 = pad->bLeftTrigger;
    out->r2 = pad->bRightTrigger;
    if (pad->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) out->buttons |= 0x100;
    if (pad->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) out->buttons |= 0x200;
    convert_stick(pad->sThumbLX, pad->sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, &out->left_x, &out->left_y);
    convert_stick(pad->sThumbRX, pad->sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, &out->right_x, &out->right_y);
}
static void read_state(PadData *out) {
    memset(out, 0, sizeof(*out));
    out->buttons = automatic_buttons();
    out->left_x = out->left_y = out->right_x = out->right_y = 128;
    out->orientation[3] = 1.0f;
    out->connected = 1;
    out->connected_count = 1;
    out->timestamp = GetTickCount64() * 1000;
    apply_controller(out);
    /* VOX_TRACE=1: every change of buttons. */
    static int trace = -1;
    static uint32_t last_buttons;
    if (trace < 0) trace = getenv("VOX_TRACE") != NULL;
    if (trace && out->buttons != last_buttons) {
        printf("TRACE buttons %#x\n", out->buttons);
        last_buttons = out->buttons;
    }
}
static ABI int32_t pad_read(int32_t handle, PadData *out, int32_t count) {
    (void)handle;
    if (!out || count < 1) return (int32_t)0x80920002;
    read_state(out);
    return 1;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *out) {
    (void)handle;
    if (!out) return (int32_t)0x80920002;
    read_state(out);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const void *param) { (void)handle; (void)param; return 0; }

/* ---- audio ---- */

/* A port is an SDL stream on the default device, which mixes them and downmixes for the speakers. */
enum { AUDIO_PORTS = 32, AUDIO_MIN_QUEUED_FRAMES = 2048 };
typedef struct {
    int used, heard;
    uint32_t grain, frequency, channels, sample_bytes, is_float, swap_rear, queue_limit;
    SDL_AudioStream *stream;
    uint8_t *scratch;
    uint64_t next_due_us;
} AudioPort;
static AudioPort ports[AUDIO_PORTS];
static SRWLOCK audio_lock = SRWLOCK_INIT;

static uint64_t now_us(void) {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (uint64_t)c.QuadPart / (uint64_t)f.QuadPart * 1000000 + (uint64_t)c.QuadPart % (uint64_t)f.QuadPart * 1000000 / (uint64_t)f.QuadPart;
}
static ABI int32_t audio_init(void) { return 0; }
static ABI int32_t audio_open(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t frequency, uint32_t param) {
    (void)user; (void)index;
    /* OrbisAudioOutParamFormat: S16 mono, stereo, 8ch; float mono, stereo, 8ch; S16 8ch std; float 8ch std. */
    static const uint8_t channels_of[8] = {1, 2, 8, 1, 2, 8, 8, 8};
    const uint32_t format = param & 0xff;
    if (format > 7) return (int32_t)0x80260006;
    AcquireSRWLockExclusive(&audio_lock);
    int id = -1;
    for (int i = 0; i < AUDIO_PORTS && id < 0; ++i)
        if (!ports[i].used) id = i;
    if (id >= 0) {
        AudioPort *p = &ports[id];
        memset(p, 0, sizeof(*p));
        p->used = 1;
        p->grain = grain;
        p->frequency = frequency ? frequency : 48000;
        p->channels = channels_of[format];
        p->is_float = format == 3 || format == 4 || format == 5 || format == 7;
        p->sample_bytes = p->is_float ? 4 : 2;
        /* The non-"std" 8ch layouts put side before back; SDL wants back before side. */
        p->swap_rear = format == 2 || format == 5;
        const uint32_t frames = grain * 3 > AUDIO_MIN_QUEUED_FRAMES ? grain * 3 : AUDIO_MIN_QUEUED_FRAMES;
        p->queue_limit = frames * p->channels * p->sample_bytes;
        static int sdl_ready = -1;
        if (sdl_ready < 0) sdl_ready = SDL_InitSubSystem(SDL_INIT_AUDIO) ? 1 : 0;
        if (sdl_ready) {
            const SDL_AudioSpec spec = {p->is_float ? SDL_AUDIO_F32LE : SDL_AUDIO_S16LE, (int)p->channels, (int)p->frequency};
            p->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
            if (p->stream) SDL_ResumeAudioStreamDevice(p->stream);
        }
        if (!p->stream) printf("HLE: no audio device, port %d is silent: %s\n", id + 1, SDL_GetError());
        if (p->swap_rear) p->scratch = malloc((size_t)grain * p->channels * p->sample_bytes);
    }
    ReleaseSRWLockExclusive(&audio_lock);
    printf("HLE: audio port type %d, %u samples at %u Hz, format %u -> %d\n", type, grain, frequency, format, id + 1);
    return id < 0 ? (int32_t)0x80260005 : id + 1;
}
static ABI int32_t audio_close(int32_t handle) {
    if (handle < 1 || handle > AUDIO_PORTS) return (int32_t)0x80260003;
    AudioPort *p = &ports[handle - 1];
    AcquireSRWLockExclusive(&audio_lock);
    if (p->stream) SDL_DestroyAudioStream(p->stream);
    free(p->scratch);
    p->stream = NULL;
    p->scratch = NULL;
    p->used = 0;
    ReleaseSRWLockExclusive(&audio_lock);
    return 0;
}
/* Logs a port's first sound, so a run shows real samples reaching the output. */
static void note_first_sound(AudioPort *p, int handle, const void *samples, size_t bytes) {
    const uint8_t *b = samples;
    for (size_t i = 0; i < bytes; ++i) {
        if (b[i]) {
            p->heard = 1;
            printf("HLE: audio port %d has sound\n", handle);
            return;
        }
    }
}
static ABI int32_t audio_output(int32_t handle, const void *samples) {
    if (handle < 1 || handle > AUDIO_PORTS || !ports[handle - 1].used) return (int32_t)0x80260003;
    AudioPort *p = &ports[handle - 1];
    if (!samples) return 0; /* NULL: wait for what's queued */
    if (!p->stream) { /* No device: take the time the samples would take, so the game's pacing holds. */
        uint64_t due = p->next_due_us;
        const uint64_t now = now_us();
        if (due > now) Sleep((DWORD)((due - now) / 1000));
        if (due < now) due = now;
        p->next_due_us = due + (uint64_t)p->grain * 1000000 / p->frequency;
        return (int32_t)p->grain;
    }
    const size_t bytes = (size_t)p->grain * p->channels * p->sample_bytes;
    if (!p->heard) note_first_sound(p, handle, samples, bytes);
    /* Like the console, block while the output is full: the device's clock paces the game. A device
     * that has stopped taking samples doesn't hold it for more than a moment. */
    for (int waits = 0; waits < 100 && SDL_GetAudioStreamQueued(p->stream) >= (int)p->queue_limit; ++waits) Sleep(1);
    const void *data = samples;
    if (p->swap_rear) {
        const size_t size = p->sample_bytes, frame = 8 * size;
        const uint8_t *in = samples;
        for (size_t f = 0; f < p->grain; ++f) {
            uint8_t *out = p->scratch + f * frame;
            memcpy(out, in + f * frame, 4 * size);
            memcpy(out + 4 * size, in + f * frame + 6 * size, 2 * size);
            memcpy(out + 6 * size, in + f * frame + 4 * size, 2 * size);
        }
        data = p->scratch;
    }
    SDL_PutAudioStreamData(p->stream, data, (int)bytes);
    return (int32_t)p->grain;
}
typedef struct { int32_t handle; int32_t pad; const void *samples; } AudioParam;
static ABI int32_t audio_outputs(const AudioParam *params, int32_t count) {
    int32_t result = 0;
    for (int32_t i = 0; params && i < count; ++i) result = audio_output(params[i].handle, params[i].samples);
    return result;
}
/* volumes[n] for each channel n set in flags, 32768 being full; a stream has one gain, the loudest. */
static ABI int32_t audio_volume(int32_t handle, int32_t flags, const int32_t *volumes) {
    if (handle < 1 || handle > AUDIO_PORTS || !ports[handle - 1].used) return (int32_t)0x80260003;
    AudioPort *p = &ports[handle - 1];
    if (!p->stream || !volumes) return 0;
    int32_t loudest = -1;
    for (uint32_t c = 0; c < p->channels; ++c)
        if (flags & (1 << c) && volumes[c] > loudest) loudest = volumes[c];
    if (loudest >= 0) SDL_SetAudioStreamGain(p->stream, (float)loudest / 32768.0f);
    return 0;
}

/* ---- the C library's busiest functions (libc.prx's would run in the interpreter) ---- */

static ABI void *host_memcpy(void *d, const void *s, uint64_t n) { return memcpy(d, s, n); }
static ABI void *host_memmove(void *d, const void *s, uint64_t n) { return memmove(d, s, n); }
static ABI void *host_memset(void *d, int v, uint64_t n) { return memset(d, v, n); }
static ABI int host_memcmp(const void *a, const void *b, uint64_t n) { return memcmp(a, b, n); }
static ABI void *host_memchr(const void *s, int c, uint64_t n) { return (void *)memchr(s, c, n); }
static ABI uint64_t host_strlen(const char *s) { return strlen(s); }
static ABI int host_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static ABI int host_strncmp(const char *a, const char *b, uint64_t n) { return strncmp(a, b, n); }
static ABI char *host_strchr(const char *s, int c) { return (char *)strchr(s, c); }
static ABI char *host_strrchr(const char *s, int c) { return (char *)strrchr(s, c); }
static ABI char *host_strstr(const char *a, const char *b) { return (char *)strstr(a, b); }
static ABI char *host_strcpy(char *d, const char *s) { return strcpy(d, s); }
static ABI char *host_strncpy(char *d, const char *s, uint64_t n) { return strncpy(d, s, n); }
static ABI char *host_strcat(char *d, const char *s) { return strcat(d, s); }
static ABI uint64_t host_strspn(const char *a, const char *b) { return strspn(a, b); }
static ABI uint64_t host_strcspn(const char *a, const char *b) { return strcspn(a, b); }

static ABI float host_tanf(float x) { return tanf(x); }
static ABI float host_atanf(float x) { return atanf(x); }
static ABI float host_atan2f(float y, float x) { return atan2f(y, x); }
static ABI float host_acosf(float x) { return acosf(x); }
static ABI float host_asinf(float x) { return asinf(x); }
static ABI float host_expf(float x) { return expf(x); }
static ABI float host_exp2f(float x) { return exp2f(x); }
static ABI float host_fmodf(float x, float y) { return fmodf(x, y); }
static ABI float host_powf(float x, float y) { return powf(x, y); }
static ABI float host_hypotf(float x, float y) { return hypotf(x, y); }
static ABI float host_roundf(float x) { return roundf(x); }
static ABI float host_ldexpf(float x, int e) { return ldexpf(x, e); }
static ABI double host_atan2(double y, double x) { return atan2(y, x); }
static ABI double host_exp2(double x) { return exp2(x); }
static ABI double host_pow(double x, double y) { return pow(x, y); }
static ABI double host_tan(double x) { return tan(x); }
static ABI double host_modf(double x, double *i) { return modf(x, i); }
/* _FSin: sinf and cosf in one; cosf for any nonzero q (bloodborne-runtime's runtime.c, from libc.prx's). */
static ABI float host_fsin(float x, unsigned q) { return q ? cosf(x) : sinf(x); }

const RtExport rt_io_exports[] = {
    {"tanf", host_tanf}, {"atanf", host_atanf}, {"atan2f", host_atan2f}, {"acosf", host_acosf}, {"asinf", host_asinf}, {"expf", host_expf},
    {"exp2f", host_exp2f}, {"fmodf", host_fmodf}, {"powf", host_powf}, {"hypotf", host_hypotf}, {"roundf", host_roundf}, {"ldexpf", host_ldexpf},
    {"atan2", host_atan2}, {"exp2", host_exp2}, {"pow", host_pow}, {"tan", host_tan}, {"modf", host_modf}, {"_FSin", host_fsin},
    {"scePadInit", pad_init}, {"scePadOpen", pad_open}, {"scePadRead", pad_read}, {"scePadReadState", pad_read_state}, {"scePadSetVibration", pad_vibration},
    {"sceAudioOutInit", audio_init}, {"sceAudioOutOpen", audio_open}, {"sceAudioOutClose", audio_close}, {"sceAudioOutOutput", audio_output},
    {"sceAudioOutOutputs", audio_outputs}, {"sceAudioOutSetVolume", audio_volume},
    {"memcpy", host_memcpy}, {"memmove", host_memmove}, {"memset", host_memset}, {"memcmp", host_memcmp}, {"memchr", host_memchr},
    {"strlen", host_strlen}, {"strcmp", host_strcmp}, {"strncmp", host_strncmp}, {"strchr", host_strchr}, {"strrchr", host_strrchr},
    {"strstr", host_strstr}, {"strcpy", host_strcpy}, {"strncpy", host_strncpy}, {"strcat", host_strcat}, {"strspn", host_strspn}, {"strcspn", host_strcspn},
};
const size_t rt_io_export_count = sizeof(rt_io_exports) / sizeof(*rt_io_exports);
