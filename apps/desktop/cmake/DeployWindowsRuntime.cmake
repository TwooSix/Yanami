if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

if(NOT DEFINED YANAMI_EXECUTABLE OR NOT EXISTS "${YANAMI_EXECUTABLE}")
    message(FATAL_ERROR "YANAMI_EXECUTABLE does not exist: ${YANAMI_EXECUTABLE}")
endif()

foreach(required_variable IN ITEMS
        YANAMI_QML_DIR
        YANAMI_QT_PREFIX
        YANAMI_WINDEPLOYQT
        YANAMI_MPV_RUNTIME
        YANAMI_VULKAN_RUNTIME)
    if(NOT DEFINED ${required_variable} OR NOT EXISTS "${${required_variable}}")
        message(FATAL_ERROR "${required_variable} does not exist: ${${required_variable}}")
    endif()
endforeach()

get_filename_component(app_dir "${YANAMI_EXECUTABLE}" DIRECTORY)
get_filename_component(mpv_name "${YANAMI_MPV_RUNTIME}" NAME)
get_filename_component(vulkan_name "${YANAMI_VULKAN_RUNTIME}" NAME)

# Limit dependency and ownership scans to files that windeployqt can place in
# the runnable application tree.  The build root may also contain nested CPack
# staging and smoke-test directories; recursively scanning the whole root makes
# duplicate Qt DLLs from those directories look like conflicting dependencies.
function(yanami_collect_deployed_runtime_libraries output_variable app_directory)
    file(GLOB runtime_libraries LIST_DIRECTORIES FALSE
        "${app_directory}/*.dll")

    set(qt_plugin_root "${YANAMI_QT_PREFIX}/share/qt6/plugins")
    if(IS_DIRECTORY "${qt_plugin_root}")
        file(GLOB qt_plugin_directories LIST_DIRECTORIES TRUE
            "${qt_plugin_root}/*")
        foreach(qt_plugin_directory IN LISTS qt_plugin_directories)
            if(NOT IS_DIRECTORY "${qt_plugin_directory}")
                continue()
            endif()
            get_filename_component(qt_plugin_category
                "${qt_plugin_directory}" NAME)
            if(IS_DIRECTORY "${app_directory}/${qt_plugin_category}")
                file(GLOB_RECURSE deployed_plugin_libraries
                    LIST_DIRECTORIES FALSE
                    "${app_directory}/${qt_plugin_category}/*.dll")
                list(APPEND runtime_libraries ${deployed_plugin_libraries})
            endif()
        endforeach()
    endif()

    if(IS_DIRECTORY "${app_directory}/qml")
        file(GLOB_RECURSE deployed_qml_libraries LIST_DIRECTORIES FALSE
            "${app_directory}/qml/*.dll")
        list(APPEND runtime_libraries ${deployed_qml_libraries})
    endif()

    list(REMOVE_DUPLICATES runtime_libraries)
    set(${output_variable} "${runtime_libraries}" PARENT_SCOPE)
endfunction()

file(COPY_FILE
    "${YANAMI_MPV_RUNTIME}"
    "${app_dir}/${mpv_name}"
    ONLY_IF_DIFFERENT)
# Seed the application directory before resolving the recursive closure.
# Otherwise GET_RUNTIME_DEPENDENCIES may resolve vulkan-1.dll from System32 on
# developer machines and omit it, producing a package that only works on hosts
# where a graphics driver happened to install the Vulkan loader globally.
file(COPY_FILE
    "${YANAMI_VULKAN_RUNTIME}"
    "${app_dir}/${vulkan_name}"
    ONLY_IF_DIFFERENT)

# MSYS2 keeps qmlimportscanner under share/qt6/bin rather than beside
# windeployqt. Put both tool directories on PATH for the deployment command,
# without making the built application depend on that developer PATH.
set(ENV{PATH}
    "${YANAMI_QT_PREFIX}/share/qt6/bin;${YANAMI_QT_PREFIX}/bin;$ENV{PATH}")
