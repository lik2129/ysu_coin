#include <ysu/node/bootstrap/bootstrap_frontier.hpp>
#include <ysu/node/bootstrap/bootstrap_lazy.hpp>
#include <ysu/node/testing.hpp>
#include <ysu/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

// If the account doesn't exist, current == end so there's no iteration
TEST (bulk_pull, no_address)
{
	ysu::system system (1);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	auto req = std::make_unique<ysu::bulk_pull> ();
	req->start = 1;
	req->end = 2;
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::bulk_pull_server> (connection, std::move (req)));
	ASSERT_EQ (request->current, request->request->end);
	ASSERT_TRUE (request->current.is_zero ());
}

TEST (bulk_pull, genesis_to_end)
{
	ysu::system system (1);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	auto req = std::make_unique<ysu::bulk_pull> ();
	req->start = ysu::dev_genesis_key.pub;
	req->end.clear ();
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::bulk_pull_server> (connection, std::move (req)));
	ASSERT_EQ (system.nodes[0]->latest (ysu::dev_genesis_key.pub), request->current);
	ASSERT_EQ (request->request->end, request->request->end);
}

// If we can't find the end block, send everything
TEST (bulk_pull, no_end)
{
	ysu::system system (1);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	auto req = std::make_unique<ysu::bulk_pull> ();
	req->start = ysu::dev_genesis_key.pub;
	req->end = 1;
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::bulk_pull_server> (connection, std::move (req)));
	ASSERT_EQ (system.nodes[0]->latest (ysu::dev_genesis_key.pub), request->current);
	ASSERT_TRUE (request->request->end.is_zero ());
}

TEST (bulk_pull, end_not_owned)
{
	ysu::system system (1);
	ysu::keypair key2;
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	ASSERT_NE (nullptr, system.wallet (0)->send_action (ysu::dev_genesis_key.pub, key2.pub, 100));
	ysu::block_hash latest (system.nodes[0]->latest (ysu::dev_genesis_key.pub));
	ysu::open_block open (0, 1, 2, ysu::keypair ().prv, 4, 5);
	open.hashables.account = key2.pub;
	open.hashables.representative = key2.pub;
	open.hashables.source = latest;
	open.refresh ();
	open.signature = ysu::sign_message (key2.prv, key2.pub, open.hash ());
	system.nodes[0]->work_generate_blocking (open);
	ASSERT_EQ (ysu::process_result::progress, system.nodes[0]->process (open).code);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	ysu::genesis genesis;
	auto req = std::make_unique<ysu::bulk_pull> ();
	req->start = key2.pub;
	req->end = genesis.hash ();
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::bulk_pull_server> (connection, std::move (req)));
	ASSERT_EQ (request->current, request->request->end);
}

TEST (bulk_pull, none)
{
	ysu::system system (1);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	ysu::genesis genesis;
	auto req = std::make_unique<ysu::bulk_pull> ();
	req->start = ysu::dev_genesis_key.pub;
	req->end = genesis.hash ();
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::bulk_pull_server> (connection, std::move (req)));
	auto block (request->get_next ());
	ASSERT_EQ (nullptr, block);
}

TEST (bulk_pull, get_next_on_open)
{
	ysu::system system (1);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	auto req = std::make_unique<ysu::bulk_pull> ();
	req->start = ysu::dev_genesis_key.pub;
	req->end.clear ();
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::bulk_pull_server> (connection, std::move (req)));
	auto block (request->get_next ());
	ASSERT_NE (nullptr, block);
	ASSERT_TRUE (block->previous ().is_zero ());
	ASSERT_FALSE (connection->requests.empty ());
	ASSERT_EQ (request->current, request->request->end);
}

TEST (bulk_pull, by_block)
{
	ysu::system system (1);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	ysu::genesis genesis;
	auto req = std::make_unique<ysu::bulk_pull> ();
	req->start = genesis.hash ();
	req->end.clear ();
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::bulk_pull_server> (connection, std::move (req)));
	auto block (request->get_next ());
	ASSERT_NE (nullptr, block);
	ASSERT_EQ (block->hash (), genesis.hash ());

	block = request->get_next ();
	ASSERT_EQ (nullptr, block);
}

TEST (bulk_pull, by_block_single)
{
	ysu::system system (1);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	ysu::genesis genesis;
	auto req = std::make_unique<ysu::bulk_pull> ();
	req->start = genesis.hash ();
	req->end = genesis.hash ();
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::bulk_pull_server> (connection, std::move (req)));
	auto block (request->get_next ());
	ASSERT_NE (nullptr, block);
	ASSERT_EQ (block->hash (), genesis.hash ());

	block = request->get_next ();
	ASSERT_EQ (nullptr, block);
}

TEST (bulk_pull, count_limit)
{
	ysu::system system (1);
	ysu::genesis genesis;

	auto send1 (std::make_shared<ysu::send_block> (system.nodes[0]->latest (ysu::dev_genesis_key.pub), ysu::dev_genesis_key.pub, 1, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (system.nodes[0]->latest (ysu::dev_genesis_key.pub))));
	ASSERT_EQ (ysu::process_result::progress, system.nodes[0]->process (*send1).code);
	auto receive1 (std::make_shared<ysu::receive_block> (send1->hash (), send1->hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (send1->hash ())));
	ASSERT_EQ (ysu::process_result::progress, system.nodes[0]->process (*receive1).code);

	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	auto req = std::make_unique<ysu::bulk_pull> ();
	req->start = receive1->hash ();
	req->set_count_present (true);
	req->count = 2;
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::bulk_pull_server> (connection, std::move (req)));

	ASSERT_EQ (request->max_count, 2);
	ASSERT_EQ (request->sent_count, 0);

	auto block (request->get_next ());
	ASSERT_EQ (receive1->hash (), block->hash ());

	block = request->get_next ();
	ASSERT_EQ (send1->hash (), block->hash ());

	block = request->get_next ();
	ASSERT_EQ (nullptr, block);
}

TEST (bootstrap_processor, DISABLED_process_none)
{
	ysu::system system (1);
	auto node1 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node1->init_error ());
	auto done (false);
	node1->bootstrap_initiator.bootstrap (system.nodes[0]->network.endpoint ());
	while (!done)
	{
		system.io_ctx.run_one ();
	}
	node1->stop ();
}

