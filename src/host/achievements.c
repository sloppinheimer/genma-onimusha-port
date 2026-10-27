// Achievements (achievements_list.h). Unlocks are kept for the whole install in saves\achievements.dat, one
// "id = unix time" line each, sealed (layout.h: not readable or editable by hand); the launcher reads the same
// file. The game's memory is checked a few times a second while play is running, so loading an old save awards
// what it already qualifies for. Each unlock queues a popup (d3d_overlay.c draws it) unless onimusha.ini [game]
// achievement_popups = 0. Nothing unlocks while a cheat is on (cheats.c)
// or in a game that was saved with one on (see clean runs, below).
#include "host.h"
#include "game.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "layout.h"   // sealed_read, sealed_write

#define X(id, name, desc) #id,
static const char *const ach_ids[ACH_COUNT] = {ACHIEVEMENTS(X)};
#undef X
#define X(id, name, desc) name,
static const char *const ach_names[ACH_COUNT] = {ACHIEVEMENTS(X)};
#undef X

static long long unlocked[ACH_COUNT];   // unix time, 0 = locked
static wchar_t path[MAX_PATH], runs_path[MAX_PATH];
static void runs_load(void);
static void save_pending(void);
static int read_sealed(const wchar_t *p, char *buf, size_t n);
static int queue[ACH_COUNT], q_head, q_tail;   // popups waiting; each achievement unlocks once, so it never overflows
static CRITICAL_SECTION lock;                  // ach_unlock may come from a game thread (hooks), the popup from Present
static int cheated;                            // the game being played had cheats on at some point

void ach_init(const wchar_t *root)
{
    InitializeCriticalSection(&lock);
    swprintf(path, MAX_PATH, L"%ls\\saves\\achievements.dat", root);
    swprintf(runs_path, MAX_PATH, L"%ls\\saves\\achievement_runs.dat", root);
    runs_load();
    atexit(save_pending);
    static char text[ACH_COUNT * 64 + 256];
    if (!read_sealed(path, text, sizeof text)) return;
    char id[64];
    long long t;
    int n = 0;
    for (char *line = text, *next; line; line = next) {
        if ((next = strchr(line, '\n'))) *next++ = 0;
        if (sscanf(line, " %63[a-z0-9_] = %lld", id, &t) == 2)
            for (int i = 0; i < ACH_COUNT; i++) if (!strcmp(id, ach_ids[i])) { unlocked[i] = t; n++; }
    }
    xlog("[ach] %d of %d unlocked", n, ACH_COUNT);
}

// A sealed file into buf: 1 when there is one. One that was edited (or damaged) is kept as <name>.bad and reads as empty.
static int read_sealed(const wchar_t *p, char *buf, size_t n)
{
    int r = sealed_read(p, buf, n);
    if (r == -2) {
        wchar_t bad[MAX_PATH + 8];
        swprintf(bad, MAX_PATH + 8, L"%ls.bad", p);
        MoveFileExW(p, bad, MOVEFILE_REPLACE_EXISTING);
        xlog("[ach] %ls was edited or is damaged: kept as .bad, starting from none", p);
    }
    return r >= 0;
}

// An unlock whose write failed (the file held open by the launcher or a virus scanner at that moment) is tried again
// every few seconds and at exit: event-only ones (Soul Thief, the results-screen checks) wouldn't come back otherwise.
static volatile int unsaved;
static void save(void)
{
    char buf[ACH_COUNT * 64 + 128];
    int n = snprintf(buf, sizeof buf, "; Unlocked achievements: id = when (unix time).\n");
    for (int i = 0; i < ACH_COUNT; i++) if (unlocked[i]) n += snprintf(buf + n, sizeof buf - n, "%s = %lld\n", ach_ids[i], unlocked[i]);
    int ok = sealed_write(path, buf, n);
    if (!ok && !unsaved) xlog("[ach] couldn't write achievements.dat: trying again");
    if (ok && unsaved) xlog("[ach] achievements.dat written");
    unsaved = !ok;
}
static void save_pending(void)   // also at exit: try only, the lock's holder may be gone
{
    if (unsaved && TryEnterCriticalSection(&lock)) { if (unsaved) save(); LeaveCriticalSection(&lock); }
}

