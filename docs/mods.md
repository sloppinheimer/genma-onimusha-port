# Game memory map and mod notes

What's built, ideas for more, and what is known about the game's memory. `modern_controls` and `weapon_wheel` are written into the port's code and switched in
`onimusha.ini` under `[mods]` (the launcher's Style choices set them). Everything else is a folder in `mods\` (see
"How mods are packaged" below; the user-facing version is `mods\README.txt`).

## Built

- `modern_controls` (was `stick_move`): the controller laid out as in the 2019 remaster; the left stick walks
  Samanosuke toward where it points on screen, and acts as a d-pad in menus, text boxes and while guarding. RT
  taps to the next weapon.
- `weapon_wheel` (was `swap_weapon`): hold RT (Modern controls) or B (original controls) for a weapon wheel; point
  with the left stick or d-pad and let go (or press A) to equip; LB/RB change bow and Matchlock ammo. Without
  Modern controls a tap of B does nothing.
- Items page on the wheel: LT/RT or a flick of the right stick switch pages; point at a Herb or Medicine and press A
  to use it (letting go of the button only closes the wheel; at full health it plays the "can't" sound instead).

## How mods are packaged

Code: `src/host/mods_list.h` (shared by the game and the launcher), `kernel.c` (file lookup), `texpack.c`, `movie.c`,
`text.c`; the list and cards are in `launcher/main.cpp` and `launcher/ui/index.html`.

