// A randomizer run (docs/randomizer.md). The launcher's Randomizer tab makes each run in randomizer\<run>\: files\ holds
// copies of the room files with items moved (read before any mod, kernel.c), tables.txt the Dark Realm's and Ogre
// Tower's floor items. onimusha.ini [randomizer] randomizer_run names the one being played; while it's set the game's
// saves are randomizer\<run>\saves (kernel.c), so a run never touches the normal saves or another run's, and neither
// achievements nor the research map (mapping.c) take anything from it.
#include "host.h"
#include "game.h"
#include "layout.h"   // LAYOUT_RUNS; mods_list.h's ini_line
#include "patch.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

int rando_on(void) { return settings.rando_run[0] != 0; }

// An orb picked up outside its own room. The "you got" routine (FUN_00162d10) gives an orb (0x11-0x13) its own
// sequence: it fades out (FUN_000d8270(3)) and runs game mode 8/3, the orb's scene, and in any other room than the
// orb's own (116, 230, 530) that came straight back and the screen stayed black for good (a run's Thunder Orb in room
// 311's chest; reproduced on a test copy). Oni Spirits takes a short path instead (G_MODE_FLAGS bit 3, the test at
// 0x163205): no fade, the orb and its sword set to level 1 (0x5471fc + e, 0x5471ee + item). In a run, outside the orb's
// room, the test is replaced by orb_short(), which takes that path, and rando_frame then puts the levels back to what
// they were (0, Lv1, for an orb not held before).
static volatile int orb_e = -1;
static uint8_t orb_was[2];
static int __cdecl orb_short(void)
{
    static const int home[3] = {116, 230, 530};
    if (*(volatile uint32_t *)G_MODE_FLAGS & 8) return 1;   // Oni Spirits: the game's own short path
    int e = *(volatile uint8_t *)0x498503 - 0x11;           // the item being given
    int room = game_room();
    if (e < 0 || e > 2 || room == home[e]) return 0;
    orb_was[0] = *(volatile uint8_t *)(G_LEVELS + e);
    orb_was[1] = *(volatile uint8_t *)(G_SWORD_LEVEL + e);
    orb_e = e;
    xlog("[randomizer] orb %d picked up in room %03d: no orb scene", e, room);
    return 1;
}

// After orb_short's pickup the short path has set the orb and its sword to level 1: back to what they were. Called first
// thing each frame by orb_items_frame (input.c), so a level item waiting for this orb counts after it, not before.
void rando_orb_levels(void)
{
    if (orb_e < 0) return;
    int e = orb_e;
    *(volatile uint8_t *)(G_LEVELS + e) = orb_was[0] == 0xff ? 0 : orb_was[0];
    *(volatile uint8_t *)(G_SWORD_LEVEL + e) = orb_was[1] == 0xff ? 0 : orb_was[1];
    orb_e = -1;
    xlog("[randomizer] orb %d: levels back to %d", e, *(volatile uint8_t *)(G_LEVELS + e));
}

// tables.txt: "va item" a line, the item byte of a floor-table entry (tools/rando/build_data.py, T lines). The tables
// are read when a floor is set up (FUN_001389a0, FUN_001467d0), so writing them once at start is enough.
void rando_install(void)
{
    if (!rando_on()) return;
    // the run's orb level items get inventory slots of their own (49-54, which no item uses), so one picked up before
    // its orb waits there (input.c, orb_items_frame) instead of taking the orb's slot
    static const uint8_t level_ids[6] = {RANDO_LEVEL_ITEMS};
    for (int k = 0; k < 6; k++) *(volatile uint8_t *)(G_ITEM_TABLE + 8 * level_ids[k] + 1) = (uint8_t)(RANDO_LEVEL_SLOT0 + k);
    wchar_t path[MAX_PATH];
    host_path(path, LAYOUT_RUNS);
    swprintf(path + wcslen(path), MAX_PATH - wcslen(path), L"\\%hs\\tables.txt", settings.rando_run);
    FILE *f = _wfopen(path, L"r");
    int n = 0;
    unsigned va, item;
    while (f && fscanf(f, "%x %x", &va, &item) == 2)
        if (va >= 0x1e0000 && va < 0x230000 && item < 0xa4) { *(volatile uint8_t *)va = (uint8_t)item; n++; }
    if (f) fclose(f);
    xlog("[randomizer] run %s: %d floor items", settings.rando_run, n);
    // 0x163205: test byte ptr [G_MODE_FLAGS], 8 (7 bytes), then je to the orb scene: becomes call orb_short; test al, al
    uint8_t *at = (uint8_t *)0x163205;
    static const uint8_t was[] = {0xf6, 0x05, 0x5c, 0x36, 0x54, 0x00, 0x08, 0x0f, 0x84};
    if (memcmp(at, was, sizeof was)) { xlog("[randomizer] orb test moved: orbs outside their rooms will black-screen"); return; }
    patch_rel32(at, 0xE8, orb_short);
    at[5] = 0x84; at[6] = 0xc0;
}

// run.ini "start_items = 4f ...": items the run starts with (runs made while the launcher had Start with the Vision
// Staff; it writes none now, but those runs still play as made). Each goes in
// its own inventory slot (the item table's byte 1) the way a pickup puts a single item there (FUN_00162d10:
// {id, 0xff, 0, flags | 0x30}) whenever that slot is empty in play, so a new game gets them and a loaded one keeps them.
static uint8_t start_items[8];
static int start_n = -1;
void rando_frame(void)
{
    if (start_n < 0) {
        wchar_t path[MAX_PATH];
        char line[256];
        start_n = 0;
        host_path(path, LAYOUT_RUNS);
        swprintf(path + wcslen(path), MAX_PATH - wcslen(path), L"\\%hs\\run.ini", settings.rando_run);
        FILE *f = _wfopen(path, L"r");
        while (f && fgets(line, sizeof line, f)) {   // ini_line, as every ini the port reads (a key like start_items_x isn't this one)
            char *k, *v;
            if (!ini_line(line, &k, &v, 1, 0, 0) || _stricmp(k, "start_items")) continue;
            for (char *t = strtok(v, " ,\r\n"); t && start_n < 8; t = strtok(0, " ,\r\n")) start_items[start_n++] = (uint8_t)strtoul(t, 0, 16);
        }
        if (f) fclose(f);
        if (start_n) xlog("[randomizer] run %s: starts with %d item(s)", settings.rando_run, start_n);
    }
    for (int i = 0; i < start_n; i++) {
        int id = start_items[i];
        if (!id || id >= 0xa4 || item_count(id)) continue;
        volatile uint8_t *slot = (volatile uint8_t *)(G_INVENTORY + 4 * *(const uint8_t *)(G_ITEM_TABLE + 8 * id + 1));
        if (slot[0]) continue;
        slot[0] = (uint8_t)id; slot[1] = 0xff; slot[2] = 0; slot[3] |= 0x30;
        xlog("[randomizer] start item %02x given", id);
    }
}
