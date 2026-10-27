// Linux: files, folders and paths (compat.h). Host paths arrive wide with backslashes; they're matched to what's on
// disk part by part, ignoring case (the game asks for IMAGE\BGS\ROOM000.BGS, the disc has image/bgs/room000.bgs).
#define _GNU_SOURCE
#include <windows.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "compat_handle.h"

int compat_match(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return 1;
            for (; *s; s++) if (compat_match(p + 1, s)) return 1;
            return 0;
        }
        if (!*s) return 0;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
    }
    return !*s;
}

// The part of dir named like name (ignoring case) into out; 0 if none.
static int find_entry(const char *dir, const char *name, char *out, size_t n)
{
    DIR *d = opendir(*dir ? dir : "/");
    struct dirent *e;
    int hit = 0;
    while (d && (e = readdir(d)))
        if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s", e->d_name); hit = 1; break; }
    if (d) closedir(d);
    return hit;
}

// Game code calls in on its own stacks (64 KB, much of it already in use), so the buffers are on the heap.
struct path_buffers { char in[4096], res[4096], part[1024], real[1024]; };
static int resolve(const wchar_t *wpath, char *out, size_t n, struct path_buffers *b)
{
    char *in = b->in, *res = b->res, *part = b->part, *real = b->real;
    if (!WideCharToMultiByte(CP_UTF8, 0, wpath, -1, in, sizeof b->in, 0, 0)) return 0;
    for (char *c = in; *c; c++) if (*c == '\\') *c = '/';
    struct stat st;
    if (in[0] != '/' || !stat(in, &st)) { snprintf(out, n, "%s", in); return 1; }   // relative, or exists as it is
    // part by part: each that exists is matched for case; from the first that doesn't, the rest as given
    res[0] = 0;
    const char *p = in;
    int found = 1;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = strchr(p, '/');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        snprintf(part, sizeof b->part, "%.*s", (int)l, p);
        if (found && strcmp(part, ".") && strcmp(part, "..") && find_entry(res, part, real, sizeof b->real)) strcpy(part, real);
        else if (strcmp(part, ".") && strcmp(part, "..")) found = 0;
        size_t r = strlen(res);
        snprintf(res + r, sizeof b->res - r, "/%s", part);
        p += l;
    }
    snprintf(out, n, "%s", res[0] ? res : "/");
    return 1;
}
int compat_path(const wchar_t *wpath, char *out, size_t n)
{
    struct path_buffers *b = malloc(sizeof *b);
    int ok = b && resolve(wpath, out, n, b);
    free(b);
    return ok;
}

ssize_t compat_read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; return got ? (ssize_t)got : -1; }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}
ssize_t compat_write_full(int fd, const void *buf, size_t n)
{
    size_t put = 0;
    while (put < n) {
        ssize_t r = write(fd, (const char *)buf + put, n - put);
        if (r < 0) { if (errno == EINTR) continue; return put ? (ssize_t)put : -1; }
        put += (size_t)r;
    }
    return (ssize_t)put;
}

