#!/bin/bash
# The Linux release from a build of src/linux/linux.cmake: OUTDIR/GenmaOnimusha-VERSION-linux.tar.gz and its .sha256.
#   tools/linux/package.sh VERSION OUTDIR [BUILD [SOURCES [LAUNCHER]]]
# BUILD is the build folder (default build-linux), SOURCES tools/linux/deps.sh's sources folder (default /opt/oni/src),
# where the built libraries' licences are read; the libraries themselves come from the build's ONI_LINUX_DEPS.
# LAUNCHER is the built Linux launcher (launcher/linux, a 64-bit build: its out/GenmaOnimusha), shipped on top as
# GenmaOnimusha; it uses the system's Qt 6 WebEngine, which SteamOS has. It needs patchelf.
# The archive holds one folder, GenmaOnimusha/, laid out like the Windows install (src/host/layout.h): genma.sh (the
# starter) and README.md on top, the game and every library it loads from beside it in bin/, their licences in
# bin/licenses/, the port's mods/, an empty saves/. Only those: anything else in it (game data, a log) fails the
# package, and so does a shipped file that needs a library neither in bin/ nor one SteamOS has.
set -e
V=$1 OUT=$2
[[ $V =~ ^[0-9A-Za-z._-]+$ && -n $OUT ]] || { echo "usage: $0 VERSION OUTDIR [BUILD [SOURCES [LAUNCHER]]]" >&2; exit 2; }
LAUNCHER=${5:-}
[ -z "$LAUNCHER" ] || [ -x "$LAUNCHER" ] || { echo "no launcher at $LAUNCHER" >&2; exit 1; }
R=$(cd "$(dirname "$0")/../.." && pwd)
B=$(cd "${3:-$R/build-linux}" && pwd)
S=${4:-/opt/oni/src}
P=$(sed -n 's/^ONI_LINUX_DEPS:PATH=//p' "$B/CMakeCache.txt")
SYS=/usr/lib/i386-linux-gnu   # the build machine's 32-bit libraries (Debian / Ubuntu)
VKD3D=$(sed -n 's/.*vkd3d-\([0-9.]*[0-9]\)\.tar\.xz.*/\1/p' "$R/tools/linux/deps.sh" | head -1)
[ -x "$B/out/bin/onimusha" ] && [ -d "$P/lib" ] && [ -n "$VKD3D" ] || { echo "no build in $B, or no ONI_LINUX_DEPS" >&2; exit 1; }
# THIRD-PARTY.txt says where vkd3d-shader's source is (LGPL): it has to name the version shipped
grep -q "vkd3d-$VKD3D.tar.xz" "$R/THIRD-PARTY.txt" || { echo "THIRD-PARTY.txt doesn't name vkd3d-$VKD3D.tar.xz" >&2; exit 1; }
# and FFmpeg's (LGPL too): the version deps.sh built
FFMPEG=$(cd "$S" && ls -d ffmpeg-*/ 2>/dev/null | head -1 | tr -d /)
[ -n "$FFMPEG" ] && grep -q "$FFMPEG.tar.xz" "$R/THIRD-PARTY.txt" || { echo "THIRD-PARTY.txt doesn't name ${FFMPEG:-ffmpeg}.tar.xz" >&2; exit 1; }

# The libraries bin/onimusha loads, all found in bin/ ($ORIGIN): deps.sh's, and the 32-bit X11 ones SDL3 opens that
# SteamOS doesn't have (Xcursor, XInput2, Xrandr; Xrender for the first and third; xkbcommon for Wayland).
DEPS="libSDL3.so.0 libdxvk_d3d9.so.0 libFAudio.so.0 libvkd3d-shader.so.1
  libavformat.so.63 libavcodec.so.63 libswscale.so.10 libavutil.so.61"   # the last four: FFmpeg, for the HD movies
