#include "console_adapter_logging.hpp"

#if __has_include(<tier0/logging.h>)

#include <GarrysMod/InterfacePointers.hpp>

#include <cdll_int.h>
#include <eiface.h>

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
	std::unique_lock<std::mutex> lock( m_mutex );

	m_callback = callback;

	if( callback )
		start( lock );
	else
		stop( lock );
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
	LoggingSystem_RegisterLoggingListener( this );

	m_thread = std::thread( &console_adapter::queue_dispatcher, this );
}

void console_adapter::stop( std::unique_lock<std::mutex> &lock )
{
	if( m_thread_stop )
		return;

	m_thread_stop = true;
	m_accept_logs = false;
	LoggingSystem_UnregisterLoggingListener( this );

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

void console_adapter::Log( const LoggingContext_t *pContext, const tchar *pMessage )
{
	if( m_thread_stop || !m_accept_logs )
		return;

	if( pContext == nullptr || pMessage == nullptr )
		return;

	std::unique_lock<std::mutex> lock( m_mutex );
	if( pContext->m_Flags & LCF_DO_NOT_ECHO )
		return;

	Color color = pContext->m_Color;
	if( color == UNSPECIFIED_LOGGING_COLOR )
		color = Color( 255, 255, 255, 255 );

	json::object jcolor;
	jcolor["r"] = json::value( static_cast<double>( color.r( ) ) );
	jcolor["g"] = json::value( static_cast<double>( color.g( ) ) );
	jcolor["b"] = json::value( static_cast<double>( color.b( ) ) );
	jcolor["a"] = json::value( static_cast<double>( color.a( ) ) );

	json::object param;
	param["channel_id"] = json::value( static_cast<double>( pContext->m_ChannelID ) );
	param["severity"] = json::value( static_cast<double>( pContext->m_Severity ) );
	param["color"] = json::value( jcolor );
	param["message"] = json::value( pMessage );

	m_messages.emplace_back( std::move( param ) );

	lock.unlock( );
	m_condition.notify_all( );
}

#endif
