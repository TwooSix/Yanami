if(NOT DEFINED YANAMI_PACKAGE_ROOT OR NOT IS_DIRECTORY "${YANAMI_PACKAGE_ROOT}")
    message(FATAL_ERROR "YANAMI_PACKAGE_ROOT is missing: ${YANAMI_PACKAGE_ROOT}")
endif()

file(GLOB_RECURSE package_files
    LIST_DIRECTORIES FALSE
    RELATIVE "${YANAMI_PACKAGE_ROOT}"
    "${YANAMI_PACKAGE_ROOT}/*")
list(FILTER package_files EXCLUDE REGEX "^SHA256SUMS\\.txt$")
list(SORT package_files)

set(manifest "# SHA-256  Size  Relative path\n")
foreach(relative_path IN LISTS package_files)
    # Package manifests describe payload bytes, not platform-specific symlink
    # metadata. The linked regular file is listed separately.
    if(IS_SYMLINK "${YANAMI_PACKAGE_ROOT}/${relative_path}")
        continue()
    endif()
    file(SHA256 "${YANAMI_PACKAGE_ROOT}/${relative_path}" file_hash)
    file(SIZE "${YANAMI_PACKAGE_ROOT}/${relative_path}" file_size)
    string(APPEND manifest "${file_hash}  ${file_size}  ${relative_path}\n")
endforeach()
file(WRITE "${YANAMI_PACKAGE_ROOT}/SHA256SUMS.txt" "${manifest}")
