// Loads default.xbe into the address range onimusha.exe reserved, patches it, and runs it.
#include "host.h"
#include "mods_list.h"
#include "layout.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XBE_BASE 0x10000u
static const uint32_t ENTRY_KEY_RETAIL = 0xA8FC57AB, THUNK_KEY_RETAIL = 0x5B6D40B6;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static wchar_t host_root[MAX_PATH];   // the install folder (layout.h), set first thing in host_main
void host_path(wchar_t *out, const wchar_t *name) { swprintf(out, MAX_PATH, L"%s\\%s", host_root, name); }
const wchar_t *host_dir(void) { return host_root; }
void game_frame(void)
{
    if (ach_frame()) {
        if (rando_on()) rando_frame();   // a run's start items
        else mapping_frame();            // the map is of the normal game
    }
    cheats_frame();
    orb_items_frame();
    intro_frame();
}
const struct mod_item *host_mods(int *n)
{
    static struct mod_item all[MOD_MAX];   // static: MOD_MAX of them is 130 KB
    static int count = -1;
    if (count < 0) count = mod_list(host_root, all);   // start-up, on one thread
    *n = count;
    return all;
}
void game_file(wchar_t *out, const wchar_t *rel)
{
    if (mod_file(rel, out, MAX_PATH)) return;
    swprintf(out, MAX_PATH, L"%s\\game\\%s", host_root, rel);
}

uint8_t *read_file(const wchar_t *path, uint32_t max, uint32_t *size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD n = GetFileSize(f, 0), r = 0;
    uint8_t *b = n != INVALID_FILE_SIZE && n <= max ? malloc((size_t)n + 1) : 0;
    if (b && (!ReadFile(f, b, n, &r, 0) || r != n)) { free(b); b = 0; }
    CloseHandle(f);
    if (b) b[n] = 0, *size = n;
    return b;
}

// Lay the XBE out the way the Xbox loader does: headers at 0x10000, each section at its VA, bss zeroed.
static uint32_t map_xbe(const uint8_t *x, uint32_t xsize, uint8_t *lo, uint8_t *hi)
{
    if (xsize < 0x178 || rd32(x) != 0x48454258) xfatal("default.xbe: not an XBE");
    uint32_t base = rd32(x + 0x104), hdr_size = rd32(x + 0x108), img_size = rd32(x + 0x10C);
    if (base != XBE_BASE || (uint8_t *)base != lo || img_size > (uint32_t)(hi - lo))
        xfatal("default.xbe: base %08x size %x doesn't fit the reserved range %p-%p", base, img_size, lo, hi);
    uint32_t nsec = rd32(x + 0x11C), sh_at = rd32(x + 0x120) - base;
    if (hdr_size > xsize || hdr_size > img_size || sh_at > hdr_size || nsec > (hdr_size - sh_at) / 56)
        xfatal("default.xbe: corrupt headers");
    DWORD old;
    if (!VirtualProtect(lo, hi - lo, PAGE_EXECUTE_READWRITE, &old)) xfatal("VirtualProtect failed: %lu", GetLastError());
    memset(lo, 0, hi - lo);
    memcpy(lo, x, hdr_size);
    const uint8_t *sh = x + sh_at;
    for (uint32_t i = 0; i < nsec; i++, sh += 56) {
        uint32_t va = rd32(sh + 4), raddr = rd32(sh + 12), rsize = rd32(sh + 16);
        if (raddr > xsize || rsize > xsize - raddr || va < base || va - base > img_size || rsize > img_size - (va - base))
            xfatal("default.xbe: section %u is outside the file or the image", i);
        memcpy((uint8_t *)va, x + raddr, rsize);
    }
    xlog("mapped default.xbe: %u sections, image %08x-%08x", nsec, base, base + img_size);
    return rd32(x + 0x128) ^ ENTRY_KEY_RETAIL;
}

