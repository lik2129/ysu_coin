#include <ysu/node/confirmation_solicitor.hpp>
#include <ysu/node/election.hpp>
#include <ysu/node/network.hpp>
#include <ysu/node/node.hpp>

#include <boost/format.hpp>

using namespace std::chrono;

std::chrono::milliseconds ysu::election::base_latency () const
{
	return node.network_params.network.is_dev_network () ? 25ms : 1000ms;
}

ysu::election_vote_result::election_vote_result (bool replay_a, bool processed_a)
{
	replay = replay_a;
	processed = processed_a;
}

ysu::election::election (ysu::node & node_a, std::shared_ptr<ysu::block> block_a, std::function<void(std::shared_ptr<ysu::block>)> const & confirmation_action_a, bool prioritized_a, ysu::election_behavior election_behavior_a) :
confirmation_action (confirmation_action_a),
prioritized_m (prioritized_a),
behavior (election_behavior_a),
node (node_a),
status ({ block_a, 0, std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now ().time_since_epoch ()), std::chrono::duration_values<std::chrono::milliseconds>::zero (), 0, 1, 0, ysu::election_status_type::ongoing }),
height (block_a->sideband ().height),
root (block_a->root ())
{
	last_votes.emplace (node.network_params.random.not_an_account, ysu::vote_info{ std::chrono::steady_clock::now (), 0, block_a->hash () });
	last_blocks.emplace (block_a->hash (), block_a);
}

void ysu::election::confirm_once (ysu::election_status_type type_a)
{
	debug_assert (!node.active.mutex.try_lock ());
	// This must be kept above the setting of election state, as dependent confirmed elections require up to date changes to election_winner_details
	ysu::unique_lock<std::mutex> election_winners_lk (node.active.election_winner_details_mutex);
	if (state_m.exchange (ysu::election::state_t::confirmed) != ysu::election::state_t::confirmed && (node.active.election_winner_details.count (status.winner->hash ()) == 0))
	{
		status.election_end = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now ().time_since_epoch ());
		status.election_duration = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now () - election_start);
		status.confirmation_request_count = confirmation_request_count;
		status.block_count = ysu::narrow_cast<decltype (status.block_count)> (last_blocks.size ());
		status.voter_count = ysu::narrow_cast<decltype (status.voter_count)> (last_votes.size ());
		status.type = type_a;
		auto status_l (status);
		auto node_l (node.shared ());
		auto confirmation_action_l (confirmation_action);
		node.active.election_winner_details.emplace (status.winner->hash (), shared_from_this ());
		node.active.add_recently_confirmed (status_l.winner->qualified_root (), status_l.winner->hash ());
		node.process_confirmed (status_l);
		node.background ([node_l, status_l, confirmation_action_l]() {
			if (confirmation_action_l)
			{
				confirmation_action_l (status_l.winner);
			}
		});
	}
}

bool ysu::election::valid_change (ysu::election::state_t expected_a, ysu::election::state_t desired_a) const
{
	bool result = false;
	switch (expected_a)
	{
		case ysu::election::state_t::passive:
			switch (desired_a)
			{
				case ysu::election::state_t::active:
				case ysu::election::state_t::confirmed:
				case ysu::election::state_t::expired_unconfirmed:
					result = true;
					break;
				default:
					break;
			}
			break;
		case ysu::election::state_t::active:
			switch (desired_a)
			{
				case ysu::election::state_t::broadcasting:
				case ysu::election::state_t::confirmed:
				case ysu::election::state_t::expired_unconfirmed:
					result = true;
					break;
				default:
					break;
			}
			break;
		case ysu::election::state_t::broadcasting:
			switch (desired_a)
			{
				case ysu::election::state_t::confirmed:
				case ysu::election::state_t::expired_unconfirmed:
					result = true;
					break;
				default:
					break;
			}
			break;
		case ysu::election::state_t::confirmed:
			switch (desired_a)
			{
				case ysu::election::state_t::expired_confirmed:
					result = true;
					break;
				default:
					break;
			}
			break;
		case ysu::election::state_t::expired_unconfirmed:
		case ysu::election::state_t::expired_confirmed:
			break;
	}
	return result;
}

