// Linux: threads, the fake TEB behind fs:, memory, synchronisation, time, text and the crash handler (compat.h).
#define _GNU_SOURCE
#include <windows.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/file.h>
#include <sys/uio.h>
#include <linux/futex.h>
#include <asm/ldt.h>
#include <SDL3/SDL.h>
#include "compat_handle.h"

static __thread DWORD last_error;
DWORD GetLastError(void) { return last_error; }
void SetLastError(DWORD e) { last_error = e; }
void compat_set_errno_error(void)
{
    last_error = errno == ENOENT ? ERROR_FILE_NOT_FOUND : errno == ENOTDIR ? ERROR_PATH_NOT_FOUND : errno == EEXIST
               ? ERROR_ALREADY_EXISTS : errno == EACCES || errno == EPERM ? ERROR_ACCESS_DENIED : errno == ENOMEM
               ? ERROR_NOT_ENOUGH_MEMORY : 31 /* ERROR_GEN_FAILURE */;
}

// ---- the fake TEB ---------------------------------------------------------------------------------------------------
// On Windows fs: is the thread's TEB: the host reads its TLS slots there (TlsGetValue), and the patched game reads
// them straight through fs:[0xE10 + 4 * slot] (host.c patch_fs_sites); fs:[0] is the game's SEH chain. Linux leaves fs
// unused on x86 (its TLS is in gs), so each thread gets a page laid out the same way and an LDT segment for fs.
#define TEB_SIZE 0x1000
#define TEB_TLS 0xE10
#define TLS_SLOTS 64
static __thread uint8_t *my_teb;
static pthread_mutex_t ldt_lock = PTHREAD_MUTEX_INITIALIZER;
static int ldt_next = 1;
static void teb_install(void)
{
    if (my_teb) return;
    my_teb = calloc(1, TEB_SIZE);
    NT_TIB *tib = (NT_TIB *)my_teb;
    pthread_attr_t a;
    void *stack = 0;
    size_t size = 0;
    if (!pthread_getattr_np(pthread_self(), &a)) { pthread_attr_getstack(&a, &stack, &size); pthread_attr_destroy(&a); }
    tib->StackLimit = stack;
    tib->StackBase = (uint8_t *)stack + size;
    tib->Self = tib;
    *(void **)(my_teb + 0x18) = my_teb;
    *(DWORD *)(my_teb + 0x24) = (DWORD)syscall(SYS_gettid);   // ClientId.UniqueThread
    pthread_mutex_lock(&ldt_lock);
    int idx = ldt_next++;
    pthread_mutex_unlock(&ldt_lock);
    struct user_desc d = {0};
    d.entry_number = idx;
    d.base_addr = (uintptr_t)my_teb;
    d.limit = TEB_SIZE - 1;
    d.seg_32bit = 1;
    d.useable = 1;
    if (syscall(SYS_modify_ldt, 1, &d, sizeof d) != 0) { fprintf(stderr, "modify_ldt failed (%d)\n", errno); abort(); }
    uint16_t sel = (uint16_t)(idx << 3 | 7);   // LDT, ring 3
    __asm__ volatile("movw %0, %%fs" : : "r"(sel));
}
void compat_thread_attach(void) { teb_install(); }
void *NtCurrentTeb(void) { teb_install(); return my_teb; }

static uint8_t slot_used[TLS_SLOTS];
DWORD TlsAlloc(void)
{
    pthread_mutex_lock(&ldt_lock);
    for (DWORD i = 0; i < TLS_SLOTS; i++)
        if (!slot_used[i]) { slot_used[i] = 1; pthread_mutex_unlock(&ldt_lock); return i; }
    pthread_mutex_unlock(&ldt_lock);
    return TLS_OUT_OF_INDEXES;
}
LPVOID TlsGetValue(DWORD slot) { teb_install(); last_error = 0; return ((void **)(my_teb + TEB_TLS))[slot]; }
BOOL TlsSetValue(DWORD slot, LPVOID v) { teb_install(); ((void **)(my_teb + TEB_TLS))[slot] = v; return 1; }