X11="libXcursor.so.1 libXi.so.6 libXrandr.so.2 libXrender.so.1 libxkbcommon.so.0"
# What a shipped file may need from the system: libraries SteamOS has in 32-bit (/usr/lib32)
SYSTEM="libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 ld-linux.so.2 libvulkan.so.1 libX11.so.6 libXext.so.6
  libXfixes.so.3 libXss.so.1 libpulse.so.0 libpipewire-0.3.so.0 libasound.so.2 libudev.so.1 libdbus-1.so.3
  libstdc++.so.6 libgcc_s.so.1"
SYSTEM=" $(echo $SYSTEM) "
GAME_DATA='\.(xbe|iso|bgs|rdt|xpr|bik|wd|xsd|ldt|ida|bg|str|img|pss|sfd)$'   # tools/publish.py's list, and the movies

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
D=$T/GenmaOnimusha
mkdir -p "$D/bin/licenses"
install -m755 "$B/out/bin/onimusha" "$D/bin/onimusha"
strip "$D/bin/onimusha"
for l in $DEPS; do install -m755 "$P/lib/$l" "$D/bin/$l"; done   # install copies what a link points to
for l in $X11; do install -m755 "$SYS/$l" "$D/bin/$l"; done
# $ORIGIN in each library too: SDL3 opens the X11 libraries itself (dlopen looks in its caller's runpath, not the
# executable's), and Xcursor and Xrandr need Xrender
for l in $DEPS $X11; do
  strip --strip-unneeded "$D/bin/$l"
  patchelf --set-rpath '$ORIGIN' "$D/bin/$l"
done

# The licences, as bin/licenses/<name>: deps.sh's libraries' from their sources, the X11 ones from their packages
LICS=
lic() { [ -f "$2" ] || { echo "licence missing: $2" >&2; exit 1; }; install -m644 "$2" "$D/bin/licenses/$1"; LICS="$LICS $1"; }
lic SDL3-LICENSE.txt "$S/SDL/LICENSE.txt"
lic DXVK-LICENSE.txt "$S/dxvk/LICENSE"
lic DXVK-libdisplay-info-LICENSE.txt "$S/dxvk/subprojects/libdisplay-info/LICENSE"   # built into libdxvk_d3d9
lic DXVK-dxbc-spirv-LICENSE.txt "$S/dxvk/subprojects/dxbc-spirv/LICENSE"             # the same
lic FAudio-LICENSE.txt "$S/FAudio/LICENSE"
lic vkd3d-COPYING.txt "$S/vkd3d-$VKD3D/COPYING"
lic vkd3d-AUTHORS.txt "$S/vkd3d-$VKD3D/AUTHORS"
lic vkd3d-LICENSE.txt "$S/vkd3d-$VKD3D/LICENSE"   # the LGPL 2.1
lic FFmpeg-LICENSE.md "$S/$FFMPEG/LICENSE.md"
lic FFmpeg-COPYING.LGPLv2.1.txt "$S/$FFMPEG/COPYING.LGPLv2.1"
for l in $X11; do
  pkg=$(dpkg -S "$SYS/$l" | cut -d: -f1)
  lic "${l%%.so*}-LICENSE.txt" "/usr/share/doc/$pkg/copyright"
done
lic KabutoSans-OFL.txt "$R/tools/font/sources/DidactGothic-OFL.txt"   # as on Windows: the font built into the game
lic KabutoSans-KosugiMaru-LICENSE.txt "$R/tools/font/sources/KosugiMaru-LICENSE.txt"
lic THIRD-PARTY.txt "$R/THIRD-PARTY.txt"

