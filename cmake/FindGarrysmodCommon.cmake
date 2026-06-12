# Builds danielga/garrysmod_common (premake-only upstream) with vendored CMake glue.
#
# Upstream master carries the helpers/symbols for BOTH Windows x86 and x86_64
# (helpers_extended/source/windows/<arch>/Symbols.cpp), so a single pinned
# submodule serves every architecture. The Source SDK is the one piece that
# still differs per architecture:
#   - 32-bit: garrysmod_common's own sourcesdk-minimal submodule (master pin),
#     which only ships 32-bit link libraries.
#   - 64-bit: a separately pinned checkout of sourcesdk-minimal's
#     x86-64-branch, which ships the x64/linux64/osx64 link libraries.
#
# Target names and semantics mirror the previous dankmolot/garrysmod_common
# CMake overlay so consuming CMakeLists need no changes:
#   gmod::common, gmod::helpers, gmod::helpers_extended, gmod::lua_shared,
#   gmod::scanning, sourcesdk::common, sourcesdk::tier0, sourcesdk::tier1,
#   sourcesdk::lzma (+ sourcesdk::internal / sourcesdk::interfaces on 64-bit).

message(STATUS "Looking for garrysmod_common...")

option(CLIENT_DLL "Build as client dll" OFF)

set(GARRYSMOD_COMMON_PATH "${CMAKE_SOURCE_DIR}/third-party/garrysmod_common"
    CACHE PATH "Path to garrysmod_common (https://github.com/danielga/garrysmod_common)")
cmake_path(ABSOLUTE_PATH GARRYSMOD_COMMON_PATH NORMALIZE)

if(NOT EXISTS "${GARRYSMOD_COMMON_PATH}/helpers_extended/source/InterfacePointers.cpp")
    message(FATAL_ERROR
        "garrysmod_common not found (or incomplete) at '${GARRYSMOD_COMMON_PATH}'. "
        "Run 'git submodule update --init --recursive' or set GARRYSMOD_COMMON_PATH.")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_sourcesdk_default "${CMAKE_SOURCE_DIR}/third-party/sourcesdk-minimal-x64")
else()
    set(_sourcesdk_default "${GARRYSMOD_COMMON_PATH}/sourcesdk-minimal")
endif()
set(SOURCESDK_PATH "${_sourcesdk_default}"
    CACHE PATH "Path to the sourcesdk-minimal checkout matching the target architecture")
cmake_path(ABSOLUTE_PATH SOURCESDK_PATH NORMALIZE)

if(NOT EXISTS "${SOURCESDK_PATH}/public/tier0/platform.h")
    message(FATAL_ERROR
        "sourcesdk-minimal not found (or incomplete) at '${SOURCESDK_PATH}'. "
        "Run 'git submodule update --init --recursive' or set SOURCESDK_PATH.")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/set_gmod_suffix_prefix.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/autoinstall.cmake")

set(_gmc "${GARRYSMOD_COMMON_PATH}")
set(_sdk "${SOURCESDK_PATH}")

# ---------------------------------------------------------------------------
# gmod::common
# ---------------------------------------------------------------------------
add_library(garrysmod_common INTERFACE EXCLUDE_FROM_ALL)
add_library(gmod::common ALIAS garrysmod_common)

target_compile_definitions(garrysmod_common INTERFACE
    GMMODULE
    IS_SERVERSIDE=$<NOT:$<BOOL:${CLIENT_DLL}>>
    GMOD_ALLOW_OLD_TYPES
    GMOD_ALLOW_LIGHTUSERDATA
)
target_include_directories(garrysmod_common SYSTEM INTERFACE "${_gmc}/include")

# ---------------------------------------------------------------------------
# gmod::helpers
# ---------------------------------------------------------------------------
add_library(garrysmod_helpers STATIC EXCLUDE_FROM_ALL
    "${_gmc}/helpers/source/ModuleLoader.cpp")
add_library(gmod::helpers ALIAS garrysmod_helpers)

target_include_directories(garrysmod_helpers SYSTEM PUBLIC "${_gmc}/helpers/include")
target_link_libraries(garrysmod_helpers gmod::common ${CMAKE_DL_LIBS})
set_target_properties(garrysmod_helpers PROPERTIES FOLDER "garrysmod")

