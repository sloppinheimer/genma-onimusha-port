# Widescreen: research note

Status: the remaster's crop is implemented as `picture = modern` (see "In the port" at the end); the in-game menu
gets a made-up side fill. Apart from that fill, nothing wider than the original art is drawn. The first half of this
note (up to "How the 2019 Steam remaster does it") is the research from before it was built, kept as history: the
options and the recommendation below were settled by the crop.

## Why it isn't just a wider projection

Most rooms are a pre-rendered 640x480 background image plus a matching 640x480 depth image
(uploaded each room by `CopyRects` into the depth buffer, see `upload_depth` in `d3d.c`).
Only the characters, items and effects are real 3D. The camera is fixed per shot to line up
with the painting.

Widening the 3D projection would let characters and effects draw past the left and right edges,
but there is no background or depth there. The result is characters walking over black, and
anything off the painted area isn't occluded. Stretching the image instead distorts everything.

## What it would take

1. **Projection.** Find where the game builds its projection (it reaches the GPU as vertex-shader
   constants; the viewport scale/offset are the reserved constants 58/59). Scale the horizontal
   term by 3/4 × the new aspect. Unverified: where the matrix is built hasn't been traced.
2. **Fill the sides.** Options, cheapest first:
   - Mirror or blur the edge columns of the background into the side bars (common in remasters;
     cosmetic only, the 3D still has to be clipped to the 4:3 area).
   - Backgrounds extended by generated outpainting per camera angle, with matching depth. Hundreds of shots, plus
     the depth images, and it ships derived art.
3. **HUD.** HUD and menus are 640x480 screen-space quads; they would need pinning to the 4:3
   centre or to the new edges per element.
4. **Culling and triggers.** The game may cull or spawn enemies based on what the 4:3 camera
   sees. A wider view could show pop-in. Unverified.

## Recommendation (superseded)

The early view was that this wasn't worth it as a core feature, and that the cheapest version would be a
blurred or mirrored side fill with the 3D clipped to 4:3. The remaster's crop (next section) turned out cheaper and
better, and is what `picture = modern` does.

## How the 2019 Steam remaster does it

Checked against the Steam install (`steamapps/common/Onimusha`, `IMAGE/` holds the same file
layout as the Xbox disc, with PC versions of some files).

- **No new background art.** `IMAGE/BGS/ROOMxxx.BGS` holds each shot as a DXT1 DDS at
  **1920x1344**, exactly 3x a 640x448 image, with the same 4:3 framing as the original. Before the
  first DDS (offset 0x118000) is a block of 1,146,880 bytes, which is 640x448 floats: depth kept at
  the original resolution. Room 101 has 2 images; room 000 has 22.
- **Widescreen is a vertical crop.** A 16:9 view of a 1920-wide image is 1080 tall,
  so 264 of the 1344 rows (about 20%) are off screen at any time. A 16:9 screenshot of the
  remaster shows full-width art with no side bars. The window pans to follow the player, with a
  dead zone and per-shot exceptions: see "The remaster's pan rule" below.
- So option 2 above has a third, cheaper choice the remaster proves works: **crop instead of
  fill**. Widen the projection to 16:9, then crop the top and bottom of the 4:3 background
  and depth to match, with no new art. It loses some picture at the top and bottom, and shots
  that frame something important near the top or bottom edge may need a per-shot offset.

### The remaster's pan rule

Worked out in October 2026 from the running Steam game (the exe is encrypted on disk; its code was
read from memory) and checked against 1,213 logged samples. Behaviour only; nothing of Capcom's is in
the repo.

Once per 60 Hz game step:

1. `live` = screen height of the player's feet (his root position, on the floor) under the current
   camera, in NDC: +1 top of the 4:3 picture, -1 bottom.
2. `s` = smoothed height: on a camera cut `s = live`; otherwise `s = 0.9*s + 0.1*live`, clamped to
   -1..1 (about 90% settled after 22 steps, a third of a second).
3. Offset `f` from a dead zone `lo..hi`, normally `lo = -0.5`, `hi = 0`:
   - `s > hi`: `f = 0.125 * (s - hi) / (1 - hi)`
   - `s < lo`: `f = -0.125 * (lo - s) / (lo + 1)`
   - else `f = 0`
4. The visible window is the 75%-tall band of the picture from `0.125 - f` to `0.875 - f` (as a
   fraction of its height, from the top). `f = 0` is centred; `f = +0.125` shows the top edge,
   `-0.125` the bottom.

So the view holds still while his feet are between the middle and three quarters down, follows
1:1 above the middle, and twice as fast in the bottom quarter, reaching the bottom edge as he does.

Per-shot exceptions, keyed by (stage, room, camera). The room file is presumably
`ROOM<stage><room>` (room 0 of stage 2 is `ROOM200`; assumed, not checked):

| Shot | `live` becomes |
|---|---|
| 1,6,6 | 0 (centred, no pan) |
| 5,21,5 | pan off: the whole 4:3 picture, bars at the sides |
| 2,35,3 / 5,1,2 / 5,21,16 | +0.8 (held near the top) |
| 5,18,1 | `live + 0.8` |
| 5,18,3 / 5,18,4 | -0.643 / -0.557 |
| 5,18,13 | -0.355, snapped |
| 4,4,2 | `live + 1.75` (in practice pinned to the top) |
| 4,4,4 | another object's height (not traced); dead zone off (`lo = 0`) |
| 4,4,1 / 4,4,25 | `live` stretched so -1..0.4 covers -1..1; dead zone off |

Other states:

- In one game state (a flag on the player record), every shot is held centred except 22 listed
  shots, which follow; (1,26,0) is held at -0.74 instead, and every camera in room (5,20) follows
  and snaps.