// Windows still reads the exe's PE header after startup (CreateThread fails with ERROR_BAD_EXE_FORMAT
// without it), but 0x10000 now holds the XBE header. Both fit in one page: 'MZ' over the first two bytes of
// 'XBEH' and e_lfanew at 0x3C land in the XBE's RSA signature (never read at runtime), and the NT headers go
// after the end of the XBE headers (0xC28 here). No code reads the XBEH magic (checked in Ghidra).
#define PE_HDR_AT 0xD00
static void make_hybrid_header(uint8_t *page, const uint8_t *pe, uint32_t xbe_hdr_size)
{
    const uint8_t *nt = pe + rd32(pe + 0x3C);
    uint32_t n = 24 + *(uint16_t *)(nt + 20) + 40 * *(uint16_t *)(nt + 6);
    if (xbe_hdr_size > PE_HDR_AT || PE_HDR_AT + n > 0x1000) xfatal("no room for the PE header (%x + %x)", xbe_hdr_size, n);
    page[0] = 'M'; page[1] = 'Z';
    uint32_t lfanew = PE_HDR_AT;
    memcpy(page + 0x3C, &lfanew, 4);
    memcpy(page + PE_HDR_AT, nt, n);
}

static void bind_kernel_imports(void)
{
    uint32_t *slot = (uint32_t *)(rd32((uint8_t *)XBE_BASE + 0x158) ^ THUNK_KEY_RETAIL);
    int n = 0;
    for (; *slot; slot++, n++) {
        uint32_t ord = *slot & 0x7FFFFFFF;
        const struct kernel_export *e = kernel_exports;
        while (e->name && e->ordinal != ord) e++;
        if (!e->name) xfatal("kernel import ordinal %u has no implementation", ord);
        *slot = (uint32_t)e->impl;
    }
    xlog("bound %d kernel imports", n);
}

// fs: reads that mean something different on Xbox (kernel per-CPU block) than on Windows (TEB).
// Each is a disp32 operand; rewrite the displacement to point at one of our TLS slots in the TEB
// (fs:[0xE10 + 4*slot]), which holds the per-thread value the Xbox code expects. Same length, no trampolines.
// fs:[0] (SEH chain) means the same on both and is left alone. Found in Phase 0 (docs/phase0-findings.md).
static const struct { uint32_t addr, xbox_off; } fs_sites[] = {
    {0x1786cb, 0x20}, {0x178898, 0x20},
    {0x176f20, 0x24}, {0x176f48, 0x24}, {0x1821dc, 0x24}, {0x18226a, 0x24},
    {0x176f2c, 0x28}, {0x176f54, 0x28}, {0x177824, 0x28}, {0x1788b4, 0x28},
    {0x17afbf, 0x28}, {0x17b09b, 0x28}, {0x1821f2, 0x28}, {0x18227e, 0x28},
    {0x0ee7f5, 0x04}, {0x176f37, 0x04}, {0x176f5a, 0x04}, {0x1776fb, 0x04}, {0x1777d0, 0x04},
    {0x17780e, 0x04}, {0x1788ba, 0x04}, {0x182203, 0x04}, {0x18228f, 0x04}, {0x182394, 0x04},
};

static void patch_fs_sites(void)
{
    for (size_t i = 0; i < sizeof fs_sites / sizeof *fs_sites; i++) {
        uint8_t *p = (uint8_t *)fs_sites[i].addr;
        uint32_t off = fs_sites[i].xbox_off;
        uint32_t slot = off == 0x20 ? tls_slot_prcb : off == 0x24 ? tls_slot_irql
                      : off == 0x28 ? tls_slot_kthread : tls_slot_anchor;
        int k = 2;                                        // 64 A1 d32 | 64 8B /r d32 | 64 0F B6 /r d32
        while (k <= 4 && rd32(p + k) != off) k++;
        if (p[0] != 0x64 || k > 4) xfatal("fs: site %08x doesn't look like fs:[%x]", fs_sites[i].addr, off);
        uint32_t disp = 0xE10 + 4 * slot;
        memcpy(p + k, &disp, 4);
    }
    // WBINVD (0F 09) faults in user mode; it only flushed caches before a surface lock.
    uint8_t *w = (uint8_t *)0x11bf9;
    if (w[0] != 0x0F || w[1] != 0x09) xfatal("WBINVD site moved");
    w[0] = 0x66; w[1] = 0x90;
    xlog("patched %u fs: sites + WBINVD", (unsigned)(sizeof fs_sites / sizeof *fs_sites));
}

