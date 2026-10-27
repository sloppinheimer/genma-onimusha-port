// Cheats (onimusha.ini [cheats]). The addresses and functions are in docs/mods.md, "Stats and damage". While any
// cheat is on no achievements unlock, and the saves made then are marked as cheated (achievements.c).
#include "host.h"
#include "game.h"
#include <string.h>
#include "patch.h"

#define G_MAGIC 0x005473ceu       // s16 per orb: Thunder, Fire, Wind
#define G_SOULS 0x00547214u       // u32, red souls to spend at the Magic Mirror (the lifetime total is 0x547218)

#define FULL 99                   // the most of an item the game keeps (FUN_0015ebd0)

int cheats_any(void)
{
    for (int i = 0; i < CHEAT_COUNT; i++) if (settings.cheat[i]) return 1;
    return 0;
}

// Points the call at c (a CALL rel32) to to, if it still calls from (patch.h). 0 if the code isn't as expected.
static int redirect(uint32_t c, uint32_t from, const void *to)
{
    if (patch_call(c, from, to)) return 1;
    xlog("[cheats] call at %06x moved: skipped", c);
    return 0;
}

// Vision Staff: a room's hidden Fluorite spots sparkle only if FUN_0015e7f0(0x4f), the Vision Staff's inventory slot,
// isn't -1 (FUN_00155ea0 as the room loads, FUN_001540b0 for a trigger made into a spot; docs/randomizer.md).
static int __cdecl vision_slot(int id)
{
    xlog("[cheats] vision_staff: a hidden Fluorite sparkles (room %03u)", game_room());
    return id == 0x4f ? 0 : ((int(__cdecl *)(int))0x15e7f0)(id);
}

// One hit kill: FUN_000a60c0(enemy, hit) works out what a hit takes off an enemy's health; its callers subtract it.
// 0x3fff takes any enemy below 0 without the short wrapping round, with Onimusha's 43% on top.
static int __cdecl one_hit(uint32_t enemy, uint32_t hit)
{
    int d = ((int(__cdecl *)(uint32_t, uint32_t))0xa60c0)(enemy, hit);
    return (int16_t)d > 0 ? 0x3fff : d;
}

void cheats_install(void)
{
    if (!cheats_any()) return;
    // God mode: the writes of a hit's damage to health become nops. Other health loss (a few scripted ones) is put
    // back each frame below; these two can be bigger than what is left, and death is checked in the same frame.
    static const struct { uint32_t at; uint8_t n, was[7]; } nop[] = {
        {0x108ad1, 7, {0x66, 0x89, 0x1d, 0x28, 0x83, 0x54, 0x00}},   // FUN_00108a50: an enemy's hit
        {0x0847aa, 6, {0x66, 0xa3, 0x28, 0x83, 0x54, 0x00}},         // FUN_00084580: a trap's
        {0x0847be, 7, {0x66, 0x89, 0x1d, 0x28, 0x83, 0x54, 0x00}},   // and its floor at 0
    };
    if (settings.cheat[CHEAT_GOD]) {
        size_t i = 0;
        while (i < sizeof nop / sizeof *nop && !memcmp((void *)nop[i].at, nop[i].was, nop[i].n)) i++;
        if (i < sizeof nop / sizeof *nop) { xlog("[cheats] code at %06x is not as expected: god_mode only refills health", nop[i].at); }
        else for (i = 0; i < sizeof nop / sizeof *nop; i++) memset((void *)nop[i].at, 0x90, nop[i].n);
    }
    static const uint32_t calls[] = {0x0a66ae, 0x0a6cac, 0x0a6dcc, 0x0a6d3c, 0x086c79, 0x086f23, 0x03c201, 0x03c40b};
    if (settings.cheat[CHEAT_ONE_HIT]) {
        int n = 0;
        for (size_t i = 0; i < sizeof calls / sizeof *calls; i++) n += redirect(calls[i], 0xa60c0, one_hit);
        xlog("[cheats] one_hit_kill: %d of %d damage calls", n, (int)(sizeof calls / sizeof *calls));
    }
    if (settings.cheat[CHEAT_VISION]) {   // the CALLs right after each PUSH 0x4f
        int n = redirect(0x155fa8, 0x15e7f0, vision_slot) + redirect(0x15411e, 0x15e7f0, vision_slot);
        xlog("[cheats] vision_staff: %d of 2 checks", n);
    }
    for (int i = 0; i < CHEAT_COUNT; i++) if (settings.cheat[i]) xlog("[cheats] %s on: no achievements this session", cheat_names[i]);
}

// Each game step (not in menus, the title or the pause screen): top the values up.
void cheats_frame(void)
{
    static uint8_t tick;
    if (!cheats_any()) return;
    uint8_t t = *(volatile uint8_t *)G_PLAY_TICK;
    if (t == tick) return;
    tick = t;
    if (settings.cheat[CHEAT_GOD]) {
        volatile int16_t *hp = (volatile int16_t *)G_HP;
        int16_t max = *(volatile int16_t *)G_MAX_HP;
        if (*hp >= 0 && *hp < max) *hp = max;   // not once dying: that has started already
    }
    if (settings.cheat[CHEAT_MAGIC] && *(volatile uint8_t *)G_PLAYER_ID == 0)   // Kaede's maximum isn't Samanosuke's
        for (int i = 0; i < 3; i++) ((volatile int16_t *)G_MAGIC)[i] = *(volatile int16_t *)G_MAX_MAGIC;
    if (settings.cheat[CHEAT_SOULS]) *(volatile uint32_t *)G_SOULS = 999999;
    // The form runs out as usual (kept going, its healing made it a second god mode); once it has, the green souls are
    // full again.
    if (settings.cheat[CHEAT_ONIMUSHA] && !*(volatile uint16_t *)G_ONI_TIME) *(volatile uint32_t *)G_ONI_CHARGES = 5;
    if (settings.cheat[CHEAT_AMMO] || settings.cheat[CHEAT_ITEMS])
        for (uint32_t a = G_INVENTORY; a < G_MAPS; a += 4) {   // only what you carry; 0xff is a single item
            uint8_t id = *(volatile uint8_t *)a;
            volatile uint8_t *n = (volatile uint8_t *)(a + 1);
            int ammo = id >= 0x17 && id <= 0x1a, heal = id == 0x01 || id == 0x02;   // arrows, bullets; Herb, Medicine
            if (*n != 0xff && ((ammo && settings.cheat[CHEAT_AMMO]) || (heal && settings.cheat[CHEAT_ITEMS]))) *n = FULL;
        }
}
