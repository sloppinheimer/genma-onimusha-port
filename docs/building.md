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

The game's hard-disk cache (its Z: drive, where the Xbox copied files from the DVD) is virtual: copies into it are
thrown away and reads get the files in `game\` (or a mod's), so nothing is duplicated. See `docs/loader.md`.

## The launcher's page
The page is `launcher/ui/index.html`, built into the exe; the art on it is decoded from the player's own disc at
install time, so none ships with the port. To work on the page, open `launcher/ui/index.html` through a local web
server: without the launcher it runs on made-up state (`?nosetup` shows the not-installed screen, `?page=controls`
opens a section). `ONI_LAUNCHER_UI=<folder>` serves the page from a folder instead, `ONI_LAUNCHER_DEVTOOLS=1` allows
devtools. Each start writes step timings to `%LOCALAPPDATA%\Genma Onimusha\Launcher\startup.log` (normally the page
is up in ~0.3 s). The messages the page and the launcher exchange are listed at the top of `launcher/main.cpp`.

## Releases and updates
Pushing a tag like `v0.8` runs `.github/workflows/release.yml`: GitHub builds the port on Windows (WebView2 SDK and
SDL3 downloaded with pinned checksums), packs `GenmaOnimusha-0.8.zip` from an allow-list of built files and
licences (anything else fails the release, so no game data can slip in), and publishes it with its `.sha256`.

A launcher built that way knows its repository and version. At start it asks GitHub for the latest release; when
one is newer, About offers "Update to 0.8". The update downloads the zip, checks its SHA-256, unpacks it with
Windows' `tar.exe` and swaps the program files in place (a replaced file is kept as `*.old` until the next start,
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