// ---- handles ------------------------------------------------------------------------------------------------------
struct compat_handle *compat_new_handle(int kind)
{
    struct compat_handle *h = calloc(1, sizeof *h);
    h->kind = kind;
    h->fd = -1;
    pthread_mutex_init(&h->m, 0);
    pthread_cond_init(&h->c, 0);
    return h;
}
static void signal_handle(struct compat_handle *h)
{
    pthread_mutex_lock(&h->m);
    h->signaled = 1;
    pthread_cond_broadcast(&h->c);
    pthread_mutex_unlock(&h->m);
}

// ---- threads ------------------------------------------------------------------------------------------------------
static __thread struct compat_handle *self_thread;   // the handle CreateThread made for this thread
static void *thread_start(void *p)
{
    struct compat_handle *h = self_thread = p;
    teb_install();
    h->tid = (DWORD)syscall(SYS_gettid);
    pthread_mutex_lock(&h->m);
    while (h->suspended) pthread_cond_wait(&h->c, &h->m);
    pthread_mutex_unlock(&h->m);
    h->code = h->fn(h->arg);
    signal_handle(h);
    return 0;
}
HANDLE CreateThread(void *sec, SIZE_T stack, LPTHREAD_START_ROUTINE fn, void *arg, DWORD flags, DWORD *tid)
{
    (void)sec;
    struct compat_handle *h = compat_new_handle(H_THREAD);
    h->fn = fn; h->arg = arg; h->suspended = (flags & CREATE_SUSPENDED) != 0;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack > 1u << 20 ? stack : 1u << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&h->thread, &a, thread_start, h)) { pthread_attr_destroy(&a); free(h); return 0; }
    pthread_attr_destroy(&a);
    for (int i = 0; i < 1000 && !h->tid; i++) usleep(50);   // the id is the new thread's own
    if (tid) *tid = h->tid;
    return h;
}
DWORD ResumeThread(HANDLE p)
{
    struct compat_handle *h = p;
    if (!h || h->kind != H_THREAD) return (DWORD)-1;
    pthread_mutex_lock(&h->m);
    DWORD was = h->suspended;
    h->suspended = 0;
    pthread_cond_broadcast(&h->c);
    pthread_mutex_unlock(&h->m);
    return was;
}
// The process's first thread doesn't end: Linux takes /proc/self/exe away from a process whose first thread has
// exited (DXVK reads it), so where Windows lets the process go on with its other threads, this one just waits.
void ExitThread(DWORD code)
{
    if (syscall(SYS_gettid) == getpid()) for (;;) pause();
    if (self_thread) { self_thread->code = code; signal_handle(self_thread); }
    pthread_exit(0);
}
void ExitProcess(UINT code) { fflush(0); _exit((int)code); }
DWORD GetCurrentThreadId(void) { return (DWORD)syscall(SYS_gettid); }
DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
BOOL SetThreadPriority(HANDLE h, int prio) { (void)h; (void)prio; return 1; }
BOOL GetThreadTimes(HANDLE h, FILETIME *create, FILETIME *exit, FILETIME *kernel, FILETIME *user)
{
    (void)h; (void)create; (void)exit;
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    uint64_t v = (uint64_t)t.tv_sec * 10000000 + t.tv_nsec / 100;   // all of it as user time
    if (kernel) *kernel = (FILETIME){0, 0};
    if (user) *user = (FILETIME){(DWORD)v, (DWORD)(v >> 32)};
    return 1;
}

