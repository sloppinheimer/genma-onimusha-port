# Building and working on the port

## Build and run
Requires Visual Studio 2022 Build Tools (x86 compiler), CMake, Python 3, and for the launcher the WebView2 SDK:
the `Microsoft.Web.WebView2` NuGet package (version and SHA-256 in `.github/workflows/release.yml`) unzipped into
`third_party/webview2/pkg`. Without it the launcher target fails; `cmake --build build --config RelWithDebInfo
--target onimusha_host onimusha` builds the game alone.

```
cmake -S . -B build -A Win32
cmake --build build --config RelWithDebInfo
python -I tools/xbe_tool.py prepare "Genma Onimusha (USA) (En,Ja).iso" build/RelWithDebInfo/game
build\RelWithDebInfo\bin\onimusha.exe
```
The build folder is laid out like an install (`src/host/layout.h`): `GenmaOnimusha.exe` (the launcher) and
`README.md` on top; the game in `bin\` (its font built into `onimusha_host.dll`), third-party licences in
`bin\licenses\`; the player's `onimusha.ini` and `mods\` (with `mods.txt`, the mods' order and switches) on top too.
The game writes `saves\` (Xbox saves, achievements, play time), `cache\` (only with French or German text: two fixed
copies of disc files), `logs\`, `captures\` and `mapping\`; the launcher's Randomizer page makes `randomizer\`.
An install from before these folders (0.7 and older, everything in one folder) is moved into them by the launcher
or the game at their next start.

Checks: `build\RelWithDebInfo\selftest.exe` and `python -I tools/xbe_tool.py selftest` check the shared code (ini
lines, default settings, mods.txt, the launcher's JSON, version and name checks, the disc reader, the update swap,
save import, the randomizer's placement); the release runs both. `build\RelWithDebInfo\rando_gen.exe` makes
randomizer runs from the command line (`rando_gen DAT GAME_DIR OUT_DIR batch N [fs] [lc] [dw] [kc]` reports how many
of N seeds fail). `launcher/randomizer.dat` is the randomizer's data (item locations, doors and the logic), built into
the launcher.

The game's hard-disk cache (its Z: drive, where the Xbox copied files from the DVD) is virtual: the game's copies
into it aren't made (`hle.c`) and reads get the files in `game\` (or a mod's), so nothing is duplicated, and nothing
opened there or on `D:` can be written or deleted. See `docs/loader.md`.

## Linux
The native Linux build (`src/linux/linux.cmake`): one 32-bit executable, `bin/onimusha` (the loader and the runtime
together), on DXVK Native (Direct3D 9 on Vulkan), SDL3, FAudio and vkd3d-shader (HLSL); `src/linux/compat_*.c`
supply the Windows calls the host code makes. No Wine. It's built on Ubuntu 22.04 (WSL works) with the packages
listed at the top of `tools/linux/deps.sh`, which builds those four libraries from source into `/opt/oni/i386`:
```
bash tools/linux/deps.sh
cmake -S . -B build-linux -G Ninja -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build-linux
tools/linux/package.sh 0.9 dist
```
The executable finds its libraries only beside it (`$ORIGIN`), so it runs from the package:
`dist/GenmaOnimusha-0.9-linux.tar.gz` unpacks to `GenmaOnimusha/`, laid out like the Windows install: `genma.sh`
(the starter), `README.md`, `mods/` and an empty `saves/` (the game quits when it can't open it) on top; the game,
the four libraries and the 32-bit X11 libraries SteamOS doesn't have (Xcursor, Xi, Xrandr, Xrender, xkbcommon, from
the build machine) in `bin/`, their licences in `bin/licenses/`. `package.sh` (it needs `patchelf`) sets each
library's runpath to `$ORIGIN` too, since SDL3 opens the X11 libraries itself, and fails on any file not on its list
and on any library a shipped file needs that is neither in `bin/` nor one SteamOS has. `game/` is made as on Windows
(`xbe_tool.py prepare`); the game writes the same folders beside it.

## The launcher's page
The page is `launcher/ui/index.html`, built into the exe; the art on it is decoded from the player's own disc at
install time, so none ships with the port. To work on the page, open `launcher/ui/index.html` through a local web
server: without the launcher it runs on made-up state (`?nosetup` shows the not-installed screen, `?page=controls`
opens a section). `ONI_LAUNCHER_UI=<folder>` serves the page from a folder instead, `ONI_LAUNCHER_DEVTOOLS=1` allows
devtools. Each start writes step timings to `%LOCALAPPDATA%\Genma Onimusha\Launcher\startup.log` (normally the page
is up in ~0.3 s). The messages the page and the launcher exchange are listed at the top of `launcher/main.cpp`.

## The Linux launcher
On Linux the launcher is `GenmaOnimusha`, a native x86_64 program showing the same page in Qt 6 WebEngine (SteamOS
ships Qt 6 WebEngine and no WebKitGTK), with `launcher/main.cpp`'s native side ported in `launcher/linux/main.cpp`.
The shared code (`text.h`, `disc.h`, `files.h`, `randomizer.h`, `mods_list.h`, `layout.h`) is the Windows launcher's
own, run on POSIX through `launcher/linux/windows.h`, so an install, `onimusha.ini`, `mods.txt` and randomizer runs
come out as on Windows. It's the 64-bit configure of this repository (the game is the 32-bit one):

```
sudo apt install qt6-base-dev qt6-webengine-dev libqt6webchannel6-dev libqt6opengl6-dev libgl-dev   # Ubuntu 22.04
cmake -S . -B build-launcher -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build-launcher        # build-launcher/out/GenmaOnimusha: in an install it sits on top, beside bin/
```
Built against Ubuntu 22.04's Qt 6.2, it runs on any later Qt 6. A player needs their system's Qt 6 WebEngine:
SteamOS has it; elsewhere it's a package (Arch: `qt6-webengine`; Debian and Ubuntu: `libqt6webenginewidgets6` and
`libqt6webenginecore6-bin`). Without it the launcher doesn't start, and `genma.sh` starts the game on its own.

What differs from Windows: there's no self-update (with a repository built in, the launcher still checks the latest
release at start, and About's Update opens the release page); removed mods and runs go to the desktop's trash; the
desktop shortcut is a `.desktop` file, on the desktop and in the applications menu; zips are unpacked with `bsdtar`,
`unzip` or `python3`, whichever there is. Controllers are read from `/dev/input` (evdev), so a Steam Deck's controls
and Steam Input's virtual pad drive the page. The art from the disc and `startup.log` are in
`~/.local/share/genma-onimusha/launcher/`. `ONI_LAUNCHER_UI=<folder>` works as on Windows; for the devtools,
`QTWEBENGINE_REMOTE_DEBUGGING=127.0.0.1:9222` opens the page to a Chromium browser's `chrome://inspect`.

