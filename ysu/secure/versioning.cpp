#include <ysu/secure/versioning.hpp>

#include <boost/endian/conversion.hpp>

#include <lmdb/libraries/liblmdb/lmdb.h>

ysu::pending_info_v14::pending_info_v14 (ysu::account const & source_a, ysu::amount const & amount_a, ysu::epoch epoch_a) :
source (source_a),
amount (amount_a),
epoch (epoch_a)
{
}

bool ysu::pending_info_v14::deserialize (ysu::stream & stream_a)
{
	auto error (false);
	try
	{
		ysu::read (stream_a, source.bytes);
		ysu::read (stream_a, amount.bytes);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

size_t ysu::pending_info_v14::db_size () const
{
	return sizeof (source) + sizeof (amount);
}

bool ysu::pending_info_v14::operator== (ysu::pending_info_v14 const & other_a) const
{
	return source == other_a.source && amount == other_a.amount && epoch == other_a.epoch;
}

ysu::account_info_v14::account_info_v14 (ysu::block_hash const & head_a, ysu::block_hash const & rep_block_a, ysu::block_hash const & open_block_a, ysu::amount const & balance_a, uint64_t modified_a, uint64_t block_count_a, uint64_t confirmation_height_a, ysu::epoch epoch_a) :
head (head_a),
rep_block (rep_block_a),
open_block (open_block_a),
balance (balance_a),
modified (modified_a),
block_count (block_count_a),
confirmation_height (confirmation_height_a),
epoch (epoch_a)
{
}

size_t ysu::account_info_v14::db_size () const
{
	debug_assert (reinterpret_cast<const uint8_t *> (this) == reinterpret_cast<const uint8_t *> (&head));
	debug_assert (reinterpret_cast<const uint8_t *> (&head) + sizeof (head) == reinterpret_cast<const uint8_t *> (&rep_block));
	debug_assert (reinterpret_cast<const uint8_t *> (&rep_block) + sizeof (rep_block) == reinterpret_cast<const uint8_t *> (&open_block));
	debug_assert (reinterpret_cast<const uint8_t *> (&open_block) + sizeof (open_block) == reinterpret_cast<const uint8_t *> (&balance));
	debug_assert (reinterpret_cast<const uint8_t *> (&balance) + sizeof (balance) == reinterpret_cast<const uint8_t *> (&modified));
	debug_assert (reinterpret_cast<const uint8_t *> (&modified) + sizeof (modified) == reinterpret_cast<const uint8_t *> (&block_count));
	debug_assert (reinterpret_cast<const uint8_t *> (&block_count) + sizeof (block_count) == reinterpret_cast<const uint8_t *> (&confirmation_height));
	return sizeof (head) + sizeof (rep_block) + sizeof (open_block) + sizeof (balance) + sizeof (modified) + sizeof (block_count) + sizeof (confirmation_height);
}

ysu::block_sideband_v14::block_sideband_v14 (ysu::block_type type_a, ysu::account const & account_a, ysu::block_hash const & successor_a, ysu::amount const & balance_a, uint64_t height_a, uint64_t timestamp_a) :
type (type_a),
successor (successor_a),
account (account_a),
balance (balance_a),
height (height_a),
timestamp (timestamp_a)
{
}

size_t ysu::block_sideband_v14::size (ysu::block_type type_a)
{
	size_t result (0);
	result += sizeof (successor);
	if (type_a != ysu::block_type::state && type_a != ysu::block_type::open)
	{
		result += sizeof (account);
	}
	if (type_a != ysu::block_type::open)
	{
		result += sizeof (height);
	}
	if (type_a == ysu::block_type::receive || type_a == ysu::block_type::change || type_a == ysu::block_type::open)
	{
		result += sizeof (balance);
	}
	result += sizeof (timestamp);
	return result;
}

void ysu::block_sideband_v14::serialize (ysu::stream & stream_a) const
{
	ysu::write (stream_a, successor.bytes);
	if (type != ysu::block_type::state && type != ysu::block_type::open)
	{
		ysu::write (stream_a, account.bytes);
	}
	if (type != ysu::block_type::open)
	{
		ysu::write (stream_a, boost::endian::native_to_big (height));
	}
	if (type == ysu::block_type::receive || type == ysu::block_type::change || type == ysu::block_type::open)
	{
		ysu::write (stream_a, balance.bytes);
	}
	ysu::write (stream_a, boost::endian::native_to_big (timestamp));
}

bool ysu::block_sideband_v14::deserialize (ysu::stream & stream_a)
{
	bool result (false);
	try
	{
		ysu::read (stream_a, successor.bytes);
		if (type != ysu::block_type::state && type != ysu::block_type::open)
		{
			ysu::read (stream_a, account.bytes);
		}
		if (type != ysu::block_type::open)
		{
			ysu::read (stream_a, height);
			boost::endian::big_to_native_inplace (height);
		}
		else
		{
			height = 1;
		}
		if (type == ysu::block_type::receive || type == ysu::block_type::change || type == ysu::block_type::open)
		{
			ysu::read (stream_a, balance.bytes);
		}
		ysu::read (stream_a, timestamp);
		boost::endian::big_to_native_inplace (timestamp);
	}
	catch (std::runtime_error &)
	{
		result = true;
	}

	return result;
}

ysu::block_sideband_v18::block_sideband_v18 (ysu::account const & account_a, ysu::block_hash const & successor_a, ysu::amount const & balance_a, uint64_t height_a, uint64_t timestamp_a, ysu::block_details const & details_a) :
successor (successor_a),
account (account_a),
balance (balance_a),
height (height_a),
timestamp (timestamp_a),
details (details_a)
{
}

ysu::block_sideband_v18::block_sideband_v18 (ysu::account const & account_a, ysu::block_hash const & successor_a, ysu::amount const & balance_a, uint64_t height_a, uint64_t timestamp_a, ysu::epoch epoch_a, bool is_send, bool is_receive, bool is_epoch) :
successor (successor_a),
account (account_a),
balance (balance_a),
height (height_a),
timestamp (timestamp_a),
details (epoch_a, is_send, is_receive, is_epoch)
{
}

size_t ysu::block_sideband_v18::size (ysu::block_type type_a)
{
	size_t result (0);
	result += sizeof (successor);
	if (type_a != ysu::block_type::state && type_a != ysu::block_type::open)
	{
		result += sizeof (account);
	}
	if (type_a != ysu::block_type::open)
	{
		result += sizeof (height);
	}
	if (type_a == ysu::block_type::receive || type_a == ysu::block_type::change || type_a == ysu::block_type::open)
	{
		result += sizeof (balance);
	}
	result += sizeof (timestamp);
	if (type_a == ysu::block_type::state)
	{
		static_assert (sizeof (ysu::epoch) == ysu::block_details::size (), "block_details_v18 is larger than the epoch enum");
		result += ysu::block_details::size ();
	}
	return result;
}

void ysu::block_sideband_v18::serialize (ysu::stream & stream_a, ysu::block_type type_a) const
{
	ysu::write (stream_a, successor.bytes);
	if (type_a != ysu::block_type::state && type_a != ysu::block_type::open)
	{
		ysu::write (stream_a, account.bytes);
	}
	if (type_a != ysu::block_type::open)
	{
		ysu::write (stream_a, boost::endian::native_to_big (height));
	}
	if (type_a == ysu::block_type::receive || type_a == ysu::block_type::change || type_a == ysu::block_type::open)
	{
		ysu::write (stream_a, balance.bytes);
	}
	ysu::write (stream_a, boost::endian::native_to_big (timestamp));
	if (type_a == ysu::block_type::state)
	{
		details.serialize (stream_a);
	}
}

bool ysu::block_sideband_v18::deserialize (ysu::stream & stream_a, ysu::block_type type_a)
{
	bool result (false);
	try
	{
		ysu::read (stream_a, successor.bytes);
		if (type_a != ysu::block_type::state && type_a != ysu::block_type::open)
		{
			ysu::read (stream_a, account.bytes);
		}
		if (type_a != ysu::block_type::open)
		{
			ysu::read (stream_a, height);
			boost::endian::big_to_native_inplace (height);
		}
		else
		{
			height = 1;
		}
		if (type_a == ysu::block_type::receive || type_a == ysu::block_type::change || type_a == ysu::block_type::open)
		{
			ysu::read (stream_a, balance.bytes);
		}
		ysu::read (stream_a, timestamp);
		boost::endian::big_to_native_inplace (timestamp);
		if (type_a == ysu::block_type::state)
		{
			result = details.deserialize (stream_a);
		}
	}
	catch (std::runtime_error &)
	{
		result = true;
	}

	return result;
}
