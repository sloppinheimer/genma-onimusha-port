// The Linux launcher's <windows.h>: the Win32 calls the launcher's shared headers make (text.h, disc.h, files.h,
// randomizer.h, src/host/mods_list.h and layout.h), on POSIX, so the Linux launcher runs the very code the Windows one
// does. Only what those headers and launcher/linux/main.cpp use; the game's own shim is src/linux/ (32-bit, separate).
//
// Paths are kept as on Windows, wide with backslashes ("\home\deck\Genma\mods\..."): that is the form the game hashes
// for its running check (layout.h), and the shared code builds paths with L"\\". px() turns one into a real path,
// matching each part that exists to what's on disk ignoring case, as Windows and the game do (a disc's IMAGE\BMP\ETC
// is image/bmp/etc here).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <ctime>
#include <string>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef int BOOL;
typedef long LONG;
typedef unsigned long ULONG;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef unsigned char UCHAR, *PUCHAR;
typedef void *HANDLE;
typedef long NTSTATUS;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define MAX_PATH 1024
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_HIDDEN 0x2
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#define FILE_SHARE_READ 1
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define FILE_BEGIN 0
#define FILE_END 2
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 8
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_ALREADY_EXISTS 183
#define CP_UTF8 65001
#define SYNCHRONIZE 0x00100000
#define GetFileExInfoStandard 0

struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; };
struct WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    wchar_t cFileName[MAX_PATH];
};
struct WIN32_FILE_ATTRIBUTE_DATA { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; };
union LARGE_INTEGER { struct { DWORD LowPart; LONG HighPart; }; LONGLONG QuadPart; };
struct SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; };

static thread_local DWORD oni_error;
static inline DWORD GetLastError() { return oni_error; }
static inline void oni_set_error()
{
    oni_error = errno == ENOENT ? ERROR_FILE_NOT_FOUND : errno == ENOTDIR ? ERROR_PATH_NOT_FOUND : errno == EEXIST ? ERROR_ALREADY_EXISTS : ERROR_ACCESS_DENIED;
}

static inline int _stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
static inline int _wcsicmp(const wchar_t *a, const wchar_t *b) { return wcscasecmp(a, b); }
static inline int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n) { return wcsncasecmp(a, b, n); }
static inline unsigned long long _strtoui64(const char *s, char **e, int base) { return strtoull(s, e, base); }

// ---- text: UTF-8 <-> wchar_t (UTF-32 here; a UTF-16 surrogate pair, as text.h's field() decodes \u escapes, is joined)
static inline int MultiByteToWideChar(unsigned cp, DWORD, const char *s, int n, wchar_t *out, int outn)
{
    (void)cp;
    size_t len = n < 0 ? strlen(s) + 1 : (size_t)n;
    int w = 0;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        uint32_t u = 0xFFFD;
        int k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!k || i + k > len) i++;
        else {
            u = k == 1 ? c : c & (0x7F >> k);
            bool ok = true;
            for (int j = 1; j < k; j++) { unsigned char d = (unsigned char)s[i + j]; if ((d >> 6) != 2) { ok = false; break; } u = u << 6 | (d & 63); }
            if (ok) i += k; else { u = 0xFFFD; i++; }
        }
        if (outn) { if (w >= outn) return 0; out[w] = (wchar_t)u; }
        w++;
    }
    return w;
}
static inline int WideCharToMultiByte(unsigned cp, DWORD, const wchar_t *s, int n, char *out, int outn, const char *, BOOL *)
{
    (void)cp;
    size_t len = n < 0 ? wcslen(s) + 1 : (size_t)n;
    int o = 0;
    for (size_t i = 0; i < len; i++) {
        uint32_t u = (uint32_t)s[i];
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < len && (uint32_t)s[i + 1] >= 0xDC00 && (uint32_t)s[i + 1] < 0xE000) u = 0x10000 + ((u - 0xD800) << 10) + ((uint32_t)s[++i] - 0xDC00);
        else if ((u >= 0xD800 && u < 0xE000) || u > 0x10FFFF) u = 0xFFFD;
        char b[4];
        int k = u < 0x80 ? 1 : u < 0x800 ? 2 : u < 0x10000 ? 3 : 4;
        if (k == 1) b[0] = (char)u;
        else {
            for (int j = k - 1; j > 0; j--) { b[j] = (char)(0x80 | (u & 63)); u >>= 6; }
            b[0] = (char)((0xF00 >> k) | u);
        }
        if (outn) { if (o + k > outn) return 0; memcpy(out + o, b, k); }
        o += k;
    }
    return o;
}

