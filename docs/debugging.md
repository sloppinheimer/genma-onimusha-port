# Finding problems and debug switches

For players, `logs\onimusha.log` is the file to attach to a bug report (the user folder in its paths is written as
`%USERPROFILE%`). The rest of this page is for working on the port.

## What the game writes down
- `logs\features.log` lists everything the game has used for the first time: texture formats,
  texture-stage operations, render states, shader inputs, unimplemented calls. It persists across sessions,
  so each session only adds what's new. Lines starting with `!` are things the port doesn't fully handle;
  for those the frame is saved as `new_<frame>.bmp` so you can see where it happened.
- `logs\stalls.txt` collects the log's stall lines over launches: game steps over 45 ms and slow HD
  pack texture loads, with the frame (and the room, for slow steps). A slow step is split into where its ms went: our
  present, draw, texture uploads, in-betweens, frame wait, the game's file I/O, and the rest (mostly the game's own code);
  `cpu` far below the step's length means the game thread was waiting or held off the CPU, not working. A
  `slow second` line is 60 steps (on any screen; not movies, which run at 30) that took over 1050 ms with none of them
  a stall, given as the average step, with the draws per frame. Past 1 MB it becomes `stalls.old.txt` and starts again.
- `logs\room_textures.txt` records which pack textures each room used; the next visit reads them ahead. An animated
  background's frame sheets (`xmov*.xpr`) are read ahead whenever the game opens the file.
