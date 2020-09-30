#include <ysu/node/gap_cache.hpp>
#include <ysu/node/node.hpp>
#include <ysu/secure/blockstore.hpp>

#include <boost/format.hpp>

ysu::gap_cache::gap_cache (ysu::node & node_a) :
node (node_a)
{
}

void ysu::gap_cache::add (ysu::block_hash const & hash_a, std::chrono::steady_clock::time_point time_point_a)
{
	ysu::lock_guard<std::mutex> lock (mutex);
	auto existing (blocks.get<tag_hash> ().find (hash_a));
	if (existing != blocks.get<tag_hash> ().end ())
	{
		blocks.get<tag_hash> ().modify (existing, [time_point_a](ysu::gap_information & info) {
			info.arrival = time_point_a;
		});
	}
	else
	{
		blocks.get<tag_arrival> ().emplace (ysu::gap_information{ time_point_a, hash_a, std::vector<ysu::account> () });
		if (blocks.get<tag_arrival> ().size () > max)
		{
			blocks.get<tag_arrival> ().erase (blocks.get<tag_arrival> ().begin ());
		}
	}
}

void ysu::gap_cache::erase (ysu::block_hash const & hash_a)
{
	ysu::lock_guard<std::mutex> lock (mutex);
	blocks.get<tag_hash> ().erase (hash_a);
}

void ysu::gap_cache::vote (std::shared_ptr<ysu::vote> vote_a)
{
	ysu::lock_guard<std::mutex> lock (mutex);
	for (auto hash : *vote_a)
	{
		auto & gap_blocks_by_hash (blocks.get<tag_hash> ());
		auto existing (gap_blocks_by_hash.find (hash));
		if (existing != gap_blocks_by_hash.end () && !existing->bootstrap_started)
		{
			auto is_new (false);
			gap_blocks_by_hash.modify (existing, [&is_new, &vote_a](ysu::gap_information & info) {
				auto it = std::find (info.voters.begin (), info.voters.end (), vote_a->account);
				is_new = (it == info.voters.end ());
				if (is_new)
				{
					info.voters.push_back (vote_a->account);
				}
			});

			if (is_new)
			{
				if (bootstrap_check (existing->voters, hash))
				{
					gap_blocks_by_hash.modify (existing, [](ysu::gap_information & info) {
						info.bootstrap_started = true;
					});
				}
			}
		}
	}
}

bool ysu::gap_cache::bootstrap_check (std::vector<ysu::account> const & voters_a, ysu::block_hash const & hash_a)
{
	ysu::uint128_t tally;
	for (auto const & voter : voters_a)
	{
		tally += node.ledger.weight (voter);
	}
	bool start_bootstrap (false);
	if (!node.flags.disable_lazy_bootstrap)
	{
		if (tally >= node.config.online_weight_minimum.number ())
		{
			start_bootstrap = true;
		}
	}
	else if (!node.flags.disable_legacy_bootstrap && tally > bootstrap_threshold ())
	{
		start_bootstrap = true;
	}
	if (start_bootstrap && !node.ledger.block_exists (hash_a))
	{
		bootstrap_start (hash_a);
	}
	return start_bootstrap;
}

void ysu::gap_cache::bootstrap_start (ysu::block_hash const & hash_a)
{
	auto node_l (node.shared ());
	node.alarm.add (std::chrono::steady_clock::now () + node.network_params.bootstrap.gap_cache_bootstrap_start_interval, [node_l, hash_a]() {
		auto transaction (node_l->store.tx_begin_read ());
		if (!node_l->store.block_exists (transaction, hash_a))
		{
			if (!node_l->bootstrap_initiator.in_progress ())
			{
				node_l->logger.try_log (boost::str (boost::format ("Missing block %1% which has enough votes to warrant lazy bootstrapping it") % hash_a.to_string ()));
			}
			if (!node_l->flags.disable_lazy_bootstrap)
			{
				node_l->bootstrap_initiator.bootstrap_lazy (hash_a);
			}
			else if (!node_l->flags.disable_legacy_bootstrap)
			{
				node_l->bootstrap_initiator.bootstrap ();
			}
		}
	});
}

ysu::uint128_t ysu::gap_cache::bootstrap_threshold ()
{
	auto result ((node.online_reps.online_stake () / 256) * node.config.bootstrap_fraction_numerator);
	return result;
}

size_t ysu::gap_cache::size ()
{
	ysu::lock_guard<std::mutex> lock (mutex);
	return blocks.size ();
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (gap_cache & gap_cache, const std::string & name)
{
	auto count = gap_cache.size ();
	auto sizeof_element = sizeof (decltype (gap_cache.blocks)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "blocks", count, sizeof_element }));
	return composite;
}
