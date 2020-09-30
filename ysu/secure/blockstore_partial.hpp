#pragma once

#include <ysu/lib/config.hpp>
#include <ysu/lib/rep_weights.hpp>
#include <ysu/lib/threading.hpp>
#include <ysu/secure/blockstore.hpp>
#include <ysu/secure/buffer.hpp>

#include <crypto/cryptopp/words.h>

#include <thread>

namespace
{
template <typename T>
void parallel_traversal (std::function<void(T const &, T const &, bool const)> const & action);
}

namespace ysu
{
template <typename Val, typename Derived_Store>
class block_predecessor_set;

/** This base class implements the block_store interface functions which have DB agnostic functionality */
template <typename Val, typename Derived_Store>
class block_store_partial : public block_store
{
public:
	using block_store::block_exists;
	using block_store::unchecked_put;

	friend class ysu::block_predecessor_set<Val, Derived_Store>;

	std::mutex cache_mutex;

	/**
	 * If using a different store version than the latest then you may need
	 * to modify some of the objects in the store to be appropriate for the version before an upgrade.
	 */
	void initialize (ysu::write_transaction const & transaction_a, ysu::genesis const & genesis_a, ysu::ledger_cache & ledger_cache_a) override
	{
		auto hash_l (genesis_a.hash ());
		debug_assert (accounts_begin (transaction_a) == accounts_end ());
		genesis_a.open->sideband_set (ysu::block_sideband (network_params.ledger.genesis_account, 0, network_params.ledger.genesis_amount, 1, ysu::seconds_since_epoch (), ysu::epoch::epoch_0, false, false, false, ysu::epoch::epoch_0));
		block_put (transaction_a, hash_l, *genesis_a.open);
		++ledger_cache_a.block_count;
		confirmation_height_put (transaction_a, network_params.ledger.genesis_account, ysu::confirmation_height_info{ 1, genesis_a.hash () });
		++ledger_cache_a.cemented_count;
		account_put (transaction_a, network_params.ledger.genesis_account, { hash_l, network_params.ledger.genesis_account, genesis_a.open->hash (), std::numeric_limits<ysu::uint128_t>::max (), ysu::seconds_since_epoch (), 1, ysu::epoch::epoch_0 });
		++ledger_cache_a.account_count;
		ledger_cache_a.rep_weights.representation_put (network_params.ledger.genesis_account, std::numeric_limits<ysu::uint128_t>::max ());
		frontier_put (transaction_a, hash_l, network_params.ledger.genesis_account);
	}

	ysu::uint128_t block_balance (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) override
	{
		auto block (block_get (transaction_a, hash_a));
		release_assert (block);
		ysu::uint128_t result (block_balance_calculated (block));
		return result;
	}

	bool account_exists (ysu::transaction const & transaction_a, ysu::account const & account_a) override
	{
		auto iterator (accounts_begin (transaction_a, account_a));
		return iterator != accounts_end () && ysu::account (iterator->first) == account_a;
	}

	void confirmation_height_clear (ysu::write_transaction const & transaction_a, ysu::account const & account_a, uint64_t existing_confirmation_height_a) override
	{
		if (existing_confirmation_height_a > 0)
		{
			confirmation_height_put (transaction_a, account_a, { 0, ysu::block_hash{ 0 } });
		}
	}

	void confirmation_height_clear (ysu::write_transaction const & transaction_a) override
	{
		for (auto i (confirmation_height_begin (transaction_a)), n (confirmation_height_end ()); i != n; ++i)
		{
			confirmation_height_clear (transaction_a, i->first, i->second.height);
		}
	}

	bool pending_exists (ysu::transaction const & transaction_a, ysu::pending_key const & key_a) override
	{
		auto iterator (pending_begin (transaction_a, key_a));
		return iterator != pending_end () && ysu::pending_key (iterator->first) == key_a;
	}

	bool pending_any (ysu::transaction const & transaction_a, ysu::account const & account_a) override
	{
		auto iterator (pending_begin (transaction_a, ysu::pending_key (account_a, 0)));
		return iterator != pending_end () && ysu::pending_key (iterator->first).account == account_a;
	}

	bool unchecked_exists (ysu::transaction const & transaction_a, ysu::unchecked_key const & unchecked_key_a) override
	{
		ysu::db_val<Val> value;
		auto status (get (transaction_a, tables::unchecked, ysu::db_val<Val> (unchecked_key_a), value));
		release_assert (success (status) || not_found (status));
		return (success (status));
	}