# ---------------------------------------------------------------------------
# gmod::lua_shared
# ---------------------------------------------------------------------------
add_library(garrysmod_lua_shared STATIC EXCLUDE_FROM_ALL
    "${_gmc}/lua_shared/source/LuaShared.cpp")
add_library(gmod::lua_shared ALIAS garrysmod_lua_shared)

target_link_libraries(garrysmod_lua_shared gmod::common ${CMAKE_DL_LIBS})
set_target_properties(garrysmod_lua_shared PROPERTIES FOLDER "garrysmod")

# ---------------------------------------------------------------------------
# gmod::scanning
# ---------------------------------------------------------------------------
if(WIN32)
    set(_scanning_platform "windows")
elseif(APPLE)
    set(_scanning_platform "macosx")
else()
    set(_scanning_platform "linux")
endif()

add_library(garrysmod_scanning STATIC EXCLUDE_FROM_ALL
    "${_gmc}/scanning/source/${_scanning_platform}/symbolfinder.cpp")
add_library(gmod::scanning ALIAS garrysmod_scanning)

target_include_directories(garrysmod_scanning SYSTEM PUBLIC "${_gmc}/scanning/include")
target_include_directories(garrysmod_scanning PRIVATE "${_gmc}/scanning/include/scanning")
target_link_libraries(garrysmod_scanning ${CMAKE_DL_LIBS})
set_target_properties(garrysmod_scanning PROPERTIES FOLDER "garrysmod")

# ---------------------------------------------------------------------------
# gmod::helpers_extended
# Upstream split Symbols.cpp by OS and architecture; pick the right one
# explicitly instead of globbing so a layout change fails loudly at configure
# time rather than linking with missing symbols.
# ---------------------------------------------------------------------------
set(_hx "${_gmc}/helpers_extended")

if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_hx_symbols "${_hx}/source/windows/x86_64/Symbols.cpp")
    else()
        set(_hx_symbols "${_hx}/source/windows/x86/Symbols.cpp")
    endif()
elseif(APPLE)
    set(_hx_symbols "${_hx}/source/macosx/Symbols.cpp")
else()
    set(_hx_symbols "${_hx}/source/linux/Symbols.cpp")
endif()

if(NOT EXISTS "${_hx_symbols}")
    message(FATAL_ERROR
        "Expected Symbols source '${_hx_symbols}' does not exist; "
        "the garrysmod_common helpers_extended layout has changed.")
endif()

add_library(garrysmod_helpers_extended STATIC EXCLUDE_FROM_ALL
    "${_hx}/source/FunctionPointers.cpp"
    "${_hx}/source/InterfacePointers.cpp"
    "${_hx_symbols}"
)
add_library(gmod::helpers_extended ALIAS garrysmod_helpers_extended)

target_compile_definitions(garrysmod_helpers_extended PUBLIC IS_SERVERSIDE=1)
target_include_directories(garrysmod_helpers_extended SYSTEM PUBLIC
    "${_hx}/include"
    "${_hx}/include/GarrysMod"
)
target_link_libraries(garrysmod_helpers_extended
    gmod::common gmod::helpers gmod::scanning ${CMAKE_DL_LIBS})
set_target_properties(garrysmod_helpers_extended PROPERTIES FOLDER "garrysmod")

# ---------------------------------------------------------------------------
# sourcesdk::lzma (identical source list on both SDK revisions)
# ---------------------------------------------------------------------------
set(_lzma "${_sdk}/utils/lzma/C")
set(_lzma_sources
    7zAlloc.c 7zArcIn.c 7zBuf.c 7zBuf2.c 7zCrc.c 7zCrcOpt.c 7zDec.c 7zFile.c
    7zStream.c Aes.c AesOpt.c Alloc.c Bcj2.c Bcj2Enc.c Bra.c Bra86.c BraIA64.c
    CpuArch.c Delta.c DllSecur.c LzFind.c Lzma2Dec.c Lzma2Enc.c Lzma86Dec.c
    Lzma86Enc.c LzmaDec.c LzmaEnc.c LzmaLib.c Ppmd7.c Ppmd7Dec.c Ppmd7Enc.c
    Sha256.c Sort.c Xz.c XzCrc64.c XzCrc64Opt.c XzDec.c XzEnc.c XzIn.c
)
list(TRANSFORM _lzma_sources PREPEND "${_lzma}/")

