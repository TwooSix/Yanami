if(NOT DEFINED YANAMI_TEST_ROOT OR NOT DEFINED YANAMI_CMAKE_DIR)
    message(FATAL_ERROR "YANAMI_TEST_ROOT and YANAMI_CMAKE_DIR are required")
endif()
get_filename_component(test_root "${YANAMI_TEST_ROOT}" ABSOLUTE)
get_filename_component(test_parent "${test_root}/.." ABSOLUTE)
cmake_path(IS_PREFIX test_parent "${test_root}" NORMALIZE safe_test_root)
if(NOT safe_test_root OR test_root STREQUAL test_parent)
    message(FATAL_ERROR "Unsafe package-manifest test root: ${test_root}")
endif()
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}/bin" "${test_root}/licenses")
file(WRITE "${test_root}/bin/yanami-test" "binary fixture\n")
file(WRITE "${test_root}/licenses/LICENSE" "license fixture\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DYANAMI_PACKAGE_ROOT=${test_root}"
        -P "${YANAMI_CMAKE_DIR}/WritePackageManifest.cmake"
    RESULT_VARIABLE write_result)
if(NOT write_result EQUAL 0)
    message(FATAL_ERROR "Writing the positive manifest fixture failed")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DYANAMI_PACKAGE_ROOT=${test_root}"
        -P "${YANAMI_CMAKE_DIR}/VerifyPackageManifest.cmake"
    RESULT_VARIABLE verify_result)
if(NOT verify_result EQUAL 0)
    message(FATAL_ERROR "The valid manifest fixture was rejected")
endif()

file(APPEND "${test_root}/bin/yanami-test" "tampered\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DYANAMI_PACKAGE_ROOT=${test_root}"
        -P "${YANAMI_CMAKE_DIR}/VerifyPackageManifest.cmake"
    RESULT_VARIABLE tamper_result
    OUTPUT_QUIET ERROR_QUIET)
if(tamper_result EQUAL 0)
    message(FATAL_ERROR "A tampered manifest payload was accepted")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DYANAMI_PACKAGE_ROOT=${test_root}"
        -P "${YANAMI_CMAKE_DIR}/WritePackageManifest.cmake"
    RESULT_VARIABLE rewrite_result)
if(NOT rewrite_result EQUAL 0)
    message(FATAL_ERROR "Rewriting the manifest fixture failed")
endif()
file(WRITE "${test_root}/unmanifested.txt" "not in manifest\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DYANAMI_PACKAGE_ROOT=${test_root}"
        -P "${YANAMI_CMAKE_DIR}/VerifyPackageManifest.cmake"
    RESULT_VARIABLE coverage_result
    OUTPUT_QUIET ERROR_QUIET)
if(coverage_result EQUAL 0)
    message(FATAL_ERROR "An unmanifested payload file was accepted")
endif()

file(REMOVE_RECURSE "${test_root}")
message(STATUS "Package manifest policy positive and negative fixtures passed")
