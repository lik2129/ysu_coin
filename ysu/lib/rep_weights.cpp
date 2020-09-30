#include <ysu/lib/rep_weights.hpp>
#include <ysu/secure/blockstore.hpp>

void ysu::rep_weights::representation_add (ysu::account const & source_rep_a, ysu::uint128_t const & amount_a)
{
	ysu::lock_guard<std::mutex> guard (mutex);
	auto source_previous (get (source_rep_a));
	put (source_rep_a, source_previous + amount_a);
}

void ysu::rep_weights::representation_add_dual (ysu::account const & source_rep_1, ysu::uint128_t const & amount_1, ysu::account const & source_rep_2, ysu::uint128_t const & amount_2)
{
	if (source_rep_1 != source_rep_2)
	{
		ysu::lock_guard<std::mutex> guard (mutex);
		auto source_previous_1 (get (source_rep_1));
		put (source_rep_1, source_previous_1 + amount_1);
		auto source_previous_2 (get (source_rep_2));
		put (source_rep_2, source_previous_2 + amount_2);
	}
	else
	{
		representation_add (source_rep_1, amount_1 + amount_2);
	}
}

void ysu::rep_weights::representation_put (ysu::account const & account_a, ysu::uint128_union const & representation_a)
{
	ysu::lock_guard<std::mutex> guard (mutex);
	put (account_a, representation_a);
}

ysu::uint128_t ysu::rep_weights::representation_get (ysu::account const & account_a) const
{
	ysu::lock_guard<std::mutex> lk (mutex);
	return get (account_a);
}

/** Makes a copy */
std::unordered_map<ysu::account, ysu::uint128_t> ysu::rep_weights::get_rep_amounts () const
{
	ysu::lock_guard<std::mutex> guard (mutex);
	return rep_amounts;
}

void ysu::rep_weights::copy_from (ysu::rep_weights & other_a)
{
	ysu::lock_guard<std::mutex> guard_this (mutex);
	ysu::lock_guard<std::mutex> guard_other (other_a.mutex);
	for (auto const & entry : other_a.rep_amounts)
	{
		auto prev_amount (get (entry.first));
		put (entry.first, prev_amount + entry.second);
	}
}

void ysu::rep_weights::put (ysu::account const & account_a, ysu::uint128_union const & representation_a)
{
	auto it = rep_amounts.find (account_a);
	auto amount = representation_a.number ();
	if (it != rep_amounts.end ())
	{
		it->second = amount;
	}
	else
	{
		rep_amounts.emplace (account_a, amount);
	}
}

ysu::uint128_t ysu::rep_weights::get (ysu::account const & account_a) const
{
	auto it = rep_amounts.find (account_a);
	if (it != rep_amounts.end ())
	{
		return it->second;
	}
	else
	{
		return ysu::uint128_t{ 0 };
	}
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (ysu::rep_weights const & rep_weights, const std::string & name)
{
	size_t rep_amounts_count;

	{
		ysu::lock_guard<std::mutex> guard (rep_weights.mutex);
		rep_amounts_count = rep_weights.rep_amounts.size ();
	}
	auto sizeof_element = sizeof (decltype (rep_weights.rep_amounts)::value_type);
	auto composite = std::make_unique<ysu::container_info_composite> (name);
	composite->add_component (std::make_unique<ysu::container_info_leaf> (container_info{ "rep_amounts", rep_amounts_count, sizeof_element }));
	return composite;
}
