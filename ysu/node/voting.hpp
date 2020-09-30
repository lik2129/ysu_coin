#pragma once

#include <ysu/lib/locks.hpp>
#include <ysu/lib/numbers.hpp>
#include <ysu/lib/utility.hpp>
#include <ysu/node/wallet.hpp>
#include <ysu/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace mi = boost::multi_index;

namespace ysu
{
class ledger;
class network;
class node_config;
class stat;
class vote_processor;
class wallets;
namespace transport
{
	class channel;
}

class local_vote_history final
{
	class local_vote final
	{
	public:
		local_vote (ysu::root const & root_a, ysu::block_hash const & hash_a, std::shared_ptr<ysu::vote> const & vote_a) :
		root (root_a),
		hash (hash_a),
		vote (vote_a)
		{
		}
		ysu::root root;
		ysu::block_hash hash;
		std::shared_ptr<ysu::vote> vote;
	};

public:
	void add (ysu::root const & root_a, ysu::block_hash const & hash_a, std::shared_ptr<ysu::vote> const & vote_a);
	void erase (ysu::root const & root_a);

	std::vector<std::shared_ptr<ysu::vote>> votes (ysu::root const & root_a, ysu::block_hash const & hash_a) const;
	bool exists (ysu::root const &) const;
	size_t size () const;

private:
	// clang-format off
	boost::multi_index_container<local_vote,
	mi::indexed_by<
		mi::hashed_non_unique<mi::tag<class tag_root>,
			mi::member<local_vote, ysu::root, &local_vote::root>>,
		mi::sequenced<mi::tag<class tag_sequence>>>>
	history;
	// clang-format on

	size_t const max_size{ ysu::network_params{}.voting.max_cache };
	void clean ();
	std::vector<std::shared_ptr<ysu::vote>> votes (ysu::root const & root_a) const;
	// Only used in Debug
	bool consistency_check (ysu::root const &) const;
	mutable std::mutex mutex;

	friend std::unique_ptr<container_info_component> collect_container_info (local_vote_history & history, const std::string & name);

	friend class local_vote_history_basic_Test;
};

std::unique_ptr<container_info_component> collect_container_info (local_vote_history & history, const std::string & name);

class vote_generator final
{
private:
	using candidate_t = std::pair<ysu::root, ysu::block_hash>;
	using request_t = std::pair<std::vector<candidate_t>, std::shared_ptr<ysu::transport::channel>>;

public:
	vote_generator (ysu::node_config const & config_a, ysu::ledger & ledger_a, ysu::wallets & wallets_a, ysu::vote_processor & vote_processor_a, ysu::local_vote_history & history_a, ysu::network & network_a, ysu::stat & stats_a);
	/** Queue items for vote generation, or broadcast votes already in cache */
	void add (ysu::root const &, ysu::block_hash const &);
	/** Queue blocks for vote generation, returning the number of successful candidates.*/
	size_t generate (std::vector<std::shared_ptr<ysu::block>> const & blocks_a, std::shared_ptr<ysu::transport::channel> const & channel_a);
	void set_reply_action (std::function<void(std::shared_ptr<ysu::vote> const &, std::shared_ptr<ysu::transport::channel> &)>);
	void stop ();

private:
	void run ();
	void broadcast (ysu::unique_lock<std::mutex> &);
	void reply (ysu::unique_lock<std::mutex> &, request_t &&);
	void vote (std::vector<ysu::block_hash> const &, std::vector<ysu::root> const &, std::function<void(std::shared_ptr<ysu::vote> const &)> const &);
	void broadcast_action (std::shared_ptr<ysu::vote> const &) const;
	std::function<void(std::shared_ptr<ysu::vote> const &, std::shared_ptr<ysu::transport::channel> &)> reply_action; // must be set only during initialization by using set_reply_action
	ysu::node_config const & config;
	ysu::ledger & ledger;
	ysu::wallets & wallets;
	ysu::vote_processor & vote_processor;
	ysu::local_vote_history & history;
	ysu::network & network;
	ysu::stat & stats;
	mutable std::mutex mutex;
	ysu::condition_variable condition;
	static size_t constexpr max_requests{ 2048 };
	std::deque<request_t> requests;
	std::deque<candidate_t> candidates;
	ysu::network_params network_params;
	std::atomic<bool> stopped{ false };
	bool started{ false };
	std::thread thread;

	friend std::unique_ptr<container_info_component> collect_container_info (vote_generator & vote_generator, const std::string & name);
};

std::unique_ptr<container_info_component> collect_container_info (vote_generator & generator, const std::string & name);

class vote_generator_session final
{
public:
	vote_generator_session (vote_generator & vote_generator_a);
	void add (ysu::root const &, ysu::block_hash const &);
	void flush ();

private:
	ysu::vote_generator & generator;
	std::vector<std::pair<ysu::root, ysu::block_hash>> items;
};
}
