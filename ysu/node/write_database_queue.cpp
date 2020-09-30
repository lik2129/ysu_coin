#include <ysu/lib/config.hpp>
#include <ysu/lib/utility.hpp>
#include <ysu/node/write_database_queue.hpp>

#include <algorithm>

ysu::write_guard::write_guard (std::function<void()> guard_finish_callback_a) :
guard_finish_callback (guard_finish_callback_a)
{
}

ysu::write_guard::write_guard (ysu::write_guard && write_guard_a) noexcept :
guard_finish_callback (std::move (write_guard_a.guard_finish_callback)),
owns (write_guard_a.owns)
{
	write_guard_a.owns = false;
	write_guard_a.guard_finish_callback = nullptr;
}

ysu::write_guard & ysu::write_guard::operator= (ysu::write_guard && write_guard_a) noexcept
{
	owns = write_guard_a.owns;
	guard_finish_callback = std::move (write_guard_a.guard_finish_callback);

	write_guard_a.owns = false;
	write_guard_a.guard_finish_callback = nullptr;
	return *this;
}

ysu::write_guard::~write_guard ()
{
	if (owns)
	{
		guard_finish_callback ();
	}
}

bool ysu::write_guard::is_owned () const
{
	return owns;
}

void ysu::write_guard::release ()
{
	debug_assert (owns);
	if (owns)
	{
		guard_finish_callback ();
	}
	owns = false;
}

ysu::write_database_queue::write_database_queue (bool use_noops_a) :
guard_finish_callback ([use_noops_a, &queue = queue, &mutex = mutex, &cv = cv]() {
	if (!use_noops_a)
	{
		{
			ysu::lock_guard<std::mutex> guard (mutex);
			queue.pop_front ();
		}
		cv.notify_all ();
	}
}),
use_noops (use_noops_a)
{
}

ysu::write_guard ysu::write_database_queue::wait (ysu::writer writer)
{
	if (use_noops)
	{
		return write_guard ([] {});
	}

	ysu::unique_lock<std::mutex> lk (mutex);
	// Add writer to the end of the queue if it's not already waiting
	auto exists = std::find (queue.cbegin (), queue.cend (), writer) != queue.cend ();
	if (!exists)
	{
		queue.push_back (writer);
	}

	while (queue.front () != writer)
	{
		cv.wait (lk);
	}

	return write_guard (guard_finish_callback);
}

bool ysu::write_database_queue::contains (ysu::writer writer)
{
	debug_assert (!use_noops && ysu::network_constants ().is_dev_network ());
	ysu::lock_guard<std::mutex> guard (mutex);
	return std::find (queue.cbegin (), queue.cend (), writer) != queue.cend ();
}

bool ysu::write_database_queue::process (ysu::writer writer)
{
	if (use_noops)
	{
		return true;
	}

	auto result = false;
	{
		ysu::lock_guard<std::mutex> guard (mutex);
		// Add writer to the end of the queue if it's not already waiting
		auto exists = std::find (queue.cbegin (), queue.cend (), writer) != queue.cend ();
		if (!exists)
		{
			queue.push_back (writer);
		}

		result = (queue.front () == writer);
	}

	if (!result)
	{
		cv.notify_all ();
	}

	return result;
}

ysu::write_guard ysu::write_database_queue::pop ()
{
	return write_guard (guard_finish_callback);
}