// ---- paths -------------------------------------------------------------------------------------------------------
// A wide Windows-form path as a real one (UTF-8, slashes), each part that exists matched ignoring case; from the first
// part that doesn't, the rest as given (a file about to be made).
static inline std::string px(const wchar_t *w)
{
    char buf[MAX_PATH * 4];
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, 0, 0)) return {};
    for (char *c = buf; *c; c++) if (*c == '\\') *c = '/';
    struct stat st;
    if (buf[0] != '/' || !stat(buf, &st)) return buf;
    std::string res;
    bool found = true;
    for (const char *p = buf; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = strchr(p, '/');
        std::string part(p, e ? (size_t)(e - p) : strlen(p));
        p += part.size();
        if (found && part != "." && part != "..") {
            found = false;
            if (DIR *d = opendir(res.empty() ? "/" : res.c_str())) {
                for (struct dirent *de; (de = readdir(d));) if (!strcasecmp(de->d_name, part.c_str())) { part = de->d_name; found = true; break; }
                closedir(d);
            }
        }
        res += "/" + part;
    }
    return res.empty() ? "/" : res;
}
static inline std::string px(const std::wstring &w) { return px(w.c_str()); }

static inline FILETIME oni_filetime(const struct timespec &t)
{
    ULONGLONG v = (ULONGLONG)t.tv_sec * 10000000ull + (ULONGLONG)t.tv_nsec / 100 + 116444736000000000ull;
    return {(DWORD)v, (DWORD)(v >> 32)};
}
// Attributes as Windows gives them; a symbolic link is a reparse point (never entered when deleting, layout.h).
static inline DWORD oni_attributes(const char *p, struct stat *out = 0)
{
    struct stat st, lst;
    if (stat(p, &st)) { oni_set_error(); if (lstat(p, &lst)) return INVALID_FILE_ATTRIBUTES; st = lst; }   // a dangling link
    if (out) *out = st;
    DWORD a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!lstat(p, &lst) && S_ISLNK(lst.st_mode)) a |= FILE_ATTRIBUTE_REPARSE_POINT;
    return a;
}
static inline DWORD GetFileAttributesW(const wchar_t *p) { return oni_attributes(px(p).c_str()); }
static inline BOOL GetFileAttributesExW(const wchar_t *p, int, WIN32_FILE_ATTRIBUTE_DATA *d)
{
    struct stat st = {};
    DWORD a = oni_attributes(px(p).c_str(), &st);
    if (a == INVALID_FILE_ATTRIBUTES) return FALSE;
    d->dwFileAttributes = a;
    d->ftCreationTime = oni_filetime(st.st_ctim);   // no creation time in stat: the last change of the file's entry
    d->ftLastAccessTime = oni_filetime(st.st_atim);
    d->ftLastWriteTime = oni_filetime(st.st_mtim);
    uint64_t n = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    d->nFileSizeHigh = (DWORD)(n >> 32); d->nFileSizeLow = (DWORD)n;
    return TRUE;
}
static inline BOOL CreateDirectoryW(const wchar_t *p, void *) { if (!mkdir(px(p).c_str(), 0755)) return TRUE; oni_set_error(); return FALSE; }
static inline BOOL RemoveDirectoryW(const wchar_t *p)
{
    std::string s = px(p);
    struct stat st;
    if (!lstat(s.c_str(), &st) && S_ISLNK(st.st_mode)) return !unlink(s.c_str());   // a link to a folder: the link only
    return !rmdir(s.c_str());
}
static inline int SHCreateDirectoryExW(void *, const wchar_t *p, void *)   // every missing folder of the path
{
    std::string s = px(p);
    for (size_t i = 1; i <= s.size(); i++) if (i == s.size() || s[i] == '/') mkdir(s.substr(0, i).c_str(), 0755);
    struct stat st;
    return !stat(s.c_str(), &st) && S_ISDIR(st.st_mode) ? 0 : 1;
}
static inline BOOL DeleteFileW(const wchar_t *p) { if (!unlink(px(p).c_str())) return TRUE; oni_set_error(); return FALSE; }
static inline BOOL MoveFileExW(const wchar_t *from, const wchar_t *to, DWORD flags)
{
    std::string a = px(from), b = px(to);
    struct stat st;
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && !lstat(b.c_str(), &st)) { oni_error = ERROR_ALREADY_EXISTS; return FALSE; }
    if (rename(a.c_str(), b.c_str())) { oni_set_error(); return FALSE; }
    if (flags & MOVEFILE_WRITE_THROUGH) {   // the rename itself on disk: its folder synced
        std::string dir = b.substr(0, b.find_last_of('/') + 1);
        int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd >= 0) { fsync(fd); close(fd); }
    }
    return TRUE;
}
static inline FILE *_wfopen(const wchar_t *p, const wchar_t *mode)
{
    char m[16] = "";
    for (int i = 0; mode[i] && mode[i] != L',' && i < 15; i++) m[i] = (char)mode[i], m[i + 1] = 0;
    return fopen(px(p).c_str(), m);
}
// Windows-form path of this program (layout.h's layout_root, unused by the launcher but compiled).
static inline DWORD GetModuleFileNameW(void *, wchar_t *out, DWORD n)
{
    char p[MAX_PATH];
    ssize_t l = readlink("/proc/self/exe", p, sizeof p - 1);
    if (l <= 0) return 0;
    p[l] = 0;
    for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
    int w = MultiByteToWideChar(CP_UTF8, 0, p, -1, out, (int)n);
    return w ? (DWORD)w - 1 : 0;
}
static inline DWORD GetLongPathNameW(const wchar_t *in, wchar_t *out, DWORD n) { wcsncpy(out, in, n); return (DWORD)wcslen(out); }   // as the game's (src/linux)

