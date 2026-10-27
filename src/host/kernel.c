// Xbox kernel (xboxkrnl.exe) exports used by Genma Onimusha, ordinal order at the bottom.
// Prototypes from nxdk's lib/xboxkrnl/xboxkrnl.h (CC0). Every argument is passed as a 32-bit value except
// by-value 64-bit ones, which is all the calling convention cares about.
//
// The Xbox kernel is a trimmed NT kernel: handles, wait semantics, file info classes, IO_STATUS_BLOCK and
// status codes match Windows NT. So most of this is a pass-through to ntdll with Xbox paths and ANSI
// names translated. Unimplemented functions log and fail.
#include "host.h"
#include "mods_list.h"
#include "layout.h"
#include "overlay.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// An open that can change the file: FILE_WRITE_DATA, FILE_APPEND_DATA or GENERIC_WRITE.
#define WRITE_ACCESS (0x2 | 0x4 | GENERIC_WRITE)

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "synchronization.lib")

#define STATUS_SUCCESS 0u
#define STATUS_NOT_IMPLEMENTED 0xC0000002u
#undef STATUS_INVALID_PARAMETER
#undef STATUS_NO_MEMORY
#define STATUS_INVALID_PARAMETER 0xC000000Du
#define STATUS_NO_MEMORY 0xC0000017u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define STATUS_BUFFER_TOO_SMALL 0xC0000023u
#define STATUS_END_OF_FILE 0xC0000011u
#define STATUS_BUFFER_OVERFLOW 0x80000005u
#define UNIMPL(...) (xlog("[kernel] UNIMPL " __VA_ARGS__), xfeature(1, "kernel: unimplemented %s", __FUNCTION__ + 2))
#define UNIMPL_CLASS(c) (xlog("[kernel] UNIMPL %s class %u", __FUNCTION__ + 2, c), xfeature(1, "kernel: unimplemented %s class %u", __FUNCTION__ + 2, c))
#define TRACE(...) xlog("[kernel] " __VA_ARGS__)

// ---- ntdll -------------------------------------------------------------------------------------------
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } NT_USTR;
typedef struct { ULONG Length; HANDLE RootDirectory; NT_USTR *ObjectName; ULONG Attributes; PVOID Sd, Qos; } NT_OA;
typedef struct { union { LONG Status; PVOID Pointer; }; ULONG_PTR Information; } NT_IOSB;
#define NTFN(ret, name, ...) __declspec(dllimport) ret NTAPI name(__VA_ARGS__);
NTFN(LONG, NtCreateFile, HANDLE *, ULONG, NT_OA *, NT_IOSB *, LARGE_INTEGER *, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG)
NTFN(LONG, NtReadFile, HANDLE, HANDLE, PVOID, PVOID, NT_IOSB *, PVOID, ULONG, LARGE_INTEGER *, ULONG *)
NTFN(LONG, NtWriteFile, HANDLE, HANDLE, PVOID, PVOID, NT_IOSB *, PVOID, ULONG, LARGE_INTEGER *, ULONG *)
NTFN(LONG, NtQueryInformationFile, HANDLE, NT_IOSB *, PVOID, ULONG, ULONG)
NTFN(LONG, NtSetInformationFile, HANDLE, NT_IOSB *, PVOID, ULONG, ULONG)
NTFN(LONG, NtQueryVolumeInformationFile, HANDLE, NT_IOSB *, PVOID, ULONG, ULONG)
NTFN(LONG, NtQueryDirectoryFile, HANDLE, HANDLE, PVOID, PVOID, NT_IOSB *, PVOID, ULONG, ULONG, BOOLEAN, NT_USTR *, BOOLEAN)
NTFN(LONG, NtFlushBuffersFile, HANDLE, NT_IOSB *)
NTFN(LONG, NtClose, HANDLE)
NTFN(LONG, NtCreateEvent, HANDLE *, ULONG, NT_OA *, ULONG, BOOLEAN)
NTFN(LONG, NtSetEvent, HANDLE, LONG *)
NTFN(LONG, NtCreateMutant, HANDLE *, ULONG, NT_OA *, BOOLEAN)
NTFN(LONG, NtReleaseMutant, HANDLE, LONG *)
NTFN(LONG, NtWaitForSingleObject, HANDLE, BOOLEAN, LARGE_INTEGER *)
NTFN(LONG, NtWaitForMultipleObjects, ULONG, HANDLE *, ULONG, BOOLEAN, LARGE_INTEGER *)
NTFN(LONG, NtDelayExecution, BOOLEAN, LARGE_INTEGER *)
NTFN(LONG, NtResumeThread, HANDLE, ULONG *)
NTFN(LONG, NtAllocateVirtualMemory, HANDLE, PVOID *, ULONG_PTR, SIZE_T *, ULONG, ULONG)
NTFN(LONG, NtFreeVirtualMemory, HANDLE, PVOID *, SIZE_T *, ULONG)
NTFN(ULONG, RtlNtStatusToDosError, LONG)
NTFN(VOID, RtlRaiseException, PEXCEPTION_RECORD)
NTFN(VOID, RtlUnwind, PVOID, PVOID, PEXCEPTION_RECORD, PVOID)

// ---- Xbox structures ---------------------------------------------------------------------------------
typedef struct { USHORT Length, MaximumLength; char *Buffer; } XSTR;              // OBJECT_STRING / ANSI_STRING
typedef struct { HANDLE RootDirectory; XSTR *ObjectName; ULONG Attributes; } XOA;  // OBJECT_ATTRIBUTES

// ---- threads: fake kernel thread / per-CPU block, reached through rewritten fs: reads (host.c) ---------
uint32_t tls_slot_kthread, tls_slot_prcb, tls_slot_irql, tls_slot_anchor;
static uint8_t fake_prcb[0x300];          // only +0x250 (debug monitor data) is read; 0 = none

enum { KT_STACK_BASE = 0x1C, KT_TLS_DATA = 0x28, KT_THREAD_ID = 0x12C, KT_SIZE = 0x200 };

// Xbox thread-local data sits at the top of the thread's stack; code finds it as fs:[4][_tls_index].
// We keep it in a separate block and give the code a fake "stack base" (anchor) pointing just past it.
static DWORD game_threads[64];
static volatile LONG game_thread_count;

static void attach_thread(uint32_t tls_size, int boot)
{
    uint8_t *kt = calloc(1, KT_SIZE);
    uint8_t *tls = calloc(1, tls_size + 16);
    if (!kt || !tls) xfatal("out of memory for a thread's TLS (%x bytes)", tls_size);
    if (boot) {   // nobody runs XapiThreadStartup on the boot thread: whatever _tls_index is, point it at a block
        uint32_t *scratch = calloc(1, 0x100);
        if (!scratch) xfatal("out of memory for the boot thread's TLS");
        for (uint32_t i = 0; i < tls_size / 4; i++) ((uint32_t *)tls)[i] = (uint32_t)scratch;
    }
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    *(uint32_t *)(kt + KT_STACK_BASE) = (uint32_t)tib->StackBase;
    *(uint32_t *)(kt + KT_TLS_DATA) = (uint32_t)tls;
    *(uint32_t *)(kt + KT_THREAD_ID) = GetCurrentThreadId();
    TlsSetValue(tls_slot_kthread, kt);
    TlsSetValue(tls_slot_prcb, fake_prcb);
    TlsSetValue(tls_slot_irql, 0);
    TlsSetValue(tls_slot_anchor, tls + tls_size);
    LONG n = InterlockedIncrement(&game_thread_count) - 1;
    if (n < 64) game_threads[n] = GetCurrentThreadId();
}

void kernel_attach_boot_thread(void) { attach_thread(0x100, 1); }

// On Xbox a thread's TLS is on its stack, so it goes when the thread does.
static void detach_thread(void)
{
    uint8_t *kt = TlsGetValue(tls_slot_kthread);
    if (!kt) return;
    free((void *)*(uint32_t *)(kt + KT_TLS_DATA));
    free(kt);
    TlsSetValue(tls_slot_kthread, 0);
}

// Debug: hardware write watchpoint (DR0) on every game thread; the exception handler in host.c logs who writes.
static uint32_t watch_addr, watch_len;
static DWORD WINAPI set_watch(void *_)
{
    for (LONG i = 0; i < game_thread_count && i < 64; i++) {
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, game_threads[i]);
        if (!t) continue;
        SuspendThread(t);
        CONTEXT c = {.ContextFlags = CONTEXT_DEBUG_REGISTERS};
        if (GetThreadContext(t, &c)) {
            c.Dr0 = watch_addr;
            c.Dr7 = (c.Dr7 & ~0xF0003u) | 1 | (1u << 16) | ((watch_len - 1) & 3) << 18;   // L0, break on write, length
            SetThreadContext(t, &c);
        }
        ResumeThread(t);
        CloseHandle(t);
    }
    return 0;
}
void kernel_watch_writes(uint32_t addr, uint32_t len)
{
    watch_addr = addr; watch_len = len == 4 ? 4 : len == 2 ? 2 : 1;   // DR7 length field (len - 1): 1,2,4 bytes -> 0,1,3
    HANDLE h = CreateThread(0, 0, set_watch, 0, 0, 0);   // a thread can't set its own debug registers while running
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}

struct thread_start { uint32_t tls_size; uint32_t start, context, system_routine; };

static DWORD WINAPI thread_trampoline(void *p)
{
    struct thread_start s = *(struct thread_start *)p;
    free(p);
    attach_thread(s.tls_size, 0);
    if (s.system_routine)   // XAPI's XapiThreadStartup(StartRoutine, StartContext)
        ((void(__stdcall *)(uint32_t, uint32_t))s.system_routine)(s.start, s.context);
    else
        ((void(__stdcall *)(uint32_t))s.start)(s.context);
    detach_thread();
    return 0;
}

