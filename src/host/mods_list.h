// The mods\ folder: every folder in it is a mod, and mods\mods.txt keeps their order and which are switched off.
// Shared by the game (kernel.c, host.c, settings.c, texpack.c) and the launcher, so both see the same list.
// A mod replaces game files (laid out as in game\), and/or changes the game's code with a patch.txt (host.c).
//
// mods\mods.txt (UTF-8, written by the launcher; lines starting with ; are comments):
//   mod = <folder>        a folder in mods\ that's on. When two mods have the same file, the higher line wins.
//   mod_off = <folder>    one that's off
// Folders mods.txt doesn't mention go after the listed ones, alphabetically, on unless their mod.ini says
// enabled = 0. (Up to the first 0.8 builds it was mods.txt on top, with patches as files in patches\: layout.h
// layout_migrate moves those.)
//
// mods\<folder>\mod.ini (optional; UTF-8, ; starts a comment line, values run to the end of the line):
//   name = Weapon wheel           shown in the launcher instead of the folder name
//   description = ...             shown in the launcher's help line
//   enabled = 0                   on or off before the player has switched it (afterwards mods.txt decides)
//   builtin = <id>                a mod written into the port's code: the id the code asks for (builtin_mod_on)
// The launcher also reads author, version (shown on its card), modern = 1 (switched on by the Modern preset) and
// pack = HD pack (a folder the launcher downloaded; version is then the release it came from).
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// Mod folders read (mods.txt and mods\ together). Past it, folders are left out, and the launcher's next save of
// mods.txt would drop their lines (their order and off switches): 64 was within reach with the HD pack's folders.
#define MOD_MAX 256

