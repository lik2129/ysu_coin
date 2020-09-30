#pragma once

#include <ysu/lib/rpcconfig.hpp>

#include <boost/property_tree/ptree_fwd.hpp>

#include <string>

namespace boost
{
namespace filesystem
{
	class path;
}
}

namespace ysu
{
class tomlconfig;
class rpc_child_process_config final
{
public:
	bool enable{ false };
	std::string rpc_path{ get_default_rpc_filepath () };
};

class node_rpc_config final
{
public:
	ysu::error serialize_json (ysu::jsonconfig &) const;
	ysu::error deserialize_json (bool & upgraded_a, ysu::jsonconfig &, boost::filesystem::path const & data_path);
	ysu::error serialize_toml (ysu::tomlconfig & toml) const;
	ysu::error deserialize_toml (ysu::tomlconfig & toml);

	bool enable_sign_hash{ false };
	ysu::rpc_child_process_config child_process;
	static unsigned json_version ()
	{
		return 1;
	}

	// Used in tests to ensure requests are modified in specific cases
	void set_request_callback (std::function<void(boost::property_tree::ptree const &)>);
	std::function<void(boost::property_tree::ptree const &)> request_callback;
};
}