// ---- waiting --------------------------------------------------------------------------------------------------------
static void deadline(struct timespec *t, DWORD ms)
{
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += ms / 1000;
    t->tv_nsec += (long)(ms % 1000) * 1000000;
    if (t->tv_nsec >= 1000000000) { t->tv_sec++; t->tv_nsec -= 1000000000; }
}
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec; }
// One object: a thread, an event, a mutex or a timer. Takes the object (an auto-reset event, a mutex) when it's taken.
DWORD compat_wait(struct compat_handle *h, DWORD ms)
{
    if (!h || h == INVALID_HANDLE_VALUE) return WAIT_FAILED;
    if (h->kind == H_TIMER) {   // a waitable timer: sleep to its due time
        uint64_t due = h->due;
        while (now_ns() < due) {
            uint64_t left = due - now_ns();
            struct timespec t = {(time_t)(left / 1000000000u), (long)(left % 1000000000u)};
            nanosleep(&t, 0);
        }
        return WAIT_OBJECT_0;
    }
    struct timespec t;
    if (ms != INFINITE) deadline(&t, ms);
    pthread_mutex_lock(&h->m);
    DWORD r = WAIT_OBJECT_0;
    for (;;) {
        int ready = h->kind == H_MUTEX ? (!h->owner || h->owner == GetCurrentThreadId()) : h->signaled;
        if (ready) {
            if (h->kind == H_EVENT && !h->manual) h->signaled = 0;
            if (h->kind == H_MUTEX) { h->owner = GetCurrentThreadId(); h->count++; }
            break;
        }
        if (ms == 0) { r = WAIT_TIMEOUT; break; }
        if (ms == INFINITE) pthread_cond_wait(&h->c, &h->m);
        else if (pthread_cond_timedwait(&h->c, &h->m, &t) == ETIMEDOUT) { r = WAIT_TIMEOUT; break; }
    }
    pthread_mutex_unlock(&h->m);
    return r;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return compat_wait(h, ms); }

HANDLE CreateEventW(void *sec, BOOL manual, BOOL initial, const wchar_t *name)
{
    (void)sec; (void)name;
    struct compat_handle *h = compat_new_handle(H_EVENT);
    h->manual = manual; h->signaled = initial;
    return h;
}
BOOL SetEvent(HANDLE p) { struct compat_handle *h = p; if (!h) return 0; signal_handle(h); return 1; }
BOOL ResetEvent(HANDLE p) { struct compat_handle *h = p; if (!h) return 0; pthread_mutex_lock(&h->m); h->signaled = 0; pthread_mutex_unlock(&h->m); return 1; }

// A named mutex is the one-game-per-install check (host.c): a lock file in /tmp named after it, held for good.
HANDLE CreateMutexW(void *sec, BOOL owner, const wchar_t *name)
{
    (void)sec;
    struct compat_handle *h = compat_new_handle(H_MUTEX);
    if (owner) { h->owner = GetCurrentThreadId(); h->count = 1; }
    last_error = 0;
    if (name) {
        char path[512] = "/tmp/", u[400];
        WideCharToMultiByte(CP_UTF8, 0, name, -1, u, sizeof u, 0, 0);
        for (char *c = u; *c; c++) if (*c == '/' || *c == '\\') *c = '_';
        strncat(path, u, sizeof path - 6);
        int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0) last_error = ERROR_ALREADY_EXISTS;
    }
    return h;
}
BOOL compat_release_mutex(HANDLE p)
{
    struct compat_handle *h = p;
    pthread_mutex_lock(&h->m);
    int ok = h->owner == GetCurrentThreadId();
    if (ok && !--h->count) { h->owner = 0; pthread_cond_broadcast(&h->c); }
    pthread_mutex_unlock(&h->m);
    return ok;
}

HANDLE CreateWaitableTimerExW(void *sec, const wchar_t *name, DWORD flags, DWORD access)
{
    (void)sec; (void)name; (void)flags; (void)access;
    return compat_new_handle(H_TIMER);
}
BOOL SetWaitableTimer(HANDLE p, const LARGE_INTEGER *due, LONG period, void *fn, void *arg, BOOL resume)
{
    (void)period; (void)fn; (void)arg; (void)resume;
    struct compat_handle *h = p;
    // relative (negative, 100 ns units) only: that's all the frame pacing uses
    h->due = now_ns() + (uint64_t)(due->QuadPart < 0 ? -due->QuadPart : 0) * 100;
    return 1;
}
BOOL CloseHandle(HANDLE p)
{
    struct compat_handle *h = p;
    if (!h || h == INVALID_HANDLE_VALUE || (intptr_t)h == -2) return 0;
    if (h->kind == H_FILE || h->kind == H_DIR) { if (h->fd >= 0) close(h->fd); if (h->dir) closedir(h->dir); free(h->path); }
    // threads and events are left allocated: another thread may still wait on them (handles are few and long-lived)
    if (h->kind == H_FILE || h->kind == H_DIR) free(h);
    return 1;
}
void Sleep(DWORD ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    if (!ms) { sched_yield(); return; }
    while (nanosleep(&t, &t) && errno == EINTR) {}
}

BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *c, SRWLOCK *l, DWORD ms, ULONG flags)
{
    (void)flags;
    if (ms == INFINITE) return pthread_cond_wait(c, l) == 0;
    struct timespec t;
    deadline(&t, ms);
    if (pthread_cond_timedwait(c, l, &t) == ETIMEDOUT) { last_error = WAIT_TIMEOUT; return 0; }
    return 1;
}
void InitializeCriticalSection(CRITICAL_SECTION *c)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(c, &a);
    pthread_mutexattr_destroy(&a);
}
BOOL InitOnceExecuteOnce(INIT_ONCE *once, PINIT_ONCE_FN fn, void *param, void **ctx)
{
    pthread_mutex_lock(&once->m);
    BOOL ok = 1;
    if (!once->done) { ok = fn(once, param, ctx); once->done = ok; }
    pthread_mutex_unlock(&once->m);
    return ok;
}
BOOL WaitOnAddress(volatile void *addr, void *cmp, SIZE_T size, DWORD ms)
{
    if (size != 4) {   // only 32-bit values use a futex; others poll
        if (memcmp((const void *)addr, cmp, size)) return 1;
        Sleep(1);
        return 1;
    }
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    long r = syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, *(uint32_t *)cmp, ms == INFINITE ? 0 : &t, 0, 0);
    if (r && errno == ETIMEDOUT) { last_error = 1460 /* ERROR_TIMEOUT */; return 0; }
    return 1;
}
void WakeByAddressSingle(void *addr) { syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0); }
void WakeByAddressAll(void *addr) { syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 0x7fffffff, 0, 0, 0); }

