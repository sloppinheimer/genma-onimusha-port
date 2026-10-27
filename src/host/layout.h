// The install's folders, shared by the game and the launcher. GenmaOnimusha.exe and README.md sit in the install
// folder (the root); everything else is in a folder under it:
//   bin\        onimusha.exe, onimusha_host.dll (the Kabuto Sans font is built into it), SDL3.dll; licenses\ in it
//   game\       the prepared disc       mods\  (every folder in it a mod, and mods.txt: their order and switches)
//   onimusha.ini  the player's settings, on top
//   saves\      the Xbox hard disk's user data (E:), achievements and play time
//   cache\      only with French or German text: the two fixed copies of the disc's files (kernel.c)
//   logs\       onimusha.log and the records the game keeps (features.log, stalls.txt, textures_seen.txt, ...)
//   captures\  mapping\
//   randomizer\<run>\  a randomizer run (made by launcher/randomizer.h): run.ini, tables.txt, spoiler.txt, files\ (its
//               room files, read before any mod) and saves\ (its own E: drive and play time)
// Achievements and the clean-run list are sealed (sealed_write): scrambled text with a checksum, so they can't be
// read or edited by hand (an edited file reads as empty). That stops casual editing, not a determined cheater.
// Up to 0.7 everything was flat in the root; layout_migrate moves an old install's files into place.
#pragma once
#include "mods_list.h"   // write_file_atomic
#include <stdint.h>

#define LAYOUT_RUNS L"randomizer"               // the runs' folder in the root: LAYOUT_RUNS\<run>\ ...
#define LAYOUT_TITLE_SAVES L"UDATA\\43430001"   // the game's saves under saves\ (and a run's saves\): its title id's folder

// The root from the running exe's own path: its folder, or that folder's parent when the folder is bin\.
static void layout_root(wchar_t *root, size_t n)
{
    GetModuleFileNameW(0, root, (DWORD)n);
    wchar_t *s = wcsrchr(root, L'\\');
    if (s) *s = 0;
    if ((s = wcsrchr(root, L'\\')) && !_wcsicmp(s + 1, L"bin")) *s = 0;
}

// One game per install: the game holds a mutex named for its root while it runs (host.c), and the launcher looks for
// it before touching what the game has open. The name hashes the root's long form, lower-cased, so the same folder
// reached by a short (8.3) path or in other letter case is the same game.
static void layout_game_mutex(const wchar_t *root, wchar_t *out, size_t n)
{
    wchar_t full[MAX_PATH * 2];
    DWORD got = GetLongPathNameW(root, full, MAX_PATH * 2);
    const wchar_t *p = got && got < MAX_PATH * 2 ? full : root;
    uint32_t hash = 2166136261u;
    for (const wchar_t *c = p; *c; c++) hash = (hash ^ (uint32_t)towlower(*c)) * 16777619u;
    swprintf(out, n, L"GenmaOnimushaGame-%08x", hash);
}
// The game is running from root (started by the launcher or not).
static int layout_game_running(const wchar_t *root)
{
    wchar_t name[64];
    layout_game_mutex(root, name, 64);
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (m) CloseHandle(m);
    return m != 0;
}

// ---- sealed files ------------------------------------------------------------------------------------------------
// "GOS1", FNV-1a of the text (keyed), then the text XORed with an xorshift stream.
static uint32_t sealed_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 0x9E3779B9u ^ 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
static void sealed_xor(uint8_t *p, size_t n)
{
    uint32_t x = 0x4F4E494Du;   // "ONIM"
    for (size_t i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; p[i] ^= (uint8_t)x; }
}
static int sealed_write(const wchar_t *path, const char *text, size_t n)
{
    uint8_t *b = (uint8_t *)malloc(n + 8);
    if (!b) return 0;
    uint32_t h = sealed_hash((const uint8_t *)text, n);
    memcpy(b, "GOS1", 4); memcpy(b + 4, &h, 4); memcpy(b + 8, text, n);
    sealed_xor(b + 8, n);
    int ok = write_file_atomic(path, b, n + 8);
    free(b);
    return ok;
}
// The text, 0-terminated, into out (max bytes with the 0): its length; -1 no file; -2 not a sealed file or edited.
static int sealed_read(const wchar_t *path, char *out, size_t max)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f) return -1;
    uint8_t hdr[8];
    size_t n = fread(hdr, 1, 8, f) == 8 && !memcmp(hdr, "GOS1", 4) ? fread(out, 1, max - 1, f) : (size_t)-1;
    int more = fgetc(f) != EOF;
    fclose(f);
    if (n == (size_t)-1 || more) return -2;
    sealed_xor((uint8_t *)out, n);
    uint32_t h;
    memcpy(&h, hdr + 4, 4);
    if (h != sealed_hash((const uint8_t *)out, n)) return -2;
    out[n] = 0;
    return (int)n;
}