add_library(sourcesdk_lzma STATIC EXCLUDE_FROM_ALL ${_lzma_sources})
add_library(sourcesdk::lzma ALIAS sourcesdk_lzma)

target_include_directories(sourcesdk_lzma SYSTEM PUBLIC "${_lzma}")
target_compile_definitions(sourcesdk_lzma PRIVATE _7ZIP_ST)
set_target_properties(sourcesdk_lzma PROPERTIES FOLDER "sourcesdk")

if(WIN32)
    target_sources(sourcesdk_lzma PRIVATE
        "${_lzma}/LzFindMt.c" "${_lzma}/Lzma2DecMt.c" "${_lzma}/MtCoder.c"
        "${_lzma}/MtDec.c" "${_lzma}/Threads.c")
endif()

# ---------------------------------------------------------------------------
# sourcesdk::common / sourcesdk::tier0 / sourcesdk::tier1
# The 32-bit and 64-bit SDK revisions differ in layout, link libraries and
# required defines, so each architecture gets the configuration its SDK
# revision expects.
# ---------------------------------------------------------------------------
add_library(sourcesdk_common INTERFACE EXCLUDE_FROM_ALL)
add_library(sourcesdk::common ALIAS sourcesdk_common)

add_library(sourcesdk_tier0 INTERFACE EXCLUDE_FROM_ALL)
add_library(sourcesdk::tier0 ALIAS sourcesdk_tier0)

target_compile_definitions(sourcesdk_common INTERFACE
    RAD_TELEMETRY_DISABLED
    GMOD_USE_SOURCESDK
)

if(CLIENT_DLL)
    target_compile_definitions(sourcesdk_common INTERFACE CLIENT_DLL)
    target_include_directories(sourcesdk_common SYSTEM INTERFACE "${_sdk}/game/client")
else()
    target_compile_definitions(sourcesdk_common INTERFACE GAME_DLL)
    target_include_directories(sourcesdk_common SYSTEM INTERFACE "${_sdk}/game/server")
endif()

target_include_directories(sourcesdk_common SYSTEM INTERFACE
    "${_sdk}/common"
    "${_sdk}/game/shared"
    "${_sdk}/public"
)

