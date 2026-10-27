// Game memory read by more than one file (addresses in the Xbox image; docs/mods.md). One name each: an address only
// one file reads stays there, with its context.
#pragma once
#include <stdint.h>

#define G_STATE 0x00543088u       // the mode word: main state, sub-state, step bytes. Low byte 2 play and pause, 0 title,
                                  // 6 the opening letter, 8 the menu, 7 the mirror's save screen, 0x0b its enhance
                                  // screen, 0x0a the clear results (rank) screen
#define G_MODE_FLAGS 0x0054365cu  // bit 3: Oni Spirits is running; bit 0x1000: the game zeroes its pad (FUN_00106bd0),
                                  // as in cutscenes and events
#define G_HP 0x00548328u          // s16, the character being played (Kaede too); below 0 is death
#define G_MAX_HP 0x00548326u      // u16, likewise (G_HP - 2)
#define G_MAX_MAGIC 0x00546bc6u   // u16, likewise
#define G_STAGE 0x00546ae8u       // stage; G_ROOM the room. Room files are roomSRR: stage S, room RR
#define G_ROOM 0x00546aeau
#define G_CUT 0x00546aecu         // u16, the room's camera cut (the shot)

#define G_STORY 0x00546be4u       // 256 permanent story flags
#define G_ITEMGOT 0x0054714cu     // one bit per item pickup in the world, 0-316 (rooms 0-208, the Dark Realm 224-254, the
                                  // Ogre Tower 288-316), kept after the item is used up
#define G_DOORS 0x005471b4u       // 64 door-unlocked bits
#define G_FILES 0x00547360u       // 20 entries after the maps: ids 0x90-0x9f, then documents 0xa0-0xa3 (+2 = pages found)
#define G_LEVELS 0x005471fcu      // 6 bytes: orb levels (thunder, fire, wind), then sword levels; 0-2, 0xff not owned
#define G_SWORD_LEVEL (G_LEVELS + 3)
#define G_ONI_TIME 0x005531bcu    // u16, frames of Onimusha left (0: not transformed)
#define G_ONI_CHARGES 0x005473f4u // u32, green souls towards the next transformation, 0-5
#define G_ITEM_TABLE 0x001fffa0u  // 8 bytes per item id: name id, inventory slot, ...
#define G_NAME_OFFS 0x00203a68u   // the English "names and labels" message list: 16-bit offsets into G_NAME_TEXT
#define G_NAME_TEXT 0x00202d80u
#define G_NAMES 167               // entries in it
#define G_SAVE_STATE 0x00546bb4u  // the game state a save slot holds starts here (slot + 0 = this address)

// save.dat (after its 20-byte signature, launcher/text.h xsave_sign): slots of SAVE_SLOT_SIZE bytes from SAVE_SLOT0, each
// the game state from G_SAVE_STATE. Word 0 is the sum of words 1..0x21e and word 0x21f their xor; the game checks both
// on loading (FUN_001493a0), and a slot never used is zeros.
#define SAVE_SLOT0 0x200
#define SAVE_SLOT_SIZE 0x880
static void save_slot_sums(const uint32_t *w, uint32_t *sum, uint32_t *x)
{
    *sum = *x = 0;
    for (int i = 1; i < 0x21f; i++) { *sum += w[i]; *x ^= w[i]; }
}
static int save_slot_used(const uint32_t *w)   // a slot the game would load
{
    uint32_t sum, x;
    save_slot_sums(w, &sum, &x);
    return w[0] && w[0] == sum && w[0x21f] == x;
}

// A randomizer run's orb level items (two per orb: thunder, fire, wind) and the inventory slots they get, 49-54, which
// no item uses (randomizer.c gives them, input.c counts them, launcher/randomizer.h places them).
#define RANDO_LEVEL_ITEMS 0x64, 0x65, 0x67, 0x68, 0x6a, 0x6b
#define RANDO_LEVEL_SLOT0 49

#define G_PLAY_TICK 0x0029eb0eu   // byte, +1 a frame during gameplay, frozen in menus, the title and the pause screen
#define G_INVENTORY 0x0054723cu   // {item id, count (0xff = single), ?, flags} entries up to the maps
#define G_MAPS 0x00547340u        // 8 entries, ids 0x80-0x87
#define G_PLAYER_ID 0x00547442u   // 0 Samanosuke, 1 Kaede (who is being played)

static int game_room(void) { return *(volatile uint16_t *)G_STAGE * 100 + *(volatile uint16_t *)G_ROOM; }   // as in roomSRR
static int item_count(int id)   // 0xff (a single item) counts as 1
{
    for (uint32_t a = G_INVENTORY; a < G_MAPS; a += 4)
        if (*(volatile uint8_t *)a == id) return *(volatile uint8_t *)(a + 1) == 0xff ? 1 : *(volatile uint8_t *)(a + 1);
    return 0;
}