- `mapping\`, for the randomizer's research: `items.txt` and `rooms.txt` record each
  pickup, story flag, door unlock, first-held item, room visit and room-to-room move once, the first time it's seen
  in play, adding up over launches, saves and playthroughs. `checklist.txt` lists every item location as missing or
  found, then the rooms and door links not seen yet; it needs `locations.tsv` and `doors.tsv` there, written from your
  game files by the randomizer's research tools (not part of this repository).
- Development builds (`ONI_DEBUG`, below) only: F12 logs every draw of the next two frames to `logs\onimusha.log` and saves
  them: `captures\screenshots\shot_*.bmp` (and `mid_*.bmp`, `new_*.bmp`, the frames where something unsupported first
  showed), `captures\depth\depth_*.bmp`. A release's F12 saves a plain PNG screenshot instead.
- F10 (development builds) takes a compare shot: the same frame as you see it (HD pack and options) and as the original
  game draws it (its own textures, 640x480), in `captures\compare\` as `<frame>_hd.png`, `<frame>_original.png` and
  `<frame>_side.png` (the two side by side, captioned). Both come from one frame, so a fight matches exactly. The frame is
  drawn 4:3 (with `picture = modern` that one frame flashes on screen). F10 bound to a game button in `[keys]` does both.
- Shift+F12 (development builds) saves every texture the next frame draws, as the game has it (not a pack's replacement, whatever mods are on):
  `captures\textures\<frame>\<pack name>.png`, with `index.txt` (each draw's textures, and HD or `--` for whether a pack
  replaced it).

## Debug switches (environment variables)
Those that write game memory, saves, achievements or files, and the launcher's URL, page and DevTools switches, work
only in a local build (`ONI_UPDATE_REPO` empty, which defines `ONI_DEBUG`); a release ignores them, and downloads over
HTTPS only. These are `ONI_POKE`, `ONI_WARP`, `ONI_WARP_SLOT`, `ONI_WARP_SET`, `ONI_WARP_AT`, `ONI_ACH`, `ONI_WATCH`,
`ONI_WATCHWRITE`, `ONI_DUMP_*`, `ONI_TEST_RESET`, `ONI_UPDATE_URL`, `ONI_PACK_URL`, `ONI_LAUNCHER_UI` and
`ONI_LAUNCHER_DEVTOOLS`. A flag is on when set to 1.
- `ONI_SHOTS=120,600` — save the frame at those frame numbers as `captures\screenshots\shot_N.bmp`
- `ONI_SHOTS_FULL=1` — `ONI_SHOTS`/F12 frames at the render resolution instead of 640x480
- `ONI_CAPTURE=<frame>` — the F12 capture (every draw logged, frames saved) at that frame, from a script
- `ONI_TEXCAPTURE=<frame>` — the Shift+F12 texture save at that frame, from a script
- `ONI_COMPARE=<frame>` — the F10 compare shot at that frame, from a script (with `ONI_FAST` its original half can come out wrong)
- `ONI_INPUT=1000-1006:start,1300-1306:a` — hold buttons during frame ranges (also `lthumb` = map,
  `rthumb` = status screen)
- `ONI_BACKGROUND=1` — window off-screen and muted, real controller and keyboard ignored: only `ONI_INPUT` drives
  the game. `ONI_PAD_SCRIPT=1` plays the `ONI_INPUT` script as a controller (to test Modern controls)
- `ONI_FAST=1` — drops vsync and the vblank waits, so scripted runs go faster than real time
- `ONI_WARP=305` — the next save you load puts you in room 305 (stage 3, room 05) at the saved position. Same-stage
  and some cross-stage warps work; others fail to load (the room table for that stage isn't set up)
- `ONI_WARP_SLOT=<file>` — with `ONI_WARP` (0 keeps that slot's own room): load that save slot's 0x880 bytes instead
  of the one picked, e.g. a slot cut out of another `save.dat`
- `ONI_WARP_SET=18:0,16:3` — with `ONI_WARP`, write 16-bit values at those byte offsets of the save being loaded
- `ONI_WARP_AT=4600:102` — at that frame, mid-game, go to that room the way a door does (its arrival, from
  `src/host/warp_table.h`); a `k` after the room also switches who is played
- `ONI_POKE=3650:546bb4<slot5.bin` — copy a file into game memory at a frame, e.g. a save slot's 0x880 bytes over the
  loaded game (saves made on another console fail the game's signature check); also writes and dumps (src/host/input.c)
- `ONI_WATCH=546ae0:32` — log a memory range when it changes; `ONI_WATCHWRITE=546aea:2` — log the code address of
  every write to it (CPU debug registers)
- `ONI_ACH=<id>` — unlock that achievement as soon as play starts (to see the popup). It is a real unlock: it's written
  to `saves\achievements.dat` like any other
- `ONI_ACH_SOUND=<hex id>` — the achievement popup plays that game sound instead of the menu chime
- `ONI_TEST_RESET=7000` — reset the D3D9 device at that frame, as after a lost device (lock screen, UAC prompt)
- `ONI_HITCH=1` — log stalls from 25 ms instead of 45 ms
- `ONI_ONLY=n` — draw only the n-th draw call of each frame; `ONI_PLAIN=1` — untextured, unblended
- `ONI_MOVIE_43=1` — with `picture = modern`, movies taller than 16:9 keep the 4:3 picture instead of filling the
  window's width (centred, top and bottom cut)
- `ONI_SHADOW_BIAS=<float>` — override the shadow depth offset (0 = off); `ONI_PRECISE_DEPTH=0` — the old
  vertex-shader depth maths; `ONI_GAPFILL=0` — no gap-filling first pass for opaque 3D draws under MSAA
- `ONI_NODEPTH=1` — don't upload rooms' pre-rendered depth images; `ONI_DEPTH_STEPS=1` — nearest depth texel at
  their edges instead of the blend (to compare)
- `ONI_DUMP_TEXT=1` — with `enhanced_text` in French/German (Kabuto Sans), save the redrawn font sheet as `text_sheet.pgm` (index x 28)
- `ONI_DUMP_BG=1` — save each new large screen image (backgrounds, title art) as `bg_<frame>_<w>x<h>.bmp`. The
  `ONI_DUMP_BG`, `_XBR`, `_PICS` and `_VSH` files (and any vertex shader that fails to compile) go to `captures\dumps\`
- `ONI_DUMP_XBR=1` — save each HUD/menu sheet's `hud_filter = xbr` upscale once as `xbr_<name>_<w>x<h>.rgba` (raw
  RGBA; the base for the HD pack's retyped sheets)
- `ONI_DUMP_PICS=1` — save the weapon wheel's item pictures as `pic_<item>.bmp`
- `ONI_DUMP_VSH=1` — write every translated vertex shader as `vsh_<handle>.hlsl` (failed ones are always written)
- Launcher: `ONI_UPDATE_URL=<release JSON>` and `ONI_PACK_URL=<pack.ini>` check those instead of GitHub;
  `ONI_LAUNCHER_UI=<folder>` serves the page from a folder; `ONI_LAUNCHER_DEVTOOLS=1` allows devtools
