# Settings

Every option, as `onimusha.ini` holds it. The launcher sets most of them; the README has the short version.

## The launcher
Its pages are Video, Mods, Game (with Controls), Randomizer, Achievements, Cheats and About. It works with the mouse,
the keyboard (arrows, Enter, Q/E for the pages, Esc back, Ctrl+Enter or F5 to play) and a controller (d-pad, A, B
back, LB/RB for the pages, Start to play); Left/Right picks a row's buttons (a mod's Open folder, Remove, Update...).
It sets most of `onimusha.ini`; a few settings are in the file only (`vsync`, `anisotropy`, `fxaa`,
`background_denoise`, `background_filter = enhanced`). About shows the version, offers updates, and has the
launcher's own switches: hide while playing, check for updates, and a log window beside the game.

## onimusha.ini
`onimusha.ini` is written next to the launcher on first run, with comments. Delete it to get the defaults back.
- `scale` — internal resolution, 640x480 times 1-8 (default 2). Pre-rendered backgrounds stay 640x480 and are upscaled.
- `window` — `windowed` or `borderless`; `window_width` / `window_height` for the windowed size.
- `picture` — how the 4:3 picture fills a wider screen: `original` (black bars, default), `stretch` (fills it,
  everything wider), `zoom` (fills it by cutting top and bottom; the edge of the health orb may be cut) or `modern`
  (as the 2019 remaster: the full width of the art, a slice of its height that slides to follow Samanosuke, the HUD
  pinned to the screen's corners and never cut; while the 3D world is drawn, so play, the pause menu and in-engine
  scenes). Under `modern`, movies fill the window's width (a wide one uncut, a taller one centred with its top and
  bottom cut), full-screen 2D menus are scaled to the window's height with their sides filled out from the art's own
  edges, and the title stays 4:3. That side fill is the only thing drawn beyond the original art; see
  [widescreen.md](widescreen.md).
- `msaa` — anti-aliasing samples: 0, 2, 4 (default) or 8.
- `vsync` — 1 (default) or 0.
- `background_filter` — `smooth` (default), `sharp` (nearest pixel, also HUD and text) or `enhanced`
  (denoise + super-xBR, run once per background; `background_denoise` 0-1 sets the cleanup). On this game's soft
  pre-rendered art `enhanced` looks almost the same as `smooth`.
- `anisotropy` — 0-16 (default 16): sharper 3D textures at angles.
- `fxaa` — 0 (default) or 1: post-process anti-aliasing for edges MSAA misses; slightly softens the image.
- `show_fps` — 0 (default) or 1: frame rate in the window's top-left corner (frames shown, and the game's own rate
  when frame interpolation draws more).
