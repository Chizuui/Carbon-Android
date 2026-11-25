set(BUILD_COMMIT "unknown")
set(BUILD_BRANCH "unknown")
set(BUILD_DIRTY "")
set(BUILD_VERSION_NAME "unknown")
set(BUILD_VERSION_CODE "0")

find_package(Git QUIET)
if(GIT_EXECUTABLE)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD WORKING_DIRECTORY "${REPO}"
                    OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
    if(NOT _rc AND _head)
        set(BUILD_COMMIT "${_head}")
    endif()

    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD WORKING_DIRECTORY "${REPO}"
                    OUTPUT_VARIABLE _branch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
    if(NOT _rc AND _branch)
        set(BUILD_BRANCH "${_branch}")
    endif()

    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no WORKING_DIRECTORY "${REPO}"
                    OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
    if(NOT _rc AND NOT "${_dirty}" STREQUAL "")
        set(BUILD_DIRTY "-dirty")
    endif()
endif()

string(TIMESTAMP BUILD_TIME "%Y-%m-%d %H:%M:%S")

if(VER_FILE AND EXISTS "${VER_FILE}")
    file(STRINGS "${VER_FILE}" _ver_lines)
    list(LENGTH _ver_lines _ver_len)
    if(_ver_len GREATER_EQUAL 1)
        list(GET _ver_lines 0 BUILD_VERSION_NAME)
    endif()
    if(_ver_len GREATER_EQUAL 2)
        list(GET _ver_lines 1 BUILD_VERSION_CODE)
    endif()
endif()

file(WRITE "${OUT}"
"#pragma once
#define BUILD_COMMIT \"${BUILD_COMMIT}${BUILD_DIRTY}\"
#define BUILD_BRANCH \"${BUILD_BRANCH}\"
#define BUILD_TIME \"${BUILD_TIME}\"
#define BUILD_VERSION \"${BUILD_VERSION_NAME} (${BUILD_VERSION_CODE})\"
")
