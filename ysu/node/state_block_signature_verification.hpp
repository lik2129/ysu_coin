#pragma once

#include <ysu/lib/locks.hpp>
#include <ysu/secure/common.hpp>

#include <deque>
#include <functional>
#include <thread>

namespace ysu
{
class epochs;
class logger_mt;
class node_config;
class signature_checker;

class state_block_signature_verification
{
public:
	state_block_signature_verification (ysu::signature_checker &, ysu::epochs &, ysu::node_config &, ysu::logger_mt &, uint64_t);
	~state_block_signature_verification ();
	void add (ysu::unchecked_info const & info_a);
	size_t size ();
	void stop ();
	bool is_active ();

	std::function<void(std::deque<ysu::unchecked_info> &, std::vector<int> const &, std::vector<ysu::block_hash> const &, std::vector<ysu::signature> const &)> blocks_verified_callback;
	std::function<void()> transition_inactive_callback;

private:
	ysu::signature_checker & signature_checker;
	ysu::epochs & epochs;
	ysu::node_config & node_config;
	ysu::logger_mt & logger;

	std::mutex mutex;
	bool stopped{ false };
	bool active{ false };
	std::deque<ysu::unchecked_info> state_blocks;
	ysu::condition_variable condition;
	std::thread thread;

	void run (uint64_t block_processor_verification_size);
	std::deque<ysu::unchecked_info> setup_items (size_t);
	void verify_state_blocks (std::deque<ysu::unchecked_info> &);
};

std::unique_ptr<ysu::container_info_component> collect_container_info (state_block_signature_verification & state_block_signature_verification, const std::string & name);
}
