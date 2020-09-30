#include <ysu/node/json_payment_observer.hpp>
#include <ysu/node/payment_observer_processor.hpp>

ysu::payment_observer_processor::payment_observer_processor (ysu::node_observers::blocks_t & blocks)
{
	blocks.add ([this](ysu::election_status const &, ysu::account const & account_a, ysu::uint128_t const &, bool) {
		observer_action (account_a);
	});
}

void ysu::payment_observer_processor::observer_action (ysu::account const & account_a)
{
	std::shared_ptr<ysu::json_payment_observer> observer;
	{
		ysu::lock_guard<std::mutex> lock (mutex);
		auto existing (payment_observers.find (account_a));
		if (existing != payment_observers.end ())
		{
			observer = existing->second;
		}
	}
	if (observer != nullptr)
	{
		observer->observe ();
	}
}

void ysu::payment_observer_processor::add (ysu::account const & account_a, std::shared_ptr<ysu::json_payment_observer> payment_observer_a)
{
	ysu::lock_guard<std::mutex> lock (mutex);
	debug_assert (payment_observers.find (account_a) == payment_observers.end ());
	payment_observers[account_a] = payment_observer_a;
}

void ysu::payment_observer_processor::erase (ysu::account & account_a)
{
	ysu::lock_guard<std::mutex> lock (mutex);
	debug_assert (payment_observers.find (account_a) != payment_observers.end ());
	payment_observers.erase (account_a);
}