void ach_unlock(int a)
{
    if (a < 0 || a >= ACH_COUNT || cheats_any() || cheated || rando_on()) return;   // none in a randomizer run
    EnterCriticalSection(&lock);
    if (!unlocked[a]) {
        unlocked[a] = (long long)time(0);
        save();
        if (settings.ach_popups) queue[q_tail++ % ACH_COUNT] = a;
        xlog("[ach] unlocked %s (%s)", ach_ids[a], ach_names[a]);
    }
    LeaveCriticalSection(&lock);
}

int ach_popup_pending(void) { return q_head != q_tail; }   // ponytail: unlocked read, a hint only

const char *ach_popup_next(void)
{
    const char *s = 0;
    EnterCriticalSection(&lock);
    if (q_head != q_tail) s = ach_names[queue[q_head++ % ACH_COUNT]];
    LeaveCriticalSection(&lock);
    return s;
}

// ---- checks against game memory (addresses in the Xbox image; docs/mods.md) ------------------------------------
// Flag arrays are MSB-first bitsets (the game's FUN_0016c6a0 sets, FUN_0016c710 tests): bit n is in the dword at
// base + 4 * (n >> 5), mask 0x80000000 >> (n & 31). Arrays the room scripts use (pointer table 0x2204b8):

#define G_UNLOCKS 0x005468b8u     // the save file's shared unlock bits, read at boot (see check_unlocks)
#define G_ONI_CLEARED 0x005531ceu // u16, non-zero once all 12 Oni Spirits stages are cleared this time
#define G_TIMER 0x00546bbcu       // play time, frames at 60/s
#define G_DARK_BEST 0x00546bc5u   // Dark Realm: highest floor reached, 1-based (20 floors)
#define G_DIFFICULTY 0x00547220u  // u16: 0 Easy, 1 Normal, 2 Hard Mode
#define G_RANK 0x00546824u        // results: 0 S, 1 A, 2 B, 3 C, 4 D, 5 QUICK, 6 MASTER, 7 ONIMUSHA

#define G_SAMA_MAX_HP 0x005471f6u     // Samanosuke's own maximum health, magic and gauntlet level while Kaede plays
#define G_SAMA_MAX_MAGIC 0x005471fau
#define G_SAMA_GAUNTLET 0x005471f3u
#define STAT_MAX 0xf8             // 0x80 plus five jewels of 0x18
#define G_SOULS_TOTAL 0x00547218u // u32, every red soul ever absorbed (the results screen's tally; never goes down)
#define G_GAUNTLET 0x005471efu    // gauntlet level 0-2, raised at a Magic Mirror once total souls (0x547218) reach 20000
                                  // / 50000 (0x1fec60/64; the mirror's caseD_1, 0xe01c3)

static int flag(uint32_t base, int n) { return *(volatile uint32_t *)(base + 4 * (n >> 5)) >> (31 - (n & 31)) & 1; }
static int has_item(int id) { return item_count(id) != 0; }
static int all_entries(uint32_t at, int n)   // every entry of a list holds an item
{
    for (int i = 0; i < n; i++) if (!*(volatile uint8_t *)(at + 4 * i)) return 0;
    return 1;
}

// ---- clean runs: "no herbs or medicine" needs the whole run watched, and the game keeps no count of it -----------
// The port watches the Herb and Medicine counts and keeps, per save, whether the run so far has used one: each time
// the game saves (its save count goes up) the play time at that moment is recorded with it in achievement_runs.dat.
// Loading a save puts that exact play time back, which finds the record again. A save the port never saw being made
// is unknown and can't count; a new game starts clean. Cheats are kept the same way: a save made with one on is marked
// cheated, and so is every later save of that game, cheats on or not. Saving a clean game over it records that one.
#define G_SAVE_COUNT 0x00546bb8u  // u16, +1 each time the game is saved
static struct { uint32_t timer; int healed, cheated; } recs[256];   // ponytail: the latest 256 saves are remembered
static int nrecs;
// The save header's unlock bits (G_UNLOCKS) are shared by every save, so a cheated clear's bits would unlock their
// achievements in any later game. Bits seen first while cheating are kept as tainted (in achievement_runs.dat) and
// never award; bits seen first in a clean game are clean. A bit the header no longer has (a new save file) is
// forgotten. Clean clears still award through the results screen's own checks.
static uint32_t bits_taint, bits_clean;
static struct { int have, healed, who, herb, med; uint32_t timer; uint16_t saves; } run;   // healed: or unknown

