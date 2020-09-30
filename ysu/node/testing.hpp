#pragma once

#include <ysu/lib/errors.hpp>
#include <ysu/node/node.hpp>

#include <chrono>

namespace ysu
{
/** Test-system related error codes */
enum class error_system
{
	generic = 1,
	deadline_expired
};
class system final
{
public:
	system ();
	system (uint16_t, ysu::transport::transport_type = ysu::transport::transport_type::tcp, ysu::node_flags = ysu::node_flags ());
	~system ();
	void generate_activity (ysu::node &, std::vector<ysu::account> &);
	void generate_mass_activity (uint32_t, ysu::node &);
	void generate_usage_traffic (uint32_t, uint32_t, size_t);
	void generate_usage_traffic (uint32_t, uint32_t);
	ysu::account get_random_account (std::vector<ysu::account> &);
	ysu::uint128_t get_random_amount (ysu::transaction const &, ysu::node &, ysu::account const &);
	void generate_rollback (ysu::node &, std::vector<ysu::account> &);
	void generate_change_known (ysu::node &, std::vector<ysu::account> &);
	void generate_change_unknown (ysu::node &, std::vector<ysu::account> &);
	void generate_receive (ysu::node &);
	void generate_send_new (ysu::node &, std::vector<ysu::account> &);
	void generate_send_existing (ysu::node &, std::vector<ysu::account> &);
	std::unique_ptr<ysu::state_block> upgrade_genesis_epoch (ysu::node &, ysu::epoch const);
	std::shared_ptr<ysu::wallet> wallet (size_t);
	ysu::account account (ysu::transaction const &, size_t);
	/** Generate work with difficulty between \p min_difficulty_a (inclusive) and \p max_difficulty_a (exclusive) */
	uint64_t work_generate_limited (ysu::block_hash const & root_a, uint64_t min_difficulty_a, uint64_t max_difficulty_a);
	/**
	 * Polls, sleep if there's no work to be done (default 50ms), then check the deadline
	 * @returns 0 or ysu::deadline_expired
	 */
	std::error_code poll (const std::chrono::nanoseconds & sleep_time = std::chrono::milliseconds (50));
	std::error_code poll_until_true (std::chrono::nanoseconds deadline, std::function<bool()>);
	void stop ();
	void deadline_set (const std::chrono::duration<double, std::nano> & delta);
	std::shared_ptr<ysu::node> add_node (ysu::node_flags = ysu::node_flags (), ysu::transport::transport_type = ysu::transport::transport_type::tcp);
	std::shared_ptr<ysu::node> add_node (ysu::node_config const &, ysu::node_flags = ysu::node_flags (), ysu::transport::transport_type = ysu::transport::transport_type::tcp);
	boost::asio::io_context io_ctx;
	ysu::alarm alarm{ io_ctx };
	std::vector<std::shared_ptr<ysu::node>> nodes;
	ysu::logging logging;
	ysu::work_pool work{ std::max (std::thread::hardware_concurrency (), 1u) };
	std::chrono::time_point<std::chrono::steady_clock, std::chrono::duration<double>> deadline{ std::chrono::steady_clock::time_point::max () };
	double deadline_scaling_factor{ 1.0 };
	unsigned node_sequence{ 0 };
};
std::unique_ptr<ysu::state_block> upgrade_epoch (ysu::work_pool &, ysu::ledger &, ysu::epoch);
void blocks_confirm (ysu::node &, std::vector<std::shared_ptr<ysu::block>> const &);
uint16_t get_available_port ();
void cleanup_dev_directories_on_exit ();
/** To use RocksDB in tests make sure the environment variable TEST_USE_ROCKSDB=1 is set */
bool using_rocksdb_in_tests ();
}
REGISTER_ERROR_CODES (ysu, error_system);