// Bootstrap can pull one basic block
TEST (bootstrap_processor, process_one)
{
	ysu::system system;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	node_config.enable_voting = false;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	auto node0 = system.add_node (node_config, node_flags);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	ASSERT_NE (nullptr, system.wallet (0)->send_action (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.pub, 100));

	node_config.peering_port = ysu::get_available_port ();
	node_flags.disable_rep_crawler = true;
	auto node1 (std::make_shared<ysu::node> (system.io_ctx, ysu::unique_path (), system.alarm, node_config, system.work, node_flags));
	ysu::block_hash hash1 (node0->latest (ysu::dev_genesis_key.pub));
	ysu::block_hash hash2 (node1->latest (ysu::dev_genesis_key.pub));
	ASSERT_NE (hash1, hash2);
	node1->bootstrap_initiator.bootstrap (node0->network.endpoint ());
	ASSERT_NE (node1->latest (ysu::dev_genesis_key.pub), node0->latest (ysu::dev_genesis_key.pub));
	ASSERT_TIMELY (10s, node1->latest (ysu::dev_genesis_key.pub) == node0->latest (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, node1->active.size ());
	node1->stop ();
}

TEST (bootstrap_processor, process_two)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	auto node0 (system.add_node (config, node_flags));
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::block_hash hash1 (node0->latest (ysu::dev_genesis_key.pub));
	ASSERT_NE (nullptr, system.wallet (0)->send_action (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.pub, 50));
	ysu::block_hash hash2 (node0->latest (ysu::dev_genesis_key.pub));
	ASSERT_NE (nullptr, system.wallet (0)->send_action (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.pub, 50));
	ysu::block_hash hash3 (node0->latest (ysu::dev_genesis_key.pub));
	ASSERT_NE (hash1, hash2);
	ASSERT_NE (hash1, hash3);
	ASSERT_NE (hash2, hash3);
	auto node1 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node1->init_error ());
	node1->bootstrap_initiator.bootstrap (node0->network.endpoint ());
	ASSERT_NE (node1->latest (ysu::dev_genesis_key.pub), node0->latest (ysu::dev_genesis_key.pub));
	ASSERT_TIMELY (10s, node1->latest (ysu::dev_genesis_key.pub) == node0->latest (ysu::dev_genesis_key.pub));
	node1->stop ();
}

// Bootstrap can pull universal blocks
TEST (bootstrap_processor, process_state)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	auto node0 (system.add_node (config, node_flags));
	ysu::genesis genesis;
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	auto block1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, node0->latest (ysu::dev_genesis_key.pub), ysu::dev_genesis_key.pub, ysu::genesis_amount - 100, ysu::dev_genesis_key.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	auto block2 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, block1->hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount, block1->hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0));
	node0->work_generate_blocking (*block1);
	node0->work_generate_blocking (*block2);
	node0->process (*block1);
	node0->process (*block2);
	auto node1 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_EQ (node0->latest (ysu::dev_genesis_key.pub), block2->hash ());
	ASSERT_NE (node1->latest (ysu::dev_genesis_key.pub), block2->hash ());
	node1->bootstrap_initiator.bootstrap (node0->network.endpoint ());
	ASSERT_NE (node1->latest (ysu::dev_genesis_key.pub), node0->latest (ysu::dev_genesis_key.pub));
	ASSERT_TIMELY (10s, node1->latest (ysu::dev_genesis_key.pub) == node0->latest (ysu::dev_genesis_key.pub));
	ASSERT_EQ (0, node1->active.size ());
	node1->stop ();
}

TEST (bootstrap_processor, process_new)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	auto node1 (system.add_node (config, node_flags));
	config.peering_port = ysu::get_available_port ();
	auto node2 (system.add_node (config, node_flags));
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::keypair key2;
	system.wallet (1)->insert_adhoc (key2.prv);
	ASSERT_NE (nullptr, system.wallet (0)->send_action (ysu::dev_genesis_key.pub, key2.pub, node1->config.receive_minimum.number ()));
	ASSERT_TIMELY (10s, !node1->balance (key2.pub).is_zero ());
	ysu::uint128_t balance1 (node1->balance (ysu::dev_genesis_key.pub));
	ysu::uint128_t balance2 (node1->balance (key2.pub));
	auto node3 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node3->init_error ());
	node3->bootstrap_initiator.bootstrap (node1->network.endpoint ());
	ASSERT_TIMELY (10s, node3->balance (key2.pub) == balance2);
	ASSERT_EQ (balance1, node3->balance (ysu::dev_genesis_key.pub));
	node3->stop ();
}

TEST (bootstrap_processor, pull_diamond)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	auto node0 (system.add_node (config, node_flags));
	ysu::keypair key;
	auto send1 (std::make_shared<ysu::send_block> (node0->latest (ysu::dev_genesis_key.pub), key.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (node0->latest (ysu::dev_genesis_key.pub))));
	ASSERT_EQ (ysu::process_result::progress, node0->process (*send1).code);
	auto open (std::make_shared<ysu::open_block> (send1->hash (), 1, key.pub, key.prv, key.pub, *system.work.generate (key.pub)));
	ASSERT_EQ (ysu::process_result::progress, node0->process (*open).code);
	auto send2 (std::make_shared<ysu::send_block> (open->hash (), ysu::dev_genesis_key.pub, std::numeric_limits<ysu::uint128_t>::max () - 100, key.prv, key.pub, *system.work.generate (open->hash ())));
	ASSERT_EQ (ysu::process_result::progress, node0->process (*send2).code);
	auto receive (std::make_shared<ysu::receive_block> (send1->hash (), send2->hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (send1->hash ())));
	ASSERT_EQ (ysu::process_result::progress, node0->process (*receive).code);
	auto node1 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node1->init_error ());
	node1->bootstrap_initiator.bootstrap (node0->network.endpoint ());
	ASSERT_TIMELY (10s, node1->balance (ysu::dev_genesis_key.pub) == 100);
	ASSERT_EQ (100, node1->balance (ysu::dev_genesis_key.pub));
	node1->stop ();
}

TEST (bootstrap_processor, DISABLED_pull_requeue_network_error)
{
	// Bootstrap attempt stopped before requeue & then cannot be found in attempts list
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	auto node1 (system.add_node (config, node_flags));
	config.peering_port = ysu::get_available_port ();
	auto node2 (system.add_node (config, node_flags));
	ysu::genesis genesis;
	ysu::keypair key1;
	auto send1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (genesis.hash ())));

	node1->bootstrap_initiator.bootstrap (node2->network.endpoint ());
	auto attempt (node1->bootstrap_initiator.current_attempt ());
	ASSERT_NE (nullptr, attempt);
	ASSERT_TIMELY (2s, attempt->frontiers_received);
	// Add non-existing pull & stop remote peer
	{
		ysu::unique_lock<std::mutex> lock (node1->bootstrap_initiator.connections->mutex);
		ASSERT_FALSE (attempt->stopped);
		++attempt->pulling;
		node1->bootstrap_initiator.connections->pulls.push_back (ysu::pull_info (ysu::dev_genesis_key.pub, send1->hash (), genesis.hash (), attempt->incremental_id));
		node1->bootstrap_initiator.connections->request_pull (lock);
		node2->stop ();
	}
	ASSERT_TIMELY (5s, attempt == nullptr || attempt->requeued_pulls == 1);
	ASSERT_EQ (0, node1->stats.count (ysu::stat::type::bootstrap, ysu::stat::detail::bulk_pull_failed_account, ysu::stat::dir::in)); // Requeue is not increasing failed attempts
}

