foreach(required_variable IN ITEMS
        YANAMI_BUILD_DIR
        YANAMI_STAGE_ROOT
        YANAMI_CONFIG
        YANAMI_PLATFORM
        YANAMI_EXECUTABLE_RELATIVE
        YANAMI_BRIDGE_RELATIVE
        YANAMI_QT_PREFIX
        YANAMI_EXPECTED_VERSION
        YANAMI_EXPECTED_SOURCE_VERSION
        YANAMI_EXPECTED_COMMIT
        YANAMI_EXPECTED_RUN_ID
        YANAMI_EXPECTED_ARCHITECTURE
        YANAMI_CMAKE_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

get_filename_component(build_dir "${YANAMI_BUILD_DIR}" ABSOLUTE)
get_filename_component(stage_root "${YANAMI_STAGE_ROOT}" ABSOLUTE)
cmake_path(IS_PREFIX build_dir "${stage_root}" NORMALIZE stage_is_within_build)
if(NOT stage_is_within_build OR stage_root STREQUAL build_dir)
    message(FATAL_ERROR "Unsafe install smoke staging root: ${stage_root}")
endif()
file(REMOVE_RECURSE "${stage_root}")

if(YANAMI_PLATFORM STREQUAL "Windows")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "YANAMI_PR_INSTALL_SMOKE=1"
            "${CMAKE_COMMAND}" --install "${build_dir}"
            --prefix "${stage_root}" --config "${YANAMI_CONFIG}"
        RESULT_VARIABLE install_result
        COMMAND_ECHO STDOUT)
else()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --install "${build_dir}"
            --prefix "${stage_root}" --config "${YANAMI_CONFIG}"
        RESULT_VARIABLE install_result
        COMMAND_ECHO STDOUT)
endif()
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "Installing the PR smoke tree failed with exit code ${install_result}")
endif()

set(YANAMI_PACKAGE_ROOT "${stage_root}")
include("${YANAMI_CMAKE_DIR}/VerifyPackageManifest.cmake")

set(required_common_files
    LICENSE
    README.md
    README.zh-CN.md
    THIRD_PARTY_NOTICES.md
    BUILD_INFO.json
    SHA256SUMS.txt
    licenses/rust/THIRD_PARTY_LICENSES.html
    "${YANAMI_EXECUTABLE_RELATIVE}"
    "${YANAMI_BRIDGE_RELATIVE}")
foreach(relative_path IN LISTS required_common_files)
    if(NOT EXISTS "${stage_root}/${relative_path}")
        message(FATAL_ERROR "Installed payload is missing: ${relative_path}")
    endif()
endforeach()

file(READ "${stage_root}/BUILD_INFO.json" build_info)
foreach(field IN ITEMS version sourceVersion commit workflowRunId architecture)
    string(JSON actual_value ERROR_VARIABLE json_error GET "${build_info}" "${field}")
    if(json_error)
        message(FATAL_ERROR "BUILD_INFO.json has no valid '${field}': ${json_error}")
    endif()
    if(field STREQUAL "version")
        set(expected_value "${YANAMI_EXPECTED_VERSION}")
    elseif(field STREQUAL "sourceVersion")
        set(expected_value "${YANAMI_EXPECTED_SOURCE_VERSION}")
    elseif(field STREQUAL "commit")
        set(expected_value "${YANAMI_EXPECTED_COMMIT}")
    elseif(field STREQUAL "workflowRunId")
        set(expected_value "${YANAMI_EXPECTED_RUN_ID}")
    else()
        set(expected_value "${YANAMI_EXPECTED_ARCHITECTURE}")
    endif()
    if(NOT actual_value STREQUAL expected_value)
        message(FATAL_ERROR
            "BUILD_INFO.json ${field} '${actual_value}' does not match '${expected_value}'")
    endif()
endforeach()
string(JSON actual_platform ERROR_VARIABLE platform_error
    GET "${build_info}" platform)
if(platform_error OR NOT actual_platform STREQUAL YANAMI_PLATFORM)
    message(FATAL_ERROR
        "BUILD_INFO.json platform '${actual_platform}' does not match '${YANAMI_PLATFORM}'")
endif()

