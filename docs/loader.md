# How the native loader works

1. **`onimusha.exe`** (`src/ldr/ldr.c`) is linked at the fixed base `0x10000`. Its PE header, one code page and a
   uninitialised `.xbox` section (`0x564000` bytes) cover `0x10000–0x576000` — exactly the range `default.xbe` occupies —
   so Windows can never put a heap or DLL there. It loads `onimusha_host.dll` and calls `host_main`.
2. **Mapping** (`src/host/host.c`): reads `game\default.xbe`, makes the range RWX, copies the XBE header to
   `0x10000` and each section to its address.
3. **Hybrid header page**: Windows re-reads the exe's PE header after start-up (`CreateThread` fails with
   `ERROR_BAD_EXE_FORMAT` otherwise). `MZ` overwrites the first two bytes of `XBEH`, `e_lfanew` sits in the XBE's
   unused RSA signature, and the PE headers are copied to `0x10D00`, past the end of the XBE headers (`0xC28`).
4. **Kernel imports**: the 116 slots of the XBE's kernel thunk table get pointers to `src/host/kernel.c`.
5. **`fs:` rewrite**: 24 instructions read the Xbox kernel's per-CPU block through `fs:` (`[0x20]` PRCB,
   `[0x24]` IRQL, `[0x28]` current thread, `[0x4]` TLS anchor). Each is rewritten in place to read a Windows TLS
   slot (`fs:[0xE10 + 4*slot]`, same length) holding a fake per-thread value. `fs:[0]` (SEH chain) is the same on
   both systems and is left alone, so Xbox SEH and `RtlUnwind` work natively.
6. **`WBINVD`** (one site, privileged) becomes a 2-byte NOP.
7. **SDK hooks** (`src/host/hle.c`, list in `sdk_symbols.h`, made with XbSymbolDatabase): 215 D3D8 /
   DirectSound / input / memory-unit functions get a `jmp` to a native replacement, or to a stub that logs and
   returns 0 with the right stack cleanup.
8. **Patches** (`apply_patches` in `host.c`): each mod that is on and has a `patch.txt` (`mods\<name>\patch.txt`)
   has its byte patches written over the mapped image, after the hooks (see `mods\README.txt` and `src/host/mods_list.h`).
9. **Threads**: `PsCreateSystemThreadEx` makes a Windows thread whose trampoline sets the fake kernel-thread
   values (TLS block, stack base, thread id) before calling XAPI's thread startup.

## Files and drives
Xbox paths are translated in `kernel.c`: `D:` → `game\`, `\Device\Harddisk0\Partition1` (E:, user data) → `saves\`, and the utility drive `Z:` (Partition5) is
virtual: its names are `D:` names, its folders "already exist" and a file opened to write it goes to the null device (the
game's copies from the DVD are thrown away; reading one gets the disc's file). No other partition is used. A file on `D:` is looked up first
in the randomizer run being played (`randomizer\<run>\files\`, its shuffled room files), then in each enabled
`mods\<name>\`, in the order of the launcher's list (`mods\README.txt`), then in `game\` (`src/host/overlay.h`). A run
also has its own E: (`randomizer\<run>\saves\`), so its saves never meet the normal game's. With `text_language` set to
French or German, the disc's English text files are swapped for the European copies that are also on it.
XAPI creates `T:`/`U:` itself through `IoCreateSymbolicLink`; `Z:` comes from our `XMountUtilityDrive`. File I/O is passed through to ntdll, since
Xbox and NT share handles, info classes and status codes. Handles are opened with extra attribute/synchronise
rights because NT enforces access the Xbox kernel doesn't.

Movies are the same lookup with extras (`open_file` in `kernel.c`, `src/host/movie.c`): when the game opens a
`.bik`, an HD replacement `mods\<mod>\movies\<name>.mp4` (or `<name>_*.mp4`, another cut of it) is used if it has
the same number of frames, a `<name>.wav` beside the `.bik` replaces its sound, and subtitles come from
`<name>.<lang>.srt` beside it or in a mod's `subtitles\` folder (`<lang>` is en, fr, de, or ja for Japanese
subtitles). Texture packs (`texpack.c`, a mod's `textures\` folder: png, dds or jpg) and the enhanced-text sheet
(`font\text_sheet_e.bin` in a mod, `text.c`) look in the enabled mods the same way.

## Running
```
cmake -S . -B build -A Win32
python -I tools/xbe_tool.py prepare "Genma Onimusha (USA) (En,Ja).iso" build/RelWithDebInfo/game   # once
cmake --build build --config RelWithDebInfo
build\RelWithDebInfo\bin\onimusha.exe                                  # logs\onimusha.log says what happened
```
