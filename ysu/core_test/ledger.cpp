#include <ysu/lib/stats.hpp>
#include <ysu/lib/threading.hpp>
#include <ysu/node/election.hpp>
#include <ysu/node/testing.hpp>
#include <ysu/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

// Init returns an error if it can't open files at the path
TEST (ledger, store_error)
{
	if (ysu::using_rocksdb_in_tests ())
	{
		// Don't test this in rocksdb mode
		return;
	}
	ysu::logger_mt logger;
	ysu::mdb_store store (logger, boost::filesystem::path ("///"));
	ASSERT_TRUE (store.init_error ());
	ysu::stat stats;
	ysu::ledger ledger (store, stats);
}

// Ledger can be initialized and returns a basic query for an empty account
TEST (ledger, empty)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::account account;
	auto transaction (store->tx_begin_read ());
	auto balance (ledger.account_balance (transaction, account));
	ASSERT_TRUE (balance.is_zero ());
}

// Genesis account should have the max balance on empty initialization
TEST (ledger, genesis_balance)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	auto balance (ledger.account_balance (transaction, ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount, balance);
	auto amount (ledger.amount (transaction, ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount, amount);
	ysu::account_info info;
	ASSERT_FALSE (store->account_get (transaction, ysu::genesis_account, info));
	ASSERT_EQ (1, ledger.cache.account_count);
	// Frontier time should have been updated when genesis balance was added
	ASSERT_GE (ysu::seconds_since_epoch (), info.modified);
	ASSERT_LT (ysu::seconds_since_epoch () - info.modified, 10);
	// Genesis block should be confirmed by default
	ysu::confirmation_height_info confirmation_height_info;
	ASSERT_FALSE (store->confirmation_height_get (transaction, ysu::genesis_account, confirmation_height_info));
	ASSERT_EQ (confirmation_height_info.height, 1);
	ASSERT_EQ (confirmation_height_info.frontier, genesis.hash ());
}

// All nodes in the system should agree on the genesis balance
TEST (system, system_genesis)
{
	ysu::system system (2);
	for (auto & i : system.nodes)
	{
		auto transaction (i->store.tx_begin_read ());
		ASSERT_EQ (ysu::genesis_amount, i->ledger.account_balance (transaction, ysu::genesis_account));
	}
}

TEST (ledger, process_modifies_sideband)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	store->initialize (store->tx_begin_write (), genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (store->tx_begin_write (), send1).code);
	ASSERT_EQ (send1.sideband ().timestamp, store->block_get (store->tx_begin_read (), send1.hash ())->sideband ().timestamp);
}

// Create a send block and publish it.
TEST (ledger, process_send)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	auto transaction (store->tx_begin_write ());
	ysu::genesis genesis;
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::account_info info1;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info1));
	ysu::keypair key2;
	ysu::send_block send (info1.head, key2.pub, 50, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (info1.head));
	ysu::block_hash hash1 (send.hash ());
	ASSERT_EQ (ysu::dev_genesis_key.pub, store->frontier_get (transaction, info1.head));
	ASSERT_EQ (1, info1.block_count);
	// This was a valid block, it should progress.
	auto return1 (ledger.process (transaction, send));
	ASSERT_EQ (ysu::dev_genesis_key.pub, send.sideband ().account);
	ASSERT_EQ (2, send.sideband ().height);
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.amount (transaction, hash1));
	ASSERT_TRUE (store->frontier_get (transaction, info1.head).is_zero ());
	ASSERT_EQ (ysu::dev_genesis_key.pub, store->frontier_get (transaction, hash1));
	ASSERT_EQ (ysu::process_result::progress, return1.code);
	ASSERT_EQ (ysu::dev_genesis_key.pub, store->block_account_calculated (send));
	ASSERT_EQ (50, ledger.account_balance (transaction, ysu::dev_genesis_key.pub));
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.account_pending (transaction, key2.pub));
	ysu::account_info info2;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info2));
	ASSERT_EQ (2, info2.block_count);
	auto latest6 (store->block_get (transaction, info2.head));
	ASSERT_NE (nullptr, latest6);
	auto latest7 (dynamic_cast<ysu::send_block *> (latest6.get ()));
	ASSERT_NE (nullptr, latest7);
	ASSERT_EQ (send, *latest7);
	// Create an open block opening an account accepting the send we just created
	ysu::open_block open (hash1, key2.pub, key2.pub, key2.prv, key2.pub, *pool.generate (key2.pub));
	ysu::block_hash hash2 (open.hash ());
	// This was a valid block, it should progress.
	auto return2 (ledger.process (transaction, open));
	ASSERT_EQ (ysu::process_result::progress, return2.code);
	ASSERT_EQ (key2.pub, open.sideband ().account);
	ASSERT_EQ (ysu::genesis_amount - 50, open.sideband ().balance.number ());
	ASSERT_EQ (1, open.sideband ().height);
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.amount (transaction, hash2));
	ASSERT_EQ (ysu::process_result::progress, return2.code);
	ASSERT_EQ (key2.pub, store->block_account_calculated (open));
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.amount (transaction, hash2));
	ASSERT_EQ (key2.pub, store->frontier_get (transaction, hash2));
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.account_balance (transaction, key2.pub));
	ASSERT_EQ (0, ledger.account_pending (transaction, key2.pub));
	ASSERT_EQ (50, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.weight (key2.pub));
	ysu::account_info info3;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info3));
	auto latest2 (store->block_get (transaction, info3.head));
	ASSERT_NE (nullptr, latest2);
	auto latest3 (dynamic_cast<ysu::send_block *> (latest2.get ()));
	ASSERT_NE (nullptr, latest3);
	ASSERT_EQ (send, *latest3);
	ysu::account_info info4;
	ASSERT_FALSE (store->account_get (transaction, key2.pub, info4));
	auto latest4 (store->block_get (transaction, info4.head));
	ASSERT_NE (nullptr, latest4);
	auto latest5 (dynamic_cast<ysu::open_block *> (latest4.get ()));
	ASSERT_NE (nullptr, latest5);
	ASSERT_EQ (open, *latest5);
	ASSERT_FALSE (ledger.rollback (transaction, hash2));
	ASSERT_TRUE (store->frontier_get (transaction, hash2).is_zero ());
	ysu::account_info info5;
	ASSERT_TRUE (ledger.store.account_get (transaction, key2.pub, info5));
	ysu::pending_info pending1;
	ASSERT_FALSE (ledger.store.pending_get (transaction, ysu::pending_key (key2.pub, hash1), pending1));
	ASSERT_EQ (ysu::dev_genesis_key.pub, pending1.source);
	ASSERT_EQ (ysu::genesis_amount - 50, pending1.amount.number ());
	ASSERT_EQ (0, ledger.account_balance (transaction, key2.pub));
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.account_pending (transaction, key2.pub));
	ASSERT_EQ (50, ledger.account_balance (transaction, ysu::dev_genesis_key.pub));
	ASSERT_EQ (50, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, ledger.weight (key2.pub));
	ysu::account_info info6;
	ASSERT_FALSE (ledger.store.account_get (transaction, ysu::dev_genesis_key.pub, info6));
	ASSERT_EQ (hash1, info6.head);
	ASSERT_FALSE (ledger.rollback (transaction, info6.head));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_EQ (ysu::dev_genesis_key.pub, store->frontier_get (transaction, info1.head));
	ASSERT_TRUE (store->frontier_get (transaction, hash1).is_zero ());
	ysu::account_info info7;
	ASSERT_FALSE (ledger.store.account_get (transaction, ysu::dev_genesis_key.pub, info7));
	ASSERT_EQ (1, info7.block_count);
	ASSERT_EQ (info1.head, info7.head);
	ysu::pending_info pending2;
	ASSERT_TRUE (ledger.store.pending_get (transaction, ysu::pending_key (key2.pub, hash1), pending2));
	ASSERT_EQ (ysu::genesis_amount, ledger.account_balance (transaction, ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, ledger.account_pending (transaction, key2.pub));
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
}

TEST (ledger, process_receive)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::account_info info1;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info1));
	ysu::keypair key2;
	ysu::send_block send (info1.head, key2.pub, 50, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (info1.head));
	ysu::block_hash hash1 (send.hash ());
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send).code);
	ysu::keypair key3;
	ysu::open_block open (hash1, key3.pub, key2.pub, key2.prv, key2.pub, *pool.generate (key2.pub));
	ysu::block_hash hash2 (open.hash ());
	auto return1 (ledger.process (transaction, open));
	ASSERT_EQ (ysu::process_result::progress, return1.code);
	ASSERT_EQ (key2.pub, store->block_account_calculated (open));
	ASSERT_EQ (key2.pub, open.sideband ().account);
	ASSERT_EQ (ysu::genesis_amount - 50, open.sideband ().balance.number ());
	ASSERT_EQ (1, open.sideband ().height);
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.amount (transaction, hash2));
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.weight (key3.pub));
	ysu::send_block send2 (hash1, key2.pub, 25, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (hash1));
	ysu::block_hash hash3 (send2.hash ());
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send2).code);
	ysu::receive_block receive (hash2, hash3, key2.prv, key2.pub, *pool.generate (hash2));
	auto hash4 (receive.hash ());
	ASSERT_EQ (key2.pub, store->frontier_get (transaction, hash2));
	auto return2 (ledger.process (transaction, receive));
	ASSERT_EQ (key2.pub, receive.sideband ().account);
	ASSERT_EQ (ysu::genesis_amount - 25, receive.sideband ().balance.number ());
	ASSERT_EQ (2, receive.sideband ().height);
	ASSERT_EQ (25, ledger.amount (transaction, hash4));
	ASSERT_TRUE (store->frontier_get (transaction, hash2).is_zero ());
	ASSERT_EQ (key2.pub, store->frontier_get (transaction, hash4));
	ASSERT_EQ (ysu::process_result::progress, return2.code);
	ASSERT_EQ (key2.pub, store->block_account_calculated (receive));
	ASSERT_EQ (hash4, ledger.latest (transaction, key2.pub));
	ASSERT_EQ (25, ledger.account_balance (transaction, ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, ledger.account_pending (transaction, key2.pub));
	ASSERT_EQ (ysu::genesis_amount - 25, ledger.account_balance (transaction, key2.pub));
	ASSERT_EQ (ysu::genesis_amount - 25, ledger.weight (key3.pub));
	ASSERT_FALSE (ledger.rollback (transaction, hash4));
	ASSERT_TRUE (store->block_successor (transaction, hash2).is_zero ());
	ASSERT_EQ (key2.pub, store->frontier_get (transaction, hash2));
	ASSERT_TRUE (store->frontier_get (transaction, hash4).is_zero ());
	ASSERT_EQ (25, ledger.account_balance (transaction, ysu::dev_genesis_key.pub));
	ASSERT_EQ (25, ledger.account_pending (transaction, key2.pub));
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.account_balance (transaction, key2.pub));
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.weight (key3.pub));
	ASSERT_EQ (hash2, ledger.latest (transaction, key2.pub));
	ysu::pending_info pending1;
	ASSERT_FALSE (ledger.store.pending_get (transaction, ysu::pending_key (key2.pub, hash3), pending1));
	ASSERT_EQ (ysu::dev_genesis_key.pub, pending1.source);
	ASSERT_EQ (25, pending1.amount.number ());
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
}

TEST (ledger, rollback_receiver)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::account_info info1;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info1));
	ysu::keypair key2;
	ysu::send_block send (info1.head, key2.pub, 50, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (info1.head));
	ysu::block_hash hash1 (send.hash ());
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send).code);
	ysu::keypair key3;
	ysu::open_block open (hash1, key3.pub, key2.pub, key2.prv, key2.pub, *pool.generate (key2.pub));
	ysu::block_hash hash2 (open.hash ());
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open).code);
	ASSERT_EQ (hash2, ledger.latest (transaction, key2.pub));
	ASSERT_EQ (50, ledger.account_balance (transaction, ysu::dev_genesis_key.pub));
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.account_balance (transaction, key2.pub));
	ASSERT_EQ (50, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, ledger.weight (key2.pub));
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.weight (key3.pub));
	ASSERT_FALSE (ledger.rollback (transaction, hash1));
	ASSERT_EQ (ysu::genesis_amount, ledger.account_balance (transaction, ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, ledger.account_balance (transaction, key2.pub));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, ledger.weight (key2.pub));
	ASSERT_EQ (0, ledger.weight (key3.pub));
	ysu::account_info info2;
	ASSERT_TRUE (ledger.store.account_get (transaction, key2.pub, info2));
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
	ysu::pending_info pending1;
	ASSERT_TRUE (ledger.store.pending_get (transaction, ysu::pending_key (key2.pub, info2.head), pending1));
}

TEST (ledger, rollback_representation)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key5;
	ysu::change_block change1 (genesis.hash (), key5.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, change1).code);
	ysu::keypair key3;
	ysu::change_block change2 (change1.hash (), key3.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (change1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, change2).code);
	ysu::keypair key2;
	ysu::send_block send1 (change2.hash (), key2.pub, 50, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (change2.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::keypair key4;
	ysu::open_block open (send1.hash (), key4.pub, key2.pub, key2.prv, key2.pub, *pool.generate (key2.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open).code);
	ysu::send_block send2 (send1.hash (), key2.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send2).code);
	ysu::receive_block receive1 (open.hash (), send2.hash (), key2.prv, key2.pub, *pool.generate (open.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive1).code);
	ASSERT_EQ (1, ledger.weight (key3.pub));
	ASSERT_EQ (ysu::genesis_amount - 1, ledger.weight (key4.pub));
	ysu::account_info info1;
	ASSERT_FALSE (store->account_get (transaction, key2.pub, info1));
	ASSERT_EQ (key4.pub, info1.representative);
	ASSERT_FALSE (ledger.rollback (transaction, receive1.hash ()));
	ysu::account_info info2;
	ASSERT_FALSE (store->account_get (transaction, key2.pub, info2));
	ASSERT_EQ (key4.pub, info2.representative);
	ASSERT_EQ (0, ledger.weight (key2.pub));
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.weight (key4.pub));
	ASSERT_FALSE (ledger.rollback (transaction, open.hash ()));
	ASSERT_EQ (1, ledger.weight (key3.pub));
	ASSERT_EQ (0, ledger.weight (key4.pub));
	ledger.rollback (transaction, send1.hash ());
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (key3.pub));
	ysu::account_info info3;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info3));
	ASSERT_EQ (key3.pub, info3.representative);
	ASSERT_FALSE (ledger.rollback (transaction, change2.hash ()));
	ysu::account_info info4;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info4));
	ASSERT_EQ (key5.pub, info4.representative);
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (key5.pub));
	ASSERT_EQ (0, ledger.weight (key3.pub));
}

TEST (ledger, receive_rollback)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::send_block send (genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send).code);
	ysu::receive_block receive (send.hash (), send.hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive).code);
	ASSERT_FALSE (ledger.rollback (transaction, receive.hash ()));
}

TEST (ledger, process_duplicate)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::account_info info1;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info1));
	ysu::keypair key2;
	ysu::send_block send (info1.head, key2.pub, 50, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (info1.head));
	ysu::block_hash hash1 (send.hash ());
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send).code);
	ASSERT_EQ (ysu::process_result::old, ledger.process (transaction, send).code);
	ysu::open_block open (hash1, 1, key2.pub, key2.prv, key2.pub, *pool.generate (key2.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open).code);
	ASSERT_EQ (ysu::process_result::old, ledger.process (transaction, open).code);
}

TEST (ledger, representative_genesis)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	auto latest (ledger.latest (transaction, ysu::dev_genesis_key.pub));
	ASSERT_FALSE (latest.is_zero ());
	ASSERT_EQ (genesis.open->hash (), ledger.representative (transaction, latest));
}

TEST (ledger, weight)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::genesis_account));
}