TEST (bootstrap_processor, frontiers_unconfirmed)
{
	ysu::system system;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	node_config.tcp_io_timeout = std::chrono::seconds (2);
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_pull_server = true;
	node_flags.disable_bootstrap_bulk_push_client = true;
	node_flags.disable_legacy_bootstrap = true;
	node_flags.disable_lazy_bootstrap = true;
	node_flags.disable_wallet_bootstrap = true;
	node_flags.disable_rep_crawler = true;
	auto node1 = system.add_node (node_config, node_flags);
	ysu::genesis genesis;
	ysu::keypair key1, key2;
	// Generating invalid chain
	auto send1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (genesis.hash ())));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send1).code);
	auto send2 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, send1->hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - 2 * ysu::Gxrb_ratio, key2.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (send1->hash ())));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send2).code);
	auto open1 (std::make_shared<ysu::state_block> (key1.pub, 0, key1.pub, ysu::Gxrb_ratio, send1->hash (), key1.prv, key1.pub, *system.work.generate (key1.pub)));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*open1).code);
	auto open2 (std::make_shared<ysu::state_block> (key2.pub, 0, key2.pub, ysu::Gxrb_ratio, send2->hash (), key2.prv, key2.pub, *system.work.generate (key2.pub)));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*open2).code);

	node_config.peering_port = ysu::get_available_port ();
	node_flags.disable_bootstrap_bulk_pull_server = false;
	node_flags.disable_rep_crawler = false;
	auto node2 = system.add_node (node_config, node_flags);
	// Generating valid chain
	auto send3 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::xrb_ratio, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (genesis.hash ())));
	ASSERT_EQ (ysu::process_result::progress, node2->process (*send3).code);
	auto open3 (std::make_shared<ysu::state_block> (key1.pub, 0, key1.pub, ysu::xrb_ratio, send3->hash (), key1.prv, key1.pub, *system.work.generate (key1.pub)));
	ASSERT_EQ (ysu::process_result::progress, node2->process (*open3).code);
	system.wallet (1)->insert_adhoc (ysu::dev_genesis_key.prv);

	// Ensure node2 can generate votes
	node2->block_confirm (send3);
	ASSERT_TIMELY (5s, node2->ledger.cache.cemented_count == 3);

	// Test node to restart bootstrap
	node_config.peering_port = ysu::get_available_port ();
	node_flags.disable_legacy_bootstrap = false;
	auto node3 = system.add_node (node_config, node_flags);
	ASSERT_TIMELY (5s, node3->rep_crawler.representative_count () != 0);
	//Add single excluded peers record (2 records are required to drop peer)
	node3->network.excluded_peers.add (ysu::transport::map_endpoint_to_tcp (node1->network.endpoint ()), 0);
	ASSERT_FALSE (node3->network.excluded_peers.check (ysu::transport::map_endpoint_to_tcp (node1->network.endpoint ())));
	node3->bootstrap_initiator.bootstrap (node1->network.endpoint ());
	ASSERT_TIMELY (15s, !node3->bootstrap_initiator.in_progress ());
	ASSERT_FALSE (node3->ledger.block_exists (send1->hash ()));
	ASSERT_FALSE (node3->ledger.block_exists (open1->hash ()));
	ASSERT_EQ (1, node3->stats.count (ysu::stat::type::bootstrap, ysu::stat::detail::frontier_confirmation_failed, ysu::stat::dir::in)); // failed request from node1
	ASSERT_TRUE (node3->network.excluded_peers.check (ysu::transport::map_endpoint_to_tcp (node1->network.endpoint ())));
}

TEST (bootstrap_processor, frontiers_confirmed)
{
	ysu::system system;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	node_config.tcp_io_timeout = std::chrono::seconds (2);
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_pull_server = true;
	node_flags.disable_bootstrap_bulk_push_client = true;
	node_flags.disable_legacy_bootstrap = true;
	node_flags.disable_lazy_bootstrap = true;
	node_flags.disable_wallet_bootstrap = true;
	node_flags.disable_rep_crawler = true;
	auto node1 = system.add_node (node_config, node_flags);
	ysu::genesis genesis;
	ysu::keypair key1, key2;
	// Generating valid chain
	auto send1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (genesis.hash ())));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send1).code);
	auto send2 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, send1->hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - 2 * ysu::Gxrb_ratio, key2.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (send1->hash ())));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send2).code);
	auto open1 (std::make_shared<ysu::state_block> (key1.pub, 0, key1.pub, ysu::Gxrb_ratio, send1->hash (), key1.prv, key1.pub, *system.work.generate (key1.pub)));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*open1).code);
	auto open2 (std::make_shared<ysu::state_block> (key2.pub, 0, key2.pub, ysu::Gxrb_ratio, send2->hash (), key2.prv, key2.pub, *system.work.generate (key2.pub)));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*open2).code);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);

	// Confirm all blocks so node1 is free to generate votes
	node1->block_confirm (send1);
	ASSERT_TIMELY (5s, node1->ledger.cache.cemented_count == 5);

	// Test node to bootstrap
	node_config.peering_port = ysu::get_available_port ();
	node_flags.disable_legacy_bootstrap = false;
	node_flags.disable_rep_crawler = false;
	auto node2 = system.add_node (node_config, node_flags);
	ASSERT_TIMELY (5s, node2->rep_crawler.representative_count () != 0);
	node2->bootstrap_initiator.bootstrap (node1->network.endpoint ());
	ASSERT_TIMELY (10s, node2->bootstrap_initiator.current_attempt () == nullptr || node2->bootstrap_initiator.current_attempt ()->frontiers_confirmed);
	ASSERT_EQ (1, node2->stats.count (ysu::stat::type::bootstrap, ysu::stat::detail::frontier_confirmation_successful, ysu::stat::dir::in)); // Successful request from node1
	ASSERT_EQ (0, node2->stats.count (ysu::stat::type::bootstrap, ysu::stat::detail::frontier_confirmation_failed, ysu::stat::dir::in));
}