- A mod is a folder in `mods\`. Files in it sit where they sit in the game folder and are read instead of the disc's
  copy; where two enabled mods have the same file the one higher in the launcher's list wins. Folders starting with
  `.` are ignored (the launcher uses `mods\.installing-<n>` while installing). `mods\mods.txt` keeps the order and what
  is switched off. A mod's `patch.txt` (address = bytes) changes the game's code while the mod is on, applied from the
  lowest mod up so the higher one wins (`host.c` apply_patches).
- `mod.ini` (optional): `name`, `description`, `author`, `version`, `enabled` (0 = off until switched on),
  `builtin = <id>` (a change written into the port, no game files in the folder), `modern = 1` (the launcher's Modern
  style switches it on; Classic switches every mod off), `pack = HD pack` (the folder came from the pack
  download; `version` says which release).
- The launcher's Mods tab has three sections: Built in, HD pack, Custom mods.
- HD pack: the launcher reads `pack.ini` from the latest release of `github.com/sloppinheimer/genma-onimusha-hd-pack`
  (the `ONI_PACK_REPO` build setting; empty turns downloads off), then downloads the chosen folders one at a time into
  `mods\` and offers an update when an installed folder's `version` differs from the release.
- Texture packs: `textures\` in a mod, PNG, DDS or JPG (JPG has no transparency), named by the texture's hash.
- HD movies: `movies\<name>.mp4` in a mod (or `<name>_*.mp4`), matched frame for frame to the game's Bink.
  Subtitles: `<movie>.<lang>.srt` (en, fr, de, ja) beside the `.bik` or in a mod's `subtitles\` folder; a `<movie>.wav`
  beside the `.bik` replaces its sound.
- Enhanced text: `font\text_sheet_e.bin` in a mod gives `enhanced_text` the game's own English letters instead of
  Kabuto Sans.

## Ideas

Rough order of value for effort.

**Cheap (mostly reuse)**
1. Quick-item wheel: built (the wheel's Items page). More items could join: upgrades 0x05 / 0x06, item 0x04.
2. Hold to fast-forward: a button that runs the game 2-4x (door transitions, walking, text). Tried as F9 for
   playtests and taken out before release (b79ffdd); the debug switch `ONI_FAST` remains.
3. Photo mode: freeze, hide the HUD. Needs the game's HUD-visible flag (not found; `0x1e7270` looked like one
   but isn't reliable).

**Medium**
4. Corner mini-map: `ITEMI.IDA` holds the area maps as pictures (items 0x80-0x87). Draw the current one with a
   dot at the player's position.
5. Enemy health bars: the targeting code walks an object list (head at `0x4b9ee4`, positions at +0x890). If HP
   is in those objects, draw bars above enemies.
6. Right-stick target switching: `FUN_0010ab90` picks the enemy nearest a stick direction on screen; the game
   doesn't use it in normal play. Wire it to the right stick.

**With SDL3 controllers (already in: input.c)**
7. Gyro aim for the bow and Matchlock (DualSense, Switch Pro, DualShock 4).
8. Stronger rumble and adaptive-trigger effects on hits and the bow draw.

**Bigger**
9. Challenge toggles: one-hit deaths, no healing items, no HUD.
10. Item randomizer: built (the launcher's Randomizer page; items, orbs and, as an option, doors).

## What we know (addresses in the Xbox image; the port maps it at the same addresses)

Player and world
- Player object `0x547440`. Position (float x, y, z; y up) `0x547cd0`; previous position `0x547ce0`.
- Facing `0x547d22`: low 12 bits, 0x1000 = full turn; walks along (sin, cos) in x/z; d-pad left raises it,
  45 units a frame.
- Action flags `0x549234`; the game's pose reset clears `0x96b10200`; bit 31 = attacking.
- Camera: world-to-screen matrix `0x53e540` (row vectors, screen pixels, y down).
- Gameplay frame counter (byte) `0x29eb0e`: +1 a frame in play, frozen in menus.
- Stage/room `0x546ae8` / `0x546aea`. Game state block from the save: `0x546bb4`-`0x547430`.

Pad
- Pad state for port 0 at `0x542c60`: dword = digital buttons << 16 | analog-button bits (A 1, B 2, X 4, Y 8,
  Black 0x10, White 0x20, LT 0x40, RT 0x80). Sticks as signed bytes at `0x542cc8`-`0x542ccb`.

Weapons and items
- Equipped weapon `0x5471ec`, its element `0x5471ee` (0xff if none), sword levels per element `0x5471ff`
  (Raizan, Enryuu, Shippuu; 0xff = not owned). Ids: orb sword element * 3 + level, 9 Normal Sword,
  0x0b-0x0e bow / Matchlock with ammo 0x17-0x1a.
- Equip: `FUN_0010cc50(0x547440, player[2], -1, id, 0)` then `FUN_00115120()` (as the equip screen,
  `FUN_0015fe10`, does).
- Inventory `0x54723c`-`0x54733f`: 4-byte entries {item id, count (0xff = single), ?, flags}.
  `FUN_0015ebd0(id, delta)` adds to a count (and clears the entry at 0).
- Health `0x548328` (short), maximum `0x548326`; `FUN_000193c0()` refreshes the health bar.
- Item use (Item screen, `FUN_0015e430` then `FUN_00160ad0`): Herb 0x01 heals 0x40, Medicine 0x02 heals fully,
  0x05 / 0x06 raise maximum health / magic by 0x18, 0x04 bumps a counter at `0x2a12c4` (unknown), Talisman 0x03
  and Fluorite 0x07 can't be used (the Talisman revives by itself, `FUN_00114ff0`). Sounds: `FUN_0015c440(0x201000f,
  0, 0, 1)` use, `0x301000f` can't.
- Item pictures: `IMAGE\ETC\ITEMI.IDA`, 256 offsets (+0x400) to CMP-compressed OIM images; decoder in
  `d3d_overlay.c` (the game's `FUN_0001b690` / `FUN_0001b720`). Picture ids: 0x01 Herb, 0x10 Normal Sword,
  0x15 bow, 0x16 Matchlock, 0x17-0x1a ammo, 0x63-0x6b orb swords by level, 0x80-0x87 maps, 0x90-0x9f scrolls
  and books.

Stats and damage (used by the cheats, cheats.c; the values checked in game, the damage paths only read from the code)
- The player object `0x547440` and enemies share one layout: health at +0xee8 (short), maximum +0xee6, the
  object that last hit you +0xec0. So the player's health `0x548328` is `0x547440` + 0xee8.
- Health below 0 is death: `FUN_00115120` (the player's frame) takes the hit (`FUN_00108a50`) and checks it
  in the same frame, so topping health up once a frame can't stop a hit bigger than what's left. Other paths
  subtract directly (traps, `FUN_00084580`).
- Kaede: `FUN_0010a140` swaps who is played. The current health and maximums at `0x548328` / `0x548326` /
  `0x546bc6` are always the one being played; the other's are parked at `0x5471f4`-`0x5471fa`.
- Magic: one short per orb at `0x5473ce` (Thunder, Fire, Wind), each up to the maximum `0x546bc6`; the
  equipped orb is `0x5471ee` (-1 none). A magic attack needs 0x30 in the equipped orb's gauge.
- Red souls: `0x547214` (u32) is what you have to spend; `0x547218` the lifetime total. Both capped at 999999.
  `FUN_00018610(kind, amount)` changes stats: 0 max health, 1 max magic, 2 health, 3 magic, 4/5 souls.
- Onimusha: green-soul charges `0x5473f4` (0-5); at 5 a button transforms (`FUN_001109a0`): charges to 0, the
  timer `0x5531bc` to 600 frames, its maximum `0x5531be` to 600. `FUN_00115120` counts it down by one a frame
  (and heals 1 health every 8 frames).
- Ammo and items are counts in the inventory (above): ammo 0x17-0x1a, Herb 0x01, Medicine 0x02, Talisman 0x03.
- Enemy damage: most enemies go through `FUN_000a66a0(enemy, hit)`: damage from `FUN_000a60c0`, taken off
  +0xee8 (43% more while Onimusha is running). About 30 enemy types call it; `FUN_00051810` has its own
  subtraction and may not be the only other one.

Debug helpers
- `ONI_INPUT` (buttons, `ls_up/down/left/right`, `lthumb/rthumb`), `ONI_POKE` (write or dump memory at a
  frame), `ONI_SHOTS` / `ONI_SHOTS_FULL`, `ONI_FAST`, `ONI_BACKGROUND`.
- Hardware watchpoints (`ONI_WATCHWRITE`) didn't fire in background runs. Making a page read-only and logging
  the faulting instruction worked to find who writes a buffer (or one value: single-step through other writes to
  the page and re-protect). The menu works on its own copy of the inventory; watch health instead of the counts.