// ---- Win32 --------------------------------------------------------------------------------------------------------
HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD share, void *sec, DWORD disp, DWORD flags, HANDLE tmpl)
{
    (void)share; (void)sec; (void)flags; (void)tmpl;
    char p[4096];
    if (!compat_path(path, p, sizeof p)) { SetLastError(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    int o = (access & GENERIC_WRITE) ? (access & GENERIC_READ ? O_RDWR : O_WRONLY) : O_RDONLY;
    o |= disp == CREATE_NEW ? O_CREAT | O_EXCL : disp == CREATE_ALWAYS ? O_CREAT | O_TRUNC : disp == OPEN_ALWAYS ? O_CREAT
       : disp == TRUNCATE_EXISTING ? O_TRUNC : 0;
    struct stat st;
    int existed = !stat(p, &st);
    int fd = open(p, o | O_CLOEXEC, 0644);
    if (fd < 0) { compat_set_errno_error(); return INVALID_HANDLE_VALUE; }
    struct compat_handle *h = compat_new_handle(H_FILE);
    h->fd = fd;
    h->path = strdup(p);
    SetLastError(existed && (disp == CREATE_ALWAYS || disp == OPEN_ALWAYS) ? ERROR_ALREADY_EXISTS : 0);
    return h;
}
static int fd_of(HANDLE p)
{
    if (p == (HANDLE)(intptr_t)STD_ERROR_HANDLE) return 2;
    if (p == (HANDLE)(intptr_t)STD_OUTPUT_HANDLE) return 1;
    struct compat_handle *h = p;
    return h && h != INVALID_HANDLE_VALUE && (h->kind == H_FILE || h->kind == H_DIR) ? h->fd : -1;
}
HANDLE GetStdHandle(DWORD which) { return (HANDLE)(intptr_t)which; }
BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov)
{
    (void)ov;
    ssize_t r = compat_read_full(fd_of(h), buf, n);
    if (got) *got = r > 0 ? (DWORD)r : 0;
    if (r < 0) { compat_set_errno_error(); return 0; }
    return 1;
}
BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, void *ov)
{
    (void)ov;
    ssize_t r = compat_write_full(fd_of(h), buf, n);
    if (put) *put = r > 0 ? (DWORD)r : 0;
    if (r < 0) { compat_set_errno_error(); return 0; }
    return 1;
}
DWORD GetFileSize(HANDLE h, DWORD *high)
{
    struct stat st;
    if (fstat(fd_of(h), &st)) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)((uint64_t)st.st_size >> 32);
    return (DWORD)st.st_size;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size)
{
    struct stat st;
    if (fstat(fd_of(h), &st)) return 0;
    size->QuadPart = st.st_size;
    return 1;
}
DWORD SetFilePointer(HANDLE h, LONG lo, LONG *hi, DWORD how)
{
    off64_t to = hi ? ((off64_t)*hi << 32 | (uint32_t)lo) : lo;
    off64_t r = lseek64(fd_of(h), to, how == FILE_BEGIN ? SEEK_SET : how == FILE_CURRENT ? SEEK_CUR : SEEK_END);
    if (r < 0) return (DWORD)-1;
    if (hi) *hi = (LONG)(r >> 32);
    return (DWORD)r;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, LARGE_INTEGER *now, DWORD how)
{
    off64_t r = lseek64(fd_of(h), to.QuadPart, how == FILE_BEGIN ? SEEK_SET : how == FILE_CURRENT ? SEEK_CUR : SEEK_END);
    if (r < 0) return 0;
    if (now) now->QuadPart = r;
    return 1;
}
BOOL FlushFileBuffers(HANDLE h) { return fsync(fd_of(h)) == 0; }
BOOL SetEndOfFile(HANDLE h) { int fd = fd_of(h); return ftruncate64(fd, lseek64(fd, 0, SEEK_CUR)) == 0; }

static DWORD attrs_of(const struct stat *st)
{
    return (S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL) | (st->st_mode & S_IWUSR ? 0 : FILE_ATTRIBUTE_READONLY);
}
static FILETIME ft_of(time_t t)
{
    uint64_t v = (uint64_t)t * 10000000 + 116444736000000000ull;
    return (FILETIME){(DWORD)v, (DWORD)(v >> 32)};
}
DWORD GetFileAttributesW(const wchar_t *path)
{
    char p[4096];
    struct stat st;
    if (!compat_path(path, p, sizeof p) || stat(p, &st)) { compat_set_errno_error(); return INVALID_FILE_ATTRIBUTES; }
    return attrs_of(&st);
}
BOOL GetFileAttributesExW(const wchar_t *path, GET_FILEEX_INFO_LEVELS lvl, void *out)
{
    (void)lvl;
    char p[4096];
    struct stat st;
    if (!compat_path(path, p, sizeof p) || stat(p, &st)) { compat_set_errno_error(); return 0; }
    WIN32_FILE_ATTRIBUTE_DATA *d = out;
    d->dwFileAttributes = attrs_of(&st);
    d->ftCreationTime = d->ftLastAccessTime = d->ftLastWriteTime = ft_of(st.st_mtime);
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    d->nFileSizeLow = (DWORD)st.st_size;
    return 1;
}
BOOL CreateDirectoryW(const wchar_t *path, void *sec)
{
    (void)sec;
    char p[4096];
    if (!compat_path(path, p, sizeof p)) return 0;
    if (mkdir(p, 0755)) { compat_set_errno_error(); return 0; }
    return 1;
}
BOOL RemoveDirectoryW(const wchar_t *path) { char p[4096]; return compat_path(path, p, sizeof p) && rmdir(p) == 0; }
BOOL DeleteFileW(const wchar_t *path)
{
    char p[4096];
    if (!compat_path(path, p, sizeof p) || unlink(p)) { compat_set_errno_error(); return 0; }
    return 1;
}
BOOL MoveFileExW(const wchar_t *from, const wchar_t *to, DWORD flags)
{
    char a[4096], b[4096];
    struct stat st;
    if (!compat_path(from, a, sizeof a) || !compat_path(to, b, sizeof b)) return 0;
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && !stat(b, &st)) { SetLastError(ERROR_ALREADY_EXISTS); return 0; }
    if (rename(a, b)) { compat_set_errno_error(); return 0; }
    return 1;
}
BOOL CopyFileW(const wchar_t *from, const wchar_t *to, BOOL fail_if_exists)
{
    enum { CHUNK = 65536 };
    char a[4096], b[4096], *buf = malloc(CHUNK);   // not on the stack: the game's own are 64 KB
    if (!buf || !compat_path(from, a, sizeof a) || !compat_path(to, b, sizeof b)) { free(buf); return 0; }
    int in = open(a, O_RDONLY | O_CLOEXEC), out = in < 0 ? -1 : open(b, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), 0644);
    ssize_t r = 0;
    while (out >= 0 && (r = read(in, buf, CHUNK)) > 0) if (write(out, buf, r) != r) { r = -1; break; }
    free(buf);
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    return in >= 0 && out >= 0 && r == 0;
}

