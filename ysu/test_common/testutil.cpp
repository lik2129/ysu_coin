#include <ysu/crypto_lib/random_pool.hpp>
#include <ysu/node/testing.hpp>
#include <ysu/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <numeric>

using namespace std::chrono_literals;

/* Convenience constants for tests which are always on the test network */
namespace
{
ysu::ledger_constants dev_constants (ysu::ysu_networks::ysu_dev_network);
}

ysu::keypair const & ysu::zero_key (dev_constants.zero_key);
ysu::keypair const & ysu::dev_genesis_key (dev_constants.dev_genesis_key);
ysu::account const & ysu::ysu_dev_account (dev_constants.ysu_dev_account);
std::string const & ysu::ysu_dev_genesis (dev_constants.ysu_dev_genesis);
ysu::account const & ysu::genesis_account (dev_constants.genesis_account);
ysu::block_hash const & ysu::genesis_hash (dev_constants.genesis_hash);
ysu::uint128_t const & ysu::genesis_amount (dev_constants.genesis_amount);
ysu::account const & ysu::burn_account (dev_constants.burn_account);

void ysu::wait_peer_connections (ysu::system & system_a)
{
	auto wait_peer_count = [&system_a](bool in_memory) {
		auto num_nodes = system_a.nodes.size ();
		system_a.deadline_set (20s);
		size_t peer_count = 0;
		while (peer_count != num_nodes * (num_nodes - 1))
		{
			ASSERT_NO_ERROR (system_a.poll ());
			peer_count = std::accumulate (system_a.nodes.cbegin (), system_a.nodes.cend (), std::size_t{ 0 }, [in_memory](auto total, auto const & node) {
				if (in_memory)
				{
					return total += node->network.size ();
				}
				else
				{
					auto transaction = node->store.tx_begin_read ();
					return total += node->store.peer_count (transaction);
				}
			});
		}
	};

	// Do a pre-pass with in-memory containers to reduce IO if still in the process of connecting to peers
	wait_peer_count (true);
	wait_peer_count (false);
}