set(executable "${stage_root}/${YANAMI_EXECUTABLE_RELATIVE}")
get_filename_component(executable_directory "${executable}" DIRECTORY)
if(YANAMI_PLATFORM STREQUAL "Windows")
    foreach(relative_path IN ITEMS
            bin/platforms/qwindows.dll
            bin/platforms/qoffscreen.dll
            bin/SDL3.dll
            bin/vulkan-1.dll)
        if(NOT EXISTS "${stage_root}/${relative_path}")
            message(FATAL_ERROR "Windows runtime closure is missing: ${relative_path}")
        endif()
    endforeach()
    file(GLOB mpv_runtime "${stage_root}/bin/*mpv*.dll")
    if(NOT mpv_runtime)
        message(FATAL_ERROR "Windows runtime closure contains no libmpv DLL")
    endif()
elseif(YANAMI_PLATFORM STREQUAL "macOS")
    file(GLOB_RECURSE mpv_runtime
        "${stage_root}/Yanami.app/Contents/*libmpv*.dylib")
    if(NOT mpv_runtime)
        message(FATAL_ERROR "macOS runtime closure contains no libmpv dylib")
    endif()
endif()

set(data_root "${stage_root}-runtime-data")
get_filename_component(data_parent "${data_root}/.." ABSOLUTE)
cmake_path(IS_PREFIX data_parent "${data_root}" NORMALIZE safe_data_root)
if(NOT safe_data_root OR data_root STREQUAL data_parent)
    message(FATAL_ERROR "Unsafe install smoke data root: ${data_root}")
endif()
file(REMOVE_RECURSE "${data_root}")
file(MAKE_DIRECTORY "${data_root}")

set(runtime_environment
    "QT_QUICK_BACKEND=software"
    "QML_IMPORT_PATH="
    "QML2_IMPORT_PATH="
    "YANAMI_DEV_AUTOPLAY_FIRST=1"
    "YANAMI_DEV_SCREENSHOT_PATH=${data_root}/forbidden.png"
    "YANAMI_DEV_LOG_PATH=${data_root}/forbidden.log")
if(YANAMI_PLATFORM STREQUAL "Windows")
    list(APPEND runtime_environment
        "QT_QPA_PLATFORM=offscreen"
        "PATH=${executable_directory}"
        "APPDATA=${data_root}/Roaming"
        "LOCALAPPDATA=${data_root}/Local")
elseif(YANAMI_PLATFORM STREQUAL "Linux")
    list(APPEND runtime_environment
        "QT_QPA_PLATFORM=offscreen"
        "QT_PLUGIN_PATH=${YANAMI_QT_PREFIX}/plugins"
        "LD_LIBRARY_PATH=${YANAMI_QT_PREFIX}/lib:$ENV{LD_LIBRARY_PATH}"
        "XDG_DATA_HOME=${data_root}/data"
        "XDG_CACHE_HOME=${data_root}/cache")
else()
    list(APPEND runtime_environment
        "HOME=${data_root}/home"
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env ${runtime_environment}
        "${executable}" --runtime-smoke-test
    WORKING_DIRECTORY "${executable_directory}"
    RESULT_VARIABLE smoke_result
    OUTPUT_VARIABLE smoke_stdout
    ERROR_VARIABLE smoke_stderr
    TIMEOUT 45)
if(NOT smoke_result EQUAL 0)
    message(STATUS "Install smoke stdout:\n${smoke_stdout}")
    message(STATUS "Install smoke stderr:\n${smoke_stderr}")
    message(FATAL_ERROR "Installed runtime smoke failed: ${smoke_result}")
endif()
if(EXISTS "${data_root}/forbidden.png" OR EXISTS "${data_root}/forbidden.log")
    message(FATAL_ERROR "Production install honored a forbidden development hook")
endif()

# The smoke may write only to its isolated data root. Rechecking proves that
# launching the application did not mutate the installed candidate.
include("${YANAMI_CMAKE_DIR}/VerifyPackageManifest.cmake")
file(REMOVE_RECURSE "${data_root}")
message(STATUS
    "Verified ${YANAMI_PLATFORM} install tree and production runtime smoke at ${stage_root}")