TEST (bootstrap_processor, frontiers_unconfirmed_threshold)
{
	ysu::system system;
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	node_config.tcp_io_timeout = std::chrono::seconds (2);
	node_config.bootstrap_fraction_numerator = 4;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_pull_server = true;
	node_flags.disable_bootstrap_bulk_push_client = true;
	node_flags.disable_legacy_bootstrap = true;
	node_flags.disable_lazy_bootstrap = true;
	node_flags.disable_wallet_bootstrap = true;
	node_flags.disable_rep_crawler = true;
	auto node1 = system.add_node (node_config, node_flags);
	ysu::genesis genesis;
	ysu::keypair key1, key2;
	// Generating invalid chain
	auto threshold (node1->gap_cache.bootstrap_threshold () + 1);
	ASSERT_LT (threshold, node1->config.online_weight_minimum.number ());
	auto send1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - threshold, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (genesis.hash ())));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send1).code);
	auto send2 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, send1->hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - threshold - ysu::Gxrb_ratio, key2.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (send1->hash ())));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send2).code);
	auto open1 (std::make_shared<ysu::state_block> (key1.pub, 0, key1.pub, threshold, send1->hash (), key1.prv, key1.pub, *system.work.generate (key1.pub)));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*open1).code);
	auto open2 (std::make_shared<ysu::state_block> (key2.pub, 0, key2.pub, ysu::Gxrb_ratio, send2->hash (), key2.prv, key2.pub, *system.work.generate (key2.pub)));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*open2).code);
	system.wallet (0)->insert_adhoc (key1.prv); // Small representative

	// Test node with large representative
	node_config.peering_port = ysu::get_available_port ();
	auto node2 = system.add_node (node_config, node_flags);
	system.wallet (1)->insert_adhoc (ysu::dev_genesis_key.prv);

	// Test node to bootstrap
	node_config.peering_port = ysu::get_available_port ();
	node_flags.disable_legacy_bootstrap = false;
	node_flags.disable_rep_crawler = false;
	auto node3 = system.add_node (node_config, node_flags);
	ASSERT_EQ (ysu::process_result::progress, node3->process (*send1).code);
	ASSERT_EQ (ysu::process_result::progress, node3->process (*open1).code); // Change known representative weight
	ASSERT_TIMELY (5s, node3->rep_crawler.representative_count () == 2);
	node3->bootstrap_initiator.bootstrap (node1->network.endpoint ());
	ASSERT_TIMELY (15s, node3->stats.count (ysu::stat::type::bootstrap, ysu::stat::detail::frontier_confirmation_failed, ysu::stat::dir::in) == 1);
	ASSERT_FALSE (node3->ledger.block_exists (send2->hash ()));
	ASSERT_FALSE (node3->ledger.block_exists (open2->hash ()));
	ASSERT_EQ (1, node3->stats.count (ysu::stat::type::bootstrap, ysu::stat::detail::frontier_confirmation_failed, ysu::stat::dir::in)); // failed confirmation
	ASSERT_EQ (0, node3->stats.count (ysu::stat::type::bootstrap, ysu::stat::detail::frontier_confirmation_successful, ysu::stat::dir::in));
}

TEST (bootstrap_processor, push_diamond)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	auto node0 (system.add_node (config));
	ysu::keypair key;
	auto node1 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node1->init_error ());
	auto wallet1 (node1->wallets.create (100));
	wallet1->insert_adhoc (ysu::dev_genesis_key.prv);
	wallet1->insert_adhoc (key.prv);
	auto send1 (std::make_shared<ysu::send_block> (node0->latest (ysu::dev_genesis_key.pub), key.pub, 0, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (node0->latest (ysu::dev_genesis_key.pub))));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send1).code);
	auto open (std::make_shared<ysu::open_block> (send1->hash (), 1, key.pub, key.prv, key.pub, *system.work.generate (key.pub)));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*open).code);
	auto send2 (std::make_shared<ysu::send_block> (open->hash (), ysu::dev_genesis_key.pub, std::numeric_limits<ysu::uint128_t>::max () - 100, key.prv, key.pub, *system.work.generate (open->hash ())));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send2).code);
	auto receive (std::make_shared<ysu::receive_block> (send1->hash (), send2->hash (), ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (send1->hash ())));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*receive).code);
	node1->bootstrap_initiator.bootstrap (node0->network.endpoint ());
	ASSERT_TIMELY (10s, node0->balance (ysu::dev_genesis_key.pub) == 100);
	ASSERT_EQ (100, node0->balance (ysu::dev_genesis_key.pub));
	node1->stop ();
}

TEST (bootstrap_processor, push_one)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	auto node0 (system.add_node (config));
	ysu::keypair key1;
	auto node1 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	auto wallet (node1->wallets.create (ysu::random_wallet_id ()));
	ASSERT_NE (nullptr, wallet);
	wallet->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::uint128_t balance1 (node1->balance (ysu::dev_genesis_key.pub));
	ASSERT_NE (nullptr, wallet->send_action (ysu::dev_genesis_key.pub, key1.pub, 100));
	ASSERT_NE (balance1, node1->balance (ysu::dev_genesis_key.pub));
	node1->bootstrap_initiator.bootstrap (node0->network.endpoint ());
	ASSERT_TIMELY (10s, node0->balance (ysu::dev_genesis_key.pub) != balance1);
	node1->stop ();
}

TEST (bootstrap_processor, lazy_hash)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	auto node0 (system.add_node (config, node_flags));
	ysu::genesis genesis;
	ysu::keypair key1;
	ysu::keypair key2;
	// Generating test chain
	auto send1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *node0->work_generate_blocking (genesis.hash ())));
	auto receive1 (std::make_shared<ysu::state_block> (key1.pub, 0, key1.pub, ysu::Gxrb_ratio, send1->hash (), key1.prv, key1.pub, *node0->work_generate_blocking (key1.pub)));
	auto send2 (std::make_shared<ysu::state_block> (key1.pub, receive1->hash (), key1.pub, 0, key2.pub, key1.prv, key1.pub, *node0->work_generate_blocking (receive1->hash ())));
	auto receive2 (std::make_shared<ysu::state_block> (key2.pub, 0, key2.pub, ysu::Gxrb_ratio, send2->hash (), key2.prv, key2.pub, *node0->work_generate_blocking (key2.pub)));
	// Processing test chain
	node0->block_processor.add (send1);
	node0->block_processor.add (receive1);
	node0->block_processor.add (send2);
	node0->block_processor.add (receive2);
	node0->block_processor.flush ();
	// Start lazy bootstrap with last block in chain known
	auto node1 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	node1->network.udp_channels.insert (node0->network.endpoint (), node1->network_params.protocol.protocol_version);
	node1->bootstrap_initiator.bootstrap_lazy (receive2->hash (), true);
	{
		auto lazy_attempt (node1->bootstrap_initiator.current_lazy_attempt ());
		ASSERT_NE (nullptr, lazy_attempt);
		ASSERT_EQ (receive2->hash ().to_string (), lazy_attempt->id);
	}
	// Check processed blocks
	ASSERT_TIMELY (10s, node1->balance (key2.pub) != 0);
	node1->stop ();
}

