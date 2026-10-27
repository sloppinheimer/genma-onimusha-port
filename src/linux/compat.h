// Linux: the slice of Win32 and the NT native API the host uses, done on POSIX (compat_sys.c, compat_file.c,
// compat_nt.c). DXVK Native's windows_base.h already has the basic types (DWORD, HANDLE, LONG, GUID...); what's here
// is the rest. Paths come in as the host builds them (wide, backslashes, absolute Linux folders) and are turned into
// UTF-8 with slashes, case-insensitively matched to what's on disk (the game's names and the disc's differ in case).
// wchar_t is 4 bytes here, not 2: counts in UNICODE_STRING-style Length fields stay in 2-byte units as on Windows
// (Length / 2 characters), so the host code that divides by 2 reads them right.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <malloc.h>
#include <ctype.h>
#include <wctype.h>

// kernel.c has a valloc of its own; glibc's (<stdlib.h>, above) would clash.
#define valloc host_valloc
#include <stdarg.h>
#define _msize(p) malloc_usable_size(p)
#define timeBeginPeriod(n) 0
#define timeEndPeriod(n) 0
#define _vscprintf(fmt, ap) vsnprintf(0, 0, (fmt), (ap))
#define LoadLibraryA(name) ((HMODULE)1)   // only asked for SDL3, which is linked in

#define _ONI_LINUX 1

// ---- the compiler ------------------------------------------------------------------------------------------------
#define __declspec(x) __oni_declspec_##x
#define __oni_declspec_dllexport __attribute__((visibility("default")))
#define __oni_declspec_dllimport
#define __oni_declspec_noreturn __attribute__((noreturn))
#define __forceinline inline __attribute__((always_inline))
#define _AddressOfReturnAddress() ((void *)((char *)__builtin_frame_address(0) + 4))
#define CALLBACK
#define NTAPI __attribute__((stdcall))
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
#define _strdup strdup
#define _countof(a) (sizeof(a) / sizeof *(a))
#define UNREFERENCED_PARAMETER(x) (void)(x)

// ---- types DXVK's windows_base.h leaves out ------------------------------------------------------------------------
typedef unsigned char BOOLEAN;
typedef BOOLEAN *PBOOLEAN;
typedef int16_t CSHORT;
typedef uint32_t *PULONG, *PDWORD;
typedef int32_t NTSTATUS;
typedef uint32_t ACCESS_MASK;
typedef int64_t LONG64;
typedef uint64_t DWORD64;
typedef unsigned long ULONG_LONG_PAD;
typedef void *PHANDLE;
typedef const wchar_t *PCWSTR;
typedef char *PSTR;
typedef uint8_t *PBYTE, *LPBYTE;
typedef uint16_t *PUSHORT;
typedef void *HGLOBAL, *HRSRC, *HFONT, *HBITMAP, *HGDIOBJ, *HBRUSH, *HICON, *HCURSOR, *HMENU;
typedef intptr_t LPARAM;
typedef uintptr_t WPARAM;
typedef intptr_t LRESULT;
// LARGE_INTEGER is DXVK's (windows_base.h)
typedef union { struct { uint32_t LowPart, HighPart; }; struct { uint32_t LowPart, HighPart; } u; uint64_t QuadPart; } ULARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME, *LPFILETIME;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME, *LPSYSTEMTIME;

#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)

// ---- errors ------------------------------------------------------------------------------------------------------
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_BAD_EXE_FORMAT 193
#define ERROR_ALREADY_EXISTS 183
#define ERROR_FILE_EXISTS 80
#define ERROR_IO_PENDING 997
#define ERROR_DEVICE_NOT_CONNECTED 1167
#define ERROR_NO_MORE_FILES 18
DWORD GetLastError(void);
void SetLastError(DWORD e);

// ---- NT status codes --------------------------------------------------------------------------------------------
#define STATUS_SUCCESS ((NTSTATUS)0x00000000)
#define STATUS_PENDING ((NTSTATUS)0x00000103)
#define STATUS_ABANDONED ((NTSTATUS)0x00000080)
#define STATUS_USER_APC ((NTSTATUS)0x000000C0)
#define STATUS_ALERTED ((NTSTATUS)0x00000101)
#define STATUS_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#define STATUS_NO_MORE_FILES ((NTSTATUS)0x80000006)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002)
#define STATUS_INVALID_HANDLE ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_NO_SUCH_FILE ((NTSTATUS)0xC000000F)
#define STATUS_END_OF_FILE ((NTSTATUS)0xC0000011)
#define STATUS_NO_MEMORY ((NTSTATUS)0xC0000017)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)
#define STATUS_OBJECT_NAME_INVALID ((NTSTATUS)0xC0000033)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035)
#define STATUS_OBJECT_PATH_NOT_FOUND ((NTSTATUS)0xC000003A)
#define STATUS_SHARING_VIOLATION ((NTSTATUS)0xC0000043)
#define STATUS_MUTANT_NOT_OWNED ((NTSTATUS)0xC0000046)
#define STATUS_DISK_FULL ((NTSTATUS)0xC000007F)
#define STATUS_FILE_IS_A_DIRECTORY ((NTSTATUS)0xC00000BA)
#define STATUS_NOT_A_DIRECTORY ((NTSTATUS)0xC0000103)
#define STATUS_DIRECTORY_NOT_EMPTY ((NTSTATUS)0xC0000101)
#define STATUS_CANCELLED ((NTSTATUS)0xC0000120)
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)

