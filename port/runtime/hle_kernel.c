/* PS4 kernel calls on Windows: time, errno, process, files. /app0 is the extracted game folder;
 * other mounts go under a per-user folder. */
#include <windows.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cpu/bbcpu.h"
#include "hle.h"

/* ---- errno ---- */

static _Thread_local int32_t *thread_errno;
static ABI int32_t *guest_errno(void) {
    if (!thread_errno) thread_errno = rt_low_alloc(sizeof(int32_t));
    return thread_errno;
}
/* A POSIX-style call failed: errno set, -1 back. */
static int64_t posix_fail(int e) {
    *guest_errno() = e;
    return -1;
}
/* From an sce* result to a POSIX one. */
static int64_t to_posix(int64_t result) {
    if (result >= 0) return result;
    return posix_fail((int)((uint32_t)result & 0xffff));
}

/* ---- time ---- */

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct { int64_t sec, usec; } GuestTimeval;

static LONGLONG counter_frequency;
static LONGLONG counter_start;
static uint64_t now_counter(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
}
static uint64_t now_realtime_100ns(void) {
    FILETIME t;
    GetSystemTimePreciseAsFileTime(&t);
    return (((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime) - UINT64_C(116444736000000000);
}
static ABI int32_t kernel_clock_gettime(int32_t clock, GuestTimespec *out) {
    if (!out) return ORBIS_ERR(E_FAULT);
    if (clock == 0) { /* CLOCK_REALTIME */
        const uint64_t t = now_realtime_100ns();
        out->sec = (int64_t)(t / 10000000);
        out->nsec = (int64_t)(t % 10000000) * 100;
    } else {
        const uint64_t c = now_counter() - (uint64_t)counter_start;
        out->sec = (int64_t)(c / (uint64_t)counter_frequency);
        out->nsec = (int64_t)((c % (uint64_t)counter_frequency) * 1000000000 / (uint64_t)counter_frequency);
    }
    return 0;
}
static ABI int64_t posix_clock_gettime(int32_t clock, GuestTimespec *out) { return to_posix(kernel_clock_gettime(clock, out)); }
static ABI int32_t kernel_gettimeofday(GuestTimeval *out, void *zone) {
    (void)zone;
    if (!out) return ORBIS_ERR(E_FAULT);
    const uint64_t t = now_realtime_100ns();
    out->sec = (int64_t)(t / 10000000);
    out->usec = (int64_t)(t % 10000000) / 10;
    return 0;
}
static ABI uint64_t process_time(void) {
    const uint64_t c = now_counter() - (uint64_t)counter_start;
    return c / (uint64_t)counter_frequency * 1000000 + (c % (uint64_t)counter_frequency) * 1000000 / (uint64_t)counter_frequency;
}
static ABI uint64_t process_counter(void) { return now_counter() - (uint64_t)counter_start; }
static ABI uint64_t process_frequency(void) { return (uint64_t)counter_frequency; }

static void sleep_us(uint64_t us) {
    if (!us) { SwitchToThread(); return; }
    Sleep((DWORD)((us + 999) / 1000));
}
static ABI int32_t kernel_usleep(uint32_t us) { sleep_us(us); return 0; }
static ABI int64_t posix_nanosleep(const GuestTimespec *request, GuestTimespec *remaining) {
    if (!request) return posix_fail(E_FAULT);
    sleep_us((uint64_t)request->sec * 1000000 + (uint64_t)request->nsec / 1000);
    if (remaining) { remaining->sec = 0; remaining->nsec = 0; }
    return 0;
}
static ABI int32_t posix_getrusage(int32_t who, void *out) {
    (void)who;
    if (out) memset(out, 0, 144);
    return 0;
}
static ABI int32_t local_to_utc(int64_t local, int32_t *dst, int64_t *out, int32_t *zone, int32_t *dst_offset) {
    if (out) *out = local;
    if (dst) *dst = 0;
    if (zone) *zone = 0;
    if (dst_offset) *dst_offset = 0;
    return 0;
}
static ABI int32_t utc_to_local(int64_t utc, int64_t *out, void *timeval_out, int32_t *zone, int32_t *dst_offset) {
    (void)timeval_out;
    if (out) *out = utc;
    if (zone) *zone = 0;
    if (dst_offset) *dst_offset = 0;
    return 0;
}

/* ---- process ---- */

static ABI _Noreturn void guest_exit(int32_t status) {
    char text[64];
    snprintf(text, sizeof(text), "the game called _exit(%d)", status);
    rt_finish(status, text);
}
static ABI int32_t is_neo(void) { return 0; }
static ABI int32_t is_sanitized(void) { return 0; }
static ABI _Noreturn void raise_exception(int32_t a, int32_t b) {
    char text[80];
    snprintf(text, sizeof(text), "the game raised a debug exception (%d, %d)", a, b);
    rt_finish(43, text);
}
static ABI int32_t print_backtrace(void) { puts("HLE: sceKernelPrintBacktraceWithModuleInfo"); return 0; }

/* ---- paths ---- */

static char app0_folder[1024] = ".", user_folder[1024] = "user";

static void ensure_tree(const char *path) {
    char part[1200];
    snprintf(part, sizeof(part), "%s", path);
    for (char *p = part; *p; ++p)
        if ((*p == '/' || *p == '\\') && p > part + 2) {
            const char c = *p;
            *p = 0;
            _mkdir(part);
            *p = c;
        }
}

/* Save data: a folder per save, mounted at /savedataN. */
enum { MOUNTS = 16 };
static struct { int used; char point[16]; char host[1100]; } mounts[MOUNTS];
static SRWLOCK mounts_lock = SRWLOCK_INIT;

/* The host path of a guest path. Returns 0 for one outside anything mapped. */
static int host_path(const char *guest, char *out, size_t size) {
    if (!guest) return 0;
    if (!strncmp(guest, "/savedata", 9)) {
        AcquireSRWLockShared(&mounts_lock);
        for (int i = 0; i < MOUNTS; ++i) {
            const size_t n = strlen(mounts[i].point);
            if (mounts[i].used && !strncmp(guest, mounts[i].point, n) && (guest[n] == '/' || !guest[n])) {
                snprintf(out, size, "%s%s", mounts[i].host, guest + n);
                ReleaseSRWLockShared(&mounts_lock);
                return 1;
            }
        }
        ReleaseSRWLockShared(&mounts_lock);
        return 0;
    }
    if (!strncmp(guest, "/app0", 5) && (guest[5] == '/' || !guest[5])) {
        snprintf(out, size, "%s%s", app0_folder, guest + 5);
        return 1;
    }
    if (guest[0] != '/') {
        snprintf(out, size, "%s/%s", app0_folder, guest);
        return 1;
    }
    snprintf(out, size, "%s/fs%s", user_folder, guest);
    ensure_tree(out);
    return 1;
}

int rt_host_path(const char *guest, char *out, size_t size) { return host_path(guest, out, size); }

void rt_set_folders(const char *app0, const char *user) {
    snprintf(app0_folder, sizeof(app0_folder), "%s", app0);
    snprintf(user_folder, sizeof(user_folder), "%s", user);
}

static int orbis_from_crt(void) {
    switch (errno) {
    case ENOENT: return E_NOENT;
    case EACCES: return E_ACCES;
    case EEXIST: return E_EXIST;
    case EBADF: return E_BADF;
    case EISDIR: return E_ISDIR;
    case ENOTDIR: return E_NOTDIR;
    case EMFILE: return E_MFILE;
    case ENOSPC: return E_NOSPC;
    case ENOTEMPTY: return E_NOTEMPTY;
    case EINVAL: return E_INVAL;
    default: return E_IO;
    }
}

/* ---- files ---- */

/* PS4 open flags (FreeBSD). */
enum { O_PS4_WRONLY = 1, O_PS4_RDWR = 2, O_PS4_APPEND = 8, O_PS4_CREAT = 0x200, O_PS4_TRUNC = 0x400, O_PS4_EXCL = 0x800, O_PS4_DIRECTORY = 0x20000 };

static SRWLOCK file_lock = SRWLOCK_INIT; /* positioned reads and writes seek first */

/* Translation overlays, with --english or --user-tl (analysis\scripts\tl_pack.py): byte ranges laid over
 * an archive as it is read, from <folder>\<disc>.overlay in the working directory. Ranges can reach past the
 * end of the file, which makes it look bigger. The archive itself never changes. */
const char *rt_overlay_folder;
typedef struct { uint64_t offset; uint32_t size, reserved; } OverlayRange;
typedef struct { const char *name; uint32_t count; uint64_t size; const OverlayRange *ranges; const uint8_t *bytes; uint8_t *seen; } Overlay;
enum { OVERLAY_FDS = 1024 };
static const Overlay *fd_overlay[OVERLAY_FDS];

static const Overlay *load_overlay(const char *guest_path) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static Overlay loaded[8];
    static int loaded_count;
    if (!rt_overlay_folder) return NULL;
    const char *base = strrchr(guest_path, '/');
    base = base ? base + 1 : guest_path;
    const size_t length = strlen(base);
    if (length < 5 || strcmp(base + length - 4, ".cpk")) return NULL;
    char name[64];
    snprintf(name, sizeof(name), "%.*s", (int)(length - 4), base);
    const Overlay *result = NULL;
    AcquireSRWLockExclusive(&lock);
    for (int i = 0; i < loaded_count && !result; ++i)
        if (!strcmp(loaded[i].name, name)) result = &loaded[i];
    if (!result && loaded_count < 8) {
        char file[128];
        snprintf(file, sizeof(file), "%s/%s.overlay", rt_overlay_folder, name);
        FILE *f = fopen(file, "rb");
        uint8_t *all = NULL;
        long size = 0;
        if (f && !fseek(f, 0, SEEK_END) && (size = ftell(f)) > 16 && !fseek(f, 0, SEEK_SET) && (all = malloc((size_t)size)) &&
            fread(all, 1, (size_t)size, f) == (size_t)size && !memcmp(all, "VOXP", 4)) {
            Overlay *o = &loaded[loaded_count++];
            o->name = _strdup(name);
            memcpy(&o->count, all + 4, 4);
            memcpy(&o->size, all + 8, 8);
            o->ranges = (const OverlayRange *)(all + 16);
            o->bytes = all + 16 + (size_t)o->count * sizeof(OverlayRange);
            o->seen = calloc(o->count, 1);
            printf("HLE: translation overlay %s for %s: %u ranges\n", rt_overlay_folder, name, o->count);
            result = o;
        }
        if (f) fclose(f);
    }
    ReleaseSRWLockExclusive(&lock);
    return result;
}

/* Lays an overlay's ranges over `size` bytes just read from archive position `position`. */
static void apply_overlay(const Overlay *o, uint64_t position, uint8_t *data, uint64_t size) {
    const uint8_t *bytes = o->bytes;
    for (uint32_t i = 0; i < o->count; ++i) {
        const OverlayRange *r = &o->ranges[i];
        const uint64_t start = r->offset > position ? r->offset : position;
        const uint64_t end = r->offset + r->size < position + size ? r->offset + r->size : position + size;
        if (start < end) {
            memcpy(data + (start - position), bytes + (start - r->offset), (size_t)(end - start));
            if (!o->seen[i]) {
                o->seen[i] = 1;
                printf("HLE: translated file %u of %s read, at %#llx\n", i, o->name, (unsigned long long)r->offset);
            }
        }
        bytes += r->size;
    }
}

static ABI int32_t kernel_open(const char *path, int32_t flags, int32_t mode) {
    (void)mode;
    char host[2048];
    if (!host_path(path, host, sizeof(host))) return ORBIS_ERR(E_NOENT);
    int f = _O_BINARY | _O_NOINHERIT;
    if (flags & O_PS4_RDWR) f |= _O_RDWR;
    else if (flags & O_PS4_WRONLY) f |= _O_WRONLY;
    else f |= _O_RDONLY;
    if (flags & O_PS4_APPEND) f |= _O_APPEND;
    if (flags & O_PS4_CREAT) f |= _O_CREAT;
    if (flags & O_PS4_TRUNC) f |= _O_TRUNC;
    if (flags & O_PS4_EXCL) f |= _O_EXCL;
    const DWORD attributes = GetFileAttributesA(host);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        /* Directory listing is not implemented yet: a handle to NUL says it exists. */
        const int fd = _open("NUL", _O_RDONLY | _O_BINARY);
        printf("HLE: open directory %s -> fd %d\n", path, fd);
        return fd < 0 ? ORBIS_ERR(E_IO) : fd;
    }
    const int fd = _open(host, f, _S_IREAD | _S_IWRITE);
    const int result = fd < 0 ? ORBIS_ERR(orbis_from_crt()) : fd;
    printf("HLE: open %s flags %#x -> %#x\n", path, flags, result);
    if (fd >= 0 && fd < OVERLAY_FDS) fd_overlay[fd] = load_overlay(path);
    return result;
}
static ABI int32_t kernel_close(int32_t fd) {
    if (fd >= 0 && fd < OVERLAY_FDS) fd_overlay[fd] = NULL;
    return _close(fd) ? ORBIS_ERR(E_BADF) : 0;
}
/* Reads through a host buffer: the renderer write-protects guest pages to track them, and a system call
 * writing into a protected page fails instead of raising the fault a user-mode copy does. */