TEST (bootstrap_processor, lazy_hash_bootstrap_id)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	auto node0 (system.add_node (config, node_flags));
	ysu::genesis genesis;
	ysu::keypair key1;
	ysu::keypair key2;
	// Generating test chain
	auto send1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *node0->work_generate_blocking (genesis.hash ())));
	auto receive1 (std::make_shared<ysu::state_block> (key1.pub, 0, key1.pub, ysu::Gxrb_ratio, send1->hash (), key1.prv, key1.pub, *node0->work_generate_blocking (key1.pub)));
	auto send2 (std::make_shared<ysu::state_block> (key1.pub, receive1->hash (), key1.pub, 0, key2.pub, key1.prv, key1.pub, *node0->work_generate_blocking (receive1->hash ())));
	auto receive2 (std::make_shared<ysu::state_block> (key2.pub, 0, key2.pub, ysu::Gxrb_ratio, send2->hash (), key2.prv, key2.pub, *node0->work_generate_blocking (key2.pub)));
	// Processing test chain
	node0->block_processor.add (send1);
	node0->block_processor.add (receive1);
	node0->block_processor.add (send2);
	node0->block_processor.add (receive2);
	node0->block_processor.flush ();
	// Start lazy bootstrap with last block in chain known
	auto node1 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	node1->network.udp_channels.insert (node0->network.endpoint (), node1->network_params.protocol.protocol_version);
	node1->bootstrap_initiator.bootstrap_lazy (receive2->hash (), true, true, "123456");
	{
		auto lazy_attempt (node1->bootstrap_initiator.current_lazy_attempt ());
		ASSERT_NE (nullptr, lazy_attempt);
		ASSERT_EQ ("123456", lazy_attempt->id);
	}
	// Check processed blocks
	ASSERT_TIMELY (10s, node1->balance (key2.pub) != 0);
	node1->stop ();
}

TEST (bootstrap_processor, lazy_max_pull_count)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	auto node0 (system.add_node (config, node_flags));
	ysu::genesis genesis;
	ysu::keypair key1;
	ysu::keypair key2;
	// Generating test chain
	auto send1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *node0->work_generate_blocking (genesis.hash ())));
	auto receive1 (std::make_shared<ysu::state_block> (key1.pub, 0, key1.pub, ysu::Gxrb_ratio, send1->hash (), key1.prv, key1.pub, *node0->work_generate_blocking (key1.pub)));
	auto send2 (std::make_shared<ysu::state_block> (key1.pub, receive1->hash (), key1.pub, 0, key2.pub, key1.prv, key1.pub, *node0->work_generate_blocking (receive1->hash ())));
	auto receive2 (std::make_shared<ysu::state_block> (key2.pub, 0, key2.pub, ysu::Gxrb_ratio, send2->hash (), key2.prv, key2.pub, *node0->work_generate_blocking (key2.pub)));
	auto change1 (std::make_shared<ysu::state_block> (key2.pub, receive2->hash (), key1.pub, ysu::Gxrb_ratio, 0, key2.prv, key2.pub, *node0->work_generate_blocking (receive2->hash ())));
	auto change2 (std::make_shared<ysu::state_block> (key2.pub, change1->hash (), ysu::dev_genesis_key.pub, ysu::Gxrb_ratio, 0, key2.prv, key2.pub, *node0->work_generate_blocking (change1->hash ())));
	auto change3 (std::make_shared<ysu::state_block> (key2.pub, change2->hash (), key2.pub, ysu::Gxrb_ratio, 0, key2.prv, key2.pub, *node0->work_generate_blocking (change2->hash ())));
	// Processing test chain
	node0->block_processor.add (send1);
	node0->block_processor.add (receive1);
	node0->block_processor.add (send2);
	node0->block_processor.add (receive2);
	node0->block_processor.add (change1);
	node0->block_processor.add (change2);
	node0->block_processor.add (change3);
	node0->block_processor.flush ();
	// Start lazy bootstrap with last block in chain known
	auto node1 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	node1->network.udp_channels.insert (node0->network.endpoint (), node1->network_params.protocol.protocol_version);
	node1->bootstrap_initiator.bootstrap_lazy (change3->hash ());
	// Check processed blocks
	ASSERT_TIMELY (10s, node1->block (change3->hash ()));

	node1->stop ();
}

TEST (bootstrap_processor, lazy_unclear_state_link)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	node_flags.disable_legacy_bootstrap = true;
	auto node1 = system.add_node (config, node_flags);
	ysu::genesis genesis;
	ysu::keypair key;
	// Generating test chain
	auto send1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (genesis.hash ())));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send1).code);
	auto send2 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, send1->hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - 2 * ysu::Gxrb_ratio, key.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (send1->hash ())));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send2).code);
	auto open (std::make_shared<ysu::open_block> (send1->hash (), key.pub, key.pub, key.prv, key.pub, *system.work.generate (key.pub)));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*open).code);
	auto receive (std::make_shared<ysu::state_block> (key.pub, open->hash (), key.pub, 2 * ysu::Gxrb_ratio, send2->hash (), key.prv, key.pub, *system.work.generate (open->hash ()))); // It is not possible to define this block send/receive status based on previous block (legacy open)
	ASSERT_EQ (ysu::process_result::progress, node1->process (*receive).code);
	// Start lazy bootstrap with last block in chain known
	auto node2 = system.add_node (ysu::node_config (ysu::get_available_port (), system.logging), node_flags);
	node2->network.udp_channels.insert (node1->network.endpoint (), node1->network_params.protocol.protocol_version);
	node2->bootstrap_initiator.bootstrap_lazy (receive->hash ());
	// Check processed blocks
	ASSERT_TIMELY (10s, !node2->bootstrap_initiator.in_progress ());
	node2->block_processor.flush ();
	ASSERT_TRUE (node2->ledger.block_exists (send1->hash ()));
	ASSERT_TRUE (node2->ledger.block_exists (send2->hash ()));
	ASSERT_TRUE (node2->ledger.block_exists (open->hash ()));
	ASSERT_TRUE (node2->ledger.block_exists (receive->hash ()));
	ASSERT_EQ (0, node2->stats.count (ysu::stat::type::bootstrap, ysu::stat::detail::bulk_pull_failed_account, ysu::stat::dir::in));
}

TEST (bootstrap_processor, lazy_unclear_state_link_not_existing)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	node_flags.disable_legacy_bootstrap = true;
	auto node1 = system.add_node (config, node_flags);
	ysu::genesis genesis;
	ysu::keypair key, key2;
	// Generating test chain
	auto send1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (genesis.hash ())));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send1).code);
	auto open (std::make_shared<ysu::open_block> (send1->hash (), key.pub, key.pub, key.prv, key.pub, *system.work.generate (key.pub)));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*open).code);
	auto send2 (std::make_shared<ysu::state_block> (key.pub, open->hash (), key.pub, 0, key2.pub, key.prv, key.pub, *system.work.generate (open->hash ()))); // It is not possible to define this block send/receive status based on previous block (legacy open)
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send2).code);
	// Start lazy bootstrap with last block in chain known
	auto node2 = system.add_node (ysu::node_config (ysu::get_available_port (), system.logging), node_flags);
	node2->network.udp_channels.insert (node1->network.endpoint (), node1->network_params.protocol.protocol_version);
	node2->bootstrap_initiator.bootstrap_lazy (send2->hash ());
	// Check processed blocks
	ASSERT_TIMELY (15s, !node2->bootstrap_initiator.in_progress ());
	node2->block_processor.flush ();
	ASSERT_TRUE (node2->ledger.block_exists (send1->hash ()));
	ASSERT_TRUE (node2->ledger.block_exists (open->hash ()));
	ASSERT_TRUE (node2->ledger.block_exists (send2->hash ()));
	ASSERT_EQ (1, node2->stats.count (ysu::stat::type::bootstrap, ysu::stat::detail::bulk_pull_failed_account, ysu::stat::dir::in));
}

