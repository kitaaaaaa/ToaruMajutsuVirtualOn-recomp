/* AvPlayer. A looping movie is a menu's or a scene's background: it plays here, video only, through Windows'
 * Media Foundation on a thread of its own. If it can't be decoded it delivers one black frame, since the game
 * holds a scene change until a frame has arrived. A one-shot movie is never active, so it ends at once. */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>
#include <string.h>

#include "hle.h"

enum { BLACK_FRAME_SIZE = 64, MOVIE_PATH = 1024, POOL = 16 };

typedef struct {
    int looping, active, frame_due;
    volatile LONG stop, failed;
    char path[MOVIE_PATH];
    HANDLE thread;
    SRWLOCK lock;
    uint8_t *buffers[2];
    size_t buffer_size;
    int front, fresh;
    uint32_t width, height;
} AvPlayer;

/* Frame buffers live below the guest's address limit and can't be freed, so they're reused. */
static struct { uint8_t *data; size_t size; } pool[POOL];
static SRWLOCK pool_lock = SRWLOCK_INIT;

static uint8_t *buffer_get(size_t size) {
    AcquireSRWLockExclusive(&pool_lock);
    for (int i = 0; i < POOL; ++i) {
        if (pool[i].data && pool[i].size == size) {
            uint8_t *data = pool[i].data;
            pool[i].data = NULL;
            ReleaseSRWLockExclusive(&pool_lock);
            return data;
        }
    }
    ReleaseSRWLockExclusive(&pool_lock);
    return rt_low_alloc(size);
}
static void buffer_put(uint8_t *data, size_t size) {
    if (!data) return;
    AcquireSRWLockExclusive(&pool_lock);
    for (int i = 0; i < POOL; ++i) {
        if (!pool[i].data) {
            pool[i].data = data;
            pool[i].size = size;
            break;
        }
    }
    ReleaseSRWLockExclusive(&pool_lock);
}

/* Copies a decoded NV12 picture (coded stride and height) into the back buffer, then makes it the front. */
static void publish(AvPlayer *p, const uint8_t *source, uint32_t stride, uint32_t coded_height, uint32_t x, uint32_t y) {
    uint8_t *to = p->buffers[!p->front];
    for (uint32_t row = 0; row < p->height; ++row) memcpy(to + row * p->width, source + (y + row) * stride + x, p->width);
    const uint8_t *chroma = source + coded_height * stride;
    to += p->width * p->height;
    for (uint32_t row = 0; row < p->height / 2; ++row) memcpy(to + row * p->width, chroma + (y / 2 + row) * stride + (x & ~1u), p->width);
    AcquireSRWLockExclusive(&p->lock);
    p->front = !p->front;
    p->fresh = 1;
    ReleaseSRWLockExclusive(&p->lock);
}

static int play(AvPlayer *p, IMFSourceReader *reader) {
    const DWORD stream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    IMFMediaType *type = NULL;
    IMFSourceReader_SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(reader, stream, TRUE);
    if (FAILED(MFCreateMediaType(&type))) return 0;
    IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    const HRESULT set = IMFSourceReader_SetCurrentMediaType(reader, stream, NULL, type);
    IMFMediaType_Release(type);
    if (FAILED(set) || FAILED(IMFSourceReader_GetCurrentMediaType(reader, stream, &type))) return 0;

    UINT64 frame_size = 0;
    UINT32 stride = 0;
    IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &frame_size);
    const uint32_t coded_width = (uint32_t)(frame_size >> 32), coded_height = (uint32_t)frame_size;
    if (FAILED(IMFMediaType_GetUINT32(type, &MF_MT_DEFAULT_STRIDE, &stride)) || (int32_t)stride <= 0) stride = coded_width;
    MFVideoArea area;
    uint32_t x = 0, y = 0;
    p->width = coded_width;
    p->height = coded_height;
    if (SUCCEEDED(IMFMediaType_GetBlob(type, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), NULL))) {
        x = (uint32_t)area.OffsetX.value;
        y = (uint32_t)area.OffsetY.value;
        p->width = (uint32_t)area.Area.cx;
        p->height = (uint32_t)area.Area.cy;
    }
    IMFMediaType_Release(type);
    p->width &= ~1u;
    p->height &= ~1u;
    if (!p->width || !p->height) return 0;
    p->buffer_size = (size_t)p->width * p->height * 3 / 2;
    p->buffers[0] = buffer_get(p->buffer_size);
    p->buffers[1] = buffer_get(p->buffer_size);
    printf("HLE: movie playing, %ux%u: %s\n", p->width, p->height, p->path);

    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    int frames = 0;
    while (!p->stop) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample *sample = NULL;
        if (FAILED(IMFSourceReader_ReadSample(reader, stream, 0, NULL, &flags, &timestamp, &sample))) return frames > 0;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sample) IMFSample_Release(sample);
            if (!frames) return 0;
            PROPVARIANT position;
            PropVariantInit(&position);
            position.vt = VT_I8;
            position.hVal.QuadPart = 0;
            if (FAILED(IMFSourceReader_SetCurrentPosition(reader, &GUID_NULL, &position))) return 1;
            QueryPerformanceCounter(&start);
            continue;
        }
        if (!sample) continue;
        /* Show each picture at its time. */
        for (;;) {
            QueryPerformanceCounter(&now);
            const int64_t due_ms = timestamp / 10000 - (now.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart;
            if (due_ms <= 0 || p->stop) break;
            Sleep(due_ms > 50 ? 50 : (DWORD)due_ms);
        }
        IMFMediaBuffer *buffer = NULL;
        if (!p->stop && SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buffer))) {
            BYTE *data = NULL;
            DWORD length = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(buffer, &data, NULL, &length))) {
                if ((uint64_t)stride * coded_height * 3 / 2 <= length) {
                    publish(p, data, stride, coded_height, x, y);
                    ++frames;
                }
                IMFMediaBuffer_Unlock(buffer);
            }
            IMFMediaBuffer_Release(buffer);
        }
        IMFSample_Release(sample);
    }
    return 1;
}

