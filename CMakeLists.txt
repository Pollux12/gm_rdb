cmake_minimum_required(VERSION 3.20)

# Enable IDE folders
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Force x86_64 architecture on MacOSX
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "x86_64")

    # Disable deprecation warnings
    add_compile_options(
        -Wno-deprecated-declarations
    )
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Allow explicit override from CI/local builds, otherwise infer from Git tags.
set(GM_RDB_FALLBACK_VERSION "0.0.9")
set(GM_RDB_VERSION "" CACHE STRING "gm_rdb semantic version (major.minor.patch)")

if(GM_RDB_VERSION STREQUAL "")
    set(_gm_rdb_tag_raw "")

    # Only use a Git-derived version when the current commit is tagged.
    execute_process(
        COMMAND git describe --tags --exact-match
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        RESULT_VARIABLE _gm_rdb_exact_tag_result
        OUTPUT_VARIABLE _gm_rdb_exact_tag
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(_gm_rdb_exact_tag_result EQUAL 0 AND NOT _gm_rdb_exact_tag STREQUAL "")
        set(_gm_rdb_tag_raw "${_gm_rdb_exact_tag}")
    endif()

    string(REGEX REPLACE "^v" "" _gm_rdb_tag_no_prefix "${_gm_rdb_tag_raw}")
    string(REGEX MATCH "^([0-9]+\\.[0-9]+\\.[0-9]+)" _gm_rdb_tag_semver "${_gm_rdb_tag_no_prefix}")

    if(_gm_rdb_tag_semver)
        set(GM_RDB_VERSION "${_gm_rdb_tag_semver}")
    else()
        set(GM_RDB_VERSION "${GM_RDB_FALLBACK_VERSION}")
    endif()
endif()

message(STATUS "gm_rdb resolved version: ${GM_RDB_VERSION}")

# Get the latest abbreviated commit hash of the working branch
execute_process(
    COMMAND git log -1 --format=%h
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Get the current working branch
if(NOT DEFINED GIT_BRANCH)
    execute_process(
        COMMAND git rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

project(gm_rdb
    VERSION ${GM_RDB_VERSION}
    LANGUAGES C CXX
    HOMEPAGE_URL "https://github.com/Pollux12/emmylua-rust/tree/master/gm_rdb"
)

# Enable -fPIC flag
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Require C++ 17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_compile_definitions(WIN64)
endif()

find_package(GarrysmodCommon REQUIRED)

add_subdirectory(source)
