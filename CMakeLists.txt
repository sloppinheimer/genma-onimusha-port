cmake_minimum_required(VERSION 3.20)
project(genma_onimusha C CXX)

# Linux, 64-bit: the launcher (GenmaOnimusha, Qt 6 WebEngine: launcher/linux/launcher.cmake). The game is the 32-bit
# configure below.
if(NOT WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    include(launcher/linux/launcher.cmake)
    return()
endif()

# The game is 32-bit x86 code running natively: everything here must be 32-bit.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure for 32-bit: cmake -A Win32 (Linux: -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32)")
endif()
if(NOT WIN32)   # Linux: its own build (src/linux/linux.cmake)
    set(CMAKE_C_STANDARD 11)
    include(src/linux/linux.cmake)
    return()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# onimusha.exe: fixed at 0x10000 so it owns the Xbox image's address range (see src/ldr/ldr.c).
add_executable(onimusha src/ldr/ldr.c src/ldr/ldr.rc)
target_compile_options(onimusha PRIVATE /GS- /O1 /utf-8)
target_link_options(onimusha PRIVATE /BASE:0x10000 /FIXED /DYNAMICBASE:NO /NODEFAULTLIB /ENTRY:start
    /SUBSYSTEM:WINDOWS /SAFESEH:NO /LARGEADDRESSAWARE)   # no console window; console = 1 opens one (host.c)
target_link_libraries(onimusha PRIVATE onimusha_host)   # host_main, an ordinary import (ldr.c)

# The runtime: XBE loader, Xbox kernel, SDK replacements.
add_library(onimusha_host SHARED src/host/host.c src/host/log.c src/host/kernel.c src/host/hle.c src/host/d3d.c src/host/d3d_overlay.c src/host/d3d_pack.c src/host/d3d_shaders.c src/host/d3d_post.c src/host/d3d_capture.c src/host/d3d_modern.c src/host/d3d_interp.c src/host/d3d_texture.c src/host/input.c src/host/dsound.c src/host/vsh.c src/host/report.c src/host/settings.c src/host/psh.c src/host/text.c src/host/achievements.c src/host/mapping.c src/host/randomizer.c src/host/cheats.c src/host/texpack.c src/host/movie.c src/host/host.rc)
# The build folder (build/<config>) is laid out like the release (src/host/layout.h): the launcher on top, the game in
# bin/, third-party licences in bin/licenses/.
set(OUT "${CMAKE_BINARY_DIR}/$<CONFIG>")
set_target_properties(onimusha onimusha_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUT}/bin")
add_custom_command(TARGET onimusha_host PRE_LINK
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OUT}/bin ${OUT}/bin/licenses)
# /utf-8: the sources are UTF-8 (comments, a few strings), read the same on any machine's code page
target_compile_options(onimusha_host PRIVATE /W3 /wd4100 /wd4996 /Oy- /utf-8)   # /Oy-: frame pointers for xbacktrace
# Xbox SEH handlers live in the game image; SafeSEH would reject them.
target_link_options(onimusha_host PRIVATE /SAFESEH:NO /MAP)

# SDL3 (zlib licence) reads PlayStation, Switch and other non-XInput controllers: the official VC package, fetched
# at configure time. Delay-loaded, so without SDL3.dll the host falls back to XInput (see src/host/input.c).
include(FetchContent)
FetchContent_Declare(sdl3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-devel-3.4.18-VC.zip
    URL_HASH SHA256=78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sdl3)
target_include_directories(onimusha_host PRIVATE ${sdl3_SOURCE_DIR}/include)
target_link_libraries(onimusha_host PRIVATE ${sdl3_SOURCE_DIR}/lib/x86/SDL3.lib delayimp)
target_link_options(onimusha_host PRIVATE /DELAYLOAD:SDL3.dll)
add_custom_command(TARGET onimusha_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${sdl3_SOURCE_DIR}/lib/x86/SDL3.dll $<TARGET_FILE_DIR:onimusha_host>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${sdl3_SOURCE_DIR}/LICENSE.txt ${OUT}/bin/licenses/SDL3-LICENSE.txt)
# KabutoSans.ttf (tools/font) is built into onimusha_host.dll (host.rc): enhanced_text draws the game's font sheet with
# it (src/host/text.c). OFL; its sources' licences go with it.
add_custom_command(TARGET onimusha_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/tools/font/sources/DidactGothic-OFL.txt ${OUT}/bin/licenses/KabutoSans-OFL.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/tools/font/sources/KosugiMaru-LICENSE.txt ${OUT}/bin/licenses/KabutoSans-KosugiMaru-LICENSE.txt)
# THIRD-PARTY.txt: the notices of code compiled into onimusha_host.dll (the super-xBR / fast-bilateral shaders, MIT).
add_custom_command(TARGET onimusha_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/THIRD-PARTY.txt ${OUT}/bin/licenses/THIRD-PARTY.txt)
# mods/: the built-in mods' folders (mod.ini with builtin = <id>, see src/host/mods_list.h) and a readme, next to the launcher.
add_custom_command(TARGET onimusha_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/mods ${OUT}/mods)
# ... and on every build, not only when onimusha_host relinks: an edit to mods/ alone left the build's copy as it was.
# (Copied over, never cleared: the build folder's mods\ may hold test mods. A file deleted from mods/ stays there.)
add_custom_target(builtin_mods ALL COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/mods ${OUT}/mods VERBATIM)