// ---- file handles: a descriptor + ONI_FD_BASE (0 and the small numbers stay free, as on Windows) -------------------
#define ONI_FD_BASE 0x10000
static inline int oni_fd(HANDLE h) { intptr_t v = (intptr_t)h; return v >= ONI_FD_BASE && v < ONI_FD_BASE + (1 << 24) ? (int)(v - ONI_FD_BASE) : -1; }
static inline HANDLE CreateFileW(const wchar_t *p, DWORD access, DWORD, void *, DWORD disp, DWORD, HANDLE)
{
    int fl = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (disp == CREATE_ALWAYS) fl |= O_CREAT | O_TRUNC;
    else if (disp == OPEN_ALWAYS) fl |= O_CREAT;
    int fd = open(px(p).c_str(), fl | O_CLOEXEC, 0644);
    if (fd < 0) { oni_set_error(); return INVALID_HANDLE_VALUE; }
    return (HANDLE)(intptr_t)(fd + ONI_FD_BASE);
}
static inline BOOL CloseHandle(HANDLE h) { int fd = oni_fd(h); return fd >= 0 && !close(fd); }   // anything else (OpenMutexW's) has nothing to close
static inline BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(oni_fd(h), (char *)buf + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { *got = (DWORD)done; return FALSE; }
        if (!r) break;
        done += (size_t)r;
    }
    *got = (DWORD)done;
    return TRUE;
}
static inline BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, void *)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(oni_fd(h), (const char *)buf + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { *put = (DWORD)done; return FALSE; }
        done += (size_t)r;
    }
    *put = (DWORD)done;
    return TRUE;
}
static inline BOOL FlushFileBuffers(HANDLE h) { return !fsync(oni_fd(h)); }
static inline BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *n) { struct stat st; if (fstat(oni_fd(h), &st)) return FALSE; n->QuadPart = st.st_size; return TRUE; }
static inline BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, LARGE_INTEGER *now, DWORD how)
{
    off_t r = lseek(oni_fd(h), (off_t)to.QuadPart, how == FILE_END ? SEEK_END : how == FILE_BEGIN ? SEEK_SET : SEEK_CUR);
    if (now) now->QuadPart = r;
    return r >= 0;
}
static inline BOOL CopyFileW(const wchar_t *from, const wchar_t *to, BOOL fail_if_there)
{
    FILE *a = fopen(px(from).c_str(), "rb");
    if (!a) return FALSE;
    int fd = open(px(to).c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_there ? O_EXCL : O_TRUNC), 0644);
    FILE *b = fd >= 0 ? fdopen(fd, "wb") : 0;
    bool ok = b;
    char buf[65536];
    for (size_t n; ok && (n = fread(buf, 1, sizeof buf, a));) ok = fwrite(buf, 1, n, b) == n;
    fclose(a);
    if (b) ok = !fclose(b) && ok;
    return ok;
}

