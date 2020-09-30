#include <ysu/lib/stats.hpp>
#include <ysu/lib/threading.hpp>
#include <ysu/lib/utility.hpp>
#include <ysu/node/network.hpp>
#include <ysu/node/nodeconfig.hpp>
#include <ysu/node/vote_processor.hpp>
#include <ysu/node/voting.hpp>
#include <ysu/node/wallet.hpp>
#include <ysu/secure/blockstore.hpp>
#include <ysu/secure/ledger.hpp>

#include <chrono>

bool ysu::local_vote_history::consistency_check (ysu::root const & root_a) const
{
	auto & history_by_root (history.get<tag_root> ());
	auto const range (history_by_root.equal_range (root_a));
	// All cached votes for a root must be for the same hash, this is actively enforced in local_vote_history::add
	auto consistent = std::all_of (range.first, range.second, [hash = range.first->hash](auto const & info_a) { return info_a.hash == hash; });
	std::vector<ysu::account> accounts;
	std::transform (range.first, range.second, std::back_inserter (accounts), [](auto const & info_a) { return info_a.vote->account; });
	std::sort (accounts.begin (), accounts.end ());
	// All cached votes must be unique by account, this is actively enforced in local_vote_history::add
	consistent = consistent && accounts.size () == std::unique (accounts.begin (), accounts.end ()) - accounts.begin ();
	return consistent;
}

void ysu::local_vote_history::add (ysu::root const & root_a, ysu::block_hash const & hash_a, std::shared_ptr<ysu::vote> const & vote_a)
{
	ysu::lock_guard<std::mutex> guard (mutex);
	clean ();
	auto & history_by_root (history.get<tag_root> ());
	// Erase any vote that is not for this hash, or duplicate by account
	auto range (history_by_root.equal_range (root_a));
	for (auto i (range.first); i != range.second;)
	{
		if (i->hash != hash_a || vote_a->account == i->vote->account)
		{
			i = history_by_root.erase (i);
		}
		else
		{
			++i;
		}
	}
	auto result (history_by_root.emplace (root_a, hash_a, vote_a));
	(void)result;
	debug_assert (result.second);
	debug_assert (consistency_check (root_a));
}

void ysu::local_vote_history::erase (ysu::root const & root_a)
{
	ysu::lock_guard<std::mutex> guard (mutex);
	auto & history_by_root (history.get<tag_root> ());
	auto range (history_by_root.equal_range (root_a));
	history_by_root.erase (range.first, range.second);
}

std::vector<std::shared_ptr<ysu::vote>> ysu::local_vote_history::votes (ysu::root const & root_a) const
{
	ysu::lock_guard<std::mutex> guard (mutex);
	std::vector<std::shared_ptr<ysu::vote>> result;
	auto range (history.get<tag_root> ().equal_range (root_a));
	std::transform (range.first, range.second, std::back_inserter (result), [](auto const & entry) { return entry.vote; });
	return result;
}

std::vector<std::shared_ptr<ysu::vote>> ysu::local_vote_history::votes (ysu::root const & root_a, ysu::block_hash const & hash_a) const
{
	ysu::lock_guard<std::mutex> guard (mutex);
	std::vector<std::shared_ptr<ysu::vote>> result;
	auto range (history.get<tag_root> ().equal_range (root_a));
	// clang-format off
	ysu::transform_if (range.first, range.second, std::back_inserter (result),
		[&hash_a](auto const & entry) { return entry.hash == hash_a; },
		[](auto const & entry) { return entry.vote; });
	// clang-format on
	return result;
}

bool ysu::local_vote_history::exists (ysu::root const & root_a) const
{
	ysu::lock_guard<std::mutex> guard (mutex);
	return history.get<tag_root> ().find (root_a) != history.get<tag_root> ().end ();
}

void ysu::local_vote_history::clean ()
{
	debug_assert (max_size > 0);
	auto & history_by_sequence (history.get<tag_sequence> ());
	while (history_by_sequence.size () > max_size)
	{
		history_by_sequence.erase (history_by_sequence.begin ());
	}
}

size_t ysu::local_vote_history::size () const
{
	ysu::lock_guard<std::mutex> guard (mutex);
	return history.size ();
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (ysu::local_vote_history & history, const std::string & name)
{
	size_t history_count = history.size ();
	auto sizeof_element = sizeof (decltype (history.history)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	/* This does not currently loop over each element inside the cache to get the sizes of the votes inside history*/
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "history", history_count, sizeof_element }));
	return composite;
}

ysu::vote_generator::vote_generator (ysu::node_config const & config_a, ysu::ledger & ledger_a, ysu::wallets & wallets_a, ysu::vote_processor & vote_processor_a, ysu::local_vote_history & history_a, ysu::network & network_a, ysu::stat & stats_a) :
config (config_a),
ledger (ledger_a),
wallets (wallets_a),
vote_processor (vote_processor_a),
history (history_a),
network (network_a),
stats (stats_a),
thread ([this]() { run (); })
{
	ysu::unique_lock<std::mutex> lock (mutex);
	condition.wait (lock, [& started = started] { return started; });
}

