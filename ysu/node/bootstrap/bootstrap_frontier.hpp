#pragma once

#include <ysu/node/common.hpp>

#include <deque>
#include <future>

namespace ysu
{
class bootstrap_attempt;
class bootstrap_client;
class frontier_req_client final : public std::enable_shared_from_this<ysu::frontier_req_client>
{
public:
	explicit frontier_req_client (std::shared_ptr<ysu::bootstrap_client>, std::shared_ptr<ysu::bootstrap_attempt>);
	~frontier_req_client ();
	void run ();
	void receive_frontier ();
	void received_frontier (boost::system::error_code const &, size_t);
	void unsynced (ysu::block_hash const &, ysu::block_hash const &);
	void next ();
	std::shared_ptr<ysu::bootstrap_client> connection;
	std::shared_ptr<ysu::bootstrap_attempt> attempt;
	ysu::account current;
	ysu::block_hash frontier;
	unsigned count;
	ysu::account landing;
	ysu::account faucet;
	std::chrono::steady_clock::time_point start_time;
	std::promise<bool> promise;
	/** A very rough estimate of the cost of `bulk_push`ing missing blocks */
	uint64_t bulk_push_cost;
	std::deque<std::pair<ysu::account, ysu::block_hash>> accounts;
	static size_t constexpr size_frontier = sizeof (ysu::account) + sizeof (ysu::block_hash);
};
class bootstrap_server;
class frontier_req;
class frontier_req_server final : public std::enable_shared_from_this<ysu::frontier_req_server>
{
public:
	frontier_req_server (std::shared_ptr<ysu::bootstrap_server> const &, std::unique_ptr<ysu::frontier_req>);
	void send_next ();
	void sent_action (boost::system::error_code const &, size_t);
	void send_finished ();
	void no_block_sent (boost::system::error_code const &, size_t);
	void next ();
	std::shared_ptr<ysu::bootstrap_server> connection;
	ysu::account current;
	ysu::block_hash frontier;
	std::unique_ptr<ysu::frontier_req> request;
	size_t count;
	std::deque<std::pair<ysu::account, ysu::block_hash>> accounts;
};
}
