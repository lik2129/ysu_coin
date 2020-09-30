#include <ysu/node/testing.hpp>
#include <ysu/secure/versioning.hpp>
#include <ysu/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST (wallets, open_create)
{
	ysu::system system (1);
	bool error (false);
	ysu::wallets wallets (error, *system.nodes[0]);
	ASSERT_FALSE (error);
	ASSERT_EQ (1, wallets.items.size ()); // it starts out with a default wallet
	auto id = ysu::random_wallet_id ();
	ASSERT_EQ (nullptr, wallets.open (id));
	auto wallet (wallets.create (id));
	ASSERT_NE (nullptr, wallet);
	ASSERT_EQ (wallet, wallets.open (id));
}

TEST (wallets, open_existing)
{
	ysu::system system (1);
	auto id (ysu::random_wallet_id ());
	{
		bool error (false);
		ysu::wallets wallets (error, *system.nodes[0]);
		ASSERT_FALSE (error);
		ASSERT_EQ (1, wallets.items.size ());
		auto wallet (wallets.create (id));
		ASSERT_NE (nullptr, wallet);
		ASSERT_EQ (wallet, wallets.open (id));
		ysu::raw_key password;
		password.data.clear ();
		system.deadline_set (10s);
		while (password.data == 0)
		{
			ASSERT_NO_ERROR (system.poll ());
			wallet->store.password.value (password);
		}
	}
	{
		bool error (false);
		ysu::wallets wallets (error, *system.nodes[0]);
		ASSERT_FALSE (error);
		ASSERT_EQ (2, wallets.items.size ());
		ASSERT_NE (nullptr, wallets.open (id));
	}
}

TEST (wallets, remove)
{
	ysu::system system (1);
	ysu::wallet_id one (1);
	{
		bool error (false);
		ysu::wallets wallets (error, *system.nodes[0]);
		ASSERT_FALSE (error);
		ASSERT_EQ (1, wallets.items.size ());
		auto wallet (wallets.create (one));
		ASSERT_NE (nullptr, wallet);
		ASSERT_EQ (2, wallets.items.size ());
		wallets.destroy (one);
		ASSERT_EQ (1, wallets.items.size ());
	}
	{
		bool error (false);
		ysu::wallets wallets (error, *system.nodes[0]);
		ASSERT_FALSE (error);
		ASSERT_EQ (1, wallets.items.size ());
	}
}

// Keeps breaking whenever we add new DBs
TEST (wallets, DISABLED_wallet_create_max)
{
	ysu::system system (1);
	bool error (false);
	ysu::wallets wallets (error, *system.nodes[0]);
	const int nonWalletDbs = 19;
	for (int i = 0; i < system.nodes[0]->config.deprecated_lmdb_max_dbs - nonWalletDbs; i++)
	{
		auto wallet_id = ysu::random_wallet_id ();
		auto wallet = wallets.create (wallet_id);
		auto existing = wallets.items.find (wallet_id);
		ASSERT_TRUE (existing != wallets.items.end ());
		ysu::raw_key seed;
		seed.data = 0;
		auto transaction (system.nodes[0]->store.tx_begin_write ());
		existing->second->store.seed_set (transaction, seed);
	}
	auto wallet_id = ysu::random_wallet_id ();
	wallets.create (wallet_id);
	auto existing = wallets.items.find (wallet_id);
	ASSERT_TRUE (existing == wallets.items.end ());
}

TEST (wallets, reload)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::wallet_id one (1);
	bool error (false);
	ASSERT_FALSE (error);
	ASSERT_EQ (1, node1.wallets.items.size ());
	{
		ysu::lock_guard<std::mutex> lock_wallet (node1.wallets.mutex);
		ysu::inactive_node node (node1.application_path, ysu::inactive_node_flag_defaults ());
		auto wallet (node.node->wallets.create (one));
		ASSERT_NE (wallet, nullptr);
	}
	ASSERT_TIMELY (5s, node1.wallets.open (one) != nullptr);
	ASSERT_EQ (2, node1.wallets.items.size ());
}