static DWORD WINAPI movie_thread(void *argument) {
    AvPlayer *p = argument;
    static volatile LONG started;
    int ok = 0;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (!InterlockedExchange(&started, 1)) MFStartup(MF_VERSION, MFSTARTUP_LITE);
    WCHAR wide[MOVIE_PATH], full[MOVIE_PATH];
    IMFAttributes *attributes = NULL;
    IMFSourceReader *reader = NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, p->path, -1, wide, MOVIE_PATH) && GetFullPathNameW(wide, MOVIE_PATH, full, NULL) &&
        SUCCEEDED(MFCreateAttributes(&attributes, 1))) {
        IMFAttributes_SetUINT32(attributes, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        const HRESULT made = MFCreateSourceReaderFromURL(full, attributes, &reader);
        if (SUCCEEDED(made)) {
            ok = play(p, reader);
            IMFSourceReader_Release(reader);
        } else {
            printf("HLE: movie not opened (%#lx): %s\n", (unsigned long)made, p->path);
        }
        IMFAttributes_Release(attributes);
    }
    if (!ok) {
        printf("HLE: movie not decoded, showing black: %s\n", p->path);
        InterlockedExchange(&p->failed, 1);
    }
    CoUninitialize();
    return 0;
}

static void stop_thread(AvPlayer *p) {
    if (!p->thread) return;
    InterlockedExchange(&p->stop, 1);
    WaitForSingleObject(p->thread, INFINITE);
    CloseHandle(p->thread);
    p->thread = NULL;
    buffer_put(p->buffers[0], p->buffer_size);
    buffer_put(p->buffers[1], p->buffer_size);
    p->buffers[0] = p->buffers[1] = NULL;
}

ABI void *rt_avplayer_init(const void *data) {
    (void)data;
    AvPlayer *player = rt_low_alloc(sizeof(AvPlayer));
    memset(player, 0, sizeof(*player));
    InitializeSRWLock(&player->lock);
    return player;
}
ABI int32_t rt_avplayer_add_source(AvPlayer *player, const char *path) {
    char host[MOVIE_PATH] = "";
    rt_host_path(path, host, sizeof(host));
    snprintf(player->path, sizeof(player->path), "%s", host);
    player->frame_due = 1;
    return 0;
}
ABI int32_t rt_avplayer_set_looping(AvPlayer *player, uint8_t looping) {
    player->looping = player->active = looping != 0;
    if (looping && !player->thread) {
        player->stop = player->failed = 0;
        player->thread = CreateThread(NULL, 0, movie_thread, player, 0, NULL);
        if (!player->thread) player->failed = 1;
    }
    return 0;
}
ABI int32_t rt_avplayer_stop(AvPlayer *player) {
    player->active = 0;
    stop_thread(player);
    return 0;
}
ABI int32_t rt_avplayer_close(AvPlayer *player) {
    player->active = 0;
    stop_thread(player);
    return 0;
}
ABI uint8_t rt_avplayer_is_active(const AvPlayer *player) { return player->active != 0; }

/* Fills what the game reads of OrbisAvPlayerFrameInfoEx; the picture is NV12, luma then chroma. */
static void fill_frame(uint8_t *frame, const uint8_t *data, uint32_t width, uint32_t height) {
    memcpy(frame, &data, 8);
    memcpy(frame + 0x18, &width, 4);
    memcpy(frame + 0x1c, &height, 4);
    memcpy(frame + 0x3c, &width, 4);
}
ABI uint8_t rt_avplayer_video_data(AvPlayer *player, uint8_t *frame) {
    static uint8_t *black;
    if (!player->active) return 0;
    if (player->failed || !player->thread) {
        if (!player->frame_due) return 0;
        if (!black) {
            black = rt_low_alloc(BLACK_FRAME_SIZE * BLACK_FRAME_SIZE * 3 / 2);
            memset(black, 16, BLACK_FRAME_SIZE * BLACK_FRAME_SIZE);
            memset(black + BLACK_FRAME_SIZE * BLACK_FRAME_SIZE, 128, BLACK_FRAME_SIZE * BLACK_FRAME_SIZE / 2);
        }
        fill_frame(frame, black, BLACK_FRAME_SIZE, BLACK_FRAME_SIZE);
        player->frame_due = 0;
        return 1;
    }
    AcquireSRWLockExclusive(&player->lock);
    const int fresh = player->fresh;
    player->fresh = 0;
    const uint8_t *picture = player->buffers[player->front];
    ReleaseSRWLockExclusive(&player->lock);
    if (!fresh) return 0;
    fill_frame(frame, picture, player->width, player->height);
    player->frame_due = 0;
    return 1;
}
