#pragma once

#include <ysu/lib/config.hpp>
#include <ysu/lib/logger_mt.hpp>
#include <ysu/lib/numbers.hpp>
#include <ysu/node/rocksdb/rocksdb_iterator.hpp>
#include <ysu/secure/blockstore_partial.hpp>
#include <ysu/secure/common.hpp>

#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/optimistic_transaction_db.h>
#include <rocksdb/utilities/transaction.h>

namespace ysu
{
class logging_mt;
class rocksdb_config;

/**
 * rocksdb implementation of the block store
 */
class rocksdb_store : public block_store_partial<rocksdb::Slice, rocksdb_store>
{
public:
	rocksdb_store (ysu::logger_mt &, boost::filesystem::path const &, ysu::rocksdb_config const & = ysu::rocksdb_config{}, bool open_read_only = false);
	ysu::write_transaction tx_begin_write (std::vector<ysu::tables> const & tables_requiring_lock = {}, std::vector<ysu::tables> const & tables_no_lock = {}) override;
	ysu::read_transaction tx_begin_read () override;

	std::string vendor_get () const override;

	uint64_t count (ysu::transaction const & transaction_a, tables table_a) const override;
	void version_put (ysu::write_transaction const &, int) override;
	std::vector<ysu::unchecked_info> unchecked_get (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) override;

	bool exists (ysu::transaction const & transaction_a, tables table_a, ysu::rocksdb_val const & key_a) const;
	int get (ysu::transaction const & transaction_a, tables table_a, ysu::rocksdb_val const & key_a, ysu::rocksdb_val & value_a) const;
	int put (ysu::write_transaction const & transaction_a, tables table_a, ysu::rocksdb_val const & key_a, ysu::rocksdb_val const & value_a);
	int del (ysu::write_transaction const & transaction_a, tables table_a, ysu::rocksdb_val const & key_a);

	void serialize_memory_stats (boost::property_tree::ptree &) override;

	bool copy_db (boost::filesystem::path const & destination) override;
	void rebuild_db (ysu::write_transaction const & transaction_a) override;

	unsigned max_block_write_batch_num () const override;

	template <typename Key, typename Value>
	ysu::store_iterator<Key, Value> make_iterator (ysu::transaction const & transaction_a, tables table_a) const
	{
		return ysu::store_iterator<Key, Value> (std::make_unique<ysu::rocksdb_iterator<Key, Value>> (db.get (), transaction_a, table_to_column_family (table_a)));
	}

	template <typename Key, typename Value>
	ysu::store_iterator<Key, Value> make_iterator (ysu::transaction const & transaction_a, tables table_a, ysu::rocksdb_val const & key) const
	{
		return ysu::store_iterator<Key, Value> (std::make_unique<ysu::rocksdb_iterator<Key, Value>> (db.get (), transaction_a, table_to_column_family (table_a), &key));
	}

	bool init_error () const override;

private:
	bool error{ false };
	ysu::logger_mt & logger;
	// Optimistic transactions are used in write mode
	rocksdb::OptimisticTransactionDB * optimistic_db = nullptr;
	std::unique_ptr<rocksdb::DB> db;
	std::vector<std::unique_ptr<rocksdb::ColumnFamilyHandle>> handles;
	std::shared_ptr<rocksdb::TableFactory> small_table_factory;
	std::unordered_map<ysu::tables, std::mutex> write_lock_mutexes;
	ysu::rocksdb_config rocksdb_config;
	unsigned const max_block_write_batch_num_m;

	class tombstone_info
	{
	public:
		tombstone_info (uint64_t, uint64_t const);
		std::atomic<uint64_t> num_since_last_flush;
		uint64_t const max;
	};

	std::unordered_map<ysu::tables, tombstone_info> tombstone_map;
	std::unordered_map<const char *, ysu::tables> cf_name_table_map;

	rocksdb::Transaction * tx (ysu::transaction const & transaction_a) const;
	std::vector<ysu::tables> all_tables () const;

	bool not_found (int status) const override;
	bool success (int status) const override;
	int status_code_not_found () const override;
	int drop (ysu::write_transaction const &, tables) override;

	rocksdb::ColumnFamilyHandle * table_to_column_family (tables table_a) const;
	int clear (rocksdb::ColumnFamilyHandle * column_family);

	void open (bool & error_a, boost::filesystem::path const & path_a, bool open_read_only_a);

	void construct_column_family_mutexes ();
	rocksdb::Options get_db_options ();
	rocksdb::ColumnFamilyOptions get_common_cf_options (std::shared_ptr<rocksdb::TableFactory> const & table_factory_a, unsigned long long memtable_size_bytes_a) const;
	rocksdb::ColumnFamilyOptions get_active_cf_options (std::shared_ptr<rocksdb::TableFactory> const & table_factory_a, unsigned long long memtable_size_bytes_a) const;
	rocksdb::ColumnFamilyOptions get_small_cf_options (std::shared_ptr<rocksdb::TableFactory> const & table_factory_a) const;
	rocksdb::BlockBasedTableOptions get_active_table_options (size_t lru_size) const;
	rocksdb::BlockBasedTableOptions get_small_table_options () const;
	rocksdb::ColumnFamilyOptions get_cf_options (std::string const & cf_name_a) const;

	void on_flush (rocksdb::FlushJobInfo const &);
	void flush_table (ysu::tables table_a);
	void flush_tombstones_check (ysu::tables table_a);
	void generate_tombstone_map ();
	std::unordered_map<const char *, ysu::tables> create_cf_name_table_map () const;

	std::vector<rocksdb::ColumnFamilyDescriptor> create_column_families ();
	unsigned long long base_memtable_size_bytes () const;
	unsigned long long blocks_memtable_size_bytes () const;

	constexpr static int base_memtable_size = 16;
	constexpr static int base_block_cache_size = 8;

	friend class rocksdb_block_store_tombstone_count_Test;
};

extern template class block_store_partial<rocksdb::Slice, rocksdb_store>;
}
