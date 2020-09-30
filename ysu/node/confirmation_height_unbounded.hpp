#pragma once

#include <ysu/lib/numbers.hpp>
#include <ysu/lib/threading.hpp>
#include <ysu/lib/timer.hpp>
#include <ysu/secure/blockstore.hpp>

#include <chrono>
#include <unordered_map>

namespace ysu
{
class ledger;
class read_transaction;
class logging;
class logger_mt;
class write_database_queue;
class write_guard;

class confirmation_height_unbounded final
{
public:
	confirmation_height_unbounded (ysu::ledger &, ysu::write_database_queue &, std::chrono::milliseconds, ysu::logging const &, ysu::logger_mt &, std::atomic<bool> &, ysu::block_hash const &, uint64_t &, std::function<void(std::vector<std::shared_ptr<ysu::block>> const &)> const &, std::function<void(ysu::block_hash const &)> const &, std::function<uint64_t ()> const &);
	bool pending_empty () const;
	void clear_process_vars ();
	void process ();
	void cement_blocks (ysu::write_guard &);
	bool has_iterated_over_block (ysu::block_hash const &) const;

private:
	class confirmed_iterated_pair
	{
	public:
		confirmed_iterated_pair (uint64_t confirmed_height_a, uint64_t iterated_height_a);
		uint64_t confirmed_height;
		uint64_t iterated_height;
	};

	class conf_height_details final
	{
	public:
		conf_height_details (ysu::account const &, ysu::block_hash const &, uint64_t, uint64_t, std::vector<ysu::block_hash> const &);

		ysu::account account;
		ysu::block_hash hash;
		uint64_t height;
		uint64_t num_blocks_confirmed;
		std::vector<ysu::block_hash> block_callback_data;
		std::vector<ysu::block_hash> source_block_callback_data;
	};

	class receive_source_pair final
	{
	public:
		receive_source_pair (std::shared_ptr<conf_height_details> const &, const ysu::block_hash &);

		std::shared_ptr<conf_height_details> receive_details;
		ysu::block_hash source_hash;
	};

	// All of the atomic variables here just track the size for use in collect_container_info.
	// This is so that no mutexes are needed during the algorithm itself, which would otherwise be needed
	// for the sake of a rarely used RPC call for debugging purposes. As such the sizes are not being acted
	// upon in any way (does not synchronize with any other data).
	// This allows the load and stores to use relaxed atomic memory ordering.
	std::unordered_map<account, confirmed_iterated_pair> confirmed_iterated_pairs;
	ysu::relaxed_atomic_integral<uint64_t> confirmed_iterated_pairs_size{ 0 };
	std::shared_ptr<ysu::block> get_block_and_sideband (ysu::block_hash const &, ysu::transaction const &);
	std::deque<conf_height_details> pending_writes;
	ysu::relaxed_atomic_integral<uint64_t> pending_writes_size{ 0 };
	std::unordered_map<ysu::block_hash, std::weak_ptr<conf_height_details>> implicit_receive_cemented_mapping;
	ysu::relaxed_atomic_integral<uint64_t> implicit_receive_cemented_mapping_size{ 0 };

	mutable std::mutex block_cache_mutex;
	std::unordered_map<ysu::block_hash, std::shared_ptr<ysu::block>> block_cache;
	uint64_t block_cache_size () const;

	ysu::timer<std::chrono::milliseconds> timer;

	class preparation_data final
	{
	public:
		uint64_t block_height;
		uint64_t confirmation_height;
		uint64_t iterated_height;
		decltype (confirmed_iterated_pairs.begin ()) account_it;
		ysu::account const & account;
		std::shared_ptr<conf_height_details> receive_details;
		bool already_traversed;
		ysu::block_hash const & current;
		std::vector<ysu::block_hash> const & block_callback_data;
		std::vector<ysu::block_hash> const & orig_block_callback_data;
	};

	void collect_unconfirmed_receive_and_sources_for_account (uint64_t, uint64_t, ysu::block_hash const &, ysu::account const &, ysu::read_transaction const &, std::vector<receive_source_pair> &, std::vector<ysu::block_hash> &, std::vector<ysu::block_hash> &);
	void prepare_iterated_blocks_for_cementing (preparation_data &);

	ysu::network_params network_params;
	ysu::ledger & ledger;
	ysu::write_database_queue & write_database_queue;
	std::chrono::milliseconds batch_separate_pending_min_time;
	ysu::logger_mt & logger;
	std::atomic<bool> & stopped;
	ysu::block_hash const & original_hash;
	uint64_t & batch_write_size;
	ysu::logging const & logging;

	std::function<void(std::vector<std::shared_ptr<ysu::block>> const &)> notify_observers_callback;
	std::function<void(ysu::block_hash const &)> notify_block_already_cemented_observers_callback;
	std::function<uint64_t ()> awaiting_processing_size_callback;

	friend class confirmation_height_dynamic_algorithm_no_transition_while_pending_Test;
	friend std::unique_ptr<ysu::container_info_component> collect_container_info (confirmation_height_unbounded &, const std::string & name_a);
};

std::unique_ptr<ysu::container_info_component> collect_container_info (confirmation_height_unbounded &, const std::string & name_a);
}