target_include_directories(sourcesdk_tier0 SYSTEM INTERFACE "${_sdk}/public/tier0")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # 64-bit: x86-64-branch SDK revision.
    add_library(sourcesdk_internal INTERFACE EXCLUDE_FROM_ALL)
    add_library(sourcesdk::internal ALIAS sourcesdk_internal)

    target_compile_definitions(sourcesdk_internal INTERFACE
        RAD_TELEMETRY_DISABLED
        NO_STRING_T
        VECTOR
        VERSION_SAFE_STEAM_API_INTERFACES
        PROTECTED_THINGS_ENABLE
        PLATFORM_64BITS
    )

    if(WIN32)
        target_compile_definitions(sourcesdk_internal INTERFACE
            _DLL_EXT=.dll WIN32 COMPILER_MSVC COMPILER_MSVC64 WIN64 _WIN64)
        target_link_options(sourcesdk_internal INTERFACE
            "$<$<CONFIG:Debug>:/NODEFAULTLIB:libcmt.lib>")
        target_link_directories(sourcesdk_internal INTERFACE "${_sdk}/lib/public/x64")
    elseif(APPLE)
        target_compile_definitions(sourcesdk_internal INTERFACE
            _DLL_EXT=.dylib COMPILER_GCC POSIX _POSIX OSX _OSX GNUC
            _DARWIN_UNLIMITED_SELECT FD_SETSIZE=10240 OVERRIDE_V_DEFINES SWDS)
        target_link_directories(sourcesdk_internal INTERFACE "${_sdk}/lib/public/osx64")
    else()
        target_compile_definitions(sourcesdk_internal INTERFACE
            _DLL_EXT=.so COMPILER_GCC POSIX _POSIX LINUX _LINUX GNUC SWDS)
        target_link_directories(sourcesdk_internal INTERFACE "${_sdk}/lib/public/linux64")
    endif()

    target_link_libraries(sourcesdk_common INTERFACE sourcesdk::internal)
    target_link_libraries(sourcesdk_tier0 INTERFACE tier0)

    add_library(sourcesdk_interfaces STATIC EXCLUDE_FROM_ALL
        "${_sdk}/interfaces/interfaces.cpp")
    add_library(sourcesdk::interfaces ALIAS sourcesdk_interfaces)
    target_link_libraries(sourcesdk_interfaces PUBLIC sourcesdk::common)
    target_include_directories(sourcesdk_interfaces PRIVATE "${_sdk}/public/interfaces")
    set_target_properties(sourcesdk_interfaces PROPERTIES FOLDER "sourcesdk")

    set(_tier1_sources
        appinstance.cpp bitbuf.cpp newbitbuf.cpp byteswap.cpp characterset.cpp
        checksum_crc.cpp checksum_md5.cpp checksum_sha1.cpp circularbuffer.cpp
        commandbuffer.cpp convar.cpp datamanager.cpp diff.cpp exprevaluator.cpp
        generichash.cpp interface.cpp keyvalues.cpp keyvaluesjson.cpp
        kvpacker.cpp lzmaDecoder.cpp lzss.cpp mempool.cpp memstack.cpp
        NetAdr.cpp splitstring.cpp rangecheckedvar.cpp stringpool.cpp
        strtools.cpp strtools_unicode.cpp tier1.cpp tier1_logging.cpp
        timeutils.cpp uniqueid.cpp utlbuffer.cpp utlbufferutil.cpp
        utlsoacontainer.cpp utlstring.cpp utlsymbol.cpp miniprofiler_hash.cpp
        sparsematrix.cpp memoverride_dummy.cpp
    )
else()
    # 32-bit: garrysmod_common's master sourcesdk-minimal pin.
    if(WIN32)
        target_compile_definitions(sourcesdk_common INTERFACE WIN32)
        target_link_options(sourcesdk_common INTERFACE
            "$<$<CONFIG:Debug>:/NODEFAULTLIB:libcmt.lib>")
        target_link_directories(sourcesdk_common INTERFACE "${_sdk}/lib/public")
        target_link_libraries(sourcesdk_tier0 INTERFACE tier0)
    elseif(APPLE)
        target_compile_definitions(sourcesdk_common INTERFACE
            COMPILER_GCC POSIX _POSIX OSX GNUC NO_MALLOC_OVERRIDE)
        target_link_directories(sourcesdk_common INTERFACE "${_sdk}/lib/public/osx32")
        target_link_libraries(sourcesdk_tier0 INTERFACE tier0)
    else()
        target_compile_definitions(sourcesdk_common INTERFACE
            COMPILER_GCC POSIX _POSIX LINUX _LINUX GNUC NO_MALLOC_OVERRIDE)
        target_link_directories(sourcesdk_common INTERFACE "${_sdk}/lib/public/linux32")
        target_link_libraries(sourcesdk_tier0 INTERFACE tier0_srv) # TODO: Make client side support
    endif()

    set(_tier1_sources
        bitbuf.cpp byteswap.cpp characterset.cpp checksum_crc.cpp
        checksum_md5.cpp checksum_sha1.cpp commandbuffer.cpp convar.cpp
        datamanager.cpp diff.cpp generichash.cpp ilocalize.cpp interface.cpp
        keyvalues.cpp kvpacker.cpp lzmaDecoder.cpp mempool.cpp memstack.cpp
        NetAdr.cpp splitstring.cpp rangecheckedvar.cpp reliabletimer.cpp
        stringpool.cpp strtools.cpp strtools_unicode.cpp tier1.cpp
        tokenreader.cpp sparsematrix.cpp uniqueid.cpp utlbuffer.cpp
        utlbufferutil.cpp utlstring.cpp utlsymbol.cpp utlbinaryblock.cpp
        snappy.cpp snappy-sinksource.cpp snappy-stubs-internal.cpp
    )
endif()

