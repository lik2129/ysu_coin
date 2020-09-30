#pragma once

#include <ysu/boost/asio/ip/tcp.hpp>
#include <ysu/lib/logger_mt.hpp>
#include <ysu/lib/rpc_handler_interface.hpp>
#include <ysu/lib/rpcconfig.hpp>

namespace boost
{
namespace asio
{
	class io_context;
}
}

namespace ysu
{
class rpc_handler_interface;

class rpc
{
public:
	rpc (boost::asio::io_context & io_ctx_a, ysu::rpc_config const & config_a, ysu::rpc_handler_interface & rpc_handler_interface_a);
	virtual ~rpc ();
	void start ();
	virtual void accept ();
	void stop ();

	ysu::rpc_config config;
	boost::asio::ip::tcp::acceptor acceptor;
	ysu::logger_mt logger;
	boost::asio::io_context & io_ctx;
	ysu::rpc_handler_interface & rpc_handler_interface;
	bool stopped{ false };
};

/** Returns the correct RPC implementation based on TLS configuration */
std::unique_ptr<ysu::rpc> get_rpc (boost::asio::io_context & io_ctx_a, ysu::rpc_config const & config_a, ysu::rpc_handler_interface & rpc_handler_interface_a);
}
