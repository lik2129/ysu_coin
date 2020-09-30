#include <ysu/lib/jsonconfig.hpp>
#include <ysu/node/confirmation_solicitor.hpp>
#include <ysu/node/testing.hpp>
#include <ysu/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST (confirmation_solicitor, batches)
{
	ysu::system system;
	ysu::node_flags node_flags;
	node_flags.disable_request_loop = true;
	node_flags.disable_rep_crawler = true;
	node_flags.disable_udp = false;
	auto & node1 = *system.add_node (node_flags);
	node_flags.disable_request_loop = true;
	auto & node2 = *system.add_node (node_flags);
	auto channel1 (node2.network.udp_channels.create (node1.network.endpoint ()));
	// Solicitor will only solicit from this representative
	ysu::representative representative (ysu::dev_genesis_key.pub, ysu::genesis_amount, channel1);
	std::vector<ysu::representative> representatives{ representative };
	ysu::confirmation_solicitor solicitor (node2.network, node2.network_params.network);
	solicitor.prepare (representatives);
	// Ensure the representatives are correct
	ASSERT_EQ (1, representatives.size ());
	ASSERT_EQ (channel1, representatives.front ().channel);
	ASSERT_EQ (ysu::dev_genesis_key.pub, representatives.front ().account);
	ASSERT_TIMELY (3s, node2.network.size () == 1);
	auto send (std::make_shared<ysu::send_block> (ysu::genesis_hash, ysu::keypair ().pub, ysu::genesis_amount - 100, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (ysu::genesis_hash)));
	send->sideband_set ({});
	{
		ysu::lock_guard<std::mutex> guard (node2.active.mutex);
		for (size_t i (0); i < ysu::network::confirm_req_hashes_max; ++i)
		{
			auto election (std::make_shared<ysu::election> (node2, send, nullptr, false, ysu::election_behavior::normal));
			ASSERT_FALSE (solicitor.add (*election));
		}
		ASSERT_EQ (1, solicitor.max_confirm_req_batches);
		// Reached the maximum amount of requests for the channel
		auto election (std::make_shared<ysu::election> (node2, send, nullptr, false, ysu::election_behavior::normal));
		ASSERT_TRUE (solicitor.add (*election));
		// Broadcasting should be immediate
		ASSERT_EQ (0, node2.stats.count (ysu::stat::type::message, ysu::stat::detail::publish, ysu::stat::dir::out));
		ASSERT_FALSE (solicitor.broadcast (*election));
	}
	// One publish through directed broadcasting and another through random flooding
	ASSERT_EQ (2, node2.stats.count (ysu::stat::type::message, ysu::stat::detail::publish, ysu::stat::dir::out));
	solicitor.flush ();
	ASSERT_EQ (1, node2.stats.count (ysu::stat::type::message, ysu::stat::detail::confirm_req, ysu::stat::dir::out));
}

namespace ysu
{
TEST (confirmation_solicitor, different_hash)
{
	ysu::system system;
	ysu::node_flags node_flags;
	node_flags.disable_request_loop = true;
	node_flags.disable_rep_crawler = true;
	node_flags.disable_udp = false;
	auto & node1 = *system.add_node (node_flags);
	auto & node2 = *system.add_node (node_flags);
	auto channel1 (node2.network.udp_channels.create (node1.network.endpoint ()));
	// Solicitor will only solicit from this representative
	ysu::representative representative (ysu::dev_genesis_key.pub, ysu::genesis_amount, channel1);
	std::vector<ysu::representative> representatives{ representative };
	ysu::confirmation_solicitor solicitor (node2.network, node2.network_params.network);
	solicitor.prepare (representatives);
	// Ensure the representatives are correct
	ASSERT_EQ (1, representatives.size ());
	ASSERT_EQ (channel1, representatives.front ().channel);
	ASSERT_EQ (ysu::dev_genesis_key.pub, representatives.front ().account);
	ASSERT_TIMELY (3s, node2.network.size () == 1);
	auto send (std::make_shared<ysu::send_block> (ysu::genesis_hash, ysu::keypair ().pub, ysu::genesis_amount - 100, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (ysu::genesis_hash)));
	send->sideband_set ({});
	auto election (std::make_shared<ysu::election> (node2, send, nullptr, false, ysu::election_behavior::normal));
	// Add a vote for something else, not the winner
	election->last_votes[representative.account] = { std::chrono::steady_clock::now (), 1, 1 };
	// Ensure the request and broadcast goes through
	ASSERT_FALSE (solicitor.add (*election));
	ASSERT_FALSE (solicitor.broadcast (*election));
	// One publish through directed broadcasting and another through random flooding
	ASSERT_EQ (2, node2.stats.count (ysu::stat::type::message, ysu::stat::detail::publish, ysu::stat::dir::out));
	solicitor.flush ();
	ASSERT_EQ (1, node2.stats.count (ysu::stat::type::message, ysu::stat::detail::confirm_req, ysu::stat::dir::out));
}

TEST (confirmation_solicitor, bypass_max_requests_cap)
{
	ysu::system system;
	ysu::node_flags node_flags;
	node_flags.disable_request_loop = true;
	node_flags.disable_rep_crawler = true;
	node_flags.disable_udp = false;
	auto & node1 = *system.add_node (node_flags);
	auto & node2 = *system.add_node (node_flags);
	ysu::confirmation_solicitor solicitor (node2.network, node2.network_params.network);
	std::vector<ysu::representative> representatives;
	auto max_representatives = std::max<size_t> (solicitor.max_election_requests, solicitor.max_election_broadcasts);
	representatives.reserve (max_representatives + 1);
	for (auto i (0); i < max_representatives + 1; ++i)
	{
		auto channel (node2.network.udp_channels.create (node1.network.endpoint ()));
		ysu::representative representative (ysu::account (i), i, channel);
		representatives.push_back (representative);
	}
	ASSERT_EQ (max_representatives + 1, representatives.size ());
	solicitor.prepare (representatives);
	ASSERT_TIMELY (3s, node2.network.size () == 1);
	auto send (std::make_shared<ysu::send_block> (ysu::genesis_hash, ysu::keypair ().pub, ysu::genesis_amount - 100, ysu::dev_genesis_key.prv, ysu::dev_genesis_key.pub, *system.work.generate (ysu::genesis_hash)));
	send->sideband_set ({});
	auto election (std::make_shared<ysu::election> (node2, send, nullptr, false, ysu::election_behavior::normal));
	// Add a vote for something else, not the winner
	for (auto const & rep : representatives)
	{
		ysu::lock_guard<std::mutex> guard (node1.active.mutex);
		election->last_votes[rep.account] = { std::chrono::steady_clock::now (), 1, 1 };
	}
	ASSERT_FALSE (solicitor.add (*election));
	ASSERT_FALSE (solicitor.broadcast (*election));
	solicitor.flush ();
	// All requests went through, the last one would normally not go through due to the cap but a vote for a different hash does not count towards the cap
	ASSERT_EQ (max_representatives + 1, node2.stats.count (ysu::stat::type::message, ysu::stat::detail::confirm_req, ysu::stat::dir::out));

	solicitor.prepare (representatives);
	auto election2 (std::make_shared<ysu::election> (node2, send, nullptr, false, ysu::election_behavior::normal));
	ASSERT_FALSE (solicitor.add (*election2));
	ASSERT_FALSE (solicitor.broadcast (*election2));

	solicitor.flush ();
	// All requests but one went through, due to the cap
	ASSERT_EQ (2 * max_representatives + 1, node2.stats.count (ysu::stat::type::message, ysu::stat::detail::confirm_req, ysu::stat::dir::out));
}
}