// ---- FindFirstFileW: a folder's entries matching the pattern's last part (* and ?, ignoring case, as Windows) --------
static inline bool oni_match(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') { while (p[1] == '*') p++; if (!p[1]) return true; for (; *s; s++) if (oni_match(p + 1, s)) return true; return false; }
        if (!*s || (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s))) return false;
    }
    return !*s;
}
struct oni_find { DIR *d; std::string dir, pattern; };
static inline BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd)
{
    auto *f = (oni_find *)h;
    for (struct dirent *e; (e = readdir(f->d));) {
        if (!oni_match(f->pattern.c_str(), e->d_name)) continue;
        std::string p = f->dir + "/" + e->d_name;
        struct stat st = {};
        DWORD a = oni_attributes(p.c_str(), &st);
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        memset(fd, 0, sizeof *fd);
        fd->dwFileAttributes = a;
        fd->ftCreationTime = oni_filetime(st.st_ctim);
        fd->ftLastAccessTime = oni_filetime(st.st_atim);
        fd->ftLastWriteTime = oni_filetime(st.st_mtim);
        uint64_t n = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
        fd->nFileSizeHigh = (DWORD)(n >> 32); fd->nFileSizeLow = (DWORD)n;
        if (!MultiByteToWideChar(CP_UTF8, 0, e->d_name, -1, fd->cFileName, MAX_PATH)) continue;
        return TRUE;
    }
    return FALSE;
}
static inline HANDLE FindFirstFileW(const wchar_t *pattern, WIN32_FIND_DATAW *fd)
{
    std::wstring w = pattern;
    size_t s = w.find_last_of(L'\\');
    std::string dir = px(s == std::wstring::npos ? std::wstring(L".") : w.substr(0, s));
    char b[MAX_PATH];
    if (!WideCharToMultiByte(CP_UTF8, 0, w.c_str() + (s == std::wstring::npos ? 0 : s + 1), -1, b, sizeof b, 0, 0)) return INVALID_HANDLE_VALUE;
    DIR *d = opendir(dir.c_str());
    if (!d) { oni_set_error(); return INVALID_HANDLE_VALUE; }
    auto *f = new oni_find{d, dir, b};
    if (FindNextFileW(f, fd)) return f;
    closedir(d);
    delete f;
    oni_error = ERROR_FILE_NOT_FOUND;
    return INVALID_HANDLE_VALUE;
}
static inline BOOL FindClose(HANDLE h) { auto *f = (oni_find *)h; if (!f || h == INVALID_HANDLE_VALUE) return FALSE; closedir(f->d); delete f; return TRUE; }

// ---- the game's running check (layout.h): the Linux game holds /tmp/<mutex name> locked (flock) while it runs. It is
// looked for in /proc/locks, never locked here: a lock taken to test it, at the moment the game starts, would make the
// game think another copy is running.
static inline HANDLE OpenMutexW(DWORD, BOOL, const wchar_t *name)
{
    char u[256];
    if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, u, sizeof u, 0, 0)) return 0;
    for (char *c = u; *c; c++) if (*c == '/' || *c == '\\') *c = '_';
    struct stat st;
    if (stat((std::string("/tmp/") + u).c_str(), &st)) return 0;
    FILE *f = fopen("/proc/locks", "r");
    if (!f) return 0;
    char line[256];
    bool held = false;
    while (!held && fgets(line, sizeof line, f)) {   // "1: FLOCK  ADVISORY  WRITE 4242 00:1f:123456 0 EOF"
        char kind[16], adv[16], mode[16], dev[64];
        unsigned maj = 0, min = 0;
        unsigned long long ino = 0;
        int pid;
        const char *rest = strchr(line, ':');
        if (rest && sscanf(rest + 1, "%15s %15s %15s %d %63s", kind, adv, mode, &pid, dev) == 5 && !strcmp(kind, "FLOCK")) {
            held = sscanf(dev, "%x:%x:%llu", &maj, &min, &ino) == 3 && ino == (unsigned long long)st.st_ino && maj == major(st.st_dev) && min == minor(st.st_dev);
        }
    }
    fclose(f);
    return held ? (HANDLE)(intptr_t)1 : 0;
}

// ---- time ----------------------------------------------------------------------------------------------------------
static inline ULONGLONG GetTickCount64() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (ULONGLONG)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
static inline DWORD GetTickCount() { return (DWORD)GetTickCount64(); }
static inline DWORD GetCurrentThreadId() { return (DWORD)syscall(SYS_gettid); }
static inline void Sleep(DWORD ms) { usleep((useconds_t)ms * 1000); }
static inline void GetLocalTime(SYSTEMTIME *s)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    struct tm m;
    localtime_r(&t.tv_sec, &m);
    *s = {(WORD)(m.tm_year + 1900), (WORD)(m.tm_mon + 1), (WORD)m.tm_wday, (WORD)m.tm_mday, (WORD)m.tm_hour, (WORD)m.tm_min, (WORD)m.tm_sec, (WORD)(t.tv_nsec / 1000000)};
}
static inline void GetSystemTimeAsFileTime(FILETIME *f) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); *f = oni_filetime(t); }

// ---- hashes (text.h's save signing; the launcher's file checks): main.cpp supplies them (Qt's) ---------------------
bool oni_hash(int kind, const void *key, size_t key_n, const void *data, size_t n, uint8_t *out);   // 1 SHA-1, 2 HMAC-SHA1
#define BCRYPT_SHA1_ALG_HANDLE ((void *)1)
#define BCRYPT_HMAC_SHA1_ALG_HANDLE ((void *)2)
#define BCRYPT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
static inline NTSTATUS BCryptHash(void *alg, PUCHAR key, ULONG key_n, PUCHAR in, ULONG n, PUCHAR out, ULONG out_n)
{
    if (out_n != 20) return -1;
    return oni_hash(alg == BCRYPT_HMAC_SHA1_ALG_HANDLE ? 2 : 1, key, key_n, in, n, out) ? 0 : -1;
}
