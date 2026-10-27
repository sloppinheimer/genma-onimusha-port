// Linux: the NT native calls kernel.c makes (its NTFN list), done on POSIX. The structures are NT's, as kernel.c
// passes the game's straight through for the classes the two share (its nameless_class). Names: NT_USTR's Length is in
// 2-byte units as on Windows, over a wchar_t (4-byte here) buffer; directory entries are written the same way (the
// name's wchar_t from 0x40, FileNameLength = characters * 2), which is how kernel.c reads them back.
#define _GNU_SOURCE
#include <windows.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "compat_handle.h"

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } NT_USTR;
typedef struct { ULONG Length; HANDLE RootDirectory; NT_USTR *ObjectName; ULONG Attributes; PVOID Sd, Qos; } NT_OA;
typedef struct { union { LONG Status; PVOID Pointer; }; ULONG_PTR Information; } NT_IOSB;

static NTSTATUS status_of_errno(void)
{
    return errno == ENOENT ? STATUS_OBJECT_NAME_NOT_FOUND : errno == ENOTDIR ? STATUS_OBJECT_PATH_NOT_FOUND : errno == EEXIST
         ? STATUS_OBJECT_NAME_COLLISION : errno == EACCES || errno == EPERM ? STATUS_ACCESS_DENIED : errno == EISDIR
         ? STATUS_FILE_IS_A_DIRECTORY : errno == ENOSPC ? STATUS_DISK_FULL : errno == ENOTEMPTY ? STATUS_DIRECTORY_NOT_EMPTY
         : STATUS_UNSUCCESSFUL;
}
static LARGE_INTEGER nt_time(const struct timespec *t)
{
    LARGE_INTEGER v;
    v.QuadPart = (int64_t)t->tv_sec * 10000000 + t->tv_nsec / 100 + 116444736000000000ll;
    return v;
}
static ULONG nt_attrs(const struct stat *st)
{
    return S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

// ---- files --------------------------------------------------------------------------------------------------------
// Game code calls in on its own stacks (64 KB, much of it already in use), so the name buffers are on the heap.
struct create_buffers { wchar_t name[2048], whole[4096]; char rel[2048], joined[4096], p[4096]; };
static NTSTATUS create_file(HANDLE *out, ULONG access, NT_OA *oa, NT_IOSB *iosb, ULONG disp, ULONG opts, struct create_buffers *b)
{
    wchar_t *name = b->name;
    size_t len = oa->ObjectName ? oa->ObjectName->Length / 2 : 0;
    if (len >= 2048) return STATUS_OBJECT_NAME_INVALID;
    wmemcpy(name, oa->ObjectName->Buffer, len);
    name[len] = 0;
    char *p = b->p;
    if (oa->RootDirectory) {   // relative to an open folder
        struct compat_handle *r = oa->RootDirectory;
        char *rel = b->rel;
        WideCharToMultiByte(CP_UTF8, 0, name, -1, rel, sizeof b->rel, 0, 0);
        for (char *c = rel; *c; c++) if (*c == '\\') *c = '/';
        snprintf(b->joined, sizeof b->joined, "%s/%s", r->path, rel[0] == '/' ? rel + 1 : rel);
        MultiByteToWideChar(CP_UTF8, 0, b->joined, -1, b->whole, 4096);
        compat_path(b->whole, p, sizeof b->p);
    } else if (!wcscasecmp(name, L"\\Device\\Null")) strcpy(p, "/dev/null");
    else compat_path(wcsncmp(name, L"\\??\\", 4) ? name : name + 4, p, sizeof b->p);

    struct stat st;
    int existed = !stat(p, &st), dir = existed ? S_ISDIR(st.st_mode) : (opts & FILE_DIRECTORY_FILE) != 0;
    ULONG info = 1 /* FILE_OPENED */;
    if ((opts & FILE_DIRECTORY_FILE) && existed && !S_ISDIR(st.st_mode)) return STATUS_NOT_A_DIRECTORY;
    if ((opts & FILE_NON_DIRECTORY_FILE) && existed && S_ISDIR(st.st_mode)) return STATUS_FILE_IS_A_DIRECTORY;
    if (!existed && (disp == FILE_OPEN || disp == FILE_OVERWRITE)) {
        char *s = strrchr(p, '/');
        if (s && s != p) { *s = 0; int parent = !stat(p, &st); *s = '/'; if (!parent) return STATUS_OBJECT_PATH_NOT_FOUND; }
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (existed && disp == FILE_CREATE) return STATUS_OBJECT_NAME_COLLISION;
    struct compat_handle *h;
    if (dir) {
        if (!existed) {
            if (mkdir(p, 0755)) return status_of_errno();
            info = 2 /* FILE_CREATED */;
        }
        h = compat_new_handle(H_DIR);
        h->fd = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } else {
        int write_access = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
        int o = write_access ? O_RDWR : O_RDONLY;
        if (!existed) { o |= O_CREAT; info = 2; }
        else if (disp == FILE_OVERWRITE || disp == FILE_OVERWRITE_IF || disp == FILE_SUPERSEDE) { o |= O_TRUNC; info = disp == FILE_SUPERSEDE ? 0 : 3; }
        int fd = open(p, o | O_CLOEXEC, 0644);
        if (fd < 0 && write_access && errno == EACCES) fd = open(p, (o & ~O_RDWR) | O_RDONLY | O_CLOEXEC);   // read-only media
        if (fd < 0) return status_of_errno();
        h = compat_new_handle(H_FILE);
        h->fd = fd;
    }
    h->path = strcmp(p, "/dev/null") ? strdup(p) : 0;
    h->access = access;
    *out = h;
    if (iosb) { iosb->Status = 0; iosb->Information = info; }
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtCreateFile(HANDLE *out, ULONG access, NT_OA *oa, NT_IOSB *iosb, LARGE_INTEGER *alloc, ULONG attrs, ULONG share,
                            ULONG disp, ULONG opts, PVOID ea, ULONG ealen)
{
    (void)alloc; (void)attrs; (void)share; (void)ea; (void)ealen;
    struct create_buffers *b = malloc(sizeof *b);
    if (!b) return STATUS_NO_MEMORY;
    NTSTATUS st = create_file(out, access, oa, iosb, disp, opts, b);
    free(b);
    return st;
}

static void done(HANDLE event, NT_IOSB *iosb, NTSTATUS st, ULONG_PTR info)
{
    if (iosb) { iosb->Status = st; iosb->Information = info; }
    if (event) SetEvent(event);
}
NTSTATUS NTAPI NtReadFile(HANDLE fh, HANDLE event, PVOID apc, PVOID ctx, NT_IOSB *iosb, PVOID buf, ULONG len, LARGE_INTEGER *at, ULONG *key)
{
    (void)apc; (void)ctx; (void)key;
    struct compat_handle *h = fh;
    if (!h || h->kind != H_FILE) return STATUS_INVALID_HANDLE;
    // an offset of -2 (FILE_USE_FILE_POINTER_POSITION) or none reads at the file pointer; either way it moves past
    if (at && at->QuadPart != -2 && lseek64(h->fd, at->QuadPart, SEEK_SET) < 0) return STATUS_INVALID_PARAMETER;
    ssize_t r = compat_read_full(h->fd, buf, len);
    if (r < 0) { NTSTATUS st = status_of_errno(); done(event, iosb, st, 0); return st; }
    if (r == 0 && len) { done(event, iosb, STATUS_END_OF_FILE, 0); return STATUS_END_OF_FILE; }
    done(event, iosb, STATUS_SUCCESS, (ULONG_PTR)r);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtWriteFile(HANDLE fh, HANDLE event, PVOID apc, PVOID ctx, NT_IOSB *iosb, PVOID buf, ULONG len, LARGE_INTEGER *at, ULONG *key)
{
    (void)apc; (void)ctx; (void)key;
    struct compat_handle *h = fh;
    if (!h || h->kind != H_FILE) return STATUS_INVALID_HANDLE;
    if (at && at->QuadPart >= 0) lseek64(h->fd, at->QuadPart, SEEK_SET);
    else if (at && at->QuadPart == -1) lseek64(h->fd, 0, SEEK_END);   // FILE_WRITE_TO_END_OF_FILE
    ssize_t r = compat_write_full(h->fd, buf, len);
    if (r < 0) { NTSTATUS st = status_of_errno(); done(event, iosb, st, 0); return st; }
    done(event, iosb, STATUS_SUCCESS, (ULONG_PTR)r);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtFlushBuffersFile(HANDLE fh, NT_IOSB *iosb)
{
    struct compat_handle *h = fh;
    if (h && h->fd >= 0) fsync(h->fd);
    if (iosb) { iosb->Status = 0; iosb->Information = 0; }
    return STATUS_SUCCESS;
}

// The classes kernel.c passes through (nameless on both sides).
NTSTATUS NTAPI NtQueryInformationFile(HANDLE fh, NT_IOSB *iosb, PVOID out, ULONG len, ULONG cls)
{
    struct compat_handle *h = fh;
    struct stat st;
    if (!h || h->fd < 0 || fstat(h->fd, &st)) return STATUS_INVALID_HANDLE;
    uint8_t *o = out;
    ULONG n = 0;
    LARGE_INTEGER size, alloc;
    size.QuadPart = st.st_size;
    alloc.QuadPart = (st.st_size + 4095) & ~4095ll;
    switch (cls) {
    case 4:   // FileBasicInformation
        if (len < 40) return STATUS_BUFFER_TOO_SMALL;
        memset(o, 0, 40);
        ((LARGE_INTEGER *)o)[0] = nt_time(&st.st_ctim); ((LARGE_INTEGER *)o)[1] = nt_time(&st.st_atim);
        ((LARGE_INTEGER *)o)[2] = nt_time(&st.st_mtim); ((LARGE_INTEGER *)o)[3] = nt_time(&st.st_ctim);
        *(ULONG *)(o + 32) = nt_attrs(&st);
        n = 40; break;
    case 5:   // FileStandardInformation
        if (len < 24) return STATUS_BUFFER_TOO_SMALL;
        memset(o, 0, 24);
        ((LARGE_INTEGER *)o)[0] = alloc; ((LARGE_INTEGER *)o)[1] = size;
        *(ULONG *)(o + 16) = 1; o[21] = S_ISDIR(st.st_mode);
        n = 24; break;
    case 6:   // FileInternalInformation
        if (len < 8) return STATUS_BUFFER_TOO_SMALL;
        *(uint64_t *)o = st.st_ino; n = 8; break;
    case 7:   // FileEaInformation
        if (len < 4) return STATUS_BUFFER_TOO_SMALL;
        *(ULONG *)o = 0; n = 4; break;
    case 8:   // FileAccessInformation
        if (len < 4) return STATUS_BUFFER_TOO_SMALL;
        *(ULONG *)o = 0x1F01FF; n = 4; break;
    case 14:  // FilePositionInformation
        if (len < 8) return STATUS_BUFFER_TOO_SMALL;
        ((LARGE_INTEGER *)o)->QuadPart = lseek64(h->fd, 0, SEEK_CUR); n = 8; break;
    case 16:  // FileModeInformation
        if (len < 4) return STATUS_BUFFER_TOO_SMALL;
        *(ULONG *)o = 0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */; n = 4; break;
    case 34:  // FileNetworkOpenInformation
        if (len < 56) return STATUS_BUFFER_TOO_SMALL;
        memset(o, 0, 56);
        ((LARGE_INTEGER *)o)[0] = nt_time(&st.st_ctim); ((LARGE_INTEGER *)o)[1] = nt_time(&st.st_atim);
        ((LARGE_INTEGER *)o)[2] = nt_time(&st.st_mtim); ((LARGE_INTEGER *)o)[3] = nt_time(&st.st_ctim);
        ((LARGE_INTEGER *)o)[4] = alloc; ((LARGE_INTEGER *)o)[5] = size;
        *(ULONG *)(o + 48) = nt_attrs(&st);
        n = 56; break;
    default:
        return STATUS_NOT_IMPLEMENTED;
    }
    if (iosb) { iosb->Status = 0; iosb->Information = n; }
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtSetInformationFile(HANDLE fh, NT_IOSB *iosb, PVOID in, ULONG len, ULONG cls)
{
    (void)len;
    struct compat_handle *h = fh;
    if (!h || h->fd < 0) return STATUS_INVALID_HANDLE;
    switch (cls) {
    case 4: break;   // FileBasicInformation: times and attributes, not kept
    case 13:         // FileDispositionInformation: delete on close (now: the handle stays usable), with the right to
        if (!(h->access & (0x10000 /* DELETE */ | GENERIC_ALL))) return STATUS_ACCESS_DENIED;
        if (*(BOOLEAN *)in && h->path) { if (h->kind == H_DIR ? rmdir(h->path) : unlink(h->path)) return status_of_errno(); }
        break;
    case 14: lseek64(h->fd, ((LARGE_INTEGER *)in)->QuadPart, SEEK_SET); break;
    case 19: break;  // FileAllocationInformation
    case 20:         // FileEndOfFileInformation, with the right to write
        if (!(h->access & (FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_WRITE | GENERIC_ALL))) return STATUS_ACCESS_DENIED;
        if (ftruncate64(h->fd, ((LARGE_INTEGER *)in)->QuadPart)) return status_of_errno();
        break;
    default: return STATUS_NOT_IMPLEMENTED;
    }
    if (iosb) { iosb->Status = 0; iosb->Information = 0; }
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtQueryVolumeInformationFile(HANDLE fh, NT_IOSB *iosb, PVOID out, ULONG len, ULONG cls)
{
    struct compat_handle *h = fh;
    struct statvfs v;
    if (!h || h->fd < 0 || fstatvfs(h->fd, &v)) { memset(&v, 0, sizeof v); v.f_frsize = 4096; v.f_blocks = v.f_bavail = 1u << 20; }
    uint8_t *o = out;
    ULONG unit = 16384, per = unit / 512;   // the Xbox hard disk's 16 KB clusters
    uint64_t total = (uint64_t)v.f_blocks * v.f_frsize / unit, avail = (uint64_t)v.f_bavail * v.f_frsize / unit;
    if (cls == 3 && len >= 24) {          // FileFsSizeInformation
        ((LARGE_INTEGER *)o)[0].QuadPart = total; ((LARGE_INTEGER *)o)[1].QuadPart = avail;
        *(ULONG *)(o + 16) = per; *(ULONG *)(o + 20) = 512;
        if (iosb) { iosb->Status = 0; iosb->Information = 24; }
        return STATUS_SUCCESS;
    }
    if (cls == 7 && len >= 32) {          // FileFsFullSizeInformation
        ((LARGE_INTEGER *)o)[0].QuadPart = total; ((LARGE_INTEGER *)o)[1].QuadPart = avail; ((LARGE_INTEGER *)o)[2].QuadPart = avail;
        *(ULONG *)(o + 24) = per; *(ULONG *)(o + 28) = 512;
        if (iosb) { iosb->Status = 0; iosb->Information = 32; }
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}
// FileDirectoryInformation (class 1), one entry (kernel.c asks for one at a time).
NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE fh, HANDLE event, PVOID apc, PVOID ctx, NT_IOSB *iosb, PVOID out, ULONG len, ULONG cls,
                                    BOOLEAN single, NT_USTR *mask, BOOLEAN restart)
{
    (void)event; (void)apc; (void)ctx; (void)single;
    struct compat_handle *h = fh;
    if (!h || h->kind != H_DIR || cls != 1) return STATUS_INVALID_PARAMETER;
    if (!h->dir || restart) {
        if (h->dir) closedir(h->dir);
        h->dir = opendir(h->path);
        if (mask && mask->Length) {
            wchar_t m[260];
            size_t n = mask->Length / 2 < 259 ? mask->Length / 2 : 259;
            wmemcpy(m, mask->Buffer, n); m[n] = 0;
            WideCharToMultiByte(CP_UTF8, 0, m, -1, h->pattern, sizeof h->pattern, 0, 0);
        } else strcpy(h->pattern, "*");
    }
    struct dirent *e;
    while (h->dir && (e = readdir(h->dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !compat_match(h->pattern, e->d_name)) continue;
        char full[4096];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", h->path, e->d_name);
        if (stat(full, &st)) continue;
        wchar_t w[260];
        int chars = MultiByteToWideChar(CP_UTF8, 0, e->d_name, -1, w, 260) - 1;
        if (chars < 0) continue;
        if (len < 0x40 + (ULONG)chars * sizeof(wchar_t)) return STATUS_BUFFER_OVERFLOW;
        uint8_t *o = out;
        memset(o, 0, 0x40);
        ((LARGE_INTEGER *)(o + 8))[0] = nt_time(&st.st_ctim); ((LARGE_INTEGER *)(o + 8))[1] = nt_time(&st.st_atim);
        ((LARGE_INTEGER *)(o + 8))[2] = nt_time(&st.st_mtim); ((LARGE_INTEGER *)(o + 8))[3] = nt_time(&st.st_ctim);
        ((LARGE_INTEGER *)(o + 8))[4].QuadPart = st.st_size; ((LARGE_INTEGER *)(o + 8))[5].QuadPart = (st.st_size + 4095) & ~4095ll;
        *(ULONG *)(o + 0x38) = nt_attrs(&st);
        *(ULONG *)(o + 0x3C) = (ULONG)chars * 2;
        memcpy(o + 0x40, w, chars * sizeof(wchar_t));
        if (iosb) { iosb->Status = 0; iosb->Information = 0x40 + chars * 2; }
        return STATUS_SUCCESS;
    }
    return STATUS_NO_MORE_FILES;
}
NTSTATUS NTAPI NtClose(HANDLE h) { return CloseHandle(h) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE; }

// ---- events, mutants, waits ---------------------------------------------------------------------------------------
NTSTATUS NTAPI NtCreateEvent(HANDLE *out, ULONG access, NT_OA *oa, ULONG type, BOOLEAN initial)
{
    (void)access; (void)oa;
    *out = CreateEventW(0, type == NotificationEvent, initial, 0);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtSetEvent(HANDLE h, LONG *prev)
{
    struct compat_handle *e = h;
    if (!e || e->kind != H_EVENT) return STATUS_INVALID_HANDLE;
    if (prev) *prev = e->signaled;
    SetEvent(h);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtCreateMutant(HANDLE *out, ULONG access, NT_OA *oa, BOOLEAN owner)
{
    (void)access; (void)oa;
    *out = CreateMutexW(0, owner, 0);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtReleaseMutant(HANDLE h, LONG *prev)
{
    struct compat_handle *m = h;
    if (prev) *prev = m ? (LONG)m->count : 0;
    return compat_release_mutex(h) ? STATUS_SUCCESS : STATUS_MUTANT_NOT_OWNED;
}
// An NT timeout as ms for compat_wait: none = forever, negative = relative (100 ns), positive = absolute (since 1601).
static DWORD timeout_ms(const LARGE_INTEGER *t)
{
    if (!t) return INFINITE;
    int64_t v = t->QuadPart;
    if (v < 0) return (DWORD)((-v + 9999) / 10000);
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    int64_t now = (int64_t)((uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime);
    return v <= now ? 0 : (DWORD)((v - now + 9999) / 10000);
}
NTSTATUS NTAPI NtWaitForSingleObject(HANDLE h, BOOLEAN alertable, LARGE_INTEGER *timeout)
{
    (void)alertable;
    DWORD r = compat_wait(h, timeout_ms(timeout));
    return r == WAIT_OBJECT_0 ? STATUS_SUCCESS : r == WAIT_TIMEOUT ? STATUS_TIMEOUT : STATUS_INVALID_HANDLE;
}
// Polled: each object checked without blocking, then a short sleep. Few waits use it, with few objects.
NTSTATUS NTAPI NtWaitForMultipleObjects(ULONG count, HANDLE *hs, ULONG type, BOOLEAN alertable, LARGE_INTEGER *timeout)
{
    (void)alertable;
    DWORD ms = timeout_ms(timeout);
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    for (;;) {
        if (type == 1 /* WaitAny */) {
            for (ULONG i = 0; i < count; i++) if (compat_wait(hs[i], 0) == WAIT_OBJECT_0) return (NTSTATUS)i;
        } else {
            ULONG ready = 0;
            for (ULONG i = 0; i < count; i++) {
                struct compat_handle *h = hs[i];
                ready += h && (h->kind == H_MUTEX ? !h->owner || h->owner == GetCurrentThreadId() : h->signaled);
            }
            if (ready == count) { for (ULONG i = 0; i < count; i++) compat_wait(hs[i], 0); return STATUS_SUCCESS; }
        }
        QueryPerformanceCounter(&t1);
        if (ms != INFINITE && (DWORD)((t1.QuadPart - t0.QuadPart) / 1000000) >= ms) return STATUS_TIMEOUT;
        usleep(500);
    }
}
NTSTATUS NTAPI NtDelayExecution(BOOLEAN alertable, LARGE_INTEGER *interval)
{
    (void)alertable;
    int64_t v = interval ? interval->QuadPart : 0;
    if (v >= 0) { sched_yield(); return STATUS_SUCCESS; }   // absolute times aren't used for sleeping
    struct timespec t = {(time_t)(-v / 10000000), (long)(-v % 10000000) * 100};
    while (nanosleep(&t, &t) && errno == EINTR) {}
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtResumeThread(HANDLE h, ULONG *prev)
{
    DWORD r = ResumeThread(h);
    if (prev) *prev = r;
    return r == (DWORD)-1 ? STATUS_INVALID_HANDLE : STATUS_SUCCESS;
}

// ---- memory -------------------------------------------------------------------------------------------------------
NTSTATUS NTAPI NtAllocateVirtualMemory(HANDLE proc, PVOID *base, ULONG_PTR zero, SIZE_T *size, ULONG type, ULONG prot)
{
    (void)proc; (void)zero;
    if (type & MEM_RESET) return STATUS_SUCCESS;
    void *p = VirtualAlloc(*base, *size, type, prot);
    if (!p) return STATUS_NO_MEMORY;
    if (!*base) *size = (*size + 0xFFFF) & ~(SIZE_T)0xFFFF;
    *base = p;
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE proc, PVOID *base, SIZE_T *size, ULONG type)
{
    (void)proc;
    return VirtualFree(*base, size ? *size : 0, type) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}
ULONG NTAPI RtlNtStatusToDosError(LONG st)
{
    return st == STATUS_SUCCESS ? 0 : st == STATUS_OBJECT_NAME_NOT_FOUND ? ERROR_FILE_NOT_FOUND : st == STATUS_OBJECT_PATH_NOT_FOUND
         ? ERROR_PATH_NOT_FOUND : st == STATUS_ACCESS_DENIED ? ERROR_ACCESS_DENIED : st == STATUS_OBJECT_NAME_COLLISION
         ? ERROR_ALREADY_EXISTS : st == STATUS_INVALID_HANDLE ? ERROR_INVALID_HANDLE : st == STATUS_NO_MEMORY
         ? ERROR_NOT_ENOUGH_MEMORY : st == STATUS_NO_MORE_FILES ? ERROR_NO_MORE_FILES : 31;
}

// ---- SEH ------------------------------------------------------------------------------------------------------------
// The game's own try/except frames chain from fs:[0] (the fake TEB). Raising isn't dispatched (nothing has needed it
// yet): it's logged and the game ends. Unwinding (an __except handler going back to its frame) calls each frame's
// handler with the unwinding flag, as NT's RtlUnwind does, then makes the target frame the chain's head.
void xlog(const char *fmt, ...);
typedef struct REG { struct REG *next; LONG (__cdecl *handler)(EXCEPTION_RECORD *, void *, CONTEXT *, void *); } REG;
VOID NTAPI RtlRaiseException(PEXCEPTION_RECORD r)
{
    xlog("[compat] RtlRaiseException code %08x at %p: not dispatched on Linux, ending", r->ExceptionCode, r->ExceptionAddress);
    ExitProcess(1);
}
VOID NTAPI RtlUnwind(PVOID target, PVOID ip, PEXCEPTION_RECORD rec, PVOID ret)
{
    (void)ip; (void)ret;
    NT_TIB *tib = NtCurrentTeb();
    EXCEPTION_RECORD local = {0};
    if (!rec) { local.ExceptionCode = 0xC0000027 /* STATUS_UNWIND */; rec = &local; }
    rec->ExceptionFlags |= 2;   // EXCEPTION_UNWINDING
    CONTEXT c = {0};
    for (REG *f = tib->ExceptionList; f && f != (REG *)-1 && f != target; f = f->next) {
        f->handler(rec, f, &c, 0);
        tib->ExceptionList = f->next;
    }
    tib->ExceptionList = target;
}