TEST (wallets, vote_minimum)
{
	ysu::system system (1);
	auto & node1 (*system.nodes[0]);
	ysu::keypair key1;
	ysu::keypair key2;
	ysu::genesis genesis;
	ysu::state_block send1 (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, std::numeric_limits<ysu::uint128_t>::max () - node1.config.vote_minimum.number (), key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (genesis.hash ()));
	ASSERT_EQ (ysu::process_result::progress, node1.process (send1).code);
	ysu::state_block open1 (key1.pub, 0, key1.pub, node1.config.vote_minimum.number (), send1.hash (), key1.prv, key1.pub, *system.work.generate (key1.pub));
	ASSERT_EQ (ysu::process_result::progress, node1.process (open1).code);
	// send2 with amount vote_minimum - 1 (not voting representative)
	ysu::state_block send2 (ysu::dev_genesis_key.pub, send1.hash (), ysu::dev_genesis_key.pub, std::numeric_limits<ysu::uint128_t>::max () - 2 * node1.config.vote_minimum.number () + 1, key2.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (send1.hash ()));
	ASSERT_EQ (ysu::process_result::progress, node1.process (send2).code);
	ysu::state_block open2 (key2.pub, 0, key2.pub, node1.config.vote_minimum.number () - 1, send2.hash (), key2.prv, key2.pub, *system.work.generate (key2.pub));
	ASSERT_EQ (ysu::process_result::progress, node1.process (open2).code);
	auto wallet (node1.wallets.items.begin ()->second);
	ASSERT_EQ (0, wallet->representatives.size ());
	wallet->insert_adhoc (ysu::dev_genesis_key.prv);
	wallet->insert_adhoc (key1.prv);
	wallet->insert_adhoc (key2.prv);
	node1.wallets.compute_reps ();
	ASSERT_EQ (2, wallet->representatives.size ());
}

TEST (wallets, exists)
{
	ysu::system system (1);
	auto & node (*system.nodes[0]);
	ysu::keypair key1;
	ysu::keypair key2;
	{
		auto transaction (node.wallets.tx_begin_read ());
		ASSERT_FALSE (node.wallets.exists (transaction, key1.pub));
		ASSERT_FALSE (node.wallets.exists (transaction, key2.pub));
	}
	system.wallet (0)->insert_adhoc (key1.prv);
	{
		auto transaction (node.wallets.tx_begin_read ());
		ASSERT_TRUE (node.wallets.exists (transaction, key1.pub));
		ASSERT_FALSE (node.wallets.exists (transaction, key2.pub));
	}
	system.wallet (0)->insert_adhoc (key2.prv);
	{
		auto transaction (node.wallets.tx_begin_read ());
		ASSERT_TRUE (node.wallets.exists (transaction, key1.pub));
		ASSERT_TRUE (node.wallets.exists (transaction, key2.pub));
	}
}

TEST (wallets, search_pending)
{
	for (auto search_all : { false, true })
	{
		ysu::system system;
		ysu::node_config config (ysu::get_available_port (), system.logging);
		config.enable_voting = false;
		config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
		ysu::node_flags flags;
		flags.disable_search_pending = true;
		auto & node (*system.add_node (config, flags));

		auto wallets = node.wallets.get_wallets ();
		ASSERT_EQ (1, wallets.size ());
		auto wallet_id = wallets.begin ()->first;
		auto wallet = wallets.begin ()->second;

		wallet->insert_adhoc (ysu::dev_genesis_key.prv);
		ysu::block_builder builder;
		auto send = builder.state ()
		            .account (ysu::genesis_account)
		            .previous (ysu::genesis_hash)
		            .representative (ysu::genesis_account)
		            .balance (ysu::genesis_amount - node.config.receive_minimum.number ())
		            .link (ysu::genesis_account)
		            .sign (ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub)
		            .work (*system.work.generate (ysu::genesis_hash))
		            .build ();
		ASSERT_EQ (ysu::process_result::progress, node.process (*send).code);

		// Pending search should start an election
		ASSERT_TRUE (node.active.empty ());
		if (search_all)
		{
			node.wallets.search_pending_all ();
		}
		else
		{
			node.wallets.search_pending (wallet_id);
		}
		auto election = node.active.election (send->qualified_root ());
		ASSERT_NE (nullptr, election);

		// Erase the key so the confirmation does not trigger an automatic receive
		wallet->store.erase (node.wallets.tx_begin_write (), ysu::genesis_account);

		// Now confirm the election
		election->force_confirm ();

		ASSERT_TIMELY (5s, node.block_confirmed (send->hash ()) && node.active.empty ());

		// Re-insert the key
		wallet->insert_adhoc (ysu::dev_genesis_key.prv);

		// Pending search should create the receive block
		ASSERT_EQ (2, node.ledger.cache.block_count);
		if (search_all)
		{
			node.wallets.search_pending_all ();
		}
		else
		{
			node.wallets.search_pending (wallet_id);
		}
		ASSERT_TIMELY (3s, node.balance (ysu::genesis_account) == ysu::genesis_amount);
		auto receive_hash = node.ledger.latest (node.store.tx_begin_read (), ysu::genesis_account);
		auto receive = node.block (receive_hash);
		ASSERT_NE (nullptr, receive);
		ASSERT_EQ (receive->sideband ().height, 3);
		ASSERT_EQ (send->hash (), receive->link ().as_block_hash ());
	}
}
