#include <ysu/lib/stats.hpp>
#include <ysu/lib/threading.hpp>
#include <ysu/node/active_transactions.hpp>
#include <ysu/node/common.hpp>
#include <ysu/node/network.hpp>
#include <ysu/node/nodeconfig.hpp>
#include <ysu/node/request_aggregator.hpp>
#include <ysu/node/transport/udp.hpp>
#include <ysu/node/voting.hpp>
#include <ysu/node/wallet.hpp>
#include <ysu/secure/blockstore.hpp>
#include <ysu/secure/ledger.hpp>

ysu::request_aggregator::request_aggregator (ysu::network_constants const & network_constants_a, ysu::node_config const & config_a, ysu::stat & stats_a, ysu::vote_generator & generator_a, ysu::local_vote_history & history_a, ysu::ledger & ledger_a, ysu::wallets & wallets_a, ysu::active_transactions & active_a) :
max_delay (network_constants_a.is_dev_network () ? 50 : 300),
small_delay (network_constants_a.is_dev_network () ? 10 : 50),
max_channel_requests (config_a.max_queued_requests),
stats (stats_a),
local_votes (history_a),
ledger (ledger_a),
wallets (wallets_a),
active (active_a),
generator (generator_a),
thread ([this]() { run (); })
{
	generator.set_reply_action ([this](std::shared_ptr<ysu::vote> const & vote_a, std::shared_ptr<ysu::transport::channel> & channel_a) {
		this->reply_action (vote_a, channel_a);
	});
	ysu::unique_lock<std::mutex> lock (mutex);
	condition.wait (lock, [& started = started] { return started; });
}

void ysu::request_aggregator::add (std::shared_ptr<ysu::transport::channel> & channel_a, std::vector<std::pair<ysu::block_hash, ysu::root>> const & hashes_roots_a)
{
	debug_assert (wallets.reps ().voting > 0);
	bool error = true;
	auto const endpoint (ysu::transport::map_endpoint_to_v6 (channel_a->get_endpoint ()));
	ysu::unique_lock<std::mutex> lock (mutex);
	// Protecting from ever-increasing memory usage when request are consumed slower than generated
	// Reject request if the oldest request has not yet been processed after its deadline + a modest margin
	if (requests.empty () || (requests.get<tag_deadline> ().begin ()->deadline + 2 * this->max_delay > std::chrono::steady_clock::now ()))
	{
		auto & requests_by_endpoint (requests.get<tag_endpoint> ());
		auto existing (requests_by_endpoint.find (endpoint));
		if (existing == requests_by_endpoint.end ())
		{
			existing = requests_by_endpoint.emplace (channel_a).first;
		}
		requests_by_endpoint.modify (existing, [&hashes_roots_a, &channel_a, &error, this](channel_pool & pool_a) {
			// This extends the lifetime of the channel, which is acceptable up to max_delay
			pool_a.channel = channel_a;
			if (pool_a.hashes_roots.size () + hashes_roots_a.size () <= this->max_channel_requests)
			{
				error = false;
				auto new_deadline (std::min (pool_a.start + this->max_delay, std::chrono::steady_clock::now () + this->small_delay));
				pool_a.deadline = new_deadline;
				pool_a.hashes_roots.insert (pool_a.hashes_roots.begin (), hashes_roots_a.begin (), hashes_roots_a.end ());
			}
		});
		if (requests.size () == 1)
		{
			lock.unlock ();
			condition.notify_all ();
		}
	}
	stats.inc (ysu::stat::type::aggregator, !error ? ysu::stat::detail::aggregator_accepted : ysu::stat::detail::aggregator_dropped);
}

void ysu::request_aggregator::run ()
{
	ysu::thread_role::set (ysu::thread_role::name::request_aggregator);
	ysu::unique_lock<std::mutex> lock (mutex);
	started = true;
	lock.unlock ();
	condition.notify_all ();
	lock.lock ();
	while (!stopped)
	{
		if (!requests.empty ())
		{
			auto & requests_by_deadline (requests.get<tag_deadline> ());
			auto front (requests_by_deadline.begin ());
			if (front->deadline < std::chrono::steady_clock::now ())
			{
				// Store the channel and requests for processing after erasing this pool
				decltype (front->channel) channel{};
				decltype (front->hashes_roots) hashes_roots{};
				requests_by_deadline.modify (front, [&channel, &hashes_roots](channel_pool & pool) {
					channel.swap (pool.channel);
					hashes_roots.swap (pool.hashes_roots);
				});
				requests_by_deadline.erase (front);
				lock.unlock ();
				erase_duplicates (hashes_roots);
				auto const remaining = aggregate (hashes_roots, channel);
				if (!remaining.empty ())
				{
					// Generate votes for the remaining hashes
					auto const generated = generator.generate (remaining, channel);
					stats.add (ysu::stat::type::requests, ysu::stat::detail::requests_cannot_vote, stat::dir::in, remaining.size () - generated);
				}
				lock.lock ();
			}
			else
			{
				auto deadline = front->deadline;
				condition.wait_until (lock, deadline, [this, &deadline]() { return this->stopped || deadline < std::chrono::steady_clock::now (); });
			}
		}
		else
		{
			condition.wait_for (lock, small_delay, [this]() { return this->stopped || !this->requests.empty (); });
		}
	}
}

