cmake_policy(SET CMP0207 NEW)

if(NOT DEFINED YANAMI_EXECUTABLE OR NOT EXISTS "${YANAMI_EXECUTABLE}")
    message(FATAL_ERROR "YANAMI_EXECUTABLE does not exist: ${YANAMI_EXECUTABLE}")
endif()

foreach(required_variable IN ITEMS
        YANAMI_QML_DIR
        YANAMI_QT_PREFIX
        YANAMI_WINDEPLOYQT
        YANAMI_MPV_RUNTIME)
    if(NOT DEFINED ${required_variable} OR NOT EXISTS "${${required_variable}}")
        message(FATAL_ERROR "${required_variable} does not exist: ${${required_variable}}")
    endif()
endforeach()

get_filename_component(app_dir "${YANAMI_EXECUTABLE}" DIRECTORY)
get_filename_component(mpv_name "${YANAMI_MPV_RUNTIME}" NAME)
file(COPY_FILE
    "${YANAMI_MPV_RUNTIME}"
    "${app_dir}/${mpv_name}"
    ONLY_IF_DIFFERENT)

# MSYS2 keeps qmlimportscanner under share/qt6/bin rather than beside
# windeployqt. Put both tool directories on PATH for the deployment command,
# without making the built application depend on that developer PATH.
set(ENV{PATH}
    "${YANAMI_QT_PREFIX}/share/qt6/bin;${YANAMI_QT_PREFIX}/bin;$ENV{PATH}")
execute_process(
    COMMAND "${YANAMI_WINDEPLOYQT}"
        --release
        --qmldir "${YANAMI_QML_DIR}"
        --compiler-runtime
        --no-translations
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
file(GLOB_RECURSE deployed_libraries LIST_DIRECTORIES FALSE "${app_dir}/*.dll")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${YANAMI_EXECUTABLE}"
    LIBRARIES ${deployed_libraries}
    DIRECTORIES "${app_dir}" "${YANAMI_QT_PREFIX}/bin"
    RESOLVED_DEPENDENCIES_VAR resolved_dependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolved_dependencies
    PRE_EXCLUDE_REGEXES
        "^api-ms-win-.*"
        "^ext-ms-win-.*"
        "^vulkan-1\\.dll$"
    POST_EXCLUDE_REGEXES
        "^[A-Za-z]:/[Ww]indows/[Ss]ystem32/.*"
        "^[A-Za-z]:/[Ww]indows/[Ss]ysWOW64/.*")

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
