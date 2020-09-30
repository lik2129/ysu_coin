#pragma once

#include <ysu/lib/numbers.hpp>
#include <ysu/node/election.hpp>
#include <ysu/node/voting.hpp>
#include <ysu/secure/common.hpp>

#include <boost/circular_buffer.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/optional.hpp>
#include <boost/thread/thread.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace mi = boost::multi_index;

namespace ysu
{
class node;
class block;
class block_sideband;
class election;
class vote;
class transaction;
class confirmation_height_processor;
class stat;

class cementable_account final
{
public:
	cementable_account (ysu::account const & account_a, size_t blocks_uncemented_a);
	ysu::account account;
	uint64_t blocks_uncemented{ 0 };
};

class election_timepoint final
{
public:
	std::chrono::steady_clock::time_point time;
	ysu::qualified_root root;
};

class inactive_cache_status final
{
public:
	bool bootstrap_started{ false };
	bool election_started{ false }; // Did item reach config threshold to start an impromptu election?
	bool confirmed{ false }; // Did item reach votes quorum? (minimum config value)

	bool operator!= (inactive_cache_status const other) const
	{
		return bootstrap_started != other.bootstrap_started || election_started != other.election_started || confirmed != other.confirmed;
	}
};

class inactive_cache_information final
{
public:
	std::chrono::steady_clock::time_point arrival;
	ysu::block_hash hash;
	std::vector<ysu::account> voters;
	ysu::inactive_cache_status status;
	bool needs_eval () const
	{
		return !status.bootstrap_started || !status.election_started || !status.confirmed;
	}
};

class expired_optimistic_election_info final
{
public:
	expired_optimistic_election_info (std::chrono::steady_clock::time_point, ysu::account);

	std::chrono::steady_clock::time_point expired_time;
	ysu::account account;
	bool election_started{ false };
};

class frontiers_confirmation_info
{
public:
	bool can_start_elections () const;

	size_t max_elections{ 0 };
	bool aggressive_mode{ false };
};

class dropped_elections final
{
public:
	dropped_elections (ysu::stat &);
	void add (ysu::qualified_root const &);
	void erase (ysu::qualified_root const &);
	std::chrono::steady_clock::time_point find (ysu::qualified_root const &) const;
	size_t size () const;

	static size_t constexpr capacity{ 16 * 1024 };

	// clang-format off
	class tag_sequence {};
	class tag_root {};
	using ordered_dropped = boost::multi_index_container<ysu::election_timepoint,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequence>>,
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<ysu::election_timepoint, decltype(ysu::election_timepoint::root), &ysu::election_timepoint::root>>>>;
	// clang-format on

private:
	ordered_dropped items;
	mutable std::mutex mutex;
	ysu::stat & stats;
};

class election_insertion_result final
{
public:
	std::shared_ptr<ysu::election> election;
	bool inserted{ false };
};

// Core class for determining consensus
// Holds all active blocks i.e. recently added blocks that need confirmation
class active_transactions final
{
	class conflict_info final
	{
	public:
		ysu::qualified_root root;
		double multiplier;
		std::shared_ptr<ysu::election> election;
		ysu::epoch epoch;
		ysu::uint128_t previous_balance;
	};

	friend class ysu::election;

	// clang-format off
	class tag_account {};
	class tag_difficulty {};
	class tag_root {};
	class tag_sequence {};
	class tag_uncemented {};
	class tag_arrival {};
	class tag_hash {};
	class tag_expired_time {};
	class tag_election_started {};
	// clang-format on

public:
	// clang-format off
	using ordered_roots = boost::multi_index_container<conflict_info,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<conflict_info, ysu::qualified_root, &conflict_info::root>>,
		mi::ordered_non_unique<mi::tag<tag_difficulty>,
			mi::member<conflict_info, double, &conflict_info::multiplier>,
			std::greater<double>>>>;
	// clang-format on
	ordered_roots roots;
	using roots_iterator = active_transactions::ordered_roots::index_iterator<tag_root>::type;