// ---- moving an old (flat) install into the folders -----------------------------------------------------------------
static int layout_exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static void layout_parent(const wchar_t *p) { wchar_t d[MAX_PATH]; wcscpy(d, p); *wcsrchr(d, L'\\') = 0; CreateDirectoryW(d, 0); }
// root\from -> root\to, when from is there and to isn't (folders too). 1: moved.
static int layout_move(const wchar_t *root, const wchar_t *from, const wchar_t *to)
{
    wchar_t a[MAX_PATH], b[MAX_PATH];
    swprintf(a, MAX_PATH, L"%ls\\%ls", root, from);
    swprintf(b, MAX_PATH, L"%ls\\%ls", root, to);
    if (!layout_exists(a) || layout_exists(b)) return 0;
    layout_parent(b);
    return MoveFileExW(a, b, 0);
}
// A text file root\from sealed into root\to (unless to is there already), then deleted.
static void layout_seal(const wchar_t *root, const wchar_t *from, const wchar_t *to)
{
    wchar_t a[MAX_PATH], b[MAX_PATH];
    swprintf(a, MAX_PATH, L"%ls\\%ls", root, from);
    swprintf(b, MAX_PATH, L"%ls\\%ls", root, to);
    FILE *f = _wfopen(a, L"rb");
    if (!f) return;
    static char text[65536];
    size_t n = fread(text, 1, sizeof text, f);
    fclose(f);
    layout_parent(b);
    if (layout_exists(b) || sealed_write(b, text, n)) DeleteFileW(a);
}
// A folder of the port's own and everything in it, without following links (a junction is removed, not entered).
static void layout_delete_tree(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH], p[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(pat, &d);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L"..")) continue;
            swprintf(p, MAX_PATH, L"%ls\\%ls", dir, d.cFileName);
            if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW(p);
            else if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(p);
            else layout_delete_tree(p);
        } while (FindNextFileW(h, &d));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

