#pragma once

#include <ysu/lib/numbers.hpp>
#include <ysu/lib/utility.hpp>
#include <ysu/secure/common.hpp>

#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace ysu
{
class signature_checker;
class active_transactions;
class block_store;
class node_observers;
class stats;
class node_config;
class logger_mt;
class online_reps;
class ledger;
class network_params;
class node_flags;

class transaction;
namespace transport
{
	class channel;
}

class vote_processor final
{
public:
	explicit vote_processor (ysu::signature_checker & checker_a, ysu::active_transactions & active_a, ysu::node_observers & observers_a, ysu::stat & stats_a, ysu::node_config & config_a, ysu::node_flags & flags_a, ysu::logger_mt & logger_a, ysu::online_reps & online_reps_a, ysu::ledger & ledger_a, ysu::network_params & network_params_a);
	/** Returns false if the vote was processed */
	bool vote (std::shared_ptr<ysu::vote>, std::shared_ptr<ysu::transport::channel>);
	/** Note: node.active.mutex lock is required */
	ysu::vote_code vote_blocking (std::shared_ptr<ysu::vote>, std::shared_ptr<ysu::transport::channel>, bool = false);
	void verify_votes (std::deque<std::pair<std::shared_ptr<ysu::vote>, std::shared_ptr<ysu::transport::channel>>> const &);
	void flush ();
	/** Block until the currently active processing cycle finishes */
	void flush_active ();
	size_t size ();
	bool empty ();
	bool half_full ();
	void calculate_weights ();
	void stop ();

private:
	void process_loop ();

	ysu::signature_checker & checker;
	ysu::active_transactions & active;
	ysu::node_observers & observers;
	ysu::stat & stats;
	ysu::node_config & config;
	ysu::logger_mt & logger;
	ysu::online_reps & online_reps;
	ysu::ledger & ledger;
	ysu::network_params & network_params;

	size_t max_votes;

	std::deque<std::pair<std::shared_ptr<ysu::vote>, std::shared_ptr<ysu::transport::channel>>> votes;
	/** Representatives levels for random early detection */
	std::unordered_set<ysu::account> representatives_1;
	std::unordered_set<ysu::account> representatives_2;
	std::unordered_set<ysu::account> representatives_3;
	ysu::condition_variable condition;
	std::mutex mutex;
	bool started;
	bool stopped;
	bool is_active;
	std::thread thread;

	friend std::unique_ptr<container_info_component> collect_container_info (vote_processor & vote_processor, const std::string & name);
	friend class vote_processor_weights_Test;
};

std::unique_ptr<container_info_component> collect_container_info (vote_processor & vote_processor, const std::string & name);
}