# The launcher: 64-bit, the system's Qt 6 (the libraries it may need are checked below)
if [ -n "$LAUNCHER" ]; then install -m755 "$LAUNCHER" "$D/GenmaOnimusha"; strip "$D/GenmaOnimusha"; fi
cat > "$D/genma.sh" <<'EOF'
#!/bin/sh
# Starts the game however it's called (a terminal, a shortcut, Steam): the install is the folder this script is in.
cd "$(dirname "$(readlink -f "$0")")" && exec ./bin/onimusha "$@"
EOF
chmod 755 "$D/genma.sh"
install -m644 "$R/README.md" "$D/README.md"
install -m644 "$R/LICENSE" "$D/LICENSE.txt"
# mods/: the repo's tracked files, as in the Windows package (without git, the folder as it is)
MODS=$(git -C "$R" ls-files mods 2>/dev/null) || MODS=$(cd "$R" && find mods -type f | sort)
while IFS= read -r f; do [ -z "$f" ] || install -D -m644 "$R/$f" "$D/$f"; done <<< "$MODS"
# saves/, empty: the game opens it (the Xbox's E: partition) before anything makes it, and quits when it isn't there.
# Extracting a newer archive over an install leaves what's in it alone.
mkdir "$D/saves"

# Checks. The allow-list, by path: exactly these files, nothing else and no links.
EXPECT=$( { printf '%s\n' genma.sh README.md LICENSE.txt bin/onimusha; for l in $DEPS $X11; do echo "bin/$l"; done
            [ -z "$LAUNCHER" ] || echo GenmaOnimusha
            for l in $LICS; do echo "bin/licenses/$l"; done; [ -z "$MODS" ] || echo "$MODS"; } | sort)
GOT=$(cd "$D" && find . ! -type d | sed 's|^\./||' | sort)
EXTRA=$(comm -13 <(echo "$EXPECT") <(echo "$GOT")) MISSING=$(comm -23 <(echo "$EXPECT") <(echo "$GOT"))
[ -z "$EXTRA$MISSING" ] || { echo "unexpected files: ${EXTRA:-none}; missing: ${MISSING:-none}" >&2; exit 1; }
[ -z "$(cd "$D" && find . ! -type d ! -type f)" ] || { echo "links or special files in the package" >&2; exit 1; }
! grep -iE "$GAME_DATA" <<< "$GOT" || { echo "game data in the package" >&2; exit 1; }
# Every program file: 32-bit x86, loads from bin/, and needs only bin/'s and SteamOS's libraries
for f in bin/onimusha $(for l in $DEPS $X11; do echo "bin/$l"; done); do
  h=$(readelf -h "$D/$f") dy=$(readelf -d "$D/$f")
  grep -q 'Class:.*ELF32' <<< "$h" && grep -q 'Machine:.*80386' <<< "$h" || { echo "$f isn't 32-bit x86" >&2; exit 1; }
  grep -q 'RUNPATH.*\[\$ORIGIN\]' <<< "$dy" || { echo "$f doesn't load from \$ORIGIN" >&2; exit 1; }
  for n in $(sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' <<< "$dy"); do
    [ -f "$D/bin/$n" ] || [[ $SYSTEM == *" $n "* ]] || { echo "$f needs $n: neither shipped nor SteamOS's" >&2; exit 1; }
  done
done

# The launcher: 64-bit x86, and only Qt 6 and the C and C++ runtimes from the system
if [ -n "$LAUNCHER" ]; then
  h=$(readelf -h "$D/GenmaOnimusha")
  grep -q 'Class:.*ELF64' <<< "$h" && grep -q 'Machine:.*X86-64' <<< "$h" || { echo "GenmaOnimusha isn't 64-bit x86" >&2; exit 1; }
  for n in $(readelf -d "$D/GenmaOnimusha" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
    [[ $n =~ ^libQt6[A-Za-z]+\.so\.6$ || " libstdc++.so.6 libgcc_s.so.1 libc.so.6 libm.so.6 ld-linux-x86-64.so.2 " == *" $n "* ]] \
      || { echo "GenmaOnimusha needs $n: not Qt 6 or the system's runtime" >&2; exit 1; }
  done
fi

mkdir -p "$OUT"
A=GenmaOnimusha-$V-linux.tar.gz
tar -C "$T" --sort=name --owner=0 --group=0 --numeric-owner -czf "$OUT/$A" GenmaOnimusha
(cd "$OUT" && sha256sum "$A" > "$A.sha256")
tar -tvzf "$OUT/$A"
cat "$OUT/$A.sha256"
