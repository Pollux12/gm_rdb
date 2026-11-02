#include "console_adapter_logging.hpp"

#if __has_include(<tier0/logging.h>)

#include <GarrysMod/InterfacePointers.hpp>
#include <GarrysMod/ModuleLoader.hpp>

#include <cdll_int.h>
#include <eiface.h>

namespace
{

enum SpewRetvalCompat_t
{
	SPEW_COMPAT_DEBUGGER = 0,
	SPEW_COMPAT_CONTINUE,
	SPEW_COMPAT_ABORT
};

using RegisterLoggingListenerFn = void ( * )( ILoggingListener * );
using SpewOutputFuncCompat_t = SpewRetvalCompat_t ( * )( int spewType, const tchar *pMsg );
using SpewOutputSetterFn = void ( * )( SpewOutputFuncCompat_t func );
using GetSpewOutputFuncFn = SpewOutputFuncCompat_t ( * )( );
using GetSpewOutputGroupFn = const tchar *( * )( );
using GetSpewOutputLevelFn = int ( * )( );
using GetSpewOutputColorFn = const Color *( * )( );

} // namespace

// Immortal: tier0 keeps the listener (LoggingSystem) or spew hook (classic Spew API) pointer for the process lifetime.
class logging_listener_proxy final : public ILoggingListener
{
public:
	void attach( console_adapter *adapter )
	{
		std::lock_guard<std::mutex> lock( m_forward_mutex );

		if( !m_registered )
			m_registered = register_with_tier0( );

		m_target = adapter;
	}

	void detach( console_adapter *adapter )
	{
		// Taking the mutex drains in-flight Log forwards before teardown can free the adapter
		std::lock_guard<std::mutex> lock( m_forward_mutex );

		if( m_target == adapter )
		{
			m_target = nullptr;
		}
	}

	void Log( const LoggingContext_t *pContext, const tchar *pMessage ) override
	{
		std::lock_guard<std::mutex> lock( m_forward_mutex );

		if( m_target != nullptr )
		{
			m_target->handle_log( pContext, pMessage );
		}
	}

private:
	bool register_with_tier0( )
	{
		SourceSDK::ModuleLoader loader( "tier0" );
		if( !loader.IsValid( ) )
			return false;

		auto register_logging = reinterpret_cast<RegisterLoggingListenerFn>(
			loader.GetSymbol( "LoggingSystem_RegisterLoggingListener" ) );
		if( register_logging != nullptr )
		{
			register_logging( this );
			return true;
		}

		auto set_spew = reinterpret_cast<SpewOutputSetterFn>(
			loader.GetSymbol( "SpewOutputFunc" ) );
		auto get_spew = reinterpret_cast<GetSpewOutputFuncFn>(
			loader.GetSymbol( "GetSpewOutputFunc" ) );
		if( set_spew == nullptr || get_spew == nullptr )
			return false;

		m_get_spew_group = reinterpret_cast<GetSpewOutputGroupFn>(
			loader.GetSymbol( "GetSpewOutputGroup" ) );
		m_get_spew_level = reinterpret_cast<GetSpewOutputLevelFn>(
			loader.GetSymbol( "GetSpewOutputLevel" ) );
		m_get_spew_color = reinterpret_cast<GetSpewOutputColorFn>(
			loader.GetSymbol( "GetSpewOutputColor" ) );
		m_original_spew = get_spew( );
		set_spew( &logging_listener_proxy::SpewCallback );
		return true;
	}

	static SpewRetvalCompat_t SpewCallback( int spewType, const tchar *pMsg );

	SpewRetvalCompat_t handle_spew_callback( int spewType, const tchar *pMsg )
	{
		SpewOutputFuncCompat_t original = m_original_spew;
		{
			std::lock_guard<std::mutex> lock( m_forward_mutex );
			if( m_target != nullptr )
			{
				const tchar *group = m_get_spew_group ? m_get_spew_group( ) : nullptr;
				const int level = m_get_spew_level ? m_get_spew_level( ) : 0;
				const Color *color = m_get_spew_color ? m_get_spew_color( ) : nullptr;
				m_target->handle_spew( spewType, group, level, color, pMsg );
			}
		}

		return original ? original( spewType, pMsg ? pMsg : "" ) : SPEW_COMPAT_CONTINUE;
	}

	std::mutex m_forward_mutex;
	console_adapter *m_target = nullptr;
	bool m_registered = false;
	SpewOutputFuncCompat_t m_original_spew = nullptr;
	GetSpewOutputGroupFn m_get_spew_group = nullptr;
	GetSpewOutputLevelFn m_get_spew_level = nullptr;
	GetSpewOutputColorFn m_get_spew_color = nullptr;
};

static logging_listener_proxy &logging_proxy( )
{
	// Leaked on purpose - must outlive any logging state that references it
	static logging_listener_proxy *proxy = new logging_listener_proxy( );
	return *proxy;
}

SpewRetvalCompat_t logging_listener_proxy::SpewCallback( int spewType, const tchar *pMsg )
{
	return logging_proxy( ).handle_spew_callback( spewType, pMsg );
}

