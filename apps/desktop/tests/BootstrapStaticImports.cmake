foreach(required IN ITEMS YANAMI_BOOTSTRAP YANAMI_OBJDUMP)
    if(NOT DEFINED ${required} OR NOT EXISTS "${${required}}")
        message(FATAL_ERROR "${required} is missing: ${${required}}")
    endif()
endforeach()

execute_process(
    COMMAND "${YANAMI_OBJDUMP}" -p "${YANAMI_BOOTSTRAP}"
    RESULT_VARIABLE inspect_result
    OUTPUT_VARIABLE headers
    ERROR_VARIABLE inspect_error)
if(NOT inspect_result EQUAL 0)
    message(FATAL_ERROR "Unable to inspect bootstrap imports: ${inspect_error}")
endif()

string(REGEX MATCHALL "DLL Name:[^\r\n]+" imported_lines "${headers}")
if(NOT imported_lines)
    message(FATAL_ERROR "Bootstrap PE import table is empty or unreadable")
endif()
foreach(imported_line IN LISTS imported_lines)
    string(TOLOWER "${imported_line}" imported_lower)
    if(imported_lower MATCHES "(qt6|mpv|avcodec|avformat|sdl3|yanami_desktop_bridge|libgcc|libstdc\\+\\+|winpthread)")
        message(FATAL_ERROR
            "Bootstrap imports a forbidden application/runtime dependency: ${imported_line}")
    endif()
endforeach()
message(STATUS "Bootstrap PE imports contain only Windows/UCRT system libraries")