static void runs_load(void)
{
    static char text[256 * 32 + 384];
    if (!read_sealed(runs_path, text, sizeof text)) return;
    for (char *line = text, *next; line && nrecs < 256; line = next) {
        if ((next = strchr(line, '\n'))) *next++ = 0;
        if (sscanf(line, "unlock_bits %x %x", &bits_taint, &bits_clean) == 2) continue;
        else if (sscanf(line, "%u %d %d", &recs[nrecs].timer, &recs[nrecs].healed, &recs[nrecs].cheated) >= 2)   // older lines: no cheats
            recs[nrecs].healed = !!recs[nrecs].healed, recs[nrecs].cheated = !!recs[nrecs].cheated, nrecs++;   // 0/1: runs_write's buffer fits
    }
}

static void runs_write(void)
{
    static char buf[256 * 32 + 384];
    int n = snprintf(buf, sizeof buf, "; Per save (play time in frames when it was saved): 1 if Herbs or Medicine were used by then, or not known;\n"
                                      "; then 1 if cheats were on in that game. unlock_bits: the save header's unlock bits first seen with\n"
                                      "; cheats on (they unlock nothing), then those first seen without.\nunlock_bits %x %x\n", bits_taint, bits_clean);
    for (int k = 0; k < nrecs; k++) n += snprintf(buf + n, sizeof buf - n, "%u %d %d\n", recs[k].timer, recs[k].healed, recs[k].cheated);
    if (!sealed_write(runs_path, buf, n)) xlog("[ach] couldn't write achievement_runs.dat");
}
static void runs_record(uint32_t timer, int healed, int cheat)
{
    int i = 0;
    while (i < nrecs && recs[i].timer != timer) i++;
    if (i == nrecs) {
        if (nrecs == 256) memmove(recs, recs + 1, sizeof recs - sizeof *recs), nrecs--;   // drop the oldest
        i = nrecs++;
    }
    recs[i].timer = timer; recs[i].healed = healed; recs[i].cheated = cheat;
    runs_write();
}

static void track_run(void)
{
    uint32_t t = *(volatile uint32_t *)G_TIMER;
    uint16_t saves = *(volatile uint16_t *)G_SAVE_COUNT;
    int who = *(volatile uint8_t *)G_PLAYER_ID, herb = item_count(0x01), med = item_count(0x02);
    if (!run.have || t < run.timer || t > run.timer + 600) {   // a new game or a loaded save (or the port just started)
        int i = nrecs;   // the clock runs on for a few frames after a load before this sees it
        while (i-- > 0 && !(recs[i].timer <= t && t - recs[i].timer <= 60)) {}
        run.healed = i >= 0 ? recs[i].healed : t >= 600;   // under 10 s of play without a record: a new game
        cheated = i >= 0 && recs[i].cheated;
        if (cheated) xlog("[ach] this save was made with cheats on: no achievements");
        run.have = 1; run.saves = saves;
    } else {
        // the Magic Mirror turns Herbs into a Medicine, and pickups add: neither is a use. Kaede has her own items.
        if (who == run.who && (med < run.med || (herb < run.herb && med <= run.med))) {
            if (!run.healed) xlog("[ach] Herbs or Medicine used (%d/%d -> %d/%d): no clean run", run.herb, run.med, herb, med);
            run.healed = 1;
        }
        if (saves != run.saves) { runs_record(t, run.healed, cheated); run.saves = saves; }
    }
    if (cheats_any()) cheated = 1;
    run.timer = t; run.who = who; run.herb = herb; run.med = med;
}

