// The game map for the randomizer (docs/randomizer.md), built up in play\mapping\ from what play uncovers, across
// launches, saves and playthroughs: each fact is written once, the first time it's seen.
//   items.txt      every pickup bit, story flag and door unlock, and the first time each item is held (event items such
//                  as the Shinobi Kit have no pickup bit), with the room it happened in: "<key>\t<room>\t<what>"
//   rooms.txt      each room's first visit and each move from room to room, with the camera arrived on
//   checklist.txt  rewritten when something new turns up: every expected item location, missing or found (Fluorite
//                  numbered), then the rooms and door links not seen yet. The expected lists are locations.tsv and
//                  doors.tsv in the same folder, written from the game's own files by tools/rando/rdt_scan.py
//                  (they hold game data, so they aren't shipped); without them the checklist is left out.
// A change to many entries at once is a save loaded (or Kaede swapping in): its pickup bits count as found, but
// nothing is credited to the room, and its story flags wait until they are seen set in play. Oni Spirits (rooms
// 407/408) is left out: game_frame doesn't call in while it runs, and the checklist leaves its rooms out.
#include "host.h"
#include "game.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ITEM_TABLE G_ITEM_TABLE   // game.h
#define NAME_TAB G_NAME_OFFS
#define NAME_TEXT G_NAME_TEXT
#define NAMES G_NAMES

enum { F_ITEMS, F_ROOMS, F_COUNT };
static FILE *out[F_COUNT];
static uint64_t keys[1 << 14];
static uint32_t nkeys;
static wchar_t dir[MAX_PATH];
static int dirty;   // the checklist needs rewriting

static void file_path(wchar_t *p, const wchar_t *name) { swprintf(p, MAX_PATH, L"%ls\\%ls", dir, name); }

// "<key>\t..." once per key, ever (the keys of both files are read back at start)
static int note(int f, const char *key, const char *fmt, ...)
{
    if (!out[f] || !keyset_add(keys, 1 << 14, &nkeys, fnv64_str(FNV64_START, key))) return 0;
    char text[200];
    va_list a;
    va_start(a, fmt);
    vsnprintf(text, sizeof text, fmt, a);
    va_end(a);
    fprintf(out[f], "%s\t%s\n", key, text);
    fflush(out[f]);
    xlog("[map] %s  %s", key, text);
    return dirty = 1;
}
static int known(const char *key)   // without adding it
{
    int absent;
    keyset_slot(keys, 1 << 14, fnv64_str(FNV64_START, key), &absent);
    return !absent;
}

// The game's own name for an item (its message text: a letter is 0xfa + (char - 0x20), 0 a space)
static const char *item_name(int id)
{
    static char s[40];
    int nid = id < 0xa4 ? *(const uint8_t *)(ITEM_TABLE + 8 * id) : 0;
    if (!nid) { snprintf(s, sizeof s, "item %02x", id); return s; }
    const uint16_t *tab = (const uint16_t *)NAME_TAB;
    const uint8_t *p = (const uint8_t *)NAME_TEXT + tab[nid], *end = nid + 1 < NAMES ? (const uint8_t *)NAME_TEXT + tab[nid + 1] : (const uint8_t *)NAME_TAB;
    int n = 0;
    while (p < end && n < (int)sizeof s - 1) {
        if (*p == 0xfa) { s[n++] = (char)(p[1] + 0x20); p += 2; continue; }
        if ((*p == 0 || *p == 0xf2) && n && s[n - 1] != ' ') s[n++] = ' ';
        p++;
    }
    while (n && s[n - 1] == ' ') n--;
    s[n] = 0;
    return s;
}

// ---- expected lists (locations.tsv, doors.tsv) and the checklist ----
static struct loc { char room[40], kind[8], name[24]; int item, flag, fluorite; } *locs;
static int nlocs;
static char (*links)[16];   // "125>106" door links, once each
static int nlinks;
static char (*rooms)[8];
static int nrooms;

