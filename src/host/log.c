#include "host.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>
#include <locale.h>
#include <string.h>
#include "redact.h"

static HANDLE log_file = INVALID_HANDLE_VALUE;
// The profile folder as the log's text spells it (UTF-8), long and 8.3: written as %USERPROFILE% (redact.h)
static char profile[2][MAX_PATH * 3];
static const char *profile_forms[2];
static int nprofile;
static SRWLOCK log_lock = SRWLOCK_INIT;
static LARGE_INTEGER t0, freq;

void log_init(const wchar_t *path)
{
    // %ls through vsnprintf in the "C" locale fails (-1) on any character past U+00FF, and the whole line was lost:
    // the open and mod lines of an install under a Cyrillic or Japanese user folder. UTF-8 for text only (LC_CTYPE),
    // so number parsing keeps its '.'.
    setlocale(LC_CTYPE, ".UTF8");
    log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, 0, 0);
    wchar_t home[MAX_PATH], shrt[MAX_PATH];
    DWORD got = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (got && got < MAX_PATH && WideCharToMultiByte(CP_UTF8, 0, home, -1, profile[0], sizeof profile[0], 0, 0))
        profile_forms[nprofile++] = profile[0];
    if (nprofile && GetShortPathNameW(home, shrt, MAX_PATH) && _wcsicmp(shrt, home)
        && WideCharToMultiByte(CP_UTF8, 0, shrt, -1, profile[1], sizeof profile[1], 0, 0))
        profile_forms[nprofile++] = profile[1];
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
}

long long qpc(void) { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }
// The frequency is the one log_init read (before any other thread runs), so the pack and movie threads read it too
double qpc_ms(long long ticks) { return (double)ticks * 1000 / freq.QuadPart; }

static void vlog(const char *fmt, va_list ap)
{
    char buf[2048];
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int n = snprintf(buf, sizeof buf, "%9.3f %5lu ", (double)(now.QuadPart - t0.QuadPart) / freq.QuadPart,
                     GetCurrentThreadId());
    int m = vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    if (m > 0) n += m;
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;   // cut short: the newline goes over vsnprintf's NUL
    buf[n++] = '\n';
    char out[sizeof buf * 2];
    DWORD w, len = (DWORD)redact_profile(buf, n, out, sizeof out, profile_forms, nprofile);
    AcquireSRWLockExclusive(&log_lock);
    WriteFile(log_file, out, len, &w, 0);   // unbuffered: the last line survives a crash
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), out, len, &w, 0);
    ReleaseSRWLockExclusive(&log_lock);
}

int env_int(const char *name, int dflt)
{
    char v[32];
    DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    return n && n < sizeof v ? atoi(v) : dflt;
}

uint32_t env_str(const char *name, char *buf, uint32_t n)
{
    if (!buf || !n) return 0;
    DWORD got = GetEnvironmentVariableA(name, buf, n);
    if (!got || got >= n) { buf[0] = 0; return 0; }
    return got;
}

uint32_t debug_env(const char *name, char *buf, uint32_t n)
{
#ifdef ONI_DEBUG
    return env_str(name, buf, n);
#else
    (void)name;
    if (buf && n) buf[0] = 0;
    return 0;
#endif
}

int debug_env_int(const char *name, int dflt)
{
#ifdef ONI_DEBUG
    return env_int(name, dflt);
#else
    (void)name;
    return dflt;
#endif
}

void xlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void xfatal(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    xlog("%s", msg);
    wchar_t w[1024];   // the text is UTF-8 (log_init's locale): MessageBoxA would show a non-Latin path as mojibake
    if (!host_background() && MultiByteToWideChar(CP_UTF8, 0, msg, -1, w, 1024)) MessageBoxW(0, w, L"Genma Onimusha", MB_ICONERROR | MB_TOPMOST);
    ExitProcess(1);
}

void xbacktrace(const char *why)
{
    char buf[512];
    int n = snprintf(buf, sizeof buf, "backtrace (%s):", why);
    uint32_t *fp = (uint32_t *)_AddressOfReturnAddress() - 1;   // our frame's saved EBP
    for (int i = 0; i < 16 && !IsBadReadPtr(fp, 8) && n < (int)sizeof buf - 12; i++) {
        n += snprintf(buf + n, sizeof buf - n, " %08x", fp[1]);
        if ((uint32_t *)fp[0] <= fp) break;
        fp = (uint32_t *)fp[0];
    }
    xlog("%s", buf);
}
