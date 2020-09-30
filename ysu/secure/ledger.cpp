#include <ysu/lib/rep_weights.hpp>
#include <ysu/lib/stats.hpp>
#include <ysu/lib/utility.hpp>
#include <ysu/lib/work.hpp>
#include <ysu/secure/blockstore.hpp>
#include <ysu/secure/common.hpp>
#include <ysu/secure/ledger.hpp>

#include <crypto/cryptopp/words.h>

namespace
{
/**
 * Roll back the visited block
 */
class rollback_visitor : public ysu::block_visitor
{
public:
	rollback_visitor (ysu::write_transaction const & transaction_a, ysu::ledger & ledger_a, std::vector<std::shared_ptr<ysu::block>> & list_a) :
	transaction (transaction_a),
	ledger (ledger_a),
	list (list_a)
	{
	}
	virtual ~rollback_visitor () = default;
	void send_block (ysu::send_block const & block_a) override
	{
		auto hash (block_a.hash ());
		ysu::pending_info pending;
		ysu::pending_key key (block_a.hashables.destination, hash);
		while (!error && ledger.store.pending_get (transaction, key, pending))
		{
			error = ledger.rollback (transaction, ledger.latest (transaction, block_a.hashables.destination), list);
		}
		if (!error)
		{
			ysu::account_info info;
			auto error (ledger.store.account_get (transaction, pending.source, info));
			(void)error;
			debug_assert (!error);
			ledger.store.pending_del (transaction, key);
			ledger.cache.rep_weights.representation_add (info.representative, pending.amount.number ());
			ysu::account_info new_info (block_a.hashables.previous, info.representative, info.open_block, ledger.balance (transaction, block_a.hashables.previous), ysu::seconds_since_epoch (), info.block_count - 1, ysu::epoch::epoch_0);
			ledger.update_account (transaction, pending.source, info, new_info);
			ledger.store.block_del (transaction, hash);
			ledger.store.frontier_del (transaction, hash);
			ledger.store.frontier_put (transaction, block_a.hashables.previous, pending.source);
			ledger.store.block_successor_clear (transaction, block_a.hashables.previous);
			ledger.stats.inc (ysu::stat::type::rollback, ysu::stat::detail::send);
		}
	}
	void receive_block (ysu::receive_block const & block_a) override
	{
		auto hash (block_a.hash ());
		auto amount (ledger.amount (transaction, block_a.hashables.source));
		auto destination_account (ledger.account (transaction, hash));
		auto source_account (ledger.account (transaction, block_a.hashables.source));
		ysu::account_info info;
		auto error (ledger.store.account_get (transaction, destination_account, info));
		(void)error;
		debug_assert (!error);
		ledger.cache.rep_weights.representation_add (info.representative, 0 - amount);
		ysu::account_info new_info (block_a.hashables.previous, info.representative, info.open_block, ledger.balance (transaction, block_a.hashables.previous), ysu::seconds_since_epoch (), info.block_count - 1, ysu::epoch::epoch_0);
		ledger.update_account (transaction, destination_account, info, new_info);
		ledger.store.block_del (transaction, hash);
		ledger.store.pending_put (transaction, ysu::pending_key (destination_account, block_a.hashables.source), { source_account, amount, ysu::epoch::epoch_0 });
		ledger.store.frontier_del (transaction, hash);
		ledger.store.frontier_put (transaction, block_a.hashables.previous, destination_account);
		ledger.store.block_successor_clear (transaction, block_a.hashables.previous);
		ledger.stats.inc (ysu::stat::type::rollback, ysu::stat::detail::receive);
	}
	void open_block (ysu::open_block const & block_a) override
	{
		auto hash (block_a.hash ());
		auto amount (ledger.amount (transaction, block_a.hashables.source));
		auto destination_account (ledger.account (transaction, hash));
		auto source_account (ledger.account (transaction, block_a.hashables.source));
		ledger.cache.rep_weights.representation_add (block_a.representative (), 0 - amount);
		ysu::account_info new_info;
		ledger.update_account (transaction, destination_account, new_info, new_info);
		ledger.store.block_del (transaction, hash);
		ledger.store.pending_put (transaction, ysu::pending_key (destination_account, block_a.hashables.source), { source_account, amount, ysu::epoch::epoch_0 });
		ledger.store.frontier_del (transaction, hash);
		ledger.stats.inc (ysu::stat::type::rollback, ysu::stat::detail::open);
	}
	void change_block (ysu::change_block const & block_a) override
	{
		auto hash (block_a.hash ());
		auto rep_block (ledger.representative (transaction, block_a.hashables.previous));
		auto account (ledger.account (transaction, block_a.hashables.previous));
		ysu::account_info info;
		auto error (ledger.store.account_get (transaction, account, info));
		(void)error;
		debug_assert (!error);
		auto balance (ledger.balance (transaction, block_a.hashables.previous));
		auto block = ledger.store.block_get (transaction, rep_block);
		release_assert (block != nullptr);
		auto representative = block->representative ();
		ledger.cache.rep_weights.representation_add_dual (block_a.representative (), 0 - balance, representative, balance);
		ledger.store.block_del (transaction, hash);
		ysu::account_info new_info (block_a.hashables.previous, representative, info.open_block, info.balance, ysu::seconds_since_epoch (), info.block_count - 1, ysu::epoch::epoch_0);
		ledger.update_account (transaction, account, info, new_info);
		ledger.store.frontier_del (transaction, hash);
		ledger.store.frontier_put (transaction, block_a.hashables.previous, account);
		ledger.store.block_successor_clear (transaction, block_a.hashables.previous);
		ledger.stats.inc (ysu::stat::type::rollback, ysu::stat::detail::change);
	}
	void state_block (ysu::state_block const & block_a) override
	{
		auto hash (block_a.hash ());
		ysu::block_hash rep_block_hash (0);
		if (!block_a.hashables.previous.is_zero ())
		{
			rep_block_hash = ledger.representative (transaction, block_a.hashables.previous);
		}
		auto balance (ledger.balance (transaction, block_a.hashables.previous));
		auto is_send (block_a.hashables.balance < balance);
		ysu::account representative{ 0 };
		if (!rep_block_hash.is_zero ())
		{
			// Move existing representation & add in amount delta
			auto block (ledger.store.block_get (transaction, rep_block_hash));
			debug_assert (block != nullptr);
			representative = block->representative ();
			ledger.cache.rep_weights.representation_add_dual (representative, balance, block_a.representative (), 0 - block_a.hashables.balance.number ());
		}
		else
		{
			// Add in amount delta only
			ledger.cache.rep_weights.representation_add (block_a.representative (), 0 - block_a.hashables.balance.number ());
		}

		ysu::account_info info;
		auto error (ledger.store.account_get (transaction, block_a.hashables.account, info));

		if (is_send)
		{
			ysu::pending_key key (block_a.hashables.link.as_account (), hash);
			while (!error && !ledger.store.pending_exists (transaction, key))
			{
				error = ledger.rollback (transaction, ledger.latest (transaction, block_a.hashables.link.as_account ()), list);
			}
			ledger.store.pending_del (transaction, key);
			ledger.stats.inc (ysu::stat::type::rollback, ysu::stat::detail::send);
		}
		else if (!block_a.hashables.link.is_zero () && !ledger.is_epoch_link (block_a.hashables.link))
		{
			ysu::pending_info pending_info (ledger.account (transaction, block_a.hashables.link.as_block_hash ()), block_a.hashables.balance.number () - balance, block_a.sideband ().source_epoch);
			ledger.store.pending_put (transaction, ysu::pending_key (block_a.hashables.account, block_a.hashables.link.as_block_hash ()), pending_info);
			ledger.stats.inc (ysu::stat::type::rollback, ysu::stat::detail::receive);
		}

		debug_assert (!error);
		auto previous_version (ledger.store.block_version (transaction, block_a.hashables.previous));
		ysu::account_info new_info (block_a.hashables.previous, representative, info.open_block, balance, ysu::seconds_since_epoch (), info.block_count - 1, previous_version);
		ledger.update_account (transaction, block_a.hashables.account, info, new_info);

		auto previous (ledger.store.block_get (transaction, block_a.hashables.previous));
		if (previous != nullptr)
		{
			ledger.store.block_successor_clear (transaction, block_a.hashables.previous);
			if (previous->type () < ysu::block_type::state)
			{
				ledger.store.frontier_put (transaction, block_a.hashables.previous, block_a.hashables.account);
			}
		}
		else
		{
			ledger.stats.inc (ysu::stat::type::rollback, ysu::stat::detail::open);
		}
		ledger.store.block_del (transaction, hash);
	}
	ysu::write_transaction const & transaction;
	ysu::ledger & ledger;
	std::vector<std::shared_ptr<ysu::block>> & list;
	bool error{ false };
};

class ledger_processor : public ysu::mutable_block_visitor
{
public:
	ledger_processor (ysu::ledger &, ysu::write_transaction const &, ysu::signature_verification = ysu::signature_verification::unknown);
	virtual ~ledger_processor () = default;
	void send_block (ysu::send_block &) override;
	void receive_block (ysu::receive_block &) override;
	void open_block (ysu::open_block &) override;
	void change_block (ysu::change_block &) override;
	void state_block (ysu::state_block &) override;
	void state_block_impl (ysu::state_block &);
	void epoch_block_impl (ysu::state_block &);
	ysu::ledger & ledger;
	ysu::write_transaction const & transaction;
	ysu::signature_verification verification;
	ysu::process_return result;

private:
	bool validate_epoch_block (ysu::state_block const & block_a);
};

// Returns true if this block which has an epoch link is correctly formed.
bool ledger_processor::validate_epoch_block (ysu::state_block const & block_a)
{
	debug_assert (ledger.is_epoch_link (block_a.hashables.link));
	ysu::amount prev_balance (0);
	if (!block_a.hashables.previous.is_zero ())
	{
		result.code = ledger.store.block_exists (transaction, block_a.hashables.previous) ? ysu::process_result::progress : ysu::process_result::gap_previous;
		if (result.code == ysu::process_result::progress)
		{
			prev_balance = ledger.balance (transaction, block_a.hashables.previous);
		}
		else if (result.verified == ysu::signature_verification::unknown)
		{
			// Check for possible regular state blocks with epoch link (send subtype)
			if (validate_message (block_a.hashables.account, block_a.hash (), block_a.signature))
			{
				// Is epoch block signed correctly
				if (validate_message (ledger.epoch_signer (block_a.link ()), block_a.hash (), block_a.signature))
				{
					result.verified = ysu::signature_verification::invalid;
					result.code = ysu::process_result::bad_signature;
				}
				else
				{
					result.verified = ysu::signature_verification::valid_epoch;
				}
			}
			else
			{
				result.verified = ysu::signature_verification::valid;
			}
		}
	}
	return (block_a.hashables.balance == prev_balance);
}

void ledger_processor::state_block (ysu::state_block & block_a)
{
	result.code = ysu::process_result::progress;
	auto is_epoch_block = false;
	if (ledger.is_epoch_link (block_a.hashables.link))
	{
		// This function also modifies the result variable if epoch is mal-formed
		is_epoch_block = validate_epoch_block (block_a);
	}

	if (result.code == ysu::process_result::progress)
	{
		if (is_epoch_block)
		{
			epoch_block_impl (block_a);
		}
		else
		{
			state_block_impl (block_a);
		}
	}
}

void ledger_processor::state_block_impl (ysu::state_block & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, hash));
	result.code = existing ? ysu::process_result::old : ysu::process_result::progress; // Have we seen this block before? (Unambiguous)
	if (result.code == ysu::process_result::progress)
	{
		// Validate block if not verified outside of ledger
		if (result.verified != ysu::signature_verification::valid)
		{
			result.code = validate_message (block_a.hashables.account, hash, block_a.signature) ? ysu::process_result::bad_signature : ysu::process_result::progress; // Is this block signed correctly (Unambiguous)
		}
		if (result.code == ysu::process_result::progress)
		{
			debug_assert (!validate_message (block_a.hashables.account, hash, block_a.signature));
			result.verified = ysu::signature_verification::valid;
			result.code = block_a.hashables.account.is_zero () ? ysu::process_result::opened_burn_account : ysu::process_result::progress; // Is this for the burn account? (Unambiguous)
			if (result.code == ysu::process_result::progress)
			{
				ysu::epoch epoch (ysu::epoch::epoch_0);
				ysu::epoch source_epoch (ysu::epoch::epoch_0);
				ysu::account_info info;
				ysu::amount amount (block_a.hashables.balance);
				auto is_send (false);
				auto is_receive (false);
				auto account_error (ledger.store.account_get (transaction, block_a.hashables.account, info));
				if (!account_error)
				{
					// Account already exists
					epoch = info.epoch ();
					result.previous_balance = info.balance;
					result.code = block_a.hashables.previous.is_zero () ? ysu::process_result::fork : ysu::process_result::progress; // Has this account already been opened? (Ambigious)
					if (result.code == ysu::process_result::progress)
					{
						result.code = ledger.store.block_exists (transaction, block_a.hashables.previous) ? ysu::process_result::progress : ysu::process_result::gap_previous; // Does the previous block exist in the ledger? (Unambigious)
						if (result.code == ysu::process_result::progress)
						{
							is_send = block_a.hashables.balance < info.balance;
							is_receive = !is_send && !block_a.hashables.link.is_zero ();
							amount = is_send ? (info.balance.number () - amount.number ()) : (amount.number () - info.balance.number ());
							result.code = block_a.hashables.previous == info.head ? ysu::process_result::progress : ysu::process_result::fork; // Is the previous block the account's head block? (Ambigious)
						}
					}
				}
				else
				{
					// Account does not yet exists
					result.previous_balance = 0;
					result.code = block_a.previous ().is_zero () ? ysu::process_result::progress : ysu::process_result::gap_previous; // Does the first block in an account yield 0 for previous() ? (Unambigious)
					if (result.code == ysu::process_result::progress)
					{
						is_receive = true;
						result.code = !block_a.hashables.link.is_zero () ? ysu::process_result::progress : ysu::process_result::gap_source; // Is the first block receiving from a send ? (Unambigious)
					}
				}
				if (result.code == ysu::process_result::progress)
				{
					if (!is_send)
					{
						if (!block_a.hashables.link.is_zero ())
						{
							result.code = ledger.store.block_exists (transaction, block_a.hashables.link.as_block_hash ()) ? ysu::process_result::progress : ysu::process_result::gap_source; // Have we seen the source block already? (Harmless)
							if (result.code == ysu::process_result::progress)
							{
								ysu::pending_key key (block_a.hashables.account, block_a.hashables.link.as_block_hash ());
								ysu::pending_info pending;
								result.code = ledger.store.pending_get (transaction, key, pending) ? ysu::process_result::unreceivable : ysu::process_result::progress; // Has this source already been received (Malformed)
								if (result.code == ysu::process_result::progress)
								{
									result.code = amount == pending.amount ? ysu::process_result::progress : ysu::process_result::balance_mismatch;
									source_epoch = pending.epoch;
									epoch = std::max (epoch, source_epoch);
								}
							}
						}
						else
						{
							// If there's no link, the balance must remain the same, only the representative can change
							result.code = amount.is_zero () ? ysu::process_result::progress : ysu::process_result::balance_mismatch;
						}
					}
				}
				if (result.code == ysu::process_result::progress)
				{
					ysu::block_details block_details (epoch, is_send, is_receive, false);
					result.code = block_a.difficulty () >= ysu::work_threshold (block_a.work_version (), block_details) ? ysu::process_result::progress : ysu::process_result::insufficient_work; // Does this block have sufficient work? (Malformed)
					if (result.code == ysu::process_result::progress)
					{
						ledger.stats.inc (ysu::stat::type::ledger, ysu::stat::detail::state_block);
						block_a.sideband_set (ysu::block_sideband (block_a.hashables.account /* unused */, 0, 0 /* unused */, info.block_count + 1, ysu::seconds_since_epoch (), block_details, source_epoch));
						ledger.store.block_put (transaction, hash, block_a);

						if (!info.head.is_zero ())
						{
							// Move existing representation & add in amount delta
							ledger.cache.rep_weights.representation_add_dual (info.representative, 0 - info.balance.number (), block_a.representative (), block_a.hashables.balance.number ());
						}
						else
						{
							// Add in amount delta only
							ledger.cache.rep_weights.representation_add (block_a.representative (), block_a.hashables.balance.number ());
						}

						if (is_send)
						{
							ysu::pending_key key (block_a.hashables.link.as_account (), hash);
							ysu::pending_info info (block_a.hashables.account, amount.number (), epoch);
							ledger.store.pending_put (transaction, key, info);
						}
						else if (!block_a.hashables.link.is_zero ())
						{
							ledger.store.pending_del (transaction, ysu::pending_key (block_a.hashables.account, block_a.hashables.link.as_block_hash ()));
						}

						ysu::account_info new_info (hash, block_a.representative (), info.open_block.is_zero () ? hash : info.open_block, block_a.hashables.balance, ysu::seconds_since_epoch (), info.block_count + 1, epoch);
						ledger.update_account (transaction, block_a.hashables.account, info, new_info);
						if (!ledger.store.frontier_get (transaction, info.head).is_zero ())
						{
							ledger.store.frontier_del (transaction, info.head);
						}
					}
				}
			}
		}
	}
}