// patches\<name>.txt of the first 0.8 builds -> a mod, mods\<name>\patch.txt (mods_list.h). The patch's first ; line
// becomes the mod's description, and a patch that mods.txt switched off (patch_off = <file>) becomes a mod that
// starts off (enabled = 0). A name a mod already has gets " (patch)" added.
static void layout_patches(const wchar_t *root)
{
    wchar_t pat[MAX_PATH], from[MAX_PATH], dir[MAX_PATH], to[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\patches\\*.txt", root);
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(pat, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        size_t n = wcslen(d.cFileName);
        swprintf(from, MAX_PATH, L"%ls\\patches\\%ls", root, d.cFileName);
        swprintf(dir, MAX_PATH, L"%ls\\mods\\%.*ls", root, (int)(n - 4), d.cFileName);
        if (layout_exists(dir)) swprintf(dir, MAX_PATH, L"%ls\\mods\\%.*ls (patch)", root, (int)(n - 4), d.cFileName);
        if (layout_exists(dir)) continue;
        char about[512] = "", line[512], file[MAX_PATH * 3], *k, *v;
        FILE *f = _wfopen(from, L"r");
        while (f && fgets(line, sizeof line, f)) {   // the first line that isn't blank, when it's a comment
            char *s = line + (!memcmp(line, "\xEF\xBB\xBF", 3) ? 3 : 0);
            while (*s == ' ' || *s == '\t') s++;
            if (*s == '\r' || *s == '\n' || !*s) continue;
            if (*s == ';') { for (s++; *s == ' ' || *s == '\t'; ) s++; snprintf(about, sizeof about, "%s", s); about[strcspn(about, "\r\n")] = 0; }
            break;
        }
        if (f) fclose(f);
        int off = 0;   // patch_off = <file> in mods.txt (it's in mods\ by now)
        WideCharToMultiByte(CP_UTF8, 0, d.cFileName, -1, file, sizeof file, 0, 0);
        swprintf(to, MAX_PATH, L"%ls\\mods\\mods.txt", root);
        if ((f = _wfopen(to, L"rb"))) {
            while (fgets(line, sizeof line, f)) if (ini_line(line, &k, &v, 0, 0, 0) && !_stricmp(k, "patch_off") && !_stricmp(v, file)) off = 1;
            fclose(f);
        }
        layout_parent(dir);
        CreateDirectoryW(dir, 0);
        swprintf(to, MAX_PATH, L"%ls\\patch.txt", dir);
        if (!MoveFileExW(from, to, 0)) continue;
        if (about[0] || off) {
            char ini[700];
            int m = snprintf(ini, sizeof ini, "; Made from patches\\%s when patches became mods.\n", file);
            if (about[0]) m += snprintf(ini + m, sizeof ini - m, "description = %s\n", about);
            if (off) m += snprintf(ini + m, sizeof ini - m, "enabled = 0\n");
            swprintf(to, MAX_PATH, L"%ls\\mod.ini", dir);
            write_file_atomic(to, ini, (size_t)m);
        }
    } while (FindNextFileW(h, &d));
    FindClose(h);
    swprintf(dir, MAX_PATH, L"%ls\\patches", root);
    RemoveDirectoryW(dir);   // only when empty
}

// Safe to run at every start, by the game and the launcher alike: each step only acts on what's still in the old place.
static void layout_migrate(const wchar_t *root)
{
    layout_move(root, L"hdd\\E", L"saves");   // the user data partition first: the rest of hdd\ is the cache
    layout_move(root, L"hdd", L"cache");
    // the hard-disk cache before the virtual Z: drive (kernel.c): copies of the disc's files, and empty partitions
    static const wchar_t *const cache[] = {L"Z", L"C", L"X", L"Y"};
    for (size_t i = 0; i < sizeof cache / sizeof *cache; i++) {
        wchar_t d[MAX_PATH];
        swprintf(d, MAX_PATH, L"%ls\\cache\\%ls", root, cache[i]);
        if (layout_exists(d)) layout_delete_tree(d);
    }
    {
        wchar_t d[MAX_PATH];
        swprintf(d, MAX_PATH, L"%ls\\cache\\partition0.bin", root);
        DeleteFileW(d);
        swprintf(d, MAX_PATH, L"%ls\\cache", root);
        RemoveDirectoryW(d);   // only when empty
    }
    layout_seal(root, L"achievements.txt", L"saves\\achievements.dat");
    layout_seal(root, L"achievements_runs.txt", L"saves\\achievement_runs.dat");
    layout_move(root, L"playtime.txt", L"saves\\playtime.txt");
    layout_move(root, L"mods.txt", L"mods\\mods.txt");
    layout_patches(root);
    static const wchar_t *const logs[] = {L"onimusha.log", L"features.log", L"stalls.txt", L"stalls.old.txt", L"textures_seen.txt", L"room_textures.txt"};
    for (size_t i = 0; i < sizeof logs / sizeof *logs; i++) {
        wchar_t to[MAX_PATH];
        swprintf(to, MAX_PATH, L"logs\\%ls", logs[i]);
        layout_move(root, logs[i], to);
    }
    // the program files of the flat layout, once the new ones are in bin\ (an update leaves them behind), and the
    // fonts\ and licenses\ folders of 0.8's first layout (the font is in the DLL now, the licences in bin\licenses\)
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\bin\\onimusha.exe", root);
    if (!layout_exists(p)) return;
    static const wchar_t *const old[] = {L"onimusha.exe", L"onimusha_host.dll", L"SDL3.dll", L"onimusha.pdb", L"onimusha_host.pdb",
        L"KabutoSans.ttf", L"SDL3-LICENSE.txt", L"KabutoSans-OFL.txt", L"KabutoSans-KosugiMaru-LICENSE.txt", L"WebView2-LICENSE.txt"};
    for (size_t i = 0; i < sizeof old / sizeof *old; i++) {
        swprintf(p, MAX_PATH, L"%ls\\%ls", root, old[i]);
        DeleteFileW(p);
        swprintf(p, MAX_PATH, L"%ls\\fonts\\%ls", root, old[i]);
        DeleteFileW(p);
        swprintf(p, MAX_PATH, L"%ls\\licenses\\%ls", root, old[i]);
        DeleteFileW(p);
    }
    swprintf(p, MAX_PATH, L"%ls\\fonts", root);
    RemoveDirectoryW(p);   // only when empty: a file of the player's own stays, and so does the folder
    swprintf(p, MAX_PATH, L"%ls\\licenses", root);
    RemoveDirectoryW(p);
}