// Patches (mods_list.h): a mod's patch.txt, applied when the mod is on; one change per line, "<address> = <bytes>",
// all hex:
//   ; Comment lines like this one; without a description in its mod.ini, the launcher shows the first.
//   11bf9 = 66 90
// The game always sits at 0x10000, so addresses are the ones Ghidra shows. A file with a line that doesn't read
// like that, or that writes outside the game's address range, is skipped whole. Where two overlap, the mod higher in
// the list wins (they're applied from the lowest up).
static void apply_patches(const wchar_t *root, uint8_t *lo, uint8_t *hi)
{
    int n;
    const struct mod_item *p = host_mods(&n);
    for (int i = n - 1; i >= 0; i--) {
        wchar_t path[MAX_PATH * 2];
        swprintf(path, MAX_PATH * 2, L"%s\\mods\\%s\\patch.txt", root, p[i].name);
        FILE *f = _wfopen(path, L"r");
        if (!f) continue;
        if (!p[i].on) { xlog("[patches] %ls: off", p[i].name); fclose(f); continue; }
        int bad = 0, written = 0;
        for (int pass = 0; pass < 2 && !bad; pass++) {   // check every line, then write
            rewind(f);
            char line[1024];
            for (int ln = 1; !bad && fgets(line, sizeof line, f); ln++) {
                // a UTF-8 BOM (Notepad's "UTF-8 with BOM") would make line 1 unreadable and the whole patch skipped
                char *s = line + (ln == 1 && !memcmp(line, "\xEF\xBB\xBF", 3) ? 3 : 0), *e, *semi = strchr(line, ';');
                if (semi) *semi = 0;
                while (*s == ' ' || *s == '\t') s++;
                if (!*s || *s == '\r' || *s == '\n') continue;
                uint32_t addr = strtoul(s, &e, 16);
                while (*e == ' ' || *e == '\t') e++;
                uint8_t bytes[256];
                int nb = 0;
                if (e == s || *e != '=') nb = -1;
                for (s = e + 1; nb >= 0;) {
                    while (*s == ' ' || *s == '\t') s++;
                    if (!*s || *s == '\r' || *s == '\n') break;
                    unsigned long b = strtoul(s, &e, 16);
                    if (e == s || e - s > 2 || nb == sizeof bytes) nb = -1;
                    else { bytes[nb++] = (uint8_t)b; s = e; }
                }
                if (nb <= 0 || addr < (uint32_t)lo || addr >= (uint32_t)hi || (uint32_t)nb > (uint32_t)hi - addr) bad = ln;
                else if (pass) { memcpy((void *)addr, bytes, nb); written += nb; }
            }
        }
        fclose(f);
        if (bad) xlog("[patches] %ls: line %d isn't \"<address> = <bytes>\" inside %08x-%08x; patch skipped", p[i].name, bad, (uint32_t)lo, (uint32_t)hi);
        else xlog("[patches] %ls: %d bytes written", p[i].name, written);
    }
}

int host_background(void)
{
    static int v = -1;
    if (v < 0) v = env_int("ONI_BACKGROUND", 0) == 1;
    return v;
}

