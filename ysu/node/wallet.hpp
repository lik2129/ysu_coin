#pragma once

#include <ysu/lib/lmdbconfig.hpp>
#include <ysu/lib/locks.hpp>
#include <ysu/lib/work.hpp>
#include <ysu/node/lmdb/lmdb.hpp>
#include <ysu/node/lmdb/wallet_value.hpp>
#include <ysu/node/openclwork.hpp>
#include <ysu/secure/blockstore.hpp>
#include <ysu/secure/common.hpp>

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>
namespace ysu
{
class node;
class node_config;
class wallets;
// The fan spreads a key out over the heap to decrease the likelihood of it being recovered by memory inspection
class fan final
{
public:
	fan (ysu::uint256_union const &, size_t);
	void value (ysu::raw_key &);
	void value_set (ysu::raw_key const &);
	std::vector<std::unique_ptr<ysu::uint256_union>> values;

private:
	std::mutex mutex;
	void value_get (ysu::raw_key &);
};
class kdf final
{
public:
	void phs (ysu::raw_key &, std::string const &, ysu::uint256_union const &);
	std::mutex mutex;
};
enum class key_type
{
	not_a_type,
	unknown,
	adhoc,
	deterministic
};
class wallet_store final
{
public:
	wallet_store (bool &, ysu::kdf &, ysu::transaction &, ysu::account, unsigned, std::string const &);
	wallet_store (bool &, ysu::kdf &, ysu::transaction &, ysu::account, unsigned, std::string const &, std::string const &);
	std::vector<ysu::account> accounts (ysu::transaction const &);
	void initialize (ysu::transaction const &, bool &, std::string const &);
	ysu::uint256_union check (ysu::transaction const &);
	bool rekey (ysu::transaction const &, std::string const &);
	bool valid_password (ysu::transaction const &);
	bool valid_public_key (ysu::public_key const &);
	bool attempt_password (ysu::transaction const &, std::string const &);
	void wallet_key (ysu::raw_key &, ysu::transaction const &);
	void seed (ysu::raw_key &, ysu::transaction const &);
	void seed_set (ysu::transaction const &, ysu::raw_key const &);
	ysu::key_type key_type (ysu::wallet_value const &);
	ysu::public_key deterministic_insert (ysu::transaction const &);
	ysu::public_key deterministic_insert (ysu::transaction const &, uint32_t const);
	ysu::private_key deterministic_key (ysu::transaction const &, uint32_t);
	uint32_t deterministic_index_get (ysu::transaction const &);
	void deterministic_index_set (ysu::transaction const &, uint32_t);
	void deterministic_clear (ysu::transaction const &);
	ysu::uint256_union salt (ysu::transaction const &);
	bool is_representative (ysu::transaction const &);
	ysu::account representative (ysu::transaction const &);
	void representative_set (ysu::transaction const &, ysu::account const &);
	ysu::public_key insert_adhoc (ysu::transaction const &, ysu::raw_key const &);
	bool insert_watch (ysu::transaction const &, ysu::account const &);
	void erase (ysu::transaction const &, ysu::account const &);
	ysu::wallet_value entry_get_raw (ysu::transaction const &, ysu::account const &);
	void entry_put_raw (ysu::transaction const &, ysu::account const &, ysu::wallet_value const &);
	bool fetch (ysu::transaction const &, ysu::account const &, ysu::raw_key &);
	bool exists (ysu::transaction const &, ysu::account const &);
	void destroy (ysu::transaction const &);
	ysu::store_iterator<ysu::account, ysu::wallet_value> find (ysu::transaction const &, ysu::account const &);
	ysu::store_iterator<ysu::account, ysu::wallet_value> begin (ysu::transaction const &, ysu::account const &);
	ysu::store_iterator<ysu::account, ysu::wallet_value> begin (ysu::transaction const &);
	ysu::store_iterator<ysu::account, ysu::wallet_value> end ();
	void derive_key (ysu::raw_key &, ysu::transaction const &, std::string const &);
	void serialize_json (ysu::transaction const &, std::string &);
	void write_backup (ysu::transaction const &, boost::filesystem::path const &);
	bool move (ysu::transaction const &, ysu::wallet_store &, std::vector<ysu::public_key> const &);
	bool import (ysu::transaction const &, ysu::wallet_store &);
	bool work_get (ysu::transaction const &, ysu::public_key const &, uint64_t &);
	void work_put (ysu::transaction const &, ysu::public_key const &, uint64_t);
	unsigned version (ysu::transaction const &);
	void version_put (ysu::transaction const &, unsigned);
	ysu::fan password;
	ysu::fan wallet_key_mem;
	static unsigned const version_1 = 1;
	static unsigned const version_2 = 2;
	static unsigned const version_3 = 3;
	static unsigned const version_4 = 4;
	static unsigned constexpr version_current = version_4;
	static ysu::account const version_special;
	static ysu::account const wallet_key_special;
	static ysu::account const salt_special;
	static ysu::account const check_special;
	static ysu::account const representative_special;
	static ysu::account const seed_special;
	static ysu::account const deterministic_index_special;
	static size_t const check_iv_index;
	static size_t const seed_iv_index;
	static int const special_count;
	ysu::kdf & kdf;
	std::atomic<MDB_dbi> handle{ 0 };
	std::recursive_mutex mutex;

private:
	MDB_txn * tx (ysu::transaction const &) const;
};
// A wallet is a set of account keys encrypted by a common encryption key
class wallet final : public std::enable_shared_from_this<ysu::wallet>
{
public:
	std::shared_ptr<ysu::block> change_action (ysu::account const &, ysu::account const &, uint64_t = 0, bool = true);
	std::shared_ptr<ysu::block> receive_action (ysu::block const &, ysu::account const &, ysu::uint128_union const &, uint64_t = 0, bool = true);
	std::shared_ptr<ysu::block> send_action (ysu::account const &, ysu::account const &, ysu::uint128_t const &, uint64_t = 0, bool = true, boost::optional<std::string> = {});
	bool action_complete (std::shared_ptr<ysu::block> const &, ysu::account const &, bool const, ysu::block_details const &);
	wallet (bool &, ysu::transaction &, ysu::wallets &, std::string const &);
	wallet (bool &, ysu::transaction &, ysu::wallets &, std::string const &, std::string const &);
	void enter_initial_password ();
	bool enter_password (ysu::transaction const &, std::string const &);
	ysu::public_key insert_adhoc (ysu::raw_key const &, bool = true);
	ysu::public_key insert_adhoc (ysu::transaction const &, ysu::raw_key const &, bool = true);
	bool insert_watch (ysu::transaction const &, ysu::public_key const &);
	ysu::public_key deterministic_insert (ysu::transaction const &, bool = true);
	ysu::public_key deterministic_insert (uint32_t, bool = true);
	ysu::public_key deterministic_insert (bool = true);
	bool exists (ysu::public_key const &);
	bool import (std::string const &, std::string const &);
	void serialize (std::string &);
	bool change_sync (ysu::account const &, ysu::account const &);
	void change_async (ysu::account const &, ysu::account const &, std::function<void(std::shared_ptr<ysu::block>)> const &, uint64_t = 0, bool = true);
	bool receive_sync (std::shared_ptr<ysu::block>, ysu::account const &, ysu::uint128_t const &);
	void receive_async (std::shared_ptr<ysu::block>, ysu::account const &, ysu::uint128_t const &, std::function<void(std::shared_ptr<ysu::block>)> const &, uint64_t = 0, bool = true);
	ysu::block_hash send_sync (ysu::account const &, ysu::account const &, ysu::uint128_t const &);
	void send_async (ysu::account const &, ysu::account const &, ysu::uint128_t const &, std::function<void(std::shared_ptr<ysu::block>)> const &, uint64_t = 0, bool = true, boost::optional<std::string> = {});
	void work_cache_blocking (ysu::account const &, ysu::root const &);
	void work_update (ysu::transaction const &, ysu::account const &, ysu::root const &, uint64_t);
	// Schedule work generation after a few seconds
	void work_ensure (ysu::account const &, ysu::root const &);
	bool search_pending ();
	void init_free_accounts (ysu::transaction const &);
	uint32_t deterministic_check (ysu::transaction const & transaction_a, uint32_t index);
	/** Changes the wallet seed and returns the first account */
	ysu::public_key change_seed (ysu::transaction const & transaction_a, ysu::raw_key const & prv_a, uint32_t count = 0);
	void deterministic_restore (ysu::transaction const & transaction_a);
	bool live ();
	ysu::network_params network_params;
	std::unordered_set<ysu::account> free_accounts;
	std::function<void(bool, bool)> lock_observer;
	ysu::wallet_store store;
	ysu::wallets & wallets;
	std::mutex representatives_mutex;
	std::unordered_set<ysu::account> representatives;
};

class work_watcher final : public std::enable_shared_from_this<ysu::work_watcher>
{
public:
	work_watcher (ysu::node &);
	~work_watcher ();
	void stop ();
	void add (std::shared_ptr<ysu::block>);
	void update (ysu::qualified_root const &, std::shared_ptr<ysu::state_block>);
	void watching (ysu::qualified_root const &, std::shared_ptr<ysu::state_block>);
	void remove (ysu::block const &);
	bool is_watched (ysu::qualified_root const &);
	size_t size ();
	std::mutex mutex;
	ysu::node & node;
	std::unordered_map<ysu::qualified_root, std::shared_ptr<ysu::state_block>> watched;
	std::atomic<bool> stopped;
};

class wallet_representatives
{
public:
	uint64_t voting{ 0 }; // Number of representatives with at least the configured minimum voting weight
	uint64_t half_principal{ 0 }; // Number of representatives with at least 50% of principal representative requirements
	std::unordered_set<ysu::account> accounts; // Representatives with at least the configured minimum voting weight
	bool have_half_rep () const
	{
		return half_principal > 0;
	}
	bool exists (ysu::account const & rep_a) const
	{
		return accounts.count (rep_a) > 0;
	}
	void clear ()
	{
		voting = 0;
		half_principal = 0;
		accounts.clear ();
	}
};

/**
 * The wallets set is all the wallets a node controls.
 * A node may contain multiple wallets independently encrypted and operated.
 */
class wallets final
{
public:
	wallets (bool, ysu::node &);
	~wallets ();
	std::shared_ptr<ysu::wallet> open (ysu::wallet_id const &);
	std::shared_ptr<ysu::wallet> create (ysu::wallet_id const &);
	bool search_pending (ysu::wallet_id const &);
	void search_pending_all ();
	void destroy (ysu::wallet_id const &);
	void reload ();
	void do_wallet_actions ();
	void queue_wallet_action (ysu::uint128_t const &, std::shared_ptr<ysu::wallet>, std::function<void(ysu::wallet &)> const &);
	void foreach_representative (std::function<void(ysu::public_key const &, ysu::raw_key const &)> const &);
	bool exists (ysu::transaction const &, ysu::account const &);
	void stop ();
	void clear_send_ids (ysu::transaction const &);
	ysu::wallet_representatives reps () const;
	bool check_rep (ysu::account const &, ysu::uint128_t const &, const bool = true);
	void compute_reps ();
	void ongoing_compute_reps ();
	void split_if_needed (ysu::transaction &, ysu::block_store &);
	void move_table (std::string const &, MDB_txn *, MDB_txn *);
	std::unordered_map<ysu::wallet_id, std::shared_ptr<ysu::wallet>> get_wallets ();
	ysu::network_params network_params;
	std::function<void(bool)> observer;
	std::unordered_map<ysu::wallet_id, std::shared_ptr<ysu::wallet>> items;
	std::multimap<ysu::uint128_t, std::pair<std::shared_ptr<ysu::wallet>, std::function<void(ysu::wallet &)>>, std::greater<ysu::uint128_t>> actions;
	ysu::locked<std::unordered_map<ysu::account, ysu::root>> delayed_work;
	std::mutex mutex;
	std::mutex action_mutex;
	ysu::condition_variable condition;
	ysu::kdf kdf;
	MDB_dbi handle;
	MDB_dbi send_action_ids;
	ysu::node & node;
	ysu::mdb_env & env;
	std::atomic<bool> stopped;
	std::shared_ptr<ysu::work_watcher> watcher;
	std::thread thread;
	static ysu::uint128_t const generate_priority;
	static ysu::uint128_t const high_priority;
	/** Start read-write transaction */
	ysu::write_transaction tx_begin_write ();

	/** Start read-only transaction */
	ysu::read_transaction tx_begin_read ();

private:
	mutable std::mutex reps_cache_mutex;
	ysu::wallet_representatives representatives;
};

std::unique_ptr<container_info_component> collect_container_info (wallets & wallets, const std::string & name);

class wallets_store
{
public:
	virtual ~wallets_store () = default;
	virtual bool init_error () const = 0;
};
class mdb_wallets_store final : public wallets_store
{
public:
	mdb_wallets_store (boost::filesystem::path const &, ysu::lmdb_config const & lmdb_config_a = ysu::lmdb_config{});
	ysu::mdb_env environment;
	bool init_error () const override;
	bool error{ false };
};
}