// ---- memory -------------------------------------------------------------------------------------------------------
// Reservations are remembered (base, size) so MEM_RELEASE knows how much to unmap and VirtualQuery can answer
// without reading /proc for the game's own blocks: the texture code asks about every texture every frame. Each keeps
// a byte per page: 0 reserved, else the PAGE_* protection it was committed with.
#define MAX_RES 8192
static struct { uintptr_t base; size_t size; uint8_t *page; } res[MAX_RES];
static int nres;
static uint8_t page_state(DWORD prot) { return prot & 0xFF ? (uint8_t)(prot & 0xFF) : PAGE_READWRITE; }
// Sets the state of the pages from a (page-aligned) for size bytes, as far as reservation i goes.
static void set_pages(int i, uintptr_t a, size_t size, uint8_t state)
{
    if (!res[i].page) return;
    size_t first = (a - res[i].base) >> 12, n = res[i].size >> 12, k = size >> 12;
    if (first + k > n) k = n - first;
    memset(res[i].page + first, state, k);
}
static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;
static int prot_of(DWORD p)
{
    p &= 0xFF;
    return p == PAGE_NOACCESS ? PROT_NONE : p == PAGE_READONLY ? PROT_READ : p == PAGE_EXECUTE ? PROT_EXEC
         : p == PAGE_EXECUTE_READ ? PROT_READ | PROT_EXEC : p == PAGE_EXECUTE_READWRITE ? PROT_READ | PROT_WRITE | PROT_EXEC
         : PROT_READ | PROT_WRITE;
}
static int find_res(uintptr_t a)
{
    for (int i = 0; i < nres; i++) if (a >= res[i].base && a < res[i].base + res[i].size) return i;
    return -1;
}
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot)
{
    size = (size + 0xFFF) & ~(SIZE_T)0xFFF;
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)0xFFF;
    pthread_mutex_lock(&mem_lock);
    int r = a ? find_res(a) : -1;
    if (r >= 0) {   // committing pages of a reservation
        int ok = mprotect((void *)a, size, prot_of(prot)) == 0;
        if (ok) set_pages(r, a, size, page_state(prot));
        pthread_mutex_unlock(&mem_lock);
        if (!ok) { compat_set_errno_error(); return 0; }
        return (void *)a;
    }
    int p = type & MEM_COMMIT ? prot_of(prot) : PROT_NONE;
    // reserved to a 64 KB boundary as Windows does (the game's allocator assumes it)
    size_t want = (size + 0xFFFF) & ~(size_t)0xFFFF;
    void *m;
    if (a) m = mmap((void *)a, want, p, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    else {
        m = mmap(0, want + 0x10000, p, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (m != MAP_FAILED) {
            uintptr_t s = (uintptr_t)m, al = (s + 0xFFFF) & ~(uintptr_t)0xFFFF;
            if (al > s) munmap(m, al - s);
            if (s + want + 0x10000 > al + want) munmap((void *)(al + want), s + want + 0x10000 - (al + want));
            m = (void *)al;
        }
    }
    if (m == MAP_FAILED || (a && (uintptr_t)m != a)) {
        if (m != MAP_FAILED) munmap(m, want);
        pthread_mutex_unlock(&mem_lock);
        last_error = ERROR_NOT_ENOUGH_MEMORY;
        return 0;
    }
    if (nres < MAX_RES) {   // committed up to size; the rest of the 64 KB is reserved, as on Windows
        res[nres].base = (uintptr_t)m; res[nres].size = want; res[nres].page = calloc(want >> 12, 1);
        if (type & MEM_COMMIT) set_pages(nres, (uintptr_t)m, size, page_state(prot));
        nres++;
    }
    pthread_mutex_unlock(&mem_lock);
    return m;
}
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
    uintptr_t a = (uintptr_t)addr;
    pthread_mutex_lock(&mem_lock);
    int i = find_res(a);
    if (type & MEM_RELEASE) {
        if (i < 0 || res[i].base != a) { pthread_mutex_unlock(&mem_lock); return 0; }
        munmap(addr, res[i].size);
        free(res[i].page);
        res[i] = res[--nres];
    } else if (i >= 0) {   // decommit: give the pages back, keep the range
        size = size ? (size + 0xFFF) & ~(SIZE_T)0xFFF : res[i].size - (a - res[i].base);
        mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
        set_pages(i, a & ~(uintptr_t)0xFFF, size, 0);
    }
    pthread_mutex_unlock(&mem_lock);
    return 1;
}
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, DWORD *old)
{
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)0xFFF;
    size = ((uintptr_t)addr + size - a + 0xFFF) & ~(SIZE_T)0xFFF;
    if (old) *old = PAGE_EXECUTE_READWRITE;
    if (mprotect((void *)a, size, prot_of(prot))) return 0;
    pthread_mutex_lock(&mem_lock);
    int i = find_res(a);
    if (i >= 0 && res[i].page) {   // the committed pages take the new protection
        size_t first = (a - res[i].base) >> 12, n = res[i].size >> 12;
        if (old && res[i].page[first]) *old = res[i].page[first];
        for (size_t k = first; k < n && k < first + (size >> 12); k++) if (res[i].page[k]) res[i].page[k] = page_state(prot);
    }
    pthread_mutex_unlock(&mem_lock);
    return 1;
}
// The game's own blocks from the reservation table. Anything else (the C heap, where the host's D3D headers are, and
// libraries): a page this process can read is reported committed read-write on its own, which takes one system
// call. Only for one it can't is /proc/self/maps read, a millisecond's work: a mapped page counts as committed with
// the mapping's protection, an unmapped one as free.
SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T len)
{
    if (len < sizeof *mbi) return 0;
    uintptr_t a = (uintptr_t)addr;
    memset(mbi, 0, sizeof *mbi);
    mbi->BaseAddress = (void *)(a & ~(uintptr_t)0xFFF);
    pthread_mutex_lock(&mem_lock);
    int i = find_res(a);
    if (i >= 0 && res[i].page) {
        size_t first = (a - res[i].base) >> 12, n = res[i].size >> 12, k = first;
        uint8_t st = res[i].page[first];
        while (k < n && res[i].page[k] == st) k++;
        mbi->AllocationBase = (void *)res[i].base;
        mbi->RegionSize = (k - first) << 12;
        mbi->Type = MEM_PRIVATE;
        mbi->State = st ? MEM_COMMIT : MEM_RESERVE;
        mbi->Protect = mbi->AllocationProtect = st;   // 0 for reserved pages, as Windows reports
        pthread_mutex_unlock(&mem_lock);
        return sizeof *mbi;
    }
    pthread_mutex_unlock(&mem_lock);
    char byte;
    struct iovec to = {&byte, 1}, from = {(void *)a, 1};
    if (process_vm_readv(getpid(), &to, 1, &from, 1, 0) == 1) {
        mbi->AllocationBase = mbi->BaseAddress;
        mbi->RegionSize = 0x1000;
        mbi->Type = MEM_PRIVATE;
        mbi->State = MEM_COMMIT;
        mbi->Protect = mbi->AllocationProtect = PAGE_READWRITE;
        return sizeof *mbi;
    }
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    uintptr_t next = 0;
    while (f && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perm[5];
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perm) != 3) continue;
        if (a >= lo && a < hi) {
            fclose(f);
            mbi->AllocationBase = (void *)lo;
            mbi->RegionSize = hi - (uintptr_t)mbi->BaseAddress;
            mbi->Type = MEM_PRIVATE;
            int r = perm[0] == 'r', w = perm[1] == 'w', x = perm[2] == 'x';
            mbi->State = r || w || x ? MEM_COMMIT : MEM_RESERVE;
            mbi->Protect = !r && !w && !x ? PAGE_NOACCESS : x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : w ? PAGE_READWRITE : PAGE_READONLY;
            mbi->AllocationProtect = mbi->Protect;
            return sizeof *mbi;
        }
        if (lo > a && (!next || lo < next)) next = lo;
    }
    if (f) fclose(f);
    mbi->State = MEM_FREE;
    mbi->Protect = PAGE_NOACCESS;
    mbi->RegionSize = next ? next - (uintptr_t)mbi->BaseAddress : 0x1000;
    return sizeof *mbi;
}
int compat_reservations(uintptr_t (*out)[2], int max)
{
    pthread_mutex_lock(&mem_lock);
    int n = nres < max ? nres : max;
    for (int i = 0; i < n; i++) { out[i][0] = res[i].base; out[i][1] = res[i].size; }
    pthread_mutex_unlock(&mem_lock);
    return n;
}
BOOL IsBadReadPtr(const void *p, UINT_PTR n)
{
    MEMORY_BASIC_INFORMATION m;
    uintptr_t a = (uintptr_t)p, end = a + (n ? n : 1);
    while (a < end) {
        if (!VirtualQuery((void *)a, &m, sizeof m) || m.State != MEM_COMMIT || m.Protect == PAGE_NOACCESS) return 1;
        a = (uintptr_t)m.BaseAddress + m.RegionSize;
    }
    return 0;
}