# Keep qwindows for end users and add qoffscreen for package smoke tests on
# hosted Windows runners. The same offscreen QPA is exercised by the desktop
# test suite, so the release smoke uses a runner-proven headless path.
execute_process(
    COMMAND "${YANAMI_WINDEPLOYQT}"
        --release
        --qmldir "${YANAMI_QML_DIR}"
        --compiler-runtime
        --no-translations
        --include-plugins qoffscreen
        --force
        "${YANAMI_EXECUTABLE}"
    RESULT_VARIABLE deploy_result
    COMMAND_ECHO STDOUT)
if(NOT deploy_result EQUAL 0)
    message(FATAL_ERROR "windeployqt failed with exit code ${deploy_result}")
endif()

# The MSYS2 Qt build keeps its compiled-in prefix even after deployment.
# Point the standalone application at the adjacent plugin and QML folders.
file(WRITE "${app_dir}/qt.conf"
    "[Paths]\nPrefix=.\nPlugins=.\nQmlImports=qml\n")

# windeployqt deploys Qt itself but the MSYS2 build does not copy the GNU
# runtime or the codec libraries used transitively by libmpv. Resolve the
# complete dependency closure of the executable and every deployed plugin.
yanami_collect_deployed_runtime_libraries(deployed_libraries "${app_dir}")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${YANAMI_EXECUTABLE}"
    LIBRARIES ${deployed_libraries}
    DIRECTORIES "${app_dir}" "${YANAMI_QT_PREFIX}/bin"
    RESOLVED_DEPENDENCIES_VAR resolved_dependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolved_dependencies
    CONFLICTING_DEPENDENCIES_PREFIX runtime_conflicts
    PRE_EXCLUDE_REGEXES
        "^api-ms-win-.*"
        "^ext-ms-win-.*"
    POST_EXCLUDE_REGEXES
        "^[A-Za-z]:/[Ww]indows/[Ss]ystem32/.*"
        "^[A-Za-z]:/[Ww]indows/[Ss]ysWOW64/.*")

# CMake reports a conflict when an already staged DLL is also present in the
# MSYS2 runtime directory. Accept that duplication only when every candidate is
# byte-identical, and explicitly keep the application-local copy. A mismatched
# duplicate is an unsafe partial-upgrade package and must fail closed.
foreach(conflicting_name IN LISTS runtime_conflicts_FILENAMES)
    set(preferred_dependency "${app_dir}/${conflicting_name}")
    if(NOT EXISTS "${preferred_dependency}")
        message(FATAL_ERROR
            "Conflicting runtime dependency has no application-local copy: "
            "${conflicting_name}")
    endif()
    file(SHA256 "${preferred_dependency}" preferred_hash)
    set(conflicting_paths "${runtime_conflicts_${conflicting_name}}")
    foreach(conflicting_path IN LISTS conflicting_paths)
        file(SHA256 "${conflicting_path}" conflicting_hash)
        if(NOT conflicting_hash STREQUAL preferred_hash)
            message(FATAL_ERROR
                "Conflicting runtime dependency differs from the staged copy: "
                "${conflicting_path}")
        endif()
    endforeach()
    list(APPEND resolved_dependencies "${preferred_dependency}")
endforeach()
list(REMOVE_DUPLICATES resolved_dependencies)

if(unresolved_dependencies)
    list(JOIN unresolved_dependencies ", " unresolved_text)
    message(FATAL_ERROR "Unresolved Windows runtime dependencies: ${unresolved_text}")
endif()

foreach(dependency IN LISTS resolved_dependencies)
    get_filename_component(dependency_name "${dependency}" NAME)
    file(COPY_FILE
        "${dependency}"
        "${app_dir}/${dependency_name}"
        ONLY_IF_DIFFERENT)
endforeach()