	void block_put (ysu::write_transaction const & transaction_a, ysu::block_hash const & hash_a, ysu::block const & block_a) override
	{
		debug_assert (block_a.sideband ().successor.is_zero () || block_exists (transaction_a, block_a.sideband ().successor));
		std::vector<uint8_t> vector;
		{
			ysu::vectorstream stream (vector);
			ysu::serialize_block (stream, block_a);
			block_a.sideband ().serialize (stream, block_a.type ());
		}
		block_raw_put (transaction_a, vector, hash_a);
		ysu::block_predecessor_set<Val, Derived_Store> predecessor (transaction_a, *this);
		block_a.visit (predecessor);
		debug_assert (block_a.previous ().is_zero () || block_successor (transaction_a, block_a.previous ()) == hash_a);
	}

	// Converts a block hash to a block height
	uint64_t block_account_height (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const override
	{
		auto block = block_get (transaction_a, hash_a);
		return block->sideband ().height;
	}

	std::shared_ptr<ysu::block> block_get (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const override
	{
		auto value (block_raw_get (transaction_a, hash_a));
		std::shared_ptr<ysu::block> result;
		if (value.size () != 0)
		{
			ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
			ysu::block_type type;
			auto error (try_read (stream, type));
			release_assert (!error);
			result = ysu::deserialize_block (stream, type);
			release_assert (result != nullptr);
			ysu::block_sideband sideband;
			error = (sideband.deserialize (stream, type));
			release_assert (!error);
			result->sideband_set (sideband);
		}
		return result;
	}

	bool block_exists (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) override
	{
		auto junk = block_raw_get (transaction_a, hash_a);
		return junk.size () != 0;
	}

	std::shared_ptr<ysu::block> block_get_no_sideband (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const override
	{
		auto value (block_raw_get (transaction_a, hash_a));
		std::shared_ptr<ysu::block> result;
		if (value.size () != 0)
		{
			ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
			result = ysu::deserialize_block (stream);
			debug_assert (result != nullptr);
		}
		return result;
	}

	bool root_exists (ysu::transaction const & transaction_a, ysu::root const & root_a) override
	{
		return block_exists (transaction_a, root_a.as_block_hash ()) || account_exists (transaction_a, root_a.as_account ());
	}

	ysu::account block_account (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const override
	{
		auto block (block_get (transaction_a, hash_a));
		debug_assert (block != nullptr);
		return block_account_calculated (*block);
	}

	ysu::account block_account_calculated (ysu::block const & block_a) const override
	{
		debug_assert (block_a.has_sideband ());
		ysu::account result (block_a.account ());
		if (result.is_zero ())
		{
			result = block_a.sideband ().account;
		}
		debug_assert (!result.is_zero ());
		return result;
	}

	ysu::uint128_t block_balance_calculated (std::shared_ptr<ysu::block> const & block_a) const override
	{
		ysu::uint128_t result;
		switch (block_a->type ())
		{
			case ysu::block_type::open:
			case ysu::block_type::receive:
			case ysu::block_type::change:
				result = block_a->sideband ().balance.number ();
				break;
			case ysu::block_type::send:
				result = boost::polymorphic_downcast<ysu::send_block *> (block_a.get ())->hashables.balance.number ();
				break;
			case ysu::block_type::state:
				result = boost::polymorphic_downcast<ysu::state_block *> (block_a.get ())->hashables.balance.number ();
				break;
			case ysu::block_type::invalid:
			case ysu::block_type::not_a_block:
				release_assert (false);
				break;
		}
		return result;
	}

	ysu::block_hash block_successor (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const override
	{
		auto value (block_raw_get (transaction_a, hash_a));
		ysu::block_hash result;
		if (value.size () != 0)
		{
			debug_assert (value.size () >= result.bytes.size ());
			auto type = block_type_from_raw (value.data ());
			ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()) + block_successor_offset (transaction_a, value.size (), type), result.bytes.size ());
			auto error (ysu::try_read (stream, result.bytes));
			(void)error;
			debug_assert (!error);
		}
		else
		{
			result.clear ();
		}
		return result;
	}

	void block_successor_clear (ysu::write_transaction const & transaction_a, ysu::block_hash const & hash_a) override
	{
		auto value (block_raw_get (transaction_a, hash_a));
		debug_assert (value.size () != 0);
		auto type = block_type_from_raw (value.data ());
		std::vector<uint8_t> data (static_cast<uint8_t *> (value.data ()), static_cast<uint8_t *> (value.data ()) + value.size ());
		std::fill_n (data.begin () + block_successor_offset (transaction_a, value.size (), type), sizeof (ysu::block_hash), uint8_t{ 0 });
		block_raw_put (transaction_a, data, hash_a);
	}

	void unchecked_put (ysu::write_transaction const & transaction_a, ysu::block_hash const & hash_a, std::shared_ptr<ysu::block> const & block_a) override
	{
		ysu::unchecked_key key (hash_a, block_a->hash ());
		ysu::unchecked_info info (block_a, block_a->account (), ysu::seconds_since_epoch (), ysu::signature_verification::unknown);
		unchecked_put (transaction_a, key, info);
	}

	std::shared_ptr<ysu::vote> vote_current (ysu::transaction const & transaction_a, ysu::account const & account_a) override
	{
		debug_assert (!cache_mutex.try_lock ());
		std::shared_ptr<ysu::vote> result;
		auto existing (vote_cache_l1.find (account_a));
		auto have_existing (true);
		if (existing == vote_cache_l1.end ())
		{
			existing = vote_cache_l2.find (account_a);
			if (existing == vote_cache_l2.end ())
			{
				have_existing = false;
			}
		}
		if (have_existing)
		{
			result = existing->second;
		}
		else
		{
			result = vote_get (transaction_a, account_a);
		}
		return result;
	}

	std::shared_ptr<ysu::vote> vote_generate (ysu::transaction const & transaction_a, ysu::account const & account_a, ysu::raw_key const & key_a, std::shared_ptr<ysu::block> block_a) override
	{
		debug_assert (ysu::network_constants ().is_dev_network () || ysu::thread_role::get () == ysu::thread_role::name::voting);
		ysu::lock_guard<std::mutex> lock (cache_mutex);
		auto result (vote_current (transaction_a, account_a));
		uint64_t sequence ((result ? result->sequence : 0) + 1);
		result = std::make_shared<ysu::vote> (account_a, key_a, sequence, block_a);
		vote_cache_l1[account_a] = result;
		return result;
	}

	std::shared_ptr<ysu::vote> vote_generate (ysu::transaction const & transaction_a, ysu::account const & account_a, ysu::raw_key const & key_a, std::vector<ysu::block_hash> blocks_a) override
	{
		debug_assert (ysu::network_constants ().is_dev_network () || ysu::thread_role::get () == ysu::thread_role::name::voting);
		ysu::lock_guard<std::mutex> lock (cache_mutex);
		auto result (vote_current (transaction_a, account_a));
		uint64_t sequence ((result ? result->sequence : 0) + 1);
		result = std::make_shared<ysu::vote> (account_a, key_a, sequence, blocks_a);
		vote_cache_l1[account_a] = result;
		return result;
	}

	std::shared_ptr<ysu::vote> vote_max (ysu::transaction const & transaction_a, std::shared_ptr<ysu::vote> vote_a) override
	{
		ysu::lock_guard<std::mutex> lock (cache_mutex);
		auto current (vote_current (transaction_a, vote_a->account));
		auto result (vote_a);
		if (current != nullptr && current->sequence > result->sequence)
		{
			result = current;
		}
		vote_cache_l1[vote_a->account] = result;
		return result;
	}

	ysu::store_iterator<ysu::unchecked_key, ysu::unchecked_info> unchecked_end () const override
	{
		return ysu::store_iterator<ysu::unchecked_key, ysu::unchecked_info> (nullptr);
	}

	ysu::store_iterator<ysu::account, std::shared_ptr<ysu::vote>> vote_end () override
	{
		return ysu::store_iterator<ysu::account, std::shared_ptr<ysu::vote>> (nullptr);
	}

	ysu::store_iterator<ysu::endpoint_key, ysu::no_value> peers_end () const override
	{
		return ysu::store_iterator<ysu::endpoint_key, ysu::no_value> (nullptr);
	}

	ysu::store_iterator<ysu::pending_key, ysu::pending_info> pending_end () override
	{
		return ysu::store_iterator<ysu::pending_key, ysu::pending_info> (nullptr);
	}

	ysu::store_iterator<uint64_t, ysu::amount> online_weight_end () const override
	{
		return ysu::store_iterator<uint64_t, ysu::amount> (nullptr);
	}

	ysu::store_iterator<ysu::account, ysu::account_info> accounts_end () const override
	{
		return ysu::store_iterator<ysu::account, ysu::account_info> (nullptr);
	}

	ysu::store_iterator<ysu::block_hash, std::shared_ptr<ysu::block>> blocks_end () const override
	{
		return ysu::store_iterator<ysu::block_hash, std::shared_ptr<ysu::block>> (nullptr);
	}

	ysu::store_iterator<ysu::account, ysu::confirmation_height_info> confirmation_height_end () const override
	{
		return ysu::store_iterator<ysu::account, ysu::confirmation_height_info> (nullptr);
	}

	std::mutex & get_cache_mutex () override
	{
		return cache_mutex;
	}

	void block_del (ysu::write_transaction const & transaction_a, ysu::block_hash const & hash_a) override
	{
		auto status = del (transaction_a, tables::blocks, hash_a);
		release_assert (success (status));
	}

	int version_get (ysu::transaction const & transaction_a) const override
	{
		ysu::uint256_union version_key (1);
		ysu::db_val<Val> data;
		auto status = get (transaction_a, tables::meta, ysu::db_val<Val> (version_key), data);
		int result (minimum_version);
		if (success (status))
		{
			ysu::uint256_union version_value (data);
			debug_assert (version_value.qwords[2] == 0 && version_value.qwords[1] == 0 && version_value.qwords[0] == 0);
			result = version_value.number ().convert_to<int> ();
		}
		return result;
	}

	ysu::epoch block_version (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) override
	{
		auto block = block_get (transaction_a, hash_a);
		if (block && block->type () == ysu::block_type::state)
		{
			return block->sideband ().details.epoch;
		}

		return ysu::epoch::epoch_0;
	}

	void block_raw_put (ysu::write_transaction const & transaction_a, std::vector<uint8_t> const & data, ysu::block_hash const & hash_a)
	{
		ysu::db_val<Val> value{ data.size (), (void *)data.data () };
		auto status = put (transaction_a, tables::blocks, hash_a, value);
		release_assert (success (status));
	}

	void pending_put (ysu::write_transaction const & transaction_a, ysu::pending_key const & key_a, ysu::pending_info const & pending_info_a) override
	{
		ysu::db_val<Val> pending (pending_info_a);
		auto status = put (transaction_a, tables::pending, key_a, pending);
		release_assert (success (status));
	}

	void pending_del (ysu::write_transaction const & transaction_a, ysu::pending_key const & key_a) override
	{
		auto status = del (transaction_a, tables::pending, key_a);
		release_assert (success (status));
	}

	bool pending_get (ysu::transaction const & transaction_a, ysu::pending_key const & key_a, ysu::pending_info & pending_a) override
	{
		ysu::db_val<Val> value;
		ysu::db_val<Val> key (key_a);
		auto status1 = get (transaction_a, tables::pending, key, value);
		release_assert (success (status1) || not_found (status1));
		bool result (true);
		if (success (status1))
		{
			ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
			result = pending_a.deserialize (stream);
		}
		return result;
	}

	void frontier_put (ysu::write_transaction const & transaction_a, ysu::block_hash const & block_a, ysu::account const & account_a) override
	{
		ysu::db_val<Val> account (account_a);
		auto status (put (transaction_a, tables::frontiers, block_a, account));
		release_assert (success (status));
	}

	ysu::account frontier_get (ysu::transaction const & transaction_a, ysu::block_hash const & block_a) const override
	{
		ysu::db_val<Val> value;
		auto status (get (transaction_a, tables::frontiers, ysu::db_val<Val> (block_a), value));
		release_assert (success (status) || not_found (status));
		ysu::account result (0);
		if (success (status))
		{
			result = static_cast<ysu::account> (value);
		}
		return result;
	}

	void frontier_del (ysu::write_transaction const & transaction_a, ysu::block_hash const & block_a) override
	{
		auto status (del (transaction_a, tables::frontiers, block_a));
		release_assert (success (status));
	}

	void unchecked_put (ysu::write_transaction const & transaction_a, ysu::unchecked_key const & key_a, ysu::unchecked_info const & info_a) override
	{
		ysu::db_val<Val> info (info_a);
		auto status (put (transaction_a, tables::unchecked, key_a, info));
		release_assert (success (status));
	}

	void unchecked_del (ysu::write_transaction const & transaction_a, ysu::unchecked_key const & key_a) override
	{
		auto status (del (transaction_a, tables::unchecked, key_a));
		release_assert (success (status));
	}

	std::shared_ptr<ysu::vote> vote_get (ysu::transaction const & transaction_a, ysu::account const & account_a) override
	{
		ysu::db_val<Val> value;
		auto status (get (transaction_a, tables::vote, ysu::db_val<Val> (account_a), value));
		release_assert (success (status) || not_found (status));
		if (success (status))
		{
			std::shared_ptr<ysu::vote> result (value);
			debug_assert (result != nullptr);
			return result;
		}
		return nullptr;
	}

	void flush (ysu::write_transaction const & transaction_a) override
	{
		{
			ysu::lock_guard<std::mutex> lock (cache_mutex);
			vote_cache_l1.swap (vote_cache_l2);
			vote_cache_l1.clear ();
		}
		for (auto i (vote_cache_l2.begin ()), n (vote_cache_l2.end ()); i != n; ++i)
		{
			std::vector<uint8_t> vector;
			{
				ysu::vectorstream stream (vector);
				i->second->serialize (stream);
			}
			ysu::db_val<Val> value (vector.size (), vector.data ());
			auto status1 (put (transaction_a, tables::vote, i->first, value));
			release_assert (success (status1));
		}
	}

	void online_weight_put (ysu::write_transaction const & transaction_a, uint64_t time_a, ysu::amount const & amount_a) override
	{
		ysu::db_val<Val> value (amount_a);
		auto status (put (transaction_a, tables::online_weight, time_a, value));
		release_assert (success (status));
	}

	void online_weight_del (ysu::write_transaction const & transaction_a, uint64_t time_a) override
	{
		auto status (del (transaction_a, tables::online_weight, time_a));
		release_assert (success (status));
	}

	void account_put (ysu::write_transaction const & transaction_a, ysu::account const & account_a, ysu::account_info const & info_a) override
	{
		// Check we are still in sync with other tables
		debug_assert (confirmation_height_exists (transaction_a, account_a));
		ysu::db_val<Val> info (info_a);
		auto status = put (transaction_a, tables::accounts, account_a, info);
		release_assert (success (status));
	}

	void account_del (ysu::write_transaction const & transaction_a, ysu::account const & account_a) override
	{
		auto status = del (transaction_a, tables::accounts, account_a);
		release_assert (success (status));
	}

	bool account_get (ysu::transaction const & transaction_a, ysu::account const & account_a, ysu::account_info & info_a) override
	{
		ysu::db_val<Val> value;
		ysu::db_val<Val> account (account_a);
		auto status1 (get (transaction_a, tables::accounts, account, value));
		release_assert (success (status1) || not_found (status1));
		bool result (true);
		if (success (status1))
		{
			ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
			result = info_a.deserialize (stream);
		}
		return result;
	}

	void unchecked_clear (ysu::write_transaction const & transaction_a) override
	{
		auto status = drop (transaction_a, tables::unchecked);
		release_assert (success (status));
	}

	size_t online_weight_count (ysu::transaction const & transaction_a) const override
	{
		return count (transaction_a, tables::online_weight);
	}

	void online_weight_clear (ysu::write_transaction const & transaction_a) override
	{
		auto status (drop (transaction_a, tables::online_weight));
		release_assert (success (status));
	}

	void pruned_put (ysu::write_transaction const & transaction_a, ysu::block_hash const & hash_a) override
	{
		auto status = put_key (transaction_a, tables::pruned, hash_a);
		release_assert (success (status));
	}

	void pruned_del (ysu::write_transaction const & transaction_a, ysu::block_hash const & hash_a) override
	{
		auto status = del (transaction_a, tables::pruned, hash_a);
		release_assert (success (status));
	}

	bool pruned_exists (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const override
	{
		return exists (transaction_a, tables::pruned, ysu::db_val<Val> (hash_a));
	}

	bool block_or_pruned_exists (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) override
	{
		return block_exists (transaction_a, hash_a) || pruned_exists (transaction_a, hash_a);
	}

	size_t pruned_count (ysu::transaction const & transaction_a) const override
	{
		return count (transaction_a, tables::pruned);
	}

	void pruned_clear (ysu::write_transaction const & transaction_a) override
	{
		auto status = drop (transaction_a, tables::pruned);
		release_assert (success (status));
	}

	void peer_put (ysu::write_transaction const & transaction_a, ysu::endpoint_key const & endpoint_a) override
	{
		auto status = put_key (transaction_a, tables::peers, endpoint_a);
		release_assert (success (status));
	}

	void peer_del (ysu::write_transaction const & transaction_a, ysu::endpoint_key const & endpoint_a) override
	{
		auto status (del (transaction_a, tables::peers, endpoint_a));
		release_assert (success (status));
	}

	bool peer_exists (ysu::transaction const & transaction_a, ysu::endpoint_key const & endpoint_a) const override
	{
		return exists (transaction_a, tables::peers, ysu::db_val<Val> (endpoint_a));
	}

	size_t peer_count (ysu::transaction const & transaction_a) const override
	{
		return count (transaction_a, tables::peers);
	}

	void peer_clear (ysu::write_transaction const & transaction_a) override
	{
		auto status = drop (transaction_a, tables::peers);
		release_assert (success (status));
	}

	bool exists (ysu::transaction const & transaction_a, tables table_a, ysu::db_val<Val> const & key_a) const
	{
		return static_cast<const Derived_Store &> (*this).exists (transaction_a, table_a, key_a);
	}

	uint64_t block_count (ysu::transaction const & transaction_a) override
	{
		return count (transaction_a, tables::blocks);
	}

	size_t account_count (ysu::transaction const & transaction_a) override
	{
		return count (transaction_a, tables::accounts);
	}

	std::shared_ptr<ysu::block> block_random (ysu::transaction const & transaction_a) override
	{
		ysu::block_hash hash;
		ysu::random_pool::generate_block (hash.bytes.data (), hash.bytes.size ());
		auto existing = make_iterator<ysu::block_hash, std::shared_ptr<ysu::block>> (transaction_a, tables::blocks, ysu::db_val<Val> (hash));
		auto end (ysu::store_iterator<ysu::block_hash, std::shared_ptr<ysu::block>> (nullptr));
		if (existing == end)
		{
			existing = make_iterator<ysu::block_hash, std::shared_ptr<ysu::block>> (transaction_a, tables::blocks);
		}
		debug_assert (existing != end);
		return existing->second;
	}

	ysu::block_hash pruned_random (ysu::transaction const & transaction_a) override
	{
		ysu::block_hash random_hash;
		ysu::random_pool::generate_block (random_hash.bytes.data (), random_hash.bytes.size ());
		auto existing = make_iterator<ysu::block_hash, ysu::db_val<Val>> (transaction_a, tables::pruned, ysu::db_val<Val> (random_hash));
		auto end (ysu::store_iterator<ysu::block_hash, ysu::db_val<Val>> (nullptr));
		if (existing == end)
		{
			existing = make_iterator<ysu::block_hash, ysu::db_val<Val>> (transaction_a, tables::pruned);
		}
		return existing != end ? existing->first : 0;
	}

	uint64_t confirmation_height_count (ysu::transaction const & transaction_a) override
	{
		return count (transaction_a, tables::confirmation_height);
	}

	void confirmation_height_put (ysu::write_transaction const & transaction_a, ysu::account const & account_a, ysu::confirmation_height_info const & confirmation_height_info_a) override
	{
		ysu::db_val<Val> confirmation_height_info (confirmation_height_info_a);
		auto status = put (transaction_a, tables::confirmation_height, account_a, confirmation_height_info);
		release_assert (success (status));
	}

	bool confirmation_height_get (ysu::transaction const & transaction_a, ysu::account const & account_a, ysu::confirmation_height_info & confirmation_height_info_a) override
	{
		ysu::db_val<Val> value;
		auto status = get (transaction_a, tables::confirmation_height, ysu::db_val<Val> (account_a), value);
		release_assert (success (status) || not_found (status));
		bool result (true);
		if (success (status))
		{
			ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (value.data ()), value.size ());
			result = confirmation_height_info_a.deserialize (stream);
		}
		return result;
	}

	void confirmation_height_del (ysu::write_transaction const & transaction_a, ysu::account const & account_a) override
	{
		auto status (del (transaction_a, tables::confirmation_height, ysu::db_val<Val> (account_a)));
		release_assert (success (status));
	}

	bool confirmation_height_exists (ysu::transaction const & transaction_a, ysu::account const & account_a) const override
	{
		return exists (transaction_a, tables::confirmation_height, ysu::db_val<Val> (account_a));
	}

	ysu::store_iterator<ysu::account, ysu::account_info> accounts_begin (ysu::transaction const & transaction_a, ysu::account const & account_a) const override
	{
		return make_iterator<ysu::account, ysu::account_info> (transaction_a, tables::accounts, ysu::db_val<Val> (account_a));
	}

	ysu::store_iterator<ysu::account, ysu::account_info> accounts_begin (ysu::transaction const & transaction_a) const override
	{
		return make_iterator<ysu::account, ysu::account_info> (transaction_a, tables::accounts);
	}

	ysu::store_iterator<ysu::block_hash, std::shared_ptr<ysu::block>> blocks_begin (ysu::transaction const & transaction_a) const override
	{
		return make_iterator<ysu::block_hash, std::shared_ptr<ysu::block>> (transaction_a, tables::blocks);
	}

	ysu::store_iterator<ysu::pending_key, ysu::pending_info> pending_begin (ysu::transaction const & transaction_a, ysu::pending_key const & key_a) override
	{
		return make_iterator<ysu::pending_key, ysu::pending_info> (transaction_a, tables::pending, ysu::db_val<Val> (key_a));
	}

	ysu::store_iterator<ysu::pending_key, ysu::pending_info> pending_begin (ysu::transaction const & transaction_a) override
	{
		return make_iterator<ysu::pending_key, ysu::pending_info> (transaction_a, tables::pending);
	}

	ysu::store_iterator<ysu::unchecked_key, ysu::unchecked_info> unchecked_begin (ysu::transaction const & transaction_a) const override
	{
		return make_iterator<ysu::unchecked_key, ysu::unchecked_info> (transaction_a, tables::unchecked);
	}

	ysu::store_iterator<ysu::unchecked_key, ysu::unchecked_info> unchecked_begin (ysu::transaction const & transaction_a, ysu::unchecked_key const & key_a) const override
	{
		return make_iterator<ysu::unchecked_key, ysu::unchecked_info> (transaction_a, tables::unchecked, ysu::db_val<Val> (key_a));
	}

	ysu::store_iterator<ysu::account, std::shared_ptr<ysu::vote>> vote_begin (ysu::transaction const & transaction_a) override
	{
		return make_iterator<ysu::account, std::shared_ptr<ysu::vote>> (transaction_a, tables::vote);
	}

	ysu::store_iterator<uint64_t, ysu::amount> online_weight_begin (ysu::transaction const & transaction_a) const override
	{
		return make_iterator<uint64_t, ysu::amount> (transaction_a, tables::online_weight);
	}

	ysu::store_iterator<ysu::endpoint_key, ysu::no_value> peers_begin (ysu::transaction const & transaction_a) const override
	{
		return make_iterator<ysu::endpoint_key, ysu::no_value> (transaction_a, tables::peers);
	}

	ysu::store_iterator<ysu::account, ysu::confirmation_height_info> confirmation_height_begin (ysu::transaction const & transaction_a, ysu::account const & account_a) const override
	{
		return make_iterator<ysu::account, ysu::confirmation_height_info> (transaction_a, tables::confirmation_height, ysu::db_val<Val> (account_a));
	}

	ysu::store_iterator<ysu::account, ysu::confirmation_height_info> confirmation_height_begin (ysu::transaction const & transaction_a) const override
	{
		return make_iterator<ysu::account, ysu::confirmation_height_info> (transaction_a, tables::confirmation_height);
	}

	size_t unchecked_count (ysu::transaction const & transaction_a) override
	{
		return count (transaction_a, tables::unchecked);
	}

	void accounts_for_each_par (std::function<void(ysu::read_transaction const &, ysu::store_iterator<ysu::account, ysu::account_info>, ysu::store_iterator<ysu::account, ysu::account_info>)> const & action_a) override
	{
		parallel_traversal<ysu::uint256_t> (
		[&action_a, this](ysu::uint256_t const & start, ysu::uint256_t const & end, bool const is_last) {
			auto transaction (this->tx_begin_read ());
			action_a (transaction, this->accounts_begin (transaction, start), !is_last ? this->accounts_begin (transaction, end) : this->accounts_end ());
		});
	}

	void confirmation_height_for_each_par (std::function<void(ysu::read_transaction const &, ysu::store_iterator<ysu::account, ysu::confirmation_height_info>, ysu::store_iterator<ysu::account, ysu::confirmation_height_info>)> const & action_a) override
	{
		parallel_traversal<ysu::uint256_t> (
		[&action_a, this](ysu::uint256_t const & start, ysu::uint256_t const & end, bool const is_last) {
			auto transaction (this->tx_begin_read ());
			action_a (transaction, this->confirmation_height_begin (transaction, start), !is_last ? this->confirmation_height_begin (transaction, end) : this->confirmation_height_end ());
		});
	}

	void pending_for_each_par (std::function<void(ysu::read_transaction const &, ysu::store_iterator<ysu::pending_key, ysu::pending_info>, ysu::store_iterator<ysu::pending_key, ysu::pending_info>)> const & action_a) override
	{
		parallel_traversal<ysu::uint512_t> (
		[&action_a, this](ysu::uint512_t const & start, ysu::uint512_t const & end, bool const is_last) {
			ysu::uint512_union union_start (start);
			ysu::uint512_union union_end (end);
			ysu::pending_key key_start (union_start.uint256s[0].number (), union_start.uint256s[1].number ());
			ysu::pending_key key_end (union_end.uint256s[0].number (), union_end.uint256s[1].number ());
			auto transaction (this->tx_begin_read ());
			action_a (transaction, this->pending_begin (transaction, key_start), !is_last ? this->pending_begin (transaction, key_end) : this->pending_end ());
		});
	}

	int const minimum_version{ 14 };

protected:
	ysu::network_params network_params;
	std::unordered_map<ysu::account, std::shared_ptr<ysu::vote>> vote_cache_l1;
	std::unordered_map<ysu::account, std::shared_ptr<ysu::vote>> vote_cache_l2;
	int const version{ 20 };

	template <typename Key, typename Value>
	ysu::store_iterator<Key, Value> make_iterator (ysu::transaction const & transaction_a, tables table_a) const
	{
		return static_cast<Derived_Store const &> (*this).template make_iterator<Key, Value> (transaction_a, table_a);
	}

	template <typename Key, typename Value>
	ysu::store_iterator<Key, Value> make_iterator (ysu::transaction const & transaction_a, tables table_a, ysu::db_val<Val> const & key) const
	{
		return static_cast<Derived_Store const &> (*this).template make_iterator<Key, Value> (transaction_a, table_a, key);
	}

	ysu::db_val<Val> block_raw_get (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const
	{
		ysu::db_val<Val> result;
		auto status = get (transaction_a, tables::blocks, hash_a, result);
		release_assert (success (status) || not_found (status));
		return result;
	}

	size_t block_successor_offset (ysu::transaction const & transaction_a, size_t entry_size_a, ysu::block_type type_a) const
	{
		return entry_size_a - ysu::block_sideband::size (type_a);
	}

	static ysu::block_type block_type_from_raw (void * data_a)
	{
		// The block type is the first byte
		return static_cast<ysu::block_type> ((reinterpret_cast<uint8_t const *> (data_a))[0]);
	}

	uint64_t count (ysu::transaction const & transaction_a, std::initializer_list<tables> dbs_a) const
	{
		uint64_t total_count = 0;
		for (auto db : dbs_a)
		{
			total_count += count (transaction_a, db);
		}
		return total_count;
	}

	int get (ysu::transaction const & transaction_a, tables table_a, ysu::db_val<Val> const & key_a, ysu::db_val<Val> & value_a) const
	{
		return static_cast<Derived_Store const &> (*this).get (transaction_a, table_a, key_a, value_a);
	}

	int put (ysu::write_transaction const & transaction_a, tables table_a, ysu::db_val<Val> const & key_a, ysu::db_val<Val> const & value_a)
	{
		return static_cast<Derived_Store &> (*this).put (transaction_a, table_a, key_a, value_a);
	}

	// Put only key without value
	int put_key (ysu::write_transaction const & transaction_a, tables table_a, ysu::db_val<Val> const & key_a)
	{
		return put (transaction_a, table_a, key_a, ysu::db_val<Val>{ std::nullptr_t{} });
	}

	int del (ysu::write_transaction const & transaction_a, tables table_a, ysu::db_val<Val> const & key_a)
	{
		return static_cast<Derived_Store &> (*this).del (transaction_a, table_a, key_a);
	}

	virtual uint64_t count (ysu::transaction const & transaction_a, tables table_a) const = 0;
	virtual int drop (ysu::write_transaction const & transaction_a, tables table_a) = 0;
	virtual bool not_found (int status) const = 0;
	virtual bool success (int status) const = 0;
	virtual int status_code_not_found () const = 0;
};

/**
 * Fill in our predecessors
 */
template <typename Val, typename Derived_Store>
class block_predecessor_set : public ysu::block_visitor
{
public:
	block_predecessor_set (ysu::write_transaction const & transaction_a, ysu::block_store_partial<Val, Derived_Store> & store_a) :
	transaction (transaction_a),
	store (store_a)
	{
	}
	virtual ~block_predecessor_set () = default;
	void fill_value (ysu::block const & block_a)
	{
		auto hash (block_a.hash ());
		auto value (store.block_raw_get (transaction, block_a.previous ()));
		debug_assert (value.size () != 0);
		auto type = store.block_type_from_raw (value.data ());
		std::vector<uint8_t> data (static_cast<uint8_t *> (value.data ()), static_cast<uint8_t *> (value.data ()) + value.size ());
		std::copy (hash.bytes.begin (), hash.bytes.end (), data.begin () + store.block_successor_offset (transaction, value.size (), type));
		store.block_raw_put (transaction, data, block_a.previous ());
	}
	void send_block (ysu::send_block const & block_a) override
	{
		fill_value (block_a);
	}
	void receive_block (ysu::receive_block const & block_a) override
	{
		fill_value (block_a);
	}
	void open_block (ysu::open_block const & block_a) override
	{
		// Open blocks don't have a predecessor
	}
	void change_block (ysu::change_block const & block_a) override
	{
		fill_value (block_a);
	}
	void state_block (ysu::state_block const & block_a) override
	{
		if (!block_a.previous ().is_zero ())
		{
			fill_value (block_a);
		}
	}
	ysu::write_transaction const & transaction;
	ysu::block_store_partial<Val, Derived_Store> & store;
};
}

namespace
{
template <typename T>
void parallel_traversal (std::function<void(T const &, T const &, bool const)> const & action)
{
	// Between 10 and 40 threads, scales well even in low power systems as long as actions are I/O bound
	unsigned const thread_count = std::max (10u, std::min (40u, 10 * std::thread::hardware_concurrency ()));
	T const value_max{ std::numeric_limits<T>::max () };
	T const split = value_max / thread_count;
	std::vector<std::thread> threads;
	threads.reserve (thread_count);
	for (unsigned thread (0); thread < thread_count; ++thread)
	{
		T const start = thread * split;
		T const end = (thread + 1) * split;
		bool const is_last = thread == thread_count - 1;

		threads.emplace_back ([&action, start, end, is_last] {
			ysu::thread_role::set (ysu::thread_role::name::db_parallel_traversal);
			action (start, end, is_last);
		});
	}
	for (auto & thread : threads)
	{
		thread.join ();
	}
}
}