TEST (ledger, representative_change)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::keypair key2;
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, ledger.weight (key2.pub));
	ysu::account_info info1;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info1));
	ysu::change_block block (info1.head, key2.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (info1.head));
	ASSERT_EQ (ysu::dev_genesis_key.pub, store->frontier_get (transaction, info1.head));
	auto return1 (ledger.process (transaction, block));
	ASSERT_EQ (0, ledger.amount (transaction, block.hash ()));
	ASSERT_TRUE (store->frontier_get (transaction, info1.head).is_zero ());
	ASSERT_EQ (ysu::dev_genesis_key.pub, store->frontier_get (transaction, block.hash ()));
	ASSERT_EQ (ysu::process_result::progress, return1.code);
	ASSERT_EQ (ysu::dev_genesis_key.pub, store->block_account_calculated (block));
	ASSERT_EQ (0, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (key2.pub));
	ysu::account_info info2;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info2));
	ASSERT_EQ (block.hash (), info2.head);
	ASSERT_FALSE (ledger.rollback (transaction, info2.head));
	ASSERT_EQ (ysu::dev_genesis_key.pub, store->frontier_get (transaction, info1.head));
	ASSERT_TRUE (store->frontier_get (transaction, block.hash ()).is_zero ());
	ysu::account_info info3;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info3));
	ASSERT_EQ (info1.head, info3.head);
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, ledger.weight (key2.pub));
}

TEST (ledger, send_fork)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::keypair key2;
	ysu::keypair key3;
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::account_info info1;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info1));
	ysu::send_block block (info1.head, key2.pub, 100, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (info1.head));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block).code);
	ysu::send_block block2 (info1.head, key3.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (info1.head));
	ASSERT_EQ (ysu::process_result::fork, ledger.process (transaction, block2).code);
}

TEST (ledger, receive_fork)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::keypair key2;
	ysu::keypair key3;
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::account_info info1;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info1));
	ysu::send_block block (info1.head, key2.pub, 100, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (info1.head));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block).code);
	ysu::open_block block2 (block.hash (), key2.pub, key2.pub, key2.prv, key2.pub, *pool.generate (key2.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block2).code);
	ysu::change_block block3 (block2.hash (), key3.pub, key2.prv, key2.pub, *pool.generate (block2.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block3).code);
	ysu::send_block block4 (block.hash (), key2.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block4).code);
	ysu::receive_block block5 (block2.hash (), block4.hash (), key2.prv, key2.pub, *pool.generate (block2.hash ()));
	ASSERT_EQ (ysu::process_result::fork, ledger.process (transaction, block5).code);
}

TEST (ledger, open_fork)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::keypair key2;
	ysu::keypair key3;
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::account_info info1;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info1));
	ysu::send_block block (info1.head, key2.pub, 100, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (info1.head));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block).code);
	ysu::open_block block2 (block.hash (), key2.pub, key2.pub, key2.prv, key2.pub, *pool.generate (key2.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block2).code);
	ysu::open_block block3 (block.hash (), key3.pub, key2.pub, key2.prv, key2.pub, *pool.generate (key2.pub));
	ASSERT_EQ (ysu::process_result::fork, ledger.process (transaction, block3).code);
}

TEST (system, DISABLED_generate_send_existing)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::thread_runner runner (system.io_ctx, node1.config.io_threads);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::keypair stake_preserver;
	auto send_block (system.wallet (0)->send_action (ysu::genesis_account, stake_preserver.pub, ysu::genesis_amount / 3 * 2, true));
	ysu::account_info info1;
	{
		auto transaction (node1.store.tx_begin_read ());
		ASSERT_FALSE (node1.store.account_get (transaction, ysu::dev_genesis_key.pub, info1));
	}
	std::vector<ysu::account> accounts;
	accounts.push_back (ysu::dev_genesis_key.pub);
	system.generate_send_existing (node1, accounts);
	// Have stake_preserver receive funds after generate_send_existing so it isn't chosen as the destination
	{
		auto transaction (node1.store.tx_begin_write ());
		auto open_block (std::make_shared<ysu::open_block> (send_block->hash (), ysu::genesis_account, stake_preserver.pub, stake_preserver.prv, stake_preserver.pub, 0));
		node1.work_generate_blocking (*open_block);
		ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, *open_block).code);
	}
	ASSERT_GT (node1.balance (stake_preserver.pub), node1.balance (ysu::genesis_account));
	ysu::account_info info2;
	{
		auto transaction (node1.store.tx_begin_read ());
		ASSERT_FALSE (node1.store.account_get (transaction, ysu::dev_genesis_key.pub, info2));
	}
	ASSERT_NE (info1.head, info2.head);
	system.deadline_set (15s);
	while (info2.block_count < info1.block_count + 2)
	{
		ASSERT_NO_ERROR (system.poll ());
		auto transaction (node1.store.tx_begin_read ());
		ASSERT_FALSE (node1.store.account_get (transaction, ysu::dev_genesis_key.pub, info2));
	}
	ASSERT_EQ (info1.block_count + 2, info2.block_count);
	ASSERT_EQ (info2.balance, ysu::genesis_amount / 3);
	{
		auto transaction (node1.store.tx_begin_read ());
		ASSERT_NE (node1.ledger.amount (transaction, info2.head), 0);
	}
	system.stop ();
	runner.join ();
}

TEST (system, DISABLED_generate_send_new)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::thread_runner runner (system.io_ctx, node1.config.io_threads);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	{
		auto transaction (node1.store.tx_begin_read ());
		auto iterator1 (node1.store.accounts_begin (transaction));
		ASSERT_NE (node1.store.accounts_end (), iterator1);
		++iterator1;
		ASSERT_EQ (node1.store.accounts_end (), iterator1);
	}
	ysu::keypair stake_preserver;
	auto send_block (system.wallet (0)->send_action (ysu::genesis_account, stake_preserver.pub, ysu::genesis_amount / 3 * 2, true));
	{
		auto transaction (node1.store.tx_begin_write ());
		auto open_block (std::make_shared<ysu::open_block> (send_block->hash (), ysu::genesis_account, stake_preserver.pub, stake_preserver.prv, stake_preserver.pub, 0));
		node1.work_generate_blocking (*open_block);
		ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, *open_block).code);
	}
	ASSERT_GT (node1.balance (stake_preserver.pub), node1.balance (ysu::genesis_account));
	std::vector<ysu::account> accounts;
	accounts.push_back (ysu::dev_genesis_key.pub);
	// This indirectly waits for online weight to stabilize, required to prevent intermittent failures
	ASSERT_TIMELY (5s, node1.wallets.reps ().voting > 0);
	system.generate_send_new (node1, accounts);
	ysu::account new_account (0);
	{
		auto transaction (node1.wallets.tx_begin_read ());
		auto iterator2 (system.wallet (0)->store.begin (transaction));
		if (iterator2->first != ysu::dev_genesis_key.pub)
		{
			new_account = iterator2->first;
		}
		++iterator2;
		ASSERT_NE (system.wallet (0)->store.end (), iterator2);
		if (iterator2->first != ysu::dev_genesis_key.pub)
		{
			new_account = iterator2->first;
		}
		++iterator2;
		ASSERT_EQ (system.wallet (0)->store.end (), iterator2);
		ASSERT_FALSE (new_account.is_zero ());
	}
	ASSERT_TIMELY (10s, node1.balance (new_account) != 0);
	system.stop ();
	runner.join ();
}

TEST (ledger, representation_changes)
{
	ysu::keypair key1;
	ysu::rep_weights rep_weights;
	ASSERT_EQ (0, rep_weights.representation_get (key1.pub));
	rep_weights.representation_put (key1.pub, 1);
	ASSERT_EQ (1, rep_weights.representation_get (key1.pub));
	rep_weights.representation_put (key1.pub, 2);
	ASSERT_EQ (2, rep_weights.representation_get (key1.pub));
}

TEST (ledger, representation)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	auto & rep_weights = ledger.cache.rep_weights;
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ASSERT_EQ (ysu::genesis_amount, rep_weights.representation_get (ysu::dev_genesis_key.pub));
	ysu::keypair key2;
	ysu::send_block block1 (genesis.hash (), key2.pub, ysu::genesis_amount - 100, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block1).code);
	ASSERT_EQ (ysu::genesis_amount - 100, rep_weights.representation_get (ysu::dev_genesis_key.pub));
	ysu::keypair key3;
	ysu::open_block block2 (block1.hash (), key3.pub, key2.pub, key2.prv, key2.pub, *pool.generate (key2.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block2).code);
	ASSERT_EQ (ysu::genesis_amount - 100, rep_weights.representation_get (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key2.pub));
	ASSERT_EQ (100, rep_weights.representation_get (key3.pub));
	ysu::send_block block3 (block1.hash (), key2.pub, ysu::genesis_amount - 200, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block3).code);
	ASSERT_EQ (ysu::genesis_amount - 200, rep_weights.representation_get (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key2.pub));
	ASSERT_EQ (100, rep_weights.representation_get (key3.pub));
	ysu::receive_block block4 (block2.hash (), block3.hash (), key2.prv, key2.pub, *pool.generate (block2.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block4).code);
	ASSERT_EQ (ysu::genesis_amount - 200, rep_weights.representation_get (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key2.pub));
	ASSERT_EQ (200, rep_weights.representation_get (key3.pub));
	ysu::keypair key4;
	ysu::change_block block5 (block4.hash (), key4.pub, key2.prv, key2.pub, *pool.generate (block4.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block5).code);
	ASSERT_EQ (ysu::genesis_amount - 200, rep_weights.representation_get (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key2.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key3.pub));
	ASSERT_EQ (200, rep_weights.representation_get (key4.pub));
	ysu::keypair key5;
	ysu::send_block block6 (block5.hash (), key5.pub, 100, key2.prv, key2.pub, *pool.generate (block5.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block6).code);
	ASSERT_EQ (ysu::genesis_amount - 200, rep_weights.representation_get (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key2.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key3.pub));
	ASSERT_EQ (100, rep_weights.representation_get (key4.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key5.pub));
	ysu::keypair key6;
	ysu::open_block block7 (block6.hash (), key6.pub, key5.pub, key5.prv, key5.pub, *pool.generate (key5.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block7).code);
	ASSERT_EQ (ysu::genesis_amount - 200, rep_weights.representation_get (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key2.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key3.pub));
	ASSERT_EQ (100, rep_weights.representation_get (key4.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key5.pub));
	ASSERT_EQ (100, rep_weights.representation_get (key6.pub));
	ysu::send_block block8 (block6.hash (), key5.pub, 0, key2.prv, key2.pub, *pool.generate (block6.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block8).code);
	ASSERT_EQ (ysu::genesis_amount - 200, rep_weights.representation_get (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key2.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key3.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key4.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key5.pub));
	ASSERT_EQ (100, rep_weights.representation_get (key6.pub));
	ysu::receive_block block9 (block7.hash (), block8.hash (), key5.prv, key5.pub, *pool.generate (block7.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block9).code);
	ASSERT_EQ (ysu::genesis_amount - 200, rep_weights.representation_get (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key2.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key3.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key4.pub));
	ASSERT_EQ (0, rep_weights.representation_get (key5.pub));
	ASSERT_EQ (200, rep_weights.representation_get (key6.pub));
}

TEST (ledger, double_open)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key2;
	ysu::send_block send1 (genesis.hash (), key2.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::open_block open1 (send1.hash (), key2.pub, key2.pub, key2.prv, key2.pub, *pool.generate (key2.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open1).code);
	ysu::open_block open2 (send1.hash (), ysu::dev_genesis_key.pub, key2.pub, key2.prv, key2.pub, *pool.generate (key2.pub));
	ASSERT_EQ (ysu::process_result::fork, ledger.process (transaction, open2).code);
}

TEST (ledger, double_receive)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key2;
	ysu::send_block send1 (genesis.hash (), key2.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::open_block open1 (send1.hash (), key2.pub, key2.pub, key2.prv, key2.pub, *pool.generate (key2.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open1).code);
	ysu::receive_block receive1 (open1.hash (), send1.hash (), key2.prv, key2.pub, *pool.generate (open1.hash ()));
	ASSERT_EQ (ysu::process_result::unreceivable, ledger.process (transaction, receive1).code);
}

TEST (votes, check_signature)
{
	ysu::system system;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.online_weight_minimum = std::numeric_limits<ysu::uint128_t>::max ();
	auto & node1 = *system.add_node (node_config);
	ysu::genesis genesis;
	ysu::keypair key1;
	auto send1 (std::make_shared<ysu::send_block> (genesis.hash (), key1.pub, ysu::genesis_amount - 100, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	{
		auto transaction (node1.store.tx_begin_write ());
		ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, *send1).code);
	}
	auto election1 = node1.active.insert (send1);
	ASSERT_EQ (1, election1.election->votes ().size ());
	auto vote1 (std::make_shared<ysu::vote> (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, 1, send1));
	vote1->signature.bytes[0] ^= 1;
	ASSERT_EQ (ysu::vote_code::invalid, node1.vote_processor.vote_blocking (vote1, std::make_shared<ysu::transport::channel_loopback> (node1)));
	vote1->signature.bytes[0] ^= 1;
	ASSERT_EQ (ysu::vote_code::vote, node1.vote_processor.vote_blocking (vote1, std::make_shared<ysu::transport::channel_loopback> (node1)));
	ASSERT_EQ (ysu::vote_code::replay, node1.vote_processor.vote_blocking (vote1, std::make_shared<ysu::transport::channel_loopback> (node1)));
}

TEST (votes, add_one)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::genesis genesis;
	ysu::keypair key1;
	auto send1 (std::make_shared<ysu::send_block> (genesis.hash (), key1.pub, ysu::genesis_amount - 100, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	auto transaction (node1.store.tx_begin_write ());
	ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, *send1).code);
	auto election1 = node1.active.insert (send1);
	ASSERT_EQ (1, election1.election->votes ().size ());
	auto vote1 (std::make_shared<ysu::vote> (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, 1, send1));
	ASSERT_EQ (ysu::vote_code::vote, node1.active.vote (vote1));
	auto vote2 (std::make_shared<ysu::vote> (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, 2, send1));
	ASSERT_EQ (ysu::vote_code::vote, node1.active.vote (vote2));
	ASSERT_EQ (2, election1.election->votes ().size ());
	auto votes1 (election1.election->votes ());
	auto existing1 (votes1.find (ysu::dev_genesis_key.pub));
	ASSERT_NE (votes1.end (), existing1);
	ASSERT_EQ (send1->hash (), existing1->second.hash);
	ysu::lock_guard<std::mutex> guard (node1.active.mutex);
	auto winner (*election1.election->tally ().begin ());
	ASSERT_EQ (*send1, *winner.second);
	ASSERT_EQ (ysu::genesis_amount - 100, winner.first);
}