// ---- memory -----------------------------------------------------------------------------------------------------
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE 0x8000
#define MEM_FREE 0x10000
#define MEM_RESET 0x80000
#define MEM_TOP_DOWN 0x100000
#define MEM_PRIVATE 0x20000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_GUARD 0x100
#define PAGE_NOCACHE 0x200
#define PAGE_WRITECOMBINE 0x400
typedef struct { PVOID BaseAddress, AllocationBase; DWORD AllocationProtect; SIZE_T RegionSize; DWORD State, Protect, Type; } MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len);
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, DWORD *old);
#define FlushInstructionCache(p, a, n) ((void)(p), (void)(a), (void)(n), 1)
BOOL IsBadReadPtr(const void *p, UINT_PTR n);
// The game's memory, for a search through it (movie.c's for Bink's state): up to max of the VirtualAlloc reservations
// as {base, size}, in no order; returns how many.
int compat_reservations(uintptr_t (*out)[2], int max);

// ---- processes, threads, synchronisation ---------------------------------------------------------------------------
typedef DWORD (*LPTHREAD_START_ROUTINE)(void *);
#define CREATE_SUSPENDED 0x4
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_MODE_BACKGROUND_BEGIN 0x10000
#define THREAD_MODE_BACKGROUND_END 0x20000
#define THREAD_SUSPEND_RESUME 0x2
#define THREAD_GET_CONTEXT 0x8
#define THREAD_SET_CONTEXT 0x10
HANDLE CreateThread(void *sec, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *tid);
DWORD ResumeThread(HANDLE h);
__attribute__((noreturn)) void ExitThread(DWORD code);
__attribute__((noreturn)) void ExitProcess(UINT code);
DWORD GetCurrentThreadId(void);
DWORD GetCurrentProcessId(void);
HANDLE GetCurrentThread(void);
HANDLE GetCurrentProcess(void);
BOOL SetThreadPriority(HANDLE h, int prio);
BOOL GetThreadTimes(HANDLE h, FILETIME *create, FILETIME *exit, FILETIME *kernel, FILETIME *user);
BOOL CloseHandle(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
HANDLE CreateEventW(void *sec, BOOL manual, BOOL initial, const wchar_t *name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
HANDLE CreateMutexW(void *sec, BOOL owner, const wchar_t *name);
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#define TIMER_ALL_ACCESS 0x1F0003
HANDLE CreateWaitableTimerExW(void *sec, const wchar_t *name, DWORD flags, DWORD access);
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, void *arg, BOOL resume);
void Sleep(DWORD ms);

typedef pthread_mutex_t SRWLOCK, *PSRWLOCK;   // shared is taken exclusively: simpler, and condition variables work
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define InitializeSRWLock(l) pthread_mutex_init((l), 0)
#define AcquireSRWLockExclusive(l) pthread_mutex_lock(l)
#define AcquireSRWLockShared(l) pthread_mutex_lock(l)
#define ReleaseSRWLockExclusive(l) pthread_mutex_unlock(l)
#define ReleaseSRWLockShared(l) pthread_mutex_unlock(l)
#define TryAcquireSRWLockExclusive(l) (pthread_mutex_trylock(l) == 0)
typedef pthread_cond_t CONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT PTHREAD_COND_INITIALIZER
#define WakeAllConditionVariable(c) pthread_cond_broadcast(c)
#define WakeConditionVariable(c) pthread_cond_signal(c)
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *c, SRWLOCK *l, DWORD ms, ULONG flags);
typedef pthread_mutex_t CRITICAL_SECTION;
void InitializeCriticalSection(CRITICAL_SECTION *c);
#define EnterCriticalSection(c) pthread_mutex_lock(c)
#define LeaveCriticalSection(c) pthread_mutex_unlock(c)
#define TryEnterCriticalSection(c) (pthread_mutex_trylock(c) == 0)
#define DeleteCriticalSection(c) pthread_mutex_destroy(c)
typedef struct { pthread_mutex_t m; int done; } INIT_ONCE, *PINIT_ONCE;
#define INIT_ONCE_STATIC_INIT {PTHREAD_MUTEX_INITIALIZER, 0}
typedef BOOL (*PINIT_ONCE_FN)(INIT_ONCE *, void *, void **);
BOOL InitOnceExecuteOnce(INIT_ONCE *once, PINIT_ONCE_FN fn, void *param, void **ctx);

