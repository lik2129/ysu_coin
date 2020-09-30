#pragma once

#include <ysu/lib/diagnosticsconfig.hpp>
#include <ysu/lib/lmdbconfig.hpp>
#include <ysu/lib/logger_mt.hpp>
#include <ysu/lib/numbers.hpp>
#include <ysu/node/lmdb/lmdb_env.hpp>
#include <ysu/node/lmdb/lmdb_iterator.hpp>
#include <ysu/node/lmdb/lmdb_txn.hpp>
#include <ysu/secure/blockstore_partial.hpp>
#include <ysu/secure/common.hpp>
#include <ysu/secure/versioning.hpp>

#include <boost/optional.hpp>

#include <lmdb/libraries/liblmdb/lmdb.h>

namespace boost
{
namespace filesystem
{
	class path;
}
}

namespace ysu
{
using mdb_val = db_val<MDB_val>;

class logging_mt;
/**
 * mdb implementation of the block store
 */
class mdb_store : public block_store_partial<MDB_val, mdb_store>
{
public:
	using block_store_partial::block_exists;
	using block_store_partial::unchecked_put;

	mdb_store (ysu::logger_mt &, boost::filesystem::path const &, ysu::txn_tracking_config const & txn_tracking_config_a = ysu::txn_tracking_config{}, std::chrono::milliseconds block_processor_batch_max_time_a = std::chrono::milliseconds (5000), ysu::lmdb_config const & lmdb_config_a = ysu::lmdb_config{}, bool backup_before_upgrade = false);
	ysu::write_transaction tx_begin_write (std::vector<ysu::tables> const & tables_requiring_lock = {}, std::vector<ysu::tables> const & tables_no_lock = {}) override;
	ysu::read_transaction tx_begin_read () override;

	std::string vendor_get () const override;

	void version_put (ysu::write_transaction const &, int) override;

	void serialize_mdb_tracker (boost::property_tree::ptree &, std::chrono::milliseconds, std::chrono::milliseconds) override;

	static void create_backup_file (ysu::mdb_env &, boost::filesystem::path const &, ysu::logger_mt &);

	void serialize_memory_stats (boost::property_tree::ptree &) override;

	unsigned max_block_write_batch_num () const override;

private:
	ysu::logger_mt & logger;
	bool error{ false };

public:
	ysu::mdb_env env;

	/**
	 * Maps head block to owning account
	 * ysu::block_hash -> ysu::account
	 */
	MDB_dbi frontiers{ 0 };

	/**
	 * Maps account v1 to account information, head, rep, open, balance, timestamp and block count. (Removed)
	 * ysu::account -> ysu::block_hash, ysu::block_hash, ysu::block_hash, ysu::amount, uint64_t, uint64_t
	 */
	MDB_dbi accounts_v0{ 0 };

	/**
	 * Maps account v0 to account information, head, rep, open, balance, timestamp and block count. (Removed)
	 * ysu::account -> ysu::block_hash, ysu::block_hash, ysu::block_hash, ysu::amount, uint64_t, uint64_t
	 */
	MDB_dbi accounts_v1{ 0 };

	/**
	 * Maps account v0 to account information, head, rep, open, balance, timestamp, block count and epoch
	 * ysu::account -> ysu::block_hash, ysu::block_hash, ysu::block_hash, ysu::amount, uint64_t, uint64_t, ysu::epoch
	 */
	MDB_dbi accounts{ 0 };

	/**
	 * Maps block hash to send block. (Removed)
	 * ysu::block_hash -> ysu::send_block
	 */
	MDB_dbi send_blocks{ 0 };

	/**
	 * Maps block hash to receive block. (Removed)
	 * ysu::block_hash -> ysu::receive_block
	 */
	MDB_dbi receive_blocks{ 0 };

	/**
	 * Maps block hash to open block. (Removed)
	 * ysu::block_hash -> ysu::open_block
	 */
	MDB_dbi open_blocks{ 0 };

	/**
	 * Maps block hash to change block. (Removed)
	 * ysu::block_hash -> ysu::change_block
	 */
	MDB_dbi change_blocks{ 0 };

	/**
	 * Maps block hash to v0 state block. (Removed)
	 * ysu::block_hash -> ysu::state_block
	 */
	MDB_dbi state_blocks_v0{ 0 };

	/**
	 * Maps block hash to v1 state block. (Removed)
	 * ysu::block_hash -> ysu::state_block
	 */
	MDB_dbi state_blocks_v1{ 0 };

	/**
	 * Maps block hash to state block. (Removed)
	 * ysu::block_hash -> ysu::state_block
	 */
	MDB_dbi state_blocks{ 0 };

	/**
	 * Maps min_version 0 (destination account, pending block) to (source account, amount). (Removed)
	 * ysu::account, ysu::block_hash -> ysu::account, ysu::amount
	 */
	MDB_dbi pending_v0{ 0 };

	/**
	 * Maps min_version 1 (destination account, pending block) to (source account, amount). (Removed)
	 * ysu::account, ysu::block_hash -> ysu::account, ysu::amount
	 */
	MDB_dbi pending_v1{ 0 };

	/**
	 * Maps (destination account, pending block) to (source account, amount, version). (Removed)
	 * ysu::account, ysu::block_hash -> ysu::account, ysu::amount, ysu::epoch
	 */
	MDB_dbi pending{ 0 };

	/**
	 * Representative weights. (Removed)
	 * ysu::account -> ysu::uint128_t
	 */
	MDB_dbi representation{ 0 };

	/**
	 * Unchecked bootstrap blocks info.
	 * ysu::block_hash -> ysu::unchecked_info
	 */
	MDB_dbi unchecked{ 0 };