void ledger_processor::epoch_block_impl (ysu::state_block & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, hash));
	result.code = existing ? ysu::process_result::old : ysu::process_result::progress; // Have we seen this block before? (Unambiguous)
	if (result.code == ysu::process_result::progress)
	{
		// Validate block if not verified outside of ledger
		if (result.verified != ysu::signature_verification::valid_epoch)
		{
			result.code = validate_message (ledger.epoch_signer (block_a.hashables.link), hash, block_a.signature) ? ysu::process_result::bad_signature : ysu::process_result::progress; // Is this block signed correctly (Unambiguous)
		}
		if (result.code == ysu::process_result::progress)
		{
			debug_assert (!validate_message (ledger.epoch_signer (block_a.hashables.link), hash, block_a.signature));
			result.verified = ysu::signature_verification::valid_epoch;
			result.code = block_a.hashables.account.is_zero () ? ysu::process_result::opened_burn_account : ysu::process_result::progress; // Is this for the burn account? (Unambiguous)
			if (result.code == ysu::process_result::progress)
			{
				ysu::account_info info;
				auto account_error (ledger.store.account_get (transaction, block_a.hashables.account, info));
				if (!account_error)
				{
					// Account already exists
					result.previous_balance = info.balance;
					result.code = block_a.hashables.previous.is_zero () ? ysu::process_result::fork : ysu::process_result::progress; // Has this account already been opened? (Ambigious)
					if (result.code == ysu::process_result::progress)
					{
						result.code = block_a.hashables.previous == info.head ? ysu::process_result::progress : ysu::process_result::fork; // Is the previous block the account's head block? (Ambigious)
						if (result.code == ysu::process_result::progress)
						{
							result.code = block_a.hashables.representative == info.representative ? ysu::process_result::progress : ysu::process_result::representative_mismatch;
						}
					}
				}
				else
				{
					result.previous_balance = 0;
					result.code = block_a.hashables.representative.is_zero () ? ysu::process_result::progress : ysu::process_result::representative_mismatch;
					// Non-exisitng account should have pending entries
					if (result.code == ysu::process_result::progress)
					{
						bool pending_exists = ledger.store.pending_any (transaction, block_a.hashables.account);
						result.code = pending_exists ? ysu::process_result::progress : ysu::process_result::block_position;
					}
				}
				if (result.code == ysu::process_result::progress)
				{
					auto epoch = ledger.network_params.ledger.epochs.epoch (block_a.hashables.link);
					// Must be an epoch for an unopened account or the epoch upgrade must be sequential
					auto is_valid_epoch_upgrade = account_error ? static_cast<std::underlying_type_t<ysu::epoch>> (epoch) > 0 : ysu::epochs::is_sequential (info.epoch (), epoch);
					result.code = is_valid_epoch_upgrade ? ysu::process_result::progress : ysu::process_result::block_position;
					if (result.code == ysu::process_result::progress)
					{
						result.code = block_a.hashables.balance == info.balance ? ysu::process_result::progress : ysu::process_result::balance_mismatch;
						if (result.code == ysu::process_result::progress)
						{
							ysu::block_details block_details (epoch, false, false, true);
							result.code = block_a.difficulty () >= ysu::work_threshold (block_a.work_version (), block_details) ? ysu::process_result::progress : ysu::process_result::insufficient_work; // Does this block have sufficient work? (Malformed)
							if (result.code == ysu::process_result::progress)
							{
								ledger.stats.inc (ysu::stat::type::ledger, ysu::stat::detail::epoch_block);
								block_a.sideband_set (ysu::block_sideband (block_a.hashables.account /* unused */, 0, 0 /* unused */, info.block_count + 1, ysu::seconds_since_epoch (), block_details, ysu::epoch::epoch_0 /* unused */));
								ledger.store.block_put (transaction, hash, block_a);
								ysu::account_info new_info (hash, block_a.representative (), info.open_block.is_zero () ? hash : info.open_block, info.balance, ysu::seconds_since_epoch (), info.block_count + 1, epoch);
								ledger.update_account (transaction, block_a.hashables.account, info, new_info);
								if (!ledger.store.frontier_get (transaction, info.head).is_zero ())
								{
									ledger.store.frontier_del (transaction, info.head);
								}
								if (epoch == ysu::epoch::epoch_2)
								{
									if (!ledger.cache.epoch_2_started.exchange (true))
									{
										// The first epoch 2 block has been seen
										if (ledger.epoch_2_started_cb)
										{
											ledger.epoch_2_started_cb ();
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
}

void ledger_processor::change_block (ysu::change_block & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, hash));
	result.code = existing ? ysu::process_result::old : ysu::process_result::progress; // Have we seen this block before? (Harmless)
	if (result.code == ysu::process_result::progress)
	{
		auto previous (ledger.store.block_get (transaction, block_a.hashables.previous));
		result.code = previous != nullptr ? ysu::process_result::progress : ysu::process_result::gap_previous; // Have we seen the previous block already? (Harmless)
		if (result.code == ysu::process_result::progress)
		{
			result.code = block_a.valid_predecessor (*previous) ? ysu::process_result::progress : ysu::process_result::block_position;
			if (result.code == ysu::process_result::progress)
			{
				auto account (ledger.store.frontier_get (transaction, block_a.hashables.previous));
				result.code = account.is_zero () ? ysu::process_result::fork : ysu::process_result::progress;
				if (result.code == ysu::process_result::progress)
				{
					ysu::account_info info;
					auto latest_error (ledger.store.account_get (transaction, account, info));
					(void)latest_error;
					debug_assert (!latest_error);
					debug_assert (info.head == block_a.hashables.previous);
					// Validate block if not verified outside of ledger
					if (result.verified != ysu::signature_verification::valid)
					{
						result.code = validate_message (account, hash, block_a.signature) ? ysu::process_result::bad_signature : ysu::process_result::progress; // Is this block signed correctly (Malformed)
					}
					if (result.code == ysu::process_result::progress)
					{
						ysu::block_details block_details (ysu::epoch::epoch_0, false /* unused */, false /* unused */, false /* unused */);
						result.code = block_a.difficulty () >= ysu::work_threshold (block_a.work_version (), block_details) ? ysu::process_result::progress : ysu::process_result::insufficient_work; // Does this block have sufficient work? (Malformed)
						if (result.code == ysu::process_result::progress)
						{
							debug_assert (!validate_message (account, hash, block_a.signature));
							result.verified = ysu::signature_verification::valid;
							block_a.sideband_set (ysu::block_sideband (account, 0, info.balance, info.block_count + 1, ysu::seconds_since_epoch (), block_details, ysu::epoch::epoch_0 /* unused */));
							ledger.store.block_put (transaction, hash, block_a);
							auto balance (ledger.balance (transaction, block_a.hashables.previous));
							ledger.cache.rep_weights.representation_add_dual (block_a.representative (), balance, info.representative, 0 - balance);
							ysu::account_info new_info (hash, block_a.representative (), info.open_block, info.balance, ysu::seconds_since_epoch (), info.block_count + 1, ysu::epoch::epoch_0);
							ledger.update_account (transaction, account, info, new_info);
							ledger.store.frontier_del (transaction, block_a.hashables.previous);
							ledger.store.frontier_put (transaction, hash, account);
							result.previous_balance = info.balance;
							ledger.stats.inc (ysu::stat::type::ledger, ysu::stat::detail::change);
						}
					}
				}
			}
		}
	}
}

void ledger_processor::send_block (ysu::send_block & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, hash));
	result.code = existing ? ysu::process_result::old : ysu::process_result::progress; // Have we seen this block before? (Harmless)
	if (result.code == ysu::process_result::progress)
	{
		auto previous (ledger.store.block_get (transaction, block_a.hashables.previous));
		result.code = previous != nullptr ? ysu::process_result::progress : ysu::process_result::gap_previous; // Have we seen the previous block already? (Harmless)
		if (result.code == ysu::process_result::progress)
		{
			result.code = block_a.valid_predecessor (*previous) ? ysu::process_result::progress : ysu::process_result::block_position;
			if (result.code == ysu::process_result::progress)
			{
				auto account (ledger.store.frontier_get (transaction, block_a.hashables.previous));
				result.code = account.is_zero () ? ysu::process_result::fork : ysu::process_result::progress;
				if (result.code == ysu::process_result::progress)
				{
					// Validate block if not verified outside of ledger
					if (result.verified != ysu::signature_verification::valid)
					{
						result.code = validate_message (account, hash, block_a.signature) ? ysu::process_result::bad_signature : ysu::process_result::progress; // Is this block signed correctly (Malformed)
					}
					if (result.code == ysu::process_result::progress)
					{
						ysu::block_details block_details (ysu::epoch::epoch_0, false /* unused */, false /* unused */, false /* unused */);
						result.code = block_a.difficulty () >= ysu::work_threshold (block_a.work_version (), block_details) ? ysu::process_result::progress : ysu::process_result::insufficient_work; // Does this block have sufficient work? (Malformed)
						if (result.code == ysu::process_result::progress)
						{
							debug_assert (!validate_message (account, hash, block_a.signature));
							result.verified = ysu::signature_verification::valid;
							ysu::account_info info;
							auto latest_error (ledger.store.account_get (transaction, account, info));
							(void)latest_error;
							debug_assert (!latest_error);
							debug_assert (info.head == block_a.hashables.previous);
							result.code = info.balance.number () >= block_a.hashables.balance.number () ? ysu::process_result::progress : ysu::process_result::negative_spend; // Is this trying to spend a negative amount (Malicious)
							if (result.code == ysu::process_result::progress)
							{
								auto amount (info.balance.number () - block_a.hashables.balance.number ());
								ledger.cache.rep_weights.representation_add (info.representative, 0 - amount);
								block_a.sideband_set (ysu::block_sideband (account, 0, block_a.hashables.balance /* unused */, info.block_count + 1, ysu::seconds_since_epoch (), block_details, ysu::epoch::epoch_0 /* unused */));
								ledger.store.block_put (transaction, hash, block_a);
								ysu::account_info new_info (hash, info.representative, info.open_block, block_a.hashables.balance, ysu::seconds_since_epoch (), info.block_count + 1, ysu::epoch::epoch_0);
								ledger.update_account (transaction, account, info, new_info);
								ledger.store.pending_put (transaction, ysu::pending_key (block_a.hashables.destination, hash), { account, amount, ysu::epoch::epoch_0 });
								ledger.store.frontier_del (transaction, block_a.hashables.previous);
								ledger.store.frontier_put (transaction, hash, account);
								result.previous_balance = info.balance;
								ledger.stats.inc (ysu::stat::type::ledger, ysu::stat::detail::send);
							}
						}
					}
				}
			}
		}
	}
}

void ledger_processor::receive_block (ysu::receive_block & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, hash));
	result.code = existing ? ysu::process_result::old : ysu::process_result::progress; // Have we seen this block already?  (Harmless)
	if (result.code == ysu::process_result::progress)
	{
		auto previous (ledger.store.block_get (transaction, block_a.hashables.previous));
		result.code = previous != nullptr ? ysu::process_result::progress : ysu::process_result::gap_previous;
		if (result.code == ysu::process_result::progress)
		{
			result.code = block_a.valid_predecessor (*previous) ? ysu::process_result::progress : ysu::process_result::block_position;
			if (result.code == ysu::process_result::progress)
			{
				auto account (ledger.store.frontier_get (transaction, block_a.hashables.previous));
				result.code = account.is_zero () ? ysu::process_result::gap_previous : ysu::process_result::progress; //Have we seen the previous block? No entries for account at all (Harmless)
				if (result.code == ysu::process_result::progress)
				{
					// Validate block if not verified outside of ledger
					if (result.verified != ysu::signature_verification::valid)
					{
						result.code = validate_message (account, hash, block_a.signature) ? ysu::process_result::bad_signature : ysu::process_result::progress; // Is the signature valid (Malformed)
					}
					if (result.code == ysu::process_result::progress)
					{
						debug_assert (!validate_message (account, hash, block_a.signature));
						result.verified = ysu::signature_verification::valid;
						result.code = ledger.store.block_exists (transaction, block_a.hashables.source) ? ysu::process_result::progress : ysu::process_result::gap_source; // Have we seen the source block already? (Harmless)
						if (result.code == ysu::process_result::progress)
						{
							ysu::account_info info;
							ledger.store.account_get (transaction, account, info);
							result.code = info.head == block_a.hashables.previous ? ysu::process_result::progress : ysu::process_result::gap_previous; // Block doesn't immediately follow latest block (Harmless)
							if (result.code == ysu::process_result::progress)
							{
								ysu::pending_key key (account, block_a.hashables.source);
								ysu::pending_info pending;
								result.code = ledger.store.pending_get (transaction, key, pending) ? ysu::process_result::unreceivable : ysu::process_result::progress; // Has this source already been received (Malformed)
								if (result.code == ysu::process_result::progress)
								{
									result.code = pending.epoch == ysu::epoch::epoch_0 ? ysu::process_result::progress : ysu::process_result::unreceivable; // Are we receiving a state-only send? (Malformed)
									if (result.code == ysu::process_result::progress)
									{
										ysu::block_details block_details (ysu::epoch::epoch_0, false /* unused */, false /* unused */, false /* unused */);
										result.code = block_a.difficulty () >= ysu::work_threshold (block_a.work_version (), block_details) ? ysu::process_result::progress : ysu::process_result::insufficient_work; // Does this block have sufficient work? (Malformed)
										if (result.code == ysu::process_result::progress)
										{
											auto new_balance (info.balance.number () + pending.amount.number ());
											ysu::account_info source_info;
											auto error (ledger.store.account_get (transaction, pending.source, source_info));
											(void)error;
											debug_assert (!error);
											ledger.store.pending_del (transaction, key);
											block_a.sideband_set (ysu::block_sideband (account, 0, new_balance, info.block_count + 1, ysu::seconds_since_epoch (), block_details, ysu::epoch::epoch_0 /* unused */));
											ledger.store.block_put (transaction, hash, block_a);
											ysu::account_info new_info (hash, info.representative, info.open_block, new_balance, ysu::seconds_since_epoch (), info.block_count + 1, ysu::epoch::epoch_0);
											ledger.update_account (transaction, account, info, new_info);
											ledger.cache.rep_weights.representation_add (info.representative, pending.amount.number ());
											ledger.store.frontier_del (transaction, block_a.hashables.previous);
											ledger.store.frontier_put (transaction, hash, account);
											result.previous_balance = info.balance;
											ledger.stats.inc (ysu::stat::type::ledger, ysu::stat::detail::receive);
										}
									}
								}
							}
						}
					}
				}
				else
				{
					result.code = ledger.store.block_exists (transaction, block_a.hashables.previous) ? ysu::process_result::fork : ysu::process_result::gap_previous; // If we have the block but it's not the latest we have a signed fork (Malicious)
				}
			}
		}
	}
}

void ledger_processor::open_block (ysu::open_block & block_a)
{
	auto hash (block_a.hash ());
	auto existing (ledger.store.block_exists (transaction, hash));
	result.code = existing ? ysu::process_result::old : ysu::process_result::progress; // Have we seen this block already? (Harmless)
	if (result.code == ysu::process_result::progress)
	{
		// Validate block if not verified outside of ledger
		if (result.verified != ysu::signature_verification::valid)
		{
			result.code = validate_message (block_a.hashables.account, hash, block_a.signature) ? ysu::process_result::bad_signature : ysu::process_result::progress; // Is the signature valid (Malformed)
		}
		if (result.code == ysu::process_result::progress)
		{
			debug_assert (!validate_message (block_a.hashables.account, hash, block_a.signature));
			result.verified = ysu::signature_verification::valid;
			result.code = ledger.store.block_exists (transaction, block_a.hashables.source) ? ysu::process_result::progress : ysu::process_result::gap_source; // Have we seen the source block? (Harmless)
			if (result.code == ysu::process_result::progress)
			{
				ysu::account_info info;
				result.code = ledger.store.account_get (transaction, block_a.hashables.account, info) ? ysu::process_result::progress : ysu::process_result::fork; // Has this account already been opened? (Malicious)
				if (result.code == ysu::process_result::progress)
				{
					ysu::pending_key key (block_a.hashables.account, block_a.hashables.source);
					ysu::pending_info pending;
					result.code = ledger.store.pending_get (transaction, key, pending) ? ysu::process_result::unreceivable : ysu::process_result::progress; // Has this source already been received (Malformed)
					if (result.code == ysu::process_result::progress)
					{
						result.code = block_a.hashables.account == ledger.network_params.ledger.burn_account ? ysu::process_result::opened_burn_account : ysu::process_result::progress; // Is it burning 0 account? (Malicious)
						if (result.code == ysu::process_result::progress)
						{
							result.code = pending.epoch == ysu::epoch::epoch_0 ? ysu::process_result::progress : ysu::process_result::unreceivable; // Are we receiving a state-only send? (Malformed)
							if (result.code == ysu::process_result::progress)
							{
								ysu::block_details block_details (ysu::epoch::epoch_0, false /* unused */, false /* unused */, false /* unused */);
								result.code = block_a.difficulty () >= ysu::work_threshold (block_a.work_version (), block_details) ? ysu::process_result::progress : ysu::process_result::insufficient_work; // Does this block have sufficient work? (Malformed)
								if (result.code == ysu::process_result::progress)
								{
									ysu::account_info source_info;
									auto error (ledger.store.account_get (transaction, pending.source, source_info));
									(void)error;
									debug_assert (!error);
									ledger.store.pending_del (transaction, key);
									block_a.sideband_set (ysu::block_sideband (block_a.hashables.account, 0, pending.amount, 1, ysu::seconds_since_epoch (), block_details, ysu::epoch::epoch_0 /* unused */));
									ledger.store.block_put (transaction, hash, block_a);
									ysu::account_info new_info (hash, block_a.representative (), hash, pending.amount.number (), ysu::seconds_since_epoch (), 1, ysu::epoch::epoch_0);
									ledger.update_account (transaction, block_a.hashables.account, info, new_info);
									ledger.cache.rep_weights.representation_add (block_a.representative (), pending.amount.number ());
									ledger.store.frontier_put (transaction, hash, block_a.hashables.account);
									result.previous_balance = 0;
									ledger.stats.inc (ysu::stat::type::ledger, ysu::stat::detail::open);
								}
							}
						}
					}
				}
			}
		}
	}
}

ledger_processor::ledger_processor (ysu::ledger & ledger_a, ysu::write_transaction const & transaction_a, ysu::signature_verification verification_a) :
ledger (ledger_a),
transaction (transaction_a),
verification (verification_a)
{
	result.verified = verification;
}
} // namespace

ysu::ledger::ledger (ysu::block_store & store_a, ysu::stat & stat_a, ysu::generate_cache const & generate_cache_a, std::function<void()> epoch_2_started_cb_a) :
store (store_a),
stats (stat_a),
check_bootstrap_weights (true),
epoch_2_started_cb (epoch_2_started_cb_a)
{
	if (!store.init_error ())
	{
		initialize (generate_cache_a);
	}
}

void ysu::ledger::initialize (ysu::generate_cache const & generate_cache_a)
{
	if (generate_cache_a.reps || generate_cache_a.account_count || generate_cache_a.epoch_2 || generate_cache_a.block_count)
	{
		store.accounts_for_each_par (
		[this](ysu::read_transaction const & /*unused*/, ysu::store_iterator<ysu::account, ysu::account_info> i, ysu::store_iterator<ysu::account, ysu::account_info> n) {
			uint64_t block_count_l{ 0 };
			uint64_t account_count_l{ 0 };
			decltype (this->cache.rep_weights) rep_weights_l;
			bool epoch_2_started_l{ false };
			for (; i != n; ++i)
			{
				ysu::account_info const & info (i->second);
				block_count_l += info.block_count;
				++account_count_l;
				rep_weights_l.representation_add (info.representative, info.balance.number ());
				epoch_2_started_l = epoch_2_started_l || info.epoch () == ysu::epoch::epoch_2;
			}
			if (epoch_2_started_l)
			{
				this->cache.epoch_2_started.store (true);
			}
			this->cache.block_count += block_count_l;
			this->cache.account_count += account_count_l;
			this->cache.rep_weights.copy_from (rep_weights_l);
		});
	}

	if (generate_cache_a.cemented_count)
	{
		store.confirmation_height_for_each_par (
		[this](ysu::read_transaction const & /*unused*/, ysu::store_iterator<ysu::account, ysu::confirmation_height_info> i, ysu::store_iterator<ysu::account, ysu::confirmation_height_info> n) {
			uint64_t cemented_count_l (0);
			for (; i != n; ++i)
			{
				cemented_count_l += i->second.height;
			}
			this->cache.cemented_count += cemented_count_l;
		});
	}

	auto transaction (store.tx_begin_read ());
	cache.pruned_count = store.pruned_count (transaction);
}

// Balance for account containing hash
ysu::uint128_t ysu::ledger::balance (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const
{
	return hash_a.is_zero () ? 0 : store.block_balance (transaction_a, hash_a);
}

ysu::uint128_t ysu::ledger::balance_safe (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a, bool & error_a) const
{
	ysu::uint128_t result (0);
	if (pruning && !hash_a.is_zero () && !store.block_exists (transaction_a, hash_a))
	{
		error_a = true;
		result = 0;
	}
	else
	{
		result = balance (transaction_a, hash_a);
	}
	return result;
}

// Balance for an account by account number
ysu::uint128_t ysu::ledger::account_balance (ysu::transaction const & transaction_a, ysu::account const & account_a)
{
	ysu::uint128_t result (0);
	ysu::account_info info;
	auto none (store.account_get (transaction_a, account_a, info));
	if (!none)
	{
		result = info.balance.number ();
	}
	return result;
}

ysu::uint128_t ysu::ledger::account_pending (ysu::transaction const & transaction_a, ysu::account const & account_a)
{
	ysu::uint128_t result (0);
	ysu::account end (account_a.number () + 1);
	for (auto i (store.pending_begin (transaction_a, ysu::pending_key (account_a, 0))), n (store.pending_begin (transaction_a, ysu::pending_key (end, 0))); i != n; ++i)
	{
		ysu::pending_info const & info (i->second);
		result += info.amount.number ();
	}
	return result;
}

ysu::process_return ysu::ledger::process (ysu::write_transaction const & transaction_a, ysu::block & block_a, ysu::signature_verification verification)
{
	debug_assert (!ysu::work_validate_entry (block_a) || network_params.network.is_dev_network ());
	ledger_processor processor (*this, transaction_a, verification);
	block_a.visit (processor);
	if (processor.result.code == ysu::process_result::progress)
	{
		++cache.block_count;
	}
	return processor.result;
}

ysu::block_hash ysu::ledger::representative (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a)
{
	auto result (representative_calculated (transaction_a, hash_a));
	debug_assert (result.is_zero () || store.block_exists (transaction_a, result));
	return result;
}

ysu::block_hash ysu::ledger::representative_calculated (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a)
{
	representative_visitor visitor (transaction_a, store);
	visitor.compute (hash_a);
	return visitor.result;
}

bool ysu::ledger::block_exists (ysu::block_hash const & hash_a) const
{
	return store.block_exists (store.tx_begin_read (), hash_a);
}

bool ysu::ledger::block_or_pruned_exists (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const
{
	return pruning ? store.block_or_pruned_exists (transaction_a, hash_a) : store.block_exists (transaction_a, hash_a);
}

bool ysu::ledger::block_or_pruned_exists (ysu::block_hash const & hash_a) const
{
	return block_or_pruned_exists (store.tx_begin_read (), hash_a);
}

std::string ysu::ledger::block_text (char const * hash_a)
{
	return block_text (ysu::block_hash (hash_a));
}

std::string ysu::ledger::block_text (ysu::block_hash const & hash_a)
{
	std::string result;
	auto transaction (store.tx_begin_read ());
	auto block (store.block_get (transaction, hash_a));
	if (block != nullptr)
	{
		block->serialize_json (result);
	}
	return result;
}

bool ysu::ledger::is_send (ysu::transaction const & transaction_a, ysu::state_block const & block_a) const
{
	/*
	 * if block_a does not have a sideband, then is_send()
	 * requires that the previous block exists in the database.
	 * This is because it must retrieve the balance of the previous block.
	 */
	debug_assert (block_a.has_sideband () || block_a.hashables.previous.is_zero () || store.block_exists (transaction_a, block_a.hashables.previous));

	bool result (false);
	if (block_a.has_sideband ())
	{
		result = block_a.sideband ().details.is_send;
	}
	else
	{
		ysu::block_hash previous (block_a.hashables.previous);
		if (!previous.is_zero ())
		{
			if (block_a.hashables.balance < balance (transaction_a, previous))
			{
				result = true;
			}
		}
	}
	return result;
}

ysu::account const & ysu::ledger::block_destination (ysu::transaction const & transaction_a, ysu::block const & block_a)
{
	ysu::send_block const * send_block (dynamic_cast<ysu::send_block const *> (&block_a));
	ysu::state_block const * state_block (dynamic_cast<ysu::state_block const *> (&block_a));
	if (send_block != nullptr)
	{
		return send_block->hashables.destination;
	}
	else if (state_block != nullptr && is_send (transaction_a, *state_block))
	{
		return state_block->hashables.link.as_account ();
	}
	static ysu::account result (0);
	return result;
}

ysu::block_hash ysu::ledger::block_source (ysu::transaction const & transaction_a, ysu::block const & block_a)
{
	/*
	 * block_source() requires that the previous block of the block
	 * passed in exist in the database.  This is because it will try
	 * to check account balances to determine if it is a send block.
	 */
	debug_assert (block_a.previous ().is_zero () || store.block_exists (transaction_a, block_a.previous ()));

	// If block_a.source () is nonzero, then we have our source.
	// However, universal blocks will always return zero.
	ysu::block_hash result (block_a.source ());
	ysu::state_block const * state_block (dynamic_cast<ysu::state_block const *> (&block_a));
	if (state_block != nullptr && !is_send (transaction_a, *state_block))
	{
		result = state_block->hashables.link.as_block_hash ();
	}
	return result;
}

std::pair<ysu::block_hash, ysu::block_hash> ysu::ledger::hash_root_random (ysu::transaction const & transaction_a) const
{
	ysu::block_hash hash (0);
	ysu::root root (0);
	if (!pruning)
	{
		auto block (store.block_random (transaction_a));
		hash = block->hash ();
		root = block->root ();
	}
	else
	{
		uint64_t count (cache.block_count);
		release_assert (std::numeric_limits<CryptoPP::word32>::max () > count);
		auto region = static_cast<size_t> (ysu::random_pool::generate_word32 (0, static_cast<CryptoPP::word32> (count - 1)));
		// Pruned cache cannot guarantee that pruned blocks are already commited
		if (region < cache.pruned_count)
		{
			hash = store.pruned_random (transaction_a);
		}
		if (hash.is_zero ())
		{
			auto block (store.block_random (transaction_a));
			hash = block->hash ();
			root = block->root ();
		}
	}
	return std::make_pair (hash, root.as_block_hash ());
}

// Vote weight of an account
ysu::uint128_t ysu::ledger::weight (ysu::account const & account_a)
{
	if (check_bootstrap_weights.load ())
	{
		if (cache.block_count < bootstrap_weight_max_blocks)
		{
			auto weight = bootstrap_weights.find (account_a);
			if (weight != bootstrap_weights.end ())
			{
				return weight->second;
			}
		}
		else
		{
			check_bootstrap_weights = false;
		}
	}
	return cache.rep_weights.representation_get (account_a);
}

// Rollback blocks until `block_a' doesn't exist or it tries to penetrate the confirmation height
bool ysu::ledger::rollback (ysu::write_transaction const & transaction_a, ysu::block_hash const & block_a, std::vector<std::shared_ptr<ysu::block>> & list_a)
{
	debug_assert (store.block_exists (transaction_a, block_a));
	auto account_l (account (transaction_a, block_a));
	auto block_account_height (store.block_account_height (transaction_a, block_a));
	rollback_visitor rollback (transaction_a, *this, list_a);
	ysu::account_info account_info;
	auto error (false);
	while (!error && store.block_exists (transaction_a, block_a))
	{
		ysu::confirmation_height_info confirmation_height_info;
		auto latest_error = store.confirmation_height_get (transaction_a, account_l, confirmation_height_info);
		debug_assert (!latest_error);
		(void)latest_error;
		if (block_account_height > confirmation_height_info.height)
		{
			latest_error = store.account_get (transaction_a, account_l, account_info);
			debug_assert (!latest_error);
			auto block (store.block_get (transaction_a, account_info.head));
			list_a.push_back (block);
			block->visit (rollback);
			error = rollback.error;
			if (!error)
			{
				--cache.block_count;
			}
		}
		else
		{
			error = true;
		}
	}
	return error;
}

bool ysu::ledger::rollback (ysu::write_transaction const & transaction_a, ysu::block_hash const & block_a)
{
	std::vector<std::shared_ptr<ysu::block>> rollback_list;
	return rollback (transaction_a, block_a, rollback_list);
}

// Return account containing hash
ysu::account ysu::ledger::account (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const
{
	return store.block_account (transaction_a, hash_a);
}

ysu::account ysu::ledger::account_safe (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a, bool & error_a) const
{
	if (!pruning)
	{
		return store.block_account (transaction_a, hash_a);
	}
	else
	{
		auto block (store.block_get (transaction_a, hash_a));
		if (block != nullptr)
		{
			return store.block_account_calculated (*block);
		}
		else
		{
			error_a = true;
			return 0;
		}
	}
}

// Return amount decrease or increase for block
ysu::uint128_t ysu::ledger::amount (ysu::transaction const & transaction_a, ysu::account const & account_a)
{
	release_assert (account_a == network_params.ledger.genesis_account);
	return network_params.ledger.genesis_amount;
}

ysu::uint128_t ysu::ledger::amount (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a)
{
	auto block (store.block_get (transaction_a, hash_a));
	auto block_balance (balance (transaction_a, hash_a));
	auto previous_balance (balance (transaction_a, block->previous ()));
	return block_balance > previous_balance ? block_balance - previous_balance : previous_balance - block_balance;
}

ysu::uint128_t ysu::ledger::amount_safe (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a, bool & error_a) const
{
	auto block (store.block_get (transaction_a, hash_a));
	debug_assert (block);
	auto block_balance (balance (transaction_a, hash_a));
	auto previous_balance (balance_safe (transaction_a, block->previous (), error_a));
	return error_a ? 0 : block_balance > previous_balance ? block_balance - previous_balance : previous_balance - block_balance;
}

// Return latest block for account
ysu::block_hash ysu::ledger::latest (ysu::transaction const & transaction_a, ysu::account const & account_a)
{
	ysu::account_info info;
	auto latest_error (store.account_get (transaction_a, account_a, info));
	return latest_error ? 0 : info.head;
}

// Return latest root for account, account number if there are no blocks for this account.
ysu::root ysu::ledger::latest_root (ysu::transaction const & transaction_a, ysu::account const & account_a)
{
	ysu::account_info info;
	if (store.account_get (transaction_a, account_a, info))
	{
		return account_a;
	}
	else
	{
		return info.head;
	}
}

void ysu::ledger::dump_account_chain (ysu::account const & account_a, std::ostream & stream)
{
	auto transaction (store.tx_begin_read ());
	auto hash (latest (transaction, account_a));
	while (!hash.is_zero ())
	{
		auto block (store.block_get (transaction, hash));
		debug_assert (block != nullptr);
		stream << hash.to_string () << std::endl;
		hash = block->previous ();
	}
}

bool ysu::ledger::could_fit (ysu::transaction const & transaction_a, ysu::block const & block_a) const
{
	auto dependencies (dependent_blocks (transaction_a, block_a));
	return std::all_of (dependencies.begin (), dependencies.end (), [this, &transaction_a](ysu::block_hash const & hash_a) {
		return hash_a.is_zero () || store.block_exists (transaction_a, hash_a);
	});
}

bool ysu::ledger::dependents_confirmed (ysu::transaction const & transaction_a, ysu::block const & block_a) const
{
	auto dependencies (dependent_blocks (transaction_a, block_a));
	return std::all_of (dependencies.begin (), dependencies.end (), [this, &transaction_a](ysu::block_hash const & hash_a) {
		auto result (hash_a.is_zero ());
		if (!result)
		{
			result = block_confirmed (transaction_a, hash_a);
		}
		return result;
	});
}

bool ysu::ledger::is_epoch_link (ysu::link const & link_a) const
{
	return network_params.ledger.epochs.is_epoch_link (link_a);
}

class dependent_block_visitor : public ysu::block_visitor
{
public:
	dependent_block_visitor (ysu::ledger const & ledger_a, ysu::transaction const & transaction_a) :
	ledger (ledger_a),
	transaction (transaction_a),
	result ({ 0, 0 })
	{
	}
	void send_block (ysu::send_block const & block_a) override
	{
		result[0] = block_a.previous ();
	}
	void receive_block (ysu::receive_block const & block_a) override
	{
		result[0] = block_a.previous ();
		result[1] = block_a.source ();
	}
	void open_block (ysu::open_block const & block_a) override
	{
		if (block_a.source () != ledger.network_params.ledger.genesis_account)
		{
			result[0] = block_a.source ();
		}
	}
	void change_block (ysu::change_block const & block_a) override
	{
		result[0] = block_a.previous ();
	}
	void state_block (ysu::state_block const & block_a) override
	{
		result[0] = block_a.hashables.previous;
		result[1] = block_a.hashables.link.as_block_hash ();
		// ledger.is_send will check the sideband first, if block_a has a loaded sideband the check that previous block exists can be skipped
		if (ledger.is_epoch_link (block_a.hashables.link) || ((block_a.has_sideband () || ledger.store.block_exists (transaction, block_a.hashables.previous)) && ledger.is_send (transaction, block_a)))
		{
			result[1].clear ();
		}
	}
	ysu::ledger const & ledger;
	ysu::transaction const & transaction;
	std::array<ysu::block_hash, 2> result;
};

std::array<ysu::block_hash, 2> ysu::ledger::dependent_blocks (ysu::transaction const & transaction_a, ysu::block const & block_a) const
{
	dependent_block_visitor visitor (*this, transaction_a);
	block_a.visit (visitor);
	return visitor.result;
}

ysu::account const & ysu::ledger::epoch_signer (ysu::link const & link_a) const
{
	return network_params.ledger.epochs.signer (network_params.ledger.epochs.epoch (link_a));
}

ysu::link const & ysu::ledger::epoch_link (ysu::epoch epoch_a) const
{
	return network_params.ledger.epochs.link (epoch_a);
}

void ysu::ledger::update_account (ysu::write_transaction const & transaction_a, ysu::account const & account_a, ysu::account_info const & old_a, ysu::account_info const & new_a)
{
	if (!new_a.head.is_zero ())
	{
		if (old_a.head.is_zero () && new_a.open_block == new_a.head)
		{
			debug_assert (!store.confirmation_height_exists (transaction_a, account_a));
			store.confirmation_height_put (transaction_a, account_a, { 0, ysu::block_hash (0) });
			++cache.account_count;
		}
		if (!old_a.head.is_zero () && old_a.epoch () != new_a.epoch ())
		{
			// store.account_put won't erase existing entries if they're in different tables
			store.account_del (transaction_a, account_a);
		}
		store.account_put (transaction_a, account_a, new_a);
	}
	else
	{
		store.confirmation_height_del (transaction_a, account_a);
		store.account_del (transaction_a, account_a);
		debug_assert (cache.account_count > 0);
		--cache.account_count;
	}
}

std::shared_ptr<ysu::block> ysu::ledger::successor (ysu::transaction const & transaction_a, ysu::qualified_root const & root_a)
{
	ysu::block_hash successor (0);
	auto get_from_previous = false;
	if (root_a.previous ().is_zero ())
	{
		ysu::account_info info;
		if (!store.account_get (transaction_a, root_a.root ().as_account (), info))
		{
			successor = info.open_block;
		}
		else
		{
			get_from_previous = true;
		}
	}
	else
	{
		get_from_previous = true;
	}

	if (get_from_previous)
	{
		successor = store.block_successor (transaction_a, root_a.previous ());
	}
	std::shared_ptr<ysu::block> result;
	if (!successor.is_zero ())
	{
		result = store.block_get (transaction_a, successor);
	}
	debug_assert (successor.is_zero () || result != nullptr);
	return result;
}

std::shared_ptr<ysu::block> ysu::ledger::forked_block (ysu::transaction const & transaction_a, ysu::block const & block_a)
{
	debug_assert (!store.block_exists (transaction_a, block_a.hash ()));
	auto root (block_a.root ());
	debug_assert (store.block_exists (transaction_a, root.as_block_hash ()) || store.account_exists (transaction_a, root.as_account ()));
	auto result (store.block_get (transaction_a, store.block_successor (transaction_a, root.as_block_hash ())));
	if (result == nullptr)
	{
		ysu::account_info info;
		auto error (store.account_get (transaction_a, root.as_account (), info));
		(void)error;
		debug_assert (!error);
		result = store.block_get (transaction_a, info.open_block);
		debug_assert (result != nullptr);
	}
	return result;
}

bool ysu::ledger::block_confirmed (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const
{
	auto confirmed (false);
	auto block = store.block_get (transaction_a, hash_a);
	if (block)
	{
		ysu::confirmation_height_info confirmation_height_info;
		release_assert (!store.confirmation_height_get (transaction_a, block->account ().is_zero () ? block->sideband ().account : block->account (), confirmation_height_info));
		confirmed = (confirmation_height_info.height >= block->sideband ().height);
	}
	return confirmed;
}

uint64_t ysu::ledger::pruning_action (ysu::write_transaction & transaction_a, ysu::block_hash const & hash_a, uint64_t const batch_size_a)
{
	uint64_t pruned_count (0);
	ysu::block_hash hash (hash_a);
	while (!hash.is_zero () && hash != network_params.ledger.genesis_hash)
	{
		auto block (store.block_get (transaction_a, hash));
		if (block != nullptr)
		{
			store.block_del (transaction_a, hash);
			store.pruned_put (transaction_a, hash);
			hash = block->previous ();
			++pruned_count;
			++cache.pruned_count;
			if (pruned_count % batch_size_a == 0)
			{
				transaction_a.commit ();
				transaction_a.renew ();
			}
		}
		else if (store.pruned_exists (transaction_a, hash))
		{
			hash = 0;
		}
		else
		{
			hash = 0;
			release_assert (false && "Error finding block for pruning");
		}
	}
	return pruned_count;
}

std::multimap<uint64_t, ysu::uncemented_info, std::greater<>> ysu::ledger::unconfirmed_frontiers () const
{
	ysu::locked<std::multimap<uint64_t, ysu::uncemented_info, std::greater<>>> result;
	using result_t = decltype (result)::value_type;

	store.accounts_for_each_par ([this, &result](ysu::read_transaction const & transaction_a, ysu::store_iterator<ysu::account, ysu::account_info> i, ysu::store_iterator<ysu::account, ysu::account_info> n) {
		result_t unconfirmed_frontiers_l;
		for (; i != n; ++i)
		{
			auto const & account (i->first);
			auto const & account_info (i->second);

			ysu::confirmation_height_info conf_height_info;
			this->store.confirmation_height_get (transaction_a, account, conf_height_info);

			if (account_info.block_count != conf_height_info.height)
			{
				// Always output as no confirmation height has been set on the account yet
				auto height_delta = account_info.block_count - conf_height_info.height;
				auto const & frontier = account_info.head;
				auto const & cemented_frontier = conf_height_info.frontier;
				unconfirmed_frontiers_l.emplace (std::piecewise_construct, std::forward_as_tuple (height_delta), std::forward_as_tuple (cemented_frontier, frontier, i->first));
			}
		}
		// Merge results
		auto result_locked = result.lock ();
		result_locked->insert (unconfirmed_frontiers_l.begin (), unconfirmed_frontiers_l.end ());
	});
	return result;
}

ysu::uncemented_info::uncemented_info (ysu::block_hash const & cemented_frontier, ysu::block_hash const & frontier, ysu::account const & account) :
cemented_frontier (cemented_frontier), frontier (frontier), account (account)
{
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (ledger & ledger, const std::string & name)
{
	auto count = ledger.bootstrap_weights_size.load ();
	auto sizeof_element = sizeof (decltype (ledger.bootstrap_weights)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "bootstrap_weights", count, sizeof_element }));
	composite->add_component (collect_container_info (ledger.cache.rep_weights, "rep_weights"));
	return composite;
}
