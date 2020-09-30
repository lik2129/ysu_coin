#pragma once

#include <ysu/node/common.hpp>
#include <ysu/node/socket.hpp>

#include <unordered_set>

namespace ysu
{
class bootstrap_attempt;
class pull_info
{
public:
	using count_t = ysu::bulk_pull::count_t;
	pull_info () = default;
	pull_info (ysu::hash_or_account const &, ysu::block_hash const &, ysu::block_hash const &, uint64_t, count_t = 0, unsigned = 16);
	ysu::hash_or_account account_or_head{ 0 };
	ysu::block_hash head{ 0 };
	ysu::block_hash head_original{ 0 };
	ysu::block_hash end{ 0 };
	count_t count{ 0 };
	unsigned attempts{ 0 };
	uint64_t processed{ 0 };
	unsigned retry_limit{ 0 };
	uint64_t bootstrap_id{ 0 };
};
class bootstrap_client;
class bulk_pull_client final : public std::enable_shared_from_this<ysu::bulk_pull_client>
{
public:
	bulk_pull_client (std::shared_ptr<ysu::bootstrap_client>, std::shared_ptr<ysu::bootstrap_attempt>, ysu::pull_info const &);
	~bulk_pull_client ();
	void request ();
	void receive_block ();
	void throttled_receive_block ();
	void received_type ();
	void received_block (boost::system::error_code const &, size_t, ysu::block_type);
	ysu::block_hash first ();
	std::shared_ptr<ysu::bootstrap_client> connection;
	std::shared_ptr<ysu::bootstrap_attempt> attempt;
	ysu::block_hash expected;
	ysu::account known_account;
	ysu::pull_info pull;
	uint64_t pull_blocks;
	uint64_t unexpected_count;
	bool network_error{ false };
};
class bulk_pull_account_client final : public std::enable_shared_from_this<ysu::bulk_pull_account_client>
{
public:
	bulk_pull_account_client (std::shared_ptr<ysu::bootstrap_client>, std::shared_ptr<ysu::bootstrap_attempt>, ysu::account const &);
	~bulk_pull_account_client ();
	void request ();
	void receive_pending ();
	std::shared_ptr<ysu::bootstrap_client> connection;
	std::shared_ptr<ysu::bootstrap_attempt> attempt;
	ysu::account account;
	uint64_t pull_blocks;
};
class bootstrap_server;
class bulk_pull;
class bulk_pull_server final : public std::enable_shared_from_this<ysu::bulk_pull_server>
{
public:
	bulk_pull_server (std::shared_ptr<ysu::bootstrap_server> const &, std::unique_ptr<ysu::bulk_pull>);
	void set_current_end ();
	std::shared_ptr<ysu::block> get_next ();
	void send_next ();
	void sent_action (boost::system::error_code const &, size_t);
	void send_finished ();
	void no_block_sent (boost::system::error_code const &, size_t);
	std::shared_ptr<ysu::bootstrap_server> connection;
	std::unique_ptr<ysu::bulk_pull> request;
	ysu::block_hash current;
	bool include_start;
	ysu::bulk_pull::count_t max_count;
	ysu::bulk_pull::count_t sent_count;
};
class bulk_pull_account;
class bulk_pull_account_server final : public std::enable_shared_from_this<ysu::bulk_pull_account_server>
{
public:
	bulk_pull_account_server (std::shared_ptr<ysu::bootstrap_server> const &, std::unique_ptr<ysu::bulk_pull_account>);
	void set_params ();
	std::pair<std::unique_ptr<ysu::pending_key>, std::unique_ptr<ysu::pending_info>> get_next ();
	void send_frontier ();
	void send_next_block ();
	void sent_action (boost::system::error_code const &, size_t);
	void send_finished ();
	void complete (boost::system::error_code const &, size_t);
	std::shared_ptr<ysu::bootstrap_server> connection;
	std::unique_ptr<ysu::bulk_pull_account> request;
	std::unordered_set<ysu::uint256_union> deduplication;
	ysu::pending_key current_key;
	bool pending_address_only;
	bool pending_include_address;
	bool invalid_request;
};
}
