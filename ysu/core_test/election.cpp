#include <ysu/node/election.hpp>
#include <ysu/node/testing.hpp>
#include <ysu/test_common/testutil.hpp>

#include <gtest/gtest.h>

TEST (election, construction)
{
	ysu::system system (1);
	ysu::genesis genesis;
	auto & node = *system.nodes[0];
	genesis.open->sideband_set (ysu::block_sideband (ysu::genesis_account, 0, ysu::genesis_amount, 1, ysu::seconds_since_epoch (), ysu::epoch::epoch_0, false, false, false, ysu::epoch::epoch_0));
	auto election = node.active.insert (genesis.open).election;
	election->transition_active ();
}