// ---- time ---------------------------------------------------------------------------------------------------------
BOOL QueryPerformanceCounter(LARGE_INTEGER *v) { v->QuadPart = (int64_t)now_ns(); return 1; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER *v) { v->QuadPart = 1000000000; return 1; }
DWORD GetTickCount(void) { return (DWORD)(now_ns() / 1000000); }
ULONGLONG GetTickCount64(void) { return now_ns() / 1000000; }
static void tm_to_st(const struct tm *t, long ms, SYSTEMTIME *s)
{
    s->wYear = (WORD)(t->tm_year + 1900); s->wMonth = (WORD)(t->tm_mon + 1); s->wDayOfWeek = (WORD)t->tm_wday;
    s->wDay = (WORD)t->tm_mday; s->wHour = (WORD)t->tm_hour; s->wMinute = (WORD)t->tm_min; s->wSecond = (WORD)t->tm_sec;
    s->wMilliseconds = (WORD)ms;
}
void GetLocalTime(SYSTEMTIME *s) { struct timespec t; struct tm m; clock_gettime(CLOCK_REALTIME, &t); localtime_r(&t.tv_sec, &m); tm_to_st(&m, t.tv_nsec / 1000000, s); }
void GetSystemTime(SYSTEMTIME *s) { struct timespec t; struct tm m; clock_gettime(CLOCK_REALTIME, &t); gmtime_r(&t.tv_sec, &m); tm_to_st(&m, t.tv_nsec / 1000000, s); }
#define EPOCH_DIFF 116444736000000000ull   // 1601 to 1970, in 100 ns
void GetSystemTimeAsFileTime(FILETIME *f)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    uint64_t v = (uint64_t)t.tv_sec * 10000000 + t.tv_nsec / 100 + EPOCH_DIFF;
    f->dwLowDateTime = (DWORD)v; f->dwHighDateTime = (DWORD)(v >> 32);
}
BOOL FileTimeToSystemTime(const FILETIME *f, SYSTEMTIME *s)
{
    uint64_t v = (uint64_t)f->dwHighDateTime << 32 | f->dwLowDateTime;
    if (v < EPOCH_DIFF) return 0;
    time_t secs = (time_t)((v - EPOCH_DIFF) / 10000000);
    struct tm m;
    gmtime_r(&secs, &m);
    tm_to_st(&m, (long)((v / 10000) % 1000), s);
    return 1;
}