enum { READ_CHUNK = 1024 * 1024 };
static _Thread_local uint8_t *read_buffer;
static int64_t read_into_guest(int32_t fd, uint8_t *data, uint64_t size) {
    if (!read_buffer) read_buffer = malloc(READ_CHUNK);
    if (!read_buffer) return -1;
    const Overlay *overlay = fd >= 0 && fd < OVERLAY_FDS ? fd_overlay[fd] : NULL;
    uint64_t done = 0;
    while (done < size) {
        const unsigned want = size - done < READ_CHUNK ? (unsigned)(size - done) : READ_CHUNK;
        const int64_t position = overlay ? _telli64(fd) : 0;
        int n = _read(fd, read_buffer, want);
        if (n < 0) return done ? (int64_t)done : -1;
        if (overlay && (uint64_t)position + (uint64_t)n < overlay->size && (unsigned)n < want) {
            /* Past the real end of the archive: only the overlay has anything here. */
            const uint64_t left = overlay->size - (uint64_t)position;
            const int extended = left < want ? (int)left : (int)want;
            memset(read_buffer + n, 0, (size_t)(extended - n));
            n = extended;
            _lseeki64(fd, position + n, SEEK_SET);
        }
        if (!n) break;
        if (overlay) apply_overlay(overlay, (uint64_t)position, read_buffer, (uint64_t)n);
        memcpy(data + done, read_buffer, (size_t)n);
        done += (uint64_t)n;
        if ((unsigned)n < want) break;
    }
    return (int64_t)done;
}
static ABI int64_t kernel_read(int32_t fd, void *data, uint64_t size) {
    const int64_t n = read_into_guest(fd, data, size);
    return n < 0 ? ORBIS_ERR(orbis_from_crt()) : n;
}
static ABI int64_t kernel_write(int32_t fd, const void *data, uint64_t size) {
    if (fd == 1 || fd == 2) {
        fwrite(data, 1, size, stdout);
        return (int64_t)size;
    }
    const int n = _write(fd, data, (unsigned)size);
    return n < 0 ? ORBIS_ERR(orbis_from_crt()) : n;
}
static ABI int64_t kernel_lseek(int32_t fd, int64_t offset, int32_t whence) {
    if (whence == SEEK_END && fd >= 0 && fd < OVERLAY_FDS && fd_overlay[fd]) {
        offset += (int64_t)fd_overlay[fd]->size;
        whence = SEEK_SET;
    }
    const int64_t r = _lseeki64(fd, offset, whence);
    return r < 0 ? ORBIS_ERR(orbis_from_crt()) : r;
}
static ABI int64_t kernel_pread(int32_t fd, void *data, uint64_t size, int64_t offset) {
    AcquireSRWLockExclusive(&file_lock);
    const int64_t saved = _lseeki64(fd, 0, SEEK_CUR);
    _lseeki64(fd, offset, SEEK_SET);
    const int64_t n = read_into_guest(fd, data, size);
    _lseeki64(fd, saved, SEEK_SET);
    ReleaseSRWLockExclusive(&file_lock);
    return n < 0 ? ORBIS_ERR(orbis_from_crt()) : n;
}
static ABI int64_t kernel_pwrite(int32_t fd, const void *data, uint64_t size, int64_t offset) {
    AcquireSRWLockExclusive(&file_lock);
    const int64_t saved = _lseeki64(fd, 0, SEEK_CUR);
    _lseeki64(fd, offset, SEEK_SET);
    const int n = _write(fd, data, (unsigned)size);
    _lseeki64(fd, saved, SEEK_SET);
    ReleaseSRWLockExclusive(&file_lock);
    return n < 0 ? ORBIS_ERR(orbis_from_crt()) : n;
}
static ABI int64_t posix_read(int32_t fd, void *data, uint64_t size) { return to_posix(kernel_read(fd, data, size)); }
static ABI int64_t posix_write(int32_t fd, const void *data, uint64_t size) { return to_posix(kernel_write(fd, data, size)); }