TEST (bootstrap_processor, lazy_destinations)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	node_flags.disable_legacy_bootstrap = true;
	auto node1 = system.add_node (config, node_flags);
	ysu::genesis genesis;
	ysu::keypair key1, key2;
	// Generating test chain
	auto send1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (genesis.hash ())));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send1).code);
	auto send2 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, send1->hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - 2 * ysu::Gxrb_ratio, key2.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (send1->hash ())));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send2).code);
	auto open (std::make_shared<ysu::open_block> (send1->hash (), key1.pub, key1.pub, key1.prv, key1.pub, *system.work.generate (key1.pub)));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*open).code);
	auto state_open (std::make_shared<ysu::state_block> (key2.pub, 0, key2.pub, ysu::Gxrb_ratio, send2->hash (), key2.prv, key2.pub, *system.work.generate (key2.pub)));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*state_open).code);
	// Start lazy bootstrap with last block in sender chain
	auto node2 = system.add_node (ysu::node_config (ysu::get_available_port (), system.logging), node_flags);
	node2->network.udp_channels.insert (node1->network.endpoint (), node1->network_params.protocol.protocol_version);
	node2->bootstrap_initiator.bootstrap_lazy (send2->hash ());
	// Check processed blocks
	ASSERT_TIMELY (10s, !node2->bootstrap_initiator.in_progress ());
	node2->block_processor.flush ();
	ASSERT_TRUE (node2->ledger.block_exists (send1->hash ()));
	ASSERT_TRUE (node2->ledger.block_exists (send2->hash ()));
	ASSERT_TRUE (node2->ledger.block_exists (open->hash ()));
	ASSERT_TRUE (node2->ledger.block_exists (state_open->hash ()));
}

TEST (bootstrap_processor, wallet_lazy_frontier)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	node_flags.disable_legacy_bootstrap = true;
	auto node0 = system.add_node (config, node_flags);
	ysu::genesis genesis;
	ysu::keypair key1;
	ysu::keypair key2;
	// Generating test chain
	auto send1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *node0->work_generate_blocking (genesis.hash ())));
	auto receive1 (std::make_shared<ysu::state_block> (key1.pub, 0, key1.pub, ysu::Gxrb_ratio, send1->hash (), key1.prv, key1.pub, *node0->work_generate_blocking (key1.pub)));
	auto send2 (std::make_shared<ysu::state_block> (key1.pub, receive1->hash (), key1.pub, 0, key2.pub, key1.prv, key1.pub, *node0->work_generate_blocking (receive1->hash ())));
	auto receive2 (std::make_shared<ysu::state_block> (key2.pub, 0, key2.pub, ysu::Gxrb_ratio, send2->hash (), key2.prv, key2.pub, *node0->work_generate_blocking (key2.pub)));
	// Processing test chain
	node0->block_processor.add (send1);
	node0->block_processor.add (receive1);
	node0->block_processor.add (send2);
	node0->block_processor.add (receive2);
	node0->block_processor.flush ();
	// Start wallet lazy bootstrap
	auto node1 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	node1->network.udp_channels.insert (node0->network.endpoint (), node1->network_params.protocol.protocol_version);
	auto wallet (node1->wallets.create (ysu::random_wallet_id ()));
	ASSERT_NE (nullptr, wallet);
	wallet->insert_adhoc (key2.prv);
	node1->bootstrap_wallet ();
	{
		auto wallet_attempt (node1->bootstrap_initiator.current_wallet_attempt ());
		ASSERT_NE (nullptr, wallet_attempt);
		ASSERT_EQ (key2.pub.to_account (), wallet_attempt->id);
	}
	// Check processed blocks
	ASSERT_TIMELY (10s, node1->ledger.block_exists (receive2->hash ()));
	node1->stop ();
}

TEST (bootstrap_processor, wallet_lazy_pending)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	node_flags.disable_legacy_bootstrap = true;
	auto node0 = system.add_node (config, node_flags);
	ysu::genesis genesis;
	ysu::keypair key1;
	ysu::keypair key2;
	// Generating test chain
	auto send1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *node0->work_generate_blocking (genesis.hash ())));
	auto receive1 (std::make_shared<ysu::state_block> (key1.pub, 0, key1.pub, ysu::Gxrb_ratio, send1->hash (), key1.prv, key1.pub, *node0->work_generate_blocking (key1.pub)));
	auto send2 (std::make_shared<ysu::state_block> (key1.pub, receive1->hash (), key1.pub, 0, key2.pub, key1.prv, key1.pub, *node0->work_generate_blocking (receive1->hash ())));
	// Processing test chain
	node0->block_processor.add (send1);
	node0->block_processor.add (receive1);
	node0->block_processor.add (send2);
	node0->block_processor.flush ();
	// Start wallet lazy bootstrap
	auto node1 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	node1->network.udp_channels.insert (node0->network.endpoint (), node1->network_params.protocol.protocol_version);
	auto wallet (node1->wallets.create (ysu::random_wallet_id ()));
	ASSERT_NE (nullptr, wallet);
	wallet->insert_adhoc (key2.prv);
	node1->bootstrap_wallet ();
	// Check processed blocks
	ASSERT_TIMELY (10s, node1->ledger.block_exists (send2->hash ()));
	node1->stop ();
}

TEST (bootstrap_processor, multiple_attempts)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	auto node1 = system.add_node (config, node_flags);
	ysu::genesis genesis;
	ysu::keypair key1;
	ysu::keypair key2;
	// Generating test chain
	auto send1 (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *node1->work_generate_blocking (genesis.hash ())));
	auto receive1 (std::make_shared<ysu::state_block> (key1.pub, 0, key1.pub, ysu::Gxrb_ratio, send1->hash (), key1.prv, key1.pub, *node1->work_generate_blocking (key1.pub)));
	auto send2 (std::make_shared<ysu::state_block> (key1.pub, receive1->hash (), key1.pub, 0, key2.pub, key1.prv, key1.pub, *node1->work_generate_blocking (receive1->hash ())));
	auto receive2 (std::make_shared<ysu::state_block> (key2.pub, 0, key2.pub, ysu::Gxrb_ratio, send2->hash (), key2.prv, key2.pub, *node1->work_generate_blocking (key2.pub)));
	// Processing test chain
	node1->block_processor.add (send1);
	node1->block_processor.add (receive1);
	node1->block_processor.add (send2);
	node1->block_processor.add (receive2);
	node1->block_processor.flush ();
	// Start 2 concurrent bootstrap attempts
	ysu::node_config node_config (ysu::get_available_port (), system.logging);
	node_config.bootstrap_initiator_threads = 3;
	auto node2 (std::make_shared<ysu::node> (system.io_ctx, ysu::unique_path (), system.alarm, node_config, system.work));
	node2->network.udp_channels.insert (node1->network.endpoint (), node2->network_params.protocol.protocol_version);
	node2->bootstrap_initiator.bootstrap_lazy (receive2->hash (), true);
	node2->bootstrap_initiator.bootstrap ();
	auto lazy_attempt (node2->bootstrap_initiator.current_lazy_attempt ());
	auto legacy_attempt (node2->bootstrap_initiator.current_attempt ());
	ASSERT_TIMELY (5s, lazy_attempt->started && legacy_attempt->started);
	// Check that both bootstrap attempts are running & not finished
	ASSERT_FALSE (lazy_attempt->stopped);
	ASSERT_FALSE (legacy_attempt->stopped);
	ASSERT_GE (node2->bootstrap_initiator.attempts.size (), 2);
	// Check processed blocks
	ASSERT_TIMELY (10s, node2->balance (key2.pub) != 0);
	// Check attempts finish
	ASSERT_TIMELY (5s, node2->bootstrap_initiator.attempts.size () == 0);
	node2->stop ();
}