list(TRANSFORM _tier1_sources PREPEND "${_sdk}/tier1/")

add_library(sourcesdk_tier1 STATIC EXCLUDE_FROM_ALL ${_tier1_sources})
add_library(sourcesdk::tier1 ALIAS sourcesdk_tier1)

target_include_directories(sourcesdk_tier1 SYSTEM PUBLIC "${_sdk}/public/tier1")
target_link_libraries(sourcesdk_tier1 PUBLIC sourcesdk::common sourcesdk::tier0)
target_link_libraries(sourcesdk_tier1 PRIVATE sourcesdk::lzma)
target_compile_definitions(sourcesdk_tier1 PRIVATE TIER1_STATIC_LIB)
set_target_properties(sourcesdk_tier1 PROPERTIES FOLDER "sourcesdk")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_include_directories(sourcesdk_tier1 SYSTEM PUBLIC "${_sdk}/common/xbox")
    target_link_libraries(sourcesdk_tier1 PUBLIC sourcesdk::interfaces)
else()
    target_compile_definitions(sourcesdk_tier1 PRIVATE RAD_TELEMETRY_DISABLED)
endif()

if(WIN32)
    target_compile_definitions(sourcesdk_tier1 PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_compile_definitions(sourcesdk_tier1 PRIVATE _DLL_EXT=.dll)
    endif()
    target_sources(sourcesdk_tier1 PRIVATE "${_sdk}/tier1/processor_detect.cpp")
    target_link_libraries(sourcesdk_tier1 INTERFACE vstdlib ws2_32 rpcrt4)
elseif(APPLE)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_compile_definitions(sourcesdk_tier1 PRIVATE _DLL_EXT=.dylib)
    endif()
    target_sources(sourcesdk_tier1 PRIVATE "${_sdk}/tier1/processor_detect_linux.cpp")
    target_link_libraries(sourcesdk_tier1 INTERFACE vstdlib iconv)
else()
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_compile_definitions(sourcesdk_tier1 PRIVATE _DLL_EXT=.so)
    endif()
    target_sources(sourcesdk_tier1 PRIVATE
        "${_sdk}/tier1/processor_detect_linux.cpp"
        "${_sdk}/tier1/qsort_s.cpp"
        "${_sdk}/tier1/pathmatch.cpp")
    target_link_options(sourcesdk_tier1 PRIVATE
        "-Xlinker" "--wrap=fopen"
        "-Xlinker" "--wrap=freopen"
        "-Xlinker" "--wrap=open"
        "-Xlinker" "--wrap=creat"
        "-Xlinker" "--wrap=access"
        "-Xlinker" "--wrap=__xstat"
        "-Xlinker" "--wrap=stat"
        "-Xlinker" "--wrap=lstat"
        "-Xlinker" "--wrap=fopen64"
        "-Xlinker" "--wrap=open64"
        "-Xlinker" "--wrap=opendir"
        "-Xlinker" "--wrap=__lxstat"
        "-Xlinker" "--wrap=chmod"
        "-Xlinker" "--wrap=chown"
        "-Xlinker" "--wrap=lchown"
        "-Xlinker" "--wrap=symlink"
        "-Xlinker" "--wrap=link"
        "-Xlinker" "--wrap=__lxstat64"
        "-Xlinker" "--wrap=mknod"
        "-Xlinker" "--wrap=utimes"
        "-Xlinker" "--wrap=unlink"
        "-Xlinker" "--wrap=rename"
        "-Xlinker" "--wrap=utime"
        "-Xlinker" "--wrap=__xstat64"
        "-Xlinker" "--wrap=mount"
        "-Xlinker" "--wrap=mkfifo"
        "-Xlinker" "--wrap=mkdir"
        "-Xlinker" "--wrap=rmdir"
        "-Xlinker" "--wrap=scandir"
        "-Xlinker" "--wrap=realpath")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_link_libraries(sourcesdk_tier1 INTERFACE vstdlib) # TODO: Make clientside support
    else()
        target_link_libraries(sourcesdk_tier1 INTERFACE vstdlib_srv) # TODO: Make clientside support
    endif()
endif()

set(GarrysmodCommon_FOUND TRUE)
