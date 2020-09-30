#pragma once

#include <ysu/lib/numbers.hpp>
#include <ysu/lib/utility.hpp>

#include <memory>
#include <unordered_set>
#include <vector>

namespace ysu
{
class ledger;
class network_params;
class transaction;

/** Track online representatives and trend online weight */
class online_reps final
{
public:
	online_reps (ysu::ledger & ledger_a, ysu::network_params & network_params_a, ysu::uint128_t minimum_a);
	/** Add voting account \p rep_account to the set of online representatives */
	void observe (ysu::account const & rep_account);
	/** Called periodically to sample online weight */
	void sample ();
	/** Returns the trended online stake, but never less than configured minimum */
	ysu::uint128_t online_stake () const;
	/** List of online representatives, both the currently sampling ones and the ones observed in the previous sampling period */
	std::vector<ysu::account> list ();

private:
	ysu::uint128_t trend (ysu::transaction &);
	mutable std::mutex mutex;
	ysu::ledger & ledger;
	ysu::network_params & network_params;
	std::unordered_set<ysu::account> reps;
	std::unordered_set<ysu::account> last_reps;
	ysu::uint128_t online;
	ysu::uint128_t minimum;

	friend std::unique_ptr<container_info_component> collect_container_info (online_reps & online_reps, const std::string & name);
};

std::unique_ptr<container_info_component> collect_container_info (online_reps & online_reps, const std::string & name);
}
