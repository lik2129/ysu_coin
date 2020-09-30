#pragma once

#include <ysu/node/node_observers.hpp>

#include <string>

namespace ysu
{
class node;

enum class payment_status
{
	not_a_status,
	unknown,
	nothing, // Timeout and nothing was received
	//insufficient, // Timeout and not enough was received
	//over, // More than requested received
	//success_fork, // Amount received but it involved a fork
	success // Amount received
};
class json_payment_observer final : public std::enable_shared_from_this<ysu::json_payment_observer>
{
public:
	json_payment_observer (ysu::node &, std::function<void(std::string const &)> const &, ysu::account const &, ysu::amount const &);
	void start (uint64_t);
	void observe ();
	void complete (ysu::payment_status);
	std::mutex mutex;
	ysu::condition_variable condition;
	ysu::node & node;
	ysu::account account;
	ysu::amount amount;
	std::function<void(std::string const &)> response;
	std::atomic_flag completed;
};
}
