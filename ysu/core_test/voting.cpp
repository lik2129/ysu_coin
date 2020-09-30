#include <ysu/node/common.hpp>
#include <ysu/node/testing.hpp>
#include <ysu/node/voting.hpp>
#include <ysu/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace ysu
{
TEST (local_vote_history, basic)
{
	ysu::local_vote_history history;
	ASSERT_FALSE (history.exists (1));
	ASSERT_FALSE (history.exists (2));
	ASSERT_TRUE (history.votes (1).empty ());
	ASSERT_TRUE (history.votes (2).empty ());
	auto vote1a (std::make_shared<ysu::vote> ());
	ASSERT_EQ (0, history.size ());
	history.add (1, 2, vote1a);
	ASSERT_EQ (1, history.size ());
	ASSERT_TRUE (history.exists (1));
	ASSERT_FALSE (history.exists (2));
	auto votes1a (history.votes (1));
	ASSERT_FALSE (votes1a.empty ());
	ASSERT_EQ (1, history.votes (1, 2).size ());
	ASSERT_TRUE (history.votes (1, 1).empty ());
	ASSERT_TRUE (history.votes (1, 3).empty ());
	ASSERT_TRUE (history.votes (2).empty ());
	ASSERT_EQ (1, votes1a.size ());
	ASSERT_EQ (vote1a, votes1a[0]);
	auto vote1b (std::make_shared<ysu::vote> ());
	history.add (1, 2, vote1b);
	auto votes1b (history.votes (1));
	ASSERT_EQ (1, votes1b.size ());
	ASSERT_EQ (vote1b, votes1b[0]);
	ASSERT_NE (vote1a, votes1b[0]);
	auto vote2 (std::make_shared<ysu::vote> ());
	vote2->account.dwords[0]++;
	ASSERT_EQ (1, history.size ());
	history.add (1, 2, vote2);
	ASSERT_EQ (2, history.size ());
	auto votes2 (history.votes (1));
	ASSERT_EQ (2, votes2.size ());
	ASSERT_TRUE (vote1b == votes2[0] || vote1b == votes2[1]);
	ASSERT_TRUE (vote2 == votes2[0] || vote2 == votes2[1]);
	auto vote3 (std::make_shared<ysu::vote> ());
	vote3->account.dwords[1]++;
	history.add (1, 3, vote3);
	ASSERT_EQ (1, history.size ());
	auto votes3 (history.votes (1));
	ASSERT_EQ (1, votes3.size ());
	ASSERT_TRUE (vote3 == votes3[0]);
}
}

TEST (vote_generator, cache)
{
	ysu::system system (1);
	auto & node (*system.nodes[0]);
	auto epoch1 = system.upgrade_genesis_epoch (node, ysu::epoch::epoch_1);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	node.active.generator.add (epoch1->root (), epoch1->hash ());
	ASSERT_TIMELY (1s, !node.history.votes (epoch1->root (), epoch1->hash ()).empty ());
	auto votes (node.history.votes (epoch1->root (), epoch1->hash ()));
	ASSERT_FALSE (votes.empty ());
	ASSERT_TRUE (std::any_of (votes[0]->begin (), votes[0]->end (), [hash = epoch1->hash ()](ysu::block_hash const & hash_a) { return hash_a == hash; }));
}

TEST (vote_generator, multiple_representatives)
{
	ysu::system system (1);
	auto & node (*system.nodes[0]);
	ysu::keypair key1, key2, key3;
	auto & wallet (*system.wallet (0));
	wallet.insert_adhoc (ysu::dev_genesis_key.prv);
	wallet.insert_adhoc (key1.prv);
	wallet.insert_adhoc (key2.prv);
	wallet.insert_adhoc (key3.prv);
	auto const amount = 100 * ysu::Gxrb_ratio;
	wallet.send_sync (ysu::dev_genesis_key.pub, key1.pub, amount);
	wallet.send_sync (ysu::dev_genesis_key.pub, key2.pub, amount);
	wallet.send_sync (ysu::dev_genesis_key.pub, key3.pub, amount);
	ASSERT_TIMELY (3s, node.balance (key1.pub) == amount && node.balance (key2.pub) == amount && node.balance (key3.pub) == amount);
	wallet.change_sync (key1.pub, key1.pub);
	wallet.change_sync (key2.pub, key2.pub);
	wallet.change_sync (key3.pub, key3.pub);
	ASSERT_TRUE (node.weight (key1.pub) == amount && node.weight (key2.pub) == amount && node.weight (key3.pub) == amount);
	node.wallets.compute_reps ();
	ASSERT_EQ (4, node.wallets.reps ().voting);
	auto hash = wallet.send_sync (ysu::dev_genesis_key.pub, ysu::dev_genesis_key.pub, 1);
	auto send = node.block (hash);
	ASSERT_NE (nullptr, send);
	ASSERT_TIMELY (5s, node.history.votes (send->root (), send->hash ()).size () == 4);
	auto votes (node.history.votes (send->root (), send->hash ()));
	for (auto const & account : { key1.pub, key2.pub, key3.pub, ysu::dev_genesis_key.pub })
	{
		auto existing (std::find_if (votes.begin (), votes.end (), [&account](std::shared_ptr<ysu::vote> const & vote_a) -> bool {
			return vote_a->account == account;
		}));
		ASSERT_NE (votes.end (), existing);
	}
}

TEST (vote_generator, session)
{
	ysu::system system (1);
	auto node (system.nodes[0]);
	system.wallet (0)->insert_adhoc (ysu::dev_genesis_key.prv);
	ysu::vote_generator_session generator_session (node->active.generator);
	boost::thread thread ([node, &generator_session]() {
		ysu::thread_role::set (ysu::thread_role::name::request_loop);
		generator_session.add (ysu::genesis_account, ysu::genesis_hash);
		ASSERT_EQ (0, node->stats.count (ysu::stat::type::vote, ysu::stat::detail::vote_indeterminate));
		generator_session.flush ();
	});
	thread.join ();
	ASSERT_TIMELY (2s, 1 == node->stats.count (ysu::stat::type::vote, ysu::stat::detail::vote_indeterminate));
}