TEST (votes, add_two)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::genesis genesis;
	ysu::keypair key1;
	auto send1 (std::make_shared<ysu::send_block> (genesis.hash (), key1.pub, ysu::genesis_amount - 100, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	auto transaction (node1.store.tx_begin_write ());
	ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, *send1).code);
	auto election1 = node1.active.insert (send1);
	ysu::keypair key2;
	auto send2 (std::make_shared<ysu::send_block> (genesis.hash (), key2.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	auto vote2 (std::make_shared<ysu::vote> (key2.pub, key2.prv, 1, send2));
	ASSERT_EQ (ysu::vote_code::vote, node1.active.vote (vote2));
	auto vote1 (std::make_shared<ysu::vote> (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, 1, send1));
	ASSERT_EQ (ysu::vote_code::vote, node1.active.vote (vote1));
	ASSERT_EQ (3, election1.election->votes ().size ());
	auto votes1 (election1.election->votes ());
	ASSERT_NE (votes1.end (), votes1.find (ysu::dev_genesis_key.pub));
	ASSERT_EQ (send1->hash (), votes1[ysu::dev_genesis_key.pub].hash);
	ASSERT_NE (votes1.end (), votes1.find (key2.pub));
	ASSERT_EQ (send2->hash (), votes1[key2.pub].hash);
	ASSERT_EQ (*send1, *election1.election->winner ());
}

namespace ysu
{
// Higher sequence numbers change the vote
TEST (votes, add_existing)
{
	ysu::system system;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.online_weight_minimum = std::numeric_limits<ysu::uint128_t>::max ();
	node_config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	auto & node1 = *system.add_node (node_config);
	ysu::genesis genesis;
	ysu::keypair key1;
	auto send1 (std::make_shared<ysu::send_block> (genesis.hash (), key1.pub, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	{
		auto transaction (node1.store.tx_begin_write ());
		ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, *send1).code);
	}
	auto election1 = node1.active.insert (send1);
	auto vote1 (std::make_shared<ysu::vote> (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, 1, send1));
	ASSERT_EQ (ysu::vote_code::vote, node1.active.vote (vote1));
	// Block is already processed from vote
	ASSERT_TRUE (node1.active.publish (send1));
	ASSERT_EQ (1, election1.election->votes ()[ysu::dev_genesis_key.pub].sequence);
	ysu::keypair key2;
	auto send2 (std::make_shared<ysu::send_block> (genesis.hash (), key2.pub, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send2);
	auto vote2 (std::make_shared<ysu::vote> (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, 2, send2));
	// Pretend we've waited the timeout
	ysu::unique_lock<std::mutex> lock (node1.active.mutex);
	election1.election->last_votes[ysu::dev_genesis_key.pub].time = std::chrono::steady_clock::now () - std::chrono::seconds (20);
	lock.unlock ();
	ASSERT_EQ (ysu::vote_code::vote, node1.active.vote (vote2));
	ASSERT_FALSE (node1.active.publish (send2));
	ASSERT_EQ (2, election1.election->votes ()[ysu::dev_genesis_key.pub].sequence);
	// Also resend the old vote, and see if we respect the sequence number
	lock.lock ();
	election1.election->last_votes[ysu::dev_genesis_key.pub].time = std::chrono::steady_clock::now () - std::chrono::seconds (20);
	lock.unlock ();
	ASSERT_EQ (ysu::vote_code::replay, node1.active.vote (vote1));
	ASSERT_EQ (2, election1.election->votes ()[ysu::dev_genesis_key.pub].sequence);
	auto votes (election1.election->votes ());
	ASSERT_EQ (2, votes.size ());
	ASSERT_NE (votes.end (), votes.find (ysu::dev_genesis_key.pub));
	ASSERT_EQ (send2->hash (), votes[ysu::dev_genesis_key.pub].hash);
	lock.lock ();
	ASSERT_EQ (*send2, *election1.election->tally ().begin ()->second);
}

// Lower sequence numbers are ignored
TEST (votes, add_old)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::genesis genesis;
	ysu::keypair key1;
	auto send1 (std::make_shared<ysu::send_block> (genesis.hash (), key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	auto transaction (node1.store.tx_begin_write ());
	ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, *send1).code);
	auto election1 = node1.active.insert (send1);
	auto vote1 (std::make_shared<ysu::vote> (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, 2, send1));
	auto channel (std::make_shared<ysu::transport::channel_loopback> (node1));
	node1.vote_processor.vote_blocking (vote1, channel);
	ysu::keypair key2;
	auto send2 (std::make_shared<ysu::send_block> (genesis.hash (), key2.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send2);
	auto vote2 (std::make_shared<ysu::vote> (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, 1, send2));
	{
		ysu::lock_guard<std::mutex> lock (node1.active.mutex);
		election1.election->last_votes[ysu::dev_genesis_key.pub].time = std::chrono::steady_clock::now () - std::chrono::seconds (20);
	}
	node1.vote_processor.vote_blocking (vote2, channel);
	ASSERT_EQ (2, election1.election->votes ().size ());
	auto votes (election1.election->votes ());
	ASSERT_NE (votes.end (), votes.find (ysu::dev_genesis_key.pub));
	ASSERT_EQ (send1->hash (), votes[ysu::dev_genesis_key.pub].hash);
	ASSERT_EQ (*send1, *election1.election->winner ());
}
}

// Lower sequence numbers are accepted for different accounts
TEST (votes, add_old_different_account)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::genesis genesis;
	ysu::keypair key1;
	auto send1 (std::make_shared<ysu::send_block> (genesis.hash (), key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	auto send2 (std::make_shared<ysu::send_block> (send1->hash (), key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send2);
	ASSERT_EQ (ysu::process_result::progress, node1.process (*send1).code);
	ASSERT_EQ (ysu::process_result::progress, node1.process (*send2).code);
	ysu::blocks_confirm (node1, { send1, send2 });
	auto election1 = node1.active.election (send1->qualified_root ());
	ASSERT_NE (nullptr, election1);
	auto election2 = node1.active.election (send2->qualified_root ());
	ASSERT_NE (nullptr, election2);
	ASSERT_EQ (1, election1->votes ().size ());
	ASSERT_EQ (1, election2->votes ().size ());
	auto vote1 (std::make_shared<ysu::vote> (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, 2, send1));
	auto channel (std::make_shared<ysu::transport::channel_loopback> (node1));
	auto vote_result1 (node1.vote_processor.vote_blocking (vote1, channel));
	ASSERT_EQ (ysu::vote_code::vote, vote_result1);
	ASSERT_EQ (2, election1->votes ().size ());
	ASSERT_EQ (1, election2->votes ().size ());
	auto vote2 (std::make_shared<ysu::vote> (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, 1, send2));
	auto vote_result2 (node1.vote_processor.vote_blocking (vote2, channel));
	ASSERT_EQ (ysu::vote_code::vote, vote_result2);
	ASSERT_EQ (2, election1->votes ().size ());
	ASSERT_EQ (2, election2->votes ().size ());
	auto votes1 (election1->votes ());
	auto votes2 (election2->votes ());
	ASSERT_NE (votes1.end (), votes1.find (ysu::dev_genesis_key.pub));
	ASSERT_NE (votes2.end (), votes2.find (ysu::dev_genesis_key.pub));
	ASSERT_EQ (send1->hash (), votes1[ysu::dev_genesis_key.pub].hash);
	ASSERT_EQ (send2->hash (), votes2[ysu::dev_genesis_key.pub].hash);
	ASSERT_EQ (*send1, *election1->winner ());
	ASSERT_EQ (*send2, *election2->winner ());
}

// The voting cooldown is respected
TEST (votes, add_cooldown)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::genesis genesis;
	ysu::keypair key1;
	auto send1 (std::make_shared<ysu::send_block> (genesis.hash (), key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	auto transaction (node1.store.tx_begin_write ());
	ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, *send1).code);
	auto election1 = node1.active.insert (send1);
	auto vote1 (std::make_shared<ysu::vote> (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, 1, send1));
	auto channel (std::make_shared<ysu::transport::channel_loopback> (node1));
	node1.vote_processor.vote_blocking (vote1, channel);
	ysu::keypair key2;
	auto send2 (std::make_shared<ysu::send_block> (genesis.hash (), key2.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send2);
	auto vote2 (std::make_shared<ysu::vote> (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, 2, send2));
	node1.vote_processor.vote_blocking (vote2, channel);
	ASSERT_EQ (2, election1.election->votes ().size ());
	auto votes (election1.election->votes ());
	ASSERT_NE (votes.end (), votes.find (ysu::dev_genesis_key.pub));
	ASSERT_EQ (send1->hash (), votes[ysu::dev_genesis_key.pub].hash);
	ASSERT_EQ (*send1, *election1.election->winner ());
}

// Query for block successor
TEST (ledger, successor)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::keypair key1;
	ysu::genesis genesis;
	ysu::send_block send1 (genesis.hash (), key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0);
	node1.work_generate_blocking (send1);
	auto transaction (node1.store.tx_begin_write ());
	ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, send1).code);
	ASSERT_EQ (send1, *node1.ledger.successor (transaction, ysu::qualified_root (genesis.hash (), ysu::root (0))));
	ASSERT_EQ (*genesis.open, *node1.ledger.successor (transaction, genesis.open->qualified_root ()));
	ASSERT_EQ (nullptr, node1.ledger.successor (transaction, ysu::qualified_root (0)));
}

TEST (ledger, fail_change_old)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::change_block block (genesis.hash (), key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	auto result1 (ledger.process (transaction, block));
	ASSERT_EQ (ysu::process_result::progress, result1.code);
	auto result2 (ledger.process (transaction, block));
	ASSERT_EQ (ysu::process_result::old, result2.code);
}

TEST (ledger, fail_change_gap_previous)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::change_block block (1, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (ysu::root (1)));
	auto result1 (ledger.process (transaction, block));
	ASSERT_EQ (ysu::process_result::gap_previous, result1.code);
}

TEST (ledger, fail_change_bad_signature)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::change_block block (genesis.hash (), key1.pub, ysu::keypair ().prv, 0, *pool.generate (genesis.hash ()));
	auto result1 (ledger.process (transaction, block));
	ASSERT_EQ (ysu::process_result::bad_signature, result1.code);
}

TEST (ledger, fail_change_fork)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::change_block block1 (genesis.hash (), key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (ysu::process_result::progress, result1.code);
	ysu::keypair key2;
	ysu::change_block block2 (genesis.hash (), key2.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (ysu::process_result::fork, result2.code);
}

TEST (ledger, fail_send_old)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	auto result1 (ledger.process (transaction, block));
	ASSERT_EQ (ysu::process_result::progress, result1.code);
	auto result2 (ledger.process (transaction, block));
	ASSERT_EQ (ysu::process_result::old, result2.code);
}

TEST (ledger, fail_send_gap_previous)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block (1, key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (ysu::root (1)));
	auto result1 (ledger.process (transaction, block));
	ASSERT_EQ (ysu::process_result::gap_previous, result1.code);
}

TEST (ledger, fail_send_bad_signature)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block (genesis.hash (), key1.pub, 1, ysu::keypair ().prv, 0, *pool.generate (genesis.hash ()));
	auto result1 (ledger.process (transaction, block));
	ASSERT_EQ (ysu::process_result::bad_signature, result1.code);
}

TEST (ledger, fail_send_negative_spend)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block1).code);
	ysu::keypair key2;
	ysu::send_block block2 (block1.hash (), key2.pub, 2, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block1.hash ()));
	ASSERT_EQ (ysu::process_result::negative_spend, ledger.process (transaction, block2).code);
}

TEST (ledger, fail_send_fork)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block1).code);
	ysu::keypair key2;
	ysu::send_block block2 (genesis.hash (), key2.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::fork, ledger.process (transaction, block2).code);
}

TEST (ledger, fail_open_old)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block1).code);
	ysu::open_block block2 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block2).code);
	ASSERT_EQ (ysu::process_result::old, ledger.process (transaction, block2).code);
}

TEST (ledger, fail_open_gap_source)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::open_block block2 (1, 1, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (ysu::process_result::gap_source, result2.code);
}

TEST (ledger, fail_open_bad_signature)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block1).code);
	ysu::open_block block2 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	block2.signature.clear ();
	ASSERT_EQ (ysu::process_result::bad_signature, ledger.process (transaction, block2).code);
}

TEST (ledger, fail_open_fork_previous)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block1).code);
	ysu::send_block block2 (block1.hash (), key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block2).code);
	ysu::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block3).code);
	ysu::open_block block4 (block2.hash (), 1, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	ASSERT_EQ (ysu::process_result::fork, ledger.process (transaction, block4).code);
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
}

TEST (ledger, fail_open_account_mismatch)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block1).code);
	ysu::keypair badkey;
	ysu::open_block block2 (block1.hash (), 1, badkey.pub, badkey.prv, badkey.pub, *pool.generate (badkey.pub));
	ASSERT_NE (ysu::process_result::progress, ledger.process (transaction, block2).code);
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
}

TEST (ledger, fail_receive_old)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block1).code);
	ysu::send_block block2 (block1.hash (), key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block2).code);
	ysu::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block3).code);
	ysu::receive_block block4 (block3.hash (), block2.hash (), key1.prv, key1.pub, *pool.generate (block3.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block4).code);
	ASSERT_EQ (ysu::process_result::old, ledger.process (transaction, block4).code);
}

TEST (ledger, fail_receive_gap_source)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (ysu::process_result::progress, result1.code);
	ysu::send_block block2 (block1.hash (), key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block1.hash ()));
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (ysu::process_result::progress, result2.code);
	ysu::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	auto result3 (ledger.process (transaction, block3));
	ASSERT_EQ (ysu::process_result::progress, result3.code);
	ysu::receive_block block4 (block3.hash (), 1, key1.prv, key1.pub, *pool.generate (block3.hash ()));
	auto result4 (ledger.process (transaction, block4));
	ASSERT_EQ (ysu::process_result::gap_source, result4.code);
}

TEST (ledger, fail_receive_overreceive)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (ysu::process_result::progress, result1.code);
	ysu::open_block block2 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	auto result3 (ledger.process (transaction, block2));
	ASSERT_EQ (ysu::process_result::progress, result3.code);
	ysu::receive_block block3 (block2.hash (), block1.hash (), key1.prv, key1.pub, *pool.generate (block2.hash ()));
	auto result4 (ledger.process (transaction, block3));
	ASSERT_EQ (ysu::process_result::unreceivable, result4.code);
}

TEST (ledger, fail_receive_bad_signature)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (ysu::process_result::progress, result1.code);
	ysu::send_block block2 (block1.hash (), key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block1.hash ()));
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (ysu::process_result::progress, result2.code);
	ysu::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	auto result3 (ledger.process (transaction, block3));
	ASSERT_EQ (ysu::process_result::progress, result3.code);
	ysu::receive_block block4 (block3.hash (), block2.hash (), ysu::keypair ().prv, 0, *pool.generate (block3.hash ()));
	auto result4 (ledger.process (transaction, block4));
	ASSERT_EQ (ysu::process_result::bad_signature, result4.code);
}

TEST (ledger, fail_receive_gap_previous_opened)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (ysu::process_result::progress, result1.code);
	ysu::send_block block2 (block1.hash (), key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block1.hash ()));
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (ysu::process_result::progress, result2.code);
	ysu::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	auto result3 (ledger.process (transaction, block3));
	ASSERT_EQ (ysu::process_result::progress, result3.code);
	ysu::receive_block block4 (1, block2.hash (), key1.prv, key1.pub, *pool.generate (ysu::root (1)));
	auto result4 (ledger.process (transaction, block4));
	ASSERT_EQ (ysu::process_result::gap_previous, result4.code);
}

TEST (ledger, fail_receive_gap_previous_unopened)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (ysu::process_result::progress, result1.code);
	ysu::send_block block2 (block1.hash (), key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block1.hash ()));
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (ysu::process_result::progress, result2.code);
	ysu::receive_block block3 (1, block2.hash (), key1.prv, key1.pub, *pool.generate (ysu::root (1)));
	auto result3 (ledger.process (transaction, block3));
	ASSERT_EQ (ysu::process_result::gap_previous, result3.code);
}

TEST (ledger, fail_receive_fork_previous)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (ysu::process_result::progress, result1.code);
	ysu::send_block block2 (block1.hash (), key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block1.hash ()));
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (ysu::process_result::progress, result2.code);
	ysu::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	auto result3 (ledger.process (transaction, block3));
	ASSERT_EQ (ysu::process_result::progress, result3.code);
	ysu::keypair key2;
	ysu::send_block block4 (block3.hash (), key1.pub, 1, key1.prv, key1.pub, *pool.generate (block3.hash ()));
	auto result4 (ledger.process (transaction, block4));
	ASSERT_EQ (ysu::process_result::progress, result4.code);
	ysu::receive_block block5 (block3.hash (), block2.hash (), key1.prv, key1.pub, *pool.generate (block3.hash ()));
	auto result5 (ledger.process (transaction, block5));
	ASSERT_EQ (ysu::process_result::fork, result5.code);
}

TEST (ledger, fail_receive_received_source)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::send_block block1 (genesis.hash (), key1.pub, 2, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	auto result1 (ledger.process (transaction, block1));
	ASSERT_EQ (ysu::process_result::progress, result1.code);
	ysu::send_block block2 (block1.hash (), key1.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block1.hash ()));
	auto result2 (ledger.process (transaction, block2));
	ASSERT_EQ (ysu::process_result::progress, result2.code);
	ysu::send_block block6 (block2.hash (), key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block2.hash ()));
	auto result6 (ledger.process (transaction, block6));
	ASSERT_EQ (ysu::process_result::progress, result6.code);
	ysu::open_block block3 (block1.hash (), 1, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	auto result3 (ledger.process (transaction, block3));
	ASSERT_EQ (ysu::process_result::progress, result3.code);
	ysu::keypair key2;
	ysu::send_block block4 (block3.hash (), key1.pub, 1, key1.prv, key1.pub, *pool.generate (block3.hash ()));
	auto result4 (ledger.process (transaction, block4));
	ASSERT_EQ (ysu::process_result::progress, result4.code);
	ysu::receive_block block5 (block4.hash (), block2.hash (), key1.prv, key1.pub, *pool.generate (block4.hash ()));
	auto result5 (ledger.process (transaction, block5));
	ASSERT_EQ (ysu::process_result::progress, result5.code);
	ysu::receive_block block7 (block3.hash (), block2.hash (), key1.prv, key1.pub, *pool.generate (block3.hash ()));
	auto result7 (ledger.process (transaction, block7));
	ASSERT_EQ (ysu::process_result::fork, result7.code);
}

TEST (ledger, latest_empty)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::keypair key;
	auto transaction (store->tx_begin_read ());
	auto latest (ledger.latest (transaction, key.pub));
	ASSERT_TRUE (latest.is_zero ());
}

