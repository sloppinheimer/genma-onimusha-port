// First-seen feature report.
//
// Code paths call xfeature() with a short description of what the game is using right now (a texture format,
// a texture-stage op, a render state we ignore, an unimplemented call...). The first time a description is
// seen, it is appended to logs\features.log and logged. The file persists across runs and is
// read back at startup, so each session only reports what no earlier session hit.
// Entries marked '!' are things the port doesn't fully handle; for those the frame is also saved as
// new_<frame>.bmp, so you can see where it happened.
#include "host.h"
#include "game.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum { SLOTS = 16384 };
static uint64_t seen[SLOTS];
static SRWLOCK lock = SRWLOCK_INIT;
static FILE *out;
static volatile LONG shot_pending, shots_taken;

static uint32_t nseen;
static uint64_t hash(const char *s) { return fnv64_str(FNV64_START, s); }
static int insert(uint64_t h) { return keyset_add(seen, SLOTS, &nseen, h); }   // 1: new (full: stops reporting)

void report_init(const wchar_t *root)
{
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%ls\\logs\\features.log", root);
    FILE *f = _wfopen(path, L"r");
    char line[512];
    int n = 0;
    while (f && fgets(line, sizeof line, f)) {   // "<!|.> <key>\t..."
        char *tab = strchr(line, '\t');
        if (tab && line[0] && line[1] == ' ') { *tab = 0; insert(hash(line + 2)); n++; }
    }
    if (f) fclose(f);
    out = _wfopen(path, L"a");
    if (out) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(out, "# session %04u-%02u-%02u %02u:%02u (%d features already known)\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, n);
        fflush(out);
    }
}

void xfeature(int unsupported, const char *fmt, ...)
{
    char key[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(key, sizeof key, fmt, ap);
    va_end(ap);
    for (char *p = key; *p; p++) if (*p == '\t' || *p == '\n') *p = ' ';
    uint64_t h = hash(key);
    AcquireSRWLockExclusive(&lock);
    int is_new = insert(h);
    if (is_new && out) {
        // the game's current stage/room (0x546AE8/0x546AEA; 0xFFFF before a room is loaded)
        uint16_t stage = *(volatile uint16_t *)G_STAGE, room = *(volatile uint16_t *)G_ROOM;
        if (room == 0xFFFF) fprintf(out, "%c %s\tframe %u, menus\n", unsupported ? '!' : '.', key, d3d_frame_number());
        else fprintf(out, "%c %s\tframe %u, room %u%02u\n", unsupported ? '!' : '.', key, d3d_frame_number(), stage, room);
        fflush(out);
    }
    ReleaseSRWLockExclusive(&lock);
    if (!is_new) return;
    xlog("[new%s] %s", unsupported ? " !" : "", key);
    if (unsupported && shots_taken < 200) InterlockedExchange(&shot_pending, 1);
}

// Called once per frame by the renderer: should this frame be saved?
int report_take_shot(void)
{
    if (!InterlockedExchange(&shot_pending, 0)) return 0;
    InterlockedIncrement(&shots_taken);
    return 1;
}