if(DEFINED YANAMI_COLLECT_PACKAGE_LICENSES AND YANAMI_COLLECT_PACKAGE_LICENSES)
    if(NOT DEFINED YANAMI_PACMAN OR NOT EXISTS "${YANAMI_PACMAN}")
        message(FATAL_ERROR
            "Release packaging requires pacman package metadata: ${YANAMI_PACMAN}")
    endif()
    set(runtime_package_root "${app_dir}")
    if(DEFINED YANAMI_PACKAGE_ROOT AND IS_DIRECTORY "${YANAMI_PACKAGE_ROOT}")
        set(runtime_package_root "${YANAMI_PACKAGE_ROOT}")
    endif()
    # Resolve ownership from each original MSYS2 path, never from its CPack
    # staging copy. GET_RUNTIME_DEPENDENCIES prefers an already deployed DLL
    # beside the executable on repeated runs, and pacman intentionally rejects
    # those unowned copies.
    yanami_collect_deployed_runtime_libraries(
        packaged_runtime_libraries "${app_dir}")
    set(package_candidates)
    foreach(deployed_library IN LISTS packaged_runtime_libraries)
        file(RELATIVE_PATH deployed_relative "${app_dir}" "${deployed_library}")
        get_filename_component(deployed_name "${deployed_library}" NAME)
        if(deployed_relative STREQUAL "yanami_desktop_bridge.dll")
            continue()
        endif()

        set(original_candidates)
        if(deployed_name STREQUAL "${mpv_name}")
            list(APPEND original_candidates "${YANAMI_MPV_RUNTIME}")
        endif()
        if(deployed_relative MATCHES "^qml/(.*)$")
            list(APPEND original_candidates
                "${YANAMI_QT_PREFIX}/share/qt6/qml/${CMAKE_MATCH_1}")
        endif()
        list(APPEND original_candidates
            "${YANAMI_QT_PREFIX}/share/qt6/plugins/${deployed_relative}"
            "${YANAMI_QT_PREFIX}/bin/${deployed_name}")

        set(original_library)
        foreach(original_candidate IN LISTS original_candidates)
            if(EXISTS "${original_candidate}")
                set(original_library "${original_candidate}")
                break()
            endif()
        endforeach()
        if(NOT original_library)
            message(FATAL_ERROR
                "Unable to map packaged runtime dependency to its MSYS2 source: "
                "${deployed_relative}")
        endif()
        list(APPEND package_candidates "${original_library}")
    endforeach()
    list(REMOVE_DUPLICATES package_candidates)

    # Query all owners in one process. Spawning pacman once per deployed QML
    # plug-in makes ordinary packaging take minutes and hides progress.
    execute_process(
        COMMAND "${YANAMI_PACMAN}" -Qqo ${package_candidates}
        RESULT_VARIABLE owner_result
        OUTPUT_VARIABLE package_owners
        ERROR_VARIABLE owner_error)
    if(NOT owner_result EQUAL 0)
        message(FATAL_ERROR
            "Unable to identify every packaged runtime dependency: ${owner_error}")
    endif()
    string(REPLACE "\r\n" "\n" package_owners "${package_owners}")
    string(REPLACE "\n" ";" runtime_packages "${package_owners}")
    list(FILTER runtime_packages EXCLUDE REGEX "^$")
    list(REMOVE_DUPLICATES runtime_packages)
    list(SORT runtime_packages)

    set(package_metadata
        "MSYS2 package metadata for the staged Yanami runtime\n"
        "Generated by DeployWindowsRuntime.cmake; review before distribution.\n\n")
    set(project_license_owners
        "mingw-w64-ucrt-x86_64-ffmpeg"
        "mingw-w64-ucrt-x86_64-libass"
        "mingw-w64-ucrt-x86_64-mpv")
    foreach(package IN LISTS runtime_packages)
        execute_process(
            COMMAND "${YANAMI_PACMAN}" -Qi "${package}"
            RESULT_VARIABLE info_result
            OUTPUT_VARIABLE package_info
            ERROR_VARIABLE package_error)
        if(NOT info_result EQUAL 0)
            message(FATAL_ERROR
                "Unable to inspect runtime package ${package}: ${package_error}")
        endif()
        string(APPEND package_metadata "${package_info}\n")
        string(REGEX MATCH
            "(^|\n)Version[ \t]*:[ \t]*([^\r\n]+)"
            package_version_match "${package_info}")
        if(NOT package_version_match)
            message(FATAL_ERROR
                "Unable to parse the installed version of runtime package ${package}")
        endif()
        set(installed_package_version "${CMAKE_MATCH_2}")
        string(STRIP "${installed_package_version}" installed_package_version)

        execute_process(
            COMMAND "${YANAMI_PACMAN}" -Ql "${package}"
            RESULT_VARIABLE list_result
            OUTPUT_VARIABLE package_files
            ERROR_VARIABLE package_error)
        if(NOT list_result EQUAL 0)
            message(FATAL_ERROR
                "Unable to list runtime package ${package}: ${package_error}")
        endif()
        string(REPLACE "\r\n" "\n" package_files "${package_files}")
        string(REPLACE "\n" ";" package_file_lines "${package_files}")
        set(package_license_files)
        foreach(package_file_line IN LISTS package_file_lines)
            if(NOT package_file_line MATCHES "^[^ ]+ (/ucrt64/.+)$")
                continue()
            endif()
            set(unix_license_path "${CMAKE_MATCH_1}")
            get_filename_component(license_name "${unix_license_path}" NAME)
            string(TOUPPER "${license_name}" upper_license_name)
            set(in_standard_license_directory FALSE)
            if(unix_license_path MATCHES "^/ucrt64/share/licenses/.+")
                set(in_standard_license_directory TRUE)
            endif()
            if(NOT in_standard_license_directory
                    AND NOT upper_license_name MATCHES
                        "^(LICENSE|LICENCE|COPYING|COPYRIGHT|NOTICE)([._-].*|[Vv][0-9].*)?$")
                continue()
            endif()
            string(REGEX REPLACE "^/ucrt64/" "" relative_license_path
                "${unix_license_path}")
            set(source_license "${YANAMI_QT_PREFIX}/${relative_license_path}")
            if(EXISTS "${source_license}" AND NOT IS_DIRECTORY "${source_license}")
                set(destination_license
                    "${runtime_package_root}/licenses/msys2/${package}/${relative_license_path}")
                get_filename_component(destination_directory
                    "${destination_license}" DIRECTORY)
                file(MAKE_DIRECTORY "${destination_directory}")
                if(EXISTS "${destination_license}")
                    file(SHA256 "${source_license}" source_hash)
                    file(SHA256 "${destination_license}" destination_hash)
                    if(NOT source_hash STREQUAL destination_hash)
                        message(FATAL_ERROR
                            "Conflicting license files map to ${destination_license}")
                    endif()
                endif()
                file(COPY_FILE
                    "${source_license}"
                    "${destination_license}"
                    ONLY_IF_DIFFERENT)
                list(APPEND package_license_files "${destination_license}")
            endif()
        endforeach()

        if(NOT package_license_files
                AND DEFINED YANAMI_LICENSE_FALLBACK_ROOT
                AND IS_DIRECTORY "${YANAMI_LICENSE_FALLBACK_ROOT}/${package}")
            set(fallback_directory "${YANAMI_LICENSE_FALLBACK_ROOT}/${package}")
            if(NOT EXISTS "${fallback_directory}/SOURCE.toml")
                message(FATAL_ERROR
                    "Fallback license metadata is missing for ${package}")
            endif()
            file(STRINGS "${fallback_directory}/SOURCE.toml"
                fallback_metadata_lines ENCODING UTF-8)
            set(fallback_package)
            set(fallback_version)
            set(fallback_manifest_paths)
            set(fallback_manifest_hashes)
            set(pending_manifest_path)
            foreach(metadata_line IN LISTS fallback_metadata_lines)
                string(STRIP "${metadata_line}" metadata_line)
                if(metadata_line MATCHES "^package = \"([^\"]+)\"$")
                    set(fallback_package "${CMAKE_MATCH_1}")
                elseif(metadata_line MATCHES "^package_version = \"([^\"]+)\"$")
                    set(fallback_version "${CMAKE_MATCH_1}")
                elseif(metadata_line MATCHES "^path = \"([^\"]+)\"$")
                    set(pending_manifest_path "${CMAKE_MATCH_1}")
                elseif(metadata_line MATCHES "^sha256 = \"([0-9a-fA-F]+)\"$"
                        AND pending_manifest_path)
                    string(TOLOWER "${CMAKE_MATCH_1}" manifest_hash)
                    list(APPEND fallback_manifest_paths "${pending_manifest_path}")
                    list(APPEND fallback_manifest_hashes "${manifest_hash}")
                    unset(pending_manifest_path)
                endif()
            endforeach()
            if(NOT fallback_package STREQUAL package)
                message(FATAL_ERROR
                    "Fallback metadata package mismatch for ${package}: ${fallback_package}")
            endif()
            if(NOT fallback_version STREQUAL installed_package_version)
                message(FATAL_ERROR
                    "Fallback license version mismatch for ${package}: installed "
                    "${installed_package_version}, fallback ${fallback_version}")
            endif()
            if(pending_manifest_path OR NOT fallback_manifest_paths)
                message(FATAL_ERROR
                    "Fallback license manifest is incomplete for ${package}")
            endif()
            file(GLOB_RECURSE fallback_files
                LIST_DIRECTORIES FALSE
                RELATIVE "${fallback_directory}"
                "${fallback_directory}/*")
            foreach(fallback_relative IN LISTS fallback_files)
                set(fallback_source "${fallback_directory}/${fallback_relative}")
                set(fallback_destination
                    "${runtime_package_root}/licenses/msys2/${package}/fallback/${fallback_relative}")
                get_filename_component(fallback_destination_directory
                    "${fallback_destination}" DIRECTORY)
                file(MAKE_DIRECTORY "${fallback_destination_directory}")
                if(NOT fallback_relative STREQUAL "SOURCE.toml")
                    list(FIND fallback_manifest_paths
                        "${fallback_relative}" fallback_manifest_index)
                    if(fallback_manifest_index EQUAL -1)
                        message(FATAL_ERROR
                            "Unmanifested fallback license file for ${package}: "
                            "${fallback_relative}")
                    endif()
                    list(GET fallback_manifest_hashes
                        ${fallback_manifest_index} expected_fallback_hash)
                    file(SHA256 "${fallback_source}" actual_fallback_hash)
                    string(TOLOWER "${actual_fallback_hash}" actual_fallback_hash)
                    if(NOT actual_fallback_hash STREQUAL expected_fallback_hash)
                        message(FATAL_ERROR
                            "Fallback license hash mismatch for ${package}/"
                            "${fallback_relative}")
                    endif()
                endif()
                file(COPY_FILE "${fallback_source}" "${fallback_destination}"
                    ONLY_IF_DIFFERENT)
                if(NOT fallback_relative STREQUAL "SOURCE.toml")
                    list(APPEND package_license_files "${fallback_destination}")
                endif()
            endforeach()
            list(LENGTH fallback_manifest_paths fallback_manifest_count)
            list(LENGTH package_license_files copied_fallback_count)
            if(NOT copied_fallback_count EQUAL fallback_manifest_count)
                message(FATAL_ERROR
                    "Fallback license manifest/file count mismatch for ${package}")
            endif()
        endif()

        if(NOT package_license_files)
            if(package IN_LIST project_license_owners)
                message(STATUS
                    "Runtime owner ${package} uses the project-level audited license copy")
            else()
                message(FATAL_ERROR
                    "No license or notice text is available for runtime owner ${package}")
            endif()
        endif()
    endforeach()
    file(MAKE_DIRECTORY "${runtime_package_root}/licenses/msys2")
    file(WRITE "${runtime_package_root}/licenses/msys2/PACKAGE_METADATA.txt"
        "${package_metadata}")
endif()
