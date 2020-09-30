#pragma once

#include <ysu/lib/numbers.hpp>
#include <ysu/lib/utility.hpp>
#include <ysu/node/active_transactions.hpp>
#include <ysu/node/transport/transport.hpp>

namespace ysu
{
class telemetry;
class node_observers final
{
public:
	using blocks_t = ysu::observer_set<ysu::election_status const &, ysu::account const &, ysu::uint128_t const &, bool>;
	blocks_t blocks;
	ysu::observer_set<bool> wallet;
	ysu::observer_set<std::shared_ptr<ysu::vote>, std::shared_ptr<ysu::transport::channel>, ysu::vote_code> vote;
	ysu::observer_set<ysu::block_hash const &> active_stopped;
	ysu::observer_set<ysu::account const &, bool> account_balance;
	ysu::observer_set<std::shared_ptr<ysu::transport::channel>> endpoint;
	ysu::observer_set<> disconnect;
	ysu::observer_set<uint64_t> difficulty;
	ysu::observer_set<ysu::root const &> work_cancel;
	ysu::observer_set<ysu::telemetry_data const &, ysu::endpoint const &> telemetry;
};

std::unique_ptr<container_info_component> collect_container_info (node_observers & node_observers, const std::string & name);
}
