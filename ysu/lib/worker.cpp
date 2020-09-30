#include <ysu/lib/threading.hpp>
#include <ysu/lib/worker.hpp>

ysu::worker::worker () :
thread ([this]() {
	ysu::thread_role::set (ysu::thread_role::name::worker);
	this->run ();
})
{
}

void ysu::worker::run ()
{
	ysu::unique_lock<std::mutex> lk (mutex);
	while (!stopped)
	{
		if (!queue.empty ())
		{
			auto func = queue.front ();
			queue.pop_front ();
			lk.unlock ();
			func ();
			// So that we reduce locking for anything being pushed as that will
			// most likely be on an io-thread
			std::this_thread::yield ();
			lk.lock ();
		}
		else
		{
			cv.wait (lk);
		}
	}
}

ysu::worker::~worker ()
{
	stop ();
}

void ysu::worker::push_task (std::function<void()> func_a)
{
	{
		ysu::lock_guard<std::mutex> guard (mutex);
		if (!stopped)
		{
			queue.emplace_back (func_a);
		}
	}

	cv.notify_one ();
}

void ysu::worker::stop ()
{
	{
		ysu::unique_lock<std::mutex> lk (mutex);
		stopped = true;
		queue.clear ();
	}
	cv.notify_one ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (ysu::worker & worker, const std::string & name)
{
	size_t count;
	{
		ysu::lock_guard<std::mutex> guard (worker.mutex);
		count = worker.queue.size ();
	}
	auto sizeof_element = sizeof (decltype (worker.queue)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<ysu::container_info_leaf> (ysu::container_info{ "queue", count, sizeof_element }));
	return composite;
}