static LONG CALLBACK on_crash(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    if (r->ExceptionCode == 0x406D1388 || r->ExceptionCode == DBG_PRINTEXCEPTION_C) return EXCEPTION_CONTINUE_SEARCH;
    // The Xbox debug print (int 0x2d; int3, eax 1, ecx -> ANSI_STRING), e.g. the start-up's "Wait Count" when copying
    // the first rooms takes over 2 s: log it and carry on after the int3 (Windows leaves Eip near it, not on it).
    if (r->ExceptionCode == EXCEPTION_BREAKPOINT && c->Eip > 0x10004 && c->Eip < 0x600000)
        for (uint8_t *p = (uint8_t *)c->Eip - 4; p <= (uint8_t *)c->Eip - 1; p++)
            if (!memcmp(p, "\xcd\x2d\xcc", 3)) {
                const uint16_t *s = (const uint16_t *)c->Ecx;
                if (c->Eax == 1 && s && s[0] < 256) xlog("[DbgPrint] %.*s", s[0], *(const char *const *)(s + 2));
                c->Eip = (DWORD)(p + 3);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && (c->Dr6 & 1)) {   // write watchpoint (kernel_watch_writes)
        // Eip is the instruction after the write. No stack walk here: probing pointers inside a vectored handler
        // re-enters it.
        xlog("[watchpoint] write to %08lx just before %08lx (value now %08x), esp %08lx ebp %08lx", (unsigned long)c->Dr0,
             (unsigned long)c->Eip, *(uint32_t *)c->Dr0, (unsigned long)c->Esp, (unsigned long)c->Ebp);
        c->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    HMODULE mod = 0;
    char modname[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)r->ExceptionAddress, &mod))
        GetModuleFileNameA(mod, modname, MAX_PATH);
    xlog("first-chance exception in %s+%lx", strrchr(modname, '\\') ? strrchr(modname, '\\') + 1 : modname,
         (unsigned long)((uint8_t *)r->ExceptionAddress - (uint8_t *)mod));
    xlog("  code %08lx at %08lx (info %08lx %08lx)", r->ExceptionCode, (unsigned long)r->ExceptionAddress,
         r->NumberParameters > 0 ? (unsigned long)r->ExceptionInformation[0] : 0,
         r->NumberParameters > 1 ? (unsigned long)r->ExceptionInformation[1] : 0);
    xlog("  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
         c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    uint32_t *sp = (uint32_t *)c->Esp;
    if (!IsBadReadPtr(sp, 64))
        xlog("  stack: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
             sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7], sp[8], sp[9], sp[10], sp[11], sp[12], sp[13], sp[14], sp[15]);
    return EXCEPTION_CONTINUE_SEARCH;   // ponytail: logged before SEH runs, so also ones the game handles itself
}

// Play time, as Steam counts it: the whole time the game runs, menus included. Kept in saves\playtime.txt (a
// randomizer run's in its own saves, randomizer\<run>\saves\playtime.txt, so the launcher shows each run's time)
// (updates never touch it, and onimusha.ini can be deleted for defaults without losing it); saved every 30 s, so a
// crash loses at most that, and at a normal exit. The launcher shows it. Tours (host_background) don't count.
static wchar_t playtime_path[MAX_PATH];
static long long playtime_base;   // seconds before this run
static ULONGLONG playtime_start;
static void playtime_save(void)
{
    static SRWLOCK lock = SRWLOCK_INIT;   // timer thread vs atexit; try only: at exit the holder may be gone
    if (!TryAcquireSRWLockExclusive(&lock)) return;
    char buf[128];
    int n = snprintf(buf, sizeof buf, "; Time the game has been running, in seconds.\nseconds = %lld\n", playtime_base + (long long)((GetTickCount64() - playtime_start) / 1000));
    write_file_atomic(playtime_path, buf, n);
    ReleaseSRWLockExclusive(&lock);
}
static DWORD WINAPI playtime_thread(void *p) { (void)p; for (;;) { Sleep(30000); playtime_save(); } }
static void playtime_init(const wchar_t *root)
{
    if (host_background()) return;
    if (rando_on()) swprintf(playtime_path, MAX_PATH, L"%s\\" LAYOUT_RUNS L"\\%hs\\saves\\playtime.txt", root, settings.rando_run);
    else swprintf(playtime_path, MAX_PATH, L"%s\\saves\\playtime.txt", root);
    // Counted from 0 only when there's no file yet. One that can't be opened (held by another program) or read (edited
    // by hand) would otherwise be replaced 30 s later by this run's time alone: the total is left as it is instead.
    FILE *f = _wfopen(playtime_path, L"r");
    DWORD why = f ? 0 : GetLastError();
    int parsed = 0;
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) parsed |= sscanf(line, " seconds = %lld", &playtime_base) == 1;
        fclose(f);
    }
    if (f ? !parsed : why != ERROR_FILE_NOT_FOUND && why != ERROR_PATH_NOT_FOUND) {
        xlog("[playtime] %ls couldn't be read (%s): not counted this run, so it isn't overwritten", playtime_path, f ? "no seconds = line" : "can't open");
        return;
    }
    playtime_start = GetTickCount64();
    xlog("[playtime] %.1f hours before this run", playtime_base / 3600.0);
    atexit(playtime_save);
    CloseHandle(CreateThread(0, 0, playtime_thread, 0, 0, 0));
}

