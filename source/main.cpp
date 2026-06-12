#include <GarrysMod/Lua/Interface.h>

#include "basic_server.hpp"

#include <lrdb/command_stream/socket.hpp>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace rdb
{
	// Module identity — different for server (rdb) and client (rdb_client) builds
#ifdef GMOD_CLIENT_MODULE
	static const char* kGlobalName = "rdb_client";
	static const int16_t default_port = 21112;
#else
	static const char* kGlobalName = "rdb";
	static const int16_t default_port = 21111;
#endif

	typedef basic_server<lrdb::command_stream_socket> lrdb_server;
	static int32_t metatype = GarrysMod::Lua::Type::None;
	static lrdb_server *active_server = nullptr;
	struct RuntimeOptions
	{
		bool allow_remote_connections = false;
		bool pause_on_activate = false;
		int pause_timeout_seconds = 0;
	};
	static RuntimeOptions runtime_options;
	static const char* kAllowRemoteFlag = "-rdb_allow_remote";
	static const char* kPauseOnActivateFlag = "-rdb_pause_on_activate";
	// Pointer to the inner slot inside the Lua userdata. Kept in sync so that
	// Deinitialize can null it before GC runs destruct, preventing a double-free.
	static lrdb_server **active_server_ptr = nullptr;

	static bool IsCommandLineBoundary( char ch )
	{
		return ch == '\0' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
	}

	static bool TryReadCommandLine( std::string& command_line )
	{
		command_line.clear( );

#if defined(_WIN32)
		const char* raw_command_line = GetCommandLineA( );
		if( raw_command_line == nullptr )
			return false;
		command_line = raw_command_line;
#else
		std::ifstream command_line_file( "/proc/self/cmdline", std::ios::binary );
		if( !command_line_file )
			return false;
		command_line.assign(
			std::istreambuf_iterator<char>( command_line_file ),
			std::istreambuf_iterator<char>( )
		);
		for( char& ch : command_line )
		{
			if( ch == '\0' )
				ch = ' ';
		}
#endif

		return true;
	}

	static bool HasLaunchFlag( const std::string& command_line, const char* flag )
	{
		if( flag == nullptr || flag[0] == '\0' )
			return false;

		const std::string flag_text( flag );
		size_t pos = 0;
		while( ( pos = command_line.find( flag_text, pos ) ) != std::string::npos )
		{
			const char before = pos == 0 ? ' ' : command_line[pos - 1];
			const size_t after_index = pos + flag_text.size( );
			const char after = after_index >= command_line.size( ) ? '\0' : command_line[after_index];
			if( IsCommandLineBoundary( before ) && IsCommandLineBoundary( after ) )
				return true;
			pos = after_index;
		}

		return false;
	}

	static bool TryParseNonNegativeInt( const std::string& text, int& value )
	{
		if( text.empty( ) )
			return false;

		errno = 0;
		char* end_ptr = nullptr;
		const long parsed = std::strtol( text.c_str( ), &end_ptr, 10 );
		if( errno != 0 || end_ptr == text.c_str( ) || *end_ptr != '\0' )
			return false;
		if( parsed < 0 || parsed > std::numeric_limits<int>::max( ) )
			return false;

		value = static_cast<int>( parsed );
		return true;
	}

	static bool TryGetLaunchFlagValue( const std::string& command_line, const char* flag, int& value )
	{
		if( flag == nullptr || flag[0] == '\0' )
			return false;

		const std::string flag_text( flag );
		size_t pos = 0;
		while( ( pos = command_line.find( flag_text, pos ) ) != std::string::npos )
		{
			const char before = pos == 0 ? ' ' : command_line[pos - 1];
			const size_t after_index = pos + flag_text.size( );
			const char after = after_index >= command_line.size( ) ? '\0' : command_line[after_index];
			if( IsCommandLineBoundary( before ) && IsCommandLineBoundary( after ) )
			{
				size_t value_start = after_index;
				while( value_start < command_line.size( ) && IsCommandLineBoundary( command_line[value_start] ) )
					++value_start;
				if( value_start >= command_line.size( ) || command_line[value_start] == '-' )
					return false;

				size_t value_end = value_start;
				while( value_end < command_line.size( ) && !IsCommandLineBoundary( command_line[value_end] ) )
					++value_end;

				return TryParseNonNegativeInt(
					command_line.substr( value_start, value_end - value_start ),
					value
				);
			}
			pos = after_index;
		}

		return false;
	}

	// The client engine unloads binary modules on disconnect while tier0 can still hold our logging listener - pin the image so engine-held pointers stay valid
	static bool PinModuleInMemory( )
	{
#if defined(_WIN32)
		HMODULE handle = NULL;
		return GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>( &PinModuleInMemory ), &handle ) != 0;
#else
		Dl_info info;
		if( dladdr( reinterpret_cast<void *>( &PinModuleInMemory ), &info ) == 0 || info.dli_fname == nullptr )
		{
			return false;
		}

		return dlopen( info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE ) != nullptr;
#endif
	}

	static void RefreshRuntimeOptions( )
	{
		runtime_options = RuntimeOptions{ };

		std::string command_line;
		if( !TryReadCommandLine( command_line ) )
			return;

		runtime_options.allow_remote_connections = HasLaunchFlag( command_line, kAllowRemoteFlag );
		runtime_options.pause_on_activate = HasLaunchFlag( command_line, kPauseOnActivateFlag );
		if( runtime_options.pause_on_activate )
		{
			runtime_options.pause_timeout_seconds = 60;
			int parsed_timeout_seconds = 0;
			if( TryGetLaunchFlagValue( command_line, kPauseOnActivateFlag, parsed_timeout_seconds ) )
				runtime_options.pause_timeout_seconds = parsed_timeout_seconds;
		}
	}

	static void PrintStatusLine( GarrysMod::Lua::ILuaBase* LUA, const std::string& text )
	{
		LUA->GetField( GarrysMod::Lua::INDEX_GLOBAL, "Msg" );
		if( !LUA->IsType( -1, GarrysMod::Lua::Type::Function ) )
		{
			LUA->Pop( 1 );
			return;
		}
		LUA->PushString( text.c_str( ) );
		LUA->Call( 1, 0 );
	}

	LUA_FUNCTION_STATIC( activate )
	{
		lrdb_server **server = LUA->GetUserType<lrdb_server *>( lua_upvalueindex( 1 ), metatype );
		if( *server != nullptr )
		{
			delete *server;
			*server = nullptr;
		}

		const int16_t port = LUA->IsType( 1, GarrysMod::Lua::Type::Number )
			? static_cast<int16_t>( LUA->GetNumber( 1 ) )
			: default_port;
		const bool loopback_only = !runtime_options.allow_remote_connections;

		try
		{
			*server = new lrdb_server( port, loopback_only );
			( *server )->set_pause_timeout( runtime_options.pause_timeout_seconds );
			( *server )->set_pause_on_activate( runtime_options.pause_on_activate );
		}
		catch( const std::exception& ex )
		{
			if( *server != nullptr )
			{
				delete *server;
				*server = nullptr;
			}
			active_server = nullptr;
			active_server_ptr = nullptr;
			PrintStatusLine( LUA, std::string( "[GLuaLS] Failed to activate " ) + kGlobalName + ": " + ex.what( ) + "\n" );
			return 0;
		}
		catch( ... )
		{
			if( *server != nullptr )
			{
				delete *server;
				*server = nullptr;
			}
			active_server = nullptr;
			active_server_ptr = nullptr;
			PrintStatusLine( LUA, std::string( "[GLuaLS] Failed to activate " ) + kGlobalName + ": unknown error\n" );
			return 0;
		}

		active_server = *server;
		active_server_ptr = server;

		( *server )->reset( LUA->GetState( ), LUA );
		return 0;
	}

	LUA_FUNCTION_STATIC( deactivate )
	{
		lrdb_server **server = LUA->GetUserType<lrdb_server *>( lua_upvalueindex( 1 ), metatype );
		if( *server != nullptr )
		{
			delete *server;
			*server = nullptr;
		}
		active_server = nullptr;
		active_server_ptr = nullptr;

		return 0;
	}

	LUA_FUNCTION_STATIC( destruct )
	{
		lrdb_server **server = LUA->GetUserType<lrdb_server *>( 1, metatype );
		if( *server != nullptr )
		{
			delete *server;
			*server = nullptr;
		}
		active_server = nullptr;
		active_server_ptr = nullptr;

		return 0;
	}

	static int32_t Initialize( GarrysMod::Lua::ILuaBase *LUA )
	{
		const bool module_pinned = PinModuleInMemory( );
		RefreshRuntimeOptions( );

		metatype = LUA->CreateMetaTable( kGlobalName );

		LUA->PushCFunction( destruct );
		LUA->SetField( -2, "__gc" );

		lrdb_server **server = LUA->NewUserType<lrdb_server *>( metatype );
		*server = nullptr;
		active_server_ptr = server;

		LUA->Push( -2 ); // push metatable to the stack top
		LUA->SetMetaTable( -2 ); // pop reference on stack top and set it as metatable of userdata
		LUA->Remove( -2 ); // remove older metatable reference on stack

		LUA->CreateTable( );

		LUA->PushString( "rdb " GM_RDB_VERSION );
		LUA->SetField( -2, "Version" );

		// version num follows LuaJIT style, xxyyzz
		LUA->PushNumber( static_cast<double>( GM_RDB_VERSION_NUM ) );
		LUA->SetField( -2, "VersionNum" );

		LUA->Push( -2 ); // push userdata to stack stop
		LUA->PushCClosure( activate, 1 );
		LUA->SetField( -2, "activate" );

		LUA->Push( -2 ); // push userdata to stack stop
		LUA->PushCClosure( deactivate, 1 );
		LUA->SetField( -2, "deactivate" );

		LUA->Push( -1 );
		LUA->SetField( GarrysMod::Lua::INDEX_GLOBAL, kGlobalName );

		PrintStatusLine( LUA, std::string( "[GLuaLS] DEBUG READY: " ) + kGlobalName + " module loaded and enabled.\n" );

		if( !module_pinned )
		{
			PrintStatusLine( LUA, std::string( "[GLuaLS] WARN: " ) + kGlobalName + " could not pin its module image in memory; the engine unloading it later may crash.\n" );
		}

		if( runtime_options.allow_remote_connections )
			PrintStatusLine( LUA, std::string( "[GLuaLS] " ) + kGlobalName + " network mode: remote connections enabled via " + kAllowRemoteFlag + ".\n" );
		else
			PrintStatusLine( LUA, std::string( "[GLuaLS] " ) + kGlobalName + " network mode: localhost-only (default). Use " + kAllowRemoteFlag + " to allow remote debugger connections.\n" );

		if( runtime_options.pause_on_activate )
		{
			const std::string pause_timer_text = runtime_options.pause_timeout_seconds == 0
				? "infinite wait"
				: std::to_string( runtime_options.pause_timeout_seconds ) + "s countdown";
			PrintStatusLine( LUA, std::string( "[GLuaLS] " ) + kGlobalName + " activation mode: pause-on-activate enabled via " + kPauseOnActivateFlag + " with " + pause_timer_text + " (updates every second; resumes on connect unless stopped on entry).\n" );
		}
		else
			PrintStatusLine( LUA, std::string( "[GLuaLS] " ) + kGlobalName + " activation mode: continue running on activate (default). Use " + kPauseOnActivateFlag + " [seconds] to pause on activate.\n" );

		return 1;
	}

	static int32_t Deinitialize( GarrysMod::Lua::ILuaBase *LUA )
	{
		if( active_server != nullptr )
		{
			// Null the userdata's inner pointer BEFORE deleting, so that if the
			// Lua GC fires destruct (e.g. triggered by SetField below), it sees
			// nullptr and does not attempt a second delete (double-free).
			if( active_server_ptr != nullptr )
			{
				*active_server_ptr = nullptr;
				active_server_ptr = nullptr;
			}
			delete active_server;
			active_server = nullptr;
		}

		LUA->PushNil( );
		LUA->SetField( GarrysMod::Lua::INDEX_GLOBAL, kGlobalName );
		return 0;
	}
}

GMOD_MODULE_OPEN( )
{
	return rdb::Initialize( LUA );
}

GMOD_MODULE_CLOSE( )
{
	return rdb::Deinitialize( LUA );
}
