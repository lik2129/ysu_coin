#include <ysu/lib/locks.hpp>
#include <ysu/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <future>
#include <regex>

#if YSU_TIMED_LOCKS > 0
namespace
{
unsigned num_matches (std::string const & str)
{
	std::regex regexpr (R"(( \d+)ms)"); // matches things like " 12312ms"
	std::smatch matches;

	auto count = 0u;
	std::string::const_iterator search_start (str.cbegin ());
	while (std::regex_search (search_start, str.cend (), matches, regexpr))
	{
		++count;
		search_start = matches.suffix ().first;
	}
	return count;
}
}

TEST (locks, no_conflicts)
{
	std::stringstream ss;
	ysu::cout_redirect (ss.rdbuf ());

	std::mutex guard_mutex;
	ysu::lock_guard<std::mutex> guard (guard_mutex);

	std::mutex lk_mutex;
	ysu::unique_lock<std::mutex> lk (lk_mutex);

	// This could fail if YSU_TIMED_LOCKS is such a low value that the above mutexes are held longer than that before reaching this statement
	ASSERT_EQ (ss.str (), "");
}

TEST (locks, lock_guard)
{
	// This test can end up taking a long time, as it sleeps for the YSU_TIMED_LOCKS amount
	ASSERT_LE (YSU_TIMED_LOCKS, 10000);

	std::stringstream ss;
	ysu::cout_redirect redirect (ss.rdbuf ());

	std::mutex mutex;

	// Depending on timing the mutex could be reached first in
	std::promise<void> promise;
	std::thread t;
	{
		t = std::thread ([&mutex, &promise] {
			ysu::lock_guard<std::mutex> guard (mutex);
			promise.set_value ();
			// Tries to make sure that the other guard to held for a minimum of YSU_TIMED_LOCKS, may need to increase this for low YSU_TIMED_LOCKS values
			std::this_thread::sleep_for (std::chrono::milliseconds (YSU_TIMED_LOCKS * 2));
		});
	}

	// Wait until the lock_guard has been reached in the other thread
	promise.get_future ().wait ();
	{
		ysu::lock_guard<std::mutex> guard (mutex);
		t.join ();
	}

	// 2 mutexes held and 1 blocked
	ASSERT_EQ (num_matches (ss.str ()), 3);
}

TEST (locks, unique_lock)
{
	// This test can end up taking a long time, as it sleeps for the YSU_TIMED_LOCKS amount
	ASSERT_LE (YSU_TIMED_LOCKS, 10000);

	std::stringstream ss;
	ysu::cout_redirect redirect (ss.rdbuf ());

	std::mutex mutex;

	// Depending on timing the mutex could be reached first in
	std::promise<void> promise;
	std::thread t ([&mutex, &promise] {
		ysu::unique_lock<std::mutex> lk (mutex);
		std::this_thread::sleep_for (std::chrono::milliseconds (YSU_TIMED_LOCKS));
		lk.unlock ();
		lk.lock ();

		promise.set_value ();
		// Tries to make sure that the other guard is held for a minimum of YSU_TIMED_LOCKS, may need to increase this for low YSU_TIMED_LOCKS values
		std::this_thread::sleep_for (std::chrono::milliseconds (YSU_TIMED_LOCKS * 2));
	});

	// Wait until the lock_guard has been reached in the other thread
	promise.get_future ().wait ();
	{
		ysu::unique_lock<std::mutex> lk (mutex);
		t.join ();
	}

	// 3 mutexes held and 1 blocked
	ASSERT_EQ (num_matches (ss.str ()), 4);
}

TEST (locks, condition_variable_wait)
{
	// This test can end up taking a long time, as it sleeps for the YSU_TIMED_LOCKS amount
	ASSERT_LE (YSU_TIMED_LOCKS, 10000);

	std::stringstream ss;
	ysu::cout_redirect redirect (ss.rdbuf ());

	ysu::condition_variable cv;
	std::mutex mutex;
	std::atomic<bool> notified{ false };
	std::atomic<bool> finished{ false };
	std::thread t ([&] {
		std::this_thread::sleep_for (std::chrono::milliseconds (YSU_TIMED_LOCKS * 2));
		while (!finished)
		{
			notified = true;
			cv.notify_one ();
		}
	});

	ysu::unique_lock<std::mutex> lk (mutex);
	std::this_thread::sleep_for (std::chrono::milliseconds (YSU_TIMED_LOCKS));
	cv.wait (lk, [&notified] {
		return notified.load ();
	});
	finished = true;

	t.join ();
	// 1 mutex held
	ASSERT_EQ (num_matches (ss.str ()), 1);
}

TEST (locks, condition_variable_wait_until)
{
	// This test can end up taking a long time, as it sleeps for the YSU_TIMED_LOCKS amount
	ASSERT_LE (YSU_TIMED_LOCKS, 10000);

	std::stringstream ss;
	ysu::cout_redirect redirect (ss.rdbuf ());

	ysu::condition_variable cv;
	std::mutex mutex;
	auto impl = [&](auto time_to_sleep) {
		std::atomic<bool> notified{ false };
		std::atomic<bool> finished{ false };
		ysu::unique_lock<std::mutex> lk (mutex);
		std::this_thread::sleep_for (std::chrono::milliseconds (time_to_sleep));
		std::thread t ([&] {
			while (!finished)
			{
				notified = true;
				cv.notify_one ();
			}
		});

		cv.wait_until (lk, std::chrono::steady_clock::now () + std::chrono::milliseconds (YSU_TIMED_LOCKS), [&notified] {
			return notified.load ();
		});
		finished = true;
		lk.unlock ();
		t.join ();
	};

	impl (0);
	// wait_until should not report any stacktraces
	ASSERT_EQ (num_matches (ss.str ()), 0);
	impl (YSU_TIMED_LOCKS);
	// Should be 1 report
	ASSERT_EQ (num_matches (ss.str ()), 1);
}

TEST (locks, defer_lock)
{
	std::mutex mutex;
	ysu::unique_lock<std::mutex> lock (mutex, std::defer_lock);
	ASSERT_FALSE (lock.owns_lock ());
	ASSERT_TRUE (lock.try_lock ());
	ASSERT_TRUE (lock.owns_lock ());
	lock.unlock ();
	ASSERT_FALSE (lock.owns_lock ());
}
#endif