#pragma once

#include <ysu/lib/numbers.hpp>

#include <type_traits>
#include <unordered_map>

namespace ysu
{
/**
 * Tag for which epoch an entry belongs to
 */
enum class epoch : uint8_t
{
	invalid = 0,
	unspecified = 1,
	epoch_begin = 2,
	epoch_0 = 2,
	epoch_1 = 3,
	epoch_2 = 4,
	max = epoch_2
};

/* This turns epoch_0 into 0 for instance */
std::underlying_type_t<ysu::epoch> normalized_epoch (ysu::epoch epoch_a);
}
namespace std
{
template <>
struct hash<::ysu::epoch>
{
	std::size_t operator() (::ysu::epoch const & epoch_a) const
	{
		std::hash<std::underlying_type_t<::ysu::epoch>> hash;
		return hash (static_cast<std::underlying_type_t<::ysu::epoch>> (epoch_a));
	}
};
}
namespace ysu
{
class epoch_info
{
public:
	ysu::public_key signer;
	ysu::link link;
};
class epochs
{
public:
	bool is_epoch_link (ysu::link const & link_a) const;
	ysu::link const & link (ysu::epoch epoch_a) const;
	ysu::public_key const & signer (ysu::epoch epoch_a) const;
	ysu::epoch epoch (ysu::link const & link_a) const;
	void add (ysu::epoch epoch_a, ysu::public_key const & signer_a, ysu::link const & link_a);
	/** Checks that new_epoch is 1 version higher than epoch */
	static bool is_sequential (ysu::epoch epoch_a, ysu::epoch new_epoch_a);

private:
	std::unordered_map<ysu::epoch, ysu::epoch_info> epochs_m;
};
}
