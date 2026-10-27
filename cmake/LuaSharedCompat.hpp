#pragma once

// Minimal compatibility shim force-included into garrysmod_lua_shared on 64-bit
// builds so third-party/garrysmod_common remains untouched while supporting both
// Garry's Mod's main branch (garrysmod/bin/win64/lua_shared.dll) and the x86-64
// beta branch (bin/win64/lua_shared.dll).

#if defined( _WIN32 ) && defined( _WIN64 )

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

static inline BOOL GmRdbGetLuaSharedModuleHandleEx( DWORD flags, LPCSTR name, HMODULE *module )
{
	if( GetModuleHandleExA( flags, name, module ) )
		return TRUE;

	if( GetModuleHandleExA( flags, "garrysmod/bin/win64/lua_shared.dll", module ) )
		return TRUE;

	return GetModuleHandleExA( flags, "lua_shared.dll", module );
}

#undef GetModuleHandleEx
#define GetModuleHandleEx GmRdbGetLuaSharedModuleHandleEx

#elif defined( __linux__ ) && defined( __x86_64__ )

#include <dlfcn.h>

static inline void *GmRdbDlopenLuaShared( const char *filename, int flags )
{
	if( void *handle = ::dlopen( filename, flags ) )
		return handle;

	static const char *const kFallbacks[] = {
		"garrysmod/bin/linux64/lua_shared_srv.so",
		"garrysmod/bin/linux64/lua_shared.so",
		"garrysmod/bin/lua_shared_srv.so",
		"garrysmod/bin/lua_shared.so",
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
