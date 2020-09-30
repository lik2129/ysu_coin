#pragma once

#include <ysu/lib/numbers.hpp>
#include <ysu/lib/utility.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace ysu
{
class block_store;
class transaction;

class rep_weights
{
public:
	void representation_add (ysu::account const & source_rep_a, ysu::uint128_t const & amount_a);
	void representation_add_dual (ysu::account const & source_rep_1, ysu::uint128_t const & amount_1, ysu::account const & source_rep_2, ysu::uint128_t const & amount_2);
	ysu::uint128_t representation_get (ysu::account const & account_a) const;
	void representation_put (ysu::account const & account_a, ysu::uint128_union const & representation_a);
	std::unordered_map<ysu::account, ysu::uint128_t> get_rep_amounts () const;
	void copy_from (rep_weights & other_a);

private:
	mutable std::mutex mutex;
	std::unordered_map<ysu::account, ysu::uint128_t> rep_amounts;
	void put (ysu::account const & account_a, ysu::uint128_union const & representation_a);
	ysu::uint128_t get (ysu::account const & account_a) const;

	friend std::unique_ptr<container_info_component> collect_container_info (rep_weights const &, const std::string &);
};

std::unique_ptr<container_info_component> collect_container_info (rep_weights const &, const std::string &);
}