console_adapter::console_adapter( ) :
	m_thread_stop( true ),
	m_accept_logs( false )
{

#if IS_SERVERSIDE

	m_engine_server = InterfacePointers::VEngineServer( );

#else

	m_engine_client = InterfacePointers::VEngineClient( );

#endif

}

console_adapter::~console_adapter( )
{
	set_callback( nullptr );
}

void console_adapter::set_callback( const std::function<void( const json::value &message )> &callback )
{
	{
		std::unique_lock<std::mutex> lock( m_mutex );

		m_callback = callback;

		if( callback )
		{
			start( lock );
		}
		else
		{
			stop( lock );
		}
	}

	// Outside m_mutex: Log holds the proxy mutex then takes m_mutex, so the reverse order here would deadlock
	if( callback )
	{
		logging_proxy( ).attach( this );
	}
	else
	{
		logging_proxy( ).detach( this );
	}
}
bool console_adapter::run_command( const std::string &command, std::string &error_message )
{
	if( command.empty( ) )
	{
		error_message = "empty command";
		return false;
	}

	std::string command_text = command;
	if( command_text.back( ) != '\n' )
		command_text.push_back( '\n' );

#if IS_SERVERSIDE
	if( m_engine_server == nullptr )
		m_engine_server = InterfacePointers::VEngineServer( );

	if( m_engine_server == nullptr )
	{
		error_message = "engine server interface unavailable";
		return false;
	}

	m_engine_server->ServerCommand( command_text.c_str( ) );
	m_engine_server->ServerExecute( );

#else
	if( m_engine_client == nullptr )
		m_engine_client = InterfacePointers::VEngineClient( );

	if( m_engine_client == nullptr )
	{
		error_message = "engine client interface unavailable";
		return false;
	}

	m_engine_client->ClientCmd_Unrestricted( command_text.c_str( ) );

#endif

	return true;

}

void console_adapter::start( [[maybe_unused]] std::unique_lock<std::mutex> &lock )
{
	if( !m_thread_stop )
		return;

	m_accept_logs = true;
	m_thread_stop = false;

	m_thread = std::thread( &console_adapter::queue_dispatcher, this );
}

void console_adapter::stop( std::unique_lock<std::mutex> &lock )
{
	if( m_thread_stop )
		return;

	m_thread_stop = true;
	m_accept_logs = false;

	m_messages.clear( );

	lock.unlock( );
	m_condition.notify_all( );
	if( m_thread.joinable( ) )
		m_thread.join( );
}

void console_adapter::queue_dispatcher( )
{
	while( true )
	{
		std::vector<json::value> messages;
		std::function<void( const json::value &message )> callback;
		{
			std::unique_lock<std::mutex> lock( m_mutex );
			m_condition.wait( lock, [this] { return !m_messages.empty( ) || m_thread_stop; } );
			if( m_thread_stop && m_messages.empty( ) )
				break;

			std::swap( messages, m_messages );
			callback = m_callback;
		}

		if( !callback )
			continue;

		for( const json::value &message : messages )
		{
			try
			{
				callback( message );
			}
			catch( ... )
			{
				// Do not let callback exceptions kill the dispatch thread.
			}
		}
	}
}

void console_adapter::handle_log( const LoggingContext_t *pContext, const tchar *pMessage )
{
	if( pContext == nullptr || ( pContext->m_Flags & LCF_DO_NOT_ECHO ) )
		return;

	Color color = pContext->m_Color;
	if( color == UNSPECIFIED_LOGGING_COLOR )
		color = Color( 255, 255, 255, 255 );

	enqueue_message( pContext->m_ChannelID, nullptr, pContext->m_Severity, color, pMessage );
}

void console_adapter::handle_spew( int spew_type, const tchar *group, int level, const Color *color, const tchar *pMessage )
{
	enqueue_message( spew_type, group, level, color ? *color : Color( 255, 255, 255, 255 ), pMessage );
}

void console_adapter::enqueue_message( int channel_id, const tchar *group, int severity, const Color &color, const tchar *pMessage )
{
	if( m_thread_stop || !m_accept_logs )
		return;

	if( pMessage == nullptr )
		return;

	json::object jcolor;
	jcolor["r"] = json::value( static_cast<double>( color.r( ) ) );
	jcolor["g"] = json::value( static_cast<double>( color.g( ) ) );
	jcolor["b"] = json::value( static_cast<double>( color.b( ) ) );
	jcolor["a"] = json::value( static_cast<double>( color.a( ) ) );

	json::object param;
	param["channel_id"] = json::value( static_cast<double>( channel_id ) );
	if( group != nullptr )
		param["group"] = json::value( group );
	param["severity"] = json::value( static_cast<double>( severity ) );
	param["color"] = json::value( jcolor );
	param["message"] = json::value( pMessage );

	std::unique_lock<std::mutex> lock( m_mutex );
	m_messages.emplace_back( std::move( param ) );

	lock.unlock( );
	m_condition.notify_all( );
}

#endif