// The unlock bits the game keeps in the save file's header (set on the results screens, OR-ed in at boot and on
// loading): each one means an achievement was earned on a past run, possibly before the port tracked it.
static void check_unlocks(void)
{
    // first the game being played (and so whether its save was cheated), before anything can unlock
    if (*(volatile uint32_t *)G_MODE_FLAGS & 8) run.have = 0, cheated = 0;   // Oni Spirits resets the play time: pick the story
    else track_run();                                                   // up again after. Its own kit: no save's cheats
    uint32_t u = *(volatile uint32_t *)G_UNLOCKS, was_t = bits_taint, was_c = bits_clean;
    if (u) {   // 0 until the game has read the save header: nothing to learn, and nothing to forget
        if (cheats_any() || cheated) bits_taint |= u & ~bits_clean;
        else bits_clean |= u & ~bits_taint;
        bits_taint &= u; bits_clean &= u;
    }
    if (bits_taint != was_t || bits_clean != was_c) {
        if (bits_taint & ~was_t) xlog("[ach] unlock bits %x appeared with cheats on: they unlock nothing", bits_taint & ~was_t);
        runs_write();
    }
    u &= ~bits_taint;
    if (u & 0x04) ach_unlock(ACH_oni_spirits);       // all of Oni Spirits cleared
    if (u & 0x08) ach_unlock(ACH_costume_samanosuke), ach_unlock(ACH_clear_hardest), ach_unlock(ACH_clear_rank_s);   // Hard clear, S/ONIMUSHA
    if (u & 0x10) ach_unlock(ACH_costume_kaede);     // a clear holding 30 Fluorite
    if (u & 0x40) ach_unlock(ACH_clear_fast);        // a clear within 3 hours
    if (u & 0x80) ach_unlock(ACH_clear_hardest);     // a Hard Mode clear
    if (*(volatile uint16_t *)G_ONI_CLEARED) ach_unlock(ACH_oni_spirits);
    const volatile uint8_t *s = (const volatile uint8_t *)G_STATE;
    if (s[0] == 0x0a) ach_unlock(ACH_boss_fortinbras);   // the clear results: only after the last boss
    if (s[0] == 0x0a && s[1] == 3 && s[2] >= 8) {    // the results screen, rank worked out
        if (*(volatile uint16_t *)G_DIFFICULTY == 2) ach_unlock(ACH_clear_hardest);
        if (*(volatile uint32_t *)G_TIMER <= 3 * 60 * 60 * 60) ach_unlock(ACH_clear_fast);
        uint8_t rank = *(volatile uint8_t *)G_RANK;
        if (rank == 0 || rank == 7) ach_unlock(ACH_clear_rank_s);   // ONIMUSHA ranks above S
        if (*(volatile uint16_t *)G_DIFFICULTY >= 1) {   // Normal (or Hard)
            if (!run.healed) ach_unlock(ACH_clear_no_heal);
            // No enhancing and no jewels needs no watching: only the Magic Mirror raises a sword or orb above 0 and
            // only jewels raise Samanosuke's maximum health and magic above 0x80
            const volatile uint8_t *lv = (const volatile uint8_t *)G_LEVELS;
            int plain = 1, kaede = *(volatile uint8_t *)G_PLAYER_ID == 1;
            for (int i = 0; i < 6; i++) if (lv[i] != 0 && lv[i] != 0xff) plain = 0;
            if (*(volatile uint16_t *)(kaede ? G_SAMA_MAX_HP : G_MAX_HP) > 0x80 || *(volatile uint16_t *)(kaede ? G_SAMA_MAX_MAGIC : G_MAX_MAGIC) > 0x80) plain = 0;
            if (plain) ach_unlock(ACH_clear_no_upgrade);
        }
    }
}

