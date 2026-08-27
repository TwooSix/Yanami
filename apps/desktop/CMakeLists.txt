cmake_minimum_required(VERSION 3.24)

get_filename_component(YANAMI_WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(STRINGS "${YANAMI_WORKSPACE}/VERSION" YANAMI_SOURCE_VERSION LIMIT_COUNT 1)
string(STRIP "${YANAMI_SOURCE_VERSION}" YANAMI_SOURCE_VERSION)
if(NOT YANAMI_SOURCE_VERSION MATCHES
        "^([0-9]+\\.[0-9]+\\.[0-9]+)(-[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?(\\+[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?$")
    message(FATAL_ERROR
        "VERSION must contain one SemVer value, got: ${YANAMI_SOURCE_VERSION}")
endif()
set(YANAMI_NUMERIC_VERSION "${CMAKE_MATCH_1}")

project(YanamiDesktop VERSION "${YANAMI_NUMERIC_VERSION}" LANGUAGES CXX)

set(YANAMI_BUILD_VERSION "${YANAMI_SOURCE_VERSION}" CACHE STRING
    "Version shown by the application and embedded in package names")
set(YANAMI_BUILD_COMMIT "unknown" CACHE STRING
    "Full Git commit used for this build")
set(YANAMI_BUILD_RUN_ID "local" CACHE STRING
    "CI workflow run ID used for this build")
set(YANAMI_RUNTIME_LICENSE_DIR "" CACHE PATH
    "Optional generated platform runtime license inventory")
set(YANAMI_RUST_LICENSE_INVENTORY
    "${CMAKE_CURRENT_BINARY_DIR}/generated-licenses/THIRD_PARTY_LICENSES.html"
    CACHE FILEPATH
    "Generated Rust third-party license inventory installed in packages")
if(NOT YANAMI_BUILD_VERSION MATCHES
        "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?(\\+[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?$")
    message(FATAL_ERROR
        "YANAMI_BUILD_VERSION must be valid SemVer: ${YANAMI_BUILD_VERSION}")
endif()

file(STRINGS "${YANAMI_WORKSPACE}/Cargo.toml" YANAMI_CARGO_VERSION_LINE
    REGEX "^version = \\\"[^\\\"]+\\\"$" LIMIT_COUNT 1)
if(NOT YANAMI_CARGO_VERSION_LINE STREQUAL
        "version = \"${YANAMI_SOURCE_VERSION}\"")
    message(FATAL_ERROR
        "VERSION and the Cargo workspace package version must match")
endif()

option(
    YANAMI_ENABLE_DEV_HOOKS
    "Enable environment-driven desktop automation hooks for local testing"
    OFF
)
if(YANAMI_ENABLE_DEV_HOOKS)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" YANAMI_BUILD_TYPE_UPPER)
    if(CMAKE_CONFIGURATION_TYPES OR YANAMI_BUILD_TYPE_UPPER STREQUAL "RELEASE")
        message(FATAL_ERROR
            "YANAMI_ENABLE_DEV_HOOKS is forbidden in Release or multi-config builds. "
            "Use a single-config Debug or RelWithDebInfo build."
        )
    endif()
endif()

if(WIN32)
    enable_language(RC)
elseif(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

set(YANAMI_BOOTSTRAP_SOURCES
    bootstrap/BootstrapProtocol.cpp
    bootstrap/BootstrapProtocol.hpp
)
if(WIN32)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    configure_file(
        resources/windows/yanami-bootstrap.rc
        "${CMAKE_CURRENT_BINARY_DIR}/generated/yanami-bootstrap.rc"
        @ONLY
    )
    add_executable(yanami-bootstrap WIN32
        ${YANAMI_BOOTSTRAP_SOURCES}
        bootstrap/BootstrapLocale.cpp
        bootstrap/BootstrapLocale.hpp
        bootstrap/BootstrapWindows.cpp
        bootstrap/BootstrapWindowsAnimation.hpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/yanami-bootstrap.rc"
        resources/windows/yanami.ico
        resources/windows/yanami-splash.ico
    )
    set_target_properties(yanami-bootstrap PROPERTIES OUTPUT_NAME "Yanami")
    target_link_libraries(yanami-bootstrap PRIVATE
        advapi32 gdi32 shell32 user32)
    if(MINGW)
        target_link_options(yanami-bootstrap PRIVATE
            -municode -static -static-libgcc -static-libstdc++)
    elseif(MSVC)
        set_property(TARGET yanami-bootstrap PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
elseif(APPLE)
    add_executable(yanami-bootstrap
        ${YANAMI_BOOTSTRAP_SOURCES}
        bootstrap/BootstrapMac.mm
    )
    set_target_properties(yanami-bootstrap PROPERTIES
        OUTPUT_NAME "yanami-bootstrap"
        OBJCXX_STANDARD 20
        OBJCXX_STANDARD_REQUIRED ON
    )
    target_compile_options(yanami-bootstrap PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(yanami-bootstrap PRIVATE
        "-framework AppKit" "-framework QuartzCore")
else()
    add_executable(yanami-bootstrap
        ${YANAMI_BOOTSTRAP_SOURCES}
        bootstrap/BootstrapLinux.cpp
    )
    set_target_properties(yanami-bootstrap PROPERTIES OUTPUT_NAME "yanami")
    find_package(X11 QUIET)
    if(X11_FOUND)
        target_compile_definitions(yanami-bootstrap PRIVATE
            YANAMI_BOOTSTRAP_HAS_X11=1)
        target_include_directories(yanami-bootstrap PRIVATE ${X11_INCLUDE_DIR})
        target_link_libraries(yanami-bootstrap PRIVATE ${X11_LIBRARIES})
    endif()
endif()
target_include_directories(yanami-bootstrap PRIVATE bootstrap)

find_package(Qt6 6.8 REQUIRED COMPONENTS Concurrent Gui Network Quick QuickControls2 OpenGL LinguistTools)
get_filename_component(YANAMI_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)
if(WIN32)
    # SDL is the primary gamepad backend for the Windows release. XInput stays
    # linked below as an exclusive runtime fallback when SDL initialization
    # fails or is explicitly selected for diagnostics.
    find_package(SDL3 3.2 CONFIG REQUIRED)
else()
    # Controller support outside the primary Windows target is experimental;
    # keep those builders working when SDL3 is not installed.
    find_package(SDL3 3.2 CONFIG QUIET)
endif()

# The MSYS2 build of pkg-config may expose POSIX paths such as
# /ucrt64/include to native Windows CMake. Locate the import library directly
# on Windows so the same project can be configured from PowerShell. Other
# platforms continue to use the canonical mpv.pc metadata.
if(WIN32)
    find_path(MPV_INCLUDE_DIR
        NAMES mpv/client.h
        REQUIRED
    )

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
    # libmpv links to the Vulkan loader through libplacebo. Consumer Windows
    # installations are not guaranteed to provide vulkan-1.dll (and hosted
    # Windows Server runners do not), so always select the MSYS2 copy instead
    # of allowing find_file() to fall back to System32.
    find_file(VULKAN_RUNTIME_DLL
        NAMES vulkan-1.dll
        HINTS "${YANAMI_QT_PREFIX}/bin"
        NO_DEFAULT_PATH
        REQUIRED
    )
    find_file(SDL3_RUNTIME_DLL
        NAMES SDL3.dll
        HINTS "${YANAMI_QT_PREFIX}/bin"
        NO_DEFAULT_PATH
        REQUIRED
    )
    find_program(PACMAN_EXECUTABLE
        NAMES pacman
        HINTS "${YANAMI_QT_PREFIX}/../usr/bin"
        REQUIRED
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MPV REQUIRED mpv)
    find_library(MPV_RUNTIME_LIBRARY
        NAMES mpv libmpv
        HINTS ${MPV_LIBRARY_DIRS}
        REQUIRED
    )
endif()

# MpvVideoItem uses libmpv's public C types but resolves every function at
# runtime. This header-only target intentionally carries no link library, so a
# future target cannot accidentally restore libmpv to the desktop PE import
# table by inheriting MPV::MPV.
add_library(yanami-mpv-headers INTERFACE)
if(WIN32)
    get_filename_component(
        YANAMI_MPV_RUNTIME_FILENAME "${MPV_RUNTIME_DLL}" NAME)
    target_include_directories(
        yanami-mpv-headers INTERFACE "${MPV_INCLUDE_DIR}")
    target_compile_definitions(yanami-mpv-headers INTERFACE
        YANAMI_MPV_RUNTIME_FILENAME="${YANAMI_MPV_RUNTIME_FILENAME}"
    )
else()
    target_include_directories(
        yanami-mpv-headers INTERFACE ${MPV_INCLUDE_DIRS})
    target_compile_options(
        yanami-mpv-headers INTERFACE ${MPV_CFLAGS_OTHER})
    if(APPLE)
        get_filename_component(
            YANAMI_MPV_RUNTIME_FILENAME "${MPV_RUNTIME_LIBRARY}" NAME)
        target_compile_definitions(yanami-mpv-headers INTERFACE
            YANAMI_MPV_RUNTIME_FILENAME="${YANAMI_MPV_RUNTIME_FILENAME}"
            YANAMI_MPV_DEVELOPMENT_RUNTIME_PATH="${MPV_RUNTIME_LIBRARY}"
        )
    endif()
endif()
find_program(CARGO_EXECUTABLE cargo
    HINTS "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin"
    REQUIRED
)

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
    "${YANAMI_WORKSPACE}/crates/*/build.rs"
    "${YANAMI_WORKSPACE}/crates/*/Cargo.toml"
)

set(YANAMI_CARGO_ENV
    "CARGO_TARGET_DIR=${YANAMI_RUST_TARGET_DIR}"
    "YANAMI_BUILD_VERSION=${YANAMI_BUILD_VERSION}"
)
set(YANAMI_RUST_BUILD_VERSION_STAMP
    "${CMAKE_CURRENT_BINARY_DIR}/yanami-rust-build-version.txt")
file(GENERATE OUTPUT "${YANAMI_RUST_BUILD_VERSION_STAMP}"
    CONTENT "${YANAMI_BUILD_VERSION}\n")
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
        "${YANAMI_RUST_BUILD_VERSION_STAMP}"
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
    native/ApplicationPaths.cpp
    native/ApplicationPaths.hpp
    native/ApplicationViewModel.cpp
    native/ApplicationViewModel.hpp
    native/BackendPorts.hpp
    native/AsyncImageKey.hpp
    native/AsyncImageProvider.cpp
    native/AsyncImageProvider.hpp
    native/AsyncResourceState.cpp
    native/AsyncResourceState.hpp
    native/BootstrapReadySignal.cpp
    native/BootstrapReadySignal.hpp
    native/AsyncOperationState.cpp
    native/AsyncOperationState.hpp
    native/BackendInfrastructure.cpp
    native/BackendInfrastructure.hpp
    native/DesktopBackendServices.cpp
    native/DesktopBackendServices.hpp
    native/DiagnosticsViewModel.cpp
    native/DiagnosticsViewModel.hpp
    native/SessionCoordinator.cpp
    native/SessionCoordinator.hpp
    native/CatalogCoordinator.cpp
    native/CatalogCoordinator.hpp
    native/CatalogCoordinatorNavigation.cpp
    native/CatalogCoordinatorStore.cpp
    native/CatalogCoordinatorCache.cpp
    native/SearchCoordinator.cpp
    native/SearchCoordinator.hpp
    native/SearchResultRowsModel.cpp
    native/SearchResultRowsModel.hpp
    native/MediaCoordinator.cpp
    native/MediaCoordinator.hpp
    native/MediaCoordinatorInteractive.cpp
    native/MediaCoordinatorMutations.cpp
    native/MediaCoordinatorRefresh.cpp
    native/PlaybackCoordinator.cpp
    native/PlaybackCoordinator.hpp
    native/DanmakuCoordinator.cpp
    native/DanmakuCoordinator.hpp
    native/DevelopmentHooks.cpp
    native/DevelopmentHooks.hpp
    native/KeyedMutationScheduler.hpp
    native/LibraryCacheContract.hpp
    native/ImageEditorViewModel.cpp
    native/ImageEditorViewModel.hpp
    native/MetadataEditorViewModel.cpp
    native/MetadataEditorViewModel.hpp
    native/MediaTargetFlowViewModel.cpp
    native/MediaTargetFlowViewModel.hpp
    native/MediaStore.cpp
    native/MediaStore.hpp
    native/RequestCoordinator.hpp
    native/LocaleController.cpp
    native/LocaleController.hpp
    native/MpvApi.cpp
    native/MpvApi.hpp
    native/MpvVideoItem.cpp
    native/MpvVideoItem.hpp
    native/PlaybackCompletionGate.hpp
    native/PlaybackStallWatchdog.hpp
    native/PlaybackTrackMapper.cpp
    native/PlaybackTrackMapper.hpp
    native/PlaybackDescriptor.cpp
    native/PlaybackDescriptor.hpp
    native/PlaybackReporter.cpp
    native/PlaybackReporter.hpp
    native/PlaybackReportQueue.cpp
    native/PlaybackReportQueue.hpp
    native/PlaybackTelemetry.cpp
    native/PlaybackTelemetry.hpp
    native/PerformanceTrace.cpp
    native/PerformanceTrace.hpp
    native/RustBridgeRuntime.cpp
    native/RustBridgeRuntime.hpp
    native/RustBridgeError.cpp
    native/RustBridgeError.hpp
    native/RuntimeLogger.cpp
    native/RuntimeLogger.hpp
    native/UpdateChecker.cpp
    native/UpdateChecker.hpp
    native/UpscalingAssetManager.cpp
    native/UpscalingAssetManager.hpp
    native/UpscalingCapabilityProbe.cpp
    native/UpscalingCapabilityProbe.hpp
    native/UpscalingCatalog.cpp
    native/UpscalingCatalog.hpp
    native/UpscalingPerformancePolicy.cpp
    native/UpscalingPerformancePolicy.hpp
    native/UpscalingRuntimeConfig.cpp
    native/UpscalingRuntimeConfig.hpp
    native/UpscalingViewModel.cpp
    native/UpscalingViewModel.hpp
    native/WindowController.cpp
    native/WindowController.hpp
)
if(WIN32)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    configure_file(
        resources/windows/yanami.rc
        "${CMAKE_CURRENT_BINARY_DIR}/generated/yanami.rc"
        @ONLY
    )
    target_sources(yanami-desktop PRIVATE
        native/DesktopEntryGuard.cpp
        native/DesktopEntryGuard.hpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/yanami.rc"
        resources/windows/yanami.ico
    )
    set_target_properties(yanami-desktop PROPERTIES WIN32_EXECUTABLE TRUE)
elseif(APPLE)
    set_target_properties(yanami-desktop PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Yanami"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.TwooSix.Yanami"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/Yanami.Info.plist.in"
        OUTPUT_NAME "Yanami"
    )
endif()
add_dependencies(yanami-desktop yanami-rust-bridge)
add_dependencies(yanami-desktop yanami-bootstrap)
if(APPLE)
    add_custom_command(TARGET yanami-desktop POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:yanami-bootstrap>"
            "$<TARGET_BUNDLE_DIR:yanami-desktop>/Contents/MacOS/yanami-bootstrap"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_DIR:yanami-desktop>/Contents/Resources"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/linux/io.github.TwooSix.Yanami.png"
            "$<TARGET_BUNDLE_DIR:yanami-desktop>/Contents/Resources/yanami-logo.png"
        VERBATIM
    )
endif()
target_compile_definitions(yanami-desktop PRIVATE
    QT_MESSAGELOGCONTEXT
    YANAMI_VERSION="${YANAMI_BUILD_VERSION}"
)
if(YANAMI_ENABLE_DEV_HOOKS)
    target_compile_definitions(yanami-desktop PRIVATE YANAMI_ENABLE_DEV_HOOKS)
endif()

set_source_files_properties(
    qml/LocaleText.qml
    qml/components/PopupCoordinator.qml
    qml/Theme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(yanami-ui
    STATIC
    URI Yanami.Ui
    VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Yanami/Ui"
    SOURCES
        native/ControllerNavigation.cpp
        native/ControllerNavigation.hpp
        native/DanmakuRenderItem.cpp
        native/DanmakuRenderItem.hpp
        native/DanmakuTimelineModel.cpp
        native/DanmakuTimelineModel.hpp
        native/InputModalityController.cpp
        native/InputModalityController.hpp
    QML_FILES
        qml/LocaleText.qml
        qml/Theme.qml
        qml/components/AppButton.qml
        qml/components/AppMenu.qml
        qml/components/AppPopupButton.qml
        qml/components/AppPopup.qml
        qml/components/AppTransientPopup.qml
        qml/components/AppModalPopup.qml
        qml/components/PopupCoordinator.qml
        qml/components/AppConfirmDialog.qml
        qml/components/AppErrorDialog.qml
        qml/components/AppIcon.qml
        qml/components/ApplicationLifecycleHost.qml
        qml/components/AppScrollBar.qml
        qml/components/AppSlider.qml
        qml/components/AppTextField.qml
        qml/components/AppToolTip.qml
        qml/components/BrandMark.qml
        qml/components/CardProgressStrip.qml
        qml/components/ControllerInputTestScope.qml
        qml/components/DanmakuMenu.qml
        qml/components/DanmakuOverlay.qml
        qml/components/DetailHero.qml
        qml/components/EpisodeCard.qml
        qml/components/EpisodeScrollPolicy.qml
        qml/components/GlassPanel.qml
        qml/components/LibraryCard.qml
        qml/components/LoadingIndicator.qml
        qml/components/LoadingOverlay.qml
        qml/components/LoadingPlaceholder.qml
        qml/components/MediaContextMenu.qml
        qml/components/MediaActionHost.qml
        qml/components/MediaTargetDialog.qml
        qml/components/MediaPlayButton.qml
        qml/components/MetadataEditorDialog.qml
        qml/components/ImageEditorDialog.qml
        qml/components/NavButton.qml
        qml/components/PosterCard.qml
        qml/components/PlaybackAdvancePolicy.qml
        qml/components/PlaybackControllerPolicy.qml
        qml/components/PlaybackEndOverlay.qml
        qml/components/PlaybackSettingsMenu.qml
        qml/components/StatusToast.qml
        qml/components/PlaybackQueueMenu.qml
        qml/components/PlaybackHost.qml
        qml/components/PopupControllerNavigator.qml
        qml/components/RecentEpisodeCard.qml
        qml/components/RefreshMetadataDialog.qml
        qml/components/RoundedImage.qml
        qml/components/SearchSubmissionPolicy.qml
        qml/components/SmoothFlickable.qml
        qml/components/SmoothGridView.qml
        qml/components/SmoothHorizontalList.qml
        qml/components/SmoothListView.qml
        qml/components/SpatialFocusNavigator.qml
        qml/components/TrackMenu.qml
        qml/components/VolumeControl.qml
        qml/components/WindowResizeHandles.qml
        qml/components/WindowTitleBar.qml
    RESOURCES
        qml/assets/yanami-logo.png
)
target_include_directories(yanami-ui PRIVATE native)
target_link_libraries(yanami-ui PRIVATE
    Qt6::Gui
    Qt6::Quick
)
if(SDL3_FOUND)
    target_compile_definitions(yanami-ui PRIVATE YANAMI_HAS_SDL3=1)
    target_link_libraries(yanami-ui PRIVATE SDL3::SDL3)
endif()
if(WIN32)
    # Yanami supports Windows 10/11, where XInput 1.4 is a system component.
    # The generic MinGW import library targets the legacy XINPUT1_3.dll,
    # which is absent on clean Windows installations unless the old DirectX
    # runtime was installed separately.
    target_link_libraries(yanami-ui PRIVATE xinput1_4)
endif()

qt_add_qml_module(yanami-desktop
    URI Yanami
    VERSION 1.0
    IMPORTS
        TARGET yanami-ui
    QML_FILES
        qml/Main.qml
        qml/pages/HomePage.qml
        qml/pages/AboutPage.qml
        qml/pages/FavoritesPage.qml
        qml/pages/PlayerPage.qml
        qml/pages/SearchPage.qml
        qml/pages/SettingsPage.qml
)

get_target_property(YANAMI_LRELEASE_EXECUTABLE Qt6::lrelease IMPORTED_LOCATION)
execute_process(
    COMMAND "${YANAMI_LRELEASE_EXECUTABLE}" -help
    RESULT_VARIABLE YANAMI_LRELEASE_HELP_RESULT
    OUTPUT_VARIABLE YANAMI_LRELEASE_HELP_STDOUT
    ERROR_VARIABLE YANAMI_LRELEASE_HELP_STDERR
)
set(YANAMI_LRELEASE_OPTIONS -nounfinished)
if(YANAMI_LRELEASE_HELP_RESULT EQUAL 0)
    string(CONCAT YANAMI_LRELEASE_HELP
        "${YANAMI_LRELEASE_HELP_STDOUT}"
        "${YANAMI_LRELEASE_HELP_STDERR}"
    )
    if(YANAMI_LRELEASE_HELP MATCHES "-fail-on-unfinished")
        list(APPEND YANAMI_LRELEASE_OPTIONS -fail-on-unfinished)
    endif()
    if(YANAMI_LRELEASE_HELP MATCHES "-fail-on-invalid")
        list(APPEND YANAMI_LRELEASE_OPTIONS -fail-on-invalid)
    endif()
endif()
string(JOIN "|" YANAMI_LRELEASE_OPTIONS_SERIALIZED
    ${YANAMI_LRELEASE_OPTIONS})

qt_add_translations(
    TARGETS yanami-desktop
    SOURCE_TARGETS yanami-desktop yanami-ui
    TS_FILES "${CMAKE_CURRENT_SOURCE_DIR}/i18n/yanami_zh_CN.ts"
    RESOURCE_PREFIX "/i18n"
    LUPDATE_OPTIONS -no-obsolete
    LUPDATE_TARGET yanami-update-translations
    LRELEASE_TARGET yanami-release-translations
    LRELEASE_OPTIONS ${YANAMI_LRELEASE_OPTIONS}
)

target_link_libraries(yanami-desktop PRIVATE
    yanami-uiplugin
    Qt6::Concurrent
    Qt6::Gui
    Qt6::Network
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::OpenGL
    yanami-mpv-headers
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
            "-DYANAMI_VULKAN_RUNTIME=${VULKAN_RUNTIME_DLL}"
            "-DYANAMI_SDL3_RUNTIME=${SDL3_RUNTIME_DLL}"
            "-DYANAMI_PACMAN=${PACMAN_EXECUTABLE}"
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
if(APPLE)
    install(TARGETS yanami-bootstrap
        RUNTIME DESTINATION "Yanami.app/Contents/MacOS"
    )
    install(FILES resources/linux/io.github.TwooSix.Yanami.png
        DESTINATION "Yanami.app/Contents/Resources"
        RENAME yanami-logo.png
    )
    install(FILES "${YANAMI_RUST_BRIDGE}"
        DESTINATION "Yanami.app/Contents/MacOS"
    )
else()
    install(TARGETS yanami-bootstrap RUNTIME DESTINATION bin)
    install(FILES "${YANAMI_RUST_BRIDGE}"
        DESTINATION bin
    )
endif()

set(YANAMI_PACKAGE_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}" CACHE STRING
    "Architecture label used in package file names")
string(TOLOWER "${YANAMI_PACKAGE_ARCHITECTURE}" YANAMI_PACKAGE_ARCHITECTURE)
if(YANAMI_PACKAGE_ARCHITECTURE MATCHES "^(amd64|x86_64)$")
    set(YANAMI_PACKAGE_ARCHITECTURE "x86_64")
elseif(YANAMI_PACKAGE_ARCHITECTURE MATCHES "^(arm64|aarch64)$")
    set(YANAMI_PACKAGE_ARCHITECTURE "arm64")
endif()

if(WIN32)
    set(YANAMI_PACKAGE_PLATFORM "Windows")
elseif(APPLE)
    set(YANAMI_PACKAGE_PLATFORM "macOS")
else()
    set(YANAMI_PACKAGE_PLATFORM "Linux")
endif()

configure_file(
    resources/BUILD_INFO.json.in
    "${CMAKE_CURRENT_BINARY_DIR}/BUILD_INFO.json"
    @ONLY
)
install(FILES
    "${YANAMI_WORKSPACE}/LICENSE"
    "${YANAMI_WORKSPACE}/README.md"
    "${YANAMI_WORKSPACE}/README.zh-CN.md"
    "${YANAMI_WORKSPACE}/THIRD_PARTY_NOTICES.md"
    "${CMAKE_CURRENT_BINARY_DIR}/BUILD_INFO.json"
    DESTINATION .
)
configure_file(
    cmake/VerifyRustLicenseInventory.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/VerifyRustLicenseInventory.cmake"
    @ONLY
)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/VerifyRustLicenseInventory.cmake")
install(FILES "${YANAMI_RUST_LICENSE_INVENTORY}"
    DESTINATION licenses/rust
    RENAME THIRD_PARTY_LICENSES.html
)
if(YANAMI_RUNTIME_LICENSE_DIR)
    if(NOT IS_DIRECTORY "${YANAMI_RUNTIME_LICENSE_DIR}")
        message(FATAL_ERROR
            "YANAMI_RUNTIME_LICENSE_DIR does not exist: ${YANAMI_RUNTIME_LICENSE_DIR}")
    endif()
    install(DIRECTORY "${YANAMI_RUNTIME_LICENSE_DIR}/"
        DESTINATION licenses/runtime
    )
endif()

if(UNIX AND NOT APPLE)
    install(FILES resources/linux/io.github.TwooSix.Yanami.desktop
        DESTINATION share/applications
    )
    install(FILES resources/linux/io.github.TwooSix.Yanami.png
        DESTINATION share/icons/hicolor/512x512/apps
    )
endif()

if(WIN32)
    install(DIRECTORY
        "${YANAMI_QT_PREFIX}/share/licenses/qt6-base"
        "${YANAMI_QT_PREFIX}/share/licenses/qt6-declarative"
        DESTINATION licenses/qt
    )
    install(FILES
        "${YANAMI_QT_PREFIX}/share/licenses/qt6-base/GPL-2.0-or-later.txt"
        DESTINATION licenses/mpv
    )
    install(FILES "${YANAMI_WORKSPACE}/LICENSE"
        DESTINATION licenses/ffmpeg
        RENAME GPL-3.0-or-later.txt
    )
    install(FILES "${YANAMI_WORKSPACE}/licenses/libass-ISC.txt"
        DESTINATION licenses/libass
        RENAME ISC.txt
    )
    configure_file(
        cmake/InstallWindowsRuntime.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/InstallWindowsRuntime.cmake"
        @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallWindowsRuntime.cmake")
endif()

if(APPLE)
    qt_generate_deploy_qml_app_script(
        TARGET yanami-desktop
        OUTPUT_SCRIPT YANAMI_QT_DEPLOY_SCRIPT
        NO_TRANSLATIONS
        POST_INCLUDE_FILES "${MPV_RUNTIME_LIBRARY}"
    )
    install(SCRIPT "${YANAMI_QT_DEPLOY_SCRIPT}")

    configure_file(
        cmake/InstallMacRuntime.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/InstallMacRuntime.cmake"
        @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallMacRuntime.cmake")
endif()

if(UNIX AND NOT APPLE)
    configure_file(
        cmake/InstallLinuxRuntime.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/InstallLinuxRuntime.cmake"
        @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallLinuxRuntime.cmake")
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS QuickTest Test)

    add_executable(yanami-bootstrap-protocol-tests
        tests/BootstrapProtocolTests.cpp
        bootstrap/BootstrapProtocol.cpp
        bootstrap/BootstrapProtocol.hpp
    )
    target_include_directories(
        yanami-bootstrap-protocol-tests PRIVATE bootstrap)
    add_test(
        NAME yanami-bootstrap-protocol-tests
        COMMAND $<TARGET_FILE:yanami-bootstrap-protocol-tests>
    )

    add_test(
        NAME yanami-translation-catalog-freshness
        COMMAND "${CMAKE_COMMAND}"
            "-DYANAMI_LUPDATE=$<TARGET_FILE:Qt6::lupdate>"
            "-DYANAMI_LRELEASE=$<TARGET_FILE:Qt6::lrelease>"
            "-DYANAMI_LRELEASE_OPTIONS=${YANAMI_LRELEASE_OPTIONS_SERIALIZED}"
            "-DYANAMI_TRANSLATION=${CMAKE_CURRENT_SOURCE_DIR}/i18n/yanami_zh_CN.ts"
            "-DYANAMI_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DYANAMI_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/translation-catalog-freshness"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/TranslationCatalogFreshness.cmake"
    )
    set_tests_properties(
        yanami-translation-catalog-freshness PROPERTIES LABELS "i18n")

    add_executable(yanami-bootstrap-smoke-child
        tests/BootstrapSmokeChild.cpp
        bootstrap/BootstrapProtocol.hpp
    )
    target_include_directories(
        yanami-bootstrap-smoke-child PRIVATE bootstrap)
    if(MINGW)
        target_link_options(yanami-bootstrap-smoke-child PRIVATE
            -static -static-libgcc -static-libstdc++)
    elseif(MSVC)
        set_property(TARGET yanami-bootstrap-smoke-child PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    if(WIN32)
        add_executable(yanami-bootstrap-locale-tests
            tests/BootstrapLocaleTests.cpp
            bootstrap/BootstrapLocale.cpp
            bootstrap/BootstrapLocale.hpp
        )
        target_include_directories(
            yanami-bootstrap-locale-tests PRIVATE bootstrap)
        target_link_libraries(
            yanami-bootstrap-locale-tests PRIVATE advapi32)
        add_test(
            NAME yanami-bootstrap-locale-tests
            COMMAND $<TARGET_FILE:yanami-bootstrap-locale-tests>
        )

        add_executable(yanami-bootstrap-windows-animation-tests
            tests/BootstrapWindowsAnimationTests.cpp
            bootstrap/BootstrapWindowsAnimation.hpp
        )
        target_include_directories(
            yanami-bootstrap-windows-animation-tests PRIVATE bootstrap)
        target_compile_definitions(
            yanami-bootstrap-windows-animation-tests PRIVATE
            YANAMI_SPLASH_ICON_PATH="${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/yanami-splash.ico"
        )
        add_test(
            NAME yanami-bootstrap-windows-animation-tests
            COMMAND $<TARGET_FILE:yanami-bootstrap-windows-animation-tests>
        )

        add_executable(yanami-bootstrap-ready-signal-tests
            tests/BootstrapReadySignalTests.cpp
            native/BootstrapReadySignal.cpp
            native/BootstrapReadySignal.hpp
        )
        target_include_directories(
            yanami-bootstrap-ready-signal-tests PRIVATE native)
        target_link_libraries(
            yanami-bootstrap-ready-signal-tests PRIVATE Qt6::Core Qt6::Test)
        add_test(
            NAME yanami-bootstrap-ready-signal-tests
            COMMAND $<TARGET_FILE:yanami-bootstrap-ready-signal-tests>
        )

        add_executable(yanami-desktop-entry-guard-tests
            tests/DesktopEntryGuardTests.cpp
            native/DesktopEntryGuard.cpp
            native/DesktopEntryGuard.hpp
        )
        target_include_directories(
            yanami-desktop-entry-guard-tests PRIVATE native)
        target_compile_definitions(yanami-desktop-entry-guard-tests PRIVATE
            YANAMI_MAIN_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/native/main.cpp"
        )
        target_link_libraries(
            yanami-desktop-entry-guard-tests PRIVATE Qt6::Core Qt6::Test)
        add_test(
            NAME yanami-desktop-entry-guard-tests
            COMMAND $<TARGET_FILE:yanami-desktop-entry-guard-tests>
        )

        add_test(
            NAME yanami-bootstrap-static-imports
            COMMAND "${CMAKE_COMMAND}"
                "-DYANAMI_BOOTSTRAP=$<TARGET_FILE:yanami-bootstrap>"
                "-DYANAMI_OBJDUMP=${CMAKE_OBJDUMP}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BootstrapStaticImports.cmake"
        )
        add_test(
            NAME yanami-bootstrap-launch-smoke
            COMMAND "${CMAKE_COMMAND}"
                "-DYANAMI_BOOTSTRAP=$<TARGET_FILE:yanami-bootstrap>"
                "-DYANAMI_SMOKE_CHILD=$<TARGET_FILE:yanami-bootstrap-smoke-child>"
                "-DYANAMI_STAGE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/bootstrap-launch-smoke"
                "-DYANAMI_BUILD_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
                "-DYANAMI_BOOTSTRAP_FILE_NAME=Yanami.exe"
                "-DYANAMI_DESKTOP_FILE_NAME=yanami-desktop.exe"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BootstrapLaunchSmoke.cmake"
        )
        set_tests_properties(yanami-bootstrap-launch-smoke PROPERTIES TIMEOUT 30)
    endif()

    if(EXISTS "${YANAMI_RUST_LICENSE_INVENTORY}")
        add_test(
            NAME yanami-rust-license-inventory-policy-tests
            COMMAND "${CMAKE_COMMAND}"
                "-DYANAMI_VALID_INVENTORY=${YANAMI_RUST_LICENSE_INVENTORY}"
                "-DYANAMI_VERIFIER=${CMAKE_CURRENT_BINARY_DIR}/VerifyRustLicenseInventory.cmake"
                "-DYANAMI_WORKSPACE=${YANAMI_WORKSPACE}"
                "-DYANAMI_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/rust-license-inventory-policy-test"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RustLicenseInventoryPolicyTests.cmake"
        )
        set_tests_properties(
            yanami-rust-license-inventory-policy-tests
            PROPERTIES LABELS "package;policy" TIMEOUT 15)
    endif()

    add_test(
        NAME yanami-package-manifest-policy-tests
        COMMAND "${CMAKE_COMMAND}"
            "-DYANAMI_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/package-manifest-policy-test"
            "-DYANAMI_CMAKE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/cmake"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/PackageManifestPolicyTests.cmake"
    )
    set_tests_properties(yanami-package-manifest-policy-tests PROPERTIES
        LABELS "package;policy"
        TIMEOUT 15
    )

    if(NOT YANAMI_ENABLE_DEV_HOOKS)
        if(WIN32)
            set(YANAMI_INSTALL_SMOKE_EXECUTABLE "bin/Yanami.exe")
            set(YANAMI_INSTALL_SMOKE_DESKTOP "bin/yanami-desktop.exe")
            set(YANAMI_INSTALL_SMOKE_BRIDGE
                "bin/${YANAMI_RUST_BRIDGE_NAME}")
        elseif(APPLE)
            set(YANAMI_INSTALL_SMOKE_EXECUTABLE
                "Yanami.app/Contents/MacOS/yanami-bootstrap")
            set(YANAMI_INSTALL_SMOKE_DESKTOP
                "Yanami.app/Contents/MacOS/Yanami")
            set(YANAMI_INSTALL_SMOKE_BRIDGE
                "Yanami.app/Contents/MacOS/${YANAMI_RUST_BRIDGE_NAME}")
        else()
            set(YANAMI_INSTALL_SMOKE_EXECUTABLE "bin/yanami")
            set(YANAMI_INSTALL_SMOKE_DESKTOP "bin/yanami-desktop")
            set(YANAMI_INSTALL_SMOKE_BRIDGE
                "bin/${YANAMI_RUST_BRIDGE_NAME}")
        endif()
        add_custom_target(yanami-pr-install-smoke
            COMMAND "${CMAKE_COMMAND}"
                "-DYANAMI_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DYANAMI_STAGE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/pr-install-smoke/$<CONFIG>"
                "-DYANAMI_CONFIG=$<CONFIG>"
                "-DYANAMI_PLATFORM=${YANAMI_PACKAGE_PLATFORM}"
                "-DYANAMI_EXECUTABLE_RELATIVE=${YANAMI_INSTALL_SMOKE_EXECUTABLE}"
                "-DYANAMI_DESKTOP_RELATIVE=${YANAMI_INSTALL_SMOKE_DESKTOP}"
                "-DYANAMI_BRIDGE_RELATIVE=${YANAMI_INSTALL_SMOKE_BRIDGE}"
                "-DYANAMI_QT_PREFIX=${YANAMI_QT_PREFIX}"
                "-DYANAMI_EXPECTED_VERSION=${YANAMI_BUILD_VERSION}"
                "-DYANAMI_EXPECTED_SOURCE_VERSION=${YANAMI_SOURCE_VERSION}"
                "-DYANAMI_EXPECTED_COMMIT=${YANAMI_BUILD_COMMIT}"
                "-DYANAMI_EXPECTED_RUN_ID=${YANAMI_BUILD_RUN_ID}"
                "-DYANAMI_EXPECTED_ARCHITECTURE=${YANAMI_PACKAGE_ARCHITECTURE}"
                "-DYANAMI_CMAKE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/cmake"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/InstallTreeSmoke.cmake"
            DEPENDS yanami-bootstrap yanami-desktop yanami-deploy-rust-bridge
            COMMENT "Auditing and launching the isolated PR install tree"
            VERBATIM
        )
    endif()

    set_source_files_properties(
        tests/manual/StatusFeedbackShowcase.qml
        PROPERTIES QT_RESOURCE_ALIAS StatusFeedbackShowcase.qml
    )
    qt_add_executable(yanami-status-feedback-showcase EXCLUDE_FROM_ALL
        tests/manual/StatusFeedbackShowcaseMain.cpp
    )
    qt_add_qml_module(yanami-status-feedback-showcase
        URI Yanami.StatusFeedbackShowcase
        VERSION 1.0
        IMPORTS
            TARGET yanami-ui
        QML_FILES
            tests/manual/StatusFeedbackShowcase.qml
    )
    target_link_libraries(yanami-status-feedback-showcase PRIVATE
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        yanami-uiplugin
    )
    if(WIN32)
        set_target_properties(yanami-status-feedback-showcase PROPERTIES
            WIN32_EXECUTABLE TRUE
        )
    endif()

    add_executable(yanami-runtime-logger-tests
        tests/RuntimeLoggerTests.cpp
        native/ApplicationPaths.cpp
        native/ApplicationPaths.hpp
        native/DevelopmentHooks.cpp
        native/DevelopmentHooks.hpp
        native/RuntimeLogger.cpp
        native/RuntimeLogger.hpp
    )
    target_compile_definitions(yanami-runtime-logger-tests PRIVATE
        QT_MESSAGELOGCONTEXT
        YANAMI_ENABLE_DEV_HOOKS
    )
    target_include_directories(yanami-runtime-logger-tests PRIVATE native)
    target_link_libraries(yanami-runtime-logger-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )

    add_test(
        NAME yanami-runtime-logger-tests
        COMMAND $<TARGET_FILE:yanami-runtime-logger-tests>
    )

    add_executable(yanami-application-paths-tests
        tests/ApplicationPathsTests.cpp
        native/ApplicationPaths.cpp
        native/ApplicationPaths.hpp
    )
    target_include_directories(
        yanami-application-paths-tests PRIVATE native)
    target_link_libraries(yanami-application-paths-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-application-paths-tests
        COMMAND $<TARGET_FILE:yanami-application-paths-tests>
    )
    add_executable(yanami-mpv-api-tests
        tests/MpvApiTests.cpp
        native/MpvApi.cpp
        native/MpvApi.hpp
    )
    target_include_directories(yanami-mpv-api-tests PRIVATE native)
    target_link_libraries(yanami-mpv-api-tests PRIVATE
        Qt6::Core
        Qt6::Test
        yanami-mpv-headers
    )
    if(WIN32)
        add_dependencies(
            yanami-mpv-api-tests yanami-deploy-windows-runtime)
    endif()
    add_test(
        NAME yanami-mpv-api-tests
        COMMAND $<TARGET_FILE:yanami-mpv-api-tests>
    )

    add_executable(yanami-mpv-unavailable-tests
        tests/MpvUnavailableTests.cpp
        native/ApplicationPaths.cpp
        native/ApplicationPaths.hpp
        native/DevelopmentHooks.cpp
        native/DevelopmentHooks.hpp
        native/MpvApi.cpp
        native/MpvApi.hpp
        native/MpvVideoItem.cpp
        native/MpvVideoItem.hpp
        native/PerformanceTrace.cpp
        native/PerformanceTrace.hpp
        native/PlaybackCompletionGate.hpp
        native/PlaybackStallWatchdog.hpp
        native/UpscalingPerformancePolicy.cpp
        native/UpscalingPerformancePolicy.hpp
        native/UpscalingRuntimeConfig.cpp
        native/UpscalingRuntimeConfig.hpp
    )
    target_compile_definitions(yanami-mpv-unavailable-tests PRIVATE
        YANAMI_MPV_RUNTIME_TEST_MISSING=1
        YANAMI_PLAYER_PAGE_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/qml/pages/PlayerPage.qml"
    )
    target_include_directories(yanami-mpv-unavailable-tests PRIVATE native)
    target_link_libraries(yanami-mpv-unavailable-tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::OpenGL
        Qt6::Test
        yanami-mpv-headers
    )
    add_test(
        NAME yanami-mpv-unavailable-tests
        COMMAND $<TARGET_FILE:yanami-mpv-unavailable-tests>
    )
    set_tests_properties(yanami-mpv-unavailable-tests PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    )
    add_executable(yanami-development-hooks-disabled-tests
        tests/DevelopmentHooksTests.cpp
        native/DevelopmentHooks.cpp
        native/DevelopmentHooks.hpp
    )
    target_include_directories(yanami-development-hooks-disabled-tests PRIVATE native)
    target_link_libraries(yanami-development-hooks-disabled-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-development-hooks-disabled-tests
        COMMAND $<TARGET_FILE:yanami-development-hooks-disabled-tests>
    )
    add_executable(yanami-input-modality-tests
        tests/InputModalityControllerTests.cpp
        native/ControllerNavigation.cpp
        native/ControllerNavigation.hpp
        native/InputModalityController.cpp
        native/InputModalityController.hpp
    )
    target_include_directories(yanami-input-modality-tests PRIVATE native)
    target_link_libraries(yanami-input-modality-tests PRIVATE
        Qt6::Gui
        Qt6::Test
    )
    if(SDL3_FOUND)
        target_compile_definitions(
            yanami-input-modality-tests PRIVATE YANAMI_HAS_SDL3=1)
        target_link_libraries(
            yanami-input-modality-tests PRIVATE SDL3::SDL3)
    endif()
    if(WIN32)
        target_link_libraries(yanami-input-modality-tests PRIVATE xinput1_4)
    endif()
    add_test(
        NAME yanami-input-modality-tests
        COMMAND $<TARGET_FILE:yanami-input-modality-tests>
    )
    set_tests_properties(yanami-input-modality-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    if(WIN32 AND SDL3_FOUND)
        add_executable(yanami-sdl-virtual-controller-tests
            tests/SdlVirtualControllerIntegrationTests.cpp
            native/ControllerNavigation.cpp
            native/ControllerNavigation.hpp
            native/InputModalityController.cpp
            native/InputModalityController.hpp
        )
        target_compile_definitions(
            yanami-sdl-virtual-controller-tests PRIVATE YANAMI_HAS_SDL3=1)
        target_include_directories(
            yanami-sdl-virtual-controller-tests PRIVATE native)
        target_link_libraries(yanami-sdl-virtual-controller-tests PRIVATE
            Qt6::Gui
            Qt6::Test
            SDL3::SDL3
            xinput1_4
        )
        add_test(
            NAME yanami-sdl-virtual-controller-tests
            COMMAND $<TARGET_FILE:yanami-sdl-virtual-controller-tests>
        )
        set_tests_properties(yanami-sdl-virtual-controller-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${YANAMI_QT_PREFIX}/share/qt6/plugins"
            LABELS "controller;integration;sdl"
            TIMEOUT 15
        )
    endif()
    if(NOT YANAMI_ENABLE_DEV_HOOKS)
        add_test(
            NAME yanami-production-artifact-policy
            COMMAND "${CMAKE_COMMAND}"
                "-DYANAMI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/native"
                "-DYANAMI_EXECUTABLE=$<TARGET_FILE:yanami-desktop>"
                "-DYANAMI_BRIDGE=${YANAMI_RUST_BRIDGE}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ProductionArtifactPolicy.cmake"
        )
    endif()
    if(WIN32)
        add_test(
            NAME yanami-mpv-lazy-import-policy
            COMMAND "${CMAKE_COMMAND}"
                "-DYANAMI_EXECUTABLE=$<TARGET_FILE:yanami-desktop>"
                "-DYANAMI_OBJDUMP=${CMAKE_OBJDUMP}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/MpvLazyImportPolicy.cmake"
        )
    endif()
    add_executable(yanami-media-store-tests
        tests/MediaStoreTests.cpp
        native/MediaStore.cpp
        native/MediaStore.hpp
    )
    target_include_directories(yanami-media-store-tests PRIVATE native)
    target_link_libraries(yanami-media-store-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-media-store-tests
        COMMAND $<TARGET_FILE:yanami-media-store-tests>
    )
    add_executable(yanami-danmaku-coordinator-tests
        tests/DanmakuCoordinatorTests.cpp
        native/DanmakuCoordinator.cpp
        native/DanmakuCoordinator.hpp
        native/BackendPorts.hpp
        native/RustBridgeRuntime.cpp
        native/RustBridgeRuntime.hpp
        native/RustBridgeError.cpp
        native/RustBridgeError.hpp
        native/PlaybackTelemetry.cpp
        native/PlaybackTelemetry.hpp
    )
    target_include_directories(yanami-danmaku-coordinator-tests PRIVATE native)
    target_link_libraries(yanami-danmaku-coordinator-tests PRIVATE
        Qt6::Concurrent
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-danmaku-coordinator-tests
        COMMAND $<TARGET_FILE:yanami-danmaku-coordinator-tests>
    )
    add_executable(yanami-danmaku-timeline-tests
        tests/DanmakuTimelineModelTests.cpp
        native/DanmakuTimelineModel.cpp
        native/DanmakuTimelineModel.hpp
    )
    target_include_directories(yanami-danmaku-timeline-tests PRIVATE native)
    target_link_libraries(yanami-danmaku-timeline-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-danmaku-timeline-tests
        COMMAND $<TARGET_FILE:yanami-danmaku-timeline-tests>
    )
    add_executable(yanami-playback-track-mapper-tests
        tests/PlaybackTrackMapperTests.cpp
        native/PlaybackTrackMapper.cpp
        native/PlaybackTrackMapper.hpp
    )
    target_include_directories(yanami-playback-track-mapper-tests PRIVATE native)
    target_link_libraries(yanami-playback-track-mapper-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-playback-track-mapper-tests
        COMMAND $<TARGET_FILE:yanami-playback-track-mapper-tests>
    )
    add_executable(yanami-playback-stall-watchdog-tests
        tests/PlaybackStallWatchdogTests.cpp
        native/PlaybackStallWatchdog.hpp
    )
    target_include_directories(
        yanami-playback-stall-watchdog-tests PRIVATE native)
    target_link_libraries(yanami-playback-stall-watchdog-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-playback-stall-watchdog-tests
        COMMAND $<TARGET_FILE:yanami-playback-stall-watchdog-tests>
    )
    add_executable(yanami-playback-reporter-tests
        tests/PlaybackReporterTests.cpp
        native/ApplicationPaths.cpp
        native/ApplicationPaths.hpp
        native/BackendPorts.hpp
        native/PlaybackReporter.cpp
        native/PlaybackReporter.hpp
        native/PlaybackTrackMapper.cpp
        native/PlaybackTrackMapper.hpp
        native/MpvApi.cpp
        native/MpvApi.hpp
        native/MpvVideoItem.cpp
        native/MpvVideoItem.hpp
        native/PlaybackCompletionGate.hpp
        native/PlaybackStallWatchdog.hpp
        native/DevelopmentHooks.cpp
        native/DevelopmentHooks.hpp
        native/PerformanceTrace.cpp
        native/PerformanceTrace.hpp
        native/UpscalingPerformancePolicy.cpp
        native/UpscalingPerformancePolicy.hpp
        native/UpscalingRuntimeConfig.cpp
        native/UpscalingRuntimeConfig.hpp
    )
    target_include_directories(yanami-playback-reporter-tests PRIVATE native)
    target_link_libraries(yanami-playback-reporter-tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::OpenGL
        Qt6::Test
        yanami-mpv-headers
    )
    add_test(
        NAME yanami-playback-reporter-tests
        COMMAND $<TARGET_FILE:yanami-playback-reporter-tests>
    )
    if(NOT WIN32)
        set_tests_properties(yanami-playback-reporter-tests PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        )
    endif()
    add_executable(yanami-playback-telemetry-tests
        tests/PlaybackTelemetryTests.cpp
        native/BackendPorts.hpp
        native/PlaybackTelemetry.cpp
        native/PlaybackTelemetry.hpp
    )
    target_include_directories(yanami-playback-telemetry-tests PRIVATE native)
    target_link_libraries(yanami-playback-telemetry-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-playback-telemetry-tests
        COMMAND $<TARGET_FILE:yanami-playback-telemetry-tests>
    )
    add_executable(yanami-playback-report-queue-tests
        tests/PlaybackReportQueueTests.cpp
        native/BackendPorts.hpp
        native/PlaybackReportQueue.cpp
        native/PlaybackReportQueue.hpp
    )
    target_include_directories(yanami-playback-report-queue-tests PRIVATE native)
    target_link_libraries(yanami-playback-report-queue-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-playback-report-queue-tests
        COMMAND $<TARGET_FILE:yanami-playback-report-queue-tests>
    )
    add_executable(yanami-rust-bridge-error-tests
        tests/RustBridgeErrorTests.cpp
        native/RustBridgeError.cpp
        native/RustBridgeError.hpp
    )
    target_include_directories(yanami-rust-bridge-error-tests PRIVATE native)
    target_link_libraries(yanami-rust-bridge-error-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-rust-bridge-error-tests
        COMMAND $<TARGET_FILE:yanami-rust-bridge-error-tests>
    )
    add_executable(yanami-playback-descriptor-tests
        tests/PlaybackDescriptorTests.cpp
        native/PlaybackDescriptor.cpp
        native/PlaybackDescriptor.hpp
    )
    target_include_directories(yanami-playback-descriptor-tests PRIVATE native)
    target_link_libraries(yanami-playback-descriptor-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-playback-descriptor-tests
        COMMAND $<TARGET_FILE:yanami-playback-descriptor-tests>
    )
    add_executable(yanami-qml-architecture-tests
        tests/QmlArchitectureTests.cpp
    )
    target_compile_definitions(yanami-qml-architecture-tests PRIVATE
        "YANAMI_QML_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/qml\""
    )
    target_link_libraries(yanami-qml-architecture-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-qml-architecture-tests
        COMMAND $<TARGET_FILE:yanami-qml-architecture-tests>
    )
    add_executable(yanami-search-coordinator-tests
        tests/SearchCoordinatorTests.cpp
        native/BackendInfrastructure.cpp
        native/BackendInfrastructure.hpp
        native/BackendPorts.hpp
        native/MediaStore.cpp
        native/MediaStore.hpp
        native/PlaybackTelemetry.cpp
        native/PlaybackTelemetry.hpp
        native/RustBridgeError.cpp
        native/RustBridgeError.hpp
        native/RustBridgeRuntime.cpp
        native/RustBridgeRuntime.hpp
        native/SearchCoordinator.cpp
        native/SearchCoordinator.hpp
        native/SearchResultRowsModel.cpp
        native/SearchResultRowsModel.hpp
    )
    target_include_directories(yanami-search-coordinator-tests PRIVATE native)
    target_link_libraries(yanami-search-coordinator-tests PRIVATE
        Qt6::Concurrent
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-search-coordinator-tests
        COMMAND $<TARGET_FILE:yanami-search-coordinator-tests>
    )
    add_executable(yanami-backend-architecture-tests
        tests/BackendArchitectureTests.cpp
    )
    target_compile_definitions(yanami-backend-architecture-tests PRIVATE
        "YANAMI_NATIVE_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/native\""
    )
    target_link_libraries(yanami-backend-architecture-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-backend-architecture-tests
        COMMAND $<TARGET_FILE:yanami-backend-architecture-tests>
    )
    add_executable(yanami-async-resource-tests
        tests/AsyncResourceStateTests.cpp
        native/AsyncResourceState.cpp
        native/AsyncResourceState.hpp
    )
    target_include_directories(yanami-async-resource-tests PRIVATE native)
    target_link_libraries(yanami-async-resource-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-async-resource-tests
        COMMAND $<TARGET_FILE:yanami-async-resource-tests>
    )
    add_executable(yanami-async-operation-tests
        tests/AsyncOperationStateTests.cpp
        native/AsyncOperationState.cpp
        native/AsyncOperationState.hpp
    )
    target_include_directories(yanami-async-operation-tests PRIVATE native)
    target_link_libraries(yanami-async-operation-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-async-operation-tests
        COMMAND $<TARGET_FILE:yanami-async-operation-tests>
    )
    add_executable(yanami-keyed-mutation-scheduler-tests
        tests/KeyedMutationSchedulerTests.cpp
        native/KeyedMutationScheduler.hpp
    )
    target_include_directories(yanami-keyed-mutation-scheduler-tests PRIVATE native)
    target_link_libraries(yanami-keyed-mutation-scheduler-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-keyed-mutation-scheduler-tests
        COMMAND $<TARGET_FILE:yanami-keyed-mutation-scheduler-tests>
    )
    add_executable(yanami-image-editor-view-model-tests
        tests/ImageEditorViewModelTests.cpp
        native/AsyncResourceState.cpp
        native/AsyncResourceState.hpp
        native/BackendPorts.hpp
        native/ImageEditorViewModel.cpp
        native/ImageEditorViewModel.hpp
    )
    target_include_directories(yanami-image-editor-view-model-tests PRIVATE native)
    target_link_libraries(yanami-image-editor-view-model-tests PRIVATE
        Qt6::Core
        Qt6::Test
    )
    add_test(
        NAME yanami-image-editor-view-model-tests
        COMMAND $<TARGET_FILE:yanami-image-editor-view-model-tests>
    )
    add_executable(yanami-application-view-model-tests
        tests/ApplicationViewModelTests.cpp
        native/ApplicationPaths.cpp
        native/ApplicationPaths.hpp
        native/ApplicationViewModel.cpp
        native/ApplicationViewModel.hpp
        native/BackendPorts.hpp
        native/AsyncOperationState.cpp
        native/AsyncOperationState.hpp
        native/AsyncResourceState.cpp
        native/AsyncResourceState.hpp
        native/DiagnosticsViewModel.cpp
        native/DiagnosticsViewModel.hpp
        native/DevelopmentHooks.cpp
        native/DevelopmentHooks.hpp
        native/ImageEditorViewModel.cpp
        native/ImageEditorViewModel.hpp
        native/MetadataEditorViewModel.cpp
        native/MetadataEditorViewModel.hpp
        native/MediaTargetFlowViewModel.cpp
        native/MediaTargetFlowViewModel.hpp
        native/MediaStore.cpp
        native/MediaStore.hpp
        native/RuntimeLogger.cpp
        native/RuntimeLogger.hpp
        native/UpdateChecker.cpp
        native/UpdateChecker.hpp
        native/UpscalingAssetManager.cpp
        native/UpscalingAssetManager.hpp
        native/UpscalingCapabilityProbe.cpp
        native/UpscalingCapabilityProbe.hpp
        native/UpscalingCatalog.cpp
        native/UpscalingCatalog.hpp
        native/UpscalingViewModel.cpp
        native/UpscalingViewModel.hpp
    )
    target_include_directories(yanami-application-view-model-tests PRIVATE native)
    target_link_libraries(yanami-application-view-model-tests PRIVATE
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
        Qt6::Network
        Qt6::Quick
        Qt6::Test
    )
    add_test(
        NAME yanami-application-view-model-tests
        COMMAND $<TARGET_FILE:yanami-application-view-model-tests>
    )
    add_executable(yanami-upscaling-performance-policy-tests
        tests/UpscalingPerformancePolicyTests.cpp
        native/UpscalingPerformancePolicy.cpp
        native/UpscalingPerformancePolicy.hpp
    )
    target_include_directories(
        yanami-upscaling-performance-policy-tests PRIVATE native)
    target_link_libraries(
        yanami-upscaling-performance-policy-tests PRIVATE Qt6::Core Qt6::Test)
    add_test(
        NAME yanami-upscaling-performance-policy-tests
        COMMAND $<TARGET_FILE:yanami-upscaling-performance-policy-tests>
    )
    add_executable(yanami-upscaling-asset-manager-tests
        tests/UpscalingAssetManagerTests.cpp
        native/UpscalingAssetManager.cpp
        native/UpscalingAssetManager.hpp
        native/UpscalingCatalog.hpp
    )
    target_include_directories(
        yanami-upscaling-asset-manager-tests PRIVATE native)
    target_link_libraries(yanami-upscaling-asset-manager-tests PRIVATE
        Qt6::Concurrent
        Qt6::Core
        Qt6::Network
        Qt6::Test
    )
    add_test(
        NAME yanami-upscaling-asset-manager-tests
        COMMAND $<TARGET_FILE:yanami-upscaling-asset-manager-tests>
    )
    add_executable(yanami-upscaling-catalog-tests
        tests/UpscalingCatalogTests.cpp
        native/UpscalingCatalog.cpp
        native/UpscalingCatalog.hpp
    )
    target_include_directories(yanami-upscaling-catalog-tests PRIVATE native)
    target_link_libraries(
        yanami-upscaling-catalog-tests PRIVATE Qt6::Core Qt6::Test)
    add_test(
        NAME yanami-upscaling-catalog-tests
        COMMAND $<TARGET_FILE:yanami-upscaling-catalog-tests>
    )
    add_executable(yanami-upscaling-runtime-config-tests
        tests/UpscalingRuntimeConfigTests.cpp
        native/UpscalingRuntimeConfig.cpp
        native/UpscalingRuntimeConfig.hpp
    )
    target_include_directories(
        yanami-upscaling-runtime-config-tests PRIVATE native)
    target_link_libraries(
        yanami-upscaling-runtime-config-tests PRIVATE Qt6::Core Qt6::Test)
    add_test(
        NAME yanami-upscaling-runtime-config-tests
        COMMAND $<TARGET_FILE:yanami-upscaling-runtime-config-tests>
    )
    add_executable(yanami-upscaling-capability-probe-tests
        tests/UpscalingCapabilityProbeTests.cpp
        native/UpscalingCapabilityProbe.cpp
        native/UpscalingCapabilityProbe.hpp
    )
    target_include_directories(
        yanami-upscaling-capability-probe-tests PRIVATE native)
    target_link_libraries(yanami-upscaling-capability-probe-tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::Test
    )
    add_test(
        NAME yanami-upscaling-capability-probe-tests
        COMMAND $<TARGET_FILE:yanami-upscaling-capability-probe-tests>
    )
    add_executable(yanami-upscaling-perf-probe
        tests/performance/UpscalingPerfProbe.cpp
        native/UpscalingCapabilityProbe.cpp
        native/UpscalingCapabilityProbe.hpp
        native/UpscalingCatalog.cpp
        native/UpscalingCatalog.hpp
        native/UpscalingPerformancePolicy.cpp
        native/UpscalingPerformancePolicy.hpp
    )
    target_include_directories(yanami-upscaling-perf-probe PRIVATE native)
    target_link_libraries(yanami-upscaling-perf-probe PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
    )
    add_test(
        NAME yanami-upscaling-perf-collect
        COMMAND $<TARGET_FILE:yanami-upscaling-perf-probe>
            --fixture
                "${YANAMI_WORKSPACE}/perf/fixtures/upscaling-capability-v1.json"
            --fixture-manifest
                "${YANAMI_WORKSPACE}/perf/fixtures/upscaling-capability-v1.manifest.json"
            --output
                "${CMAKE_CURRENT_BINARY_DIR}/perf/upscaling-hosted-run-manifest.json"
            --mode collect
    )
    set_tests_properties(yanami-upscaling-perf-collect PROPERTIES
        LABELS "performance;collect"
        TIMEOUT 60
    )
    add_executable(yanami-upscaling-download-smoke
        tests/integration/UpscalingDownloadSmoke.cpp
        tests/integration/OfflineNetworkAccessManager.hpp
        native/UpscalingAssetManager.cpp
        native/UpscalingAssetManager.hpp
        native/UpscalingCatalog.cpp
        native/UpscalingCatalog.hpp
    )
    target_include_directories(yanami-upscaling-download-smoke PRIVATE native)
    target_link_libraries(yanami-upscaling-download-smoke PRIVATE
        Qt6::Concurrent
        Qt6::Core
        Qt6::Network
    )
    add_test(
        NAME yanami-upscaling-download-offline-smoke
        COMMAND $<TARGET_FILE:yanami-upscaling-download-smoke>
            anime4k performance offline
    )
    set_tests_properties(yanami-upscaling-download-offline-smoke PROPERTIES
        LABELS "integration;offline;upscaling"
        TIMEOUT 15
    )
    add_executable(yanami-upscaling-mpv-smoke
        tests/integration/UpscalingMpvSmoke.cpp
        tests/integration/OfflineNetworkAccessManager.hpp
        native/ApplicationPaths.cpp
        native/ApplicationPaths.hpp
        native/DevelopmentHooks.cpp
        native/DevelopmentHooks.hpp
        native/MpvApi.cpp
        native/MpvApi.hpp
        native/MpvVideoItem.cpp
        native/MpvVideoItem.hpp
        native/PerformanceTrace.cpp
        native/PerformanceTrace.hpp
        native/PlaybackCompletionGate.hpp
        native/PlaybackStallWatchdog.hpp
        native/UpscalingAssetManager.cpp
        native/UpscalingAssetManager.hpp
        native/UpscalingCapabilityProbe.cpp
        native/UpscalingCapabilityProbe.hpp
        native/UpscalingCatalog.cpp
        native/UpscalingCatalog.hpp
        native/UpscalingPerformancePolicy.cpp
        native/UpscalingPerformancePolicy.hpp
        native/UpscalingRuntimeConfig.cpp
        native/UpscalingRuntimeConfig.hpp
    )
    target_include_directories(yanami-upscaling-mpv-smoke PRIVATE native)
    target_link_libraries(yanami-upscaling-mpv-smoke PRIVATE
        Qt6::Concurrent
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::OpenGL
        Qt6::Quick
        yanami-mpv-headers
    )
    add_test(
        NAME yanami-upscaling-mpv-offline-smoke
        COMMAND $<TARGET_FILE:yanami-upscaling-mpv-smoke>
            anime4k performance offline
    )
    set_tests_properties(yanami-upscaling-mpv-offline-smoke PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_OPENGL=software;LIBGL_ALWAYS_SOFTWARE=1"
        LABELS "integration;mpv;offline;upscaling"
        TIMEOUT 30
    )
    add_executable(yanami-update-checker-tests
        tests/UpdateCheckerTests.cpp
        native/UpdateChecker.cpp
        native/UpdateChecker.hpp
    )
    target_include_directories(yanami-update-checker-tests PRIVATE native)
    target_link_libraries(yanami-update-checker-tests PRIVATE
        Qt6::Network
        Qt6::Test
    )
    add_test(
        NAME yanami-update-checker-tests
        COMMAND $<TARGET_FILE:yanami-update-checker-tests>
    )

    add_executable(yanami-performance-trace-tests
        tests/performance/PerformanceTraceTests.cpp
        native/PerformanceTrace.cpp
        native/PerformanceTrace.hpp
    )
    target_include_directories(yanami-performance-trace-tests PRIVATE native)
    target_link_libraries(yanami-performance-trace-tests PRIVATE Qt6::Core)
    add_test(
        NAME yanami-performance-trace-tests
        COMMAND $<TARGET_FILE:yanami-performance-trace-tests>
    )

    add_executable(yanami-desktop-perf-probe
        tests/performance/DesktopPerfProbe.cpp
        native/BackendInfrastructure.cpp
        native/BackendInfrastructure.hpp
        native/BackendPorts.hpp
        native/MediaStore.cpp
        native/MediaStore.hpp
        native/PlaybackTelemetry.cpp
        native/PlaybackTelemetry.hpp
        native/RequestCoordinator.hpp
        native/RustBridgeError.cpp
        native/RustBridgeError.hpp
        native/RustBridgeRuntime.cpp
        native/RustBridgeRuntime.hpp
        native/SearchCoordinator.cpp
        native/SearchCoordinator.hpp
        native/SearchResultRowsModel.cpp
        native/SearchResultRowsModel.hpp
    )
    target_include_directories(yanami-desktop-perf-probe PRIVATE native)
    target_compile_definitions(yanami-desktop-perf-probe PRIVATE
        "YANAMI_PERF_BUILD_TYPE=\"$<CONFIG>\""
        "YANAMI_PERF_VERSION=\"${YANAMI_BUILD_VERSION}\""
    )
    target_link_libraries(yanami-desktop-perf-probe PRIVATE
        Qt6::Concurrent
        Qt6::Core
    )
    add_test(
        NAME yanami-desktop-perf-collect
        COMMAND $<TARGET_FILE:yanami-desktop-perf-probe>
            --profile pull-request
            --mode collect
            --output "${CMAKE_CURRENT_BINARY_DIR}/perf/desktop-media-store.json"
    )
    set_tests_properties(yanami-desktop-perf-collect PROPERTIES
        LABELS "performance;collect"
        TIMEOUT 180
    )

    qt_add_executable(yanami-danmaku-perf-probe
        tests/performance/DanmakuPerfProbe.cpp
    )
    qt_add_qml_module(yanami-danmaku-perf-probe
        URI Yanami.DanmakuPerfProbe
        VERSION 1.0
        IMPORTS
            TARGET yanami-ui
        QML_FILES
            tests/performance/DanmakuPerfHarness.qml
    )
    target_compile_definitions(yanami-danmaku-perf-probe PRIVATE
        "YANAMI_PERF_BUILD_TYPE=\"$<CONFIG>\""
        "YANAMI_PERF_VERSION=\"${YANAMI_BUILD_VERSION}\""
    )
    target_link_libraries(yanami-danmaku-perf-probe PRIVATE
        Qt6::Gui
        Qt6::Quick
        yanami-uiplugin
    )

    qt_add_executable(yanami-popup-quicktests
        tests/qml/PopupQuickTestMain.cpp
    )
    target_compile_definitions(yanami-popup-quicktests PRIVATE
        "QUICK_TEST_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/qml\""
    )
    target_link_libraries(yanami-popup-quicktests PRIVATE
        Qt6::QuickTest
        Qt6::QuickControls2
        yanami-uiplugin
    )
    add_test(
        NAME yanami-popup-quicktests
        COMMAND $<TARGET_FILE:yanami-popup-quicktests>
    )
    set_tests_properties(yanami-popup-quicktests PROPERTIES
        ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    )

    if(WIN32)
        add_test(
            NAME yanami-desktop-build-runtime-smoke
            COMMAND $<TARGET_FILE:yanami-desktop> --runtime-smoke-test
        )
        set_tests_properties(yanami-desktop-build-runtime-smoke PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=;QML2_IMPORT_PATH="
        )
        set_property(TEST yanami-input-modality-tests APPEND PROPERTY ENVIRONMENT
            "QT_PLUGIN_PATH=${YANAMI_QT_PREFIX}/share/qt6/plugins"
        )
        set_property(TEST yanami-popup-quicktests APPEND PROPERTY ENVIRONMENT
            "QT_PLUGIN_PATH=${YANAMI_QT_PREFIX}/share/qt6/plugins"
            "QML_IMPORT_PATH=${YANAMI_QT_PREFIX}/share/qt6/qml"
        )
        set_tests_properties(
            yanami-runtime-logger-tests
            yanami-development-hooks-disabled-tests
            yanami-input-modality-tests
            yanami-media-store-tests
            yanami-playback-track-mapper-tests
            yanami-playback-stall-watchdog-tests
            yanami-playback-reporter-tests
            yanami-playback-telemetry-tests
            yanami-playback-report-queue-tests
            yanami-rust-bridge-error-tests
            yanami-playback-descriptor-tests
            yanami-qml-architecture-tests
            yanami-backend-architecture-tests
            yanami-async-resource-tests
            yanami-async-operation-tests
            yanami-keyed-mutation-scheduler-tests
            yanami-image-editor-view-model-tests
            yanami-application-view-model-tests
            yanami-upscaling-perf-collect
            yanami-update-checker-tests
            yanami-performance-trace-tests
            yanami-desktop-perf-collect
            yanami-popup-quicktests
            PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${YANAMI_QT_PREFIX}/bin"
        )
    endif()
endif()

set(CPACK_PACKAGE_NAME "Yanami")
set(CPACK_PACKAGE_VENDOR "Yanami contributors")
set(CPACK_PACKAGE_VERSION "${YANAMI_BUILD_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "Yanami-${YANAMI_BUILD_VERSION}-${YANAMI_PACKAGE_PLATFORM}-${YANAMI_PACKAGE_ARCHITECTURE}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CPACK_PACKAGE_CHECKSUM SHA256)
if(APPLE)
    set(CPACK_GENERATOR DragNDrop)
    # LZFSE keeps the DMG compressed while reducing the compression work on
    # hosted Intel macOS runners compared with CPack's zlib default.
    set(CPACK_DMG_FORMAT ULFO)
elseif(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR TGZ)
endif()
include(CPack)
