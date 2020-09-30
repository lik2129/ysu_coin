#pragma once

#include <boost/property_tree/ptree.hpp>

#include <functional>
#include <string>

namespace ysu
{
class rpc_config;
class rpc_handler_interface;
class logger_mt;
class rpc_handler_request_params;

class rpc_handler : public std::enable_shared_from_this<ysu::rpc_handler>
{
public:
	rpc_handler (ysu::rpc_config const & rpc_config, std::string const & body_a, std::string const & request_id_a, std::function<void(std::string const &)> const & response_a, ysu::rpc_handler_interface & rpc_handler_interface_a, ysu::logger_mt & logger);
	void process_request (ysu::rpc_handler_request_params const & request_params);

private:
	std::string body;
	std::string request_id;
	boost::property_tree::ptree request;
	std::function<void(std::string const &)> response;
	ysu::rpc_config const & rpc_config;
	ysu::rpc_handler_interface & rpc_handler_interface;
	ysu::logger_mt & logger;
};
}
