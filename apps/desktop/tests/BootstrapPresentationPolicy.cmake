if(NOT DEFINED YANAMI_BOOTSTRAP_SOURCE_DIR)
    message(FATAL_ERROR "YANAMI_BOOTSTRAP_SOURCE_DIR is required")
endif()

function(read_bootstrap_source file_name output_variable)
    set(path "${YANAMI_BOOTSTRAP_SOURCE_DIR}/${file_name}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Bootstrap presentation source is missing: ${path}")
    endif()
    file(READ "${path}" content)
    set(${output_variable} "${content}" PARENT_SCOPE)
endfunction()

function(require_absent content label)
    foreach(token IN LISTS ARGN)
        string(FIND "${content}" "${token}" position)
        if(NOT position EQUAL -1)
            message(FATAL_ERROR
                "${label} must not expose an inline startup cancel control: ${token}")
        endif()
    endforeach()
endfunction()

function(require_present content label)
    foreach(token IN LISTS ARGN)
        string(FIND "${content}" "${token}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR
                "${label} lost its explicit timeout/window-close recovery path: ${token}")
        endif()
    endforeach()
endfunction()

function(require_transition content label)
    foreach(token IN LISTS ARGN)
        string(FIND "${content}" "${token}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR
                "${label} lost the shared 220 ms ease-out handoff: ${token}")
        endif()
    endforeach()
endfunction()

read_bootstrap_source("BootstrapWindows.cpp" windows_source)
require_absent("${windows_source}" "Windows"
    "cancelRevealDelay" "cancelButton" "drawCancelButton" "cancelButtonId")
require_present("${windows_source}" "Windows"
    "case WM_CLOSE:" "MB_RETRYCANCEL" "requestSafeCancellation")

read_bootstrap_source("BootstrapMac.mm" mac_source)
require_absent("${mac_source}" "macOS" "cancelButton" "cancelStartup:")
require_present("${mac_source}" "macOS"
    "cancelChild"
    "[alert addButtonWithTitle:@\"Cancel\"]"
    "applicationShouldTerminate"
    "NSApp.delegate = controller"
    "return NSTerminateCancel")
require_transition("${mac_source}" "macOS"
    "context.duration = 0.22"
    "kCAMediaTimingFunctionEaseOut"
    "+ 220ms")

read_bootstrap_source("BootstrapLinux.cpp" linux_source)
require_absent("${linux_source}" "Linux"
    "buttonPixel" "ButtonPressMask" "\"Cancel\", 6")
require_present("${linux_source}" "Linux"
    "deleteMessage" "kill(child, SIGTERM)")
require_transition("${linux_source}" "Linux"
    "constexpr int steps = 11"
    "remaining * remaining * remaining"
    "sleep_for(20ms)")