#define InterlockedIncrement(p) __sync_add_and_fetch((p), 1)
#define InterlockedDecrement(p) __sync_sub_and_fetch((p), 1)
#define InterlockedExchangeAdd(p, v) __sync_fetch_and_add((p), (v))
#define InterlockedCompareExchange(p, x, c) __sync_val_compare_and_swap((p), (c), (x))
#define InterlockedCompareExchangePointer(p, x, c) __sync_val_compare_and_swap((p), (c), (x))
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangePointer(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedOr(p, v) __sync_fetch_and_or((p), (v))
#define InterlockedAnd(p, v) __sync_fetch_and_and((p), (v))
#define MemoryBarrier() __sync_synchronize()
#define YieldProcessor() __builtin_ia32_pause()
BOOL WaitOnAddress(volatile void *addr, void *cmp, SIZE_T size, DWORD ms);
void WakeByAddressSingle(void *addr);
void WakeByAddressAll(void *addr);

// The fake TEB each thread's fs: points at (compat_sys.c): Windows' layout where the host and the patched game read it,
// the self pointer at 0x18 and the TLS slots at 0xE10.
typedef struct _NT_TIB { void *ExceptionList; void *StackBase; void *StackLimit; void *SubSystemTib; void *FiberData;
                         void *ArbitraryUserPointer; struct _NT_TIB *Self; } NT_TIB;
void *NtCurrentTeb(void);
DWORD TlsAlloc(void);
LPVOID TlsGetValue(DWORD slot);
BOOL TlsSetValue(DWORD slot, LPVOID v);
#define TLS_OUT_OF_INDEXES 0xFFFFFFFFu
void compat_thread_attach(void);   // a thread not made by CreateThread (SDL's, DXVK's) that runs host code

// ---- exceptions (signals) -----------------------------------------------------------------------------------------
#define EXCEPTION_MAXIMUM_PARAMETERS 15
typedef struct _EXCEPTION_RECORD { DWORD ExceptionCode, ExceptionFlags; struct _EXCEPTION_RECORD *ExceptionRecord; PVOID ExceptionAddress;
                                   DWORD NumberParameters; ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS]; } EXCEPTION_RECORD, *PEXCEPTION_RECORD;
typedef struct { DWORD ContextFlags, Dr0, Dr1, Dr2, Dr3, Dr6, Dr7; DWORD SegGs, SegFs, SegEs, SegDs;
                 DWORD Edi, Esi, Ebx, Edx, Ecx, Eax, Ebp, Eip, SegCs, EFlags, Esp, SegSs; } CONTEXT, *PCONTEXT;
typedef struct { EXCEPTION_RECORD *ExceptionRecord; CONTEXT *ContextRecord; } EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS *);
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_BREAKPOINT 0x80000003u
#define EXCEPTION_SINGLE_STEP 0x80000004u
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001Du
#define EXCEPTION_INT_DIVIDE_BY_ZERO 0xC0000094u
#define CONTEXT_DEBUG_REGISTERS 0x10010
void *AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h);
#define OpenThread(a, i, t) ((HANDLE)0)
#define SuspendThread(h) ((DWORD)-1)
#define GetThreadContext(h, c) 0
#define SetThreadContext(h, c) 0

// ---- time ---------------------------------------------------------------------------------------------------------
BOOL QueryPerformanceCounter(LARGE_INTEGER *v);
BOOL QueryPerformanceFrequency(LARGE_INTEGER *v);
DWORD GetTickCount(void);
ULONGLONG GetTickCount64(void);
void GetLocalTime(SYSTEMTIME *t);
void GetSystemTime(SYSTEMTIME *t);
void GetSystemTimeAsFileTime(FILETIME *t);
BOOL FileTimeToSystemTime(const FILETIME *f, SYSTEMTIME *s);

// ---- text ---------------------------------------------------------------------------------------------------------
#define CP_ACP 0
#define CP_UTF8 65001
int MultiByteToWideChar(UINT cp, DWORD flags, const char *s, int n, wchar_t *out, int outn);
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *s, int n, char *out, int outn, const char *dflt, BOOL *used);
DWORD GetEnvironmentVariableA(const char *name, char *buf, DWORD n);
DWORD GetEnvironmentVariableW(const wchar_t *name, wchar_t *buf, DWORD n);

// ---- files --------------------------------------------------------------------------------------------------------
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_ALL 0x10000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_HIDDEN 0x2
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE 0x20
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_ATTRIBUTE_REPARSE_POINT 0x400
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 8
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow;
                 DWORD dwReserved0, dwReserved1; wchar_t cFileName[MAX_PATH]; wchar_t cAlternateFileName[14]; } WIN32_FIND_DATAW;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD share, void *sec, DWORD disp, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov);
