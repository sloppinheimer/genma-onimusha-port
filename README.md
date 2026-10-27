<div align="center">

# Genma Xcorcised

**A native Windows port of Genma Onimusha (Xbox)**

[![Latest release](https://img.shields.io/github/v/release/sloppinheimer/genma-onimusha-port?label=download)](https://github.com/sloppinheimer/genma-onimusha-port/releases/latest)
[![HD pack](https://img.shields.io/github/v/release/sloppinheimer/genma-onimusha-hd-pack?label=HD%20pack)](https://github.com/sloppinheimer/genma-onimusha-hd-pack)
[![Licence: MIT](https://img.shields.io/badge/licence-MIT-blue)](LICENSE)

</div>

The Xbox game's own code runs natively on Windows: no emulator and no recompilation. A runtime written for this one
game stands in for the Xbox system (Direct3D 8 → Direct3D 9, DirectSound → XAudio2, controllers → SDL3), and the
game's code is loaded from your disc and patched in memory. **You need your own copy of the game**: an `.iso` of the
USA Xbox release. No game files are included.

It has been played from a new game to the end, saving and loading along the way.

## Features

| Area | Highlights |
|---|---|
| **Picture** | Up to 8× resolution · widescreen like the 2019 remaster (HUD pinned to the corners) · 120/240 fps frame interpolation · MSAA, FXAA, anisotropic filtering · smooth menus, HUD and text |
| **Controls** | Any controller SDL3 knows, several at once · the remaster's modern controls · a weapon wheel with an Items page · rebindable keys |
| **Game** | English, French or German text · movie subtitles · skip the intro logos · 3D sound · import saves from a real Xbox |
| **Launcher** | Installs from your disc image · every setting, mods and Play in one window · mouse, keyboard or controller · updates itself |
| **Achievements** | 59 of them: the remaster's Steam list plus a few for what only Genma has, with an in-game note when one unlocks |
| **Cheats** | God mode, one-hit kills, infinite magic, souls, ammo, items and Onimusha form, the Vision Staff's sight |
| **Randomizer** | Item shuffle checked to be finishable, optional door shuffle, seeds for racing, a spoiler log |
| **Mods** | File-replacing mod folders with load order, code patches, HD texture packs, HD movies and subtitles |
| **HD pack** | A separate download: 4× backgrounds, characters, effects, menus and text, HD movies, subtitles in four languages |

## Getting started

1. Download the [latest release](https://github.com/sloppinheimer/genma-onimusha-port/releases/latest) and unzip it
   anywhere you can write to (not `Program Files`).
2. Start `GenmaOnimusha.exe` and choose your disc image of **Genma Onimusha (USA) (En,Ja)**. The launcher copies the
   game into `game\` beside it and checks it's that release.
3. Pick **Modern** (fitted to your screen, high frame rate, smoothing, the remaster's controls) or **Classic** (as it
   was on the Xbox). Everything can be changed later.
4. Optional: on the **Mods** page, get the HD pack.
5. Press **Play**.

The launcher needs the Microsoft Edge WebView2 Runtime, which Windows 11 already has (see
[Troubleshooting](#troubleshooting) for Windows 10).

## Controls

Controllers work by button position, Xbox layout (the bottom face button is A). The keyboard defaults:

| Button | Key | Button | Key |
|---|---|---|---|
| D-pad | Arrows | Start | Enter |
| A | Z | Back | Backspace |
| B | X | White / Black | C / D |
| X | A | Left / right trigger | Q / W |
| Y | S | | |

Rebind them on **Game → Controls**. **Alt+Enter** or **F11** toggles fullscreen, **F12** saves a screenshot to
`captures\screenshots\`.

With **Modern controls** on, the left stick walks Samanosuke where it points on screen: LB guard, RB draw weapon,
LT the gauntlet's power, a tap of RT for the next weapon (hold it for the weapon wheel), R3 a 180° turn, X attack,
Y magic, B absorb souls, A check.

## Randomizer

The **Randomizer** page makes runs that shuffle where the items are, checked to be finishable. Give a run a name and
a seed (the same seed and options always make the same run, so you can race a friend), then pick options:

- **Dark Realm & Ogre Tower**: their floors join the shuffle.
- **Fluorite-sanity**: needed items can hide in Fluorite spots before you have the Vision Staff.
- **Don't Shuffle Consumables**: Herbs, Medicine, ammo and the like stay where they are.
- **Door-sanity** (experimental): doors lead to new places.

Each run keeps its own saves, so it never touches your normal game, and has a spoiler log. Runs unlock no
achievements.

## Saves, backups and updates

Your saves, achievements and play time are in `saves\` beside the launcher, randomizer runs in `randomizer\`. Copy
those two folders to back up. Updates never touch them, nor `game\` or `onimusha.ini`.

The launcher checks for a new release when it starts and offers it on the **About** page; the download is checked
against its SHA-256 before it replaces anything. A save from a real Xbox can be brought over with
**Game → Xbox save → Import**.

## Troubleshooting

<details>
<summary><b>"The launcher needs the Microsoft Edge WebView2 Runtime"</b> (Windows 10)</summary>

Install it free from microsoft.com/edge/webview2. If it's installed but won't start, restart Windows or repair it in
Settings → Apps. `bin\onimusha.exe` starts the game without the launcher meanwhile.
</details>

<details>
<summary><b>Windows protected your PC</b> (SmartScreen)</summary>

The release isn't signed, so Windows may warn the first time. **More info → Run anyway** starts it.
</details>

<details>
<summary><b>"This disc image isn't the USA release"</b></summary>

The port is made for Genma Onimusha (USA) (En,Ja). Another release can be installed anyway when asked, but isn't
supported.
</details>

<details>
<summary><b>A controller does nothing</b></summary>

`SDL3.dll` must be in `bin\` beside the game; without it only Xbox-compatible pads work. The Controls page says
whether a controller is connected.
</details>

<details>
<summary><b>Settings went wrong</b></summary>

Delete `onimusha.ini`; the next start writes the defaults again.
</details>

<details>
<summary><b>A crash, a black screen or a wrong picture</b></summary>

`logs\onimusha.log` says what the game was doing. Attach it to an issue; your user folder in it is written as
`%USERPROFILE%`.
</details>

## Documentation

- [Settings](docs/settings.md): every option in `onimusha.ini` and the launcher
- [Making mods](docs/modding.md): mod folders, patches, texture packs, HD movies and subtitles
- [Game memory](docs/mods.md): the addresses mods and cheats use
- [Building](docs/building.md) · [Debugging](docs/debugging.md) · [How the loader works](docs/loader.md) ·
  [Widescreen](docs/widescreen.md)

## Credits

- **Genma Onimusha** is Capcom's. This project includes none of its files.
- **Fonts:** Kabuto Sans, built into the game, is a modified
  [Didact Gothic](https://github.com/ossobuffo/didact-gothic) (Daniel Johnson, OFL 1.1) with glyphs from
  [Kosugi Maru](https://github.com/googlefonts/kosugi-maru) (MOTOYA, Apache 2.0). The icon's lettering uses Yuji Boku
  (The Yuji Project Authors, OFL 1.1).
- **Libraries:** [SDL3](https://github.com/libsdl-org/SDL) (zlib) and the Microsoft WebView2 SDK.
- **Shaders:** super-xBR and fast-bilateral by Hyllian, from
  [libretro/slang-shaders](https://github.com/libretro/slang-shaders) (MIT); FXAA after Timothy Lottes.
- **Xbox research:** [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase),
  [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded), [xemu](https://github.com/xemu-project/xemu) and
  [nxdk](https://github.com/XboxDev/nxdk), used as references (no code from Cxbx-Reloaded or xemu is included);
  [Ghidra](https://github.com/NationalSecurityAgency/ghidra) for reading the game's code.
- The HD pack's movie subtitles come from the 2019 remaster.
- Inspired by the Bloodborne native port (bbport).

Licence texts: [THIRD-PARTY.txt](THIRD-PARTY.txt).

## Licence

The port's own code is [MIT](LICENSE). Genma Onimusha is a trademark of Capcom; this project is not affiliated with
or endorsed by Capcom.
