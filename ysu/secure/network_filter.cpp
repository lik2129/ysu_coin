#include <ysu/crypto_lib/random_pool.hpp>
#include <ysu/lib/locks.hpp>
#include <ysu/secure/buffer.hpp>
#include <ysu/secure/common.hpp>
#include <ysu/secure/network_filter.hpp>

ysu::network_filter::network_filter (size_t size_a) :
items (size_a, ysu::uint128_t{ 0 })
{
	ysu::random_pool::generate_block (key, key.size ());
}

bool ysu::network_filter::apply (uint8_t const * bytes_a, size_t count_a, ysu::uint128_t * digest_a)
{
	// Get hash before locking
	auto digest (hash (bytes_a, count_a));

	ysu::lock_guard<std::mutex> lock (mutex);
	auto & element (get_element (digest));
	bool existed (element == digest);
	if (!existed)
	{
		// Replace likely old element with a new one
		element = digest;
	}
	if (digest_a)
	{
		*digest_a = digest;
	}
	return existed;
}

void ysu::network_filter::clear (ysu::uint128_t const & digest_a)
{
	ysu::lock_guard<std::mutex> lock (mutex);
	auto & element (get_element (digest_a));
	if (element == digest_a)
	{
		element = ysu::uint128_t{ 0 };
	}
}

void ysu::network_filter::clear (std::vector<ysu::uint128_t> const & digests_a)
{
	ysu::lock_guard<std::mutex> lock (mutex);
	for (auto const & digest : digests_a)
	{
		auto & element (get_element (digest));
		if (element == digest)
		{
			element = ysu::uint128_t{ 0 };
		}
	}
}

void ysu::network_filter::clear (uint8_t const * bytes_a, size_t count_a)
{
	clear (hash (bytes_a, count_a));
}

template <typename OBJECT>
void ysu::network_filter::clear (OBJECT const & object_a)
{
	clear (hash (object_a));
}

void ysu::network_filter::clear ()
{
	ysu::lock_guard<std::mutex> lock (mutex);
	items.assign (items.size (), ysu::uint128_t{ 0 });
}

template <typename OBJECT>
ysu::uint128_t ysu::network_filter::hash (OBJECT const & object_a) const
{
	std::vector<uint8_t> bytes;
	{
		ysu::vectorstream stream (bytes);
		object_a->serialize (stream);
	}
	return hash (bytes.data (), bytes.size ());
}

ysu::uint128_t & ysu::network_filter::get_element (ysu::uint128_t const & hash_a)
{
	debug_assert (!mutex.try_lock ());
	debug_assert (items.size () > 0);
	size_t index (hash_a % items.size ());
	return items[index];
}

ysu::uint128_t ysu::network_filter::hash (uint8_t const * bytes_a, size_t count_a) const
{
	ysu::uint128_union digest{ 0 };
	siphash_t siphash (key, static_cast<unsigned int> (key.size ()));
	siphash.CalculateDigest (digest.bytes.data (), bytes_a, count_a);
	return digest.number ();
}

// Explicitly instantiate
template ysu::uint128_t ysu::network_filter::hash (std::shared_ptr<ysu::block> const &) const;
template void ysu::network_filter::clear (std::shared_ptr<ysu::block> const &);