	explicit active_transactions (ysu::node &, ysu::confirmation_height_processor &);
	~active_transactions ();
	// Start an election for a block
	// Call action with confirmed block, may be different than what we started with
	// clang-format off
	ysu::election_insertion_result insert (std::shared_ptr<ysu::block> const &, boost::optional<ysu::uint128_t> const & = boost::none, ysu::election_behavior = ysu::election_behavior::normal, std::function<void(std::shared_ptr<ysu::block>)> const & = nullptr);
	// clang-format on
	// Distinguishes replay votes, cannot be determined if the block is not in any election
	ysu::vote_code vote (std::shared_ptr<ysu::vote>);
	// Is the root of this block in the roots container
	bool active (ysu::block const &);
	bool active (ysu::qualified_root const &);
	std::shared_ptr<ysu::election> election (ysu::qualified_root const &) const;
	std::shared_ptr<ysu::block> winner (ysu::block_hash const &) const;
	// Activates the first unconfirmed block of \p account_a
	ysu::election_insertion_result activate (ysu::account const &);
	// Returns false if the election difficulty was updated
	bool update_difficulty (ysu::block const &);
	// Returns false if the election was restarted
	bool restart (std::shared_ptr<ysu::block> const &, ysu::write_transaction const &);
	double normalized_multiplier (ysu::block const &, boost::optional<roots_iterator> const & = boost::none) const;
	void update_active_multiplier (ysu::unique_lock<std::mutex> &);
	uint64_t active_difficulty ();
	uint64_t limited_active_difficulty (ysu::block const &);
	uint64_t limited_active_difficulty (ysu::work_version const, uint64_t const);
	double active_multiplier ();
	void erase (ysu::block const &);
	bool empty ();
	size_t size ();
	void stop ();
	bool publish (std::shared_ptr<ysu::block> block_a);
	boost::optional<ysu::election_status_type> confirm_block (ysu::transaction const &, std::shared_ptr<ysu::block>);
	void block_cemented_callback (std::shared_ptr<ysu::block> const & block_a);
	void block_already_cemented_callback (ysu::block_hash const &);
	boost::optional<double> last_prioritized_multiplier{ boost::none };
	std::unordered_map<ysu::block_hash, std::shared_ptr<ysu::election>> blocks;
	std::deque<ysu::election_status> list_recently_cemented ();
	std::deque<ysu::election_status> recently_cemented;
	dropped_elections recently_dropped;

	void add_recently_cemented (ysu::election_status const &);
	void add_recently_confirmed (ysu::qualified_root const &, ysu::block_hash const &);
	void erase_recently_confirmed (ysu::block_hash const &);
	void add_inactive_votes_cache (ysu::block_hash const &, ysu::account const &);
	// Inserts an election if conditions are met
	void trigger_inactive_votes_cache_election (std::shared_ptr<ysu::block> const &);
	ysu::inactive_cache_information find_inactive_votes_cache (ysu::block_hash const &);
	void erase_inactive_votes_cache (ysu::block_hash const &);
	ysu::confirmation_height_processor & confirmation_height_processor;
	ysu::node & node;
	mutable std::mutex mutex;
	boost::circular_buffer<double> multipliers_cb;
	std::atomic<double> trended_active_multiplier;
	size_t priority_cementable_frontiers_size ();
	size_t priority_wallet_cementable_frontiers_size ();
	boost::circular_buffer<double> difficulty_trend ();
	size_t inactive_votes_cache_size ();
	size_t election_winner_details_size ();
	void add_election_winner_details (ysu::block_hash const &, std::shared_ptr<ysu::election> const &);
	void remove_election_winner_details (ysu::block_hash const &);

	ysu::vote_generator generator;

private:
	std::mutex election_winner_details_mutex;
	std::unordered_map<ysu::block_hash, std::shared_ptr<ysu::election>> election_winner_details;

	// Call action with confirmed block, may be different than what we started with
	// clang-format off
	ysu::election_insertion_result insert_impl (std::shared_ptr<ysu::block> const &, boost::optional<ysu::uint128_t> const & = boost::none, ysu::election_behavior = ysu::election_behavior::normal, std::function<void(std::shared_ptr<ysu::block>)> const & = nullptr);
	// clang-format on
	// Returns false if the election difficulty was updated
	bool update_difficulty_impl (roots_iterator const &, ysu::block const &);
	void request_loop ();
	void request_confirm (ysu::unique_lock<std::mutex> &);
	// Erase all blocks from active and, if not confirmed, clear digests from network filters
	void cleanup_election (ysu::election_cleanup_info const &);
	ysu::condition_variable condition;
	bool started{ false };
	std::atomic<bool> stopped{ false };

	// Periodically check all elections
	std::chrono::milliseconds const check_all_elections_period;
	std::chrono::steady_clock::time_point last_check_all_elections{};

	// Maximum time an election can be kept active if it is extending the container
	std::chrono::seconds const election_time_to_live;

	// Elections above this position in the queue are prioritized
	size_t const prioritized_cutoff;

	static size_t constexpr recently_confirmed_size{ 65536 };
	using recent_confirmation = std::pair<ysu::qualified_root, ysu::block_hash>;
	// clang-format off
	boost::multi_index_container<recent_confirmation,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequence>>,
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<recent_confirmation, ysu::qualified_root, &recent_confirmation::first>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<recent_confirmation, ysu::block_hash, &recent_confirmation::second>>>>
	recently_confirmed;
	using prioritize_num_uncemented = boost::multi_index_container<ysu::cementable_account,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_account>,
			mi::member<ysu::cementable_account, ysu::account, &ysu::cementable_account::account>>,
		mi::ordered_non_unique<mi::tag<tag_uncemented>,
			mi::member<ysu::cementable_account, uint64_t, &ysu::cementable_account::blocks_uncemented>,
			std::greater<uint64_t>>>>;