// ---- text ---------------------------------------------------------------------------------------------------------
// UTF-8 <-> wide (UTF-32 here). CP_ACP is taken as UTF-8 too: Linux has no other system code page.
int MultiByteToWideChar(UINT cp, DWORD flags, const char *s, int n, wchar_t *out, int outn)
{
    (void)cp; (void)flags;
    const unsigned char *p = (const unsigned char *)s, *e = n < 0 ? 0 : p + n;
    int k = 0;
    for (;;) {
        if (e ? p >= e : 0) break;
        uint32_t c = *p++;
        if (c >= 0xC0) {
            int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            c &= 0x3F >> more;
            while (more-- && (e ? p < e : 1) && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 0x3F);
        }
        if (outn) { if (k >= outn) { last_error = 122; return 0; } out[k] = (wchar_t)c; }
        k++;
        if (!e && c == 0) break;
    }
    return k;
}
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t *s, int n, char *out, int outn, const char *dflt, BOOL *used)
{
    (void)cp; (void)flags; (void)dflt;
    if (used) *used = 0;
    int k = 0;
    for (int i = 0; n < 0 || i < n; i++) {
        uint32_t c = (uint32_t)s[i];
        char b[4];
        int m = c < 0x80 ? (b[0] = (char)c, 1) : c < 0x800 ? (b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3F)), 2)
              : c < 0x10000 ? (b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 0x3F)), b[2] = (char)(0x80 | (c & 0x3F)), 3)
              : (b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | (c >> 12 & 0x3F)), b[2] = (char)(0x80 | (c >> 6 & 0x3F)), b[3] = (char)(0x80 | (c & 0x3F)), 4);
        if (outn) { if (k + m > outn) { last_error = 122; return 0; } memcpy(out + k, b, m); }
        k += m;
        if (n < 0 && c == 0) break;
    }
    return k;
}
DWORD GetEnvironmentVariableA(const char *name, char *buf, DWORD n)
{
    const char *v = !strcmp(name, "USERPROFILE") ? getenv("HOME") : getenv(name);
    if (!v) { last_error = 203; if (n) buf[0] = 0; return 0; }
    size_t l = strlen(v);
    if (l + 1 > n) return (DWORD)(l + 1);
    memcpy(buf, v, l + 1);
    return (DWORD)l;
}
DWORD GetEnvironmentVariableW(const wchar_t *name, wchar_t *buf, DWORD n)
{
    char nm[256], v[4096];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, nm, sizeof nm, 0, 0);
    DWORD l = GetEnvironmentVariableA(nm, v, sizeof v);
    if (!l || l >= sizeof v) return 0;
    int w = MultiByteToWideChar(CP_UTF8, 0, v, -1, 0, 0);
    if ((DWORD)w > n) return (DWORD)w;
    MultiByteToWideChar(CP_UTF8, 0, v, -1, buf, n);
    return (DWORD)(w - 1);
}

