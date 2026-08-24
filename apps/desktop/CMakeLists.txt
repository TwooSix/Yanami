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
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Concurrent Gui Network Quick QuickControls2 OpenGL LinguistTools)
get_filename_component(YANAMI_QT_PREFIX "${Qt6_DIR}/../../.." ABSOLUTE)

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
    find_program(PACMAN_EXECUTABLE
        NAMES pacman
        HINTS "${YANAMI_QT_PREFIX}/../usr/bin"
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
    native/ApplicationViewModel.cpp
    native/ApplicationViewModel.hpp
    native/BackendPorts.hpp
    native/AsyncImageKey.hpp
    native/AsyncImageProvider.cpp
    native/AsyncImageProvider.hpp
    native/AsyncResourceState.cpp
    native/AsyncResourceState.hpp
    native/AsyncOperationState.cpp
    native/AsyncOperationState.hpp
    native/BackendInfrastructure.cpp
    native/BackendInfrastructure.hpp
    native/DesktopBackendServices.cpp
    native/DesktopBackendServices.hpp
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
        OUTPUT_NAME "Yanami"
    )
endif()
add_dependencies(yanami-desktop yanami-rust-bridge)
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
        qml/components/DanmakuMenu.qml
        qml/components/DanmakuOverlay.qml
        qml/components/DetailHero.qml
        qml/components/EpisodeCard.qml
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
        qml/components/PlaybackEndOverlay.qml
        qml/components/StatusToast.qml
        qml/components/PlaybackQueueMenu.qml
        qml/components/PlaybackHost.qml
        qml/components/RecentEpisodeCard.qml
        qml/components/RefreshMetadataDialog.qml
        qml/components/RoundedImage.qml
        qml/components/SearchSubmissionPolicy.qml
        qml/components/SmoothFlickable.qml
        qml/components/SmoothGridView.qml
        qml/components/SmoothHorizontalList.qml
        qml/components/SmoothListView.qml
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

qt_add_translations(
    TARGETS yanami-desktop
    SOURCE_TARGETS yanami-desktop yanami-ui
    TS_FILES "${CMAKE_CURRENT_SOURCE_DIR}/i18n/yanami_zh_CN.ts"
    RESOURCE_PREFIX "/i18n"
    LUPDATE_OPTIONS -no-obsolete
    LUPDATE_TARGET yanami-update-translations
    LRELEASE_TARGET yanami-release-translations
)

target_link_libraries(yanami-desktop PRIVATE
    yanami-uiplugin
    Qt6::Concurrent
    Qt6::Gui
    Qt6::Network
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
            "-DYANAMI_VULKAN_RUNTIME=${VULKAN_RUNTIME_DLL}"
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
    install(FILES "${YANAMI_RUST_BRIDGE}"
        DESTINATION "Yanami.app/Contents/MacOS"
    )
else()
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
install(FILES "${YANAMI_WORKSPACE}/licenses/RUST_DEPENDENCIES.md"
    DESTINATION licenses
)
install(FILES "${YANAMI_WORKSPACE}/licenses/rust/THIRD_PARTY_LICENSES.html"
    DESTINATION licenses/rust
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
    )
    install(SCRIPT "${YANAMI_QT_DEPLOY_SCRIPT}")

    configure_file(
        cmake/InstallMacRuntime.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/InstallMacRuntime.cmake"
        @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallMacRuntime.cmake")
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS QuickTest Test)

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
        native/BackendPorts.hpp
        native/PlaybackReporter.cpp
        native/PlaybackReporter.hpp
        native/PlaybackTrackMapper.cpp
        native/PlaybackTrackMapper.hpp
        native/MpvVideoItem.cpp
        native/MpvVideoItem.hpp
        native/PlaybackCompletionGate.hpp
        native/PlaybackStallWatchdog.hpp
        native/DevelopmentHooks.cpp
        native/DevelopmentHooks.hpp
        native/PerformanceTrace.cpp
        native/PerformanceTrace.hpp
    )
    target_include_directories(yanami-playback-reporter-tests PRIVATE native)
    target_link_libraries(yanami-playback-reporter-tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::OpenGL
        Qt6::Test
        MPV::MPV
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
        native/ApplicationViewModel.cpp
        native/ApplicationViewModel.hpp
        native/BackendPorts.hpp
        native/AsyncOperationState.cpp
        native/AsyncOperationState.hpp
        native/AsyncResourceState.cpp
        native/AsyncResourceState.hpp
        native/ImageEditorViewModel.cpp
        native/ImageEditorViewModel.hpp
        native/MetadataEditorViewModel.cpp
        native/MetadataEditorViewModel.hpp
        native/MediaTargetFlowViewModel.cpp
        native/MediaTargetFlowViewModel.hpp
        native/MediaStore.cpp
        native/MediaStore.hpp
        native/UpdateChecker.cpp
        native/UpdateChecker.hpp
    )
    target_include_directories(yanami-application-view-model-tests PRIVATE native)
    target_link_libraries(yanami-application-view-model-tests PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Test
    )
    add_test(
        NAME yanami-application-view-model-tests
        COMMAND $<TARGET_FILE:yanami-application-view-model-tests>
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
