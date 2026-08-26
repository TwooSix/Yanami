if(NOT DEFINED YANAMI_PACKAGE_ROOT OR NOT IS_DIRECTORY "${YANAMI_PACKAGE_ROOT}")
    message(FATAL_ERROR "YANAMI_PACKAGE_ROOT is missing: ${YANAMI_PACKAGE_ROOT}")
endif()

get_filename_component(package_root "${YANAMI_PACKAGE_ROOT}" REALPATH)
set(manifest_path "${package_root}/SHA256SUMS.txt")
if(NOT EXISTS "${manifest_path}")
    message(FATAL_ERROR "Package manifest is missing: ${manifest_path}")
endif()

file(STRINGS "${manifest_path}" manifest_lines)
set(manifest_files)
foreach(manifest_line IN LISTS manifest_lines)
    if(manifest_line STREQUAL "" OR manifest_line MATCHES "^#")
        continue()
    endif()
    if(NOT manifest_line MATCHES "^([0-9a-fA-F]+)  ([0-9]+)  (.+)$")
        message(FATAL_ERROR "Malformed package manifest entry: ${manifest_line}")
    endif()
    string(LENGTH "${CMAKE_MATCH_1}" hash_length)
    if(NOT hash_length EQUAL 64)
        message(FATAL_ERROR "Malformed package manifest hash: ${manifest_line}")
    endif()
    string(TOLOWER "${CMAKE_MATCH_1}" expected_hash)
    set(expected_size "${CMAKE_MATCH_2}")
    set(relative_path "${CMAKE_MATCH_3}")
    if(IS_ABSOLUTE "${relative_path}"
        OR relative_path MATCHES "(^|/)\\.\\.(/|$)"
        OR relative_path MATCHES "\\\\")
        message(FATAL_ERROR
            "Package manifest path is not a portable relative path: ${relative_path}")
    endif()
    list(FIND manifest_files "${relative_path}" duplicate_index)
    if(NOT duplicate_index EQUAL -1)
        message(FATAL_ERROR "Duplicate package manifest path: ${relative_path}")
    endif()

    set(full_path "${package_root}/${relative_path}")
    if(NOT EXISTS "${full_path}" OR IS_DIRECTORY "${full_path}"
        OR IS_SYMLINK "${full_path}")
        message(FATAL_ERROR "Manifest file is missing or not regular: ${relative_path}")
    endif()
    get_filename_component(resolved_path "${full_path}" REALPATH)
    cmake_path(IS_PREFIX package_root "${resolved_path}" NORMALIZE within_root)
    if(NOT within_root OR resolved_path STREQUAL package_root)
        message(FATAL_ERROR "Package manifest path escapes its root: ${relative_path}")
    endif()

    file(SIZE "${full_path}" actual_size)
    file(SHA256 "${full_path}" actual_hash)
    string(TOLOWER "${actual_hash}" actual_hash)
    if(NOT actual_size EQUAL expected_size OR NOT actual_hash STREQUAL expected_hash)
        message(FATAL_ERROR "Package manifest mismatch: ${relative_path}")
    endif()
    list(APPEND manifest_files "${relative_path}")
endforeach()

if(NOT manifest_files)
    message(FATAL_ERROR "Package manifest contains no payload files")
endif()

file(GLOB_RECURSE package_files
    LIST_DIRECTORIES FALSE
    RELATIVE "${package_root}"
    "${package_root}/*")
list(FILTER package_files EXCLUDE REGEX "^SHA256SUMS\\.txt$")
set(regular_package_files)
foreach(relative_path IN LISTS package_files)
    if(NOT IS_SYMLINK "${package_root}/${relative_path}")
        list(APPEND regular_package_files "${relative_path}")
    endif()
endforeach()
list(SORT manifest_files)
list(SORT regular_package_files)
if(NOT manifest_files STREQUAL regular_package_files)
    message(FATAL_ERROR
        "Package manifest does not cover the exact regular-file payload. "
        "Manifest: [${manifest_files}] Payload: [${regular_package_files}]")
endif()

list(LENGTH manifest_files manifest_file_count)
message(STATUS
    "Verified ${manifest_file_count} package manifest entries under ${package_root}")
