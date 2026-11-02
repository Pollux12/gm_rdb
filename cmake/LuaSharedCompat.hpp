#pragma once

// Adds main-branch lua_shared paths after garrysmod_common's final 64-bit probe fails.

#if defined( _WIN32 ) && defined( _WIN64 )

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <cstring>

static inline BOOL GmRdbGetLuaSharedModuleHandleEx( DWORD flags, LPCSTR name, HMODULE *module )
{
	if( GetModuleHandleExA( flags, name, module ) )
		return TRUE;

	if( std::strcmp( name, "bin/win64/lua_shared.dll" ) != 0 )
		return FALSE;

	return GetModuleHandleExA( flags, "garrysmod/bin/win64/lua_shared.dll", module ) ||
		GetModuleHandleExA( flags, "lua_shared.dll", module );
}

#undef GetModuleHandleEx
#define GetModuleHandleEx GmRdbGetLuaSharedModuleHandleEx

#elif defined( __linux__ ) && defined( __x86_64__ )

#include <dlfcn.h>
#include <cstring>

static inline void *GmRdbDlopenLuaShared( const char *filename, int flags )
{
	if( void *handle = ::dlopen( filename, flags ) )
		return handle;

	if( filename == nullptr || std::strcmp( filename, "bin/linux64/lua_shared_client.so" ) != 0 )
		return nullptr;

	static const char *const kFallbacks[] = {
		"garrysmod/bin/linux64/lua_shared_srv.so",
		"garrysmod/bin/linux64/lua_shared.so",
	};

	for( const char *candidate : kFallbacks )
	{
		if( void *handle = ::dlopen( candidate, flags ) )
			return handle;
	}

	return nullptr;
}

#define dlopen GmRdbDlopenLuaShared

#endif
