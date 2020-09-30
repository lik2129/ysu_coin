#include <ysu/lib/tomlconfig.hpp>
#include <ysu/node/node_pow_server_config.hpp>

ysu::error ysu::node_pow_server_config::serialize_toml (ysu::tomlconfig & toml) const
{
	toml.put ("enable", enable, "Value is currently not in use. Enable or disable starting Ysu PoW Server as a child process.\ntype:bool");
	toml.put ("ysu_pow_server_path", pow_server_path, "Value is currently not in use. Path to the ysu_pow_server executable.\ntype:string,path");
	return toml.get_error ();
}

ysu::error ysu::node_pow_server_config::deserialize_toml (ysu::tomlconfig & toml)
{
	toml.get_optional<bool> ("enable", enable);
	toml.get_optional<std::string> ("ysu_pow_server_path", pow_server_path);

	return toml.get_error ();
}