static void check(void)
{
    static int env_done;
    if (!env_done) {   // debug: ONI_ACH=<id> unlocks that achievement as soon as play starts (to see the popup)
        char v[64] = "";
        env_done = 1;
        if (debug_env("ONI_ACH", v, sizeof v))
            for (int i = 0; i < ACH_COUNT; i++) if (!strcmp(v, ach_ids[i])) ach_unlock(i);
    }
    // Item ids by the game's own name table (0x1fffa0: {name id, slot, ..}); pickup bits from the room scripts. Checked
    // against an early save and a full run's save slots.
    if (*(volatile uint32_t *)G_SOULS_TOTAL) ach_unlock(ACH_soul_absorb);
    const volatile uint8_t *orb = (const volatile uint8_t *)G_LEVELS;   // 0xff until the orb is found
    if (orb[0] != 0xff && has_item(0x11)) ach_unlock(ACH_shiden);   // and the orb itself held (Thunder, Fire, Wind)
    if (orb[1] != 0xff && has_item(0x12)) ach_unlock(ACH_kouen);
    if (orb[2] != 0xff && has_item(0x13)) ach_unlock(ACH_arashi);
    if (*(volatile uint8_t *)G_STAGE == 6 || flag(G_DOORS, 58)) ach_unlock(ACH_demon_gate);   // the crank door, room 220 -> 600
    if ((has_item(0x15) || flag(G_ITEMGOT, 117)) && (has_item(0x16) || flag(G_ITEMGOT, 24) || flag(G_ITEMGOT, 181)))
        ach_unlock(ACH_ranged_all);   // bow, Matchlock
    if (has_item(0x1b) && has_item(0x1c) && has_item(0x1d)) ach_unlock(ACH_armor_all);   // ponytail: Ogre Armor (0x21) not required
    if (has_item(0x14) || flag(G_ITEMGOT, 103)) ach_unlock(ACH_bishamon);      // Bishamon Sword
    if (has_item(0x42) || flag(G_ITEMGOT, 59)) ach_unlock(ACH_rosary);         // Rosary of Com.
    if (has_item(0x20) || flag(G_ITEMGOT, 77)) ach_unlock(ACH_sacred_knife);   // Sacred Knife (Kaede's)
    if (has_item(0x4f) || flag(G_ITEMGOT, 35)) ach_unlock(ACH_vision_staff);   // Vision Staff
    if (all_entries(G_MAPS, 8)) ach_unlock(ACH_maps_all);
    if (*(volatile int8_t *)G_DARK_BEST >= 11) ach_unlock(ACH_dark_realm_1);   // ponytail: "half" read as 10 of 20 floors
    if (*(volatile int8_t *)G_DARK_BEST >= 20) ach_unlock(ACH_dark_realm_2);
    {   // gear. An orb or sword is level 0 when picked up (the save's slots show it); only the Magic Mirror raises it
        const volatile uint8_t *lv = (const volatile uint8_t *)G_LEVELS;
        int orbs = 1, swords = 1;
        for (int i = 0; i < 6; i++) {
            if (lv[i] == 1 || lv[i] == 2) ach_unlock(ACH_enhance_any);
            if (lv[i] != 2) *(i < 3 ? &orbs : &swords) = 0;
        }
        if (orbs) ach_unlock(ACH_orbs_max);
        if (swords) ach_unlock(ACH_swords_max);
        int kaede = *(volatile uint8_t *)G_PLAYER_ID == 1;
        if (*(volatile uint16_t *)(kaede ? G_SAMA_MAX_HP : G_MAX_HP) >= STAT_MAX) ach_unlock(ACH_vitality_max);
        if (*(volatile uint16_t *)(kaede ? G_SAMA_MAX_MAGIC : G_MAX_MAGIC) >= STAT_MAX) ach_unlock(ACH_magic_max);
        if (*(volatile uint8_t *)(kaede ? G_SAMA_GAUNTLET : G_GAUNTLET) == 2) ach_unlock(ACH_gauntlet_max);
    }
    if (all_entries(G_FILES, 20)) {   // and every page of the four documents
        int pages = 1;
        for (int i = 16; i < 20; i++) pages &= (*(volatile uint8_t *)(G_FILES + 4 * i + 2) & 0x0f) == 0x0f;
        if (pages) ach_unlock(ACH_files_all);
    }
    // Story events: story flags set by a boss's death state or by the room script, matched to rooms through the
    // walkthroughs and checked against the order they appear in a full run's save slots.
    static const struct { int ach, bit; } story[] = {
        {ACH_boss_osric, 25}, {ACH_boss_marcellus, 154}, {ACH_boss_stylado, 61}, {ACH_boss_hecuba, 82},
        {ACH_boss_marcellus2, 92}, {ACH_boss_fortinbras, 98},
        {ACH_crest_puzzle, 145},      // the Saito crest panel, rooms 521/522
        {ACH_abandoned_keep, 115},    // first entry to room 500
        {ACH_keep_underground, 126},  // the room 504 shrine fence, after its three crystals
        {ACH_east_area, 28},          // the Decorated Sword set in the room 200 pedestal
        {ACH_soldier_saved, 195},     // room 500
    };
    for (size_t i = 0; i < sizeof story / sizeof *story; i++) if (flag(G_STORY, story[i].bit)) ach_unlock(story[i].ach);
    if (*(volatile uint8_t *)0x546de0u & 1) ach_unlock(ACH_boss_barabazuu);   // room 220's boss group killed (room flags)
    if (flag(G_STORY, 118) && flag(G_STORY, 56)) ach_unlock(ACH_moth_men);  // the two Dark Realm guides, rooms 501 / 235
    if (flag(G_ITEMGOT, 2) && flag(G_ITEMGOT, 27) && flag(G_ITEMGOT, 57) && flag(G_ITEMGOT, 99))
        ach_unlock(ACH_decoder_boxes);   // the boxes' rewards, rooms 005, 125, 222, 311
    if (item_count(0x07) >= 30) ach_unlock(ACH_fluorite_all);   // Genma has 30 (the Kaede Extra reward asks for all)
    static const int number_boxes[] = {117, 129, 70, 77, 35, 24, 20, 0};   // the 8 sliding-number boxes' rewards: rooms
    int boxes = 1;                                                         // 116 510 233 238 123 310 119 241
    for (size_t i = 0; i < sizeof number_boxes / sizeof *number_boxes; i++) boxes &= flag(G_ITEMGOT, number_boxes[i]);
    if (boxes) ach_unlock(ACH_number_puzzles);   // ponytail: box 241's bit is shared with a Talisman pickup in room 518
    // Genma's own. The Secret key opens the Tower of Ogres, where the Ogre Armor is won; the key is used up, the armor
    // isn't. (Soul Thief is watched every frame: track_tug.)
    if (has_item(0x34) || has_item(0x21)) ach_unlock(ACH_secret_key);
    if (has_item(0x21)) ach_unlock(ACH_ogre_armor);
    // Ayame (enemy type 0x1d) appears in 8 episodes, each a chain of rooms she follows you through. A story flag
    // (199-206) marks an episode over, killed or fled (FUN_000a1d00, table 0x1f7c90); the room's enemy-group flag
    // (room flags, 0x546c0c + 8 per room) is set only by a kill. Both: she was beaten there.
    static const struct { int bit; uint32_t at; uint8_t mask; } ayame[] = {
        {199, 0x547018, 0x01}, {199, 0x546fe8, 0x03}, {200, 0x546fb0, 0x01}, {201, 0x547010, 0x03}, {201, 0x547050, 0x03},
        {201, 0x546ff0, 0x40}, {202, 0x547068, 0x01}, {203, 0x546ce8, 0x01}, {204, 0x546d70, 0x01}, {205, 0x546eb8, 0x01},
        {205, 0x546ec0, 0x01}, {205, 0x546ec7, 0x80}, {206, 0x546f38, 0x01},
    };   // rooms 516 510 503 515 523 511 526 117 206 301 302 303 317
    for (size_t i = 0; i < sizeof ayame / sizeof *ayame; i++)
        if (flag(G_STORY, ayame[i].bit) && (*(volatile uint8_t *)ayame[i].at & ayame[i].mask)) ach_unlock(ACH_ayame);
    // ponytail: no check yet, so these ten can't unlock: the combat moments (special_kill, kaede_special,
    // deflect_marcellus, waterfall_kill, ram_kill, power_struggle; they leave no flag, the combat code needs hooking)
    // and the scenery ones (geese, beetle, carp_soul, bamboo)
}