// 255: NTSTATUS NTAPI PsCreateSystemThreadEx(...)
static uint32_t __stdcall k_PsCreateSystemThreadEx(uint32_t ThreadHandle, uint32_t ThreadExtensionSize, uint32_t KernelStackSize,
    uint32_t TlsDataSize, uint32_t ThreadId, uint32_t StartRoutine, uint32_t StartContext, uint32_t CreateSuspended,
    uint32_t DebuggerThread, uint32_t SystemRoutine)
{
    struct thread_start *s = malloc(sizeof *s);
    if (!s) return STATUS_NO_MEMORY;
    *s = (struct thread_start){TlsDataSize, StartRoutine, StartContext, SystemRoutine};
    DWORD tid;
    // Xbox threads ask for ~64 KB; PC drivers (D3D9 device creation, audio) need far more on the calling thread.
    HANDLE h = CreateThread(0, KernelStackSize < 0x100000 ? 0x100000 : KernelStackSize, thread_trampoline, s,
                            ((CreateSuspended & 0xFF) ? CREATE_SUSPENDED : 0) | STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    if (!h) { TRACE("PsCreateSystemThreadEx: CreateThread(stack %x) failed %lu", KernelStackSize, GetLastError()); free(s); return STATUS_NO_MEMORY; }
    TRACE("PsCreateSystemThreadEx start=%08x ctx=%08x tls=%x stack=%x -> tid %lu", StartRoutine, StartContext,
          TlsDataSize, KernelStackSize, tid);
    *(HANDLE *)ThreadHandle = h;
    if (ThreadId) *(uint32_t *)ThreadId = tid;
    return STATUS_SUCCESS;
}

// 258: VOID NTAPI PsTerminateSystemThread(IN NTSTATUS ExitStatus)
static void __stdcall k_PsTerminateSystemThread(uint32_t ExitStatus) { detach_thread(); ExitThread(ExitStatus); }

// 197: NTSTATUS NTAPI NtResumeThread(IN HANDLE ThreadHandle, OUT PULONG PreviousSuspendCount OPTIONAL)
static uint32_t __stdcall k_NtResumeThread(uint32_t ThreadHandle, uint32_t PreviousSuspendCount)
{ return NtResumeThread((HANDLE)ThreadHandle, (ULONG *)PreviousSuspendCount); }

// ---- IRQL / DPC / interrupts: only the replaced SDK internals use these ----------------------------------
// Initialising a DPC or timer does nothing; queueing a DPC or setting a timer is UNIMPL below (nothing would ever
// fire), so a cancel always finds the timer unset.
static uint32_t __fastcall k_KfRaiseIrql(uint32_t NewIrql) { return 0; }
static void __fastcall k_KfLowerIrql(uint32_t NewIrql) {}
static uint32_t __stdcall k_KeRaiseIrqlToDpcLevel(void) { return 0; }
static void __stdcall k_KeInitializeDpc(uint32_t Dpc, uint32_t DeferredRoutine, uint32_t DeferredContext) {}
static void __stdcall k_KeInitializeTimerEx(uint32_t Timer, uint32_t Type) {}
static uint32_t __stdcall k_KeCancelTimer(uint32_t Timer) { return 0; }

// ---- memory ------------------------------------------------------------------------------------------
static void *valloc(uint32_t n, uint32_t protect)
{
    return VirtualAlloc(0, n, MEM_COMMIT | MEM_RESERVE, protect ? (protect & 0xFF) : PAGE_READWRITE);
}

// 165: PVOID NTAPI MmAllocateContiguousMemory(IN SIZE_T NumberOfBytes)
static uint32_t __stdcall k_MmAllocateContiguousMemory(uint32_t NumberOfBytes) { return (uint32_t)valloc(NumberOfBytes, 0); }
// 166: PVOID NTAPI MmAllocateContiguousMemoryEx(...)
static uint32_t __stdcall k_MmAllocateContiguousMemoryEx(uint32_t NumberOfBytes, uint32_t LowestAcceptableAddress,
    uint32_t HighestAcceptableAddress, uint32_t Alignment, uint32_t Protect)
{ return (uint32_t)valloc(NumberOfBytes, Protect); }   // ponytail: ignores Alignment > 64K, no caller seen needing it
// 171: VOID NTAPI MmFreeContiguousMemory(IN PVOID BaseAddress)
static void __stdcall k_MmFreeContiguousMemory(uint32_t BaseAddress) { VirtualFree((void *)BaseAddress, 0, MEM_RELEASE); }
// 167: PVOID NTAPI MmAllocateSystemMemory(IN SIZE_T NumberOfBytes, IN ULONG Protect)
static uint32_t __stdcall k_MmAllocateSystemMemory(uint32_t NumberOfBytes, uint32_t Protect) { return (uint32_t)valloc(NumberOfBytes, Protect); }
// 172: ULONG NTAPI MmFreeSystemMemory(IN PVOID BaseAddress, IN SIZE_T NumberOfBytes)
static uint32_t __stdcall k_MmFreeSystemMemory(uint32_t BaseAddress, uint32_t NumberOfBytes)
{ VirtualFree((void *)BaseAddress, 0, MEM_RELEASE); return NumberOfBytes; }
// 180: SIZE_T NTAPI MmQueryAllocationSize(IN PVOID BaseAddress)
static uint32_t __stdcall k_MmQueryAllocationSize(uint32_t BaseAddress)
{
    MEMORY_BASIC_INFORMATION m;
    uint32_t total = 0;
    for (uint8_t *p = (uint8_t *)BaseAddress; VirtualQuery(p, &m, sizeof m) && m.AllocationBase == (void *)BaseAddress
         && m.State == MEM_COMMIT; p += m.RegionSize)
        total += (uint32_t)m.RegionSize;
    return total;
}
static void __stdcall k_MmSetAddressProtect(uint32_t BaseAddress, uint32_t NumberOfBytes, uint32_t NewProtect) {}
static uint32_t __stdcall k_MmQueryAddressProtect(uint32_t VirtualAddress) { return PAGE_READWRITE; }
static void __stdcall k_MmPersistContiguousMemory(uint32_t BaseAddress, uint32_t NumberOfBytes, uint32_t Persist) {}
static void __stdcall k_MmLockUnlockBufferPages(uint32_t BaseAddress, uint32_t NumberOfBytes, uint32_t UnlockPages) {}
static void __stdcall k_MmLockUnlockPhysicalPage(uint32_t PhysicalAddress, uint32_t UnlockPage) {}
static uint32_t __stdcall k_MmGetPhysicalAddress(uint32_t BaseAddress) { return BaseAddress; }

// 169: PVOID NTAPI MmCreateKernelStack(IN SIZE_T NumberOfBytes, IN BOOLEAN DebuggerThread) — returns the stack top
static uint32_t __stdcall k_MmCreateKernelStack(uint32_t NumberOfBytes, uint32_t DebuggerThread)
{
    uint8_t *p = valloc(NumberOfBytes, 0);
    return p ? (uint32_t)(p + NumberOfBytes) : 0;
}
// 170: VOID NTAPI MmDeleteKernelStack(IN PVOID KernelStackBase, IN PVOID KernelStackLimit)
static void __stdcall k_MmDeleteKernelStack(uint32_t KernelStackBase, uint32_t KernelStackLimit)
{ VirtualFree((void *)KernelStackLimit, 0, MEM_RELEASE); }

// 184: NTSTATUS NTAPI NtAllocateVirtualMemory(IN OUT PVOID *BaseAddress, IN ULONG_PTR ZeroBits, IN OUT PSIZE_T RegionSize, IN ULONG AllocationType, IN ULONG Protect)
static uint32_t __stdcall k_NtAllocateVirtualMemory(uint32_t BaseAddress, uint32_t ZeroBits, uint32_t RegionSize, uint32_t AllocationType, uint32_t Protect)
{
    // Straight to NT's, which rounds and writes back base and size. Flag and protection bits NT doesn't know are
    // masked off; ZeroBits is ignored, as it always was here.
    void **base = (void **)BaseAddress, *want = *base;
    SIZE_T *size = (SIZE_T *)RegionSize, want_size = *size;
    LONG st = NtAllocateVirtualMemory((HANDLE)-1, base, 0, size, AllocationType & (MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN), Protect & 0x7FF);
    if (st < 0) TRACE("NtAllocateVirtualMemory(%p, %lx, %x, %x) failed %08lx", want, (unsigned long)want_size, AllocationType, Protect, st);
    return st;
}
// 199: NTSTATUS NTAPI NtFreeVirtualMemory(IN OUT PVOID *BaseAddress, IN OUT PSIZE_T RegionSize, IN ULONG FreeType)
static uint32_t __stdcall k_NtFreeVirtualMemory(uint32_t BaseAddress, uint32_t RegionSize, uint32_t FreeType)
{
    SIZE_T *size = (SIZE_T *)RegionSize;
    if (FreeType == MEM_RELEASE) *size = 0;   // a release frees the whole allocation, as it did through VirtualFree
    return NtFreeVirtualMemory((HANDLE)-1, (void **)BaseAddress, size, FreeType);
}
// 217: NTSTATUS NTAPI NtQueryVirtualMemory(IN PVOID BaseAddress, OUT PMEMORY_BASIC_INFORMATION MemoryInformation)
static uint32_t __stdcall k_NtQueryVirtualMemory(uint32_t BaseAddress, uint32_t MemoryInformation)
{   // Xbox MEMORY_BASIC_INFORMATION has the same 28-byte layout as Win32's
    return VirtualQuery((void *)BaseAddress, (MEMORY_BASIC_INFORMATION *)MemoryInformation, 28) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

// pool
static uint32_t __stdcall k_ExAllocatePool(uint32_t NumberOfBytes) { return (uint32_t)malloc(NumberOfBytes); }
static uint32_t __stdcall k_ExAllocatePoolWithTag(uint32_t NumberOfBytes, uint32_t Tag) { return (uint32_t)malloc(NumberOfBytes); }
static void __stdcall k_ExFreePool(uint32_t P) { free((void *)P); }
static uint32_t __stdcall k_ExQueryPoolBlockSize(uint32_t PoolBlock) { return (uint32_t)_msize((void *)PoolBlock); }

// ---- synchronisation ---------------------------------------------------------------------------------
// 189: NTSTATUS NTAPI NtCreateEvent(OUT PHANDLE EventHandle, IN POBJECT_ATTRIBUTES ObjectAttributes OPTIONAL, IN EVENT_TYPE EventType, IN BOOLEAN InitialState)
static uint32_t __stdcall k_NtCreateEvent(uint32_t EventHandle, uint32_t ObjectAttributes, uint32_t EventType, uint32_t InitialState)
{   // ponytail: event names ignored — named events would only matter across processes
    return NtCreateEvent((HANDLE *)EventHandle, EVENT_ALL_ACCESS, 0, EventType, (BOOLEAN)InitialState);
}
static uint32_t __stdcall k_NtSetEvent(uint32_t EventHandle, uint32_t PreviousState) { return NtSetEvent((HANDLE)EventHandle, (LONG *)PreviousState); }
static uint32_t __stdcall k_NtCreateMutant(uint32_t MutantHandle, uint32_t ObjectAttributes, uint32_t InitialOwner)
{ return NtCreateMutant((HANDLE *)MutantHandle, MUTANT_ALL_ACCESS, 0, (BOOLEAN)InitialOwner); }
static uint32_t __stdcall k_NtReleaseMutant(uint32_t MutantHandle, uint32_t PreviousCount) { return NtReleaseMutant((HANDLE)MutantHandle, (LONG *)PreviousCount); }
static uint32_t __stdcall k_NtWaitForSingleObject(uint32_t Handle, uint32_t Alertable, uint32_t Timeout)
{ return NtWaitForSingleObject((HANDLE)Handle, (BOOLEAN)Alertable, (LARGE_INTEGER *)Timeout); }
static uint32_t __stdcall k_NtWaitForSingleObjectEx(uint32_t Handle, uint32_t WaitMode, uint32_t Alertable, uint32_t Timeout)
{ return NtWaitForSingleObject((HANDLE)Handle, (BOOLEAN)Alertable, (LARGE_INTEGER *)Timeout); }
static uint32_t __stdcall k_NtWaitForMultipleObjectsEx(uint32_t Count, uint32_t Handles, uint32_t WaitType, uint32_t WaitMode, uint32_t Alertable, uint32_t Timeout)
{ return NtWaitForMultipleObjects(Count, (HANDLE *)Handles, WaitType, (BOOLEAN)Alertable, (LARGE_INTEGER *)Timeout); }
static uint32_t __stdcall k_KeDelayExecutionThread(uint32_t WaitMode, uint32_t Alertable, uint32_t Interval)
{ return NtDelayExecution((BOOLEAN)Alertable, (LARGE_INTEGER *)Interval); }
static uint32_t __stdcall k_NtClose(uint32_t Handle) { movie_close((HANDLE)Handle); movie_sound_close((HANDLE)Handle); return NtClose((HANDLE)Handle); }

// Objects: we hand out the handle itself as the "object pointer"; the Ke* calls below accept that back.
// ponytail: breaks if anyone reads fields of a real KEVENT/KTHREAD through the pointer, or passes a KEVENT it built in
// memory (no KeInitializeEvent import, so none seen yet); KeSetEvent logs when what it gets isn't a handle.
static uint8_t kv_ExEventObjectType[64];
static uint32_t __stdcall k_ObReferenceObjectByHandle(uint32_t Handle, uint32_t ObjectType, uint32_t ReturnedObject)
{ *(uint32_t *)ReturnedObject = Handle; return STATUS_SUCCESS; }
static void __fastcall k_ObfDereferenceObject(uint32_t Object) {}
static uint32_t __stdcall k_KeWaitForSingleObject(uint32_t Object, uint32_t WaitReason, uint32_t WaitMode, uint32_t Alertable, uint32_t Timeout)
{ return NtWaitForSingleObject((HANDLE)Object, (BOOLEAN)Alertable, (LARGE_INTEGER *)Timeout); }
static uint32_t __stdcall k_KeSetEvent(uint32_t Event, uint32_t Increment, uint32_t Wait)
{
    LONG prev = 0, st = NtSetEvent((HANDLE)Event, &prev);
    if (st < 0) TRACE("KeSetEvent(%x) -> %08lx", Event, st);
    return prev;
}

// Critical sections: implemented on the Xbox struct's own RecursionCount/OwningThread fields, so it works
// whether or not the game initialised it (the struct is 28 bytes: 16-byte header, LockCount, Recursion, Owner).
typedef struct { uint8_t header[16]; volatile LONG LockCount, RecursionCount; volatile LONG OwningThread; } XCS;
static void __stdcall k_RtlInitializeCriticalSection(uint32_t CriticalSection)
{ XCS *c = (XCS *)CriticalSection; memset(c, 0, sizeof *c); c->LockCount = -1; }
static void __stdcall k_RtlEnterCriticalSection(uint32_t CriticalSection)
{
    XCS *c = (XCS *)CriticalSection;
    LONG me = (LONG)GetCurrentThreadId();
    if (c->OwningThread == me) { c->RecursionCount++; return; }
    for (LONG cur; (cur = InterlockedCompareExchange(&c->OwningThread, me, 0)) != 0;)
        WaitOnAddress(&c->OwningThread, &cur, sizeof cur, INFINITE);
    c->RecursionCount = 1;
}
static void __stdcall k_RtlLeaveCriticalSection(uint32_t CriticalSection)
{
    XCS *c = (XCS *)CriticalSection;
    if (--c->RecursionCount == 0) {
        InterlockedExchange(&c->OwningThread, 0);
        WakeByAddressSingle((void *)&c->OwningThread);
    }
}

// ---- time --------------------------------------------------------------------------------------------
static volatile uint32_t kv_KeTickCount[1];   // one ULONG, ms since boot
static DWORD WINAPI tick_thread(void *_)
{
    timeBeginPeriod(1);
    for (;;) { kv_KeTickCount[0] = GetTickCount(); Sleep(1); }
}
#pragma comment(lib, "winmm.lib")

static uint64_t __stdcall k_KeQueryPerformanceCounter(void) { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }
static uint64_t __stdcall k_KeQueryPerformanceFrequency(void) { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v.QuadPart; }
static void __stdcall k_KeQuerySystemTime(uint32_t CurrentTime) { GetSystemTimeAsFileTime((FILETIME *)CurrentTime); }
static uint64_t __stdcall k_KeQueryInterruptTime(void)   // 100 ns units since boot
{ LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (uint64_t)(c.QuadPart / (double)f.QuadPart * 1e7); }
static void __stdcall k_KeStallExecutionProcessor(uint32_t MicroSeconds)
{
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    do QueryPerformanceCounter(&b); while ((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart < MicroSeconds);
}
// 304: VOID NTAPI RtlTimeToTimeFields(IN PLARGE_INTEGER Time, OUT PTIME_FIELDS TimeFields)
static void __stdcall k_RtlTimeToTimeFields(uint32_t Time, uint32_t TimeFields)
{
    SYSTEMTIME s;
    if (!FileTimeToSystemTime((FILETIME *)Time, &s)) memset(&s, 0, sizeof s);   // past year 30827
    int16_t *f = (int16_t *)TimeFields;   // Year Month Day Hour Minute Second Milliseconds Weekday
    f[0] = s.wYear; f[1] = s.wMonth; f[2] = s.wDay; f[3] = s.wHour; f[4] = s.wMinute; f[5] = s.wSecond;
    f[6] = s.wMilliseconds; f[7] = s.wDayOfWeek;
}

// ---- Rtl ---------------------------------------------------------------------------------------------
static void __stdcall k_RtlInitAnsiString(uint32_t DestinationString, uint32_t SourceString)
{
    XSTR *d = (XSTR *)DestinationString;
    d->Buffer = (char *)SourceString;
    d->Length = SourceString ? (USHORT)strlen(d->Buffer) : 0;
    d->MaximumLength = SourceString ? d->Length + 1 : 0;
}
static uint32_t __stdcall k_RtlEqualString(uint32_t String1, uint32_t String2, uint32_t CaseInSensitive)
{
    XSTR *a = (XSTR *)String1, *b = (XSTR *)String2;
    if (a->Length != b->Length) return 0;
    return (CaseInSensitive & 0xFF) ? !_strnicmp(a->Buffer, b->Buffer, a->Length) : !memcmp(a->Buffer, b->Buffer, a->Length);
}
static uint32_t __stdcall k_RtlCompareMemoryUlong(uint32_t Source, uint32_t Length, uint32_t Pattern)
{
    uint32_t *p = (uint32_t *)Source, n = 0;
    while (n + 4 <= Length && *p++ == Pattern) n += 4;
    return n;
}
static uint32_t __stdcall k_RtlNtStatusToDosError(uint32_t Status) { return RtlNtStatusToDosError((LONG)Status); }
static void __stdcall k_RtlRaiseException(uint32_t ExceptionRecord) { RtlRaiseException((PEXCEPTION_RECORD)ExceptionRecord); }
// fs:[0] SEH chain is shared with Windows, so ntdll's unwinder walks the Xbox frames correctly.
static void __stdcall k_RtlUnwind(uint32_t TargetFrame, uint32_t TargetIp, uint32_t ExceptionRecord, uint32_t ReturnValue)
{ RtlUnwind((PVOID)TargetFrame, (PVOID)TargetIp, (PEXCEPTION_RECORD)ExceptionRecord, (PVOID)ReturnValue); }

static uint32_t __cdecl k_DbgPrint(uint32_t Format, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, Format);
    vsnprintf(buf, sizeof buf, (const char *)Format, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    xlog("[DbgPrint] %s", buf);
    return 0;
}

// ---- SHA-1 (XcSHA*: save-game signatures; any correct SHA-1 keeps them consistent) ------------------
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; } SHA1;
static uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }
static void sha1_block(SHA1 *s, const uint8_t *p)
{
    uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 16; i++) w[i] = p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
        uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void __stdcall k_XcSHAInit(uint32_t ctx)
{ *(SHA1 *)ctx = (SHA1){{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}, 0}; }
static void __stdcall k_XcSHAUpdate(uint32_t ctx, uint32_t input, uint32_t len)
{
    SHA1 *s = (SHA1 *)ctx;
    const uint8_t *p = (const uint8_t *)input;
    for (uint32_t i = 0; i < len; i++) {
        s->buf[s->len++ % 64] = p[i];
        if (s->len % 64 == 0) sha1_block(s, s->buf);
    }
}
static void __stdcall k_XcSHAFinal(uint32_t ctx, uint32_t digest)
{
    SHA1 *s = (SHA1 *)ctx;
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, z = 0, lenb[8];
    k_XcSHAUpdate(ctx, (uint32_t)&pad, 1);
    while (s->len % 64 != 56) k_XcSHAUpdate(ctx, (uint32_t)&z, 1);
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    k_XcSHAUpdate(ctx, (uint32_t)lenb, 8);
    uint8_t *d = (uint8_t *)digest;
    for (int i = 0; i < 20; i++) d[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}
static uint8_t kv_XboxHDKey[16], kv_XboxSignatureKey[16];

// ---- system / settings -------------------------------------------------------------------------------
static uint32_t kv_XboxHardwareInfo[2] = {0x00000020, 0x0000D3D3};   // ponytail: plausible retail values, unverified
static uint32_t kv_HalDiskCachePartitionCount[1] = {3};
static uint32_t kv_HalBootSMCVideoMode[1] = {0};
static uint32_t kv_LaunchDataPage[1] = {0};                         // no launch data: cold boot

// 24: NTSTATUS NTAPI ExQueryNonVolatileSetting(IN ULONG ValueIndex, OUT PULONG Type, OUT PVOID Value, IN ULONG ValueLength, OUT PULONG ResultLength)
static uint32_t __stdcall k_ExQueryNonVolatileSetting(uint32_t ValueIndex, uint32_t Type, uint32_t Value, uint32_t ValueLength, uint32_t ResultLength)
{
    uint32_t v = 0;
    switch (ValueIndex) {      // indices from nxdk's XC_* defines; values from memory — unverified
    case 0x0007: v = 1; break;            // XC_LANGUAGE: English
    case 0x0103: v = 0x00400100; break;   // XC_FACTORY_AV_REGION: NTSC-M
    case 0x0104: v = 1; break;            // XC_FACTORY_GAME_REGION: North America
    }
    TRACE("ExQueryNonVolatileSetting(%x) -> %x", ValueIndex, v);
    if (Type) *(uint32_t *)Type = 4;       // REG_DWORD
    if (ResultLength) *(uint32_t *)ResultLength = 4;
    if (!Value || ValueLength < 4) return STATUS_BUFFER_TOO_SMALL;
    memset((void *)Value, 0, ValueLength);
    memcpy((void *)Value, &v, 4);
    return STATUS_SUCCESS;
}

static void __stdcall k_KeBugCheck(uint32_t BugCheckCode) { xfatal("KeBugCheck(%x)", BugCheckCode); }
static void __stdcall k_HalReturnToFirmware(uint32_t Routine) { xlog("HalReturnToFirmware(%u): game exited", Routine); xbacktrace("HalReturnToFirmware"); ExitProcess(0); }
static void __stdcall k_HalInitiateShutdown(void) { xlog("HalInitiateShutdown"); ExitProcess(0); }
static uint32_t __stdcall k_HalIsResetOrShutdownPending(void) { return 0; }
static void __stdcall k_HalRegisterShutdownNotification(uint32_t ShutdownRegistration, uint32_t Register) {}
static uint32_t __stdcall k_AvGetSavedDataAddress(void) { return 0; }
static void __stdcall k_AvSetSavedDataAddress(uint32_t Address) {}
static uint32_t __stdcall k_FscSetCacheSize(uint32_t NumberOfCachePages) { return STATUS_SUCCESS; }

// XBE sections are all mapped at load (every one is flagged preload); just keep the reference counts honest.
// [6]: the section header's reference count (+0x18).
static uint32_t __stdcall k_XeLoadSection(uint32_t Section) { ((uint32_t *)Section)[6]++; return STATUS_SUCCESS; }
static uint32_t __stdcall k_XeUnloadSection(uint32_t Section) { if (((uint32_t *)Section)[6]) ((uint32_t *)Section)[6]--; return STATUS_SUCCESS; }

// ---- files -------------------------------------------------------------------------------------------
// Xbox object names are ANSI: "\??\D:\image\x.wd", "\Device\CdRom0\...", "\Device\Harddisk0\Partition1\...".
// Drive letters resolve through symbolic links (XAPI creates T:, U:, Z: at startup); devices map to folders.
static SRWLOCK link_lock = SRWLOCK_INIT;
static struct { char name[64], target[192]; } links[32];
static int nlinks;

static void add_link(const char *name, const char *target)
{
    AcquireSRWLockExclusive(&link_lock);
    int i = 0;
    while (i < nlinks && _stricmp(links[i].name, name)) i++;
    if (i == nlinks && nlinks < 32) nlinks++;
    if (i < 32) {
        snprintf(links[i].name, sizeof links[i].name, "%s", name);
        snprintf(links[i].target, sizeof links[i].target, "%s", target);
    }
    ReleaseSRWLockExclusive(&link_lock);
}

// E: is randomizer\<run>\saves while a randomizer run is played (randomizer.c, set in kernel_init)
static wchar_t saves_dir[96] = L"saves";
static const struct { const char *dev; const wchar_t *dir; } devices[] = {
    {"\\Device\\CdRom0", L"game"},
    {"\\Device\\Harddisk0\\Partition1", saves_dir},     // E: user data (TDATA/UDATA)
};
// The utility drive Z: (Partition5, XMountUtilityDrive in hle.c) is virtual. On the Xbox the game copied the files it
// reads most (each room's backgrounds as it's entered, voices, the item table) from the DVD to this hard-disk cache,
// formatted clean at every boot, and read them back from there; it tries Z: first for others and falls back to D:.
// Here the disc is already on a hard disk: a copy only duplicated game\ (345 MB after some play, never cleared). So
// Z: names are D: names (a mod's copy first, the French/German swap), the copies' folders "already exist", and a
// file opened to write goes to the null device: the copy is thrown away and its later reads get the disc's file.
// No other partition (0, 2-4) is opened by this game.
#define UTILITY_DRIVE "\\Device\\Harddisk0\\Partition5"

// Mods that are on, highest priority first (mods_list.h). A file in mods\<name>\ is read instead of the same file in game\.
static struct mod_item mods[MOD_MAX];
static int nmods;

int mod_file(const wchar_t *rel, wchar_t *out, size_t n)   // a randomizer run's room files first, then the mods' (overlay.h)
{
    return overlay_path(host_dir(), rando_on() ? settings.rando_run : 0, mods, nmods, rel, out, n, 0) != 0;
}

// onimusha.ini text_language = french|german. The USA disc still has the European release's French (F) and German (G)
// copies of the text files: title menu, documents, puzzles, map and status pictures, the font. Their words are drawn
// into the images, so they are the same kind of file as the English ones. The game stays in its English mode (its
// own French/German code is half removed from this executable; the message text is switched in hle.c) and each
// English file it opens is swapped for its F/G copy when the disc has one: E_TITLE.LDT -> F_TITLE.LDT,
// FILE03E.LDT -> FILE03F.LDT, the options screen OPTIONBG -> F_OPBG, the font MOJIST_E -> MOJIST_P (the English one
// plus accented letters). The font and STATUS_E need fixing first (fixed_copy).
static void text_language(char *name, size_t n)
{
    if (settings.text_lang < LANG_FRENCH) return;
    char *base = strrchr(name, '\\'), *img = name, alt[64], L = settings.text_lang == LANG_FRENCH ? 'F' : 'G';
    while (*img && _strnicmp(img, "\\image\\", 7)) img++;
    if (!base || !*img || strlen(++base) >= sizeof alt - 2) return;
    size_t len = strlen(base);
    const char *dot = strchr(base, '.');
    snprintf(alt, sizeof alt, "%s", base);
    if (!_stricmp(base, "STATUS_E.LDT")) return;   // fixed_copy
    if (!_stricmp(base, "MOJIST_E.LDT")) alt[7] = 'P';
    else if (!_stricmp(base, "OPTIONBG.BG")) snprintf(alt, sizeof alt, "%c_OPBG.BG", L);
    else if (!_stricmp(base, "RDSV_BG1.BG")) snprintf(alt, sizeof alt, "%c_RDSV_BG1.BG", L);
    else if (!_strnicmp(base, "E_", 2)) alt[0] = L;
    else if (!_strnicmp(base, "FILE", 4) && len > 6 && (base[6] == 'E' || base[6] == 'e')) alt[6] = L;
    else if (dot && dot - base >= 2 && dot[-2] == '_' && (dot[-1] == 'E' || dot[-1] == 'e')) alt[dot - base - 1] = L;
    else return;
    wchar_t disc[MAX_PATH];   // only when the disc has that file
    swprintf(disc, MAX_PATH, L"%s\\game%.*hs%hs", host_dir(), (int)(base - img), img, alt);
    if (GetFileAttributesW(disc) == INVALID_FILE_ATTRIBUTES) return;
    TRACE("[lang] %s -> %s", base, alt);
    snprintf(base, n - (base - name), "%s", alt);
}

// Two French/German files need fixing before the USA code can use them; the fixed copies are written to cache\.
static size_t read_etc(const char *file, uint8_t *b, size_t max)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\game\\image\\etc\\%hs", host_dir(), file);
    FILE *f = _wfopen(path, L"rb");
    size_t n = f ? fread(b, 1, max, f) : 0;
    if (f && fgetc(f) != EOF) n = 0;   // bigger than the buffer: not the file we know
    if (f) fclose(f);
    return n;
}
static int write_hdd(wchar_t *path, const char *file, const uint8_t *b, size_t n)
{
    swprintf(path, MAX_PATH, L"%s\\cache", host_dir());
    CreateDirectoryW(path, 0);
    swprintf(path, MAX_PATH, L"%s\\cache\\%hs", host_dir(), file);
    FILE *f = _wfopen(path, L"wb");
    int ok = f && fwrite(b, 1, n, f) == n;
    if (f) fclose(f);
    xlog("[lang] %s %ls", ok ? "wrote" : "can't write", path);
    return ok;
}

// The font: MOJIST_P keeps its accented letters (codes 0x82-0xb0, A-umlaut to u-circumflex) in 20-pixel rows from
// y = 176, but this executable's text drawing only knows the 16-pixel grid: code c is the cell at column c & 15, row
// (c >> 4) + 2, where the accents' codes find "LvMAX" and empty cells. The copy has each accented letter moved to the
// cell its code reads: the letter keeps its 14 rows and its accent is squeezed into the 2 rows above it, so it sits
// 2 pixels lower than plain letters. ("LvMAX" goes: in French and German those codes are the accents.) Widths: hle.c.
static wchar_t font_path[MAX_PATH];
static int european_font(void)
{
    static int ok = -1;
    static uint8_t b[0x20000], old[0x8000];
    if (ok >= 0) return ok;
    size_t n = read_etc("MOJIST_P.LDT", b, sizeof b);
    uint32_t off = *(uint32_t *)(b + 12);   // first entry {flag, offset, size}: the 256x256 4-bit glyph sheet
    if ((ok = 0), n < 0x10000 || memcmp(b, "LINK", 4) || off > n - 0x8100 || memcmp(b + off, "OIM ", 4) ||
        b[off + 0x10] != 8 || b[off + 0x11] != 8 || b[off + 0x12] != 0x14 ||
        *(uint32_t *)(b + off + 0x14) > n - off - 0x8000) { xlog("[lang] MOJIST_P.LDT isn't the expected font"); return 0; }
    uint8_t *px = b + off + *(uint32_t *)(b + off + 0x14);   // 2 pixels a byte, left one in the low nibble
    memcpy(old, px, sizeof old);
    for (int y = 160; y < 240; y++) memset(px + y * 128 + (y < 176 ? 16 : 0), 0, y < 176 ? 112 : 128);   // keep the infinity sign (0x80, 0x81)
    static const uint8_t rows[16] = {0, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};   // of the 20-pixel row
    for (int i = 0; i <= 0xb0 - 0x82; i++) {
        int sx = i % 16 * 16, sy = 176 + i / 16 * 20, cell = 0xa2 + i, dx = cell % 16 * 16, dy = cell / 16 * 16;
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 16; c++) {
                int v = old[(sy + rows[r]) * 128 + (sx + c) / 2] >> (c & 1) * 4 & 15;
                uint8_t *p = &px[(dy + r) * 128 + (dx + c) / 2];
                *p = c & 1 ? (*p & 0x0f) | v << 4 : (*p & 0xf0) | v;
            }
    }
    return ok = write_hdd(font_path, "MOJIST_FG.LDT", b, n);
}

