# cmake/version.cmake — invoked at build time via cmake -P
# Generates version.h from git describe, falling back to PROJECT_VERSION.
#
# Expected -D flags:
#   SOURCE_DIR      — top-level source directory (for git)
#   GIT_EXECUTABLE  — path to git binary (may be empty)
#   VERSION_IN      — path to version.h.in template
#   VERSION_OUT     — path to write version.h
#   FALLBACK_VERSION — PROJECT_VERSION from CMakeLists.txt

set(RECMEET_GIT_HASH "")
set(RECMEET_VERSION "${FALLBACK_VERSION}")
set(RECMEET_VERSION_MAJOR 0)
set(RECMEET_VERSION_MINOR 0)
set(RECMEET_VERSION_PATCH 0)

if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*" --dirty --always
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_RESULT
    )
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE RECMEET_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(GIT_RESULT EQUAL 0 AND GIT_DESCRIBE MATCHES "^v")
        # Strip leading 'v'
        string(SUBSTRING "${GIT_DESCRIBE}" 1 -1 RECMEET_VERSION)
    endif()
endif()

# Parse major.minor.patch from version string
if(RECMEET_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(RECMEET_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(RECMEET_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(RECMEET_VERSION_PATCH "${CMAKE_MATCH_3}")
endif()

# Configure to a temp file, only overwrite if changed
configure_file("${VERSION_IN}" "${VERSION_OUT}.tmp")
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files
    "${VERSION_OUT}.tmp" "${VERSION_OUT}"
    RESULT_VARIABLE FILES_DIFFER
    OUTPUT_QUIET ERROR_QUIET
)
if(FILES_DIFFER)
    file(RENAME "${VERSION_OUT}.tmp" "${VERSION_OUT}")
else()
    file(REMOVE "${VERSION_OUT}.tmp")
endif()
