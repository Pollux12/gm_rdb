# Include every source file in source directory
file(GLOB_RECURSE SOURCES RELATIVE ${CMAKE_CURRENT_LIST_DIR} *.cpp *.hpp)
source_group(TREE ${CMAKE_CURRENT_LIST_DIR} PREFIX "Sources" FILES ${SOURCES})

add_library(rdb SHARED ${SOURCES})

math(EXPR GM_RDB_VERSION_NUM
    "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")

target_link_libraries(rdb PRIVATE
    gmod::common
    gmod::helpers_extended
    gmod::lua_shared
    sourcesdk::common
    sourcesdk::tier0
    sourcesdk::tier1
)

if(WIN32)
    target_link_libraries(rdb PRIVATE ws2_32)
endif()

target_include_directories(rdb PRIVATE 
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_BINARY_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/../LRDB/include
    ${CMAKE_CURRENT_LIST_DIR}/../asio/asio/include
    ${CMAKE_CURRENT_LIST_DIR}/../picojson
)

target_compile_definitions(rdb PRIVATE
    _WIN32_WINNT=0x0601
    GM_RDB_VERSION=\"${PROJECT_VERSION}\"
    GM_RDB_VERSION_NUM=${GM_RDB_VERSION_NUM}
)

if(WIN32)
    if(MSVC)
        target_compile_options(rdb PRIVATE /UNO_STRING_T)
    else()
        target_compile_options(rdb PRIVATE -UNO_STRING_T)
    endif()
endif()

set_gmod_suffix_prefix(rdb)

# Autoinstall
set(AUTOINSTALL "" CACHE PATH "Autoinstall path")
if(IS_DIRECTORY ${AUTOINSTALL})
    autoinstall(rdb ${AUTOINSTALL})
endif()

# --- Client debug module (gmcl_rdb_client) ---
add_library(rdb_client SHARED ${SOURCES})

target_link_libraries(rdb_client PRIVATE
    gmod::common
    gmod::helpers_extended
    gmod::lua_shared
    sourcesdk::common
    sourcesdk::tier0
    sourcesdk::tier1
)

if(WIN32)
    target_link_libraries(rdb_client PRIVATE ws2_32)
endif()

target_include_directories(rdb_client PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_BINARY_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/../LRDB/include
    ${CMAKE_CURRENT_LIST_DIR}/../asio/asio/include
    ${CMAKE_CURRENT_LIST_DIR}/../picojson
)

target_compile_definitions(rdb_client PRIVATE
    _WIN32_WINNT=0x0601
    GM_RDB_VERSION=\"${PROJECT_VERSION}\"
    GM_RDB_VERSION_NUM=${GM_RDB_VERSION_NUM}
    GMOD_CLIENT_MODULE=1
)

if(WIN32)
    if(MSVC)
        target_compile_options(rdb_client PRIVATE /UNO_STRING_T)
    else()
        target_compile_options(rdb_client PRIVATE -UNO_STRING_T)
    endif()
endif()

# Temporarily set CLIENT_DLL=ON so set_gmod_suffix_prefix produces gmcl_ prefix
set(CLIENT_DLL ON)
set_gmod_suffix_prefix(rdb_client)
set(CLIENT_DLL OFF)

# Autoinstall client module
set(AUTOINSTALL_CLIENT "" CACHE PATH "Autoinstall path for client module")
if(IS_DIRECTORY ${AUTOINSTALL_CLIENT})
    autoinstall(rdb_client ${AUTOINSTALL_CLIENT})
endif()
