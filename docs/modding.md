# Making mods

How the port reads mods, and what a mod can hold. The game's memory addresses, for patches and cheats, are in
[mods.md](mods.md).

Mods are listed on the launcher's Mods page, where each can be switched on or off; they're read when the game starts.
The list has three parts: Built in, HD pack and Custom mods (last). The launcher installs mods from a .zip and
removes them to the Recycle Bin; `mods\README.txt` explains the format.
- **Mods** are folders in `mods\` next to the launcher. A mod holds files laid out like `game\`, for example
  `mods\My mod\image\bmp\etc\x_oni.bg` (the title logo). Whenever the game opens a file, the first mod that is on and
  has it wins; otherwise it reads `game\`, which is never changed. Folder and file names ignore case. Mods replace
  whole files, nothing is merged, and a file only a mod has is used only if the game asks for it by name (folder
  listings show `game\` alone). The log says which mod each file came from.
- **Patches**: a mod can change the game's code with a `patch.txt` in its folder, each line `<address> = <bytes>` in
  hex, applied after the game is loaded while the mod is on (a patch alone is just a mod with only a `patch.txt`).
  The game always sits at 0x10000, so addresses are those of the game in a disassembler loaded there. Where two
  overlap, the higher mod wins. Without a description in `mod.ini`, a first line starting with `;` is shown as one. A
  file with a line that doesn't read like that, or that writes outside the game, is skipped whole (the log says which
  line).
- A mod can carry a `mod.ini`: `name`, `description`, `author` and `version` (shown in the launcher), `enabled = 0` to
  start off, `modern = 1` to be switched on by the Modern style; and a `preview.png`/`.jpg` for the launcher's list.
- **Texture packs**: a mod's `textures\` folder (any subfolders) holds `.png`, `.dds` or `.jpg` files named like the
  game's textures (`logs\textures_seen.txt` lists them while a pack is on), each the original's size times a whole number. A
  `depth\<hash>.f32` file replaces a background's depth image (what 3D objects hide behind).
- **HD movies and subtitles**: `movies\<name>.mp4` in a mod replaces the movie `<name>.bik` (H.264, 30 fps, the
  original's size times a whole number, the same number of frames); the game still plays its own movie for sound,
  timing and skipping. An English-voice cut (`<name>e.bik`) with no HD file of its own uses `<name>.mp4` when the frame
  count matches (the pairs show the same picture), so a pack needs one file for both. A `.wav` beside a `.bik` is used
  as that movie's sound. Subtitles are `<movie>.<lang>.srt` (UTF-8) beside the movie or in the mod's `subtitles\`
  folder; the setting `movie_subtitles` picks the language.
- **The HD pack** is a separate download, from the latest release of
  [genma-onimusha-hd-pack](https://github.com/sloppinheimer/genma-onimusha-hd-pack). The launcher downloads it on the
  Mods page, one entry at a time, queued: **HD graphics** (HD Backgrounds, HD Textures and HD Movies as one entry),
  **FMV Subtitles** and **Uncut Hecuba**. A download can be cancelled, and one cut off (the launcher closed, the
  connection lost) carries on where it stopped when you get it again. Each lands in `mods\` with its version in its
  mod.ini, and is offered again when a newer release is out. It also holds the cleaned English font sheet
  (`HD Textures\font\text_sheet_e.bin`, see `enhanced_text`).