// The status screen: the USA one draws pieces that only its English sheet has (the frame beside the weapon, an orb;
// the F/G sheets show purple there). Only sheet 1 has words (the EQUIP/ITEM/FILE/MAP/EXIT tabs) and the 8-bit
// palettes are the same, so the copy is STATUS_E.LDT with that sheet from STATUS_F/G. "TOTAL:" stays English.
static wchar_t status_path[MAX_PATH];
static int european_status(void)
{
    static int ok = -1;
    static uint8_t e[0x30000], l[0x30000];
    if (ok >= 0) return ok;
    static const uint8_t sheet1[] = {8, 8, 0x13, 0, 0x60, 0, 1, 0};   // OIM image record: 256x256 8-bit at +0x10060
    size_t n = read_etc("STATUS_E.LDT", e, sizeof e), m = read_etc(settings.text_lang == LANG_FRENCH ? "STATUS_F.LDT" : "STATUS_G.LDT", l, sizeof l);
    if ((ok = 0), n < 0x20080 || m < 0x20080 || memcmp(e + 0x20, "OIM ", 4) || memcmp(l + 0x20, "OIM ", 4) ||
        memcmp(e + 0x40, sheet1, 8) || memcmp(l + 0x40, sheet1, 8)) { xlog("[lang] STATUS files aren't the expected ones"); return 0; }
    memcpy(e + 0x20 + 0x10060, l + 0x20 + 0x10060, 0x10000);
    return ok = write_hdd(status_path, "STATUS_FG.LDT", e, n);
}

