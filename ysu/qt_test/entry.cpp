#include <ysu/node/common.hpp>

#include <gtest/gtest.h>

#include <QApplication>
QApplication * test_application = nullptr;
namespace ysu
{
void cleanup_dev_directories_on_exit ();
void force_ysu_dev_network ();
}

int main (int argc, char ** argv)
{
	ysu::force_ysu_dev_network ();
	ysu::node_singleton_memory_pool_purge_guard memory_pool_cleanup_guard;
	QApplication application (argc, argv);
	test_application = &application;
	testing::InitGoogleTest (&argc, argv);
	auto res = RUN_ALL_TESTS ();
	ysu::cleanup_dev_directories_on_exit ();
	return res;
}
