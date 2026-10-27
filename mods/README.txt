Genma Onimusha (native port) - mods

Each mod is a folder here. Switch mods on and off, and put them in order, in the launcher's Mods section
(Built in, HD pack, Custom mods). mods.txt, here too, is where the launcher keeps that order and the switches.

Replacing game files: put files in the folder the way they sit in the game folder, for example
  mods\My mod\image\bmp\etc\x_oni.bg
The game then reads that file instead of game\image\bmp\etc\x_oni.bg, which stays as it is. Where two mods
have the same file, the one higher in the launcher's list wins.

mod.ini (optional) describes a mod:
  name = My mod
  description = What it changes, shown in the launcher.
  author = Your name
  version = 1.0
  enabled = 0
  modern = 1
  pack = HD pack
enabled = 0 keeps the mod off until it's switched on (1, the default, is on). modern = 1 has the launcher's Modern
preset switch it on (Classic switches every mod off). A value runs to the end of its line, so keep comments on
lines of their own, starting with ;. pack = HD pack marks a folder the launcher downloaded from the HD pack;
its version then says which release, and the launcher offers an update when a newer one is out.
A preview.png (or preview.jpg) in the folder is shown in the launcher's mod list; 16:9 looks best.

HD texture packs: put PNG (or DDS) files in a textures folder in the mod, any subfolders you like. JPEG (.jpg)
works too and is much smaller, but has no transparency: use it only for solid pictures such as backgrounds.
  mods\My HD pack\textures\256x256_0b_f5dc105cf1685dbf_3b6458e6.png
Each file replaces the game texture with that name, at any size that is the original's times a whole number
(2x, 4x, ...; the same on both sides). With a texture pack on, logs\textures_seen.txt lists the name of every
texture the game draws, marked HD where a pack replaced it.
A name is <width>x<height>_<format>_<contents>; paletted ones (format 0b) end in their palette, and the same
picture often shows up with several palettes (fades, flashes). Name a file with _$ in place of the palette part,
e.g. 256x256_0b_f5dc105cf1685dbf_$.png, to replace it whatever the palette. Keep the alpha as the original has it: many of
this game's textures top out at 128 and still draw solid.

HD movies: put movies\<name>.mp4 in the mod, named like the game's movie (image\movie\<name>.bik). It must be H.264,
30 fps, the original's size times a whole number, with the same number of frames. The game still plays its own movie
for sound, timing and skipping; the picture is swapped for yours. A .wav (PCM) beside a .bik is used as that movie's
sound.
Movie subtitles: <movie>.<language>.srt (UTF-8; en, fr, de or ja), in the mod's subtitles folder. The movie_subtitles
setting picks text (the game's text language), japanese or off.

The HD pack (HD graphics, FMV Subtitles, Uncut Hecuba) is a separate download that the launcher fetches in its Mods
section. Its folders are ordinary mods with pack = HD pack in their mod.ini. HD Textures also holds the cleaned
English menu font, font\text_sheet_e.bin, which the enhanced_text setting uses when that mod is on.

Patches: a mod can change the game's code with a patch.txt in its folder, one change per line, all hex:
  ; Skips the logos     (a first ; line is shown as the description when mod.ini has none)
  11bf9 = 66 90
<address> = <bytes>, at the game's own addresses (it always sits at 0x10000). It is applied when the game starts,
while the mod is on; where two mods change the same bytes, the higher one wins. A file with a line that doesn't
read like that is skipped whole (onimusha.log says which line). A mod can hold a patch.txt and game files both.

The launcher can also install a mod from a .zip (Install from .zip) and remove one (to the Recycle Bin).

Folders with "builtin = <id>" in their mod.ini come with the port: they switch on changes written into the
port itself. They contain no game files.