# GenmaOnimusha.exe: the launcher. Its page is ui/index.html, shown with WebView2 (SDK in third_party/webview2, from NuGet).
set(WEBVIEW2 ${CMAKE_SOURCE_DIR}/third_party/webview2/pkg/build/native)
if(NOT EXISTS ${WEBVIEW2}/include/WebView2.h)
    message(WARNING "WebView2 SDK missing, so the launcher won't build: unzip the Microsoft.Web.WebView2 NuGet package "
        "(the version and hash in .github/workflows/release.yml) into third_party/webview2/pkg")
endif()
add_executable(launcher WIN32 launcher/main.cpp launcher/launcher.rc)
set_target_properties(launcher PROPERTIES OUTPUT_NAME GenmaOnimusha CXX_STANDARD 17)
set_source_files_properties(launcher/launcher.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/launcher/version.rc2;${CMAKE_SOURCE_DIR}/launcher/ui/index.html;${CMAKE_SOURCE_DIR}/launcher/app.ico;${CMAKE_SOURCE_DIR}/launcher/randomizer.dat")
set_source_files_properties(src/ldr/ldr.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/launcher/app.ico;${CMAKE_SOURCE_DIR}/launcher/version.rc2")
set_source_files_properties(src/host/host.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/launcher/app.ico;${CMAKE_SOURCE_DIR}/launcher/version.rc2;${CMAKE_SOURCE_DIR}/tools/font/KabutoSans.ttf")
target_include_directories(launcher PRIVATE ${WEBVIEW2}/include)
# The release workflow sets these from the tag and the GitHub repository; a local build is "0.7" and never updates.
set(ONI_VERSION "0.7" CACHE STRING "Version shown by the launcher and compared against releases")
set(ONI_UPDATE_REPO "" CACHE STRING "owner/name of the GitHub repository the launcher updates from (empty: no updates)")
set(ONI_PACK_REPO "sloppinheimer/genma-onimusha-hd-pack" CACHE STRING "owner/name of the GitHub repository whose latest release holds the HD pack (empty: no downloads)")
string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" ONI_VERSION_NUMS "${ONI_VERSION}")   # the numbers only: "0.9-rc1" is 0.9, not 0.9.1
string(REGEX MATCHALL "[0-9]+" ONI_VERSION_PARTS "${ONI_VERSION_NUMS}")
list(APPEND ONI_VERSION_PARTS 0 0 0)   # "0.7" -> 0,7,0; "dev" -> 0,0,0
list(GET ONI_VERSION_PARTS 0 ONI_V0)
list(GET ONI_VERSION_PARTS 1 ONI_V1)
list(GET ONI_VERSION_PARTS 2 ONI_V2)
target_compile_definitions(launcher PRIVATE UNICODE _UNICODE ONI_VERSION="${ONI_VERSION}" ONI_UPDATE_REPO="${ONI_UPDATE_REPO}" ONI_PACK_REPO="${ONI_PACK_REPO}"
    ONI_VERSION_NUM=${ONI_V0},${ONI_V1},${ONI_V2},0)
# ONI_DEBUG: the debug switches that write game memory, saves or files, the launcher's URL and page overrides, DevTools
# and plain-HTTP downloads. On for a local build; off for a release (the workflow sets ONI_UPDATE_REPO).
if(ONI_UPDATE_REPO STREQUAL "")
    target_compile_definitions(onimusha_host PRIVATE ONI_DEBUG)
    target_compile_definitions(launcher PRIVATE ONI_DEBUG)
    # src/host/dev/: development tools kept out of the public repository; built when they're there
    file(GLOB ONI_DEV_SOURCES ${CMAKE_SOURCE_DIR}/src/host/dev/*.c)
    if(ONI_DEV_SOURCES)
        target_sources(onimusha_host PRIVATE ${ONI_DEV_SOURCES})
        target_compile_definitions(onimusha_host PRIVATE ONI_DEV_TOOLS)
    endif()
endif()
# the game's exe and DLL carry the launcher's version details too (launcher/version.rc2)
set_property(SOURCE src/ldr/ldr.rc src/host/host.rc APPEND PROPERTY COMPILE_DEFINITIONS ONI_VERSION="${ONI_VERSION}"
    ONI_VERSION_NUM=${ONI_V0},${ONI_V1},${ONI_V2},0)
target_compile_options(launcher PRIVATE /W3 /wd4996 /utf-8)
target_link_libraries(launcher PRIVATE ${WEBVIEW2}/x86/WebView2LoaderStatic.lib advapi32 winhttp)
# /RELEASE: a real header checksum in all three programs (an empty one is another mark against an unsigned program)
foreach(t onimusha onimusha_host launcher)
    target_link_options(${t} PRIVATE /RELEASE /INCREMENTAL:NO)
endforeach()

# selftest.exe: checks of the shared pure code (tests/selftest.cpp). Not shipped; the release workflow runs it.
add_executable(selftest tests/selftest.cpp)
target_compile_definitions(selftest PRIVATE ONI_SOURCE_DIR="${CMAKE_SOURCE_DIR}")   # it reads launcher/randomizer.dat
set_target_properties(selftest PROPERTIES CXX_STANDARD 17)
target_compile_options(selftest PRIVATE /W3 /wd4996 /utf-8)

# rando_gen.exe: the randomizer's generator (launcher/randomizer.h) from the command line, to make and check seeds.
# Not shipped.
add_executable(rando_gen tools/rando/gen.cpp)
set_target_properties(rando_gen PROPERTIES CXX_STANDARD 17)
target_compile_options(rando_gen PRIVATE /W3 /wd4996 /utf-8)