TEST (ledger, latest_root)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key;
	ASSERT_EQ (key.pub, ledger.latest_root (transaction, key.pub));
	auto hash1 (ledger.latest (transaction, ysu::dev_genesis_key.pub));
	ysu::send_block send (hash1, 0, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (hash1));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send).code);
	ASSERT_EQ (send.hash (), ledger.latest_root (transaction, ysu::dev_genesis_key.pub));
}

TEST (ledger, change_representative_move_representation)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::keypair key1;
	auto transaction (store->tx_begin_write ());
	ysu::genesis genesis;
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	auto hash1 (genesis.hash ());
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::dev_genesis_key.pub));
	ysu::send_block send (hash1, key1.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (hash1));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send).code);
	ASSERT_EQ (0, ledger.weight (ysu::dev_genesis_key.pub));
	ysu::keypair key2;
	ysu::change_block change (send.hash (), key2.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, change).code);
	ysu::keypair key3;
	ysu::open_block open (send.hash (), key3.pub, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open).code);
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (key3.pub));
}

TEST (ledger, send_open_receive_rollback)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	auto transaction (store->tx_begin_write ());
	ysu::genesis genesis;
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::account_info info1;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info1));
	ysu::keypair key1;
	ysu::send_block send1 (info1.head, key1.pub, ysu::genesis_amount - 50, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (info1.head));
	auto return1 (ledger.process (transaction, send1));
	ASSERT_EQ (ysu::process_result::progress, return1.code);
	ysu::send_block send2 (send1.hash (), key1.pub, ysu::genesis_amount - 100, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	auto return2 (ledger.process (transaction, send2));
	ASSERT_EQ (ysu::process_result::progress, return2.code);
	ysu::keypair key2;
	ysu::open_block open (send2.hash (), key2.pub, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	auto return4 (ledger.process (transaction, open));
	ASSERT_EQ (ysu::process_result::progress, return4.code);
	ysu::receive_block receive (open.hash (), send1.hash (), key1.prv, key1.pub, *pool.generate (open.hash ()));
	auto return5 (ledger.process (transaction, receive));
	ASSERT_EQ (ysu::process_result::progress, return5.code);
	ysu::keypair key3;
	ASSERT_EQ (100, ledger.weight (key2.pub));
	ASSERT_EQ (ysu::genesis_amount - 100, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, ledger.weight (key3.pub));
	ysu::change_block change1 (send2.hash (), key3.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send2.hash ()));
	auto return6 (ledger.process (transaction, change1));
	ASSERT_EQ (ysu::process_result::progress, return6.code);
	ASSERT_EQ (100, ledger.weight (key2.pub));
	ASSERT_EQ (0, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_EQ (ysu::genesis_amount - 100, ledger.weight (key3.pub));
	ASSERT_FALSE (ledger.rollback (transaction, receive.hash ()));
	ASSERT_EQ (50, ledger.weight (key2.pub));
	ASSERT_EQ (0, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_EQ (ysu::genesis_amount - 100, ledger.weight (key3.pub));
	ASSERT_FALSE (ledger.rollback (transaction, open.hash ()));
	ASSERT_EQ (0, ledger.weight (key2.pub));
	ASSERT_EQ (0, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_EQ (ysu::genesis_amount - 100, ledger.weight (key3.pub));
	ASSERT_FALSE (ledger.rollback (transaction, change1.hash ()));
	ASSERT_EQ (0, ledger.weight (key2.pub));
	ASSERT_EQ (0, ledger.weight (key3.pub));
	ASSERT_EQ (ysu::genesis_amount - 100, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_FALSE (ledger.rollback (transaction, send2.hash ()));
	ASSERT_EQ (0, ledger.weight (key2.pub));
	ASSERT_EQ (0, ledger.weight (key3.pub));
	ASSERT_EQ (ysu::genesis_amount - 50, ledger.weight (ysu::dev_genesis_key.pub));
	ASSERT_FALSE (ledger.rollback (transaction, send1.hash ()));
	ASSERT_EQ (0, ledger.weight (key2.pub));
	ASSERT_EQ (0, ledger.weight (key3.pub));
	ASSERT_EQ (ysu::genesis_amount - 0, ledger.weight (ysu::dev_genesis_key.pub));
}

TEST (ledger, bootstrap_rep_weight)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::account_info info1;
	ysu::keypair key2;
	ysu::genesis genesis;
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	{
		auto transaction (store->tx_begin_write ());
		store->initialize (transaction, genesis, ledger.cache);
		ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info1));
		ysu::send_block send (info1.head, key2.pub, std::numeric_limits<ysu::uint128_t>::max () - 50, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (info1.head));
		ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send).code);
	}
	ASSERT_EQ (2, ledger.cache.block_count);
	{
		ledger.bootstrap_weight_max_blocks = 3;
		ledger.bootstrap_weights[key2.pub] = 1000;
		ASSERT_EQ (1000, ledger.weight (key2.pub));
	}
	{
		auto transaction (store->tx_begin_write ());
		ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, info1));
		ysu::send_block send (info1.head, key2.pub, std::numeric_limits<ysu::uint128_t>::max () - 100, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (info1.head));
		ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send).code);
	}
	ASSERT_EQ (3, ledger.cache.block_count);
	{
		auto transaction (store->tx_begin_read ());
		ASSERT_EQ (0, ledger.weight (key2.pub));
	}
}

TEST (ledger, block_destination_source)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair dest;
	ysu::uint128_t balance (ysu::genesis_amount);
	balance -= ysu::Gxrb_ratio;
	ysu::send_block block1 (genesis.hash (), dest.pub, balance, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	balance -= ysu::Gxrb_ratio;
	ysu::send_block block2 (block1.hash (), ysu::genesis_account, balance, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block1.hash ()));
	balance += ysu::Gxrb_ratio;
	ysu::receive_block block3 (block2.hash (), block2.hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block2.hash ()));
	balance -= ysu::Gxrb_ratio;
	ysu::state_block block4 (ysu::genesis_account, block3.hash (), ysu::genesis_account, balance, dest.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block3.hash ()));
	balance -= ysu::Gxrb_ratio;
	ysu::state_block block5 (ysu::genesis_account, block4.hash (), ysu::genesis_account, balance, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block4.hash ()));
	balance += ysu::Gxrb_ratio;
	ysu::state_block block6 (ysu::genesis_account, block5.hash (), ysu::genesis_account, balance, block5.hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (block5.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block1).code);
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block2).code);
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block3).code);
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block4).code);
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block5).code);
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, block6).code);
	ASSERT_EQ (balance, ledger.balance (transaction, block6.hash ()));
	ASSERT_EQ (dest.pub, ledger.block_destination (transaction, block1));
	ASSERT_TRUE (ledger.block_source (transaction, block1).is_zero ());
	ASSERT_EQ (ysu::genesis_account, ledger.block_destination (transaction, block2));
	ASSERT_TRUE (ledger.block_source (transaction, block2).is_zero ());
	ASSERT_TRUE (ledger.block_destination (transaction, block3).is_zero ());
	ASSERT_EQ (block2.hash (), ledger.block_source (transaction, block3));
	ASSERT_EQ (dest.pub, ledger.block_destination (transaction, block4));
	ASSERT_TRUE (ledger.block_source (transaction, block4).is_zero ());
	ASSERT_EQ (ysu::genesis_account, ledger.block_destination (transaction, block5));
	ASSERT_TRUE (ledger.block_source (transaction, block5).is_zero ());
	ASSERT_TRUE (ledger.block_destination (transaction, block6).is_zero ());
	ASSERT_EQ (block5.hash (), ledger.block_source (transaction, block6));
}

TEST (ledger, state_account)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_EQ (ysu::genesis_account, ledger.account (transaction, send1.hash ()));
}

TEST (ledger, state_send_receive)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (store->block_exists (transaction, send1.hash ()));
	auto send2 (store->block_get (transaction, send1.hash ()));
	ASSERT_NE (nullptr, send2);
	ASSERT_EQ (send1, *send2);
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.balance (transaction, send1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, send1.hash ()));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (ysu::genesis_account));
	ASSERT_TRUE (store->pending_exists (transaction, ysu::pending_key (ysu::genesis_account, send1.hash ())));
	ASSERT_EQ (2, send2->sideband ().height);
	ASSERT_TRUE (send2->sideband ().details.is_send);
	ASSERT_FALSE (send2->sideband ().details.is_receive);
	ASSERT_FALSE (send2->sideband ().details.is_epoch);
	ysu::state_block receive1 (ysu::genesis_account, send1.hash (), ysu::genesis_account, ysu::genesis_amount, send1.hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive1).code);
	ASSERT_TRUE (store->block_exists (transaction, receive1.hash ()));
	auto receive2 (store->block_get (transaction, receive1.hash ()));
	ASSERT_NE (nullptr, receive2);
	ASSERT_EQ (receive1, *receive2);
	ASSERT_EQ (ysu::genesis_amount, ledger.balance (transaction, receive1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, receive1.hash ()));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::genesis_account));
	ASSERT_FALSE (store->pending_exists (transaction, ysu::pending_key (ysu::genesis_account, send1.hash ())));
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
	ASSERT_EQ (3, receive2->sideband ().height);
	ASSERT_FALSE (receive2->sideband ().details.is_send);
	ASSERT_TRUE (receive2->sideband ().details.is_receive);
	ASSERT_FALSE (receive2->sideband ().details.is_epoch);
}

TEST (ledger, state_receive)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::send_block send1 (genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (store->block_exists (transaction, send1.hash ()));
	auto send2 (store->block_get (transaction, send1.hash ()));
	ASSERT_NE (nullptr, send2);
	ASSERT_EQ (send1, *send2);
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.balance (transaction, send1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, send1.hash ()));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (ysu::genesis_account));
	ysu::state_block receive1 (ysu::genesis_account, send1.hash (), ysu::genesis_account, ysu::genesis_amount, send1.hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive1).code);
	ASSERT_TRUE (store->block_exists (transaction, receive1.hash ()));
	auto receive2 (store->block_get (transaction, receive1.hash ()));
	ASSERT_NE (nullptr, receive2);
	ASSERT_EQ (receive1, *receive2);
	ASSERT_EQ (ysu::genesis_amount, ledger.balance (transaction, receive1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, receive1.hash ()));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::genesis_account));
	ASSERT_EQ (3, receive2->sideband ().height);
	ASSERT_FALSE (receive2->sideband ().details.is_send);
	ASSERT_TRUE (receive2->sideband ().details.is_receive);
	ASSERT_FALSE (receive2->sideband ().details.is_epoch);
}

TEST (ledger, state_rep_change)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair rep;
	ysu::state_block change1 (ysu::genesis_account, genesis.hash (), rep.pub, ysu::genesis_amount, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, change1).code);
	ASSERT_TRUE (store->block_exists (transaction, change1.hash ()));
	auto change2 (store->block_get (transaction, change1.hash ()));
	ASSERT_NE (nullptr, change2);
	ASSERT_EQ (change1, *change2);
	ASSERT_EQ (ysu::genesis_amount, ledger.balance (transaction, change1.hash ()));
	ASSERT_EQ (0, ledger.amount (transaction, change1.hash ()));
	ASSERT_EQ (0, ledger.weight (ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (rep.pub));
	ASSERT_EQ (2, change2->sideband ().height);
	ASSERT_FALSE (change2->sideband ().details.is_send);
	ASSERT_FALSE (change2->sideband ().details.is_receive);
	ASSERT_FALSE (change2->sideband ().details.is_epoch);
}

TEST (ledger, state_open)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair destination;
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (store->block_exists (transaction, send1.hash ()));
	auto send2 (store->block_get (transaction, send1.hash ()));
	ASSERT_NE (nullptr, send2);
	ASSERT_EQ (send1, *send2);
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.balance (transaction, send1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, send1.hash ()));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (ysu::genesis_account));
	ASSERT_TRUE (store->pending_exists (transaction, ysu::pending_key (destination.pub, send1.hash ())));
	ysu::state_block open1 (destination.pub, 0, ysu::genesis_account, ysu::Gxrb_ratio, send1.hash (), destination.prv, destination.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open1).code);
	ASSERT_FALSE (store->pending_exists (transaction, ysu::pending_key (destination.pub, send1.hash ())));
	ASSERT_TRUE (store->block_exists (transaction, open1.hash ()));
	auto open2 (store->block_get (transaction, open1.hash ()));
	ASSERT_NE (nullptr, open2);
	ASSERT_EQ (open1, *open2);
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.balance (transaction, open1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, open1.hash ()));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::genesis_account));
	ASSERT_EQ (ledger.cache.account_count, store->account_count (transaction));
	ASSERT_EQ (1, open2->sideband ().height);
	ASSERT_FALSE (open2->sideband ().details.is_send);
	ASSERT_TRUE (open2->sideband ().details.is_receive);
	ASSERT_FALSE (open2->sideband ().details.is_epoch);
}

// Make sure old block types can't be inserted after a state block.
TEST (ledger, send_after_state_fail)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::send_block send2 (send1.hash (), ysu::genesis_account, ysu::genesis_amount - (2 * ysu::Gxrb_ratio), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::block_position, ledger.process (transaction, send2).code);
}

// Make sure old block types can't be inserted after a state block.
TEST (ledger, receive_after_state_fail)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::receive_block receive1 (send1.hash (), send1.hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::block_position, ledger.process (transaction, receive1).code);
}

// Make sure old block types can't be inserted after a state block.
TEST (ledger, change_after_state_fail)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::keypair rep;
	ysu::change_block change1 (send1.hash (), rep.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::block_position, ledger.process (transaction, change1).code);
}

TEST (ledger, state_unreceivable_fail)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::send_block send1 (genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (store->block_exists (transaction, send1.hash ()));
	auto send2 (store->block_get (transaction, send1.hash ()));
	ASSERT_NE (nullptr, send2);
	ASSERT_EQ (send1, *send2);
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.balance (transaction, send1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, send1.hash ()));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (ysu::genesis_account));
	ysu::state_block receive1 (ysu::genesis_account, send1.hash (), ysu::genesis_account, ysu::genesis_amount, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::gap_source, ledger.process (transaction, receive1).code);
}

TEST (ledger, state_receive_bad_amount_fail)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::send_block send1 (genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (store->block_exists (transaction, send1.hash ()));
	auto send2 (store->block_get (transaction, send1.hash ()));
	ASSERT_NE (nullptr, send2);
	ASSERT_EQ (send1, *send2);
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.balance (transaction, send1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, send1.hash ()));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (ysu::genesis_account));
	ysu::state_block receive1 (ysu::genesis_account, send1.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, send1.hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::balance_mismatch, ledger.process (transaction, receive1).code);
}

TEST (ledger, state_no_link_amount_fail)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::keypair rep;
	ysu::state_block change1 (ysu::genesis_account, send1.hash (), rep.pub, ysu::genesis_amount, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::balance_mismatch, ledger.process (transaction, change1).code);
}

TEST (ledger, state_receive_wrong_account_fail)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (store->block_exists (transaction, send1.hash ()));
	auto send2 (store->block_get (transaction, send1.hash ()));
	ASSERT_NE (nullptr, send2);
	ASSERT_EQ (send1, *send2);
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.balance (transaction, send1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, send1.hash ()));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (ysu::genesis_account));
	ysu::keypair key;
	ysu::state_block receive1 (key.pub, 0, ysu::genesis_account, ysu::Gxrb_ratio, send1.hash (), key.prv, key.pub, *pool.generate (key.pub));
	ASSERT_EQ (ysu::process_result::unreceivable, ledger.process (transaction, receive1).code);
}

TEST (ledger, state_open_state_fork)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair destination;
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::state_block open1 (destination.pub, 0, ysu::genesis_account, ysu::Gxrb_ratio, send1.hash (), destination.prv, destination.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open1).code);
	ysu::open_block open2 (send1.hash (), ysu::genesis_account, destination.pub, destination.prv, destination.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::fork, ledger.process (transaction, open2).code);
	ASSERT_EQ (open1.root (), open2.root ());
}

TEST (ledger, state_state_open_fork)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair destination;
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::open_block open1 (send1.hash (), ysu::genesis_account, destination.pub, destination.prv, destination.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open1).code);
	ysu::state_block open2 (destination.pub, 0, ysu::genesis_account, ysu::Gxrb_ratio, send1.hash (), destination.prv, destination.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::fork, ledger.process (transaction, open2).code);
	ASSERT_EQ (open1.root (), open2.root ());
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
}

