#pragma once

#include <ysu/node/node_observers.hpp>

namespace ysu
{
class json_payment_observer;

class payment_observer_processor final
{
public:
	explicit payment_observer_processor (ysu::node_observers::blocks_t & blocks);
	void observer_action (ysu::account const & account_a);
	void add (ysu::account const & account_a, std::shared_ptr<ysu::json_payment_observer> payment_observer_a);
	void erase (ysu::account & account_a);

private:
	std::mutex mutex;
	std::unordered_map<ysu::account, std::shared_ptr<ysu::json_payment_observer>> payment_observers;
};
}
