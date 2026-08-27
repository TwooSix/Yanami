foreach(required IN ITEMS
        YANAMI_BOOTSTRAP
        YANAMI_SMOKE_CHILD
        YANAMI_STAGE_ROOT
        YANAMI_BUILD_ROOT
        YANAMI_BOOTSTRAP_FILE_NAME
        YANAMI_DESKTOP_FILE_NAME)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

get_filename_component(stage_root "${YANAMI_STAGE_ROOT}" ABSOLUTE)
get_filename_component(build_root "${YANAMI_BUILD_ROOT}" ABSOLUTE)
cmake_path(IS_PREFIX build_root "${stage_root}" NORMALIZE safe_stage)
if(NOT safe_stage OR stage_root STREQUAL build_root)
    message(FATAL_ERROR "Unsafe bootstrap smoke staging root: ${stage_root}")
endif()
file(REMOVE_RECURSE "${stage_root}")
file(MAKE_DIRECTORY "${stage_root}")
file(COPY_FILE "${YANAMI_BOOTSTRAP}"
    "${stage_root}/${YANAMI_BOOTSTRAP_FILE_NAME}" ONLY_IF_DIFFERENT)
file(COPY_FILE "${YANAMI_SMOKE_CHILD}"
    "${stage_root}/${YANAMI_DESKTOP_FILE_NAME}" ONLY_IF_DIFFERENT)

set(launcher "${stage_root}/${YANAMI_BOOTSTRAP_FILE_NAME}")
set(trace "${stage_root}/desktop-trace.jsonl")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "YANAMI_PERF_RUN_ID=bootstrap-smoke"
        "${launcher}"
        --yanami-bootstrap-timeout-ms=2000
        "--performance-trace=${trace}"
        --bootstrap-child-mode=delayed-ready-exit
    WORKING_DIRECTORY "${stage_root}"
    RESULT_VARIABLE ready_result
    TIMEOUT 10)
if(NOT ready_result EQUAL 0)
    message(FATAL_ERROR "Bootstrap ready smoke failed: ${ready_result}")
endif()

set(sidecar "${trace}.bootstrap.jsonl")
if(NOT EXISTS "${sidecar}")
    message(FATAL_ERROR "Bootstrap did not write its trace sidecar")
endif()
file(READ "${sidecar}" sidecar_content)
foreach(milestone IN ITEMS
        bootstrap_first_visible
        bootstrap_animation_active
        desktop_ready
        handoff_complete)
    string(REGEX MATCHALL
        "\\\"milestone\\\":\\\"${milestone}\\\""
        milestone_matches "${sidecar_content}")
    list(LENGTH milestone_matches milestone_count)
    if(NOT milestone_count EQUAL 1)
        message(FATAL_ERROR
            "Bootstrap trace must contain ${milestone} exactly once; got ${milestone_count}")
    endif()
endforeach()
if(NOT sidecar_content MATCHES
        "\\\"progressSemantic\\\":\\\"indeterminate\\\"")
    message(FATAL_ERROR "Bootstrap trace contains no indeterminate progress contract")
endif()
string(REGEX MATCH
    "\\\"monitorMaxDispatchGapMs\\\":([0-9]+)"
    monitor_gap_match "${sidecar_content}")
if(NOT monitor_gap_match)
    message(FATAL_ERROR "Bootstrap trace lacks monitor dispatch-gap evidence")
endif()
if(CMAKE_MATCH_1 GREATER 500)
    message(FATAL_ERROR
        "Bootstrap monitor was blocked in one dispatch for ${CMAKE_MATCH_1} ms")
endif()
foreach(diagnostic_field IN ITEMS
        monitorMaxLoopGapMs
        maxFullLoopGapMs
        maxExistsCallMs
        maxReadyProbeMs
        readyFirstSeenMs
        validateMs
        validationAttributesMs
        validationOpenMs
        validationHandleInformationMs
        validationSizeMs
        validationReadMs
        validationCloseMs)
    string(REGEX MATCH
        "\\\"${diagnostic_field}\\\":([0-9]+)"
        diagnostic_match "${sidecar_content}")
    if(NOT diagnostic_match)
        message(FATAL_ERROR
            "Bootstrap trace lacks ${diagnostic_field} evidence")
    endif()
    if(NOT diagnostic_field STREQUAL "readyFirstSeenMs"
            AND CMAKE_MATCH_1 GREATER 500)
        message(FATAL_ERROR
            "Bootstrap ${diagnostic_field} exceeded 500 ms: ${CMAKE_MATCH_1}")
    endif()
endforeach()

# Also cover the inverse race: the child exits after handoff_complete. The
# launcher must stop waiting and mirror that later exit instead of retaining a
# signaled process handle forever.
execute_process(
    COMMAND "${launcher}"
        --yanami-bootstrap-timeout-ms=2000
        --bootstrap-child-mode=ready
    WORKING_DIRECTORY "${stage_root}"
    RESULT_VARIABLE ready_then_exit_result
    TIMEOUT 5)
if(NOT ready_then_exit_result EQUAL 0)
    message(FATAL_ERROR
        "Bootstrap ready-then-exit smoke failed: ${ready_then_exit_result}")
endif()

execute_process(
    COMMAND "${launcher}"
        --yanami-bootstrap-timeout-ms=200
        --bootstrap-child-mode=timeout
    WORKING_DIRECTORY "${stage_root}"
    RESULT_VARIABLE timeout_result
    TIMEOUT 5)
if(NOT timeout_result EQUAL 70)
    message(FATAL_ERROR
        "Bootstrap timeout smoke expected 70, got ${timeout_result}")
endif()

execute_process(
    COMMAND "${launcher}"
        --yanami-bootstrap-timeout-ms=200
        --bootstrap-child-mode=missing-event
    WORKING_DIRECTORY "${stage_root}"
    RESULT_VARIABLE missing_event_result
    TIMEOUT 5)
if(NOT missing_event_result EQUAL 70)
    message(FATAL_ERROR
        "Bootstrap missing-event smoke expected 70, got ${missing_event_result}")
endif()

# A file with a spoofed PID cannot complete Windows handoff without the
# unnamed event inherited specifically by the launched child.
execute_process(
    COMMAND "${launcher}"
        --yanami-bootstrap-timeout-ms=200
        --bootstrap-child-mode=file-spoof
    WORKING_DIRECTORY "${stage_root}"
    RESULT_VARIABLE file_spoof_result
    TIMEOUT 5)
if(NOT file_spoof_result EQUAL 70)
    message(FATAL_ERROR
        "Bootstrap file-spoof smoke expected 70, got ${file_spoof_result}")
endif()

execute_process(
    COMMAND "${launcher}"
        --yanami-bootstrap-timeout-ms=2000
        --bootstrap-child-mode=exit19
    WORKING_DIRECTORY "${stage_root}"
    RESULT_VARIABLE child_result
    TIMEOUT 5)
if(NOT child_result EQUAL 19)
    message(FATAL_ERROR
        "Bootstrap must preserve child exit code 19, got ${child_result}")
endif()

file(REMOVE_RECURSE "${stage_root}")
message(STATUS
    "Bootstrap event ready, missing/spoof timeout, trace, and exit propagation passed")