TEST (ledger, state_open_previous_fail)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair destination;
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::state_block open1 (destination.pub, 1, ysu::genesis_account, ysu::Gxrb_ratio, send1.hash (), destination.prv, destination.pub, *pool.generate (1));
	ASSERT_EQ (ysu::process_result::gap_previous, ledger.process (transaction, open1).code);
}

TEST (ledger, state_open_source_fail)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair destination;
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::state_block open1 (destination.pub, 0, ysu::genesis_account, 0, 0, destination.prv, destination.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::gap_source, ledger.process (transaction, open1).code);
}

TEST (ledger, state_send_change)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair rep;
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), rep.pub, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (store->block_exists (transaction, send1.hash ()));
	auto send2 (store->block_get (transaction, send1.hash ()));
	ASSERT_NE (nullptr, send2);
	ASSERT_EQ (send1, *send2);
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.balance (transaction, send1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, send1.hash ()));
	ASSERT_EQ (0, ledger.weight (ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (rep.pub));
	ASSERT_EQ (2, send2->sideband ().height);
	ASSERT_TRUE (send2->sideband ().details.is_send);
	ASSERT_FALSE (send2->sideband ().details.is_receive);
	ASSERT_FALSE (send2->sideband ().details.is_epoch);
}

TEST (ledger, state_receive_change)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (store->block_exists (transaction, send1.hash ()));
	auto send2 (store->block_get (transaction, send1.hash ()));
	ASSERT_NE (nullptr, send2);
	ASSERT_EQ (send1, *send2);
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.balance (transaction, send1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, send1.hash ()));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (ysu::genesis_account));
	ysu::keypair rep;
	ysu::state_block receive1 (ysu::genesis_account, send1.hash (), rep.pub, ysu::genesis_amount, send1.hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive1).code);
	ASSERT_TRUE (store->block_exists (transaction, receive1.hash ()));
	auto receive2 (store->block_get (transaction, receive1.hash ()));
	ASSERT_NE (nullptr, receive2);
	ASSERT_EQ (receive1, *receive2);
	ASSERT_EQ (ysu::genesis_amount, ledger.balance (transaction, receive1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, receive1.hash ()));
	ASSERT_EQ (0, ledger.weight (ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (rep.pub));
	ASSERT_EQ (3, receive2->sideband ().height);
	ASSERT_FALSE (receive2->sideband ().details.is_send);
	ASSERT_TRUE (receive2->sideband ().details.is_receive);
	ASSERT_FALSE (receive2->sideband ().details.is_epoch);
}

TEST (ledger, state_open_old)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair destination;
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::open_block open1 (send1.hash (), ysu::genesis_account, destination.pub, destination.prv, destination.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open1).code);
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.balance (transaction, open1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, open1.hash ()));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::genesis_account));
}

TEST (ledger, state_receive_old)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair destination;
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::state_block send2 (ysu::genesis_account, send1.hash (), ysu::genesis_account, ysu::genesis_amount - (2 * ysu::Gxrb_ratio), destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send2).code);
	ysu::open_block open1 (send1.hash (), ysu::genesis_account, destination.pub, destination.prv, destination.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open1).code);
	ysu::receive_block receive1 (open1.hash (), send2.hash (), destination.prv, destination.pub, *pool.generate (open1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive1).code);
	ASSERT_EQ (2 * ysu::Gxrb_ratio, ledger.balance (transaction, receive1.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, receive1.hash ()));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::genesis_account));
}

TEST (ledger, state_rollback_send)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (store->block_exists (transaction, send1.hash ()));
	auto send2 (store->block_get (transaction, send1.hash ()));
	ASSERT_NE (nullptr, send2);
	ASSERT_EQ (send1, *send2);
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.account_balance (transaction, ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (ysu::genesis_account));
	ysu::pending_info info;
	ASSERT_FALSE (store->pending_get (transaction, ysu::pending_key (ysu::genesis_account, send1.hash ()), info));
	ASSERT_EQ (ysu::genesis_account, info.source);
	ASSERT_EQ (ysu::Gxrb_ratio, info.amount.number ());
	ASSERT_FALSE (ledger.rollback (transaction, send1.hash ()));
	ASSERT_FALSE (store->block_exists (transaction, send1.hash ()));
	ASSERT_EQ (ysu::genesis_amount, ledger.account_balance (transaction, ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::genesis_account));
	ASSERT_FALSE (store->pending_exists (transaction, ysu::pending_key (ysu::genesis_account, send1.hash ())));
	ASSERT_TRUE (store->block_successor (transaction, genesis.hash ()).is_zero ());
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
}

TEST (ledger, state_rollback_receive)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::state_block receive1 (ysu::genesis_account, send1.hash (), ysu::genesis_account, ysu::genesis_amount, send1.hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive1).code);
	ASSERT_FALSE (store->pending_exists (transaction, ysu::pending_key (ysu::genesis_account, receive1.hash ())));
	ASSERT_FALSE (ledger.rollback (transaction, receive1.hash ()));
	ysu::pending_info info;
	ASSERT_FALSE (store->pending_get (transaction, ysu::pending_key (ysu::genesis_account, send1.hash ()), info));
	ASSERT_EQ (ysu::genesis_account, info.source);
	ASSERT_EQ (ysu::Gxrb_ratio, info.amount.number ());
	ASSERT_FALSE (store->block_exists (transaction, receive1.hash ()));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.account_balance (transaction, ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (ysu::genesis_account));
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
}

TEST (ledger, state_rollback_received_send)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key;
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, key.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::state_block receive1 (key.pub, 0, key.pub, ysu::Gxrb_ratio, send1.hash (), key.prv, key.pub, *pool.generate (key.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive1).code);
	ASSERT_FALSE (store->pending_exists (transaction, ysu::pending_key (ysu::genesis_account, receive1.hash ())));
	ASSERT_FALSE (ledger.rollback (transaction, send1.hash ()));
	ASSERT_FALSE (store->pending_exists (transaction, ysu::pending_key (ysu::genesis_account, send1.hash ())));
	ASSERT_FALSE (store->block_exists (transaction, send1.hash ()));
	ASSERT_FALSE (store->block_exists (transaction, receive1.hash ()));
	ASSERT_EQ (ysu::genesis_amount, ledger.account_balance (transaction, ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::genesis_account));
	ASSERT_EQ (0, ledger.account_balance (transaction, key.pub));
	ASSERT_EQ (0, ledger.weight (key.pub));
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
}

TEST (ledger, state_rep_change_rollback)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair rep;
	ysu::state_block change1 (ysu::genesis_account, genesis.hash (), rep.pub, ysu::genesis_amount, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, change1).code);
	ASSERT_FALSE (ledger.rollback (transaction, change1.hash ()));
	ASSERT_FALSE (store->block_exists (transaction, change1.hash ()));
	ASSERT_EQ (ysu::genesis_amount, ledger.account_balance (transaction, ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::genesis_account));
	ASSERT_EQ (0, ledger.weight (rep.pub));
}

TEST (ledger, state_open_rollback)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair destination;
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::state_block open1 (destination.pub, 0, ysu::genesis_account, ysu::Gxrb_ratio, send1.hash (), destination.prv, destination.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open1).code);
	ASSERT_FALSE (ledger.rollback (transaction, open1.hash ()));
	ASSERT_FALSE (store->block_exists (transaction, open1.hash ()));
	ASSERT_EQ (0, ledger.account_balance (transaction, destination.pub));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (ysu::genesis_account));
	ysu::pending_info info;
	ASSERT_FALSE (store->pending_get (transaction, ysu::pending_key (destination.pub, send1.hash ()), info));
	ASSERT_EQ (ysu::genesis_account, info.source);
	ASSERT_EQ (ysu::Gxrb_ratio, info.amount.number ());
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
}

TEST (ledger, state_send_change_rollback)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair rep;
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), rep.pub, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_FALSE (ledger.rollback (transaction, send1.hash ()));
	ASSERT_FALSE (store->block_exists (transaction, send1.hash ()));
	ASSERT_EQ (ysu::genesis_amount, ledger.account_balance (transaction, ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount, ledger.weight (ysu::genesis_account));
	ASSERT_EQ (0, ledger.weight (rep.pub));
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
}

TEST (ledger, state_receive_change_rollback)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::keypair rep;
	ysu::state_block receive1 (ysu::genesis_account, send1.hash (), rep.pub, ysu::genesis_amount, send1.hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive1).code);
	ASSERT_FALSE (ledger.rollback (transaction, receive1.hash ()));
	ASSERT_FALSE (store->block_exists (transaction, receive1.hash ()));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.account_balance (transaction, ysu::genesis_account));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (ysu::genesis_account));
	ASSERT_EQ (0, ledger.weight (rep.pub));
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
}

TEST (ledger, epoch_blocks_v1_general)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair destination;
	ysu::state_block epoch1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount, ledger.epoch_link (ysu::epoch::epoch_1), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, epoch1).code);
	ASSERT_FALSE (epoch1.sideband ().details.is_send);
	ASSERT_FALSE (epoch1.sideband ().details.is_receive);
	ASSERT_TRUE (epoch1.sideband ().details.is_epoch);
	ASSERT_EQ (ysu::epoch::epoch_1, epoch1.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, epoch1.sideband ().source_epoch); // Not used for epoch blocks
	ysu::state_block epoch2 (ysu::genesis_account, epoch1.hash (), ysu::genesis_account, ysu::genesis_amount, ledger.epoch_link (ysu::epoch::epoch_1), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (epoch1.hash ()));
	ASSERT_EQ (ysu::process_result::block_position, ledger.process (transaction, epoch2).code);
	ysu::account_info genesis_info;
	ASSERT_FALSE (ledger.store.account_get (transaction, ysu::genesis_account, genesis_info));
	ASSERT_EQ (genesis_info.epoch (), ysu::epoch::epoch_1);
	ASSERT_FALSE (ledger.rollback (transaction, epoch1.hash ()));
	ASSERT_FALSE (ledger.store.account_get (transaction, ysu::genesis_account, genesis_info));
	ASSERT_EQ (genesis_info.epoch (), ysu::epoch::epoch_0);
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, epoch1).code);
	ASSERT_FALSE (ledger.store.account_get (transaction, ysu::genesis_account, genesis_info));
	ASSERT_EQ (genesis_info.epoch (), ysu::epoch::epoch_1);
	ASSERT_FALSE (epoch1.sideband ().details.is_send);
	ASSERT_FALSE (epoch1.sideband ().details.is_receive);
	ASSERT_TRUE (epoch1.sideband ().details.is_epoch);
	ASSERT_EQ (ysu::epoch::epoch_1, epoch1.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, epoch1.sideband ().source_epoch); // Not used for epoch blocks
	ysu::change_block change1 (epoch1.hash (), ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (epoch1.hash ()));
	ASSERT_EQ (ysu::process_result::block_position, ledger.process (transaction, change1).code);
	ysu::state_block send1 (ysu::genesis_account, epoch1.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (epoch1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (send1.sideband ().details.is_send);
	ASSERT_FALSE (send1.sideband ().details.is_receive);
	ASSERT_FALSE (send1.sideband ().details.is_epoch);
	ASSERT_EQ (ysu::epoch::epoch_1, send1.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, send1.sideband ().source_epoch); // Not used for send blocks
	ysu::open_block open1 (send1.hash (), ysu::genesis_account, destination.pub, destination.prv, destination.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::unreceivable, ledger.process (transaction, open1).code);
	ysu::state_block epoch3 (destination.pub, 0, ysu::genesis_account, 0, ledger.epoch_link (ysu::epoch::epoch_1), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::representative_mismatch, ledger.process (transaction, epoch3).code);
	ysu::state_block epoch4 (destination.pub, 0, 0, 0, ledger.epoch_link (ysu::epoch::epoch_1), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, epoch4).code);
	ASSERT_FALSE (epoch4.sideband ().details.is_send);
	ASSERT_FALSE (epoch4.sideband ().details.is_receive);
	ASSERT_TRUE (epoch4.sideband ().details.is_epoch);
	ASSERT_EQ (ysu::epoch::epoch_1, epoch4.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, epoch4.sideband ().source_epoch); // Not used for epoch blocks
	ysu::receive_block receive1 (epoch4.hash (), send1.hash (), destination.prv, destination.pub, *pool.generate (epoch4.hash ()));
	ASSERT_EQ (ysu::process_result::block_position, ledger.process (transaction, receive1).code);
	ysu::state_block receive2 (destination.pub, epoch4.hash (), destination.pub, ysu::Gxrb_ratio, send1.hash (), destination.prv, destination.pub, *pool.generate (epoch4.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive2).code);
	ASSERT_EQ (ysu::epoch::epoch_1, receive2.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_1, receive2.sideband ().source_epoch);
	ASSERT_EQ (0, ledger.balance (transaction, epoch4.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.balance (transaction, receive2.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, receive2.hash ()));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (ysu::genesis_account));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.weight (destination.pub));
	ASSERT_FALSE (receive2.sideband ().details.is_send);
	ASSERT_TRUE (receive2.sideband ().details.is_receive);
	ASSERT_FALSE (receive2.sideband ().details.is_epoch);
}

TEST (ledger, epoch_blocks_v2_general)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair destination;
	ysu::state_block epoch1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount, ledger.epoch_link (ysu::epoch::epoch_2), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	// Trying to upgrade from epoch 0 to epoch 2. It is a requirement epoch upgrades are sequential unless the account is unopened
	ASSERT_EQ (ysu::process_result::block_position, ledger.process (transaction, epoch1).code);
	// Set it to the first epoch and it should now succeed
	epoch1 = ysu::state_block (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount, ledger.epoch_link (ysu::epoch::epoch_1), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, epoch1.work);
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, epoch1).code);
	ASSERT_EQ (ysu::epoch::epoch_1, epoch1.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, epoch1.sideband ().source_epoch); // Not used for epoch blocks
	ysu::state_block epoch2 (ysu::genesis_account, epoch1.hash (), ysu::genesis_account, ysu::genesis_amount, ledger.epoch_link (ysu::epoch::epoch_2), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (epoch1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, epoch2).code);
	ASSERT_EQ (ysu::epoch::epoch_2, epoch2.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, epoch2.sideband ().source_epoch); // Not used for epoch blocks
	ysu::state_block epoch3 (ysu::genesis_account, epoch2.hash (), ysu::genesis_account, ysu::genesis_amount, ledger.epoch_link (ysu::epoch::epoch_2), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (epoch2.hash ()));
	ASSERT_EQ (ysu::process_result::block_position, ledger.process (transaction, epoch3).code);
	ysu::account_info genesis_info;
	ASSERT_FALSE (ledger.store.account_get (transaction, ysu::genesis_account, genesis_info));
	ASSERT_EQ (genesis_info.epoch (), ysu::epoch::epoch_2);
	ASSERT_FALSE (ledger.rollback (transaction, epoch1.hash ()));
	ASSERT_FALSE (ledger.store.account_get (transaction, ysu::genesis_account, genesis_info));
	ASSERT_EQ (genesis_info.epoch (), ysu::epoch::epoch_0);
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, epoch1).code);
	ASSERT_FALSE (ledger.store.account_get (transaction, ysu::genesis_account, genesis_info));
	ASSERT_EQ (genesis_info.epoch (), ysu::epoch::epoch_1);
	ysu::change_block change1 (epoch1.hash (), ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (epoch1.hash ()));
	ASSERT_EQ (ysu::process_result::block_position, ledger.process (transaction, change1).code);
	ysu::state_block send1 (ysu::genesis_account, epoch1.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (epoch1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_EQ (ysu::epoch::epoch_1, send1.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, send1.sideband ().source_epoch); // Not used for send blocks
	ysu::open_block open1 (send1.hash (), ysu::genesis_account, destination.pub, destination.prv, destination.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::unreceivable, ledger.process (transaction, open1).code);
	ysu::state_block epoch4 (destination.pub, 0, 0, 0, ledger.epoch_link (ysu::epoch::epoch_1), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, epoch4).code);
	ASSERT_EQ (ysu::epoch::epoch_1, epoch4.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, epoch4.sideband ().source_epoch); // Not used for epoch blocks
	ysu::state_block epoch5 (destination.pub, epoch4.hash (), ysu::genesis_account, 0, ledger.epoch_link (ysu::epoch::epoch_2), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (epoch4.hash ()));
	ASSERT_EQ (ysu::process_result::representative_mismatch, ledger.process (transaction, epoch5).code);
	ysu::state_block epoch6 (destination.pub, epoch4.hash (), 0, 0, ledger.epoch_link (ysu::epoch::epoch_2), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (epoch4.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, epoch6).code);
	ASSERT_EQ (ysu::epoch::epoch_2, epoch6.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, epoch6.sideband ().source_epoch); // Not used for epoch blocks
	ysu::receive_block receive1 (epoch6.hash (), send1.hash (), destination.prv, destination.pub, *pool.generate (epoch6.hash ()));
	ASSERT_EQ (ysu::process_result::block_position, ledger.process (transaction, receive1).code);
	ysu::state_block receive2 (destination.pub, epoch6.hash (), destination.pub, ysu::Gxrb_ratio, send1.hash (), destination.prv, destination.pub, *pool.generate (epoch6.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive2).code);
	ASSERT_EQ (ysu::epoch::epoch_2, receive2.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_1, receive2.sideband ().source_epoch);
	ASSERT_EQ (0, ledger.balance (transaction, epoch6.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.balance (transaction, receive2.hash ()));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.amount (transaction, receive2.hash ()));
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio, ledger.weight (ysu::genesis_account));
	ASSERT_EQ (ysu::Gxrb_ratio, ledger.weight (destination.pub));
}