bool ysu::election::state_change (ysu::election::state_t expected_a, ysu::election::state_t desired_a)
{
	debug_assert (!timepoints_mutex.try_lock ());
	bool result = true;
	if (valid_change (expected_a, desired_a))
	{
		if (state_m.compare_exchange_strong (expected_a, desired_a))
		{
			state_start = std::chrono::steady_clock::now ();
			result = false;
		}
	}
	else
	{
		debug_assert (false);
	}
	return result;
}

void ysu::election::send_confirm_req (ysu::confirmation_solicitor & solicitor_a)
{
	if ((base_latency () * (optimistic () ? 10 : 5)) < (std::chrono::steady_clock::now () - last_req))
	{
		if (!solicitor_a.add (*this))
		{
			last_req = std::chrono::steady_clock::now ();
			++confirmation_request_count;
		}
	}
}

void ysu::election::transition_active ()
{
	ysu::lock_guard<std::mutex> guard (timepoints_mutex);
	transition_active_impl ();
}

void ysu::election::transition_active_impl ()
{
	state_change (ysu::election::state_t::passive, ysu::election::state_t::active);
}

bool ysu::election::confirmed () const
{
	return state_m == ysu::election::state_t::confirmed || state_m == ysu::election::state_t::expired_confirmed;
}

bool ysu::election::failed () const
{
	return state_m == ysu::election::state_t::expired_unconfirmed;
}

void ysu::election::broadcast_block (ysu::confirmation_solicitor & solicitor_a)
{
	if (base_latency () * 15 < std::chrono::steady_clock::now () - last_block)
	{
		if (!solicitor_a.broadcast (*this))
		{
			last_block = std::chrono::steady_clock::now ();
		}
	}
}

bool ysu::election::transition_time (ysu::confirmation_solicitor & solicitor_a)
{
	debug_assert (!node.active.mutex.try_lock ());
	ysu::lock_guard<std::mutex> guard (timepoints_mutex);
	bool result = false;
	switch (state_m)
	{
		case ysu::election::state_t::passive:
			if (base_latency () * passive_duration_factor < std::chrono::steady_clock::now () - state_start)
			{
				state_change (ysu::election::state_t::passive, ysu::election::state_t::active);
			}
			break;
		case ysu::election::state_t::active:
			send_confirm_req (solicitor_a);
			if (confirmation_request_count > active_request_count_min)
			{
				state_change (ysu::election::state_t::active, ysu::election::state_t::broadcasting);
			}
			break;
		case ysu::election::state_t::broadcasting:
			broadcast_block (solicitor_a);
			send_confirm_req (solicitor_a);
			break;
		case ysu::election::state_t::confirmed:
			if (base_latency () * confirmed_duration_factor < std::chrono::steady_clock::now () - state_start)
			{
				result = true;
				state_change (ysu::election::state_t::confirmed, ysu::election::state_t::expired_confirmed);
			}
			break;
		case ysu::election::state_t::expired_unconfirmed:
		case ysu::election::state_t::expired_confirmed:
			debug_assert (false);
			break;
	}
	auto optimistic_expiration_time = node.network_params.network.is_dev_network () ? 500 : 60 * 1000;
	auto expire_time = std::chrono::milliseconds (optimistic () ? optimistic_expiration_time : 5 * 60 * 1000);
	if (!confirmed () && expire_time < std::chrono::steady_clock::now () - election_start)
	{
		result = true;
		state_change (state_m.load (), ysu::election::state_t::expired_unconfirmed);
		status.type = ysu::election_status_type::stopped;
		if (node.config.logging.election_expiration_tally_logging ())
		{
			log_votes (tally (), "Election expired: ");
		}
	}
	return result;
}

bool ysu::election::have_quorum (ysu::tally_t const & tally_a, ysu::uint128_t tally_sum) const
{
	bool result = false;
	if (tally_sum >= node.config.online_weight_minimum.number ())
	{
		auto i (tally_a.begin ());
		++i;
		auto second (i != tally_a.end () ? i->first : 0);
		auto delta_l (node.delta ());
		result = tally_a.begin ()->first > (second + delta_l);
	}
	return result;
}