__declspec(dllexport) void host_main(void *lo, void *hi)
{
    wchar_t root[MAX_PATH], path[MAX_PATH];
    // Real pixels on scaled (e.g. 150%) displays; otherwise Windows renders at the scaled size and stretches it.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    layout_root(root, MAX_PATH);
    wcscpy(host_root, root);
    {   // one game per install: two would write the same saves, achievements and play time (and log) over each other
        wchar_t name[64];
        layout_game_mutex(root, name, 64);   // the launcher checks for it too (layout_game_running)
        CreateMutexW(0, TRUE, name);
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            MessageBoxW(0, L"Genma Onimusha is already running from this folder.", L"Genma Onimusha", MB_OK | MB_ICONINFORMATION);
            ExitProcess(0);
        }
    }
    layout_migrate(root);   // an install from before the folders (layout.h)
    swprintf(path, MAX_PATH, L"%s\\logs", root);
    CreateDirectoryW(path, 0);
    swprintf(path, MAX_PATH, L"%s\\logs\\onimusha.log", root);
    log_init(path);
    xlog("Genma Onimusha native host starting, root %ls", root);
    AddVectoredExceptionHandler(1, on_crash);

    uint32_t xsize;
    swprintf(path, MAX_PATH, L"%s\\game\\default.xbe", root);
    uint8_t *xbe = read_file(path, 64u << 20, &xsize);
    if (!xbe) xfatal("can't read %ls: install the game from the launcher (or run the prepare step) first", path);

    static uint8_t pe_header[0x1000];
    memcpy(pe_header, lo, sizeof pe_header);
    uint32_t entry = map_xbe(xbe, xsize, lo, hi);
    make_hybrid_header(lo, pe_header, rd32(xbe + 0x108));
    free(xbe);
    settings_load(root);
    // console = 1 (the launcher's About page): a window with the log as it's written. Closing it ends the game.
    if (settings.console && AllocConsole()) xlog("console window opened (console = 1)");
    texpack_init(root);
    report_init(root);
    ach_init(root);
    mapping_init();
    playtime_init(root);
    kernel_init(root);
    bind_kernel_imports();
    patch_fs_sites();
    hle_install();
    apply_patches(root, lo, hi);
    FlushInstructionCache(GetCurrentProcess(), lo, (uint8_t *)hi - (uint8_t *)lo);

    kernel_attach_boot_thread();
    xlog("jumping to entry %08x", entry);
    ((void (*)(void))entry)();
    xlog("entry returned");
    ExitThread(0);   // the game's main thread keeps running; the process ends when it calls HalReturnToFirmware
}
