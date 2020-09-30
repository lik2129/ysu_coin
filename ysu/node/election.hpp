#pragma once

#include <ysu/secure/blockstore.hpp>
#include <ysu/secure/common.hpp>
#include <ysu/secure/ledger.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_set>

namespace ysu
{
class channel;
class confirmation_solicitor;
class json_handler;
class node;
class vote_generator_session;
class vote_info final
{
public:
	std::chrono::steady_clock::time_point time;
	uint64_t sequence;
	ysu::block_hash hash;
};
class election_vote_result final
{
public:
	election_vote_result () = default;
	election_vote_result (bool, bool);
	bool replay{ false };
	bool processed{ false };
};
enum class election_behavior
{
	normal,
	optimistic
};
struct election_cleanup_info final
{
	bool confirmed;
	ysu::qualified_root root;
	ysu::block_hash winner;
	std::unordered_map<ysu::block_hash, std::shared_ptr<ysu::block>> blocks;
};

class election final : public std::enable_shared_from_this<ysu::election>
{
	// Minimum time between broadcasts of the current winner of an election, as a backup to requesting confirmations
	std::chrono::milliseconds base_latency () const;
	std::function<void(std::shared_ptr<ysu::block>)> confirmation_action;

private: // State management
	enum class state_t
	{
		passive, // only listening for incoming votes
		active, // actively request confirmations
		broadcasting, // request confirmations and broadcast the winner
		confirmed, // confirmed but still listening for votes
		expired_confirmed,
		expired_unconfirmed
	};
	static unsigned constexpr passive_duration_factor = 5;
	static unsigned constexpr active_request_count_min = 2;
	static unsigned constexpr confirmed_duration_factor = 5;
	std::atomic<ysu::election::state_t> state_m = { state_t::passive };

	// These time points must be protected by this mutex
	std::mutex timepoints_mutex;
	std::chrono::steady_clock::time_point state_start = { std::chrono::steady_clock::now () };
	std::chrono::steady_clock::time_point last_block = { std::chrono::steady_clock::now () };
	std::chrono::steady_clock::time_point last_req = { std::chrono::steady_clock::time_point () };

	bool valid_change (ysu::election::state_t, ysu::election::state_t) const;
	bool state_change (ysu::election::state_t, ysu::election::state_t);
	std::atomic<bool> prioritized_m = { false };

public: // State transitions
	bool transition_time (ysu::confirmation_solicitor &);
	void transition_active ();

public: // Status
	bool confirmed () const;
	bool failed () const;
	bool prioritized () const;
	bool optimistic () const;
	std::shared_ptr<ysu::block> winner ();

	void log_votes (ysu::tally_t const &, std::string const & = "") const;
	ysu::tally_t tally ();
	bool have_quorum (ysu::tally_t const &, ysu::uint128_t) const;

	ysu::election_status status;
	unsigned confirmation_request_count{ 0 };

public: // Interface
	election (ysu::node &, std::shared_ptr<ysu::block>, std::function<void(std::shared_ptr<ysu::block>)> const &, bool, ysu::election_behavior);
	ysu::election_vote_result vote (ysu::account, uint64_t, ysu::block_hash);
	bool publish (std::shared_ptr<ysu::block> block_a);
	size_t insert_inactive_votes_cache (ysu::block_hash const &);
	// Confirm this block if quorum is met
	void confirm_if_quorum ();
	void prioritize_election (ysu::vote_generator_session &);
	ysu::election_cleanup_info cleanup_info () const;

public: // Information
	uint64_t const height;
	ysu::root const root;

private:
	void transition_active_impl ();
	void confirm_once (ysu::election_status_type = ysu::election_status_type::active_confirmed_quorum);
	void broadcast_block (ysu::confirmation_solicitor &);
	void send_confirm_req (ysu::confirmation_solicitor &);
	// Calculate votes for local representatives
	void generate_votes ();
	void remove_votes (ysu::block_hash const &);

private:
	std::unordered_map<ysu::block_hash, std::shared_ptr<ysu::block>> last_blocks;
	std::unordered_map<ysu::account, ysu::vote_info> last_votes;
	std::unordered_map<ysu::block_hash, ysu::uint128_t> last_tally;

	ysu::election_behavior const behavior{ ysu::election_behavior::normal };
	std::chrono::steady_clock::time_point const election_start = { std::chrono::steady_clock::now () };

	ysu::node & node;

	static std::chrono::seconds constexpr late_blocks_delay{ 5 };

	friend class active_transactions;
	friend class confirmation_solicitor;
	friend class json_handler;

public: // Only used in tests
	void force_confirm (ysu::election_status_type = ysu::election_status_type::active_confirmed_quorum);
	std::unordered_map<ysu::account, ysu::vote_info> votes ();
	std::unordered_map<ysu::block_hash, std::shared_ptr<ysu::block>> blocks ();

	friend class confirmation_solicitor_different_hash_Test;
	friend class confirmation_solicitor_bypass_max_requests_cap_Test;
	friend class votes_add_existing_Test;
	friend class votes_add_old_Test;
};
}
