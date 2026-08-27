if(NOT DEFINED YANAMI_EXECUTABLE OR NOT EXISTS "${YANAMI_EXECUTABLE}")
    message(FATAL_ERROR
        "YANAMI_EXECUTABLE does not exist: ${YANAMI_EXECUTABLE}")
endif()
if(NOT DEFINED YANAMI_OBJDUMP OR NOT EXISTS "${YANAMI_OBJDUMP}")
    message(FATAL_ERROR "YANAMI_OBJDUMP does not exist: ${YANAMI_OBJDUMP}")
endif()

execute_process(
    COMMAND "${YANAMI_OBJDUMP}" -p "${YANAMI_EXECUTABLE}"
    RESULT_VARIABLE objdump_result
    OUTPUT_VARIABLE pe_headers
    ERROR_VARIABLE objdump_error)
if(NOT objdump_result EQUAL 0)
    message(FATAL_ERROR
        "Unable to inspect desktop PE imports: ${objdump_error}")
endif()

string(REGEX MATCHALL "DLL Name:[^\r\n]+" imported_dll_lines "${pe_headers}")
foreach(imported_dll_line IN LISTS imported_dll_lines)
    string(TOLOWER "${imported_dll_line}" imported_dll_line_lower)
    if(imported_dll_line_lower MATCHES
            "dll name:[ \t]*(lib)?mpv(-[0-9]+)?\\.dll")
        message(FATAL_ERROR
            "yanami-desktop.exe must load libmpv on demand, but its PE import "
            "table contains: ${imported_dll_line}")
    endif()
endforeach()

message(STATUS "Desktop PE import table does not contain libmpv")