- Outside normal play (a different flag), the pan is off. One list of shots then shows the whole
  4:3 picture with a 1/112 margin (pillarboxed); another shows a fixed centred band, nudged by
  +0.075 in (3,11,3) and -0.1 in (2,20,5). These look like in-engine scene framings.

Rooms differ between Genma and Warlords, so the shot lists apply to the port only where a shot is
the same in both.

### Font

`IMAGE/ETC/MOJIST_E.LDT` is the same container as the Xbox file (`LINK` / `OIM `) but holds a
512x512 DXT5 DDS instead of a 256x256 4-bit texture. The letter designs are unchanged (one-storey
a, two-loop g, cross-shaped t, PS2 button labels still present), redrawn at 2x with clean edges.
The `RDT/*FONT.FNT` files are glyph position tables. No font file or font maker's name is in
`game.exe` or the data. The menu font looks like Capcom's own lettering, not a licensed typeface.

These are Capcom's files from a separate purchase. The port could read them from the player's own
Steam install, but must not ship them.

## In the port: `picture = modern`

Checked against Steam screenshots of the remaster at 16:9 (October 2026): the view slides up and down with Samanosuke
inside each fixed camera shot; the health HUD sits in the screen's top-left corner, uncut; the pause menu is centred
over the cropped scene; the equip and map screens are redrawn full width (new art, not reproducible here).

How the port does it (`d3d_modern.c`, with the frame composed in `d3d_post.c`):

- The game draws its art at 640x448 (y 16-464, black strips above and below) and ends each frame by copying the frame
  and drawing it back squeezed to x 21.33-618.67 (the TV's pixel shape). That squeezed area is a 4:3 picture; Modern
  shows its full width and a slice of its height the window's shape.
- The slice pans by the remaster's rule (above, "The remaster's pan rule"), from Samanosuke's feet projected with the
  game's world-to-screen matrix (`game_view` in `input.c`): centred while they are between the middle and three
  quarters down the picture, following above and below that, snapping at camera cuts. At other window shapes the same
  -1..1 pan spans the slice's whole travel. The remaster's 13 normal-play shot exceptions are applied by stage, room
  and camera cut (`wide_shots`). Each is the same shot in both games: Genma's background for that cut is closest to Warlords'
  same-numbered one, with the same art (`genma_map.tsv` sdiff 1.0-1.9), except (4,4,25), which shares its
  background with cut 24 in Genma and so can't be told apart that way (likely the same); in (4,4,4)
  it follows him without the dead zone, as the remaster's other object isn't traced. (5,21,5) is shown in 4:3.
- HUD pieces (pre-transformed 2D drawn with the depth test off or "always", under 560 px wide) are drawn into their
  own layer instead of the frame, each pinned left / centre / right and top / middle / bottom by where it sits, at
  the picture's zoom. Full-width 2D (letterbox strips, fades, the final squeeze) stays in the picture.
- In-engine scenes draw letterbox bars (y 16-64 and 416-464 in the first scene): while they are drawn the slice holds
  still on the middle of the picture between them, so the bars stay cropped away.
- The top band of the HUD keeps its distance from the top of the art (it moves up by the 16 px strip). Fades drawn
  after the HUD also cover the HUD layer, as they cover the HUD in 4:3: dark ones dim it, light ones (the white
  flash of a talisman's revival) whiten it.
- Wide while the frame draws 3D (play, the pause menu, in-engine scenes; up to two frames without 3D in play; frames
  in play that draw the HUD or a message box but no 3D, such as a Magic Mirror's close-up).
  When a menu opens, the game fades out a 2D still of the scene: that stays wide until the screen is dark, so the
  framing doesn't jump.
- The in-game menu (equip, item, file, map; the game's mode word at `0x543088` has low byte 8), the Magic Mirror's
  save and enhance screens (7, 0x0b) and the title's load screen (0, with either of the two bytes after the word set,
  its fades included) are 2D only: their picture is scaled to the window's height and the sides are filled by
  carrying the art outwards (a mirror of the edge, then new blotches made from the edge's colours, with grain, darker
  towards the window edge; `compose_fill` in `d3d_post.c`).
- Movies: most are 640x360 (16:9), drawn letterboxed in the 4:3 frame at the frame's full width (x 21.33-618.67,
  y 72-408 for 360 rows). While one plays, if its picture's height fits the window's slice (centred), the frame is
  wide: a 16:9 movie fills a 16:9 window exactly, a wider one (links05 1.9:1, links07 2.06:1) keeps thin bars top and
  bottom. Taller ones (links01, 640x448; links0c, 640x384; linksu06) fill the window's width too, centred, with their
  top and bottom cut (a trial since 2026-10-24, kept; `ONI_MOVIE_43=1` keeps them 4:3 instead). Subtitles are drawn at the
  bottom of the window as before, over the picture when it fills it. Checked: linksu0be (attract) and linksu00e (the
  opening, with its subtitles) at 16:9.
- The Options screen and the screens it opens (Controls, Brightness, Screen Adjust) are 2D only too, but the game
  shows them in mode 0 like the title: they get the same fill, recognised by the sheets they draw (`screen2d_names`
  in `d3d_texture.c`, with the decode puzzle). The game's mode word is logged as it changes (`[d3d] frame N: mode ...`).
- Everything else that isn't wide (title, the opening letter) keeps the 4:3 picture, as does every frame
  in a window narrower than 4:3.

Not checked yet: subtitles and message boxes in scenes, 2D effects drawn with the depth test off (they would be
pinned like HUD), and rooms whose important detail sits at the top or bottom of the art.
