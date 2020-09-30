#pragma once

#include <ysu/lib/blocks.hpp>
#include <ysu/node/state_block_signature_verification.hpp>
#include <ysu/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <chrono>
#include <memory>
#include <unordered_set>

namespace ysu
{
class node;
class transaction;
class write_transaction;
class write_database_queue;

enum class block_origin
{
	local,
	remote
};

class block_post_events final
{
public:
	~block_post_events ();
	std::deque<std::function<void()>> events;
};

/**
 * Processing blocks is a potentially long IO operation.
 * This class isolates block insertion from other operations like servicing network operations
 */
class block_processor final
{
public:
	explicit block_processor (ysu::node &, ysu::write_database_queue &);
	~block_processor ();
	void stop ();
	void flush ();
	size_t size ();
	bool full ();
	bool half_full ();
	void add (ysu::unchecked_info const &, const bool = false);
	void add (std::shared_ptr<ysu::block>, uint64_t = 0);
	void force (std::shared_ptr<ysu::block>);
	void wait_write ();
	bool should_log ();
	bool have_blocks ();
	void process_blocks ();
	ysu::process_return process_one (ysu::write_transaction const &, block_post_events &, ysu::unchecked_info, const bool = false, ysu::block_origin const = ysu::block_origin::remote);
	ysu::process_return process_one (ysu::write_transaction const &, block_post_events &, std::shared_ptr<ysu::block>, const bool = false);
	std::atomic<bool> flushing{ false };
	// Delay required for average network propagartion before requesting confirmation
	static std::chrono::milliseconds constexpr confirmation_request_delay{ 1500 };

private:
	void queue_unchecked (ysu::write_transaction const &, ysu::block_hash const &);
	void process_batch (ysu::unique_lock<std::mutex> &);
	void process_live (ysu::block_hash const &, std::shared_ptr<ysu::block>, ysu::process_return const &, const bool = false, ysu::block_origin const = ysu::block_origin::remote);
	void process_old (ysu::write_transaction const &, std::shared_ptr<ysu::block> const &, ysu::block_origin const);
	void requeue_invalid (ysu::block_hash const &, ysu::unchecked_info const &);
	void process_verified_state_blocks (std::deque<ysu::unchecked_info> &, std::vector<int> const &, std::vector<ysu::block_hash> const &, std::vector<ysu::signature> const &);
	bool stopped{ false };
	bool active{ false };
	bool awaiting_write{ false };
	std::chrono::steady_clock::time_point next_log;
	std::deque<ysu::unchecked_info> blocks;
	std::deque<std::shared_ptr<ysu::block>> forced;
	ysu::condition_variable condition;
	ysu::node & node;
	ysu::write_database_queue & write_database_queue;
	std::mutex mutex;
	ysu::state_block_signature_verification state_block_signature_verification;

	friend std::unique_ptr<container_info_component> collect_container_info (block_processor & block_processor, const std::string & name);
};
std::unique_ptr<ysu::container_info_component> collect_container_info (block_processor & block_processor, const std::string & name);
}