ysu::tally_t ysu::election::tally ()
{
	std::unordered_map<ysu::block_hash, ysu::uint128_t> block_weights;
	for (auto const & [account, info] : last_votes)
	{
		block_weights[info.hash] += node.ledger.weight (account);
	}
	last_tally = block_weights;
	ysu::tally_t result;
	for (auto item : block_weights)
	{
		auto block (last_blocks.find (item.first));
		if (block != last_blocks.end ())
		{
			result.emplace (item.second, block->second);
		}
	}
	return result;
}

void ysu::election::confirm_if_quorum ()
{
	auto tally_l (tally ());
	debug_assert (!tally_l.empty ());
	auto winner (tally_l.begin ());
	auto block_l (winner->second);
	auto const & winner_hash_l (block_l->hash ());
	status.tally = winner->first;
	auto const & status_winner_hash_l (status.winner->hash ());
	ysu::uint128_t sum (0);
	for (auto & i : tally_l)
	{
		sum += i.first;
	}
	if (sum >= node.config.online_weight_minimum.number () && winner_hash_l != status_winner_hash_l)
	{
		status.winner = block_l;
		remove_votes (status_winner_hash_l);
		node.block_processor.force (block_l);
	}
	if (have_quorum (tally_l, sum))
	{
		if (node.config.logging.vote_logging () || (node.config.logging.election_fork_tally_logging () && last_blocks.size () > 1))
		{
			log_votes (tally_l);
		}
		confirm_once (ysu::election_status_type::active_confirmed_quorum);
	}
}

void ysu::election::log_votes (ysu::tally_t const & tally_a, std::string const & prefix_a) const
{
	std::stringstream tally;
	std::string line_end (node.config.logging.single_line_record () ? "\t" : "\n");
	tally << boost::str (boost::format ("%1%%2%Vote tally for root %3%") % prefix_a % line_end % root.to_string ());
	for (auto i (tally_a.begin ()), n (tally_a.end ()); i != n; ++i)
	{
		tally << boost::str (boost::format ("%1%Block %2% weight %3%") % line_end % i->second->hash ().to_string () % i->first.convert_to<std::string> ());
	}
	for (auto i (last_votes.begin ()), n (last_votes.end ()); i != n; ++i)
	{
		if (i->first != node.network_params.random.not_an_account)
		{
			tally << boost::str (boost::format ("%1%%2% %3% %4%") % line_end % i->first.to_account () % std::to_string (i->second.sequence) % i->second.hash.to_string ());
		}
	}
	node.logger.try_log (tally.str ());
}

ysu::election_vote_result ysu::election::vote (ysu::account rep, uint64_t sequence, ysu::block_hash block_hash)
{
	// see republish_vote documentation for an explanation of these rules
	auto replay (false);
	auto online_stake (node.online_reps.online_stake ());
	auto weight (node.ledger.weight (rep));
	auto should_process (false);
	if (node.network_params.network.is_dev_network () || weight > node.minimum_principal_weight (online_stake))
	{
		unsigned int cooldown;
		if (weight < online_stake / 100) // 0.1% to 1%
		{
			cooldown = 15;
		}
		else if (weight < online_stake / 20) // 1% to 5%
		{
			cooldown = 5;
		}
		else // 5% or above
		{
			cooldown = 1;
		}
		auto last_vote_it (last_votes.find (rep));
		if (last_vote_it == last_votes.end ())
		{
			should_process = true;
		}
		else
		{
			auto last_vote_l (last_vote_it->second);
			if (last_vote_l.sequence < sequence || (last_vote_l.sequence == sequence && last_vote_l.hash < block_hash))
			{
				if (last_vote_l.time <= std::chrono::steady_clock::now () - std::chrono::seconds (cooldown))
				{
					should_process = true;
				}
			}
			else
			{
				replay = true;
			}
		}
		if (should_process)
		{
			node.stats.inc (ysu::stat::type::election, ysu::stat::detail::vote_new);
			last_votes[rep] = { std::chrono::steady_clock::now (), sequence, block_hash };
			if (!confirmed ())
			{
				confirm_if_quorum ();
			}
		}
	}
	return ysu::election_vote_result (replay, should_process);
}