/* OrbisKernelStat (FreeBSD's stat) */
typedef struct {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    GuestTimespec atime, mtime, ctime;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t lspare;
    GuestTimespec birthtime;
} GuestStat;

static void fill_stat(GuestStat *out, const struct _stat64 *s) {
    memset(out, 0, sizeof(*out));
    out->mode = (uint16_t)(((s->st_mode & _S_IFDIR) ? 0x4000 : 0x8000) | 0777);
    out->nlink = 1;
    out->size = s->st_size;
    out->blksize = 512;
    out->blocks = (s->st_size + 511) / 512;
    out->atime.sec = s->st_atime;
    out->mtime.sec = s->st_mtime;
    out->ctime.sec = s->st_ctime;
    out->birthtime.sec = s->st_ctime;
}
static ABI int32_t kernel_stat(const char *path, GuestStat *out) {
    char host[2048];
    struct _stat64 s;
    if (!out || !host_path(path, host, sizeof(host))) return ORBIS_ERR(E_INVAL);
    size_t n = strlen(host);
    while (n > 3 && (host[n - 1] == '/' || host[n - 1] == '\\')) host[--n] = 0;
    if (_stat64(host, &s)) {
        printf("HLE: stat %s -> not found\n", path);
        return ORBIS_ERR(orbis_from_crt());
    }
    fill_stat(out, &s);
    const Overlay *overlay = load_overlay(path);
    if (overlay) out->size = (int64_t)overlay->size;
    return 0;
}
static ABI int32_t kernel_check_reachability(const char *path) {
    GuestStat unused;
    return kernel_stat(path, &unused);
}
static ABI int32_t kernel_fstat(int32_t fd, GuestStat *out) {
    struct _stat64 s;
    if (!out) return ORBIS_ERR(E_INVAL);
    if (_fstat64(fd, &s)) return ORBIS_ERR(E_BADF);
    fill_stat(out, &s);
    if (fd >= 0 && fd < OVERLAY_FDS && fd_overlay[fd]) out->size = (int64_t)fd_overlay[fd]->size;
    return 0;
}
static ABI int32_t kernel_fsync(int32_t fd) { return _commit(fd) ? ORBIS_ERR(E_BADF) : 0; }
static ABI int32_t kernel_ftruncate(int32_t fd, int64_t size) { return _chsize_s(fd, size) ? ORBIS_ERR(orbis_from_crt()) : 0; }
static ABI int32_t kernel_truncate(const char *path, int64_t size) {
    char host[2048];
    if (!host_path(path, host, sizeof(host))) return ORBIS_ERR(E_NOENT);
    const int fd = _open(host, _O_WRONLY | _O_BINARY);
    if (fd < 0) return ORBIS_ERR(orbis_from_crt());
    const int e = _chsize_s(fd, size);
    _close(fd);
    return e ? ORBIS_ERR(E_IO) : 0;
}
static ABI int32_t kernel_mkdir(const char *path, int32_t mode) {
    (void)mode;
    char host[2048];
    if (!host_path(path, host, sizeof(host))) return ORBIS_ERR(E_NOENT);
    return _mkdir(host) ? ORBIS_ERR(orbis_from_crt()) : 0;
}
static ABI int32_t kernel_rmdir(const char *path) {
    char host[2048];
    if (!host_path(path, host, sizeof(host))) return ORBIS_ERR(E_NOENT);
    return _rmdir(host) ? ORBIS_ERR(orbis_from_crt()) : 0;
}
static ABI int32_t kernel_unlink(const char *path) {
    char host[2048];
    if (!host_path(path, host, sizeof(host))) return ORBIS_ERR(E_NOENT);
    return _unlink(host) ? ORBIS_ERR(orbis_from_crt()) : 0;
}
static ABI int32_t kernel_rename(const char *from, const char *to) {
    char a[2048], b[2048];
    if (!host_path(from, a, sizeof(a)) || !host_path(to, b, sizeof(b))) return ORBIS_ERR(E_NOENT);
    return rename(a, b) ? ORBIS_ERR(orbis_from_crt()) : 0;
}

