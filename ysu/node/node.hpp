#pragma once

#include <ysu/boost/asio/spawn.hpp>
#include <ysu/lib/alarm.hpp>
#include <ysu/lib/config.hpp>
#include <ysu/lib/stats.hpp>
#include <ysu/lib/work.hpp>
#include <ysu/lib/worker.hpp>
#include <ysu/node/active_transactions.hpp>
#include <ysu/node/blockprocessor.hpp>
#include <ysu/node/bootstrap/bootstrap.hpp>
#include <ysu/node/bootstrap/bootstrap_attempt.hpp>
#include <ysu/node/bootstrap/bootstrap_server.hpp>
#include <ysu/node/confirmation_height_processor.hpp>
#include <ysu/node/distributed_work_factory.hpp>
#include <ysu/node/election.hpp>
#include <ysu/node/gap_cache.hpp>
#include <ysu/node/network.hpp>
#include <ysu/node/node_observers.hpp>
#include <ysu/node/nodeconfig.hpp>
#include <ysu/node/online_reps.hpp>
#include <ysu/node/payment_observer_processor.hpp>
#include <ysu/node/portmapping.hpp>
#include <ysu/node/repcrawler.hpp>
#include <ysu/node/request_aggregator.hpp>
#include <ysu/node/signatures.hpp>
#include <ysu/node/telemetry.hpp>
#include <ysu/node/vote_processor.hpp>
#include <ysu/node/wallet.hpp>
#include <ysu/node/write_database_queue.hpp>
#include <ysu/secure/ledger.hpp>
#include <ysu/secure/utility.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/program_options.hpp>
#include <boost/thread/latch.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace ysu
{
namespace websocket
{
	class listener;
}
class node;
class telemetry;
class work_pool;
class block_arrival_info final
{
public:
	std::chrono::steady_clock::time_point arrival;
	ysu::block_hash hash;
};
// This class tracks blocks that are probably live because they arrived in a UDP packet
// This gives a fairly reliable way to differentiate between blocks being inserted via bootstrap or new, live blocks.
class block_arrival final
{
public:
	// Return `true' to indicated an error if the block has already been inserted
	bool add (ysu::block_hash const &);
	bool recent (ysu::block_hash const &);
	// clang-format off
	class tag_sequence {};
	class tag_hash {};
	boost::multi_index_container<ysu::block_arrival_info,
		boost::multi_index::indexed_by<
			boost::multi_index::sequenced<boost::multi_index::tag<tag_sequence>>,
			boost::multi_index::hashed_unique<boost::multi_index::tag<tag_hash>,
				boost::multi_index::member<ysu::block_arrival_info, ysu::block_hash, &ysu::block_arrival_info::hash>>>>
	arrival;
	// clang-format on
	std::mutex mutex;
	static size_t constexpr arrival_size_min = 8 * 1024;
	static std::chrono::seconds constexpr arrival_time_min = std::chrono::seconds (300);
};

std::unique_ptr<container_info_component> collect_container_info (block_arrival & block_arrival, const std::string & name);

std::unique_ptr<container_info_component> collect_container_info (rep_crawler & rep_crawler, const std::string & name);

class node final : public std::enable_shared_from_this<ysu::node>
{
public:
	node (boost::asio::io_context &, uint16_t, boost::filesystem::path const &, ysu::alarm &, ysu::logging const &, ysu::work_pool &, ysu::node_flags = ysu::node_flags (), unsigned seq = 0);
	node (boost::asio::io_context &, boost::filesystem::path const &, ysu::alarm &, ysu::node_config const &, ysu::work_pool &, ysu::node_flags = ysu::node_flags (), unsigned seq = 0);
	~node ();
	template <typename T>
	void background (T action_a)
	{
		alarm.io_ctx.post (action_a);
	}
	template <typename... Params>
	void spawn (Params... args)
	{
		boost::coroutines::attributes attributes{ boost::coroutines::stack_allocator::traits_type::default_size () * (is_sanitizer_build ? 2 : 1) };
		boost::asio::spawn (io_ctx, std::forward<Params> (args)..., attributes);
	}
	bool copy_with_compaction (boost::filesystem::path const &);
	void keepalive (std::string const &, uint16_t);
	void start ();
	void stop ();
	std::shared_ptr<ysu::node> shared ();
	int store_version ();
	void receive_confirmed (ysu::transaction const & wallet_transaction_a, ysu::transaction const & block_transaction_a, std::shared_ptr<ysu::block> const &, ysu::block_hash const &);
	void process_confirmed_data (ysu::transaction const &, std::shared_ptr<ysu::block>, ysu::block_hash const &, ysu::account &, ysu::uint128_t &, bool &, ysu::account &);
	void process_confirmed (ysu::election_status const &, uint64_t = 0);
	void process_active (std::shared_ptr<ysu::block>);
	ysu::process_return process (ysu::block &);
	ysu::process_return process_local (std::shared_ptr<ysu::block>, bool const = false);
	void keepalive_preconfigured (std::vector<std::string> const &);
	ysu::block_hash latest (ysu::account const &);
	ysu::uint128_t balance (ysu::account const &);
	std::shared_ptr<ysu::block> block (ysu::block_hash const &);
	std::pair<ysu::uint128_t, ysu::uint128_t> balance_pending (ysu::account const &);
	ysu::uint128_t weight (ysu::account const &);
	ysu::block_hash rep_block (ysu::account const &);
	ysu::uint128_t minimum_principal_weight ();
	ysu::uint128_t minimum_principal_weight (ysu::uint128_t const &);
	void ongoing_rep_calculation ();
	void ongoing_bootstrap ();
	void ongoing_store_flush ();
	void ongoing_peer_store ();
	void ongoing_unchecked_cleanup ();
	void backup_wallet ();
	void search_pending ();
	void bootstrap_wallet ();
	void unchecked_cleanup ();
	int price (ysu::uint128_t const &, int);
	// The default difficulty updates to base only when the first epoch_2 block is processed
	uint64_t default_difficulty (ysu::work_version const) const;
	uint64_t default_receive_difficulty (ysu::work_version const) const;
	uint64_t max_work_generate_difficulty (ysu::work_version const) const;
	bool local_work_generation_enabled () const;
	bool work_generation_enabled () const;
	bool work_generation_enabled (std::vector<std::pair<std::string, uint16_t>> const &) const;
	boost::optional<uint64_t> work_generate_blocking (ysu::block &, uint64_t);
	boost::optional<uint64_t> work_generate_blocking (ysu::work_version const, ysu::root const &, uint64_t, boost::optional<ysu::account> const & = boost::none);
	void work_generate (ysu::work_version const, ysu::root const &, uint64_t, std::function<void(boost::optional<uint64_t>)>, boost::optional<ysu::account> const & = boost::none, bool const = false);
	void add_initial_peers ();
	void block_confirm (std::shared_ptr<ysu::block>);
	bool block_confirmed (ysu::block_hash const &);
	bool block_confirmed_or_being_confirmed (ysu::transaction const &, ysu::block_hash const &);
	void process_fork (ysu::transaction const &, std::shared_ptr<ysu::block>, uint64_t);
	void do_rpc_callback (boost::asio::ip::tcp::resolver::iterator i_a, std::string const &, uint16_t, std::shared_ptr<std::string>, std::shared_ptr<std::string>, std::shared_ptr<boost::asio::ip::tcp::resolver>);
	ysu::uint128_t delta () const;
	void ongoing_online_weight_calculation ();
	void ongoing_online_weight_calculation_queue ();
	bool online () const;
	bool init_error () const;
	bool epoch_upgrader (ysu::private_key const &, ysu::epoch, uint64_t, uint64_t);
	std::pair<uint64_t, decltype (ysu::ledger::bootstrap_weights)> get_bootstrap_weights () const;
	ysu::worker worker;
	ysu::write_database_queue write_database_queue;
	boost::asio::io_context & io_ctx;
	boost::latch node_initialized_latch;
	ysu::network_params network_params;
	ysu::node_config config;
	ysu::stat stats;
	std::shared_ptr<ysu::websocket::listener> websocket_server;
	ysu::node_flags flags;
	ysu::alarm & alarm;
	ysu::work_pool & work;
	ysu::distributed_work_factory distributed_work;
	ysu::logger_mt logger;
	std::unique_ptr<ysu::block_store> store_impl;
	ysu::block_store & store;
	std::unique_ptr<ysu::wallets_store> wallets_store_impl;
	ysu::wallets_store & wallets_store;
	ysu::gap_cache gap_cache;
	ysu::ledger ledger;
	ysu::signature_checker checker;
	ysu::network network;
	std::shared_ptr<ysu::telemetry> telemetry;
	ysu::bootstrap_initiator bootstrap_initiator;
	ysu::bootstrap_listener bootstrap;
	boost::filesystem::path application_path;
	ysu::node_observers observers;
	ysu::port_mapping port_mapping;
	ysu::vote_processor vote_processor;
	ysu::rep_crawler rep_crawler;
	unsigned warmed_up;
	ysu::block_processor block_processor;
	std::thread block_processor_thread;
	ysu::block_arrival block_arrival;
	ysu::online_reps online_reps;
	ysu::local_vote_history history;
	ysu::keypair node_id;
	ysu::block_uniquer block_uniquer;
	ysu::vote_uniquer vote_uniquer;
	ysu::confirmation_height_processor confirmation_height_processor;
	ysu::active_transactions active;
	ysu::request_aggregator aggregator;
	ysu::payment_observer_processor payment_observer_processor;
	ysu::wallets wallets;
	const std::chrono::steady_clock::time_point startup_time;
	std::chrono::seconds unchecked_cutoff = std::chrono::seconds (7 * 24 * 60 * 60); // Week
	std::atomic<bool> unresponsive_work_peers{ false };
	std::atomic<bool> stopped{ false };
	static double constexpr price_max = 16.0;
	static double constexpr free_cutoff = 1024.0;
	// For tests only
	unsigned node_seq;
	// For tests only
	boost::optional<uint64_t> work_generate_blocking (ysu::block &);
	// For tests only
	boost::optional<uint64_t> work_generate_blocking (ysu::root const &, uint64_t);
	// For tests only
	boost::optional<uint64_t> work_generate_blocking (ysu::root const &);

private:
	void long_inactivity_cleanup ();
	void epoch_upgrader_impl (ysu::private_key const &, ysu::epoch, uint64_t, uint64_t);
	ysu::locked<std::future<void>> epoch_upgrading;
};

std::unique_ptr<container_info_component> collect_container_info (node & node, const std::string & name);

ysu::node_flags const & inactive_node_flag_defaults ();

class inactive_node final
{
public:
	inactive_node (boost::filesystem::path const & path_a, ysu::node_flags const & node_flags_a);
	~inactive_node ();
	std::shared_ptr<boost::asio::io_context> io_context;
	ysu::alarm alarm;
	ysu::work_pool work;
	std::shared_ptr<ysu::node> node;
};
std::unique_ptr<ysu::inactive_node> default_inactive_node (boost::filesystem::path const &, boost::program_options::variables_map const &);
}