// p is there and is a file, not a folder (the game, the overlay lookup and the launcher all ask this).
static int is_file(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
// p isn't there at all (not merely unreadable: locked, no access): only then may a default replace it.
static int file_missing(const wchar_t *p)
{
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return 0;
    DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}

// A file written whole or not at all (shared by the game and the launcher for every file they keep): a .tmp beside it,
// flushed to disk, then renamed over it, so a crash, a power cut or a full disk leaves the old file. 1: written.
static int write_file_atomic(const wchar_t *path, const void *data, size_t n)
{
    wchar_t tmp[MAX_PATH * 2];
    swprintf(tmp, MAX_PATH * 2, L"%ls.tmp", path);
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    BOOL ok = WriteFile(f, data, (DWORD)n, &w, 0) && w == n && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
    DeleteFileW(tmp);
    return 0;
}
// text (lines ending "\n") written as write_file_atomic does, with Windows line ends, as a text-mode fputs gave it.
static int write_text_atomic(const wchar_t *path, const char *text)
{
    size_t n = 0, o = 0;
    for (const char *c = text; *c; c++) n += *c == '\n' ? 2 : 1;
    char *b = (char *)malloc(n ? n : 1);
    if (!b) return 0;
    for (const char *c = text; *c; c++) { if (*c == '\n') b[o++] = '\r'; b[o++] = *c; }
    int ok = write_file_atomic(path, b, o);
    free(b);
    return ok;
}
struct mod_item { wchar_t name[MAX_PATH]; int on; };

// One line of an ini-style file (onimusha.ini, mod.ini, mods.txt, pack.ini; game and launcher alike): "key = value",
// trimmed, a UTF-8 BOM skipped. cut_comments: a ';' ends the value (onimusha.ini); without it a value may hold ';'
// (a mod's description, a folder name). 1 with key and value pointing into line; 0 for a blank, comment or [section]
// line (section, when given, gets a [section] line's name).
static int ini_line(char *line, char **key, char **value, int cut_comments, char *section, size_t section_n)
{
    char *s = line + (!memcmp(line, "\xEF\xBB\xBF", 3) ? 3 : 0), *e;
    while (*s == ' ' || *s == '\t') s++;
    for (e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'); ) *--e = 0;
    if (*s == '[') { if (section && (e = strchr(s, ']'))) { *e = 0; snprintf(section, section_n, "%s", s + 1); } return 0; }
    char *eq = strchr(s, '=');
    if (*s == ';' || !eq) return 0;
    if (cut_comments && (e = strchr(eq, ';'))) *e = 0;
    for (e = eq; e > s && (e[-1] == ' ' || e[-1] == '\t'); ) e--;
    *e = 0;
    for (s = eq + 1; *s == ' ' || *s == '\t'; ) s++;
    for (e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
    *key = line + (!memcmp(line, "\xEF\xBB\xBF", 3) ? 3 : 0);
    while (**key == ' ' || **key == '\t') ++*key;
    *value = s;
    return 1;
}

// The value of key in section ("" = before any [section]) of mods\<folder>\mod.ini, as UTF-8; 0 if it isn't there.
static int mod_ini(const wchar_t *root, const wchar_t *folder, const char *section, const char *key, char *out, size_t n)
{
    wchar_t p[MAX_PATH * 2];
    swprintf(p, MAX_PATH * 2, L"%ls\\mods\\%ls\\mod.ini", root, folder);
    FILE *f = _wfopen(p, L"rb");
    if (!f) return 0;
    char line[1024], sec[64] = "", *k, *v;
    int found = 0;
    while (!found && fgets(line, sizeof line, f))
        if (ini_line(line, &k, &v, 0, sec, sizeof sec) && !_stricmp(sec, section) && !_stricmp(k, key)) { snprintf(out, n, "%s", v); found = 1; }
    fclose(f);
    return found;
}

static int mod_cmp(const void *a, const void *b) { return _wcsicmp(((const struct mod_item *)a)->name, ((const struct mod_item *)b)->name); }
static int mod_find(const struct mod_item *v, int n, const wchar_t *name)
{
    for (int i = 0; i < n; i++) if (!_wcsicmp(v[i].name, name)) return i;
    return -1;
}
static int mod_exists(const wchar_t *root, const wchar_t *name)
{
    wchar_t p[MAX_PATH * 2];
    swprintf(p, MAX_PATH * 2, L"%ls\\mods\\%ls", root, name);
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Fills out[MOD_MAX] with the mods, in order (the highest first); returns how many.
static int mod_list(const wchar_t *root, struct mod_item *out)
{
    int n = 0;
    wchar_t p[MAX_PATH * 2];
    swprintf(p, MAX_PATH * 2, L"%ls\\mods\\mods.txt", root);
    FILE *f = _wfopen(p, L"rb");
    char line[1024];
    char *k, *v;
    while (f && fgets(line, sizeof line, f) && n < MOD_MAX) {
        if (!ini_line(line, &k, &v, 0, 0, 0)) continue;
        int on = !_stricmp(k, "mod");
        if (!on && _stricmp(k, "mod_off")) continue;
        struct mod_item it = {{0}, on};
        if (!MultiByteToWideChar(CP_UTF8, 0, v, -1, it.name, MAX_PATH) || !it.name[0] || it.name[0] == L'.' || wcspbrk(it.name, L"\\/:")) continue;   // "..": the exe's folder
        if (mod_find(out, n, it.name) < 0 && mod_exists(root, it.name)) out[n++] = it;
    }
    if (f) fclose(f);
    int listed = n;
    swprintf(p, MAX_PATH * 2, L"%ls\\mods\\*", root);
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(p, &d);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || d.cFileName[0] == L'.') continue;
            if (n < MOD_MAX && mod_find(out, n, d.cFileName) < 0) {
                char en[8] = "1";
                mod_ini(root, d.cFileName, "", "enabled", en, sizeof en);
                wcscpy(out[n].name, d.cFileName);
                out[n++].on = atoi(en) != 0;
            }
        } while (FindNextFileW(h, &d));
        FindClose(h);
    }
    qsort(out + listed, n - listed, sizeof *out, mod_cmp);
    return n;
}