## Releases and updates
Pushing a tag like `v0.8` runs `.github/workflows/release.yml`: GitHub builds the port on Windows (WebView2 SDK and
SDL3 downloaded with pinned checksums), packs `GenmaOnimusha-0.8.zip` from an allow-list of built files and
licences (anything else fails the release, so no game data can slip in), and publishes it with its `.sha256`.
Then it builds the Linux port on Ubuntu 22.04 (`deps.sh`'s libraries are cached by the script's hash) and adds
`GenmaOnimusha-0.8-linux.tar.gz` and its `.sha256`, made by `tools/linux/package.sh`, to the same release.

A launcher built that way knows its repository and version. At start it asks GitHub for the latest release; when
one is newer, About offers "Update to 0.8". The update downloads the zip, checks its SHA-256, unpacks it with
`launcher/unzip.h` and swaps the program files in place (a replaced file is kept as `*.old` until the next start,
and a failed swap is put back), then restarts the launcher. `game\`, `saves\` and `onimusha.ini` are never in the
zip, so they're never touched. A local build is version 0.7 with no repository, so it never updates. To test the
updater against a fake release, `ONI_UPDATE_URL=<url of a release JSON>` checks that URL instead of GitHub.

## Mods, from the code's side
A **built-in mod** is a folder with `builtin = <id>` in its mod.ini: it switches on a tool written into the port
itself (development builds have one, for making the HD pack). The port's own built-in mods live in `mods/` in this repo and are copied next to
the exe by the build; the code asks `builtin_mod_on` (`src/host/host.h`) whether one is on. Gameplay toggles
(`modern_controls`, `weapon_wheel`) are plain `[mods]` settings instead. `mods.txt` (kept by the launcher) holds the
mods' order and what's off; see `src/host/mods_list.h`. The game's memory addresses mods and cheats use are in
`docs/mods.md`.

Debugging and the logs: `docs/debugging.md`.