bool ysu::election::publish (std::shared_ptr<ysu::block> block_a)
{
	// Do not insert new blocks if already confirmed
	auto result (confirmed ());
	if (!result && last_blocks.size () >= 10)
	{
		if (last_tally[block_a->hash ()] < node.online_reps.online_stake () / 10)
		{
			result = true;
		}
	}
	if (!result)
	{
		auto existing = last_blocks.find (block_a->hash ());
		if (existing == last_blocks.end ())
		{
			last_blocks.emplace (std::make_pair (block_a->hash (), block_a));
			if (!insert_inactive_votes_cache (block_a->hash ()))
			{
				// Even if no votes were in cache, they could be in the election
				confirm_if_quorum ();
			}
			node.network.flood_block (block_a, ysu::buffer_drop_policy::no_limiter_drop);
		}
		else
		{
			result = true;
			existing->second = block_a;
			if (status.winner->hash () == block_a->hash ())
			{
				status.winner = block_a;
			}
		}
	}
	return result;
}

ysu::election_cleanup_info ysu::election::cleanup_info () const
{
	debug_assert (!node.active.mutex.try_lock ());
	return ysu::election_cleanup_info{
		confirmed (),
		status.winner->qualified_root (),
		status.winner->hash (),
		last_blocks
	};
}

size_t ysu::election::insert_inactive_votes_cache (ysu::block_hash const & hash_a)
{
	auto cache (node.active.find_inactive_votes_cache (hash_a));
	for (auto const & rep : cache.voters)
	{
		auto inserted (last_votes.emplace (rep, ysu::vote_info{ std::chrono::steady_clock::time_point::min (), 0, hash_a }));
		if (inserted.second)
		{
			node.stats.inc (ysu::stat::type::election, ysu::stat::detail::vote_cached);
		}
	}
	if (!confirmed () && !cache.voters.empty ())
	{
		auto delay (std::chrono::duration_cast<std::chrono::seconds> (std::chrono::steady_clock::now () - cache.arrival));
		if (delay > late_blocks_delay)
		{
			node.stats.inc (ysu::stat::type::election, ysu::stat::detail::late_block);
			node.stats.add (ysu::stat::type::election, ysu::stat::detail::late_block_seconds, ysu::stat::dir::in, delay.count (), true);
		}
		confirm_if_quorum ();
	}
	return cache.voters.size ();
}

bool ysu::election::prioritized () const
{
	return prioritized_m;
}

bool ysu::election::optimistic () const
{
	return behavior == ysu::election_behavior::optimistic;
}

void ysu::election::prioritize_election (ysu::vote_generator_session & generator_session_a)
{
	debug_assert (!node.active.mutex.try_lock ());
	debug_assert (!prioritized_m);
	prioritized_m = true;
	generator_session_a.add (root, status.winner->hash ());
}

std::shared_ptr<ysu::block> ysu::election::winner ()
{
	ysu::lock_guard<std::mutex> guard (node.active.mutex);
	return status.winner;
}

void ysu::election::generate_votes ()
{
	debug_assert (!node.active.mutex.try_lock ());
	if (node.config.enable_voting && node.wallets.reps ().voting > 0)
	{
		node.active.generator.add (root, status.winner->hash ());
	}
}

void ysu::election::remove_votes (ysu::block_hash const & hash_a)
{
	if (node.config.enable_voting && node.wallets.reps ().voting > 0)
	{
		// Remove votes from election
		auto list_generated_votes (node.history.votes (root, hash_a));
		for (auto const & vote : list_generated_votes)
		{
			last_votes.erase (vote->account);
		}
		// Clear votes cache
		node.history.erase (root);
	}
}

void ysu::election::force_confirm (ysu::election_status_type type_a)
{
	release_assert (node.network_params.network.is_dev_network ());
	ysu::lock_guard<std::mutex> guard (node.active.mutex);
	confirm_once (type_a);
}

std::unordered_map<ysu::block_hash, std::shared_ptr<ysu::block>> ysu::election::blocks ()
{
	debug_assert (node.network_params.network.is_dev_network ());
	ysu::lock_guard<std::mutex> guard (node.active.mutex);
	return last_blocks;
}

std::unordered_map<ysu::account, ysu::vote_info> ysu::election::votes ()
{
	debug_assert (node.network_params.network.is_dev_network ());
	ysu::lock_guard<std::mutex> guard (node.active.mutex);
	return last_votes;
}
