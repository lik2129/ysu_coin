#include <ysu/lib/config.hpp>
#include <ysu/lib/jsonconfig.hpp>
#include <ysu/lib/tomlconfig.hpp>
#include <ysu/node/node_rpc_config.hpp>

#include <boost/property_tree/ptree.hpp>

ysu::error ysu::node_rpc_config::serialize_json (ysu::jsonconfig & json) const
{
	json.put ("version", json_version ());
	json.put ("enable_sign_hash", enable_sign_hash);

	ysu::jsonconfig child_process_l;
	child_process_l.put ("enable", child_process.enable);
	child_process_l.put ("rpc_path", child_process.rpc_path);
	json.put_child ("child_process", child_process_l);
	return json.get_error ();
}

ysu::error ysu::node_rpc_config::serialize_toml (ysu::tomlconfig & toml) const
{
	toml.put ("enable_sign_hash", enable_sign_hash, "Allow or disallow signing of hashes.\ntype:bool");

	ysu::tomlconfig child_process_l;
	child_process_l.put ("enable", child_process.enable, "Enable or disable RPC child process. If false, an in-process RPC server is used.\ntype:bool");
	child_process_l.put ("rpc_path", child_process.rpc_path, "Path to the ysu_rpc executable. Must be set if child process is enabled.\ntype:string,path");
	toml.put_child ("child_process", child_process_l);
	return toml.get_error ();
}

ysu::error ysu::node_rpc_config::deserialize_toml (ysu::tomlconfig & toml)
{
	toml.get_optional ("enable_sign_hash", enable_sign_hash);
	toml.get_optional<bool> ("enable_sign_hash", enable_sign_hash);

	auto child_process_l (toml.get_optional_child ("child_process"));
	if (child_process_l)
	{
		child_process_l->get_optional<bool> ("enable", child_process.enable);
		child_process_l->get_optional<std::string> ("rpc_path", child_process.rpc_path);
	}

	return toml.get_error ();
}

ysu::error ysu::node_rpc_config::deserialize_json (bool & upgraded_a, ysu::jsonconfig & json, boost::filesystem::path const & data_path)
{
	json.get_optional<bool> ("enable_sign_hash", enable_sign_hash);

	auto child_process_l (json.get_optional_child ("child_process"));
	if (child_process_l)
	{
		child_process_l->get_optional<bool> ("enable", child_process.enable);
		child_process_l->get_optional<std::string> ("rpc_path", child_process.rpc_path);
	}

	return json.get_error ();
}

void ysu::node_rpc_config::set_request_callback (std::function<void(boost::property_tree::ptree const &)> callback_a)
{
	debug_assert (ysu::network_constants ().is_dev_network ());
	request_callback = std::move (callback_a);
}
