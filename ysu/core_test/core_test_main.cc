#include "gtest/gtest.h"

#include <ysu/node/common.hpp>
#include <ysu/node/logging.hpp>

#include <boost/filesystem/path.hpp>

namespace ysu
{
void cleanup_dev_directories_on_exit ();
void force_ysu_dev_network ();
boost::filesystem::path unique_path ();
}

GTEST_API_ int main (int argc, char ** argv)
{
	printf ("Running main() from core_test_main.cc\n");
	ysu::force_ysu_dev_network ();
	ysu::node_singleton_memory_pool_purge_guard memory_pool_cleanup_guard;
	// Setting up logging so that there aren't any piped to standard output.
	ysu::logging logging;
	logging.init (ysu::unique_path ());
	testing::InitGoogleTest (&argc, argv);
	auto res = RUN_ALL_TESTS ();
	ysu::cleanup_dev_directories_on_exit ();
	return res;
}
