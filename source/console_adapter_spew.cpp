#include "console_adapter_spew.hpp"

#if !__has_include(<tier0/logging.h>)

#include <GarrysMod/InterfacePointers.hpp>

#include <cdll_int.h>
#include <eiface.h>

SpewOutputFunc_t console_adapter::s_original_spew = nullptr;
std::atomic_bool console_adapter::s_hook_active = false;
std::mutex console_adapter::s_mutex;
std::condition_variable console_adapter::s_condition;
std::vector<json::value> console_adapter::s_messages;

console_adapter::console_adapter( ) :
	m_thread_stop( true )
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
	std::unique_lock<std::mutex> lock( s_mutex );

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

#if IS_SERVERSIDE
	if( m_engine_server == nullptr )
	{
		error_message = "engine server interface unavailable";
		return false;
	}

	m_engine_server->ServerCommand( command.c_str( ) );

#else
	if( m_engine_client == nullptr )
	{
		error_message = "engine client interface unavailable";
		return false;
	}

	m_engine_client->ClientCmd_Unrestricted( command.c_str( ) );

#endif

	return true;

}

void console_adapter::start( [[maybe_unused]] std::unique_lock<std::mutex> &lock )
{
	if( !m_thread_stop )
		return;

	if( s_hook_active )
		return;

#if defined( WIN64 )
	// Tier0 spew exports are not available for x64 in our current SDK linkage,
	// so log interception is disabled on this platform.
	return;
#endif

	s_original_spew = GetSpewOutputFunc( );
	SpewOutputFunc( &console_adapter::Log );

	s_hook_active = true;
	m_thread_stop = false;
	m_thread = std::thread( &console_adapter::queue_dispatcher, this );
}

void console_adapter::stop( std::unique_lock<std::mutex> &lock )
{
	if( m_thread_stop )
		return;

#if defined( WIN64 )
	return;
#endif

	s_hook_active = false;
	if( s_original_spew )
		SpewOutputFunc( s_original_spew );
	s_original_spew = nullptr;

	s_messages.clear( );

	m_thread_stop = true;
	lock.unlock( );
	s_condition.notify_all( );
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
			std::unique_lock<std::mutex> lock( s_mutex );
			s_condition.wait( lock, [this] { return !s_messages.empty( ) || m_thread_stop; } );
			if( m_thread_stop && s_messages.empty( ) )
				break;

			std::swap( messages, s_messages );
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
				// Keep the dispatch thread alive even if callback throws.
			}
		}
	}
}

SpewRetval_t console_adapter::Log( SpewType_t spewType, const tchar *pMsg )
{
	SpewOutputFunc_t original = nullptr;
	std::unique_lock<std::mutex> lock( s_mutex );
	original = s_original_spew;
	if( !s_hook_active )
		return original ? original( spewType, pMsg ) : SPEW_CONTINUE;

	if( pMsg == nullptr )
		return original ? original( spewType, "" ) : SPEW_CONTINUE;

#if defined( WIN64 )
	return original ? original( spewType, pMsg ) : SPEW_CONTINUE;
#endif

	const Color *output_color = GetSpewOutputColor( );
	const Color color = output_color ? *output_color : Color( 255, 255, 255, 255 );

	json::object jcolor;
	jcolor["r"] = json::value( static_cast<double>( color.r( ) ) );
	jcolor["g"] = json::value( static_cast<double>( color.g( ) ) );
	jcolor["b"] = json::value( static_cast<double>( color.b( ) ) );
	jcolor["a"] = json::value( static_cast<double>( color.a( ) ) );

	json::object param;
	param["channel_id"] = json::value( static_cast<double>( spewType ) );
	param["group"] = json::value( GetSpewOutputGroup( ) );
	param["severity"] = json::value( static_cast<double>( GetSpewOutputLevel( ) ) );
	param["color"] = json::value( jcolor );
	param["message"] = json::value( pMsg );

	s_messages.emplace_back( std::move( param ) );

	lock.unlock( );
	s_condition.notify_all( );

	return original ? original( spewType, pMsg ) : SPEW_CONTINUE;
}

#endif
