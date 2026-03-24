#include <GarrysMod/Lua/Interface.h>

#include "basic_server.hpp"

#include <lrdb/command_stream/socket.hpp>

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
	// Pointer to the inner slot inside the Lua userdata. Kept in sync so that
	// Deinitialize can null it before GC runs destruct, preventing a double-free.
	static lrdb_server **active_server_ptr = nullptr;

	LUA_FUNCTION_STATIC( activate )
	{
		lrdb_server **server = LUA->GetUserType<lrdb_server *>( lua_upvalueindex( 1 ), metatype );
		if( *server != nullptr )
		{
			delete *server;
			*server = nullptr;
		}

		if( LUA->IsType( 1, GarrysMod::Lua::Type::Number ) )
			*server = new lrdb_server( static_cast<int16_t>( LUA->GetNumber( 1 ) ) );
		else
			*server = new lrdb_server( default_port );

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