TEST (bootstrap_processor, bootstrap_fork)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	node_flags.disable_lazy_bootstrap = true;
	node_flags.disable_legacy_bootstrap = true;
	auto node0 (system.add_node (config, node_flags));
	ysu::keypair key;
	auto send (std::make_shared<ysu::state_block> (ysu::dev_genesis_key.pub, node0->latest (ysu::dev_genesis_key.pub), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (node0->latest (ysu::dev_genesis_key.pub))));
	ASSERT_EQ (ysu::process_result::progress, node0->process (*send).code);
	// Confirm send block to vote later
	node0->block_confirm (send);
	auto election = node0->active.election (send->qualified_root ());
	ASSERT_NE (nullptr, election);
	election->force_confirm ();
	ASSERT_TIMELY (2s, node0->block_confirmed (send->hash ()));
	node0->active.erase (*send);
	auto open_work (*system.work.generate (key.pub));
	auto open (std::make_shared<ysu::state_block> (key.pub, 0, key.pub, ysu::Gxrb_ratio, send->hash (), key.prv, key.pub, open_work));
	ASSERT_EQ (ysu::process_result::progress, node0->process (*open).code);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	// Create forked node
	config.peering_port = ysu::get_available_port ();
	node_flags.disable_legacy_bootstrap = false;
	auto node1 (system.add_node (config, node_flags));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*send).code);
	auto open_fork (std::make_shared<ysu::state_block> (key.pub, 0, ysu::dev_genesis_key.pub, ysu::Gxrb_ratio, send->hash (), key.prv, key.pub, open_work));
	ASSERT_EQ (ysu::process_result::progress, node1->process (*open_fork).code);
	// Resolve fork
	node1->bootstrap_initiator.bootstrap (node0->network.endpoint ());
	ASSERT_TIMELY (10s, node1->ledger.block_exists (open->hash ()));
	ASSERT_FALSE (node1->ledger.block_exists (open_fork->hash ()));
	node1->stop ();
}

TEST (frontier_req_response, DISABLED_destruction)
{
	{
		std::shared_ptr<ysu::frontier_req_server> hold; // Destructing tcp acceptor on non-existent io_context
		{
			ysu::system system (1);
			auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
			auto req = std::make_unique<ysu::frontier_req> ();
			req->start.clear ();
			req->age = std::numeric_limits<decltype (req->age)>::max ();
			req->count = std::numeric_limits<decltype (req->count)>::max ();
			connection->requests.push (std::unique_ptr<ysu::message>{});
			hold = std::make_shared<ysu::frontier_req_server> (connection, std::move (req));
		}
	}
	ASSERT_TRUE (true);
}

TEST (frontier_req, begin)
{
	ysu::system system (1);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	auto req = std::make_unique<ysu::frontier_req> ();
	req->start.clear ();
	req->age = std::numeric_limits<decltype (req->age)>::max ();
	req->count = std::numeric_limits<decltype (req->count)>::max ();
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::frontier_req_server> (connection, std::move (req)));
	ASSERT_EQ (ysu::dev_genesis_key.pub, request->current);
	ysu::genesis genesis;
	ASSERT_EQ (genesis.hash (), request->frontier);
}

TEST (frontier_req, end)
{
	ysu::system system (1);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	auto req = std::make_unique<ysu::frontier_req> ();
	req->start = ysu::dev_genesis_key.pub.number () + 1;
	req->age = std::numeric_limits<decltype (req->age)>::max ();
	req->count = std::numeric_limits<decltype (req->count)>::max ();
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::frontier_req_server> (connection, std::move (req)));
	ASSERT_TRUE (request->current.is_zero ());
}

TEST (frontier_req, count)
{
	ysu::system system (1);
	auto node1 = system.nodes[0];
	ysu::genesis genesis;
	// Public key FB93... after genesis in accounts table
	ysu::keypair key1 ("ED5AE0A6505B14B67435C29FD9FEEBC26F597D147BC92F6D795FFAD7AFD3D967");
	ysu::state_block send1 (ysu::dev_genesis_key.pub, genesis.hash (), ysu::dev_genesis_key.pub, ysu::genesis_amount - ysu::Gxrb_ratio, key1.pub, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, 0);
	node1->work_generate_blocking (send1);
	ASSERT_EQ (ysu::process_result::progress, node1->process (send1).code);
	ysu::state_block receive1 (key1.pub, 0, ysu::dev_genesis_key.pub, ysu::Gxrb_ratio, send1.hash (), key1.prv, key1.pub, 0);
	node1->work_generate_blocking (receive1);
	ASSERT_EQ (ysu::process_result::progress, node1->process (receive1).code);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, node1));
	auto req = std::make_unique<ysu::frontier_req> ();
	req->start.clear ();
	req->age = std::numeric_limits<decltype (req->age)>::max ();
	req->count = 1;
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::frontier_req_server> (connection, std::move (req)));
	ASSERT_EQ (ysu::dev_genesis_key.pub, request->current);
	ASSERT_EQ (send1.hash (), request->frontier);
}

TEST (frontier_req, time_bound)
{
	ysu::system system (1);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	auto req = std::make_unique<ysu::frontier_req> ();
	req->start.clear ();
	req->age = 1;
	req->count = std::numeric_limits<decltype (req->count)>::max ();
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::frontier_req_server> (connection, std::move (req)));
	ASSERT_EQ (ysu::dev_genesis_key.pub, request->current);
	// Wait 2 seconds until age of account will be > 1 seconds
	std::this_thread::sleep_for (std::chrono::milliseconds (2100));
	auto req2 (std::make_unique<ysu::frontier_req> ());
	req2->start.clear ();
	req2->age = 1;
	req2->count = std::numeric_limits<decltype (req2->count)>::max ();
	auto connection2 (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	connection2->requests.push (std::unique_ptr<ysu::message>{});
	auto request2 (std::make_shared<ysu::frontier_req_server> (connection, std::move (req2)));
	ASSERT_TRUE (request2->current.is_zero ());
}

