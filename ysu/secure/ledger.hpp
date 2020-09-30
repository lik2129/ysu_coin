#pragma once

#include <ysu/lib/rep_weights.hpp>
#include <ysu/secure/common.hpp>

#include <map>

namespace ysu
{
class block_store;
class stat;
class write_transaction;

using tally_t = std::map<ysu::uint128_t, std::shared_ptr<ysu::block>, std::greater<ysu::uint128_t>>;

class uncemented_info
{
public:
	uncemented_info (ysu::block_hash const & cemented_frontier, ysu::block_hash const & frontier, ysu::account const & account);
	ysu::block_hash cemented_frontier;
	ysu::block_hash frontier;
	ysu::account account;
};

class ledger final
{
public:
	ledger (ysu::block_store &, ysu::stat &, ysu::generate_cache const & = ysu::generate_cache (), std::function<void()> = nullptr);
	ysu::account account (ysu::transaction const &, ysu::block_hash const &) const;
	ysu::account account_safe (ysu::transaction const &, ysu::block_hash const &, bool &) const;
	ysu::uint128_t amount (ysu::transaction const &, ysu::account const &);
	ysu::uint128_t amount (ysu::transaction const &, ysu::block_hash const &);
	/** Safe for previous block, but block hash_a must exist */
	ysu::uint128_t amount_safe (ysu::transaction const &, ysu::block_hash const & hash_a, bool &) const;
	ysu::uint128_t balance (ysu::transaction const &, ysu::block_hash const &) const;
	ysu::uint128_t balance_safe (ysu::transaction const &, ysu::block_hash const &, bool &) const;
	ysu::uint128_t account_balance (ysu::transaction const &, ysu::account const &);
	ysu::uint128_t account_pending (ysu::transaction const &, ysu::account const &);
	ysu::uint128_t weight (ysu::account const &);
	std::shared_ptr<ysu::block> successor (ysu::transaction const &, ysu::qualified_root const &);
	std::shared_ptr<ysu::block> forked_block (ysu::transaction const &, ysu::block const &);
	bool block_confirmed (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const;
	ysu::block_hash latest (ysu::transaction const &, ysu::account const &);
	ysu::root latest_root (ysu::transaction const &, ysu::account const &);
	ysu::block_hash representative (ysu::transaction const &, ysu::block_hash const &);
	ysu::block_hash representative_calculated (ysu::transaction const &, ysu::block_hash const &);
	bool block_exists (ysu::block_hash const &) const;
	bool block_or_pruned_exists (ysu::transaction const &, ysu::block_hash const &) const;
	bool block_or_pruned_exists (ysu::block_hash const &) const;
	std::string block_text (char const *);
	std::string block_text (ysu::block_hash const &);
	bool is_send (ysu::transaction const &, ysu::state_block const &) const;
	ysu::account const & block_destination (ysu::transaction const &, ysu::block const &);
	ysu::block_hash block_source (ysu::transaction const &, ysu::block const &);
	std::pair<ysu::block_hash, ysu::block_hash> hash_root_random (ysu::transaction const &) const;
	ysu::process_return process (ysu::write_transaction const &, ysu::block &, ysu::signature_verification = ysu::signature_verification::unknown);
	bool rollback (ysu::write_transaction const &, ysu::block_hash const &, std::vector<std::shared_ptr<ysu::block>> &);
	bool rollback (ysu::write_transaction const &, ysu::block_hash const &);
	void update_account (ysu::write_transaction const &, ysu::account const &, ysu::account_info const &, ysu::account_info const &);
	uint64_t pruning_action (ysu::write_transaction &, ysu::block_hash const &, uint64_t const);
	void dump_account_chain (ysu::account const &, std::ostream & = std::cout);
	bool could_fit (ysu::transaction const &, ysu::block const &) const;
	bool dependents_confirmed (ysu::transaction const &, ysu::block const &) const;
	bool is_epoch_link (ysu::link const &) const;
	std::array<ysu::block_hash, 2> dependent_blocks (ysu::transaction const &, ysu::block const &) const;
	ysu::account const & epoch_signer (ysu::link const &) const;
	ysu::link const & epoch_link (ysu::epoch) const;
	std::multimap<uint64_t, uncemented_info, std::greater<>> unconfirmed_frontiers () const;
	static ysu::uint128_t const unit;
	ysu::network_params network_params;
	ysu::block_store & store;
	ysu::ledger_cache cache;
	ysu::stat & stats;
	std::unordered_map<ysu::account, ysu::uint128_t> bootstrap_weights;
	std::atomic<size_t> bootstrap_weights_size{ 0 };
	uint64_t bootstrap_weight_max_blocks{ 1 };
	std::atomic<bool> check_bootstrap_weights;
	bool pruning{ false };
	std::function<void()> epoch_2_started_cb;

private:
	void initialize (ysu::generate_cache const &);
};

std::unique_ptr<container_info_component> collect_container_info (ledger & ledger, const std::string & name);
}