static void load_expected(void)
{
    wchar_t p[MAX_PATH];
    char line[256];
    file_path(p, L"locations.tsv");
    FILE *f = _wfopen(p, L"r");
    int fl = 0;
    while (f && fgets(line, sizeof line, f)) {   // room, kind, item (hex), name, count, pickup flag
        struct loc l = {0};
        if (sscanf(line, "%39[^\t]\t%7[^\t]\t%x\t%23[^\t]\t%*[^\t]\t%d", l.room, l.kind, (unsigned *)&l.item, l.name, &l.flag) != 5) continue;
        if (strstr(l.room, "hard") || !strcmp(l.room, "407") || !strcmp(l.room, "408") || (strcmp(l.kind, "pickup") && strcmp(l.kind, "search") && strcmp(l.kind, "floor"))) continue;
        int dup = 0;   // a flag seen from more than one room (room versions) is one location
        for (int i = 0; i < nlocs && !dup; i++) dup = locs[i].flag == l.flag;
        if (dup) continue;
        if (l.item == 7) l.fluorite = ++fl;
        void *grown = realloc(locs, (nlocs + 1) * sizeof *locs);   // out of memory: the checklist just lists fewer
        if (!grown) break;
        locs = grown;
        locs[nlocs++] = l;
    }
    if (f) fclose(f);
    file_path(p, L"doors.tsv");
    f = _wfopen(p, L"r");
    while (f && fgets(line, sizeof line, f)) {   // room, to, camera, lock bit, needs
        char a[8], b[8], key[16];
        if (sscanf(line, "%7[^\t]\t%7[^\t]", a, b) != 2 || strlen(a) != 3 || strlen(b) != 3) continue;   // the header; 5181 & co (room versions)
        if (!strncmp(a, "40", 2) && (a[2] == '7' || a[2] == '8')) continue;   // 407/408: Oni Spirits, not the story
        for (int k = 0; k < 2; k++) {
            const char *r = k ? b : a;
            int have = 0;
            for (int i = 0; i < nrooms && !have; i++) have = !strcmp(rooms[i], r);
            void *grown = have ? 0 : realloc(rooms, (nrooms + 1) * sizeof *rooms);
            if (grown) { rooms = grown; snprintf(rooms[nrooms++], 8, "%s", r); }
        }
        if (!strcmp(a, b)) continue;
        snprintf(key, sizeof key, "%s>%s", a, b);
        int have = 0;
        for (int i = 0; i < nlinks && !have; i++) have = !strcmp(links[i], key);
        void *grown = have ? 0 : realloc(links, (nlinks + 1) * sizeof *links);
        if (grown) { links = grown; strcpy(links[nlinks++], key); }
    }
    if (f) fclose(f);
}

static void write_checklist(void)
{
    if (!nlocs) return;
    wchar_t p[MAX_PATH];
    file_path(p, L"checklist.txt");
    FILE *f = _wfopen(p, L"w");
    if (!f) return;
    int found = 0, fl = 0, flf = 0, rv = 0, lt = 0;
    char key[24];
    for (int i = 0; i < nlocs; i++) {
        snprintf(key, sizeof key, "pickup %d", locs[i].flag);
        int got = known(key);
        found += got;
        if (locs[i].fluorite) fl++, flf += got;
    }
    for (int i = 0; i < nrooms; i++) { snprintf(key, sizeof key, "visit %s", rooms[i]); rv += known(key); }
    for (int i = 0; i < nlinks; i++) lt += known(links[i]);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "; Randomizer map (mapping.c), %04d-%02d-%02d %02d:%02d. Item locations found: %d of %d (Fluorite %d of %d).\n"
               "; Rooms visited: %d of %d. Door links taken: %d of %d. Rooms are roomSRR: stage S, room RR.\n",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, found, nlocs, flf, fl, rv, nrooms, lt, nlinks);
    for (int pass = 0; pass < 2; pass++) {
        fprintf(f, "\n%s\n", pass ? "FOUND" : "MISSING");
        for (int i = 0; i < nlocs; i++) {
            snprintf(key, sizeof key, "pickup %d", locs[i].flag);
            if (known(key) != pass) continue;
            const struct loc *l = &locs[i];
            const char *how = !strcmp(l->kind, "search") ? (l->item == 7 ? "hidden on the ground (sparkles once you hold the Vision Staff)" : "sparkling spot on the ground")
                            : !strcmp(l->kind, "floor") ? "chest on a random floor" : "chest or item in the room";
            char what[40];
            if (l->fluorite) snprintf(what, sizeof what, "Fluorite #%d", l->fluorite); else snprintf(what, sizeof what, "%s", l->name);
            char where[48];
            if (!strcmp(l->kind, "floor")) snprintf(where, sizeof where, "%s", l->room); else snprintf(where, sizeof where, "room %s", l->room);
            fprintf(f, "  %-22s %-30s %s (pickup flag %d)\n", what, where, how, l->flag);
        }
    }
    fprintf(f, "\nROOMS NOT VISITED\n ");
    for (int i = 0; i < nrooms; i++) { snprintf(key, sizeof key, "visit %s", rooms[i]); if (!known(key)) fprintf(f, " %s", rooms[i]); }
    fprintf(f, "\n\nDOOR LINKS NOT TAKEN (from>to)\n ");
    for (int i = 0, n = 0; i < nlinks; i++) if (!known(links[i])) fprintf(f, "%s%s", ++n % 12 ? " " : "\n  ", links[i]);
    fprintf(f, "\n");
    fclose(f);
}

