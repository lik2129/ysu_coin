#pragma once

#include <ysu/lib/errors.hpp>
#include <ysu/node/node_pow_server_config.hpp>
#include <ysu/node/node_rpc_config.hpp>
#include <ysu/node/nodeconfig.hpp>
#include <ysu/node/openclconfig.hpp>

#include <vector>

namespace ysu
{
class jsonconfig;
class tomlconfig;
class daemon_config
{
public:
	daemon_config () = default;
	daemon_config (boost::filesystem::path const & data_path);
	ysu::error deserialize_json (bool &, ysu::jsonconfig &);
	ysu::error serialize_json (ysu::jsonconfig &);
	ysu::error deserialize_toml (ysu::tomlconfig &);
	ysu::error serialize_toml (ysu::tomlconfig &);
	bool rpc_enable{ false };
	ysu::node_rpc_config rpc;
	ysu::node_config node;
	bool opencl_enable{ false };
	ysu::opencl_config opencl;
	ysu::node_pow_server_config pow_server;
	boost::filesystem::path data_path;
	unsigned json_version () const
	{
		return 2;
	}
};

ysu::error read_node_config_toml (boost::filesystem::path const &, ysu::daemon_config & config_a, std::vector<std::string> const & config_overrides = std::vector<std::string> ());
ysu::error read_and_update_daemon_config (boost::filesystem::path const &, ysu::daemon_config & config_a, ysu::jsonconfig & json_a);
}
