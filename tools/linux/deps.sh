#!/bin/bash
# The Linux build's 32-bit libraries, built from source into PREFIX (default /opt/oni/i386, linux.cmake's
# ONI_LINUX_DEPS): SDL3 (window, input, sound), FAudio (XAudio2 on SDL3), DXVK Native (Direct3D 9 on Vulkan, SDL3
# window), vkd3d-shader (D3DCompile) and FFmpeg (the HD movies: libavformat, libavcodec, libswscale, libavutil). They
# ship beside the executable in bin/.
#   tools/linux/deps.sh [PREFIX [SOURCES]]
# Written on Ubuntu 22.04 (and WSL). It needs:
#   sudo dpkg --add-architecture i386 && sudo apt update
#   sudo apt install gcc-multilib g++-multilib cmake ninja-build curl libvulkan-dev:i386 \
#     libx11-dev:i386 libxext-dev:i386 libxcursor-dev:i386 libxi-dev:i386 libxfixes-dev:i386 libxrandr-dev:i386 \
#     libxss-dev:i386 libxtst-dev:i386 libxinerama-dev:i386 libxkbcommon-dev:i386 libxcb1-dev:i386 \
#     libwayland-dev:i386 libdecor-0-dev:i386 libegl-dev:i386 libgl-dev:i386 libgbm-dev:i386 libdrm-dev:i386 \
#     libasound2-dev:i386 libpulse-dev:i386 libpipewire-0.3-dev:i386 libdbus-1-dev:i386 libibus-1.0-dev:i386 \
#     libudev-dev:i386 libusb-1.0-0-dev:i386 flex bison libjson-perl nasm
#   pip install meson   (DXVK needs a newer one than 22.04's)
set -e
P=${1:-/opt/oni/i386}; S=${2:-/opt/oni/src}
mkdir -p "$P" "$S"
export PKG_CONFIG_PATH=$P/lib/pkgconfig:/usr/lib/i386-linux-gnu/pkgconfig PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig:$P/lib/pkgconfig
cd "$S"
[ -d SDL ] || git clone -q --depth 1 -b release-3.4.18 https://github.com/libsdl-org/SDL.git
[ -d FAudio ] || git clone -q --depth 1 -b 26.10 https://github.com/FNA-XNA/FAudio.git
[ -d dxvk ] || git clone -q --depth 1 -b v3.1.1 --recurse-submodules https://github.com/doitsujin/dxvk.git
[ -d glslang ] || git clone -q --depth 1 -b 16.6.0 https://github.com/KhronosGroup/glslang.git
[ -f vkd3d-1.19.tar.xz ] || curl -sfLO https://dl.winehq.org/vkd3d/source/vkd3d-1.19.tar.xz
[ -f ffmpeg-9.0.2.tar.xz ] || curl -sfLO https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
echo "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e  ffmpeg-9.0.2.tar.xz" | sha256sum -c --quiet

# DXVK's own shaders are compiled with it at build time. 22.04's glslang (11.8) writes SPIR-V that Mesa rejects
# (OpTypeSampledImage of a texel buffer, invalid in SPIR-V 1.6): RADV crashes and lavapipe fails on DXVK's first
# pipeline. A build tool for this machine, not shipped.
echo "== glslang"
cmake -S glslang -B glslang/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF \
  -DGLSLANG_ENABLE_INSTALL=OFF > glslang/b.log 2>&1 || { tail -30 glslang/b.log; exit 1; }
ninja -C glslang/b glslang-standalone >> glslang/b.log 2>&1 || { tail -30 glslang/b.log; exit 1; }

echo "== SDL3"
cmake -S SDL -B SDL/b32 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 \
  -DCMAKE_INSTALL_PREFIX="$P" -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF > SDL/b32.log 2>&1 || { tail -30 SDL/b32.log; exit 1; }
ninja -C SDL/b32 install >> SDL/b32.log 2>&1 || { tail -30 SDL/b32.log; exit 1; }

echo "== FAudio"
cmake -S FAudio -B FAudio/b32 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-m32 -DCMAKE_PREFIX_PATH="$P" \
  -DCMAKE_INSTALL_PREFIX="$P" -DBUILD_SDL3=ON > FAudio/b32.log 2>&1 || { tail -30 FAudio/b32.log; exit 1; }
ninja -C FAudio/b32 install >> FAudio/b32.log 2>&1 || { tail -30 FAudio/b32.log; exit 1; }

echo "== DXVK Native"
cat > linux32.txt <<EOF
[binaries]
c = 'gcc'
cpp = 'g++'
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'
glslang = '$S/glslang/b/StandAlone/glslang'
[built-in options]
c_args = ['-m32']
cpp_args = ['-m32']
c_link_args = ['-m32']
cpp_link_args = ['-m32']
[host_machine]
system = 'linux'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
EOF
(cd dxvk && rm -rf b32 && meson setup b32 --cross-file ../linux32.txt --prefix "$P" --buildtype release -Dnative_sdl3=enabled \
  -Dnative_sdl2=disabled -Dnative_glfw=disabled > b32.log 2>&1 && ninja -C b32 install >> b32.log 2>&1) \
  || { tail -40 dxvk/b32.log; exit 1; }

# Only the shader library is built; the Vulkan and SPIR-V headers are DXVK's
# (newer than 22.04's).
echo "== vkd3d-shader"
rm -rf vkd3d-1.19 && tar xf vkd3d-1.19.tar.xz
(cd vkd3d-1.19 && ./configure --host=i686-linux-gnu CC="gcc -m32" CFLAGS="-O2" \
  CPPFLAGS="-I$S/dxvk/include/vulkan/include -I$S/dxvk/include/spirv/include" --prefix="$P" --disable-tests \
  --disable-demos --without-ncurses --without-xcb --without-spirv-tools --disable-doxygen-doc > build.log 2>&1 \
  && make include/private/spirv_grammar.h include/private/vkd3d_version.h libs/vkd3d-shader/hlsl.tab.h libs/vkd3d-shader/preproc.tab.h >> build.log 2>&1 \
  && make -j"$(nproc)" libvkd3d-shader.la >> build.log 2>&1) || { tail -30 vkd3d-1.19/build.log; exit 1; }
cp -a vkd3d-1.19/.libs/libvkd3d-shader.so* "$P/lib/"

# FFmpeg for the HD movies (src/linux/movie_ff.c, which loads it when a movie first needs it): the MP4 demuxer and
# the H.264 decoder only, no encoders, filters, devices, network or outside libraries. LGPL 2.1 or later (no
# --enable-gpl or --enable-nonfree). The release tarball, checked against the hash above (its signature, by FFmpeg's
# release key FCF986EA15E6E293A5644F10B4322F04D67658D8, was checked when the hash was taken).
echo "== FFmpeg"
rm -rf ffmpeg-9.0.2 && tar xf ffmpeg-9.0.2.tar.xz
(cd ffmpeg-9.0.2 && ./configure --prefix="$P" --arch=x86 --cc="gcc -m32" --extra-ldflags=-m32 --enable-shared \
  --disable-static --disable-programs --disable-doc --disable-debug --disable-avdevice --disable-avfilter \
  --disable-swresample --disable-network --disable-autodetect --enable-pthreads --disable-everything \
  --enable-protocol=file --enable-demuxer=mov --enable-decoder=h264 --enable-parser=h264 > build.log 2>&1 \
  && make -j"$(nproc)" >> build.log 2>&1 && make install >> build.log 2>&1) || { tail -30 ffmpeg-9.0.2/build.log; exit 1; }
echo "== done"; ls "$P/lib"