TEST (ledger, epoch_blocks_receive_upgrade)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair destination;
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::state_block epoch1 (ysu::genesis_account, send1.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ledger.epoch_link (ysu::epoch::epoch_1), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, epoch1).code);
	ysu::state_block send2 (ysu::genesis_account, epoch1.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio * 2, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (epoch1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send2).code);
	ASSERT_EQ (ysu::epoch::epoch_1, send2.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, send2.sideband ().source_epoch); // Not used for send blocks
	ysu::open_block open1 (send1.hash (), destination.pub, destination.pub, destination.prv, destination.pub, *pool.generate (destination.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open1).code);
	ASSERT_EQ (ysu::epoch::epoch_0, open1.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, open1.sideband ().source_epoch);
	ysu::receive_block receive1 (open1.hash (), send2.hash (), destination.prv, destination.pub, *pool.generate (open1.hash ()));
	ASSERT_EQ (ysu::process_result::unreceivable, ledger.process (transaction, receive1).code);
	ysu::state_block receive2 (destination.pub, open1.hash (), destination.pub, ysu::Gxrb_ratio * 2, send2.hash (), destination.prv, destination.pub, *pool.generate (open1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive2).code);
	ASSERT_EQ (ysu::epoch::epoch_1, receive2.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_1, receive2.sideband ().source_epoch);
	ysu::account_info destination_info;
	ASSERT_FALSE (ledger.store.account_get (transaction, destination.pub, destination_info));
	ASSERT_EQ (destination_info.epoch (), ysu::epoch::epoch_1);
	ASSERT_FALSE (ledger.rollback (transaction, receive2.hash ()));
	ASSERT_FALSE (ledger.store.account_get (transaction, destination.pub, destination_info));
	ASSERT_EQ (destination_info.epoch (), ysu::epoch::epoch_0);
	ysu::pending_info pending_send2;
	ASSERT_FALSE (ledger.store.pending_get (transaction, ysu::pending_key (destination.pub, send2.hash ()), pending_send2));
	ASSERT_EQ (ysu::dev_genesis_key.pub, pending_send2.source);
	ASSERT_EQ (ysu::Gxrb_ratio, pending_send2.amount.number ());
	ASSERT_EQ (ysu::epoch::epoch_1, pending_send2.epoch);
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive2).code);
	ASSERT_EQ (ysu::epoch::epoch_1, receive2.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_1, receive2.sideband ().source_epoch);
	ASSERT_FALSE (ledger.store.account_get (transaction, destination.pub, destination_info));
	ASSERT_EQ (destination_info.epoch (), ysu::epoch::epoch_1);
	ysu::keypair destination2;
	ysu::state_block send3 (destination.pub, receive2.hash (), destination.pub, ysu::Gxrb_ratio, destination2.pub, destination.prv, destination.pub, *pool.generate (receive2.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send3).code);
	ysu::open_block open2 (send3.hash (), destination2.pub, destination2.pub, destination2.prv, destination2.pub, *pool.generate (destination2.pub));
	ASSERT_EQ (ysu::process_result::unreceivable, ledger.process (transaction, open2).code);
	// Upgrade to epoch 2 and send to destination. Try to create an open block from an epoch 2 source block.
	ysu::keypair destination3;
	ysu::state_block epoch2 (ysu::genesis_account, send2.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio * 2, ledger.epoch_link (ysu::epoch::epoch_2), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send2.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, epoch2).code);
	ysu::state_block send4 (ysu::genesis_account, epoch2.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio * 3, destination3.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (epoch2.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send4).code);
	ysu::open_block open3 (send4.hash (), destination3.pub, destination3.pub, destination3.prv, destination3.pub, *pool.generate (destination3.pub));
	ASSERT_EQ (ysu::process_result::unreceivable, ledger.process (transaction, open3).code);
	// Send it to an epoch 1 account
	ysu::state_block send5 (ysu::genesis_account, send4.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio * 4, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send4.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send5).code);
	ASSERT_FALSE (ledger.store.account_get (transaction, destination.pub, destination_info));
	ASSERT_EQ (destination_info.epoch (), ysu::epoch::epoch_1);
	ysu::state_block receive3 (destination.pub, send3.hash (), destination.pub, ysu::Gxrb_ratio * 2, send5.hash (), destination.prv, destination.pub, *pool.generate (send3.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive3).code);
	ASSERT_EQ (ysu::epoch::epoch_2, receive3.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_2, receive3.sideband ().source_epoch);
	ASSERT_FALSE (ledger.store.account_get (transaction, destination.pub, destination_info));
	ASSERT_EQ (destination_info.epoch (), ysu::epoch::epoch_2);
	// Upgrade an unopened account straight to epoch 2
	ysu::keypair destination4;
	ysu::state_block send6 (ysu::genesis_account, send5.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio * 5, destination4.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send5.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send6).code);
	ysu::state_block epoch4 (destination4.pub, 0, 0, 0, ledger.epoch_link (ysu::epoch::epoch_2), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (destination4.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, epoch4).code);
	ASSERT_EQ (ysu::epoch::epoch_2, epoch4.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, epoch4.sideband ().source_epoch); // Not used for epoch blocks
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
}

TEST (ledger, epoch_blocks_fork)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair destination;
	ysu::send_block send1 (genesis.hash (), ysu::account (0), ysu::genesis_amount, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ysu::state_block epoch1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount, ledger.epoch_link (ysu::epoch::epoch_1), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::fork, ledger.process (transaction, epoch1).code);
	ysu::state_block epoch2 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount, ledger.epoch_link (ysu::epoch::epoch_2), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::fork, ledger.process (transaction, epoch2).code);
	ysu::state_block epoch3 (ysu::genesis_account, send1.hash (), ysu::genesis_account, ysu::genesis_amount, ledger.epoch_link (ysu::epoch::epoch_1), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, epoch3).code);
	ASSERT_EQ (ysu::epoch::epoch_1, epoch3.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, epoch3.sideband ().source_epoch); // Not used for epoch state blocks
	ysu::state_block epoch4 (ysu::genesis_account, send1.hash (), ysu::genesis_account, ysu::genesis_amount, ledger.epoch_link (ysu::epoch::epoch_2), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::fork, ledger.process (transaction, epoch2).code);
}

TEST (ledger, successor_epoch)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::keypair key1;
	ysu::genesis genesis;
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::send_block send1 (genesis.hash (), key1.pub, ysu::genesis_amount - 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ysu::state_block open (key1.pub, 0, key1.pub, 1, send1.hash (), key1.prv, key1.pub, *pool.generate (key1.pub));
	ysu::state_block change (key1.pub, open.hash (), key1.pub, 1, 0, key1.prv, key1.pub, *pool.generate (open.hash ()));
	auto open_hash = open.hash ();
	ysu::send_block send2 (send1.hash (), reinterpret_cast<ysu::account const &> (open_hash), ysu::genesis_amount - 2, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ysu::state_block epoch_open (reinterpret_cast<ysu::account const &> (open_hash), 0, 0, 0, node1.ledger.epoch_link (ysu::epoch::epoch_1), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (open.hash ()));
	auto transaction (node1.store.tx_begin_write ());
	ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, send1).code);
	ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, open).code);
	ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, change).code);
	ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, send2).code);
	ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, epoch_open).code);
	ASSERT_EQ (change, *node1.ledger.successor (transaction, change.qualified_root ()));
	ASSERT_EQ (epoch_open, *node1.ledger.successor (transaction, epoch_open.qualified_root ()));
	ASSERT_EQ (ysu::epoch::epoch_1, epoch_open.sideband ().details.epoch);
	ASSERT_EQ (ysu::epoch::epoch_0, epoch_open.sideband ().source_epoch); // Not used for epoch state blocks
}

TEST (ledger, epoch_open_pending)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	ysu::state_block epoch_open (key1.pub, 0, 0, 0, node1.ledger.epoch_link (ysu::epoch::epoch_1), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (key1.pub));
	auto transaction (node1.store.tx_begin_write ());
	ASSERT_EQ (ysu::process_result::block_position, node1.ledger.process (transaction, epoch_open).code);
}

TEST (ledger, block_hash_account_conflict)
{
	ysu::block_builder builder;
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::genesis genesis;
	ysu::keypair key1;
	ysu::keypair key2;
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());

	/*
	 * Generate a send block whose destination is a block hash already
	 * in the ledger and not an account
	 */
	auto send1 = builder.state ()
	             .account (ysu::genesis_account)
	             .previous (genesis.hash ())
	             .representative (ysu::genesis_account)
	             .balance (ysu::genesis_amount - 100)
	             .link (key1.pub)
	             .sign (ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub)
	             .work (*pool.generate (genesis.hash ()))
	             .build_shared ();

	auto receive1 = builder.state ()
	                .account (key1.pub)
	                .previous (0)
	                .representative (ysu::genesis_account)
	                .balance (100)
	                .link (send1->hash ())
	                .sign (key1.prv, key1.pub)
	                .work (*pool.generate (key1.pub))
	                .build_shared ();

	/*
	 * Note that the below link is a block hash when this is intended
	 * to represent a send state block. This can generally never be
	 * received , except by epoch blocks, which can sign an open block
	 * for arbitrary accounts.
	 */
	auto send2 = builder.state ()
	             .account (key1.pub)
	             .previous (receive1->hash ())
	             .representative (ysu::genesis_account)
	             .balance (90)
	             .link (receive1->hash ())
	             .sign (key1.prv, key1.pub)
	             .work (*pool.generate (receive1->hash ()))
	             .build_shared ();

	/*
	 * Generate an epoch open for the account with the same value as the block hash
	 */
	auto receive1_hash = receive1->hash ();
	auto open_epoch1 = builder.state ()
	                   .account (reinterpret_cast<ysu::account const &> (receive1_hash))
	                   .previous (0)
	                   .representative (0)
	                   .balance (0)
	                   .link (node1.ledger.epoch_link (ysu::epoch::epoch_1))
	                   .sign (ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub)
	                   .work (*pool.generate (receive1->hash ()))
	                   .build_shared ();

	node1.work_generate_blocking (*send1);
	node1.work_generate_blocking (*receive1);
	node1.work_generate_blocking (*send2);
	node1.work_generate_blocking (*open_epoch1);
	ASSERT_EQ (ysu::process_result::progress, node1.process (*send1).code);
	ASSERT_EQ (ysu::process_result::progress, node1.process (*receive1).code);
	ASSERT_EQ (ysu::process_result::progress, node1.process (*send2).code);
	ASSERT_EQ (ysu::process_result::progress, node1.process (*open_epoch1).code);
	ysu::blocks_confirm (node1, { send1, receive1, send2, open_epoch1 });
	auto election1 = node1.active.election (send1->qualified_root ());
	ASSERT_NE (nullptr, election1);
	auto election2 = node1.active.election (receive1->qualified_root ());
	ASSERT_NE (nullptr, election2);
	auto election3 = node1.active.election (send2->qualified_root ());
	ASSERT_NE (nullptr, election3);
	auto election4 = node1.active.election (open_epoch1->qualified_root ());
	ASSERT_NE (nullptr, election4);
	auto winner1 (election1->winner ());
	auto winner2 (election2->winner ());
	auto winner3 (election3->winner ());
	auto winner4 (election4->winner ());
	ASSERT_EQ (*send1, *winner1);
	ASSERT_EQ (*receive1, *winner2);
	ASSERT_EQ (*send2, *winner3);
	ASSERT_EQ (*open_epoch1, *winner4);
}

TEST (ledger, could_fit)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair destination;
	// Test legacy and state change blocks could_fit
	ysu::change_block change1 (genesis.hash (), ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ysu::state_block change2 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_TRUE (ledger.could_fit (transaction, change1));
	ASSERT_TRUE (ledger.could_fit (transaction, change2));
	// Test legacy and state send
	ysu::keypair key1;
	ysu::send_block send1 (change1.hash (), key1.pub, ysu::genesis_amount - 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (change1.hash ()));
	ysu::state_block send2 (ysu::genesis_account, change1.hash (), ysu::genesis_account, ysu::genesis_amount - 1, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (change1.hash ()));
	ASSERT_FALSE (ledger.could_fit (transaction, send1));
	ASSERT_FALSE (ledger.could_fit (transaction, send2));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, change1).code);
	ASSERT_TRUE (ledger.could_fit (transaction, change1));
	ASSERT_TRUE (ledger.could_fit (transaction, change2));
	ASSERT_TRUE (ledger.could_fit (transaction, send1));
	ASSERT_TRUE (ledger.could_fit (transaction, send2));
	// Test legacy and state open
	ysu::open_block open1 (send2.hash (), ysu::genesis_account, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	ysu::state_block open2 (key1.pub, 0, ysu::genesis_account, 1, send2.hash (), key1.prv, key1.pub, *pool.generate (key1.pub));
	ASSERT_FALSE (ledger.could_fit (transaction, open1));
	ASSERT_FALSE (ledger.could_fit (transaction, open2));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send2).code);
	ASSERT_TRUE (ledger.could_fit (transaction, send1));
	ASSERT_TRUE (ledger.could_fit (transaction, send2));
	ASSERT_TRUE (ledger.could_fit (transaction, open1));
	ASSERT_TRUE (ledger.could_fit (transaction, open2));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open1).code);
	ASSERT_TRUE (ledger.could_fit (transaction, open1));
	ASSERT_TRUE (ledger.could_fit (transaction, open2));
	// Create another send to receive
	ysu::state_block send3 (ysu::genesis_account, send2.hash (), ysu::genesis_account, ysu::genesis_amount - 2, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send2.hash ()));
	// Test legacy and state receive
	ysu::receive_block receive1 (open1.hash (), send3.hash (), key1.prv, key1.pub, *pool.generate (open1.hash ()));
	ysu::state_block receive2 (key1.pub, open1.hash (), ysu::genesis_account, 2, send3.hash (), key1.prv, key1.pub, *pool.generate (open1.hash ()));
	ASSERT_FALSE (ledger.could_fit (transaction, receive1));
	ASSERT_FALSE (ledger.could_fit (transaction, receive2));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send3).code);
	ASSERT_TRUE (ledger.could_fit (transaction, receive1));
	ASSERT_TRUE (ledger.could_fit (transaction, receive2));
	// Test epoch (state)
	ysu::state_block epoch1 (key1.pub, receive1.hash (), ysu::genesis_account, 2, ledger.epoch_link (ysu::epoch::epoch_1), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (receive1.hash ()));
	ASSERT_FALSE (ledger.could_fit (transaction, epoch1));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive1).code);
	ASSERT_TRUE (ledger.could_fit (transaction, receive1));
	ASSERT_TRUE (ledger.could_fit (transaction, receive2));
	ASSERT_TRUE (ledger.could_fit (transaction, epoch1));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, epoch1).code);
	ASSERT_TRUE (ledger.could_fit (transaction, epoch1));
}