void ysu::request_aggregator::stop ()
{
	{
		ysu::lock_guard<std::mutex> guard (mutex);
		stopped = true;
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

std::size_t ysu::request_aggregator::size ()
{
	ysu::unique_lock<std::mutex> lock (mutex);
	return requests.size ();
}

bool ysu::request_aggregator::empty ()
{
	return size () == 0;
}

void ysu::request_aggregator::reply_action (std::shared_ptr<ysu::vote> const & vote_a, std::shared_ptr<ysu::transport::channel> & channel_a) const
{
	ysu::confirm_ack confirm (vote_a);
	channel_a->send (confirm);
}

void ysu::request_aggregator::erase_duplicates (std::vector<std::pair<ysu::block_hash, ysu::root>> & requests_a) const
{
	std::sort (requests_a.begin (), requests_a.end (), [](auto const & pair1, auto const & pair2) {
		return pair1.first < pair2.first;
	});
	requests_a.erase (std::unique (requests_a.begin (), requests_a.end (), [](auto const & pair1, auto const & pair2) {
		return pair1.first == pair2.first;
	}),
	requests_a.end ());
}

std::vector<std::shared_ptr<ysu::block>> ysu::request_aggregator::aggregate (std::vector<std::pair<ysu::block_hash, ysu::root>> const & requests_a, std::shared_ptr<ysu::transport::channel> & channel_a) const
{
	auto transaction (ledger.store.tx_begin_read ());
	size_t cached_hashes = 0;
	std::vector<std::shared_ptr<ysu::block>> to_generate;
	std::vector<std::shared_ptr<ysu::vote>> cached_votes;
	for (auto const & hash_root : requests_a)
	{
		// 1. Votes in cache
		auto find_votes (local_votes.votes (hash_root.second, hash_root.first));
		if (!find_votes.empty ())
		{
			++cached_hashes;
			cached_votes.insert (cached_votes.end (), find_votes.begin (), find_votes.end ());
		}
		else
		{
			// 2. Election winner by hash
			auto block = active.winner (hash_root.first);

			// 3. Ledger by hash
			if (block == nullptr)
			{
				block = ledger.store.block_get (transaction, hash_root.first);
			}

			// 4. Ledger by root
			if (block == nullptr && !hash_root.second.is_zero ())
			{
				// Search for block root
				auto successor (ledger.store.block_successor (transaction, hash_root.second.as_block_hash ()));

				// Search for account root
				if (successor.is_zero ())
				{
					ysu::account_info info;
					auto error (ledger.store.account_get (transaction, hash_root.second.as_account (), info));
					if (!error)
					{
						successor = info.open_block;
					}
				}
				if (!successor.is_zero ())
				{
					auto successor_block = ledger.store.block_get (transaction, successor);
					debug_assert (successor_block != nullptr);
					// 5. Votes in cache for successor
					auto find_successor_votes (local_votes.votes (hash_root.second, successor));
					if (!find_successor_votes.empty ())
					{
						cached_votes.insert (cached_votes.end (), find_successor_votes.begin (), find_successor_votes.end ());
					}
					else
					{
						block = std::move (successor_block);
					}
				}
			}

			if (block)
			{
				to_generate.push_back (block);

				// Let the node know about the alternative block
				if (block->hash () != hash_root.first)
				{
					ysu::publish publish (block);
					channel_a->send (publish);
				}
			}
			else
			{
				stats.inc (ysu::stat::type::requests, ysu::stat::detail::requests_unknown, stat::dir::in);
			}
		}
	}
	// Unique votes
	std::sort (cached_votes.begin (), cached_votes.end ());
	cached_votes.erase (std::unique (cached_votes.begin (), cached_votes.end ()), cached_votes.end ());
	for (auto const & vote : cached_votes)
	{
		reply_action (vote, channel_a);
	}
	stats.add (ysu::stat::type::requests, ysu::stat::detail::requests_cached_hashes, stat::dir::in, cached_hashes);
	stats.add (ysu::stat::type::requests, ysu::stat::detail::requests_cached_votes, stat::dir::in, cached_votes.size ());
	return to_generate;
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (ysu::request_aggregator & aggregator, const std::string & name)
{
	auto pools_count = aggregator.size ();
	auto sizeof_element = sizeof (decltype (aggregator.requests)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "pools", pools_count, sizeof_element }));
	return composite;
}