TEST (frontier_req, time_cutoff)
{
	ysu::system system (1);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	auto req = std::make_unique<ysu::frontier_req> ();
	req->start.clear ();
	req->age = 3;
	req->count = std::numeric_limits<decltype (req->count)>::max ();
	connection->requests.push (std::unique_ptr<ysu::message>{});
	auto request (std::make_shared<ysu::frontier_req_server> (connection, std::move (req)));
	ASSERT_EQ (ysu::dev_genesis_key.pub, request->current);
	ysu::genesis genesis;
	ASSERT_EQ (genesis.hash (), request->frontier);
	// Wait 4 seconds until age of account will be > 3 seconds
	std::this_thread::sleep_for (std::chrono::milliseconds (4100));
	auto req2 (std::make_unique<ysu::frontier_req> ());
	req2->start.clear ();
	req2->age = 3;
	req2->count = std::numeric_limits<decltype (req2->count)>::max ();
	auto connection2 (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));
	connection2->requests.push (std::unique_ptr<ysu::message>{});
	auto request2 (std::make_shared<ysu::frontier_req_server> (connection, std::move (req2)));
	ASSERT_TRUE (request2->frontier.is_zero ());
}

TEST (bulk, genesis)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	node_flags.disable_lazy_bootstrap = true;
	auto node1 = system.add_node (config, node_flags);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	auto node2 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node2->init_error ());
	ysu::block_hash latest1 (node1->latest (ysu::dev_genesis_key.pub));
	ysu::block_hash latest2 (node2->latest (ysu::dev_genesis_key.pub));
	ASSERT_EQ (latest1, latest2);
	ysu::keypair key2;
	ASSERT_NE (nullptr, system.wallet (0)->send_action (ysu::dev_genesis_key.pub, key2.pub, 100));
	ysu::block_hash latest3 (node1->latest (ysu::dev_genesis_key.pub));
	ASSERT_NE (latest1, latest3);
	node2->bootstrap_initiator.bootstrap (node1->network.endpoint ());
	ASSERT_TIMELY (10s, node2->latest (ysu::dev_genesis_key.pub) == node1->latest (ysu::dev_genesis_key.pub));
	ASSERT_EQ (node2->latest (ysu::dev_genesis_key.pub), node1->latest (ysu::dev_genesis_key.pub));
	node2->stop ();
}

TEST (bulk, offline_send)
{
	ysu::system system;
	ysu::node_config config (ysu::get_available_port (), system.logging);
	config.frontiers_confirmation = ysu::frontiers_confirmation_mode::disabled;
	ysu::node_flags node_flags;
	node_flags.disable_bootstrap_bulk_push_client = true;
	node_flags.disable_lazy_bootstrap = true;
	auto node1 = system.add_node (config, node_flags);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	auto node2 (std::make_shared<ysu::node> (system.io_ctx, ysu::get_available_port (), ysu::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node2->init_error ());
	node2->start ();
	system.nodes.push_back (node2);
	ysu::keypair key2;
	auto wallet (node2->wallets.create (ysu::random_wallet_id ()));
	wallet->insert_adhoc (key2.prv);
	ASSERT_NE (nullptr, system.wallet (0)->send_action (ysu::dev_genesis_key.pub, key2.pub, node1->config.receive_minimum.number ()));
	ASSERT_NE (std::numeric_limits<ysu::uint256_t>::max (), node1->balance (ysu::dev_genesis_key.pub));
	// Wait to finish election background tasks
	ASSERT_TIMELY (10s, node1->active.empty ());
	// Initiate bootstrap
	node2->bootstrap_initiator.bootstrap (node1->network.endpoint ());
	// Nodes should find each other
	system.deadline_set (10s);
	do
	{
		ASSERT_NO_ERROR (system.poll ());
	} while (node1->network.empty () || node2->network.empty ());
	// Send block arrival via bootstrap
	ASSERT_TIMELY (10s, node2->balance (ysu::dev_genesis_key.pub) != std::numeric_limits<ysu::uint256_t>::max ());
	// Receiving send block
	ASSERT_TIMELY (20s, node2->balance (key2.pub) == node1->config.receive_minimum.number ());
	node2->stop ();
}

TEST (bulk_pull_account, basics)
{
	ysu::system system (1);
	system.nodes[0]->config.receive_minimum = 20;
	ysu::keypair key1;
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	system.wallet (0)->insert_adhoc (key1.prv);
	auto send1 (system.wallet (0)->send_action (ysu::genesis_account, key1.pub, 25));
	auto send2 (system.wallet (0)->send_action (ysu::genesis_account, key1.pub, 10));
	auto send3 (system.wallet (0)->send_action (ysu::genesis_account, key1.pub, 2));
	ASSERT_TIMELY (5s, system.nodes[0]->balance (key1.pub) == 25);
	auto connection (std::make_shared<ysu::bootstrap_server> (nullptr, system.nodes[0]));

	{
		auto req = std::make_unique<ysu::bulk_pull_account> ();
		req->account = key1.pub;
		req->minimum_amount = 5;
		req->flags = ysu::bulk_pull_account_flags ();
		connection->requests.push (std::unique_ptr<ysu::message>{});
		auto request (std::make_shared<ysu::bulk_pull_account_server> (connection, std::move (req)));
		ASSERT_FALSE (request->invalid_request);
		ASSERT_FALSE (request->pending_include_address);
		ASSERT_FALSE (request->pending_address_only);
		ASSERT_EQ (request->current_key.account, key1.pub);
		ASSERT_EQ (request->current_key.hash, 0);
		auto block_data (request->get_next ());
		ASSERT_EQ (send2->hash (), block_data.first.get ()->hash);
		ASSERT_EQ (ysu::uint128_union (10), block_data.second.get ()->amount);
		ASSERT_EQ (ysu::genesis_account, block_data.second.get ()->source);
		ASSERT_EQ (nullptr, request->get_next ().first.get ());
	}

	{
		auto req = std::make_unique<ysu::bulk_pull_account> ();
		req->account = key1.pub;
		req->minimum_amount = 0;
		req->flags = ysu::bulk_pull_account_flags::pending_address_only;
		auto request (std::make_shared<ysu::bulk_pull_account_server> (connection, std::move (req)));
		ASSERT_TRUE (request->pending_address_only);
		auto block_data (request->get_next ());
		ASSERT_NE (nullptr, block_data.first.get ());
		ASSERT_NE (nullptr, block_data.second.get ());
		ASSERT_EQ (ysu::genesis_account, block_data.second.get ()->source);
		block_data = request->get_next ();
		ASSERT_EQ (nullptr, block_data.first.get ());
		ASSERT_EQ (nullptr, block_data.second.get ());
	}
}