TEST (ledger, unchecked_epoch)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::genesis genesis;
	ysu::keypair destination;
	auto send1 (std::make_shared<ysu::state_block> (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	auto open1 (std::make_shared<ysu::state_block> (destination.pub, 0, destination.pub, ysu::Gxrb_ratio, send1->hash (), destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*open1);
	auto epoch1 (std::make_shared<ysu::state_block> (destination.pub, open1->hash (), destination.pub, ysu::Gxrb_ratio, node1.ledger.epoch_link (ysu::epoch::epoch_1), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*epoch1);
	node1.block_processor.add (epoch1);
	node1.block_processor.flush ();
	{
		auto transaction (node1.store.tx_begin_read ());
		auto unchecked_count (node1.store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 1);
		ASSERT_EQ (unchecked_count, node1.store.unchecked_count (transaction));
		auto blocks (node1.store.unchecked_get (transaction, epoch1->previous ()));
		ASSERT_EQ (blocks.size (), 1);
		ASSERT_EQ (blocks[0].verified, ysu::signature_verification::valid_epoch);
	}
	node1.block_processor.add (send1);
	node1.block_processor.add (open1);
	node1.block_processor.flush ();
	{
		auto transaction (node1.store.tx_begin_read ());
		ASSERT_TRUE (node1.store.block_exists (transaction, epoch1->hash ()));
		auto unchecked_count (node1.store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 0);
		ASSERT_EQ (unchecked_count, node1.store.unchecked_count (transaction));
		ysu::account_info info;
		ASSERT_FALSE (node1.store.account_get (transaction, destination.pub, info));
		ASSERT_EQ (info.epoch (), ysu::epoch::epoch_1);
	}
}

TEST (ledger, unchecked_epoch_invalid)
{
	ysu::system system;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	auto & node1 (*system.add_node (node_config));
	ysu::genesis genesis;
	ysu::keypair destination;
	auto send1 (std::make_shared<ysu::state_block> (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	auto open1 (std::make_shared<ysu::state_block> (destination.pub, 0, destination.pub, ysu::Gxrb_ratio, send1->hash (), destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*open1);
	// Epoch block with account own signature
	auto epoch1 (std::make_shared<ysu::state_block> (destination.pub, open1->hash (), destination.pub, ysu::Gxrb_ratio, node1.ledger.epoch_link (ysu::epoch::epoch_1), destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*epoch1);
	// Pseudo epoch block (send subtype, destination - epoch link)
	auto epoch2 (std::make_shared<ysu::state_block> (destination.pub, open1->hash (), destination.pub, ysu::Gxrb_ratio - 1, node1.ledger.epoch_link (ysu::epoch::epoch_1), destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*epoch2);
	node1.block_processor.add (epoch1);
	node1.block_processor.add (epoch2);
	node1.block_processor.flush ();
	{
		auto transaction (node1.store.tx_begin_read ());
		auto unchecked_count (node1.store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 2);
		ASSERT_EQ (unchecked_count, node1.store.unchecked_count (transaction));
		auto blocks (node1.store.unchecked_get (transaction, epoch1->previous ()));
		ASSERT_EQ (blocks.size (), 2);
		ASSERT_EQ (blocks[0].verified, ysu::signature_verification::valid);
		ASSERT_EQ (blocks[1].verified, ysu::signature_verification::valid);
	}
	node1.block_processor.add (send1);
	node1.block_processor.add (open1);
	node1.block_processor.flush ();
	{
		auto transaction (node1.store.tx_begin_read ());
		ASSERT_FALSE (node1.store.block_exists (transaction, epoch1->hash ()));
		ASSERT_TRUE (node1.store.block_exists (transaction, epoch2->hash ()));
		ASSERT_TRUE (node1.active.empty ());
		auto unchecked_count (node1.store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 0);
		ASSERT_EQ (unchecked_count, node1.store.unchecked_count (transaction));
		ysu::account_info info;
		ASSERT_FALSE (node1.store.account_get (transaction, destination.pub, info));
		ASSERT_NE (info.epoch (), ysu::epoch::epoch_1);
		auto epoch2_store (node1.store.block_get (transaction, epoch2->hash ()));
		ASSERT_NE (nullptr, epoch2_store);
		ASSERT_EQ (ysu::epoch::epoch_0, epoch2_store->sideband ().details.epoch);
		ASSERT_TRUE (epoch2_store->sideband ().details.is_send);
		ASSERT_FALSE (epoch2_store->sideband ().details.is_epoch);
		ASSERT_FALSE (epoch2_store->sideband ().details.is_receive);
	}
}

TEST (ledger, unchecked_open)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::genesis genesis;
	ysu::keypair destination;
	auto send1 (std::make_shared<ysu::state_block> (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	auto open1 (std::make_shared<ysu::open_block> (send1->hash (), destination.pub, destination.pub, destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*open1);
	// Invalid signature for open block
	auto open2 (std::make_shared<ysu::open_block> (send1->hash (), ysu::dev_genesis_key.pub, destination.pub, destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*open2);
	open2->signature.bytes[0] ^= 1;
	node1.block_processor.add (open1);
	node1.block_processor.add (open2);
	node1.block_processor.flush ();
	{
		auto transaction (node1.store.tx_begin_read ());
		auto unchecked_count (node1.store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 1);
		ASSERT_EQ (unchecked_count, node1.store.unchecked_count (transaction));
		auto blocks (node1.store.unchecked_get (transaction, open1->source ()));
		ASSERT_EQ (blocks.size (), 1);
		ASSERT_EQ (blocks[0].verified, ysu::signature_verification::valid);
	}
	node1.block_processor.add (send1);
	node1.block_processor.flush ();
	{
		auto transaction (node1.store.tx_begin_read ());
		ASSERT_TRUE (node1.store.block_exists (transaction, open1->hash ()));
		auto unchecked_count (node1.store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 0);
		ASSERT_EQ (unchecked_count, node1.store.unchecked_count (transaction));
	}
}

TEST (ledger, unchecked_receive)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::genesis genesis;
	ysu::keypair destination;
	auto send1 (std::make_shared<ysu::state_block> (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	auto send2 (std::make_shared<ysu::state_block> (ysu::genesis_account, send1->hash (), ysu::genesis_account, ysu::genesis_amount - 2 * ysu::Gxrb_ratio, destination.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node1.work_generate_blocking (*send2);
	auto open1 (std::make_shared<ysu::open_block> (send1->hash (), destination.pub, destination.pub, destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*open1);
	auto receive1 (std::make_shared<ysu::receive_block> (open1->hash (), send2->hash (), destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*receive1);
	node1.block_processor.add (send1);
	node1.block_processor.add (receive1);
	node1.block_processor.flush ();
	// Previous block for receive1 is unknown, signature cannot be validated
	{
		auto transaction (node1.store.tx_begin_read ());
		auto unchecked_count (node1.store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 1);
		ASSERT_EQ (unchecked_count, node1.store.unchecked_count (transaction));
		auto blocks (node1.store.unchecked_get (transaction, receive1->previous ()));
		ASSERT_EQ (blocks.size (), 1);
		ASSERT_EQ (blocks[0].verified, ysu::signature_verification::unknown);
	}
	node1.block_processor.add (open1);
	node1.block_processor.flush ();
	// Previous block for receive1 is known, signature was validated
	{
		auto transaction (node1.store.tx_begin_read ());
		auto unchecked_count (node1.store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 1);
		ASSERT_EQ (unchecked_count, node1.store.unchecked_count (transaction));
		auto blocks (node1.store.unchecked_get (transaction, receive1->source ()));
		ASSERT_EQ (blocks.size (), 1);
		ASSERT_EQ (blocks[0].verified, ysu::signature_verification::valid);
	}
	node1.block_processor.add (send2);
	node1.block_processor.flush ();
	{
		auto transaction (node1.store.tx_begin_read ());
		ASSERT_TRUE (node1.store.block_exists (transaction, receive1->hash ()));
		auto unchecked_count (node1.store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 0);
		ASSERT_EQ (unchecked_count, node1.store.unchecked_count (transaction));
	}
}

TEST (ledger, confirmation_height_not_updated)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	auto transaction (store->tx_begin_write ());
	ysu::genesis genesis;
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::account_info account_info;
	ASSERT_FALSE (store->account_get (transaction, ysu::dev_genesis_key.pub, account_info));
	ysu::keypair key;
	ysu::send_block send1 (account_info.head, key.pub, 50, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (account_info.head));
	ysu::confirmation_height_info confirmation_height_info;
	ASSERT_FALSE (store->confirmation_height_get (transaction, ysu::genesis_account, confirmation_height_info));
	ASSERT_EQ (1, confirmation_height_info.height);
	ASSERT_EQ (genesis.hash (), confirmation_height_info.frontier);
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_FALSE (store->confirmation_height_get (transaction, ysu::genesis_account, confirmation_height_info));
	ASSERT_EQ (1, confirmation_height_info.height);
	ASSERT_EQ (genesis.hash (), confirmation_height_info.frontier);
	ysu::open_block open1 (send1.hash (), ysu::genesis_account, key.pub, key.prv, key.pub, *pool.generate (key.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open1).code);
	ASSERT_FALSE (store->confirmation_height_get (transaction, key.pub, confirmation_height_info));
	ASSERT_EQ (0, confirmation_height_info.height);
	ASSERT_EQ (ysu::block_hash (0), confirmation_height_info.frontier);
}

TEST (ledger, zero_rep)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::genesis genesis;
	ysu::block_builder builder;
	auto block1 = builder.state ()
	              .account (ysu::dev_genesis_key.pub)
	              .previous (genesis.hash ())
	              .representative (0)
	              .balance (ysu::genesis_amount)
	              .link (0)
	              .sign (ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub)
	              .work (*system.work.generate (genesis.hash ()))
	              .build ();
	auto transaction (node1.store.tx_begin_write ());
	ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, *block1).code);
	ASSERT_EQ (0, node1.ledger.cache.rep_weights.representation_get (ysu::dev_genesis_key.pub));
	ASSERT_EQ (ysu::genesis_amount, node1.ledger.cache.rep_weights.representation_get (0));
	auto block2 = builder.state ()
	              .account (ysu::dev_genesis_key.pub)
	              .previous (block1->hash ())
	              .representative (ysu::dev_genesis_key.pub)
	              .balance (ysu::genesis_amount)
	              .link (0)
	              .sign (ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub)
	              .work (*system.work.generate (block1->hash ()))
	              .build ();
	ASSERT_EQ (ysu::process_result::progress, node1.ledger.process (transaction, *block2).code);
	ASSERT_EQ (ysu::genesis_amount, node1.ledger.cache.rep_weights.representation_get (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, node1.ledger.cache.rep_weights.representation_get (0));
}

TEST (ledger, work_validation)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	store->initialize (store->tx_begin_write (), genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::block_builder builder;
	auto gen = ysu::dev_genesis_key;
	ysu::keypair key;

	// With random work the block doesn't pass, then modifies the block with sufficient work and ensures a correct result
	auto process_block = [&store, &ledger, &pool](ysu::block & block_a, ysu::block_details const details_a) {
		auto threshold = ysu::work_threshold (block_a.work_version (), details_a);
		// Rarely failed with random work, so modify until it doesn't have enough difficulty
		while (block_a.difficulty () >= threshold)
		{
			block_a.block_work_set (block_a.block_work () + 1);
		}
		EXPECT_EQ (ysu::process_result::insufficient_work, ledger.process (store->tx_begin_write (), block_a).code);
		block_a.block_work_set (*pool.generate (block_a.root (), threshold));
		EXPECT_EQ (ysu::process_result::progress, ledger.process (store->tx_begin_write (), block_a).code);
	};

	std::error_code ec;

	auto send = *builder.send ()
	             .previous (ysu::genesis_hash)
	             .destination (gen.pub)
	             .balance (ysu::genesis_amount - 1)
	             .sign (gen.prv, gen.pub)
	             .work (0)
	             .build (ec);
	ASSERT_FALSE (ec);

	auto receive = *builder.receive ()
	                .previous (send.hash ())
	                .source (send.hash ())
	                .sign (gen.prv, gen.pub)
	                .work (0)
	                .build (ec);
	ASSERT_FALSE (ec);

	auto change = *builder.change ()
	               .previous (receive.hash ())
	               .representative (key.pub)
	               .sign (gen.prv, gen.pub)
	               .work (0)
	               .build (ec);
	ASSERT_FALSE (ec);

	auto state = *builder.state ()
	              .account (gen.pub)
	              .previous (change.hash ())
	              .representative (gen.pub)
	              .balance (ysu::genesis_amount - 1)
	              .link (key.pub)
	              .sign (gen.prv, gen.pub)
	              .work (0)
	              .build (ec);
	ASSERT_FALSE (ec);

	auto open = *builder.open ()
	             .account (key.pub)
	             .source (state.hash ())
	             .representative (key.pub)
	             .sign (key.prv, key.pub)
	             .work (0)
	             .build (ec);
	ASSERT_FALSE (ec);

	auto epoch = *builder.state ()
	              .account (key.pub)
	              .previous (open.hash ())
	              .balance (1)
	              .representative (key.pub)
	              .link (ledger.epoch_link (ysu::epoch::epoch_1))
	              .sign (gen.prv, gen.pub)
	              .work (0)
	              .build (ec);
	ASSERT_FALSE (ec);

	process_block (send, {});
	process_block (receive, {});
	process_block (change, {});
	process_block (state, ysu::block_details (ysu::epoch::epoch_0, true, false, false));
	process_block (open, {});
	process_block (epoch, ysu::block_details (ysu::epoch::epoch_1, false, false, true));
}

TEST (ledger, epoch_2_started_flag)
{
	ysu::system system (2);

	auto & node1 = *system.nodes[0];
	ASSERT_FALSE (node1.ledger.cache.epoch_2_started.load ());
	ASSERT_NE (nullptr, system.upgrade_genesis_epoch (node1, ysu::epoch::epoch_1));
	ASSERT_FALSE (node1.ledger.cache.epoch_2_started.load ());
	ASSERT_NE (nullptr, system.upgrade_genesis_epoch (node1, ysu::epoch::epoch_2));
	ASSERT_TRUE (node1.ledger.cache.epoch_2_started.load ());

	auto & node2 = *system.nodes[1];
	ysu::keypair key;
	auto epoch1 = system.upgrade_genesis_epoch (node2, ysu::epoch::epoch_1);
	ASSERT_NE (nullptr, epoch1);
	ASSERT_FALSE (node2.ledger.cache.epoch_2_started.load ());
	ysu::state_block send (ysu::dev_genesis_key.pub, epoch1->hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - 1, key.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (epoch1->hash ()));
	ASSERT_EQ (ysu::process_result::progress, node2.process (send).code);
	ASSERT_FALSE (node2.ledger.cache.epoch_2_started.load ());
	ysu::state_block epoch2 (key.pub, 0, 0, 0, node2.ledger.epoch_link (ysu::epoch::epoch_2), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (key.pub));
	ASSERT_EQ (ysu::process_result::progress, node2.process (epoch2).code);
	ASSERT_TRUE (node2.ledger.cache.epoch_2_started.load ());

	// Ensure state is kept on ledger initialization
	ysu::stat stats;
	ysu::ledger ledger (node1.store, stats);
	ASSERT_TRUE (ledger.cache.epoch_2_started.load ());
}

TEST (ledger, epoch_2_upgrade_callback)
{
	ysu::genesis genesis;
	ysu::stat stats;
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	bool cb_hit = false;
	ysu::ledger ledger (*store, stats, ysu::generate_cache (), [&cb_hit]() {
		cb_hit = true;
	});
	{
		auto transaction (store->tx_begin_write ());
		store->initialize (transaction, genesis, ledger.cache);
	}
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	upgrade_epoch (pool, ledger, ysu::epoch::epoch_1);
	ASSERT_FALSE (cb_hit);
	auto latest = upgrade_epoch (pool, ledger, ysu::epoch::epoch_2);
	ASSERT_TRUE (cb_hit);
}

TEST (ledger, dependents_confirmed)
{
	ysu::block_builder builder;
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	auto transaction (store->tx_begin_write ());
	ysu::genesis genesis;
	store->initialize (transaction, genesis, ledger.cache);
	ASSERT_TRUE (ledger.dependents_confirmed (transaction, *genesis.open));
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	auto send1 = builder.state ()
	             .account (ysu::genesis_account)
	             .previous (genesis.hash ())
	             .representative (ysu::genesis_account)
	             .balance (ysu::genesis_amount - 100)
	             .link (key1.pub)
	             .sign (ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub)
	             .work (*pool.generate (genesis.hash ()))
	             .build_shared ();
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, *send1).code);
	ASSERT_TRUE (ledger.dependents_confirmed (transaction, *send1));
	auto send2 = builder.state ()
	             .account (ysu::genesis_account)
	             .previous (send1->hash ())
	             .representative (ysu::genesis_account)
	             .balance (ysu::genesis_amount - 200)
	             .link (key1.pub)
	             .sign (ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub)
	             .work (*pool.generate (send1->hash ()))
	             .build_shared ();
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, *send2).code);
	ASSERT_FALSE (ledger.dependents_confirmed (transaction, *send2));
	auto receive1 = builder.state ()
	                .account (key1.pub)
	                .previous (0)
	                .representative (ysu::genesis_account)
	                .balance (100)
	                .link (send1->hash ())
	                .sign (key1.prv, key1.pub)
	                .work (*pool.generate (key1.pub))
	                .build_shared ();
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, *receive1).code);
	ASSERT_FALSE (ledger.dependents_confirmed (transaction, *receive1));
	ysu::confirmation_height_info height;
	ASSERT_FALSE (ledger.store.confirmation_height_get (transaction, ysu::genesis_account, height));
	height.height += 1;
	ledger.store.confirmation_height_put (transaction, ysu::genesis_account, height);
	ASSERT_TRUE (ledger.dependents_confirmed (transaction, *receive1));
	auto receive2 = builder.state ()
	                .account (key1.pub)
	                .previous (receive1->hash ())
	                .representative (ysu::genesis_account)
	                .balance (200)
	                .link (send2->hash ())
	                .sign (key1.prv, key1.pub)
	                .work (*pool.generate (receive1->hash ()))
	                .build_shared ();
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, *receive2).code);
	ASSERT_FALSE (ledger.dependents_confirmed (transaction, *receive2));
	ASSERT_FALSE (ledger.store.confirmation_height_get (transaction, key1.pub, height));
	height.height += 1;
	ledger.store.confirmation_height_put (transaction, key1.pub, height);
	ASSERT_FALSE (ledger.dependents_confirmed (transaction, *receive2));
	ASSERT_FALSE (ledger.store.confirmation_height_get (transaction, ysu::genesis_account, height));
	height.height += 1;
	ledger.store.confirmation_height_put (transaction, ysu::genesis_account, height);
	ASSERT_TRUE (ledger.dependents_confirmed (transaction, *receive2));
}

TEST (ledger, block_confirmed)
{
	ysu::block_builder builder;
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_FALSE (store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	auto transaction (store->tx_begin_write ());
	ysu::genesis genesis;
	store->initialize (transaction, genesis, ledger.cache);
	ASSERT_TRUE (ledger.block_confirmed (transaction, genesis.open->hash ()));
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::keypair key1;
	auto send1 = builder.state ()
	             .account (ysu::genesis_account)
	             .previous (genesis.hash ())
	             .representative (ysu::genesis_account)
	             .balance (ysu::genesis_amount - 100)
	             .link (key1.pub)
	             .sign (ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub)
	             .work (*pool.generate (genesis.hash ()))
	             .build ();
	// Must be safe against non-existing blocks
	ASSERT_FALSE (ledger.block_confirmed (transaction, send1->hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, *send1).code);
	ASSERT_FALSE (ledger.block_confirmed (transaction, send1->hash ()));
	ysu::confirmation_height_info height;
	ASSERT_FALSE (ledger.store.confirmation_height_get (transaction, ysu::genesis_account, height));
	++height.height;
	ledger.store.confirmation_height_put (transaction, ysu::genesis_account, height);
	ASSERT_TRUE (ledger.block_confirmed (transaction, send1->hash ()));
}

TEST (ledger, cache)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ysu::genesis genesis;
	store->initialize (store->tx_begin_write (), genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::block_builder builder;

	size_t const total = 100;

	// Check existing ledger (incremental cache update) and reload on a new ledger
	for (size_t i (0); i < total; ++i)
	{
		auto account_count = 1 + i;
		auto block_count = 1 + 2 * (i + 1) - 2;
		auto cemented_count = 1 + 2 * (i + 1) - 2;
		auto genesis_weight = ysu::genesis_amount - i;
		auto pruned_count = i;

		auto cache_check = [&, i](ysu::ledger_cache const & cache_a) {
			ASSERT_EQ (account_count, cache_a.account_count);
			ASSERT_EQ (block_count, cache_a.block_count);
			ASSERT_EQ (cemented_count, cache_a.cemented_count);
			ASSERT_EQ (genesis_weight, cache_a.rep_weights.representation_get (ysu::genesis_account));
			ASSERT_EQ (pruned_count, cache_a.pruned_count);
		};

		ysu::keypair key;
		auto const latest = ledger.latest (store->tx_begin_read (), ysu::genesis_account);
		auto send = builder.state ()
		            .account (ysu::genesis_account)
		            .previous (latest)
		            .representative (ysu::genesis_account)
		            .balance (ysu::genesis_amount - (i + 1))
		            .link (key.pub)
		            .sign (ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub)
		            .work (*pool.generate (latest))
		            .build ();
		auto open = builder.state ()
		            .account (key.pub)
		            .previous (0)
		            .representative (key.pub)
		            .balance (1)
		            .link (send->hash ())
		            .sign (key.prv, key.pub)
		            .work (*pool.generate (key.pub))
		            .build ();
		{
			auto transaction (store->tx_begin_write ());
			ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, *send).code);
		}

		++block_count;
		--genesis_weight;
		cache_check (ledger.cache);
		cache_check (ysu::ledger (*store, stats).cache);

		{
			auto transaction (store->tx_begin_write ());
			ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, *open).code);
		}

		++block_count;
		++account_count;
		cache_check (ledger.cache);
		cache_check (ysu::ledger (*store, stats).cache);

		{
			auto transaction (store->tx_begin_write ());
			ysu::confirmation_height_info height;
			ASSERT_FALSE (ledger.store.confirmation_height_get (transaction, ysu::genesis_account, height));
			++height.height;
			height.frontier = send->hash ();
			ledger.store.confirmation_height_put (transaction, ysu::genesis_account, height);
			ASSERT_TRUE (ledger.block_confirmed (transaction, send->hash ()));
			++ledger.cache.cemented_count;
		}

		++cemented_count;
		cache_check (ledger.cache);
		cache_check (ysu::ledger (*store, stats).cache);

		{
			auto transaction (store->tx_begin_write ());
			ysu::confirmation_height_info height;
			ledger.store.confirmation_height_get (transaction, key.pub, height);
			height.height += 1;
			height.frontier = open->hash ();
			ledger.store.confirmation_height_put (transaction, key.pub, height);
			ASSERT_TRUE (ledger.block_confirmed (transaction, open->hash ()));
			++ledger.cache.cemented_count;
		}

		++cemented_count;
		cache_check (ledger.cache);
		cache_check (ysu::ledger (*store, stats).cache);

		{
			auto transaction (store->tx_begin_write ());
			ledger.store.pruned_put (transaction, open->hash ());
			++ledger.cache.pruned_count;
		}
		++pruned_count;
		cache_check (ledger.cache);
		cache_check (ysu::ledger (*store, stats).cache);
	}
}

TEST (ledger, pruning_action)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ledger.pruning = true;
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (store->block_exists (transaction, send1.hash ()));
	auto send1_stored (store->block_get (transaction, send1.hash ()));
	ASSERT_NE (nullptr, send1_stored);
	ASSERT_EQ (send1, *send1_stored);
	ASSERT_TRUE (store->pending_exists (transaction, ysu::pending_key (ysu::genesis_account, send1.hash ())));
	ysu::state_block send2 (ysu::genesis_account, send1.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio * 2, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send2).code);
	ASSERT_TRUE (store->block_exists (transaction, send2.hash ()));
	// Pruning action
	ASSERT_EQ (1, ledger.pruning_action (transaction, send1.hash (), 1));
	ASSERT_EQ (0, ledger.pruning_action (transaction, genesis.hash (), 1));
	ASSERT_TRUE (store->pending_exists (transaction, ysu::pending_key (ysu::genesis_account, send1.hash ())));
	ASSERT_FALSE (store->block_exists (transaction, send1.hash ()));
	ASSERT_TRUE (ledger.block_or_pruned_exists (transaction, send1.hash ()));
	// Pruned ledger start without proper flags emulation
	ledger.pruning = false;
	ASSERT_FALSE (ledger.block_or_pruned_exists (transaction, send1.hash ()));
	ledger.pruning = true;
	ASSERT_TRUE (store->pruned_exists (transaction, send1.hash ()));
	ASSERT_TRUE (store->block_exists (transaction, genesis.hash ()));
	ASSERT_TRUE (store->block_exists (transaction, send2.hash ()));
	// Middle block pruning
	ASSERT_TRUE (store->block_exists (transaction, send2.hash ()));
	ASSERT_EQ (1, ledger.pruning_action (transaction, send2.hash (), 1));
	ASSERT_TRUE (store->pruned_exists (transaction, send2.hash ()));
	ASSERT_FALSE (store->block_exists (transaction, send2.hash ()));
	ASSERT_EQ (store->account_count (transaction), ledger.cache.account_count);
	ASSERT_EQ (store->pruned_count (transaction), ledger.cache.pruned_count);
	ASSERT_EQ (store->block_count (transaction), ledger.cache.block_count - ledger.cache.pruned_count);
}

