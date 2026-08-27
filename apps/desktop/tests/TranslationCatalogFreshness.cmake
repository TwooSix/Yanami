if(NOT DEFINED YANAMI_LUPDATE OR NOT EXISTS "${YANAMI_LUPDATE}")
    message(FATAL_ERROR "YANAMI_LUPDATE must name the Qt lupdate executable")
endif()
if(NOT DEFINED YANAMI_LRELEASE OR NOT EXISTS "${YANAMI_LRELEASE}")
    message(FATAL_ERROR "YANAMI_LRELEASE must name the Qt lrelease executable")
endif()
if(NOT DEFINED YANAMI_LRELEASE_OPTIONS)
    message(FATAL_ERROR "YANAMI_LRELEASE_OPTIONS is required")
endif()
if(NOT DEFINED YANAMI_TRANSLATION OR NOT EXISTS "${YANAMI_TRANSLATION}")
    message(FATAL_ERROR "YANAMI_TRANSLATION must name the checked-in TS catalog")
endif()
if(NOT DEFINED YANAMI_SOURCE_ROOT OR NOT IS_DIRECTORY "${YANAMI_SOURCE_ROOT}")
    message(FATAL_ERROR "YANAMI_SOURCE_ROOT must name the desktop source directory")
endif()
if(NOT DEFINED YANAMI_TEST_ROOT)
    message(FATAL_ERROR "YANAMI_TEST_ROOT is required")
endif()

file(REMOVE_RECURSE "${YANAMI_TEST_ROOT}")
file(MAKE_DIRECTORY "${YANAMI_TEST_ROOT}")
set(updated_translation "${YANAMI_TEST_ROOT}/yanami_zh_CN.ts")
file(COPY_FILE "${YANAMI_TRANSLATION}" "${updated_translation}")

execute_process(
    COMMAND "${YANAMI_LUPDATE}"
        -recursive
        "${YANAMI_SOURCE_ROOT}/native"
        "${YANAMI_SOURCE_ROOT}/qml"
        -no-obsolete
        -ts "${updated_translation}"
    RESULT_VARIABLE update_result
    OUTPUT_VARIABLE update_output
    ERROR_VARIABLE update_error
)
if(NOT update_result EQUAL 0)
    message(FATAL_ERROR
        "lupdate could not audit the translation catalog (${update_result}).\n"
        "${update_output}\n${update_error}")
endif()

file(READ "${updated_translation}" updated_contents)
if(updated_contents MATCHES "type=\"unfinished\"")
    message(FATAL_ERROR
        "The translation catalog is stale or contains unfinished entries. "
        "Run the yanami-update-translations target, translate every new entry, "
        "and remove all unfinished markers.\n${update_output}\n${update_error}")
endif()

string(REPLACE "|" ";" release_options "${YANAMI_LRELEASE_OPTIONS}")

execute_process(
    COMMAND "${YANAMI_LRELEASE}"
        ${release_options}
        "${updated_translation}"
        -qm "${YANAMI_TEST_ROOT}/yanami_zh_CN.qm"
    RESULT_VARIABLE release_result
    OUTPUT_VARIABLE release_output
    ERROR_VARIABLE release_error
)
if(NOT release_result EQUAL 0)
    message(FATAL_ERROR
        "The refreshed translation catalog is not releaseable (${release_result}).\n"
        "${release_output}\n${release_error}")
endif()
set(released_catalog "${YANAMI_TEST_ROOT}/yanami_zh_CN.qm")
if(NOT EXISTS "${released_catalog}")
    message(FATAL_ERROR "lrelease did not create ${released_catalog}")
endif()
file(SIZE "${released_catalog}" released_catalog_size)
if(released_catalog_size EQUAL 0)
    message(FATAL_ERROR "lrelease created an empty ${released_catalog}")
endif()
