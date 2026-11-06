/* AvPlayer. Movies play here through Windows' Media Foundation, with a thread each for picture and sound: looping
 * ones are menu and scene backgrounds, and one-shot ones are story scenes. Most have no sound track at all. If a
 * looping movie can't be decoded it delivers one black frame, since the game holds a scene change until a frame
 * has arrived. A one-shot movie that can't be decoded just ends. */
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

enum {
    BLACK_FRAME_SIZE = 64, MOVIE_PATH = 1024, POOL = 16,
    /* The game's audio thread outputs 1024 samples at a time; 8 channels of 16 bits at most. */
    AUDIO_SAMPLES = 1024, AUDIO_CHUNK_MAX = AUDIO_SAMPLES * 8 * 2, AUDIO_RING = 512 * 1024,
};

typedef struct {
    int looping, active, frame_due;
    volatile LONG stop, failed;
    char path[MOVIE_PATH];
    HANDLE thread, audio_thread;
    SRWLOCK lock;
    uint8_t *buffers[2];
    size_t buffer_size;
    int front, fresh;
    uint32_t width, height;
    /* Decoded sound, from audio_thread to the game's audio thread. */
    uint8_t *ring, *audio_frame;
    size_t ring_read, ring_filled;
    volatile LONG channels, sample_rate;
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

/* The PS4's decoder gives frames a pitch aligned to 64 and a height aligned to 16, with crop values saying which
 * part is the picture. Games build their textures from that, so 1080 lines would leave a texture reaching past
 * the data (all zero is bright green in YUV). */
static uint32_t frame_pitch(const AvPlayer *p) { return (p->width + 63) & ~63u; }
static uint32_t frame_height(const AvPlayer *p) { return (p->height + 15) & ~15u; }

/* One plane: the picture's rows, each padded by repeating its last sample, then repeats of the last row. */
static void copy_plane(uint8_t *to, uint32_t pitch, uint32_t rows, const uint8_t *from, uint32_t stride, uint32_t width, uint32_t height, uint32_t sample) {
    for (uint32_t row = 0; row < height; ++row) {
        uint8_t *line = to + row * pitch;
        memcpy(line, from + row * stride, width);
        for (uint32_t at = width; at + sample <= pitch; at += sample) memcpy(line + at, line + width - sample, sample);
    }
    for (uint32_t row = height; row < rows; ++row) memcpy(to + row * pitch, to + (height - 1) * pitch, pitch);
}

/* Copies a decoded NV12 picture (coded stride and height) into the back buffer, then makes it the front. */
static void publish(AvPlayer *p, const uint8_t *source, uint32_t stride, uint32_t coded_height, uint32_t x, uint32_t y) {
    uint8_t *to = p->buffers[!p->front];
    const uint32_t pitch = frame_pitch(p), rows = frame_height(p);
    copy_plane(to, pitch, rows, source + y * stride + x, stride, p->width, p->height, 1);
    copy_plane(to + pitch * rows, pitch, rows / 2, source + coded_height * stride + (y / 2) * stride + (x & ~1u), stride, p->width, p->height / 2, 2);
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
    p->buffer_size = (size_t)frame_pitch(p) * frame_height(p) * 3 / 2;
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
            if (!p->looping) {
                p->active = 0; /* the game sees it has ended */
                return 1;
            }
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
                /* The decoder's rows can outnumber what the media type says (1088 for 1080): only the
                 * buffer's size tells where the chroma plane starts. */
                const uint32_t rows = (uint32_t)((uint64_t)length * 2 / 3 / stride);
                if (rows >= coded_height) {
                    publish(p, data, stride, rows, x, y);
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

/* Copies decoded PCM into the ring, waiting while it's full. */
static void ring_push(AvPlayer *p, const uint8_t *data, size_t size) {
    while (size && !p->stop) {
        AcquireSRWLockExclusive(&p->lock);
        const size_t room = AUDIO_RING - p->ring_filled;
        const size_t write_at = (p->ring_read + p->ring_filled) % AUDIO_RING;
        size_t take = size < room ? size : room;
        if (take > AUDIO_RING - write_at) take = AUDIO_RING - write_at;
        memcpy(p->ring + write_at, data, take);
        p->ring_filled += take;
        ReleaseSRWLockExclusive(&p->lock);
        data += take;
        size -= take;
        if (size) Sleep(5);
    }
}

static DWORD WINAPI audio_thread(void *argument) {
    AvPlayer *p = argument;
    const DWORD stream = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    WCHAR wide[MOVIE_PATH], full[MOVIE_PATH];
    IMFSourceReader *reader = NULL;
    IMFMediaType *type = NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, p->path, -1, wide, MOVIE_PATH) && GetFullPathNameW(wide, MOVIE_PATH, full, NULL) &&
        SUCCEEDED(MFCreateSourceReaderFromURL(full, NULL, &reader))) {
        IMFSourceReader_SetStreamSelection(reader, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        /* Selecting fails for a movie with no sound track, which is most of them. */
        if (SUCCEEDED(IMFSourceReader_SetStreamSelection(reader, stream, TRUE)) && SUCCEEDED(MFCreateMediaType(&type))) {
            IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
            IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
            IMFMediaType_SetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            const HRESULT set = IMFSourceReader_SetCurrentMediaType(reader, stream, NULL, type);
            IMFMediaType_Release(type);
            type = NULL;
            UINT32 channels = 0, rate = 0;
            if (SUCCEEDED(set) && SUCCEEDED(IMFSourceReader_GetCurrentMediaType(reader, stream, &type))) {
                IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &channels);
                IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                IMFMediaType_Release(type);
            }
            if (channels >= 1 && channels <= 8 && rate) {
                printf("HLE: movie sound, %u channels at %u Hz: %s\n", channels, rate, p->path);
                InterlockedExchange(&p->channels, (LONG)channels);
                InterlockedExchange(&p->sample_rate, (LONG)rate);
                while (!p->stop) {
                    DWORD flags = 0;
                    IMFSample *sample = NULL;
                    if (FAILED(IMFSourceReader_ReadSample(reader, stream, 0, NULL, &flags, NULL, &sample))) break;
                    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                        if (sample) IMFSample_Release(sample);
                        if (!p->looping) break;
                        PROPVARIANT position;
                        PropVariantInit(&position);
                        position.vt = VT_I8;
                        position.hVal.QuadPart = 0;
                        if (FAILED(IMFSourceReader_SetCurrentPosition(reader, &GUID_NULL, &position))) break;
                        continue;
                    }
                    if (!sample) continue;
                    IMFMediaBuffer *buffer = NULL;
                    if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buffer))) {
                        BYTE *data = NULL;
                        DWORD length = 0;
                        if (SUCCEEDED(IMFMediaBuffer_Lock(buffer, &data, NULL, &length))) {
                            ring_push(p, data, length);
                            IMFMediaBuffer_Unlock(buffer);
                        }
                        IMFMediaBuffer_Release(buffer);
                    }
                    IMFSample_Release(sample);
                }
            }
        }
        IMFSourceReader_Release(reader);
    }
    CoUninitialize();
    return 0;
}

