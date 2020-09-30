#include <ysu/node/testing.hpp>
#include <ysu/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST (gap_cache, add_new)
{
	ysu::system system (1);
	ysu::gap_cache cache (*system.nodes[0]);
	auto block1 (std::make_shared<ysu::send_block> (0, 1, 2, ysu::keypair ().prv, 4, 5));
	cache.add (block1->hash ());
}

TEST (gap_cache, add_existing)
{
	ysu::system system (1);
	ysu::gap_cache cache (*system.nodes[0]);
	auto block1 (std::make_shared<ysu::send_block> (0, 1, 2, ysu::keypair ().prv, 4, 5));
	cache.add (block1->hash ());
	ysu::unique_lock<std::mutex> lock (cache.mutex);
	auto existing1 (cache.blocks.get<1> ().find (block1->hash ()));
	ASSERT_NE (cache.blocks.get<1> ().end (), existing1);
	auto arrival (existing1->arrival);
	lock.unlock ();
	ASSERT_TIMELY (20s, arrival != std::chrono::steady_clock::now ());
	cache.add (block1->hash ());
	ASSERT_EQ (1, cache.size ());
	lock.lock ();
	auto existing2 (cache.blocks.get<1> ().find (block1->hash ()));
	ASSERT_NE (cache.blocks.get<1> ().end (), existing2);
	ASSERT_GT (existing2->arrival, arrival);
}

TEST (gap_cache, comparison)
{
	ysu::system system (1);
	ysu::gap_cache cache (*system.nodes[0]);
	auto block1 (std::make_shared<ysu::send_block> (1, 0, 2, ysu::keypair ().prv, 4, 5));
	cache.add (block1->hash ());
	ysu::unique_lock<std::mutex> lock (cache.mutex);
	auto existing1 (cache.blocks.get<1> ().find (block1->hash ()));
	ASSERT_NE (cache.blocks.get<1> ().end (), existing1);
	auto arrival (existing1->arrival);
	lock.unlock ();
	ASSERT_TIMELY (20s, std::chrono::steady_clock::now () != arrival);
	auto block3 (std::make_shared<ysu::send_block> (0, 42, 1, ysu::keypair ().prv, 3, 4));
	cache.add (block3->hash ());
	ASSERT_EQ (2, cache.size ());
	lock.lock ();
	auto existing2 (cache.blocks.get<1> ().find (block3->hash ()));
	ASSERT_NE (cache.blocks.get<1> ().end (), existing2);
	ASSERT_GT (existing2->arrival, arrival);
	ASSERT_EQ (arrival, cache.blocks.get<1> ().begin ()->arrival);
}

// Upon receiving enough votes for a gapped block, a lazy bootstrap should be initiated
TEST (gap_cache, gap_bootstrap)
{
	ysu::node_flags node_flags;
	node_flags.disable_legacy_bootstrap = true;
	node_flags.disable_request_loop = true; // to avoid fallback behavior of broadcasting blocks
	ysu::system system (2, ysu::transport::transport_type::tcp, node_flags);

	auto & node1 (*system.nodes[0]);
	auto & node2 (*system.nodes[1]);
	ysu::block_hash latest (node1.latest (ysu::dev_genesis_key.pub));
	ysu::keypair key;
	auto send (std::make_shared<ysu::send_block> (latest, key.pub, ysu::genesis_amount - 100, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (latest)));
	node1.process (*send);
	ASSERT_EQ (ysu::genesis_amount - 100, node1.balance (ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount, node2.balance (ysu::genesis_account));
	// Confirm send block, allowing voting on the upcoming block
	node1.block_confirm (send);
	auto election = node1.active.election (send->qualified_root ());
	ASSERT_NE (nullptr, election);
	election->force_confirm ();
	ASSERT_TIMELY (2s, node1.block_confirmed (send->hash ()));
	node1.active.erase (*send);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	auto latest_block (system.wallet (0)->send_action (ysu::dev_genesis_key.pub, key.pub, 100));
	ASSERT_NE (nullptr, latest_block);
	ASSERT_EQ (ysu::genesis_amount - 200, node1.balance (ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount, node2.balance (ysu::genesis_account));
	ASSERT_TIMELY (10s, node2.balance (ysu::genesis_account) == ysu::genesis_amount - 200);
}

TEST (gap_cache, two_dependencies)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::keypair key;
	ysu::genesis genesis;
	auto send1 (std::make_shared<ysu::send_block> (genesis.hash (), key.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (genesis.hash ())));
	auto send2 (std::make_shared<ysu::send_block> (send1->hash (), key.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (send1->hash ())));
	auto open (std::make_shared<ysu::open_block> (send1->hash (), key.pub, key.pub, key.prv, key.pub, *system.work.generate (key.pub)));
	ASSERT_EQ (0, node1.gap_cache.size ());
	node1.block_processor.add (send2, ysu::seconds_since_epoch ());
	node1.block_processor.flush ();
	ASSERT_EQ (1, node1.gap_cache.size ());
	node1.block_processor.add (open, ysu::seconds_since_epoch ());
	node1.block_processor.flush ();
	ASSERT_EQ (2, node1.gap_cache.size ());
	node1.block_processor.add (send1, ysu::seconds_since_epoch ());
	node1.block_processor.flush ();
	ASSERT_EQ (0, node1.gap_cache.size ());
	auto transaction (node1.store.tx_begin_read ());
	ASSERT_TRUE (node1.store.block_exists (transaction, send1->hash ()));
	ASSERT_TRUE (node1.store.block_exists (transaction, send2->hash ()));
	ASSERT_TRUE (node1.store.block_exists (transaction, open->hash ()));
}
