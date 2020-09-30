#include <ysu/lib/memory.hpp>
#include <ysu/secure/common.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace
{
/** This allocator records the size of all allocations that happen */
template <class T>
class record_allocations_new_delete_allocator
{
public:
	using value_type = T;

	explicit record_allocations_new_delete_allocator (std::vector<size_t> * allocated) :
	allocated (allocated)
	{
	}

	template <typename U>
	record_allocations_new_delete_allocator (const record_allocations_new_delete_allocator<U> & a)
	{
		allocated = a.allocated;
	}

	template <typename U>
	record_allocations_new_delete_allocator & operator= (const record_allocations_new_delete_allocator<U> &) = delete;

	T * allocate (size_t num_to_allocate)
	{
		auto size_allocated = (sizeof (T) * num_to_allocate);
		allocated->push_back (size_allocated);
		return static_cast<T *> (::operator new (size_allocated));
	}

	void deallocate (T * p, size_t num_to_deallocate)
	{
		::operator delete (p);
	}

	std::vector<size_t> * allocated;
};

template <typename T>
size_t get_allocated_size ()
{
	std::vector<size_t> allocated;
	record_allocations_new_delete_allocator<T> alloc (&allocated);
	(void)std::allocate_shared<T, record_allocations_new_delete_allocator<T>> (alloc);
	debug_assert (allocated.size () == 1);
	return allocated.front ();
}
}

TEST (memory_pool, validate_cleanup)
{
	// This might be turned off, e.g on Mac for instance, so don't do this test
	if (!ysu::get_use_memory_pools ())
	{
		return;
	}

	ysu::make_shared<ysu::open_block> ();
	ysu::make_shared<ysu::receive_block> ();
	ysu::make_shared<ysu::send_block> ();
	ysu::make_shared<ysu::change_block> ();
	ysu::make_shared<ysu::state_block> ();
	ysu::make_shared<ysu::vote> ();

	ASSERT_TRUE (ysu::purge_singleton_pool_memory<ysu::open_block> ());
	ASSERT_TRUE (ysu::purge_singleton_pool_memory<ysu::receive_block> ());
	ASSERT_TRUE (ysu::purge_singleton_pool_memory<ysu::send_block> ());
	ASSERT_TRUE (ysu::purge_singleton_pool_memory<ysu::state_block> ());
	ASSERT_TRUE (ysu::purge_singleton_pool_memory<ysu::vote> ());

	// Change blocks have the same size as open_block so won't deallocate any memory
	ASSERT_FALSE (ysu::purge_singleton_pool_memory<ysu::change_block> ());

	ASSERT_EQ (ysu::determine_shared_ptr_pool_size<ysu::open_block> (), get_allocated_size<ysu::open_block> () - sizeof (size_t));
	ASSERT_EQ (ysu::determine_shared_ptr_pool_size<ysu::receive_block> (), get_allocated_size<ysu::receive_block> () - sizeof (size_t));
	ASSERT_EQ (ysu::determine_shared_ptr_pool_size<ysu::send_block> (), get_allocated_size<ysu::send_block> () - sizeof (size_t));
	ASSERT_EQ (ysu::determine_shared_ptr_pool_size<ysu::change_block> (), get_allocated_size<ysu::change_block> () - sizeof (size_t));
	ASSERT_EQ (ysu::determine_shared_ptr_pool_size<ysu::state_block> (), get_allocated_size<ysu::state_block> () - sizeof (size_t));
	ASSERT_EQ (ysu::determine_shared_ptr_pool_size<ysu::vote> (), get_allocated_size<ysu::vote> () - sizeof (size_t));
}
