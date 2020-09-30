#include <ysu/lib/config.hpp>

#include <boost/filesystem/path.hpp>
#include <boost/lexical_cast.hpp>

#include <valgrind/valgrind.h>

namespace ysu
{
work_thresholds const network_constants::publish_full (
0xffffffc000000000,
0xfffffff800000000, // 8x higher than epoch_1
0xfffffe0000000000 // 8x lower than epoch_1
);

work_thresholds const network_constants::publish_beta (
0xfffff00000000000, // 64x lower than publish_full.epoch_1
0xfffff00000000000, // same as epoch_1
0xffffe00000000000 // 2x lower than epoch_1
);

work_thresholds const network_constants::publish_dev (
0xfe00000000000000, // Very low for tests
0xffc0000000000000, // 8x higher than epoch_1
0xf000000000000000 // 8x lower than epoch_1
);

const char * network_constants::active_network_err_msg = "Invalid network. Valid values are live, test, beta and dev.";

uint8_t get_major_node_version ()
{
	return boost::numeric_cast<uint8_t> (boost::lexical_cast<int> (YSU_MAJOR_VERSION_STRING));
}
uint8_t get_minor_node_version ()
{
	return boost::numeric_cast<uint8_t> (boost::lexical_cast<int> (YSU_MINOR_VERSION_STRING));
}
uint8_t get_patch_node_version ()
{
	return boost::numeric_cast<uint8_t> (boost::lexical_cast<int> (YSU_PATCH_VERSION_STRING));
}
uint8_t get_pre_release_node_version ()
{
	return boost::numeric_cast<uint8_t> (boost::lexical_cast<int> (YSU_PRE_RELEASE_VERSION_STRING));
}

void force_ysu_dev_network ()
{
	ysu::network_constants::set_active_network (ysu::ysu_networks::ysu_dev_network);
}

bool running_within_valgrind ()
{
	return (RUNNING_ON_VALGRIND > 0);
}

std::string get_config_path (boost::filesystem::path const & data_path)
{
	return (data_path / "config.json").string ();
}

std::string get_rpc_config_path (boost::filesystem::path const & data_path)
{
	return (data_path / "rpc_config.json").string ();
}

std::string get_node_toml_config_path (boost::filesystem::path const & data_path)
{
	return (data_path / "config-node.toml").string ();
}

std::string get_rpc_toml_config_path (boost::filesystem::path const & data_path)
{
	return (data_path / "config-rpc.toml").string ();
}

std::string get_qtwallet_toml_config_path (boost::filesystem::path const & data_path)
{
	return (data_path / "config-qtwallet.toml").string ();
}
std::string get_access_toml_config_path (boost::filesystem::path const & data_path)
{
	return (data_path / "config-access.toml").string ();
}
}