void ysu::vote_generator::add (ysu::root const & root_a, ysu::block_hash const & hash_a)
{
	auto votes (history.votes (root_a, hash_a));
	if (!votes.empty ())
	{
		for (auto const & vote : votes)
		{
			broadcast_action (vote);
		}
	}
	else
	{
		auto transaction (ledger.store.tx_begin_read ());
		auto block (ledger.store.block_get (transaction, hash_a));
		if (block != nullptr && ledger.dependents_confirmed (transaction, *block))
		{
			ysu::unique_lock<std::mutex> lock (mutex);
			candidates.emplace_back (root_a, hash_a);
			if (candidates.size () >= ysu::network::confirm_ack_hashes_max)
			{
				lock.unlock ();
				condition.notify_all ();
			}
		}
	}
}

void ysu::vote_generator::stop ()
{
	ysu::unique_lock<std::mutex> lock (mutex);
	stopped = true;

	lock.unlock ();
	condition.notify_all ();

	if (thread.joinable ())
	{
		thread.join ();
	}
}

size_t ysu::vote_generator::generate (std::vector<std::shared_ptr<ysu::block>> const & blocks_a, std::shared_ptr<ysu::transport::channel> const & channel_a)
{
	request_t::first_type candidates;
	{
		auto transaction (ledger.store.tx_begin_read ());
		auto dependents_confirmed = [&transaction, this](auto const & block_a) {
			return this->ledger.dependents_confirmed (transaction, *block_a);
		};
		auto as_candidate = [](auto const & block_a) {
			return candidate_t{ block_a->root (), block_a->hash () };
		};
		ysu::transform_if (blocks_a.begin (), blocks_a.end (), std::back_inserter (candidates), dependents_confirmed, as_candidate);
	}
	auto const result = candidates.size ();
	ysu::lock_guard<std::mutex> guard (mutex);
	requests.emplace_back (std::move (candidates), channel_a);
	while (requests.size () > max_requests)
	{
		// On a large queue of requests, erase the oldest one
		requests.pop_front ();
		stats.inc (ysu::stat::type::vote_generator, ysu::stat::detail::generator_replies_discarded);
	}
	return result;
}

void ysu::vote_generator::set_reply_action (std::function<void(std::shared_ptr<ysu::vote> const &, std::shared_ptr<ysu::transport::channel> &)> action_a)
{
	release_assert (!reply_action);
	reply_action = action_a;
}

void ysu::vote_generator::broadcast (ysu::unique_lock<std::mutex> & lock_a)
{
	debug_assert (lock_a.owns_lock ());
	std::unordered_set<std::shared_ptr<ysu::vote>> cached_sent;
	std::vector<ysu::block_hash> hashes;
	std::vector<ysu::root> roots;
	hashes.reserve (ysu::network::confirm_ack_hashes_max);
	roots.reserve (ysu::network::confirm_ack_hashes_max);
	while (!candidates.empty () && hashes.size () < ysu::network::confirm_ack_hashes_max)
	{
		auto const & [root, hash] = candidates.front ();
		auto cached_votes = history.votes (root, hash);
		for (auto const & cached_vote : cached_votes)
		{
			if (cached_sent.insert (cached_vote).second)
			{
				broadcast_action (cached_vote);
			}
		}
		if (cached_votes.empty ())
		{
			roots.push_back (root);
			hashes.push_back (hash);
		}
		candidates.pop_front ();
	}
	if (!hashes.empty ())
	{
		lock_a.unlock ();
		vote (hashes, roots, [this](auto const & vote_a) { this->broadcast_action (vote_a); });
		lock_a.lock ();
	}
	stats.inc (ysu::stat::type::vote_generator, ysu::stat::detail::generator_broadcasts);
}