// Xbox name (after text_language) -> the fixed copy to read instead, or 0.
static const wchar_t *fixed_copy(const char *name)
{
    static SRWLOCK lock = SRWLOCK_INIT;   // files are opened from several threads: build each copy once
    size_t n = strlen(name);
    const wchar_t *r = 0;
    if (settings.text_lang < LANG_FRENCH || n <= 13) return 0;
    AcquireSRWLockExclusive(&lock);
    if (!_stricmp(name + n - 13, "\\MOJIST_P.LDT")) r = european_font() ? font_path : 0;
    else if (!_stricmp(name + n - 13, "\\STATUS_E.LDT")) r = european_status() ? status_path : 0;
    ReleaseSRWLockExclusive(&lock);
    return r;
}

// Xbox name -> NT path ("\??\C:\...\game\image\x.wd"). Returns 0 if the name isn't something we map.
// utility: set to 1 for a name on the virtual Z: drive (mapped to the disc), else 0.
static int translate_path(const XSTR *xname, wchar_t *out, size_t outn, int *utility)
{
    char name[512], tmp[512];
    int bare_drive = xname->Length >= 2 && xname->Buffer[1] == ':';   // "d:\image\..." as well as "\??\D:\..."
    snprintf(name, sizeof name, "%s%.*s", bare_drive ? "\\??\\" : "", xname->Length, xname->Buffer);
    text_language(name, sizeof name);
    for (int pass = 0; pass < 4; pass++) {   // resolve \??\X: links (possibly to another link)
        int hit = 0;
        AcquireSRWLockShared(&link_lock);
        for (int i = 0; i < nlinks && !hit; i++) {
            size_t n = strlen(links[i].name);
            if (!_strnicmp(name, links[i].name, n) && (name[n] == 0 || name[n] == '\\')) {
                snprintf(tmp, sizeof tmp, "%s%s", links[i].target, name + n);
                strcpy(name, tmp);
                hit = 1;
            }
        }
        ReleaseSRWLockShared(&link_lock);
        if (!hit) break;
    }
    size_t un = strlen(UTILITY_DRIVE);
    if (utility) *utility = 0;
    if (!_strnicmp(name, UTILITY_DRIVE, un) && (name[un] == 0 || name[un] == '\\')) {
        snprintf(tmp, sizeof tmp, "\\Device\\CdRom0%s", name + un);
        strcpy(name, tmp);
        if (utility) *utility = 1;
    }
    for (size_t i = 0; i < sizeof devices / sizeof *devices; i++) {
        size_t n = strlen(devices[i].dev);
        if (!_strnicmp(name, devices[i].dev, n) && (name[n] == 0 || name[n] == '\\')) {
            if (i == 0 && name[n]) {   // the disc: a randomizer run's copy first, then a mod's (overlay.h, as mod_file)
                wchar_t rel[512], found[MAX_PATH * 2];
                int m = 0;
                swprintf(rel, 512, L"%hs", name + n + 1);
                int layer = overlay_path(host_dir(), rando_on() ? settings.rando_run : 0, mods, nmods, rel, found, MAX_PATH * 2, &m);
                if (layer) {
                    if (layer == 1) xlog("[mods] %s from %ls", name + n + 1, mods[m].name);
                    swprintf(out, outn, L"\\??\\%s", found);
                    return 1;
                }
            }
            const wchar_t *fixed = i == 0 ? fixed_copy(name) : 0;
            // MOJIST_P can't be used as it is (its accent rows are in a layout this executable can't read: garbled
            // letters and "LvMAX"), so when its fixed copy can't be made, the English font is read instead.
            size_t nl = strlen(name);
            if (!fixed && i == 0 && nl > 13 && !_stricmp(name + nl - 13, "\\MOJIST_P.LDT")) {
                name[nl - 5] = 'E';
                xlog("[lang] no fixed MOJIST_P: the English font instead (accented letters won't show)");
            }
            if (fixed)
                swprintf(out, outn, L"\\??\\%s", fixed);
            else
                swprintf(out, outn, L"\\??\\%s\\%s%hs", host_dir(), devices[i].dir, name + n);
            return 1;
        }
    }
    return 0;
}