- `hud_filter` — text, menus and HUD: `pixel` (default) or `xbr` (redrawn with smooth curves; no longer pixelated).
- `enhanced_text` — 0 (default) or 1: the menu and message font drawn sharp at the internal resolution, with smooth
  edges. In English with the HD pack's HD Textures on, it's the game's own letters cleaned up at 12x
  (`font\text_sheet_e.bin` in that mod, made from the game's font, so the port doesn't ship it; it is also read from
  beside the exe). Without it, and in French and German (accented letters that sheet doesn't have), the port uses
  Kabuto Sans, a font matched to the original (Didact Gothic made bolder, with Kosugi Maru's g and symbols). Each
  letter goes where the original's is, so layout and colours don't change. Needs `scale` 2 or more. Pictures with
  words drawn into them (documents, the title menu) stay as they are. On for the launcher's Modern style.
- `frame_interpolation` — frames shown per game step: 0 (off, default), 2 (120 fps) or 4 (240 fps). The game itself
  always runs 59.94 steps per second; in-between frames blend character and camera movement and add about 17 ms of
  delay. Skipped at camera cuts and on menus that rewrite their text mid-frame.
- `[game]` `movie_subtitles` — `text` (default), `japanese` or `off`: subtitles for the movies, from a mod that has
  them (the HD pack's FMV Subtitles). `text` follows `text_language`. See [modding.md](modding.md).
- `[game]` `skip_intro` — 0 (default) or 1: start without the Capcom logo, the violence warning and the hard disk
  messages before them. Without it, A or Start skips each of them.
- `[mods]` `modern_controls` — 0 (default) or 1: the controller laid out as in the Onimusha: Warlords remaster. The
  left stick walks Samanosuke in the direction it points on screen (the d-pad keeps the tank controls) and works as a
  d-pad in menus; LB guard, RB draw weapon, LT the Gauntlet's power, tap RT for the next weapon you own (waits for an
  attack to finish), R3 180-degree turn, View pause, Menu inventory, L3 map; X attack, Y magic, B absorb souls /
  cancel, A check (the game's B-TYPE, which this sets). The keyboard keeps its own keys.
- `[mods]` `weapon_wheel` — 0 (default) or 1: hold RT (Modern controls) or B (original controls) during play for a
  weapon wheel (point the left stick or d-pad, let go to equip; LB/RB pick a launcher's ammo; the game runs slowly
  while it is open; LT/RT or a flick of the right stick switch to an Items page, where A uses the Herb or Medicine
  pointed at, and letting go only closes the wheel). While Kaede is played the wheel is the Items page alone (she has
  only her dagger) and a tap of RT does nothing. Without Modern controls, B is taken over during play. The wheel shows
  the game's own item pictures, decoded from `IMAGE\ETC\ITEMI.IDA` on your disc.
- `[game]` `text_language` — `english` (default), `french` or `german`: menus, item names and descriptions,
  documents, prompts, subtitles and puzzles (puzzles not checked in play) from the European release's text, which the
  USA disc still carries. Accents show. Voices stay English or Japanese (the game's own Language option). Untested:
  the game's Subtitle option set to Japanese while French or German is chosen. Still English or not quite right: the
  "TOTAL:" label on the status screen, the "Hard Mode" title-menu entry (the European title pictures have none), and
  subtitles over movies that run to two lines are cut at the bottom edge. Accented letters sit about 2 pixels low. A
  few messages the USA version added (Voice Volume, Ogre Armor, Secret key, Petit Panda and four prompts) use the
  port's own translations.
- `[game]` `achievement_popups` — 1 (default) or 0: a note in the corner of the game when an achievement unlocks.
  Achievements (the 2019 remaster's Steam list, `src/host/achievements_list.h`) are kept for the whole install in
  `saves\achievements.dat` (sealed: not readable or editable by hand) and listed on the launcher's Achievements page.
  The game's memory is checked a few times a second, so loading a save awards what it already qualifies for. "None
  for Me, Thanks" (no Herbs or Medicine) needs the whole run watched: `saves\achievement_runs.dat` remembers, per save,
  whether one was used by then, so it counts only for runs started (or saved) with the port. The one-off combat
  moments don't unlock yet: see `src/host/achievements.c` for which ones are checked.
- `[cheats]` — `god_mode` (no damage), `infinite_magic`, `infinite_souls` (red souls stay at 999999),
  `one_hit_kill`, `infinite_ammo` / `infinite_items` (arrows and bullets / Herbs and Medicine you carry stay at 99),
  `infinite_onimusha` (the green souls refill as soon as the Onimusha form ends), `vision_staff` (hidden Fluorite
  sparkles, as with the Vision Staff); each 0 (default) or 1, also on the launcher's Cheats page. While any cheat is
  on no achievements unlock, and every save made then is marked: loading it later keeps achievements off, with cheats
  on or not, and that game stays marked through later saves. A different game played without cheats, saved over it,
  isn't marked. See [mods.md](mods.md) for the addresses.
- `[randomizer]` — `randomizer_run`: the run being played (a folder in `randomizer\`), empty for the normal game. The
  rest (`randomizer_on`, `randomizer_pick` and the options for a new run, `randomizer_fluorite_sanity`,
  `randomizer_long_challenges`, `randomizer_door_warp`, `randomizer_keep_consumables`) are the launcher's own. See the
  README's Randomizer section.
- `[launcher]` — `hide_while_playing` (1, default, or 0), `check_updates` (1 or 0), `console` (0 or 1: a window
  beside the game showing `onimusha.log` as it's written; closing it ends the game) and `setup_style` (the style
  chosen at the first start). Set on the launcher's About page.
- `[keys]` — keyboard bindings.
- Play time: the game keeps the total time it has run (menus included, as Steam counts it) in `saves\playtime.txt`,
  saved every 30 s and at exit; the launcher shows it under Play.
