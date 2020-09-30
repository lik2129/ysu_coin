#pragma once

#include <ysu/lib/config.hpp>
#include <ysu/lib/errors.hpp>

#include <string>
#include <thread>
#include <vector>

namespace boost
{
namespace filesystem
{
	class path;
}
}

namespace ysu
{
class jsonconfig;
class tomlconfig;

/** Configuration options for RPC TLS */
class rpc_secure_config final
{
public:
	ysu::error serialize_json (ysu::jsonconfig &) const;
	ysu::error deserialize_json (ysu::jsonconfig &);
	ysu::error serialize_toml (ysu::tomlconfig &) const;
	ysu::error deserialize_toml (ysu::tomlconfig &);

	/** If true, enable TLS */
	bool enable{ false };
	/** If true, log certificate verification details */
	bool verbose_logging{ false };
	/** Must be set if the private key PEM is password protected */
	std::string server_key_passphrase;
	/** Path to certificate- or chain file. Must be PEM formatted. */
	std::string server_cert_path;
	/** Path to private key file. Must be PEM formatted.*/
	std::string server_key_path;
	/** Path to dhparam file */
	std::string server_dh_path;
	/** Optional path to directory containing client certificates */
	std::string client_certs_path;
};

class rpc_process_config final
{
public:
	rpc_process_config ();
	ysu::network_constants network_constants;
	unsigned io_threads{ (4 < std::thread::hardware_concurrency ()) ? std::thread::hardware_concurrency () : 4 };
	std::string ipc_address;
	uint16_t ipc_port{ network_constants.default_ipc_port };
	unsigned num_ipc_connections{ (network_constants.is_live_network () || network_constants.is_test_network ()) ? 8u : network_constants.is_beta_network () ? 4u : 1u };
	static unsigned json_version ()
	{
		return 1;
	}
};

class rpc_logging_config final
{
public:
	bool log_rpc{ true };
};

class rpc_config final
{
public:
	rpc_config ();
	explicit rpc_config (uint16_t, bool);
	ysu::error serialize_json (ysu::jsonconfig &) const;
	ysu::error deserialize_json (bool & upgraded_a, ysu::jsonconfig &);
	ysu::error serialize_toml (ysu::tomlconfig &) const;
	ysu::error deserialize_toml (ysu::tomlconfig &);

	ysu::rpc_process_config rpc_process;
	std::string address;
	uint16_t port{ rpc_process.network_constants.default_rpc_port };
	bool enable_control{ false };
	rpc_secure_config secure;
	uint8_t max_json_depth{ 20 };
	uint64_t max_request_size{ 32 * 1024 * 1024 };
	ysu::rpc_logging_config rpc_logging;
	static unsigned json_version ()
	{
		return 1;
	}
};

ysu::error read_rpc_config_toml (boost::filesystem::path const & data_path_a, ysu::rpc_config & config_a, std::vector<std::string> const & config_overrides = std::vector<std::string> ());
ysu::error read_and_update_rpc_config (boost::filesystem::path const & data_path, ysu::rpc_config & config_a);

std::string get_default_rpc_filepath ();
}
