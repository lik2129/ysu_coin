#include <ysu/lib/epoch.hpp>
#include <ysu/lib/utility.hpp>

ysu::link const & ysu::epochs::link (ysu::epoch epoch_a) const
{
	return epochs_m.at (epoch_a).link;
}

bool ysu::epochs::is_epoch_link (ysu::link const & link_a) const
{
	return std::any_of (epochs_m.begin (), epochs_m.end (), [&link_a](auto const & item_a) { return item_a.second.link == link_a; });
}

ysu::public_key const & ysu::epochs::signer (ysu::epoch epoch_a) const
{
	return epochs_m.at (epoch_a).signer;
}

ysu::epoch ysu::epochs::epoch (ysu::link const & link_a) const
{
	auto existing (std::find_if (epochs_m.begin (), epochs_m.end (), [&link_a](auto const & item_a) { return item_a.second.link == link_a; }));
	debug_assert (existing != epochs_m.end ());
	return existing->first;
}

void ysu::epochs::add (ysu::epoch epoch_a, ysu::public_key const & signer_a, ysu::link const & link_a)
{
	debug_assert (epochs_m.find (epoch_a) == epochs_m.end ());
	epochs_m[epoch_a] = { signer_a, link_a };
}

bool ysu::epochs::is_sequential (ysu::epoch epoch_a, ysu::epoch new_epoch_a)
{
	auto head_epoch = std::underlying_type_t<ysu::epoch> (epoch_a);
	bool is_valid_epoch (head_epoch >= std::underlying_type_t<ysu::epoch> (ysu::epoch::epoch_0));
	return is_valid_epoch && (std::underlying_type_t<ysu::epoch> (new_epoch_a) == (head_epoch + 1));
}

std::underlying_type_t<ysu::epoch> ysu::normalized_epoch (ysu::epoch epoch_a)
{
	// Currently assumes that the epoch versions in the enum are sequential.
	auto start = std::underlying_type_t<ysu::epoch> (ysu::epoch::epoch_0);
	auto end = std::underlying_type_t<ysu::epoch> (epoch_a);
	debug_assert (end >= start);
	return end - start;
}
