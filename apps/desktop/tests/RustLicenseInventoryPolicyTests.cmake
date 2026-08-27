foreach(required IN ITEMS
        YANAMI_VALID_INVENTORY
        YANAMI_VERIFIER
        YANAMI_WORKSPACE
        YANAMI_TEST_ROOT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${YANAMI_TEST_ROOT}")
file(MAKE_DIRECTORY "${YANAMI_TEST_ROOT}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -P "${YANAMI_VERIFIER}"
    RESULT_VARIABLE valid_result
    OUTPUT_VARIABLE valid_output
    ERROR_VARIABLE valid_error)
if(NOT valid_result EQUAL 0)
    message(FATAL_ERROR
        "The current Rust license inventory was rejected:\n"
        "${valid_output}${valid_error}")
endif()

file(READ "${YANAMI_VALID_INVENTORY}" valid_content)
file(SHA256 "${YANAMI_WORKSPACE}/Cargo.lock" cargo_lock_hash)
string(REPEAT "0" 64 stale_hash)
set(current_marker
    "data-path=\"Cargo.lock\" content=\"${cargo_lock_hash}\"")
set(stale_marker
    "data-path=\"Cargo.lock\" content=\"${stale_hash}\"")
string(REPLACE "${current_marker}" "${stale_marker}"
    stale_content "${valid_content}")
if(stale_content STREQUAL valid_content)
    message(FATAL_ERROR "The Cargo.lock provenance marker was not found")
endif()
set(stale_inventory "${YANAMI_TEST_ROOT}/stale.html")
file(WRITE "${stale_inventory}" "${stale_content}")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DYANAMI_RUST_LICENSE_INVENTORY_OVERRIDE=${stale_inventory}"
        -P "${YANAMI_VERIFIER}"
    RESULT_VARIABLE stale_result
    OUTPUT_VARIABLE stale_output
    ERROR_VARIABLE stale_error)
if(stale_result EQUAL 0 OR
        NOT "${stale_output}${stale_error}" MATCHES
            "inventory is stale for Cargo.lock")
    message(FATAL_ERROR
        "A stale Rust license inventory was not rejected correctly:\n"
        "${stale_output}${stale_error}")
endif()

set(active_inventory "${YANAMI_TEST_ROOT}/active.html")
string(REPLACE "</head>" "<script></script></head>"
    active_content "${valid_content}")
file(WRITE "${active_inventory}" "${active_content}")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DYANAMI_RUST_LICENSE_INVENTORY_OVERRIDE=${active_inventory}"
        -P "${YANAMI_VERIFIER}"
    RESULT_VARIABLE active_result
    OUTPUT_VARIABLE active_output
    ERROR_VARIABLE active_error)
if(active_result EQUAL 0 OR
        NOT "${active_output}${active_error}" MATCHES
            "unexpectedly contains active HTML")
    message(FATAL_ERROR
        "An active Rust license inventory was not rejected correctly:\n"
        "${active_output}${active_error}")
endif()

set(missing_inventory "${YANAMI_TEST_ROOT}/missing.html")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DYANAMI_RUST_LICENSE_INVENTORY_OVERRIDE=${missing_inventory}"
        -P "${YANAMI_VERIFIER}"
    RESULT_VARIABLE missing_result
    OUTPUT_VARIABLE missing_output
    ERROR_VARIABLE missing_error)
if(missing_result EQUAL 0 OR
        NOT "${missing_output}${missing_error}" MATCHES
            "inventory is missing")
    message(FATAL_ERROR
        "A missing Rust license inventory was not rejected correctly:\n"
        "${missing_output}${missing_error}")
endif()

message(STATUS "Rust license inventory policy accepted current provenance and rejected stale, active, and missing inputs")