void mapping_init(void)
{
    host_path(dir, L"mapping");
    CreateDirectoryW(dir, 0);
    static const wchar_t *const names[F_COUNT] = {L"items.txt", L"rooms.txt"};
    static const char *const heads[F_COUNT] = {
        "; Items and flags as first seen in play, once each, adding up over launches (mapping.c). key, room, what.\n",
        "; Rooms as first seen in play, once each, adding up over launches (mapping.c). visit ROOM, or FROM>TO, camera.\n"};
    for (int i = 0; i < F_COUNT; i++) {
        wchar_t p[MAX_PATH];
        char line[256];
        file_path(p, names[i]);
        FILE *old = _wfopen(p, L"r");
        while (old && fgets(line, sizeof line, old)) {
            char *tab = strchr(line, '\t');
            if (line[0] != ';' && tab) { *tab = 0; keyset_add(keys, 1 << 14, &nkeys, fnv64_str(FNV64_START, line)); }
        }
        if (old) fclose(old);
        if ((out[i] = _wfopen(p, L"a")) && !old) fputs(heads[i], out[i]);
    }
    load_expected();
    write_checklist();
}

// Once a quarter second in play (host.c game_frame, on ach_frame's checks of the game state)
void mapping_frame(void)
{
    enum { INV = (G_FILES + 20 * 4 - G_INVENTORY) / 4 };
    static uint32_t inv[INV], got[10], story[8], doors[2];
    static int have, last = -1;
    static ULONGLONG seen_ms;   // ponytail: play unseen for 20 s (the title, menus) breaks the chain of rooms
    char room[8], key[32];
    int now = *(volatile uint16_t *)G_STAGE * 100 + *(volatile uint16_t *)G_ROOM;
    snprintf(room, sizeof room, "%03d", now);
    const volatile uint32_t *ni = (const volatile uint32_t *)G_INVENTORY;
    int changed = 0;
    for (int i = 0; i < INV; i++) changed += (ni[i] & 0xffff) != (inv[i] & 0xffff);
    for (int i = 0; i < 10; i++) changed += *(volatile uint32_t *)(G_ITEMGOT + 4 * i) != got[i];
    int loaded = !have || changed > 4;   // a save loaded: take it as it is
    if (have && loaded) xlog("[map] room %s: %d entries changed at once (a save loaded, or Kaede swapping in): taken as they are", room, changed);
    // rooms
    snprintf(key, sizeof key, "visit %s", room);
    if (!known(key)) note(F_ROOMS, key, "first visit");
    ULONGLONG ms = GetTickCount64();
    if (ms - seen_ms > 20000) last = -1;
    seen_ms = ms;
    if (last >= 0 && last != now && !loaded) {
        snprintf(key, sizeof key, "%03d>%s", last, room);
        note(F_ROOMS, key, "camera %d", *(volatile uint16_t *)G_CUT);
    }
    last = now;
    // items held: the first time each id shows up anywhere in the inventory, maps or documents
    for (int i = 0; i < INV; i++) {
        uint32_t v = ni[i];
        int id = v & 0xff;
        if (id && (v & 0xffff) != (inv[i] & 0xffff) && !loaded) {   // a loaded save's items: not here, not now
            snprintf(key, sizeof key, "got %02x", id);
            note(F_ITEMS, key, "%s\t%s", room, item_name(id));
        }
        inv[i] = v;
    }
    // bits: pickups (found even from a loaded save), story flags and door unlocks (only as seen set in play)
    static const struct { uint32_t at; int n; const char *name; uint32_t *last; } sets[] = {
        {G_ITEMGOT, 10, "pickup", got}, {G_STORY, 8, "story", story}, {G_DOORS, 2, "door", doors}};
    for (int k = 0; k < 3; k++)
        for (int w = 0; w < sets[k].n; w++) {
            uint32_t v = *(volatile uint32_t *)(sets[k].at + 4 * w), d = v & ~sets[k].last[w];
            for (int b = 0; d && b < 32; b++)   // MSB first: bit n is mask 0x80000000 >> (n & 31)
                if (d >> (31 - b) & 1) {
                    snprintf(key, sizeof key, "%s %d", sets[k].name, w * 32 + b);
                    if (!loaded) note(F_ITEMS, key, "%s\tset", room);
                    else if (k == 0) note(F_ITEMS, key, "(in a loaded save)\tset");
                }
            sets[k].last[w] = v;
        }
    have = 1;
    if (dirty) { dirty = 0; write_checklist(); }
}
