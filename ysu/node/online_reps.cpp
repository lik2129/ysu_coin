#include <ysu/node/online_reps.hpp>
#include <ysu/secure/blockstore.hpp>
#include <ysu/secure/common.hpp>
#include <ysu/secure/ledger.hpp>

ysu::online_reps::online_reps (ysu::ledger & ledger_a, ysu::network_params & network_params_a, ysu::uint128_t minimum_a) :
ledger (ledger_a),
network_params (network_params_a),
minimum (minimum_a)
{
	if (!ledger.store.init_error ())
	{
		auto transaction (ledger.store.tx_begin_read ());
		online = trend (transaction);
	}
}

void ysu::online_reps::observe (ysu::account const & rep_a)
{
	if (ledger.weight (rep_a) > 0)
	{
		ysu::lock_guard<std::mutex> lock (mutex);
		reps.insert (rep_a);
	}
}

void ysu::online_reps::sample ()
{
	auto transaction (ledger.store.tx_begin_write ({ tables::online_weight }));
	// Discard oldest entries
	while (ledger.store.online_weight_count (transaction) >= network_params.node.max_weight_samples)
	{
		auto oldest (ledger.store.online_weight_begin (transaction));
		debug_assert (oldest != ledger.store.online_weight_end ());
		ledger.store.online_weight_del (transaction, oldest->first);
	}
	// Calculate current active rep weight
	ysu::uint128_t current;
	std::unordered_set<ysu::account> reps_copy;
	{
		ysu::lock_guard<std::mutex> lock (mutex);
		last_reps = reps;
		reps_copy.swap (reps);
	}
	for (auto & i : reps_copy)
	{
		current += ledger.weight (i);
	}
	ledger.store.online_weight_put (transaction, std::chrono::system_clock::now ().time_since_epoch ().count (), current);
	auto trend_l (trend (transaction));
	ysu::lock_guard<std::mutex> lock (mutex);
	online = trend_l;
}

ysu::uint128_t ysu::online_reps::trend (ysu::transaction & transaction_a)
{
	std::vector<ysu::uint128_t> items;
	items.reserve (network_params.node.max_weight_samples + 1);
	items.push_back (minimum);
	for (auto i (ledger.store.online_weight_begin (transaction_a)), n (ledger.store.online_weight_end ()); i != n; ++i)
	{
		items.push_back (i->second.number ());
	}

	// Pick median value for our target vote weight
	auto median_idx = items.size () / 2;
	nth_element (items.begin (), items.begin () + median_idx, items.end ());
	return ysu::uint128_t{ items[median_idx] };
}

ysu::uint128_t ysu::online_reps::online_stake () const
{
	ysu::lock_guard<std::mutex> lock (mutex);
	return std::max (online, minimum);
}

std::vector<ysu::account> ysu::online_reps::list ()
{
	std::vector<ysu::account> result;
	decltype (reps) all_reps;
	{
		ysu::lock_guard<std::mutex> lock (mutex);
		all_reps.insert (last_reps.begin (), last_reps.end ());
		all_reps.insert (reps.begin (), reps.end ());
	}
	result.insert (result.end (), all_reps.begin (), all_reps.end ());
	return result;
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (online_reps & online_reps, const std::string & name)
{
	size_t count;
	{
		ysu::lock_guard<std::mutex> guard (online_reps.mutex);
		count = online_reps.last_reps.size ();
	}

	auto sizeof_element = sizeof (decltype (online_reps.last_reps)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "reps", count, sizeof_element }));
	return composite;
}