static DWORD WINAPI movie_thread(void *argument) {
    AvPlayer *p = argument;
    int ok = 0;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
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
        printf("HLE: movie not decoded: %s\n", p->path);
        InterlockedExchange(&p->failed, 1);
        if (!p->looping) p->active = 0;
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
    if (p->audio_thread) {
        WaitForSingleObject(p->audio_thread, INFINITE);
        CloseHandle(p->audio_thread);
        p->audio_thread = NULL;
    }
    AcquireSRWLockExclusive(&p->lock);
    p->ring_read = p->ring_filled = 0;
    ReleaseSRWLockExclusive(&p->lock);
    p->channels = p->sample_rate = 0;
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
    static volatile LONG started;
    if (!InterlockedExchange(&started, 1)) MFStartup(MF_VERSION, MFSTARTUP_LITE);
    char host[MOVIE_PATH] = "";
    rt_host_path(path, host, sizeof(host));
    snprintf(player->path, sizeof(player->path), "%s", host);
    player->frame_due = 1;
    player->active = 1;
    player->stop = player->failed = 0;
    if (!player->ring) {
        player->ring = malloc(AUDIO_RING);
        player->audio_frame = rt_low_alloc(AUDIO_CHUNK_MAX);
    }
    if (!player->thread) {
        player->thread = CreateThread(NULL, 0, movie_thread, player, 0, NULL);
        if (player->thread && player->ring) player->audio_thread = CreateThread(NULL, 0, audio_thread, player, 0, NULL);
    }
    if (!player->thread) {
        player->failed = 1;
        player->active = 0;
    }
    return 0;
}