// ---- crashes ------------------------------------------------------------------------------------------------------
// Signals come in as the exceptions the vectored handler (host.c on_crash) expects; it may fix the context and go on.
static PVECTORED_EXCEPTION_HANDLER vectored;
static void on_signal(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = uc_;
    greg_t *g = uc->uc_mcontext.gregs;
    EXCEPTION_RECORD r = {0};
    CONTEXT c = {0};
    r.ExceptionCode = sig == SIGSEGV || sig == SIGBUS ? EXCEPTION_ACCESS_VIOLATION : sig == SIGILL ? EXCEPTION_ILLEGAL_INSTRUCTION
                    : sig == SIGFPE ? EXCEPTION_INT_DIVIDE_BY_ZERO : sig == SIGTRAP ? EXCEPTION_BREAKPOINT : 0xC0000001u;
    r.ExceptionAddress = (void *)g[REG_EIP];
    r.NumberParameters = 2;
    r.ExceptionInformation[0] = (g[REG_ERR] & 2) ? 1 : 0;
    r.ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
    c.Eax = g[REG_EAX]; c.Ebx = g[REG_EBX]; c.Ecx = g[REG_ECX]; c.Edx = g[REG_EDX]; c.Esi = g[REG_ESI]; c.Edi = g[REG_EDI];
    c.Ebp = g[REG_EBP]; c.Esp = g[REG_ESP]; c.Eip = g[REG_EIP]; c.EFlags = g[REG_EFL];
    static DWORD dr0;
    c.Dr0 = (DWORD)(uintptr_t)&dr0;   // on_crash reads *Dr0 (the watchpoint), which is never set here
    EXCEPTION_POINTERS ep = {&r, &c};
    if (vectored && vectored(&ep) == EXCEPTION_CONTINUE_EXECUTION) {
        g[REG_EAX] = c.Eax; g[REG_EBX] = c.Ebx; g[REG_ECX] = c.Ecx; g[REG_EDX] = c.Edx; g[REG_ESI] = c.Esi; g[REG_EDI] = c.Edi;
        g[REG_EBP] = c.Ebp; g[REG_ESP] = c.Esp; g[REG_EIP] = c.Eip; g[REG_EFL] = c.EFlags;
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
void *AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h)
{
    (void)first;
    vectored = h;
    static uint8_t alt[1 << 16];
    stack_t ss = {alt, 0, sizeof alt};
    sigaltstack(&ss, 0);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, 0);
    return (void *)h;
}

// ---- the rest -----------------------------------------------------------------------------------------------------
// Printed, and shown as SDL's dialog (it needs no window of the game's; where no dialog can be shown, it just returns).
int MessageBoxA(HWND w, const char *text, const char *title, UINT type)
{
    (void)w;
    fprintf(stderr, "%s: %s\n", title, text);
    UINT icon = type & 0xF0;
    SDL_ShowSimpleMessageBox(icon == MB_ICONERROR ? SDL_MESSAGEBOX_ERROR : icon == 0x30 /* MB_ICONWARNING */ ? SDL_MESSAGEBOX_WARNING
                             : SDL_MESSAGEBOX_INFORMATION, title, text, 0);
    return 1;   // IDOK
}
int MessageBoxW(HWND w, const wchar_t *text, const wchar_t *title, UINT type)
{
    char t[1024], c[256];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, t, sizeof t, 0, 0);
    WideCharToMultiByte(CP_UTF8, 0, title, -1, c, sizeof c, 0, 0);
    return MessageBoxA(w, t, c, type);
}
HMODULE GetModuleHandleW(const wchar_t *name) { (void)name; return (HMODULE)1; }
BOOL GetModuleHandleExW(DWORD flags, const wchar_t *name, HMODULE *out) { (void)flags; (void)name; *out = (HMODULE)1; return 1; }
BOOL GetModuleHandleExA(DWORD flags, const char *name, HMODULE *out) { (void)flags; (void)name; *out = (HMODULE)1; return 1; }

void compat_init(int argc, char **argv)
{
    (void)argc; (void)argv;
    teb_install();
    // DXVK Native: the window is SDL3's; its log goes in logs/ beside the game's (bin/onimusha -> logs), not the
    // current folder. A value set before the game starts is kept.
    setenv("DXVK_WSI_DRIVER", "SDL3", 0);
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 8);
    char *bin = n > 0 ? (exe[n] = 0, strrchr(exe, '/')) : 0;
    if (bin) { *bin = 0; bin = strrchr(exe, '/'); }
    if (bin) { strcpy(bin, "/logs"); setenv("DXVK_LOG_PATH", exe, 0); }
}
