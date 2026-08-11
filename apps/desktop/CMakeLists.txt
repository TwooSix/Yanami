cmake_minimum_required(VERSION 3.24)

project(YanamiDesktop VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Concurrent Gui Quick QuickControls2 OpenGL LinguistTools)

# The MSYS2 build of pkg-config may expose POSIX paths such as
# /ucrt64/include to native Windows CMake. Locate the import library directly
# on Windows so the same project can be configured from PowerShell. Other
# platforms continue to use the canonical mpv.pc metadata.
if(WIN32)
    find_path(MPV_INCLUDE_DIR
        NAMES mpv/client.h
        REQUIRED
    )
    find_library(MPV_LIBRARY
        NAMES mpv
        REQUIRED
    )
    add_library(MPV::MPV UNKNOWN IMPORTED)
    set_target_properties(MPV::MPV PROPERTIES
        IMPORTED_LOCATION "${MPV_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}"
    )

    get_filename_component(YANAMI_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)
    find_program(WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt6 windeployqt
        HINTS "${YANAMI_QT_PREFIX}/bin"
        REQUIRED
    )
    find_file(MPV_RUNTIME_DLL
        NAMES libmpv-2.dll mpv-2.dll
        HINTS
            "${MPV_INCLUDE_DIR}/../bin"
            "${YANAMI_QT_PREFIX}/bin"
        REQUIRED
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
    add_library(MPV::MPV ALIAS PkgConfig::MPV)
endif()
find_program(CARGO_EXECUTABLE cargo
    HINTS "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin"
    REQUIRED
)

get_filename_component(YANAMI_WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(YANAMI_RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/rust-target")
if(WIN32)
    set(YANAMI_RUST_BRIDGE_NAME "yanami_desktop_bridge.dll")
elseif(APPLE)
    set(YANAMI_RUST_BRIDGE_NAME "libyanami_desktop_bridge.dylib")
else()
    set(YANAMI_RUST_BRIDGE_NAME "libyanami_desktop_bridge.so")
endif()
set(YANAMI_RUST_BRIDGE "${YANAMI_RUST_TARGET_DIR}/release/${YANAMI_RUST_BRIDGE_NAME}")
file(GLOB_RECURSE YANAMI_RUST_SOURCES CONFIGURE_DEPENDS
    "${YANAMI_WORKSPACE}/crates/*/src/*.rs"
    "${YANAMI_WORKSPACE}/crates/*/Cargo.toml"
)

set(YANAMI_CARGO_ENV "CARGO_TARGET_DIR=${YANAMI_RUST_TARGET_DIR}")
if(WIN32)
    # Match the Rust bridge ABI/toolchain with the MSYS2 UCRT environment used
    # by Qt and the native desktop executable. This keeps source builds from
    # requiring a separate Visual Studio/MSVC installation.
    list(APPEND YANAMI_CARGO_ENV
        "RUSTUP_TOOLCHAIN=stable-x86_64-pc-windows-gnu"
        "CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=${YANAMI_QT_PREFIX}/bin/gcc.exe"
        "CC_x86_64_pc_windows_gnu=${YANAMI_QT_PREFIX}/bin/gcc.exe"
    )
endif()

add_custom_command(
    OUTPUT "${YANAMI_RUST_BRIDGE}"
    COMMAND "${CMAKE_COMMAND}" -E env ${YANAMI_CARGO_ENV}
            "${CARGO_EXECUTABLE}" build --locked --release --package yanami-desktop-bridge
            --manifest-path "${YANAMI_WORKSPACE}/Cargo.toml"
    WORKING_DIRECTORY "${YANAMI_WORKSPACE}"
    DEPENDS
        "${YANAMI_WORKSPACE}/Cargo.lock"
        "${YANAMI_WORKSPACE}/Cargo.toml"
        ${YANAMI_RUST_SOURCES}
    COMMENT "Building the Rust application backend"
    VERBATIM
)
add_custom_target(yanami-rust-bridge ALL DEPENDS "${YANAMI_RUST_BRIDGE}")

qt_standard_project_setup(
    REQUIRES 6.8
    I18N_SOURCE_LANGUAGE en
    I18N_TRANSLATED_LANGUAGES zh_CN
)

qt_add_executable(yanami-desktop
    native/main.cpp
    native/BackendController.cpp
    native/BackendController.hpp
    native/LocaleController.cpp
    native/LocaleController.hpp
    native/MpvVideoItem.cpp
    native/MpvVideoItem.hpp
    native/WindowController.cpp
    native/WindowController.hpp
)
if(WIN32)
    set_target_properties(yanami-desktop PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
add_dependencies(yanami-desktop yanami-rust-bridge)

set_source_files_properties(
    qml/LocaleText.qml
    qml/Theme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(yanami-desktop
    URI Yanami
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/LocaleText.qml
        qml/Theme.qml
        qml/components/AppButton.qml
        qml/components/AppErrorDialog.qml
        qml/components/AppIcon.qml
        qml/components/AppScrollBar.qml
        qml/components/AppSlider.qml
        qml/components/AppTextField.qml
        qml/components/BrandMark.qml
        qml/components/DetailHero.qml
        qml/components/EpisodeCard.qml
        qml/components/GlassPanel.qml
        qml/components/LibraryCard.qml
        qml/components/LoadingIndicator.qml
        qml/components/LoadingOverlay.qml
        qml/components/MediaPlayButton.qml
        qml/components/NavButton.qml
        qml/components/PosterCard.qml
        qml/components/RecentEpisodeCard.qml
        qml/components/RoundedImage.qml
        qml/components/SmoothFlickable.qml
        qml/components/SmoothHorizontalList.qml
        qml/components/TrackMenu.qml
        qml/components/VolumeControl.qml
        qml/components/WindowResizeHandles.qml
        qml/components/WindowTitleBar.qml
        qml/pages/HomePage.qml
        qml/pages/PlayerPage.qml
        qml/pages/SearchPage.qml
        qml/pages/SettingsPage.qml
)

qt_add_translations(
    TARGETS yanami-desktop
    SOURCE_TARGETS yanami-desktop
    TS_FILES "${CMAKE_CURRENT_SOURCE_DIR}/i18n/yanami_zh_CN.ts"
    RESOURCE_PREFIX "/i18n"
    LUPDATE_TARGET yanami-update-translations
    LRELEASE_TARGET yanami-release-translations
)

target_link_libraries(yanami-desktop PRIVATE
    Qt6::Concurrent
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::OpenGL
    MPV::MPV
)

if(WIN32)
    target_link_libraries(yanami-desktop PRIVATE dwmapi dxgi)
endif()

add_custom_target(yanami-deploy-rust-bridge ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${YANAMI_RUST_BRIDGE}" "$<TARGET_FILE_DIR:yanami-desktop>/${YANAMI_RUST_BRIDGE_NAME}"
)
add_dependencies(yanami-deploy-rust-bridge yanami-rust-bridge)
add_dependencies(yanami-desktop yanami-deploy-rust-bridge)

if(WIN32)
    add_custom_target(yanami-deploy-windows-runtime ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DYANAMI_EXECUTABLE=$<TARGET_FILE:yanami-desktop>"
            "-DYANAMI_QML_DIR=${CMAKE_CURRENT_SOURCE_DIR}/qml"
            "-DYANAMI_QT_PREFIX=${YANAMI_QT_PREFIX}"
            "-DYANAMI_WINDEPLOYQT=${WINDEPLOYQT_EXECUTABLE}"
            "-DYANAMI_MPV_RUNTIME=${MPV_RUNTIME_DLL}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployWindowsRuntime.cmake"
        DEPENDS yanami-desktop yanami-deploy-rust-bridge
        COMMENT "Deploying the Windows desktop runtime"
        VERBATIM
    )
endif()

install(TARGETS yanami-desktop
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
install(FILES "${YANAMI_RUST_BRIDGE}"
    DESTINATION bin
)
