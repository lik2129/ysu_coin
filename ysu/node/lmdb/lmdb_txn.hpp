#pragma once

#include <ysu/lib/diagnosticsconfig.hpp>
#include <ysu/lib/timer.hpp>
#include <ysu/secure/blockstore.hpp>

#include <boost/property_tree/ptree_fwd.hpp>
#include <boost/stacktrace/stacktrace_fwd.hpp>

#include <mutex>

#include <lmdb/libraries/liblmdb/lmdb.h>

namespace ysu
{
class transaction_impl;
class logger_mt;
class mdb_env;

class mdb_txn_callbacks
{
public:
	std::function<void(const ysu::transaction_impl *)> txn_start{ [](const ysu::transaction_impl *) {} };
	std::function<void(const ysu::transaction_impl *)> txn_end{ [](const ysu::transaction_impl *) {} };
};

class read_mdb_txn final : public read_transaction_impl
{
public:
	read_mdb_txn (ysu::mdb_env const &, mdb_txn_callbacks mdb_txn_callbacks);
	~read_mdb_txn ();
	void reset () override;
	void renew () override;
	void * get_handle () const override;
	MDB_txn * handle;
	mdb_txn_callbacks txn_callbacks;
};

class write_mdb_txn final : public write_transaction_impl
{
public:
	write_mdb_txn (ysu::mdb_env const &, mdb_txn_callbacks mdb_txn_callbacks);
	~write_mdb_txn ();
	void commit () const override;
	void renew () override;
	void * get_handle () const override;
	bool contains (ysu::tables table_a) const override;
	MDB_txn * handle;
	ysu::mdb_env const & env;
	mdb_txn_callbacks txn_callbacks;
};

class mdb_txn_stats
{
public:
	mdb_txn_stats (const ysu::transaction_impl * transaction_impl_a);
	bool is_write () const;
	ysu::timer<std::chrono::milliseconds> timer;
	const ysu::transaction_impl * transaction_impl;
	std::string thread_name;

	// Smart pointer so that we don't need the full definition which causes min/max issues on Windows
	std::shared_ptr<boost::stacktrace::stacktrace> stacktrace;
};

class mdb_txn_tracker
{
public:
	mdb_txn_tracker (ysu::logger_mt & logger_a, ysu::txn_tracking_config const & txn_tracking_config_a, std::chrono::milliseconds block_processor_batch_max_time_a);
	void serialize_json (boost::property_tree::ptree & json, std::chrono::milliseconds min_read_time, std::chrono::milliseconds min_write_time);
	void add (const ysu::transaction_impl * transaction_impl);
	void erase (const ysu::transaction_impl * transaction_impl);

private:
	std::mutex mutex;
	std::vector<mdb_txn_stats> stats;
	ysu::logger_mt & logger;
	ysu::txn_tracking_config txn_tracking_config;
	std::chrono::milliseconds block_processor_batch_max_time;

	void log_if_held_long_enough (ysu::mdb_txn_stats const & mdb_txn_stats) const;
};
}
