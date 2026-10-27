# The Linux launcher: GenmaOnimusha, a native x86_64 program showing the Windows launcher's page (launcher/ui) in
# Qt 6 WebEngine, with launcher/main.cpp's native side ported (launcher/linux/main.cpp). Qt WebEngine, not WebKitGTK:
# SteamOS ships Qt 6 WebEngine and no WebKitGTK. It's the 64-bit configure of this repository (the game is the
# 32-bit one, src/linux/linux.cmake); built against Ubuntu 22.04's Qt 6.2, it runs on any later Qt 6 (SteamOS: 6.8).
#   sudo apt install qt6-base-dev qt6-webengine-dev libqt6webchannel6-dev libqt6opengl6-dev libgl-dev
#   cmake -S . -B build-launcher -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && ninja -C build-launcher
# Output: build-launcher/out/GenmaOnimusha, which goes in the install's top folder, beside bin/.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network WebChannel WebEngineCore WebEngineWidgets)
add_executable(launcher launcher/linux/main.cpp launcher/linux/windows.h)
set_target_properties(launcher PROPERTIES OUTPUT_NAME GenmaOnimusha RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
# Built in: the page (:/ui/index.html), the randomizer's data (:/randomizer.dat) and the icon (:/app.ico).
set_source_files_properties(launcher/ui/index.html PROPERTIES QT_RESOURCE_ALIAS ui/index.html)
set_source_files_properties(launcher/randomizer.dat PROPERTIES QT_RESOURCE_ALIAS randomizer.dat)
set_source_files_properties(launcher/app.ico PROPERTIES QT_RESOURCE_ALIAS app.ico)
qt_add_resources(launcher launcher_files PREFIX / FILES launcher/ui/index.html launcher/randomizer.dat launcher/app.ico)
target_include_directories(launcher PRIVATE ${CMAKE_SOURCE_DIR}/launcher/linux)   # its <windows.h>, for the shared headers
target_link_libraries(launcher PRIVATE Qt6::Widgets Qt6::Network Qt6::WebChannel Qt6::WebEngineCore Qt6::WebEngineWidgets)
target_compile_options(launcher PRIVATE -Wall -Wno-unknown-pragmas -Wno-unused-function -Wno-unused-variable -Wno-sign-compare -Wno-format-truncation)
# The release workflow sets these, as for the Windows launcher; a local build is "0.7", never checks for updates and
# has the debug switches (ONI_DEBUG: ONI_LAUNCHER_UI, ONI_UPDATE_URL, ONI_PACK_URL, plain-HTTP downloads).
set(ONI_VERSION "0.7" CACHE STRING "Version shown by the launcher and compared against releases")
set(ONI_UPDATE_REPO "" CACHE STRING "owner/name of the GitHub repository whose releases the launcher checks (empty: none)")
set(ONI_PACK_REPO "sloppinheimer/genma-onimusha-hd-pack" CACHE STRING "owner/name of the GitHub repository whose latest release holds the HD pack (empty: no downloads)")
target_compile_definitions(launcher PRIVATE _GNU_SOURCE ONI_VERSION="${ONI_VERSION}" ONI_UPDATE_REPO="${ONI_UPDATE_REPO}" ONI_PACK_REPO="${ONI_PACK_REPO}")
if(ONI_UPDATE_REPO STREQUAL "")
    target_compile_definitions(launcher PRIVATE ONI_DEBUG)
endif()