/* ---- save data (structures from shadPS4's savedata.cpp) ---- */

enum {
    SAVE_NOT_FOUND = (int32_t)0x809f0008, SAVE_PARAMETER = (int32_t)0x809f0000, SAVE_NOT_MOUNTED = (int32_t)0x809f0004,
    SAVE_MOUNT_FULL = (int32_t)0x809f000c,
    MODE_RDONLY = 1, MODE_RDWR = 2, MODE_CREATE = 4, MODE_CREATE2 = 32,
};
typedef struct { int32_t user, pad; const char *directory; uint64_t blocks; uint32_t mode; uint8_t reserved[36]; } SaveMount2;
typedef struct { char point[16]; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[32]; } SaveMountResult;
typedef struct { int32_t user, pad; const void *title; const char *directory; uint32_t unused; uint8_t reserved[36]; } SaveDelete;
typedef struct { void *data; uint64_t size, data_size; uint8_t reserved[32]; } SaveIcon;

static void save_folder(const char *directory, char *out, size_t size) {
    snprintf(out, size, "%s/savedata/%s", user_folder, directory);
}
static int folder_exists(const char *path) {
    const DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static int mounted_folder(const char *point, char *out, size_t size) {
    int found = 0;
    AcquireSRWLockShared(&mounts_lock);
    for (int i = 0; i < MOUNTS && !found; ++i)
        if (mounts[i].used && !strcmp(mounts[i].point, point)) {
            snprintf(out, size, "%s", mounts[i].host);
            found = 1;
        }
    ReleaseSRWLockShared(&mounts_lock);
    return found;
}
static ABI int32_t save_initialize(const void *param) { (void)param; return 0; }
static ABI int32_t save_mount2(const SaveMount2 *mount, SaveMountResult *result) {
    if (!mount || !mount->directory || !result) return SAVE_PARAMETER;
    char folder[1100], probe[1200];
    save_folder(mount->directory, folder, sizeof(folder));
    const int exists = folder_exists(folder), create = (mount->mode & (MODE_CREATE | MODE_CREATE2)) != 0;
    static volatile long logged;
    if (InterlockedIncrement(&logged) <= 12)
        printf("HLE: save mount \"%s\" mode %#x blocks %#llx, exists %d, from +%llx\n", mount->directory, mount->mode, (unsigned long long)mount->blocks, exists,
               (unsigned long long)(bbcpu_import_return() - bb_image_base));
    if (!exists && !create) return SAVE_NOT_FOUND;
    if (!exists) {
        snprintf(probe, sizeof(probe), "%s/", folder);
        ensure_tree(probe);
    }
    AcquireSRWLockExclusive(&mounts_lock);
    int slot = -1;
    for (int i = 0; i < MOUNTS && slot < 0; ++i)
        if (!mounts[i].used) slot = i;
    if (slot >= 0) {
        mounts[slot].used = 1;
        snprintf(mounts[slot].point, sizeof(mounts[slot].point), "/savedata%d", slot);
        snprintf(mounts[slot].host, sizeof(mounts[slot].host), "%s", folder);
    }
    ReleaseSRWLockExclusive(&mounts_lock);
    if (slot < 0) return SAVE_MOUNT_FULL;
    memset(result, 0, sizeof(*result));
    snprintf(result->point, sizeof(result->point), "/savedata%d", slot);
    result->status = exists ? 0 : 1;
    return 0;
}
static ABI int32_t save_umount(const char *point) {
    if (!point) return SAVE_PARAMETER;
    int found = 0;
    AcquireSRWLockExclusive(&mounts_lock);
    for (int i = 0; i < MOUNTS; ++i)
        if (mounts[i].used && !strcmp(mounts[i].point, point)) {
            mounts[i].used = 0;
            found = 1;
        }
    ReleaseSRWLockExclusive(&mounts_lock);
    return found ? 0 : SAVE_NOT_MOUNTED;
}
/* A parameter of each type in its own small file beside the save. */
static ABI int32_t save_set_param(const char *point, uint32_t type, const void *data, uint64_t size) {
    char folder[1100], path[1200];
    if (!point || !data) return SAVE_PARAMETER;
    if (!mounted_folder(point, folder, sizeof(folder))) return SAVE_NOT_MOUNTED;
    snprintf(path, sizeof(path), "%s/.param%u", folder, type);
    FILE *f = fopen(path, "wb");
    if (!f) return SAVE_PARAMETER;
    fwrite(data, 1, size, f);
    fclose(f);
    return 0;
}
static ABI int32_t save_get_param(const char *point, uint32_t type, void *out, uint64_t size, uint64_t *got) {
    char folder[1100], path[1200];
    if (!point || !out) return SAVE_PARAMETER;
    if (!mounted_folder(point, folder, sizeof(folder))) return SAVE_NOT_MOUNTED;
    memset(out, 0, size);
    snprintf(path, sizeof(path), "%s/.param%u", folder, type);
    FILE *f = fopen(path, "rb");
    const uint64_t n = f ? fread(out, 1, size, f) : 0;
    if (f) fclose(f);
    if (got) *got = n;
    return 0;
}
static ABI int32_t save_load_icon(const char *point, SaveIcon *icon) {
    (void)point;
    if (icon) icon->data_size = 0;
    return SAVE_NOT_FOUND;
}
static ABI int32_t save_delete(const SaveDelete *request) {
    if (!request || !request->directory) return SAVE_PARAMETER;
    char folder[1100], pattern[1200];
    save_folder(request->directory, folder, sizeof(folder));
    if (!folder_exists(folder)) return SAVE_NOT_FOUND;
    printf("HLE: save delete \"%s\"\n", request->directory);
    snprintf(pattern, sizeof(pattern), "%s/*", folder);
    WIN32_FIND_DATAA found;
    const HANDLE h = FindFirstFileA(pattern, &found);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char file[1500];
            snprintf(file, sizeof(file), "%s/%s", folder, found.cFileName);
            DeleteFileA(file);
        } while (FindNextFileA(h, &found));
        FindClose(h);
    }
    RemoveDirectoryA(folder);
    return 0;
}