TEST (ledger, pruning_large_chain)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ledger.pruning = true;
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	size_t send_receive_pairs (20);
	auto last_hash (genesis.hash ());
	for (auto i (0); i < send_receive_pairs; i++)
	{
		ysu::state_block send (ysu::genesis_account, last_hash, ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (last_hash));
		ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send).code);
		ASSERT_TRUE (store->block_exists (transaction, send.hash ()));
		ysu::state_block receive (ysu::genesis_account, send.hash (), ysu::genesis_account, ysu::genesis_amount, send.hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send.hash ()));
		ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive).code);
		ASSERT_TRUE (store->block_exists (transaction, receive.hash ()));
		last_hash = receive.hash ();
	}
	ASSERT_EQ (0, store->pruned_count (transaction));
	ASSERT_EQ (send_receive_pairs * 2 + 1, store->block_count (transaction));
	// Pruning action
	ASSERT_EQ (send_receive_pairs * 2, ledger.pruning_action (transaction, last_hash, 5));
	ASSERT_TRUE (store->pruned_exists (transaction, last_hash));
	ASSERT_TRUE (store->block_exists (transaction, genesis.hash ()));
	ASSERT_FALSE (store->block_exists (transaction, last_hash));
	ASSERT_EQ (store->pruned_count (transaction), ledger.cache.pruned_count);
	ASSERT_EQ (store->block_count (transaction), ledger.cache.block_count - ledger.cache.pruned_count);
	ASSERT_EQ (send_receive_pairs * 2, store->pruned_count (transaction));
	ASSERT_EQ (1, store->block_count (transaction)); // Genesis
}

TEST (ledger, pruning_legacy_blocks)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ledger.pruning = true;
	ysu::genesis genesis;
	ysu::keypair key1;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::send_block send1 (genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (store->pending_exists (transaction, ysu::pending_key (ysu::genesis_account, send1.hash ())));
	ysu::receive_block receive1 (send1.hash (), send1.hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, receive1).code);
	ysu::change_block change1 (receive1.hash (), key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (receive1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, change1).code);
	ysu::send_block send2 (change1.hash (), key1.pub, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (change1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send2).code);
	ysu::open_block open1 (send2.hash (), ysu::genesis_account, key1.pub, key1.prv, key1.pub, *pool.generate (key1.pub));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, open1).code);
	ysu::send_block send3 (open1.hash (), ysu::genesis_account, 0, key1.prv, key1.pub, *pool.generate (open1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send3).code);
	// Pruning action
	ASSERT_EQ (3, ledger.pruning_action (transaction, change1.hash (), 2));
	ASSERT_EQ (1, ledger.pruning_action (transaction, open1.hash (), 1));
	ASSERT_TRUE (store->block_exists (transaction, genesis.hash ()));
	ASSERT_FALSE (store->block_exists (transaction, send1.hash ()));
	ASSERT_TRUE (store->pruned_exists (transaction, send1.hash ()));
	ASSERT_FALSE (store->block_exists (transaction, receive1.hash ()));
	ASSERT_TRUE (store->pruned_exists (transaction, receive1.hash ()));
	ASSERT_FALSE (store->block_exists (transaction, change1.hash ()));
	ASSERT_TRUE (store->pruned_exists (transaction, change1.hash ()));
	ASSERT_TRUE (store->block_exists (transaction, send2.hash ()));
	ASSERT_FALSE (store->block_exists (transaction, open1.hash ()));
	ASSERT_TRUE (store->pruned_exists (transaction, open1.hash ()));
	ASSERT_TRUE (store->block_exists (transaction, send3.hash ()));
	ASSERT_EQ (4, ledger.cache.pruned_count);
	ASSERT_EQ (7, ledger.cache.block_count);
	ASSERT_EQ (store->pruned_count (transaction), ledger.cache.pruned_count);
	ASSERT_EQ (store->block_count (transaction), ledger.cache.block_count - ledger.cache.pruned_count);
}

TEST (ledger, pruning_safe_functions)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ledger.pruning = true;
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (store->block_exists (transaction, send1.hash ()));
	ysu::state_block send2 (ysu::genesis_account, send1.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio * 2, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send2).code);
	ASSERT_TRUE (store->block_exists (transaction, send2.hash ()));
	// Pruning action
	ASSERT_EQ (1, ledger.pruning_action (transaction, send1.hash (), 1));
	ASSERT_FALSE (store->block_exists (transaction, send1.hash ()));
	ASSERT_TRUE (ledger.block_or_pruned_exists (transaction, send1.hash ()));
	ASSERT_TRUE (store->pruned_exists (transaction, send1.hash ()));
	ASSERT_TRUE (store->block_exists (transaction, genesis.hash ()));
	ASSERT_TRUE (store->block_exists (transaction, send2.hash ()));
	// Safe ledger actions
	bool error (false);
	ASSERT_EQ (0, ledger.balance_safe (transaction, send1.hash (), error));
	ASSERT_TRUE (error);
	error = false;
	ASSERT_EQ (ysu::genesis_amount - ysu::Gxrb_ratio * 2, ledger.balance_safe (transaction, send2.hash (), error));
	ASSERT_FALSE (error);
	error = false;
	ASSERT_EQ (0, ledger.amount_safe (transaction, send2.hash (), error));
	ASSERT_TRUE (error);
	error = false;
	ASSERT_TRUE (ledger.account_safe (transaction, send1.hash (), error).is_zero ());
	ASSERT_TRUE (error);
	error = false;
	ASSERT_EQ (ysu::genesis_account, ledger.account_safe (transaction, send2.hash (), error));
	ASSERT_FALSE (error);
}

TEST (ledger, hash_root_random)
{
	ysu::logger_mt logger;
	auto store = ysu::make_store (logger, ysu::unique_path ());
	ASSERT_TRUE (!store->init_error ());
	ysu::stat stats;
	ysu::ledger ledger (*store, stats);
	ledger.pruning = true;
	ysu::genesis genesis;
	auto transaction (store->tx_begin_write ());
	store->initialize (transaction, genesis, ledger.cache);
	ysu::work_pool pool (std::numeric_limits<unsigned>::max ());
	ysu::state_block send1 (ysu::genesis_account, genesis.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send1).code);
	ASSERT_TRUE (store->block_exists (transaction, send1.hash ()));
	ysu::state_block send2 (ysu::genesis_account, send1.hash (), ysu::genesis_account, ysu::genesis_amount - ysu::Gxrb_ratio * 2, ysu::genesis_account, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *pool.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, ledger.process (transaction, send2).code);
	ASSERT_TRUE (store->block_exists (transaction, send2.hash ()));
	// Pruning action
	ASSERT_EQ (1, ledger.pruning_action (transaction, send1.hash (), 1));
	ASSERT_FALSE (store->block_exists (transaction, send1.hash ()));
	ASSERT_TRUE (store->pruned_exists (transaction, send1.hash ()));
	ASSERT_TRUE (store->block_exists (transaction, genesis.hash ()));
	ASSERT_TRUE (store->block_exists (transaction, send2.hash ()));
	// Test random block including pruned
	bool done (false);
	auto iteration (0);
	while (!done)
	{
		++iteration;
		auto root_hash (ledger.hash_root_random (transaction));
		done = (root_hash.first == send1.hash ()) && (root_hash.second.is_zero ());
		ASSERT_LE (iteration, 1000);
	}
	done = false;
	while (!done)
	{
		++iteration;
		auto root_hash (ledger.hash_root_random (transaction));
		done = (root_hash.first == send2.hash ()) && (root_hash.second == send2.root ().as_block_hash ());
		ASSERT_LE (iteration, 1000);
	}
}