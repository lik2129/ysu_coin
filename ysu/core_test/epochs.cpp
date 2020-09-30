#include <ysu/lib/epoch.hpp>
#include <ysu/secure/common.hpp>

#include <gtest/gtest.h>

TEST (epochs, is_epoch_link)
{
	ysu::epochs epochs;
	// Test epoch 1
	ysu::keypair key1;
	auto link1 = 42;
	auto link2 = 43;
	ASSERT_FALSE (epochs.is_epoch_link (link1));
	ASSERT_FALSE (epochs.is_epoch_link (link2));
	epochs.add (ysu::epoch::epoch_1, key1.pub, link1);
	ASSERT_TRUE (epochs.is_epoch_link (link1));
	ASSERT_FALSE (epochs.is_epoch_link (link2));
	ASSERT_EQ (key1.pub, epochs.signer (ysu::epoch::epoch_1));
	ASSERT_EQ (epochs.epoch (link1), ysu::epoch::epoch_1);

	// Test epoch 2
	ysu::keypair key2;
	epochs.add (ysu::epoch::epoch_2, key2.pub, link2);
	ASSERT_TRUE (epochs.is_epoch_link (link2));
	ASSERT_EQ (key2.pub, epochs.signer (ysu::epoch::epoch_2));
	ASSERT_EQ (ysu::uint256_union (link1), epochs.link (ysu::epoch::epoch_1));
	ASSERT_EQ (ysu::uint256_union (link2), epochs.link (ysu::epoch::epoch_2));
	ASSERT_EQ (epochs.epoch (link2), ysu::epoch::epoch_2);
}

TEST (epochs, is_sequential)
{
	ASSERT_TRUE (ysu::epochs::is_sequential (ysu::epoch::epoch_0, ysu::epoch::epoch_1));
	ASSERT_TRUE (ysu::epochs::is_sequential (ysu::epoch::epoch_1, ysu::epoch::epoch_2));

	ASSERT_FALSE (ysu::epochs::is_sequential (ysu::epoch::epoch_0, ysu::epoch::epoch_2));
	ASSERT_FALSE (ysu::epochs::is_sequential (ysu::epoch::epoch_0, ysu::epoch::invalid));
	ASSERT_FALSE (ysu::epochs::is_sequential (ysu::epoch::unspecified, ysu::epoch::epoch_1));
	ASSERT_FALSE (ysu::epochs::is_sequential (ysu::epoch::epoch_1, ysu::epoch::epoch_0));
	ASSERT_FALSE (ysu::epochs::is_sequential (ysu::epoch::epoch_2, ysu::epoch::epoch_0));
	ASSERT_FALSE (ysu::epochs::is_sequential (ysu::epoch::epoch_2, ysu::epoch::epoch_2));
}