void ysu::vote_generator::reply (ysu::unique_lock<std::mutex> & lock_a, request_t && request_a)
{
	lock_a.unlock ();
	std::unordered_set<std::shared_ptr<ysu::vote>> cached_sent;
	auto transaction (ledger.store.tx_begin_read ());
	auto i (request_a.first.cbegin ());
	auto n (request_a.first.cend ());
	while (i != n && !stopped)
	{
		std::vector<ysu::block_hash> hashes;
		std::vector<ysu::root> roots;
		hashes.reserve (ysu::network::confirm_ack_hashes_max);
		roots.reserve (ysu::network::confirm_ack_hashes_max);
		for (; i != n && hashes.size () < ysu::network::confirm_ack_hashes_max; ++i)
		{
			auto cached_votes = history.votes (i->first, i->second);
			for (auto const & cached_vote : cached_votes)
			{
				if (cached_sent.insert (cached_vote).second)
				{
					stats.add (ysu::stat::type::requests, ysu::stat::detail::requests_cached_late_hashes, stat::dir::in, cached_vote->blocks.size ());
					stats.inc (ysu::stat::type::requests, ysu::stat::detail::requests_cached_late_votes, stat::dir::in);
					reply_action (cached_vote, request_a.second);
				}
			}
			if (cached_votes.empty ())
			{
				roots.push_back (i->first);
				hashes.push_back (i->second);
			}
		}
		if (!hashes.empty ())
		{
			stats.add (ysu::stat::type::requests, ysu::stat::detail::requests_generated_hashes, stat::dir::in, hashes.size ());
			vote (hashes, roots, [this, &channel = request_a.second](std::shared_ptr<ysu::vote> const & vote_a) {
				this->reply_action (vote_a, channel);
				this->stats.inc (ysu::stat::type::requests, ysu::stat::detail::requests_generated_votes, stat::dir::in);
			});
		}
	}
	stats.inc (ysu::stat::type::vote_generator, ysu::stat::detail::generator_replies);
	lock_a.lock ();
}

void ysu::vote_generator::vote (std::vector<ysu::block_hash> const & hashes_a, std::vector<ysu::root> const & roots_a, std::function<void(std::shared_ptr<ysu::vote> const &)> const & action_a)
{
	debug_assert (hashes_a.size () == roots_a.size ());
	std::vector<std::shared_ptr<ysu::vote>> votes_l;
	{
		auto transaction (ledger.store.tx_begin_read ());
		wallets.foreach_representative ([this, &hashes_a, &transaction, &votes_l](ysu::public_key const & pub_a, ysu::raw_key const & prv_a) {
			votes_l.emplace_back (this->ledger.store.vote_generate (transaction, pub_a, prv_a, hashes_a));
		});
	}
	for (auto const & vote_l : votes_l)
	{
		for (size_t i (0), n (hashes_a.size ()); i != n; ++i)
		{
			history.add (roots_a[i], hashes_a[i], vote_l);
		}
		action_a (vote_l);
	}
}

void ysu::vote_generator::broadcast_action (std::shared_ptr<ysu::vote> const & vote_a) const
{
	network.flood_vote_pr (vote_a);
	network.flood_vote (vote_a, 2.0f);
	vote_processor.vote (vote_a, std::make_shared<ysu::transport::channel_loopback> (network.node));
}

void ysu::vote_generator::run ()
{
	ysu::thread_role::set (ysu::thread_role::name::voting);
	ysu::unique_lock<std::mutex> lock (mutex);
	started = true;
	lock.unlock ();
	condition.notify_all ();
	lock.lock ();
	while (!stopped)
	{
		if (candidates.size () >= ysu::network::confirm_ack_hashes_max)
		{
			broadcast (lock);
		}
		else if (!requests.empty ())
		{
			auto request (requests.front ());
			requests.pop_front ();
			reply (lock, std::move (request));
		}
		else
		{
			condition.wait_for (lock, config.vote_generator_delay, [this]() { return this->candidates.size () >= ysu::network::confirm_ack_hashes_max; });
			if (candidates.size () >= config.vote_generator_threshold && candidates.size () < ysu::network::confirm_ack_hashes_max)
			{
				condition.wait_for (lock, config.vote_generator_delay, [this]() { return this->candidates.size () >= ysu::network::confirm_ack_hashes_max; });
			}
			if (!candidates.empty ())
			{
				broadcast (lock);
			}
		}
	}
}

ysu::vote_generator_session::vote_generator_session (ysu::vote_generator & vote_generator_a) :
generator (vote_generator_a)
{
}

void ysu::vote_generator_session::add (ysu::root const & root_a, ysu::block_hash const & hash_a)
{
	debug_assert (ysu::thread_role::get () == ysu::thread_role::name::request_loop);
	items.emplace_back (root_a, hash_a);
}

void ysu::vote_generator_session::flush ()
{
	debug_assert (ysu::thread_role::get () == ysu::thread_role::name::request_loop);
	for (auto const & [root, hash] : items)
	{
		generator.add (root, hash);
	}
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (ysu::vote_generator & vote_generator, const std::string & name)
{
	size_t candidates_count = 0;
	size_t requests_count = 0;
	{
		ysu::lock_guard<std::mutex> guard (vote_generator.mutex);
		candidates_count = vote_generator.candidates.size ();
		requests_count = vote_generator.requests.size ();
	}
	auto sizeof_candidate_element = sizeof (decltype (vote_generator.candidates)::value_type);
	auto sizeof_request_element = sizeof (decltype (vote_generator.requests)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "candidates", candidates_count, sizeof_candidate_element }));
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "requests", requests_count, sizeof_request_element }));
	return composite;
}