static int find_next(struct compat_handle *h, WIN32_FIND_DATAW *d)
{
    struct dirent *e;
    while (h->dir && (e = readdir(h->dir))) {
        if (!compat_match(h->pattern, e->d_name)) continue;
        char full[4096];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", h->path, e->d_name);
        if (stat(full, &st)) continue;
        memset(d, 0, sizeof *d);
        d->dwFileAttributes = attrs_of(&st);
        d->ftCreationTime = d->ftLastAccessTime = d->ftLastWriteTime = ft_of(st.st_mtime);
        d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
        d->nFileSizeLow = (DWORD)st.st_size;
        MultiByteToWideChar(CP_UTF8, 0, e->d_name, -1, d->cFileName, MAX_PATH);
        return 1;
    }
    return 0;
}
HANDLE FindFirstFileW(const wchar_t *pattern, WIN32_FIND_DATAW *d)
{
    char p[4096];
    if (!compat_path(pattern, p, sizeof p)) return INVALID_HANDLE_VALUE;
    char *slash = strrchr(p, '/');
    struct compat_handle *h = compat_new_handle(H_FIND);
    snprintf(h->pattern, sizeof h->pattern, "%s", slash ? slash + 1 : p);
    if (slash) *slash = 0;
    h->path = strdup(slash ? (*p ? p : "/") : ".");
    h->dir = opendir(h->path);
    if (!find_next(h, d)) {
        if (h->dir) closedir(h->dir);
        free(h->path); free(h);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}
BOOL FindNextFileW(HANDLE p, WIN32_FIND_DATAW *d)
{
    if (find_next(p, d)) return 1;
    SetLastError(ERROR_NO_MORE_FILES);
    return 0;
}
BOOL FindClose(HANDLE p)
{
    struct compat_handle *h = p;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    if (h->dir) closedir(h->dir);
    free(h->path); free(h);
    return 1;
}

// Wide with backslashes, as the host splits it (layout.h: the exe's folder, its parent when that's bin).
DWORD GetModuleFileNameW(HMODULE m, wchar_t *out, DWORD n)
{
    (void)m;
    char p[4096];
    ssize_t l = readlink("/proc/self/exe", p, sizeof p - 1);
    if (l <= 0) return 0;
    p[l] = 0;
    for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
    int w = MultiByteToWideChar(CP_UTF8, 0, p, -1, out, n);
    return w ? (DWORD)w - 1 : 0;
}
DWORD GetModuleFileNameA(HMODULE m, char *out, DWORD n)
{
    (void)m;
    ssize_t l = readlink("/proc/self/exe", out, n - 1);
    if (l <= 0) return 0;
    out[l] = 0;
    return (DWORD)l;
}
DWORD GetShortPathNameW(const wchar_t *in, wchar_t *out, DWORD n) { wcsncpy(out, in, n); return (DWORD)wcslen(out); }
DWORD GetLongPathNameW(const wchar_t *in, wchar_t *out, DWORD n) { wcsncpy(out, in, n); return (DWORD)wcslen(out); }
DWORD GetFullPathNameW(const wchar_t *in, DWORD n, wchar_t *out, wchar_t **file)
{
    wcsncpy(out, in, n);
    if (file) { wchar_t *s = wcsrchr(out, L'\\'); *file = s ? s + 1 : out; }
    return (DWORD)wcslen(out);
}
FILE *_wfopen(const wchar_t *path, const wchar_t *mode)
{
    char p[4096], m[16];
    if (!compat_path(path, p, sizeof p)) return 0;
    WideCharToMultiByte(CP_UTF8, 0, mode, -1, m, sizeof m, 0, 0);
    char *c = strchr(m, ',');   // ", ccs=UTF-8" and the like
    if (c) *c = 0;
    return fopen(p, m);
}