/* Fills what the game reads of OrbisAvPlayerFrameInfo: 1024 samples of 16-bit PCM. Nothing yet, or nothing at all
 * for a movie with no sound: the game's audio thread outputs silence instead. */
ABI uint8_t rt_avplayer_audio_data(AvPlayer *player, uint8_t *frame) {
    const uint32_t channels = (uint32_t)player->channels, rate = (uint32_t)player->sample_rate;
    if (!player->active || !channels || !player->ring) return 0;
    const size_t size = (size_t)AUDIO_SAMPLES * channels * 2;
    AcquireSRWLockExclusive(&player->lock);
    const int ready = player->ring_filled >= size;
    if (ready) {
        size_t first = AUDIO_RING - player->ring_read;
        if (first > size) first = size;
        memcpy(player->audio_frame, player->ring + player->ring_read, first);
        memcpy(player->audio_frame + first, player->ring, size - first);
        player->ring_read = (player->ring_read + size) % AUDIO_RING;
        player->ring_filled -= size;
    }
    ReleaseSRWLockExclusive(&player->lock);
    if (!ready) return 0;
    const uint8_t *data = player->audio_frame;
    const uint16_t count = (uint16_t)channels;
    const uint32_t bytes = (uint32_t)size;
    memcpy(frame, &data, 8);
    memcpy(frame + 0x18, &count, 2);
    memcpy(frame + 0x1c, &rate, 4);
    memcpy(frame + 0x20, &bytes, 4);
    return 1;
}
ABI int32_t rt_avplayer_set_looping(AvPlayer *player, uint8_t looping) {
    player->looping = looping != 0;
    if (looping) player->active = 1;
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
static void fill_frame(uint8_t *frame, const uint8_t *data, uint32_t width, uint32_t height, uint32_t pitch, uint32_t rows) {
    const uint32_t aligned = (width + 15) & ~15u, crop_right = pitch - width, crop_bottom = rows - height;
    memcpy(frame, &data, 8);
    memcpy(frame + 0x18, &aligned, 4);
    memcpy(frame + 0x1c, &rows, 4);
    memset(frame + 0x2c, 0, 12);
    memcpy(frame + 0x30, &crop_right, 4);
    memcpy(frame + 0x38, &crop_bottom, 4);
    memcpy(frame + 0x3c, &pitch, 4);
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
        fill_frame(frame, black, BLACK_FRAME_SIZE, BLACK_FRAME_SIZE, BLACK_FRAME_SIZE, BLACK_FRAME_SIZE);
        player->frame_due = 0;
        return 1;
    }
    AcquireSRWLockExclusive(&player->lock);
    const int fresh = player->fresh;
    player->fresh = 0;
    const uint8_t *picture = player->buffers[player->front];
    ReleaseSRWLockExclusive(&player->lock);
    if (!fresh) return 0;
    fill_frame(frame, picture, player->width, player->height, frame_pitch(player), frame_height(player));
    player->frame_due = 0;
    return 1;
}