	/**
	 * Highest vote observed for account.
	 * ysu::account -> uint64_t
	 */
	MDB_dbi vote{ 0 };

	/**
	 * Samples of online vote weight
	 * uint64_t -> ysu::amount
	 */
	MDB_dbi online_weight{ 0 };

	/**
	 * Meta information about block store, such as versions.
	 * ysu::uint256_union (arbitrary key) -> blob
	 */
	MDB_dbi meta{ 0 };

	/**
	 * Pruned blocks hashes
	 * ysu::block_hash -> none
	 */
	MDB_dbi pruned{ 0 };

	/*
	 * Endpoints for peers
	 * ysu::endpoint_key -> no_value
	*/
	MDB_dbi peers{ 0 };

	/*
	 * Confirmation height of an account, and the hash for the block at that height
	 * ysu::account -> uint64_t, ysu::block_hash
	 */
	MDB_dbi confirmation_height{ 0 };

	/*
	 * Contains block_sideband and block for all block types (legacy send/change/open/receive & state blocks)
	 * ysu::block_hash -> ysu::block_sideband, ysu::block
	 */
	MDB_dbi blocks{ 0 };

	bool exists (ysu::transaction const & transaction_a, tables table_a, ysu::mdb_val const & key_a) const;
	std::vector<ysu::unchecked_info> unchecked_get (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) override;

	int get (ysu::transaction const & transaction_a, tables table_a, ysu::mdb_val const & key_a, ysu::mdb_val & value_a) const;
	int put (ysu::write_transaction const & transaction_a, tables table_a, ysu::mdb_val const & key_a, const ysu::mdb_val & value_a) const;
	int del (ysu::write_transaction const & transaction_a, tables table_a, ysu::mdb_val const & key_a) const;

	bool copy_db (boost::filesystem::path const & destination_file) override;
	void rebuild_db (ysu::write_transaction const & transaction_a) override;

	template <typename Key, typename Value>
	ysu::store_iterator<Key, Value> make_iterator (ysu::transaction const & transaction_a, tables table_a) const
	{
		return ysu::store_iterator<Key, Value> (std::make_unique<ysu::mdb_iterator<Key, Value>> (transaction_a, table_to_dbi (table_a)));
	}

	template <typename Key, typename Value>
	ysu::store_iterator<Key, Value> make_iterator (ysu::transaction const & transaction_a, tables table_a, ysu::mdb_val const & key) const
	{
		return ysu::store_iterator<Key, Value> (std::make_unique<ysu::mdb_iterator<Key, Value>> (transaction_a, table_to_dbi (table_a), key));
	}

	bool init_error () const override;

	uint64_t count (ysu::transaction const &, MDB_dbi) const;

	// These are only use in the upgrade process.
	std::shared_ptr<ysu::block> block_get_v14 (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a, ysu::block_sideband_v14 * sideband_a = nullptr, bool * is_state_v1 = nullptr) const;
	size_t block_successor_offset_v14 (ysu::transaction const & transaction_a, size_t entry_size_a, ysu::block_type type_a) const;
	ysu::block_hash block_successor_v14 (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const;
	ysu::mdb_val block_raw_get_v14 (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a, ysu::block_type & type_a, bool * is_state_v1 = nullptr) const;
	boost::optional<ysu::mdb_val> block_raw_get_by_type_v14 (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a, ysu::block_type & type_a, bool * is_state_v1) const;

private:
	bool do_upgrades (ysu::write_transaction &, bool &);
	void upgrade_v14_to_v15 (ysu::write_transaction &);
	void upgrade_v15_to_v16 (ysu::write_transaction const &);
	void upgrade_v16_to_v17 (ysu::write_transaction const &);
	void upgrade_v17_to_v18 (ysu::write_transaction const &);
	void upgrade_v18_to_v19 (ysu::write_transaction const &);
	void upgrade_v19_to_v20 (ysu::write_transaction const &);

	std::shared_ptr<ysu::block> block_get_v18 (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const;
	ysu::mdb_val block_raw_get_v18 (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a, ysu::block_type & type_a) const;
	boost::optional<ysu::mdb_val> block_raw_get_by_type_v18 (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a, ysu::block_type & type_a) const;
	ysu::uint128_t block_balance_v18 (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const;

	void open_databases (bool &, ysu::transaction const &, unsigned);

	int drop (ysu::write_transaction const & transaction_a, tables table_a) override;
	int clear (ysu::write_transaction const & transaction_a, MDB_dbi handle_a);

	bool not_found (int status) const override;
	bool success (int status) const override;
	int status_code_not_found () const override;

	MDB_dbi table_to_dbi (tables table_a) const;

	ysu::mdb_txn_tracker mdb_txn_tracker;
	ysu::mdb_txn_callbacks create_txn_callbacks ();
	bool txn_tracking_enabled;

	uint64_t count (ysu::transaction const & transaction_a, tables table_a) const override;

	bool vacuum_after_upgrade (boost::filesystem::path const & path_a, ysu::lmdb_config const & lmdb_config_a);

	class upgrade_counters
	{
	public:
		upgrade_counters (uint64_t count_before_v0, uint64_t count_before_v1);
		bool are_equal () const;

		uint64_t before_v0;
		uint64_t before_v1;
		uint64_t after_v0{ 0 };
		uint64_t after_v1{ 0 };
	};
};

template <>
void * mdb_val::data () const;
template <>
size_t mdb_val::size () const;
template <>
mdb_val::db_val (size_t size_a, void * data_a);
template <>
void mdb_val::convert_buffer_to_value ();

extern template class block_store_partial<MDB_val, mdb_store>;
}
