#pragma once

#include <ysu/lib/config.hpp>
#include <ysu/lib/locks.hpp>
#include <ysu/lib/numbers.hpp>
#include <ysu/lib/utility.hpp>

#include <boost/optional.hpp>
#include <boost/thread/thread.hpp>

#include <atomic>
#include <memory>

namespace ysu
{
enum class work_version
{
	unspecified,
	work_1
};
std::string to_string (ysu::work_version const version_a);

class block;
class block_details;
enum class block_type : uint8_t;
bool work_validate_entry (ysu::block const &);
bool work_validate_entry (ysu::work_version const, ysu::root const &, uint64_t const);

uint64_t work_difficulty (ysu::work_version const, ysu::root const &, uint64_t const);

uint64_t work_threshold_base (ysu::work_version const);
uint64_t work_threshold_entry (ysu::work_version const, ysu::block_type const);
// Ledger threshold
uint64_t work_threshold (ysu::work_version const, ysu::block_details const);

namespace work_v1
{
	uint64_t value (ysu::root const & root_a, uint64_t work_a);
	uint64_t threshold_base ();
	uint64_t threshold_entry ();
	uint64_t threshold (ysu::block_details const);
}

double normalized_multiplier (double const, uint64_t const);
double denormalized_multiplier (double const, uint64_t const);
class opencl_work;
class work_item final
{
public:
	work_item (ysu::work_version const version_a, ysu::root const & item_a, uint64_t difficulty_a, std::function<void(boost::optional<uint64_t> const &)> const & callback_a) :
	version (version_a), item (item_a), difficulty (difficulty_a), callback (callback_a)
	{
	}
	ysu::work_version const version;
	ysu::root const item;
	uint64_t const difficulty;
	std::function<void(boost::optional<uint64_t> const &)> const callback;
};
class work_pool final
{
public:
	work_pool (unsigned, std::chrono::nanoseconds = std::chrono::nanoseconds (0), std::function<boost::optional<uint64_t> (ysu::work_version const, ysu::root const &, uint64_t, std::atomic<int> &)> = nullptr);
	~work_pool ();
	void loop (uint64_t);
	void stop ();
	void cancel (ysu::root const &);
	void generate (ysu::work_version const, ysu::root const &, uint64_t, std::function<void(boost::optional<uint64_t> const &)>);
	boost::optional<uint64_t> generate (ysu::work_version const, ysu::root const &, uint64_t);
	// For tests only
	boost::optional<uint64_t> generate (ysu::root const &);
	boost::optional<uint64_t> generate (ysu::root const &, uint64_t);
	size_t size ();
	ysu::network_constants network_constants;
	std::atomic<int> ticket;
	bool done;
	std::vector<boost::thread> threads;
	std::list<ysu::work_item> pending;
	std::mutex mutex;
	ysu::condition_variable producer_condition;
	std::chrono::nanoseconds pow_rate_limiter;
	std::function<boost::optional<uint64_t> (ysu::work_version const, ysu::root const &, uint64_t, std::atomic<int> &)> opencl;
	ysu::observer_set<bool> work_observers;
};

std::unique_ptr<container_info_component> collect_container_info (work_pool & work_pool, const std::string & name);
}