BOOL WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, void *ov);
DWORD GetFileSize(HANDLE h, DWORD *high);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);
DWORD SetFilePointer(HANDLE h, LONG lo, LONG *hi, DWORD how);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, LARGE_INTEGER *now, DWORD how);
BOOL FlushFileBuffers(HANDLE h);
BOOL SetEndOfFile(HANDLE h);
HANDLE GetStdHandle(DWORD which);
DWORD GetFileAttributesW(const wchar_t *path);
BOOL GetFileAttributesExW(const wchar_t *path, GET_FILEEX_INFO_LEVELS lvl, void *out);
BOOL CreateDirectoryW(const wchar_t *path, void *sec);
BOOL RemoveDirectoryW(const wchar_t *path);
BOOL DeleteFileW(const wchar_t *path);
BOOL MoveFileExW(const wchar_t *from, const wchar_t *to, DWORD flags);
BOOL CopyFileW(const wchar_t *from, const wchar_t *to, BOOL fail_if_exists);
HANDLE FindFirstFileW(const wchar_t *pattern, WIN32_FIND_DATAW *d);
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *d);
BOOL FindClose(HANDLE h);
DWORD GetModuleFileNameW(HMODULE m, wchar_t *out, DWORD n);
DWORD GetModuleFileNameA(HMODULE m, char *out, DWORD n);
DWORD GetShortPathNameW(const wchar_t *in, wchar_t *out, DWORD n);
DWORD GetFullPathNameW(const wchar_t *in, DWORD n, wchar_t *out, wchar_t **file);
FILE *_wfopen(const wchar_t *path, const wchar_t *mode);
// a host path (wide, backslashes) as the Linux one that exists, matching each part's case to what's on disk; when
// the file doesn't exist yet, the parts that do are matched and the rest kept as given. out: UTF-8, n bytes.
int compat_path(const wchar_t *path, char *out, size_t n);

// ---- the rest -----------------------------------------------------------------------------------------------------
#define MB_OK 0
#define MB_ICONERROR 0x10
#define MB_ICONINFORMATION 0x40
#define MB_TOPMOST 0x40000
int MessageBoxW(HWND w, const wchar_t *text, const wchar_t *title, UINT type);
int MessageBoxA(HWND w, const char *text, const char *title, UINT type);
#define AllocConsole() 0
#define SetProcessDpiAwarenessContext(x) 1
#define OutputDebugStringA(s) ((void)(s))
HMODULE GetModuleHandleW(const wchar_t *name);
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
BOOL GetModuleHandleExW(DWORD flags, const wchar_t *name, HMODULE *out);
BOOL GetModuleHandleExA(DWORD flags, const char *name, HMODULE *out);
#define CoInitializeEx(a, b) 0
#define CoUninitialize() ((void)0)
#define COINIT_MULTITHREADED 0
#define COINIT_APARTMENTTHREADED 2

// ---- NT native API (compat_nt.c, compat_file.c) -------------------------------------------------------------------
#define SYNCHRONIZE 0x00100000
#define FILE_READ_DATA 0x1
#define FILE_WRITE_DATA 0x2
#define FILE_APPEND_DATA 0x4
#define FILE_READ_ATTRIBUTES 0x80
#define FILE_WRITE_ATTRIBUTES 0x100
#define DELETE 0x00010000
#define EVENT_ALL_ACCESS 0x1F0003
#define MUTANT_ALL_ACCESS 0x1F0001
typedef enum { NotificationEvent, SynchronizationEvent } EVENT_TYPE;
#define FILE_SUPERSEDE 0
#define FILE_OPEN 1
#define FILE_CREATE 2
#define FILE_OPEN_IF 3
#define FILE_OVERWRITE 4
#define FILE_OVERWRITE_IF 5
#define FILE_DIRECTORY_FILE 0x1
#define FILE_NON_DIRECTORY_FILE 0x40
DWORD GetLongPathNameW(const wchar_t *in, wchar_t *out, DWORD n);
#define OpenMutexW(access, inherit, name) compat_open_mutex(name)
HANDLE compat_open_mutex(const wchar_t *name);   // 0 unless a running game holds it

uint32_t *compat_image_load(const wchar_t *path, uint32_t *w, uint32_t *h);   // compat_image.c: PNG/JPEG -> BGRA
int compat_png_write(const wchar_t *path, const void *bgra, uint32_t w, uint32_t h, uint32_t pitch);

#include "compat_win.h"
static inline BOOL OffsetRect(RECT *r, int dx, int dy) { r->left += dx; r->right += dx; r->top += dy; r->bottom += dy; return 1; }