static LONG open_file(HANDLE *h, ULONG access, XOA *xoa, NT_IOSB *iosb, LARGE_INTEGER *alloc, ULONG attrs, ULONG share, ULONG disp, ULONG opts)
{
    wchar_t path[MAX_PATH * 2];
    int utility = 0;
    NT_USTR us;
    NT_OA oa = {sizeof oa, 0, &us, 0x40 /* OBJ_CASE_INSENSITIVE */};
    if (!xoa->ObjectName) { TRACE("open: no name"); return (LONG)STATUS_INVALID_PARAMETER; }
    // (HANDLE)-3, a special Xbox root, is taken as no root: translate_path also reads bare "d:\..." names.
    if (xoa->RootDirectory && xoa->RootDirectory != (HANDLE)-3) {   // relative open: pass through
        swprintf(path, MAX_PATH * 2, L"%.*hs", xoa->ObjectName->Length, xoa->ObjectName->Buffer);
        oa.RootDirectory = xoa->RootDirectory;
    } else if (!translate_path(xoa->ObjectName, path, MAX_PATH * 2, &utility)) {
        TRACE("open: no mapping for '%.*s'", xoa->ObjectName->Length, xoa->ObjectName->Buffer);
        return (LONG)STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (utility && (opts & 1 /* FILE_DIRECTORY_FILE */) && disp != 1 /* FILE_OPEN */)
        return (LONG)STATUS_OBJECT_NAME_COLLISION;   // making the cache's folders: they exist (they're the disc's)
    if (utility && (disp != 1 || (access & WRITE_ACCESS))) {   // a copy being written: thrown away
        TRACE("open '%.*s' for the cache: discarded", xoa->ObjectName->Length, xoa->ObjectName->Buffer);
        wcscpy(path, L"\\Device\\Null");
        disp = 1;
    }
    us.Buffer = path;
    us.Length = us.MaximumLength = (USHORT)(wcslen(path) * 2);
    // The Xbox kernel is lax about handle rights; NT isn't. Always allow attribute queries and waiting,
    // and attribute writes on writable handles.
    access |= FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    if (access & WRITE_ACCESS) access |= FILE_WRITE_ATTRIBUTES;
    LONG st = NtCreateFile(h, access, &oa, iosb, alloc, attrs, share, disp, opts, 0, 0);
    TRACE("open '%.*s' -> %ls : %08lx", xoa->ObjectName->Length, xoa->ObjectName->Buffer, path, st);
    size_t n = wcslen(path);
    const wchar_t *base = wcsrchr(path, L'\\');
    // HD movie in a mod (movie.c): movies\<name>.mp4 or movies\<name>_<anything>.mp4 (another cut of it, e.g. a
    // mod's uncut .bik); movie_open takes only one with as many frames as the .bik the game opened.
    if (st >= 0 && n > 4 && n < MAX_PATH && !_wcsicmp(path + n - 4, L".bik") && base) {
        movie_bik(path + 4, *h);
        wchar_t f[MAX_PATH];
        swprintf(f, MAX_PATH, L"%.*s.wav", (int)(n - 8), path + 4);   // its sound as a .wav beside it (dsound.c)
        if (layout_exists(f)) movie_sound_open(f, *h);
        // subtitles (movie.c): <name>.<lang>.srt beside it (a mod's own cut of it), else in a mod's subtitles folder
        const wchar_t *lang = settings.movie_subs == SUBS_JAPANESE ? L"ja" : settings.text_lang == LANG_FRENCH ? L"fr"
                            : settings.text_lang == LANG_GERMAN ? L"de" : L"en";
        swprintf(f, MAX_PATH, L"%.*s.%s.srt", (int)(n - 8), path + 4, lang);
        for (int m = 0; settings.movie_subs != SUBS_OFF && m <= nmods; m++) {
            if (m) swprintf(f, MAX_PATH, L"%s\\mods\\%s\\subtitles%.*s.%s.srt", host_dir(), mods[m - 1].name, (int)(path + n - 4 - base), base, lang);
            if (layout_exists(f)) { movie_subs(f); break; }
        }
    }
    // a room's backgrounds (d3d_pack.c); opened to read (the game also copies them to its Z: cache)
    if (st >= 0 && n > 4 && !_wcsicmp(path + n - 4, L".bgs") && disp == 1 && !(access & WRITE_ACCESS)) bg_prefetch(path + 4);
    if (st >= 0 && n > 4 && (!_wcsicmp(path + n - 4, L".bgs") || !_wcsicmp(path + n - 4, L".rdt")) && disp == 1) room_open(path + 4);
    // an animated background's frame sheets (xmovNNN*.xpr, opened as the room is entered): read ahead too
    if (st >= 0 && n > 4 && base && !_wcsicmp(path + n - 4, L".xpr") && !_wcsnicmp(base + 1, L"xmov", 4) && disp == 1
        && !(access & WRITE_ACCESS)) xmov_prefetch(path + 4);
    // An HD movie: movies\<name>.mp4 (or <name>_*.mp4) in an enabled mod. For an English-voice cut (<name>e.bik) with
    // no file of its own, the other language's <name> is tried next: most pairs show the same picture (only the voice
    // differs), so the pack ships one file for both. movie_open checks the frame count, so a different cut isn't used.
    int found = 0, len = base ? (int)(path + n - 4 - base) : 0;
    for (int e = 0; e < 2 && !found; e++)
    for (int m = 0; st >= 0 && n > 4 && !_wcsicmp(path + n - 4, L".bik") && base && m < nmods && !found; m++)
        for (int k = 0; k < 2 && !found; k++) {
            wchar_t pat[MAX_PATH], hd[MAX_PATH];
            WIN32_FIND_DATAW d;
            if (e && (len < 3 || (base[len - 1] | 0x20) != L'e')) break;
            swprintf(pat, MAX_PATH, L"%s\\mods\\%s\\movies%.*s%s", host_dir(), mods[m].name, len - e, base, k ? L"_*.mp4" : L".mp4");
            HANDLE f = FindFirstFileW(pat, &d);
            if (f == INVALID_HANDLE_VALUE) continue;
            do {
                swprintf(hd, MAX_PATH, L"%s\\mods\\%s\\movies\\%s", host_dir(), mods[m].name, d.cFileName);
                found = movie_open(hd, *h);
            } while (!found && FindNextFileW(f, &d));
            FindClose(f);
        }
    return st;
}

volatile LONGLONG stall_io_ticks;
volatile DWORD stall_thread;
static LONGLONG io_start(void) { LARGE_INTEGER v; if (GetCurrentThreadId() != stall_thread) return 0; QueryPerformanceCounter(&v); return v.QuadPart; }
static void io_end(LONGLONG t0) { LARGE_INTEGER v; if (!t0) return; QueryPerformanceCounter(&v); stall_io_ticks += v.QuadPart - t0; }
// 190: NtCreateFile(OUT PHANDLE, IN ACCESS_MASK, IN POBJECT_ATTRIBUTES, OUT PIO_STATUS_BLOCK, IN PLARGE_INTEGER AllocationSize, IN ULONG FileAttributes, IN ULONG ShareAccess, IN ULONG CreateDisposition, IN ULONG CreateOptions)
static uint32_t __stdcall k_NtCreateFile(uint32_t FileHandle, uint32_t DesiredAccess, uint32_t ObjectAttributes, uint32_t IoStatusBlock,
    uint32_t AllocationSize, uint32_t FileAttributes, uint32_t ShareAccess, uint32_t CreateDisposition, uint32_t CreateOptions)
{
    LONGLONG t0 = io_start();
    uint32_t st = open_file((HANDLE *)FileHandle, DesiredAccess, (XOA *)ObjectAttributes, (NT_IOSB *)IoStatusBlock,
                            (LARGE_INTEGER *)AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions);
    io_end(t0);
    return st;
}
// 202: NtOpenFile(OUT PHANDLE, IN ACCESS_MASK, IN POBJECT_ATTRIBUTES, OUT PIO_STATUS_BLOCK, IN ULONG ShareAccess, IN ULONG OpenOptions)
static uint32_t __stdcall k_NtOpenFile(uint32_t FileHandle, uint32_t DesiredAccess, uint32_t ObjectAttributes, uint32_t IoStatusBlock, uint32_t ShareAccess, uint32_t OpenOptions)
{
    LONGLONG t0 = io_start();
    uint32_t st = open_file((HANDLE *)FileHandle, DesiredAccess, (XOA *)ObjectAttributes, (NT_IOSB *)IoStatusBlock, 0, 0, ShareAccess, 1 /* FILE_OPEN */, OpenOptions);
    io_end(t0);
    return st;
}
// Returns call's status, logging it in one line if it failed (end of file is normal and not logged).
#define RETURN_LOGGED(call, fmt, ...) do { LONG st_ = (call); if (st_ < 0 && st_ != (LONG)STATUS_END_OF_FILE) TRACE(fmt " -> %08lx", __VA_ARGS__, st_); return st_; } while (0)
static uint32_t __stdcall k_NtReadFile(uint32_t FileHandle, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext, uint32_t IoStatusBlock, uint32_t Buffer, uint32_t Length, uint32_t ByteOffset)
{
    LONGLONG t0 = io_start();
    LONG st = NtReadFile((HANDLE)FileHandle, (HANDLE)Event, (PVOID)ApcRoutine, (PVOID)ApcContext, (NT_IOSB *)IoStatusBlock, (PVOID)Buffer, Length, (LARGE_INTEGER *)ByteOffset, 0);
    io_end(t0);
    static int traced;   // debug: who asked for the first failing read (e.g. a nonsense length)
    if (st < 0 && st != (LONG)STATUS_END_OF_FILE && !traced++) xbacktrace("first failing NtReadFile");
    RETURN_LOGGED(st, "NtReadFile(%x, buf %x, len %x)", FileHandle, Buffer, Length);
}
static uint32_t __stdcall k_NtWriteFile(uint32_t FileHandle, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext, uint32_t IoStatusBlock, uint32_t Buffer, uint32_t Length, uint32_t ByteOffset)
{ RETURN_LOGGED(NtWriteFile((HANDLE)FileHandle, (HANDLE)Event, (PVOID)ApcRoutine, (PVOID)ApcContext, (NT_IOSB *)IoStatusBlock, (PVOID)Buffer, Length, (LARGE_INTEGER *)ByteOffset, 0), "NtWriteFile(%x, len %x)", FileHandle, Length); }
static uint32_t __stdcall k_NtFlushBuffersFile(uint32_t FileHandle, uint32_t IoStatusBlock) { return NtFlushBuffersFile((HANDLE)FileHandle, (NT_IOSB *)IoStatusBlock); }

// Info classes whose structures carry no names are identical on Xbox and NT.
static int nameless_class(uint32_t c) { return c == 4 || c == 5 || c == 6 || c == 7 || c == 8 || c == 13 || c == 14 || c == 16 || c == 19 || c == 20 || c == 34; }

static uint32_t __stdcall k_NtQueryInformationFile(uint32_t FileHandle, uint32_t IoStatusBlock, uint32_t FileInformation, uint32_t Length, uint32_t FileInformationClass)
{
    if (!nameless_class(FileInformationClass)) { UNIMPL_CLASS(FileInformationClass); return STATUS_NOT_IMPLEMENTED; }
    RETURN_LOGGED(NtQueryInformationFile((HANDLE)FileHandle, (NT_IOSB *)IoStatusBlock, (PVOID)FileInformation, Length, FileInformationClass), "NtQueryInformationFile(%x, class %u)", FileHandle, FileInformationClass);
}
static uint32_t __stdcall k_NtSetInformationFile(uint32_t FileHandle, uint32_t IoStatusBlock, uint32_t FileInformation, uint32_t Length, uint32_t FileInformationClass)
{
    if (!nameless_class(FileInformationClass)) { UNIMPL_CLASS(FileInformationClass); return STATUS_NOT_IMPLEMENTED; }
    RETURN_LOGGED(NtSetInformationFile((HANDLE)FileHandle, (NT_IOSB *)IoStatusBlock, (PVOID)FileInformation, Length, FileInformationClass), "NtSetInformationFile(%x, class %u)", FileHandle, FileInformationClass);
}

// 218: NtQueryVolumeInformationFile. Size classes (3, 7) match NT; volume info (1) carries an ANSI label on Xbox.
static uint32_t __stdcall k_NtQueryVolumeInformationFile(uint32_t FileHandle, uint32_t IoStatusBlock, uint32_t FsInformation, uint32_t Length, uint32_t FsInformationClass)
{
    if (FsInformationClass == 3 || FsInformationClass == 7)
        return NtQueryVolumeInformationFile((HANDLE)FileHandle, (NT_IOSB *)IoStatusBlock, (PVOID)FsInformation, Length, FsInformationClass);
    if (FsInformationClass == 1 && Length >= 17) {   // FILE_FS_VOLUME_INFORMATION, empty label
        memset((void *)FsInformation, 0, 17);
        ((NT_IOSB *)IoStatusBlock)->Status = 0;
        ((NT_IOSB *)IoStatusBlock)->Information = 17;
        return STATUS_SUCCESS;
    }
    UNIMPL_CLASS(FsInformationClass);
    return STATUS_NOT_IMPLEMENTED;
}

// 207: NtQueryDirectoryFile: NT returns WCHAR names, Xbox expects CHAR. Convert one entry at a time.
static uint32_t __stdcall k_NtQueryDirectoryFile(uint32_t FileHandle, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext,
    uint32_t IoStatusBlock, uint32_t FileInformation, uint32_t Length, uint32_t FileInformationClass, uint32_t FileName, uint32_t RestartScan)
{
    if (FileInformationClass != 1) { UNIMPL_CLASS(FileInformationClass); return STATUS_NOT_IMPLEMENTED; }
    if (Length < 0x40) return STATUS_BUFFER_TOO_SMALL;   // checked first: the query below uses up the entry
    wchar_t mask[260];
    NT_USTR us = {0}, *pus = 0;
    if (FileName && ((XSTR *)FileName)->Length) {
        XSTR *x = (XSTR *)FileName;
        swprintf(mask, 260, L"%.*hs", x->Length, x->Buffer);
        us.Buffer = mask; us.Length = us.MaximumLength = (USHORT)(wcslen(mask) * 2); pus = &us;
    }
    uint8_t nt[0x40 + 520];
    LONG st = NtQueryDirectoryFile((HANDLE)FileHandle, 0, 0, 0, (NT_IOSB *)IoStatusBlock, nt, sizeof nt, 1, TRUE, pus, (BOOLEAN)RestartScan);
    if (st < 0) return st;   // includes STATUS_NO_MORE_FILES (0x80000006)
    uint32_t wlen = *(uint32_t *)(nt + 0x3C) / 2, fit = wlen < Length - 0x40 ? wlen : Length - 0x40;
    uint8_t *x = (uint8_t *)FileInformation;
    memcpy(x, nt, 0x40);
    *(uint32_t *)x = 0;                                   // NextEntryOffset: one entry per call
    *(uint32_t *)(x + 0x3C) = wlen;
    for (uint32_t i = 0; i < fit; i++) x[0x40 + i] = (uint8_t)((wchar_t *)(nt + 0x40))[i];
    ((NT_IOSB *)IoStatusBlock)->Information = 0x40 + fit;
    return fit < wlen ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;   // name cut to fit
}

// Symbolic links (drive letters)
static uint32_t __stdcall k_IoCreateSymbolicLink(uint32_t SymbolicLinkName, uint32_t DeviceName)
{
    XSTR *l = (XSTR *)SymbolicLinkName, *d = (XSTR *)DeviceName;
    char a[64], b[192];
    snprintf(a, sizeof a, "%.*s", l->Length, l->Buffer);
    snprintf(b, sizeof b, "%.*s", d->Length, d->Buffer);
    TRACE("IoCreateSymbolicLink %s -> %s", a, b);
    add_link(a, b);
    // make sure the backing folder exists, e.g. saves\TDATA\43430001
    wchar_t path[MAX_PATH * 2];
    if (translate_path(d, path, MAX_PATH * 2, 0)) {
        for (wchar_t *p = path + 4; *p; p++)
            if (*p == L'\\') { *p = 0; CreateDirectoryW(path + 4, 0); *p = L'\\'; }
        CreateDirectoryW(path + 4, 0);
    }
    return STATUS_SUCCESS;
}
static uint32_t __stdcall k_IoDeleteSymbolicLink(uint32_t SymbolicLinkName) { return STATUS_SUCCESS; }
static uint32_t __stdcall k_IoDismountVolumeByName(uint32_t DeviceName) { return STATUS_SUCCESS; }

void kernel_add_link(const char *name, const char *target) { add_link(name, target); }

void kernel_init(const wchar_t *root)
{
    if (rando_on()) {
        swprintf(saves_dir, 96, LAYOUT_RUNS L"\\%hs\\saves", settings.rando_run);
        wchar_t p[MAX_PATH];
        swprintf(p, MAX_PATH, L"%s\\%s", root, saves_dir);
        CreateDirectoryW(p, 0);   // the run folder itself is made by the launcher
        xlog("[randomizer] run %s: saves in %ls", settings.rando_run, saves_dir);
    }
    int n;
    const struct mod_item *all = host_mods(&n);
    for (int i = 0; i < n; i++) {
        xlog("[mods] %ls: %s", all[i].name, all[i].on ? "on" : "off");
        if (all[i].on) mods[nmods++] = all[i];
    }
    tls_slot_kthread = TlsAlloc(); tls_slot_prcb = TlsAlloc(); tls_slot_irql = TlsAlloc(); tls_slot_anchor = TlsAlloc();
    if ((tls_slot_kthread | tls_slot_prcb | tls_slot_irql | tls_slot_anchor) >= 64)   // host.c uses fs:[0xE10 + 4 * slot]
        xfatal("TLS slots out of TEB range");
    add_link("\\??\\D:", "\\Device\\CdRom0");
    kv_KeTickCount[0] = GetTickCount();
    CloseHandle(CreateThread(0, 0, tick_thread, 0, 0, 0));
}

// ---- not implemented yet ------------------------------------------------------------------------------
// 2: VOID NTAPI AvSendTVEncoderOption(IN PVOID RegisterBase, IN ULONG Option, IN ULONG Param, OUT PULONG Result)
static void __stdcall k_AvSendTVEncoderOption(uint32_t RegisterBase, uint32_t Option, uint32_t Param, uint32_t Result)
{ UNIMPL("AvSendTVEncoderOption(%x, %x, %x, %x)", RegisterBase, Option, Param, Result); }

// 3: ULONG NTAPI AvSetDisplayMode(IN PVOID RegisterBase, IN ULONG Step, IN ULONG DisplayMode, IN ULONG SourceColorFormat, IN ULONG Pitch, IN ULONG FrameBuffer)
static uint32_t __stdcall k_AvSetDisplayMode(uint32_t RegisterBase, uint32_t Step, uint32_t DisplayMode, uint32_t SourceColorFormat, uint32_t Pitch, uint32_t FrameBuffer)
{ UNIMPL("AvSetDisplayMode(%x, %x, %x, %x, %x, %x)", RegisterBase, Step, DisplayMode, SourceColorFormat, Pitch, FrameBuffer); return 0; }

// 44: ULONG NTAPI HalGetInterruptVector(IN ULONG BusInterruptLevel, OUT PKIRQL Irql)
static uint32_t __stdcall k_HalGetInterruptVector(uint32_t BusInterruptLevel, uint32_t Irql)
{ UNIMPL("HalGetInterruptVector(%x, %x)", BusInterruptLevel, Irql); return 0; }

// 65: NTSTATUS NTAPI IoCreateDevice(IN PDRIVER_OBJECT DriverObject, IN ULONG DeviceExtensionSize, IN POBJECT_STRING DeviceName OPTIONAL, IN DEVICE_TYPE DeviceType, IN BOOLEAN Exclusive, OUT PDEVICE_OBJECT *DeviceObject)
static uint32_t __stdcall k_IoCreateDevice(uint32_t DriverObject, uint32_t DeviceExtensionSize, uint32_t DeviceName, uint32_t DeviceType, uint32_t Exclusive, uint32_t DeviceObject)
{ UNIMPL("IoCreateDevice(%x, %x, %x, %x, %x, %x)", DriverObject, DeviceExtensionSize, DeviceName, DeviceType, Exclusive, DeviceObject); return STATUS_NOT_IMPLEMENTED; }

// 74: NTSTATUS NTAPI IoInvalidDeviceRequest(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
static uint32_t __stdcall k_IoInvalidDeviceRequest(uint32_t DeviceObject, uint32_t Irp)
{ UNIMPL("IoInvalidDeviceRequest(%x, %x)", DeviceObject, Irp); return STATUS_NOT_IMPLEMENTED; }

// 81: VOID NTAPI IoStartNextPacket(IN PDEVICE_OBJECT DeviceObject)
static void __stdcall k_IoStartNextPacket(uint32_t DeviceObject)
{ UNIMPL("IoStartNextPacket(%x)", DeviceObject); }

// 83: VOID NTAPI IoStartPacket(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp, IN PULONG Key OPTIONAL)
static void __stdcall k_IoStartPacket(uint32_t DeviceObject, uint32_t Irp, uint32_t Key)
{ UNIMPL("IoStartPacket(%x, %x, %x)", DeviceObject, Irp, Key); }

// 87: VOID FASTCALL IofCompleteRequest(IN PIRP Irp, IN CCHAR PriorityBoost)
static void __fastcall k_IofCompleteRequest(uint32_t Irp, uint32_t PriorityBoost)
{ UNIMPL("IofCompleteRequest(%x, %x)", Irp, PriorityBoost); }

// 98: BOOLEAN NTAPI KeConnectInterrupt(IN PKINTERRUPT Interrupt)
static uint32_t __stdcall k_KeConnectInterrupt(uint32_t Interrupt)
{ UNIMPL("KeConnectInterrupt(%x)", Interrupt); return 0; }

// 100: BOOLEAN NTAPI KeDisconnectInterrupt(IN PKINTERRUPT Interrupt)
static uint32_t __stdcall k_KeDisconnectInterrupt(uint32_t Interrupt)
{ UNIMPL("KeDisconnectInterrupt(%x)", Interrupt); return 0; }

// 109: VOID NTAPI KeInitializeInterrupt(IN PKINTERRUPT Interrupt, IN PKSERVICE_ROUTINE ServiceRoutine, IN PVOID ServiceContext, IN ULONG Vector, IN KIRQL Irql, IN KINTERRUPT_MODE InterruptMode, IN BOOLEAN ShareVector)
static void __stdcall k_KeInitializeInterrupt(uint32_t Interrupt, uint32_t ServiceRoutine, uint32_t ServiceContext, uint32_t Vector, uint32_t Irql, uint32_t InterruptMode, uint32_t ShareVector)
{ UNIMPL("KeInitializeInterrupt(%x, %x, %x, %x, %x, %x, %x)", Interrupt, ServiceRoutine, ServiceContext, Vector, Irql, InterruptMode, ShareVector); }

// 119: BOOLEAN NTAPI KeInsertQueueDpc(IN PRKDPC Dpc, IN PVOID SystemArgument1, IN PVOID SystemArgument2)
static uint32_t __stdcall k_KeInsertQueueDpc(uint32_t Dpc, uint32_t SystemArgument1, uint32_t SystemArgument2)
{ UNIMPL("KeInsertQueueDpc(%x, %x, %x)", Dpc, SystemArgument1, SystemArgument2); return 0; }

// 137: BOOLEAN NTAPI KeRemoveQueueDpc(IN PRKDPC Dpc)
static uint32_t __stdcall k_KeRemoveQueueDpc(uint32_t Dpc)
{ UNIMPL("KeRemoveQueueDpc(%x)", Dpc); return 0; }

// 139: NTSTATUS NTAPI KeRestoreFloatingPointState(IN PKFLOATING_SAVE FloatSave)
static uint32_t __stdcall k_KeRestoreFloatingPointState(uint32_t FloatSave)
{ UNIMPL("KeRestoreFloatingPointState(%x)", FloatSave); return STATUS_NOT_IMPLEMENTED; }

// 142: NTSTATUS NTAPI KeSaveFloatingPointState(OUT PKFLOATING_SAVE FloatSave)
static uint32_t __stdcall k_KeSaveFloatingPointState(uint32_t FloatSave)
{ UNIMPL("KeSaveFloatingPointState(%x)", FloatSave); return STATUS_NOT_IMPLEMENTED; }

// 149: BOOLEAN NTAPI KeSetTimer(IN PKTIMER Timer, IN LARGE_INTEGER DueTime, IN PKDPC Dpc OPTIONAL)
static uint32_t __stdcall k_KeSetTimer(uint32_t Timer, int64_t DueTime, uint32_t Dpc)
{ UNIMPL("KeSetTimer(%x, %llx, %x)", Timer, DueTime, Dpc); return 0; }

// 153: BOOLEAN NTAPI KeSynchronizeExecution(IN PKINTERRUPT Interrupt, IN PKSYNCHRONIZE_ROUTINE SynchronizeRoutine, IN PVOID SynchronizeContext)
static uint32_t __stdcall k_KeSynchronizeExecution(uint32_t Interrupt, uint32_t SynchronizeRoutine, uint32_t SynchronizeContext)
{ UNIMPL("KeSynchronizeExecution(%x, %x, %x)", Interrupt, SynchronizeRoutine, SynchronizeContext); return 0; }

// 168: PVOID NTAPI MmClaimGpuInstanceMemory(IN SIZE_T NumberOfBytes, OUT SIZE_T *NumberOfPaddingBytes)
static uint32_t __stdcall k_MmClaimGpuInstanceMemory(uint32_t NumberOfBytes, uint32_t NumberOfPaddingBytes)
{ UNIMPL("MmClaimGpuInstanceMemory(%x, %x)", NumberOfBytes, NumberOfPaddingBytes); return 0; }

// 196: NTSTATUS NTAPI NtDeviceIoControlFile(IN HANDLE FileHandle, IN HANDLE Event OPTIONAL, IN PIO_APC_ROUTINE ApcRoutine OPTIONAL, IN PVOID ApcContext OPTIONAL, OUT PIO_STATUS_BLOCK IoStatusBlock, IN ULONG IoControlCode, IN PVOID InputBuffer OPTIONAL, IN ULONG InputBufferLength, OUT PVOID OutputBuffer OPTIONAL, IN ULONG OutputBufferLength)
static uint32_t __stdcall k_NtDeviceIoControlFile(uint32_t FileHandle, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext, uint32_t IoStatusBlock, uint32_t IoControlCode, uint32_t InputBuffer, uint32_t InputBufferLength, uint32_t OutputBuffer, uint32_t OutputBufferLength)
{ UNIMPL("NtDeviceIoControlFile(%x, %x, %x, %x, %x, %x, %x, %x, %x, %x)", FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, IoControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength); return STATUS_NOT_IMPLEMENTED; }

// 200: NTSTATUS NTAPI NtFsControlFile(IN HANDLE FileHandle, IN HANDLE Event OPTIONAL, IN PIO_APC_ROUTINE ApcRoutine OPTIONAL, IN PVOID ApcContext OPTIONAL, OUT PIO_STATUS_BLOCK IoStatusBlock, IN ULONG FsControlCode, IN PVOID InputBuffer OPTIONAL, IN ULONG InputBufferLength, OUT PVOID OutputBuffer OPTIONAL, IN ULONG OutputBufferLength)
static uint32_t __stdcall k_NtFsControlFile(uint32_t FileHandle, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext, uint32_t IoStatusBlock, uint32_t FsControlCode, uint32_t InputBuffer, uint32_t InputBufferLength, uint32_t OutputBuffer, uint32_t OutputBufferLength)
{ UNIMPL("NtFsControlFile(%x, %x, %x, %x, %x, %x, %x, %x, %x, %x)", FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FsControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength); return STATUS_NOT_IMPLEMENTED; }

// 203: NTSTATUS NTAPI NtOpenSymbolicLinkObject(OUT PHANDLE LinkHandle, IN POBJECT_ATTRIBUTES ObjectAttributes)
static uint32_t __stdcall k_NtOpenSymbolicLinkObject(uint32_t LinkHandle, uint32_t ObjectAttributes)
{ UNIMPL("NtOpenSymbolicLinkObject(%x, %x)", LinkHandle, ObjectAttributes); return STATUS_NOT_IMPLEMENTED; }

// 215: NTSTATUS NTAPI NtQuerySymbolicLinkObject(IN HANDLE LinkHandle, IN OUT POBJECT_STRING LinkTarget, OUT PULONG ReturnedLength OPTIONAL)
static uint32_t __stdcall k_NtQuerySymbolicLinkObject(uint32_t LinkHandle, uint32_t LinkTarget, uint32_t ReturnedLength)
{ UNIMPL("NtQuerySymbolicLinkObject(%x, %x, %x)", LinkHandle, LinkTarget, ReturnedLength); return STATUS_NOT_IMPLEMENTED; }

// 359: VOID NTAPI IoMarkIrpMustComplete(IN OUT PIRP Irp)
static void __stdcall k_IoMarkIrpMustComplete(uint32_t Irp)
{ UNIMPL("IoMarkIrpMustComplete(%x)", Irp); }

const struct kernel_export kernel_exports[] = {
    {1, "AvGetSavedDataAddress", (void *)k_AvGetSavedDataAddress},
    {2, "AvSendTVEncoderOption", (void *)k_AvSendTVEncoderOption},
    {3, "AvSetDisplayMode", (void *)k_AvSetDisplayMode},
    {4, "AvSetSavedDataAddress", (void *)k_AvSetSavedDataAddress},
    {8, "DbgPrint", (void *)k_DbgPrint},
    {14, "ExAllocatePool", (void *)k_ExAllocatePool},
    {15, "ExAllocatePoolWithTag", (void *)k_ExAllocatePoolWithTag},
    {16, "ExEventObjectType", (void *)kv_ExEventObjectType},
    {17, "ExFreePool", (void *)k_ExFreePool},
    {23, "ExQueryPoolBlockSize", (void *)k_ExQueryPoolBlockSize},
    {24, "ExQueryNonVolatileSetting", (void *)k_ExQueryNonVolatileSetting},
    {37, "FscSetCacheSize", (void *)k_FscSetCacheSize},
    {40, "HalDiskCachePartitionCount", (void *)kv_HalDiskCachePartitionCount},
    {44, "HalGetInterruptVector", (void *)k_HalGetInterruptVector},
    {47, "HalRegisterShutdownNotification", (void *)k_HalRegisterShutdownNotification},
    {49, "HalReturnToFirmware", (void *)k_HalReturnToFirmware},
    {65, "IoCreateDevice", (void *)k_IoCreateDevice},
    {67, "IoCreateSymbolicLink", (void *)k_IoCreateSymbolicLink},
    {69, "IoDeleteSymbolicLink", (void *)k_IoDeleteSymbolicLink},
    {74, "IoInvalidDeviceRequest", (void *)k_IoInvalidDeviceRequest},
    {81, "IoStartNextPacket", (void *)k_IoStartNextPacket},
    {83, "IoStartPacket", (void *)k_IoStartPacket},
    {87, "IofCompleteRequest", (void *)k_IofCompleteRequest},
    {91, "IoDismountVolumeByName", (void *)k_IoDismountVolumeByName},
    {95, "KeBugCheck", (void *)k_KeBugCheck},
    {97, "KeCancelTimer", (void *)k_KeCancelTimer},
    {98, "KeConnectInterrupt", (void *)k_KeConnectInterrupt},
    {99, "KeDelayExecutionThread", (void *)k_KeDelayExecutionThread},
    {100, "KeDisconnectInterrupt", (void *)k_KeDisconnectInterrupt},
    {107, "KeInitializeDpc", (void *)k_KeInitializeDpc},
    {109, "KeInitializeInterrupt", (void *)k_KeInitializeInterrupt},
    {113, "KeInitializeTimerEx", (void *)k_KeInitializeTimerEx},
    {119, "KeInsertQueueDpc", (void *)k_KeInsertQueueDpc},
    {125, "KeQueryInterruptTime", (void *)k_KeQueryInterruptTime},
    {126, "KeQueryPerformanceCounter", (void *)k_KeQueryPerformanceCounter},
    {127, "KeQueryPerformanceFrequency", (void *)k_KeQueryPerformanceFrequency},
    {128, "KeQuerySystemTime", (void *)k_KeQuerySystemTime},
    {129, "KeRaiseIrqlToDpcLevel", (void *)k_KeRaiseIrqlToDpcLevel},
    {137, "KeRemoveQueueDpc", (void *)k_KeRemoveQueueDpc},
    {139, "KeRestoreFloatingPointState", (void *)k_KeRestoreFloatingPointState},
    {142, "KeSaveFloatingPointState", (void *)k_KeSaveFloatingPointState},
    {145, "KeSetEvent", (void *)k_KeSetEvent},
    {149, "KeSetTimer", (void *)k_KeSetTimer},
    {151, "KeStallExecutionProcessor", (void *)k_KeStallExecutionProcessor},
    {153, "KeSynchronizeExecution", (void *)k_KeSynchronizeExecution},
    {156, "KeTickCount", (void *)kv_KeTickCount},
    {159, "KeWaitForSingleObject", (void *)k_KeWaitForSingleObject},
    {160, "KfRaiseIrql", (void *)k_KfRaiseIrql},
    {161, "KfLowerIrql", (void *)k_KfLowerIrql},
    {164, "LaunchDataPage", (void *)kv_LaunchDataPage},
    {165, "MmAllocateContiguousMemory", (void *)k_MmAllocateContiguousMemory},
    {166, "MmAllocateContiguousMemoryEx", (void *)k_MmAllocateContiguousMemoryEx},
    {167, "MmAllocateSystemMemory", (void *)k_MmAllocateSystemMemory},
    {168, "MmClaimGpuInstanceMemory", (void *)k_MmClaimGpuInstanceMemory},
    {169, "MmCreateKernelStack", (void *)k_MmCreateKernelStack},
    {170, "MmDeleteKernelStack", (void *)k_MmDeleteKernelStack},
    {171, "MmFreeContiguousMemory", (void *)k_MmFreeContiguousMemory},
    {172, "MmFreeSystemMemory", (void *)k_MmFreeSystemMemory},
    {173, "MmGetPhysicalAddress", (void *)k_MmGetPhysicalAddress},
    {175, "MmLockUnlockBufferPages", (void *)k_MmLockUnlockBufferPages},
    {176, "MmLockUnlockPhysicalPage", (void *)k_MmLockUnlockPhysicalPage},
    {178, "MmPersistContiguousMemory", (void *)k_MmPersistContiguousMemory},
    {179, "MmQueryAddressProtect", (void *)k_MmQueryAddressProtect},
    {180, "MmQueryAllocationSize", (void *)k_MmQueryAllocationSize},
    {182, "MmSetAddressProtect", (void *)k_MmSetAddressProtect},
    {184, "NtAllocateVirtualMemory", (void *)k_NtAllocateVirtualMemory},
    {187, "NtClose", (void *)k_NtClose},
    {189, "NtCreateEvent", (void *)k_NtCreateEvent},
    {190, "NtCreateFile", (void *)k_NtCreateFile},
    {192, "NtCreateMutant", (void *)k_NtCreateMutant},
    {196, "NtDeviceIoControlFile", (void *)k_NtDeviceIoControlFile},
    {198, "NtFlushBuffersFile", (void *)k_NtFlushBuffersFile},
    {199, "NtFreeVirtualMemory", (void *)k_NtFreeVirtualMemory},
    {200, "NtFsControlFile", (void *)k_NtFsControlFile},
    {202, "NtOpenFile", (void *)k_NtOpenFile},
    {203, "NtOpenSymbolicLinkObject", (void *)k_NtOpenSymbolicLinkObject},
    {207, "NtQueryDirectoryFile", (void *)k_NtQueryDirectoryFile},
    {211, "NtQueryInformationFile", (void *)k_NtQueryInformationFile},
    {215, "NtQuerySymbolicLinkObject", (void *)k_NtQuerySymbolicLinkObject},
    {217, "NtQueryVirtualMemory", (void *)k_NtQueryVirtualMemory},
    {218, "NtQueryVolumeInformationFile", (void *)k_NtQueryVolumeInformationFile},
    {219, "NtReadFile", (void *)k_NtReadFile},
    {221, "NtReleaseMutant", (void *)k_NtReleaseMutant},
    {224, "NtResumeThread", (void *)k_NtResumeThread},
    {225, "NtSetEvent", (void *)k_NtSetEvent},
    {226, "NtSetInformationFile", (void *)k_NtSetInformationFile},
    {233, "NtWaitForSingleObject", (void *)k_NtWaitForSingleObject},
    {234, "NtWaitForSingleObjectEx", (void *)k_NtWaitForSingleObjectEx},
    {235, "NtWaitForMultipleObjectsEx", (void *)k_NtWaitForMultipleObjectsEx},
    {236, "NtWriteFile", (void *)k_NtWriteFile},
    {246, "ObReferenceObjectByHandle", (void *)k_ObReferenceObjectByHandle},
    {250, "ObfDereferenceObject", (void *)k_ObfDereferenceObject},
    {255, "PsCreateSystemThreadEx", (void *)k_PsCreateSystemThreadEx},
    {258, "PsTerminateSystemThread", (void *)k_PsTerminateSystemThread},
    {269, "RtlCompareMemoryUlong", (void *)k_RtlCompareMemoryUlong},
    {277, "RtlEnterCriticalSection", (void *)k_RtlEnterCriticalSection},
    {279, "RtlEqualString", (void *)k_RtlEqualString},
    {289, "RtlInitAnsiString", (void *)k_RtlInitAnsiString},
    {291, "RtlInitializeCriticalSection", (void *)k_RtlInitializeCriticalSection},
    {294, "RtlLeaveCriticalSection", (void *)k_RtlLeaveCriticalSection},
    {301, "RtlNtStatusToDosError", (void *)k_RtlNtStatusToDosError},
    {302, "RtlRaiseException", (void *)k_RtlRaiseException},
    {305, "RtlTimeToTimeFields", (void *)k_RtlTimeToTimeFields},
    {312, "RtlUnwind", (void *)k_RtlUnwind},
    {322, "XboxHardwareInfo", (void *)kv_XboxHardwareInfo},
    {323, "XboxHDKey", (void *)kv_XboxHDKey},
    {325, "XboxSignatureKey", (void *)kv_XboxSignatureKey},
    {327, "XeLoadSection", (void *)k_XeLoadSection},
    {328, "XeUnloadSection", (void *)k_XeUnloadSection},
    {335, "XcSHAInit", (void *)k_XcSHAInit},
    {336, "XcSHAUpdate", (void *)k_XcSHAUpdate},
    {337, "XcSHAFinal", (void *)k_XcSHAFinal},
    {356, "HalBootSMCVideoMode", (void *)kv_HalBootSMCVideoMode},
    {358, "HalIsResetOrShutdownPending", (void *)k_HalIsResetOrShutdownPending},
    {359, "IoMarkIrpMustComplete", (void *)k_IoMarkIrpMustComplete},
    {360, "HalInitiateShutdown", (void *)k_HalInitiateShutdown},
    {0},
};