void rt_kernel_init(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    counter_frequency = f.QuadPart;
    counter_start = c.QuadPart;
}

const RtExport rt_kernel_exports[] = {
    {"__error", guest_errno},
    {"sceKernelClockGettime", kernel_clock_gettime}, {"clock_gettime", posix_clock_gettime},
    {"sceKernelGettimeofday", kernel_gettimeofday}, {"gettimeofday", kernel_gettimeofday},
    {"sceKernelGetProcessTime", process_time}, {"sceKernelGetProcessTimeCounter", process_counter},
    {"sceKernelGetProcessTimeCounterFrequency", process_frequency}, {"sceKernelUsleep", kernel_usleep},
    {"nanosleep", posix_nanosleep}, {"_nanosleep", posix_nanosleep}, {"getrusage", posix_getrusage},
    {"sceKernelConvertLocaltimeToUtc", local_to_utc}, {"sceKernelConvertUtcToLocaltime", utc_to_local},
    {"_exit", guest_exit}, {"sceKernelIsNeoMode", is_neo}, {"sceKernelIsAddressSanitizerEnabled", is_sanitized},
    {"sceKernelDebugRaiseException", raise_exception}, {"sceKernelDebugRaiseExceptionOnReleaseMode", raise_exception},
    {"sceKernelPrintBacktraceWithModuleInfo", print_backtrace},
    {"sceKernelOpen", kernel_open}, {"sceKernelClose", kernel_close}, {"sceKernelRead", kernel_read}, {"sceKernelWrite", kernel_write},
    {"sceKernelLseek", kernel_lseek}, {"sceKernelPread", kernel_pread}, {"sceKernelPwrite", kernel_pwrite},
    {"_read", posix_read}, {"_write", posix_write}, {"sceKernelStat", kernel_stat}, {"sceKernelFstat", kernel_fstat},
    {"sceKernelCheckReachability", kernel_check_reachability},
    {"sceKernelFsync", kernel_fsync}, {"sceKernelFtruncate", kernel_ftruncate}, {"sceKernelTruncate", kernel_truncate},
    {"sceSaveDataInitialize3", save_initialize}, {"sceSaveDataTerminate", save_initialize}, {"sceSaveDataMount2", save_mount2},
    {"sceSaveDataUmount", save_umount}, {"sceSaveDataSetParam", save_set_param}, {"sceSaveDataGetParam", save_get_param},
    {"sceSaveDataLoadIcon", save_load_icon}, {"sceSaveDataDelete", save_delete},
    {"sceKernelMkdir", kernel_mkdir}, {"sceKernelRmdir", kernel_rmdir}, {"sceKernelUnlink", kernel_unlink}, {"sceKernelRename", kernel_rename},
};
const size_t rt_kernel_export_count = sizeof(rt_kernel_exports) / sizeof(*rt_kernel_exports);
