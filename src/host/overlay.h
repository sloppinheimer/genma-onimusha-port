// Which copy of a disc file is read: a randomizer run's (randomizer\<run>\files\<rel>), then the first mod that has it
// (mods\<name>\<rel>, the highest first), else none and the caller reads the disc's (game\<rel>). Shared by kernel.c's two
// lookups, the game's own opens (translate_path) and the port's reads of disc files (mod_file), which were two copies
// that had drifted; header-only, so tests/selftest.cpp checks it. Needs <windows.h>, <stdio.h> and layout.h before it.
#pragma once


// rel: under the disc, with no leading '\'. run: the randomizer run's folder name, or 0 / "" outside one. Returns 2 (the
// run's copy), 1 (a mod's: *which, if given, its index in mods) or 0, with out the path it found.
static int overlay_path(const wchar_t *root, const char *run, const struct mod_item *mods, int nmods, const wchar_t *rel,
                        wchar_t *out, size_t n, int *which)
{
    if (run && *run) {
        swprintf(out, n, L"%s\\" LAYOUT_RUNS L"\\%hs\\files\\%s", root, run, rel);
        if (is_file(out)) return 2;
    }
    for (int m = 0; m < nmods; m++) {
        swprintf(out, n, L"%s\\mods\\%s\\%s", root, mods[m].name, rel);
        if (is_file(out)) { if (which) *which = m; return 1; }
    }
    return 0;
}
