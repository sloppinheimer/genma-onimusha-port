# Linux: the game as one 32-bit executable, bin/onimusha (the loader and the runtime together), on DXVK Native
# (Direct3D 9 on Vulkan), SDL3 and FAudio built for i386 into ONI_LINUX_DEPS by tools/linux/deps.sh (with vkd3d-shader,
# loaded when a shader is first compiled, and FFmpeg, loaded when an HD movie first plays: only its headers are used
# here). The host code is the Windows build's; src/linux/ supplies the Win32 and NT calls it makes (compat.h).
#   cmake -S . -B build-linux -G Ninja -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_BUILD_TYPE=RelWithDebInfo
set(ONI_LINUX_DEPS /opt/oni/i386 CACHE PATH "Prefix of the i386 SDL3, DXVK Native and FAudio builds")
file(GLOB ONI_HOST_SOURCES ${CMAKE_SOURCE_DIR}/src/host/*.c)
file(GLOB ONI_LINUX_SOURCES ${CMAKE_SOURCE_DIR}/src/linux/*.c)
add_executable(onimusha ${ONI_HOST_SOURCES} ${ONI_LINUX_SOURCES})
set(OUT "${CMAKE_BINARY_DIR}/out")
# $ORIGIN: the libraries ship beside the executable, and only there is searched (not the dependency prefix)
set_target_properties(onimusha PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUT}/bin" BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
target_include_directories(onimusha PRIVATE ${CMAKE_SOURCE_DIR}/src/linux/include ${ONI_LINUX_DEPS}/include/dxvk ${ONI_LINUX_DEPS}/include)
# <windows.h> (src/linux/include) first in every file: the host code's own includes then find it already there.
# -mincoming-stack-boundary=2: the game's code (and the Xbox SDK's) keeps the stack 4-byte aligned, the i386 Linux
# libraries expect 16 (DXVK's SSE stores fault on less), so the host realigns where it's called from the game.
# -msse2 -mfpmath=sse: as MSVC builds the Windows host (/arch:SSE2, its x86 default); without them the host was x87
# and had no SIMD (stb_image's SSE2 JPEG paths compiled out, d3d_pack.c's DXT1 blocks can't build).
target_compile_options(onimusha PRIVATE -m32 -msse2 -mfpmath=sse -mincoming-stack-boundary=2 -include ${CMAKE_SOURCE_DIR}/src/linux/include/windows.h -fno-omit-frame-pointer
    -fno-strict-aliasing -fms-extensions -Wall -Wno-unused-function -Wno-format -Wno-unknown-pragmas -Wno-missing-braces
    -Wno-unused-variable -Wno-pointer-sign -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-multichar)
target_compile_definitions(onimusha PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 COBJMACROS CINTERFACE)
# Kabuto Sans (tools/font), built into the executable as host.rc builds it into the DLL (compat_gdi.c, FindResourceW)
set(ONI_KABUTO_TTF ${CMAKE_SOURCE_DIR}/tools/font/KabutoSans.ttf)
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/linux/compat_gdi.c PROPERTIES
    COMPILE_DEFINITIONS "ONI_KABUTO_TTF=\"${ONI_KABUTO_TTF}\"" OBJECT_DEPENDS ${ONI_KABUTO_TTF})
# The debug switches (ONI_DEBUG) and src/host/dev/ only in a local build, as on Windows (CMakeLists.txt): a release
# build is configured with -DONI_UPDATE_REPO=owner/name.
if("${ONI_UPDATE_REPO}" STREQUAL "")
    target_compile_definitions(onimusha PRIVATE ONI_DEBUG)
    file(GLOB ONI_DEV_SOURCES ${CMAKE_SOURCE_DIR}/src/host/dev/*.c)
    if(ONI_DEV_SOURCES)
        target_sources(onimusha PRIVATE ${ONI_DEV_SOURCES})
        target_compile_definitions(onimusha PRIVATE ONI_DEV_TOOLS)
    endif()
endif()
# no-pie: a fixed base well above the Xbox range
target_link_options(onimusha PRIVATE -m32 -no-pie)
target_link_libraries(onimusha PRIVATE ${ONI_LINUX_DEPS}/lib/libdxvk_d3d9.so ${ONI_LINUX_DEPS}/lib/libSDL3.so
    ${ONI_LINUX_DEPS}/lib/libFAudio.so pthread m dl)