// Soul Thief: a green soul won from a demon. While a demon pulls a soul towards itself the game sets 0x4a0870 for
// that frame (FUN_00104d10; the soul manager plays the tug sound from it, FUN_00104cb0 clears it each frame). Winning
// = the player gains a green charge (0x5473f4, 0-5) or tops up a running oni mode (0x5531bc counts down; an absorb
// adds 0x78, FUN_00110720) within 2 s of a frame with a demon pulling. Every frame: the pull flag lasts one frame.
static void track_tug(void)
{
    static unsigned frame, last_pull = ~0u;
    static uint32_t charges;
    static uint16_t oni;
    uint32_t c = *(volatile uint32_t *)G_ONI_CHARGES;
    uint16_t t = *(volatile uint16_t *)G_ONI_TIME;
    frame++;
    if (*(volatile uint32_t *)0x4a0870u) last_pull = frame;
    if ((c > charges || t > oni) && last_pull != ~0u && frame - last_pull < 120) ach_unlock(ACH_soul_thief);
    charges = c; oni = t;
}

int ach_frame(void)
{
    static unsigned frames;
    static uint8_t tick;
    track_tug();
    if (++frames % 15) return 0;   // 4 times a second is plenty
    if (unsaved && frames % 300 == 0) save_pending();
    // Not in a randomizer run: its saves and the header bits it sets would go into the normal game's
    // achievement_runs.dat, pushing out (or tainting) the records of the normal game's saves.
    if (!rando_on()) check_unlocks();
    uint8_t t = *(volatile uint8_t *)G_PLAY_TICK;
    if (t == tick) return 0;     // not in play: memory may not hold a loaded game yet
    tick = t;
    if (*(volatile uint32_t *)G_MODE_FLAGS & 8) return 0;   // Oni Spirits: its own kit, not the story's progress
    // only in the states the play clock runs in (play, cutscenes, the item menus): going back to the title after a
    // game over clears the game's memory and can still move the tick once
    uint8_t s = *(volatile uint8_t *)G_STATE;
    if (s != 2 && s != 6 && s != 8 && s != 0x0b) return 0;
    check();
    return 1;
}
