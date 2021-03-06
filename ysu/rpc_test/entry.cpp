#include <ysu/lib/memory.hpp>
#include <ysu/node/common.hpp>

#include <gtest/gtest.h>
namespace ysu
{
void cleanup_dev_directories_on_exit ();
void force_ysu_dev_network ();
}

int main (int argc, char ** argv)
{
	ysu::force_ysu_dev_network ();
	ysu::set_use_memory_pools (false);
	ysu::node_singleton_memory_pool_purge_guard cleanup_guard;
	testing::InitGoogleTest (&argc, argv);
	auto res = RUN_ALL_TESTS ();
	ysu::cleanup_dev_directories_on_exit ();
	return res;
}