	boost::multi_index_container<ysu::expired_optimistic_election_info,
	mi::indexed_by<
		mi::ordered_non_unique<mi::tag<tag_expired_time>,
			mi::member<expired_optimistic_election_info, std::chrono::steady_clock::time_point, &expired_optimistic_election_info::expired_time>>,
		mi::hashed_unique<mi::tag<tag_account>,
			mi::member<expired_optimistic_election_info, ysu::account, &expired_optimistic_election_info::account>>,
		mi::ordered_non_unique<mi::tag<tag_election_started>,
			mi::member<expired_optimistic_election_info, bool, &expired_optimistic_election_info::election_started>, std::greater<bool>>>>
	expired_optimistic_election_infos;
	// clang-format on
	std::atomic<uint64_t> expired_optimistic_election_infos_size{ 0 };

	// Frontiers confirmation
	ysu::frontiers_confirmation_info get_frontiers_confirmation_info ();
	void confirm_prioritized_frontiers (ysu::transaction const &, uint64_t, uint64_t &);
	void confirm_expired_frontiers_pessimistically (ysu::transaction const &, uint64_t, uint64_t &);
	void frontiers_confirmation (ysu::unique_lock<std::mutex> &);
	bool insert_election_from_frontiers_confirmation (std::shared_ptr<ysu::block> const &, ysu::account const &, ysu::uint128_t, ysu::election_behavior);
	ysu::account next_frontier_account{ 0 };
	std::chrono::steady_clock::time_point next_frontier_check{ std::chrono::steady_clock::now () };
	constexpr static size_t max_active_elections_frontier_insertion{ 1000 };
	prioritize_num_uncemented priority_wallet_cementable_frontiers;
	prioritize_num_uncemented priority_cementable_frontiers;
	std::unordered_set<ysu::wallet_id> wallet_ids_already_iterated;
	std::unordered_map<ysu::wallet_id, ysu::account> next_wallet_id_accounts;
	bool skip_wallets{ false };
	std::atomic<unsigned> optimistic_elections_count{ 0 };
	void prioritize_frontiers_for_confirmation (ysu::transaction const &, std::chrono::milliseconds, std::chrono::milliseconds);
	bool prioritize_account_for_confirmation (prioritize_num_uncemented &, size_t &, ysu::account const &, ysu::account_info const &, uint64_t);
	unsigned max_optimistic ();
	void set_next_frontier_check (bool);
	void add_expired_optimistic_election (ysu::election const &);
	bool should_do_frontiers_confirmation () const;
	static size_t constexpr max_priority_cementable_frontiers{ 100000 };
	static size_t constexpr confirmed_frontiers_max_pending_size{ 10000 };
	static std::chrono::minutes constexpr expired_optimistic_election_info_cutoff{ 30 };
	// clang-format off
	using ordered_cache = boost::multi_index_container<ysu::inactive_cache_information,
	mi::indexed_by<
		mi::ordered_non_unique<mi::tag<tag_arrival>,
			mi::member<ysu::inactive_cache_information, std::chrono::steady_clock::time_point, &ysu::inactive_cache_information::arrival>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<ysu::inactive_cache_information, ysu::block_hash, &ysu::inactive_cache_information::hash>>>>;
	ordered_cache inactive_votes_cache;
	// clang-format on
	ysu::inactive_cache_status inactive_votes_bootstrap_check (std::vector<ysu::account> const &, ysu::block_hash const &, ysu::inactive_cache_status const &);
	boost::thread thread;

	friend class election;
	friend std::unique_ptr<container_info_component> collect_container_info (active_transactions &, const std::string &);

	friend class active_transactions_dropped_cleanup_dev;
	friend class active_transactions_vote_replays_Test;
	friend class frontiers_confirmation_prioritize_frontiers_Test;
	friend class frontiers_confirmation_prioritize_frontiers_max_optimistic_elections_Test;
	friend class confirmation_height_prioritize_frontiers_overwrite_Test;
	friend class active_transactions_confirmation_consistency_Test;
	friend class node_deferred_dependent_elections_Test;
	friend class active_transactions_pessimistic_elections_Test;
	friend class frontiers_confirmation_expired_optimistic_elections_removal_Test;
};

std::unique_ptr<container_info_component> collect_container_info (active_transactions & active_transactions, const std::string & name);
}
