#include <ysu/crypto_lib/random_pool.hpp>
#include <ysu/lib/blocks.hpp>
#include <ysu/lib/memory.hpp>
#include <ysu/lib/numbers.hpp>
#include <ysu/lib/threading.hpp>

#include <crypto/cryptopp/words.h>

#include <boost/endian/conversion.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <bitset>

/** Compare blocks, first by type, then content. This is an optimization over dynamic_cast, which is very slow on some platforms. */
namespace
{
template <typename T>
bool blocks_equal (T const & first, ysu::block const & second)
{
	static_assert (std::is_base_of<ysu::block, T>::value, "Input parameter is not a block type");
	return (first.type () == second.type ()) && (static_cast<T const &> (second)) == first;
}

template <typename block>
std::shared_ptr<block> deserialize_block (ysu::stream & stream_a)
{
	auto error (false);
	auto result = ysu::make_shared<block> (error, stream_a);
	if (error)
	{
		result = nullptr;
	}

	return result;
}
}

void ysu::block_memory_pool_purge ()
{
	ysu::purge_singleton_pool_memory<ysu::open_block> ();
	ysu::purge_singleton_pool_memory<ysu::state_block> ();
	ysu::purge_singleton_pool_memory<ysu::send_block> ();
	ysu::purge_singleton_pool_memory<ysu::change_block> ();
}

std::string ysu::block::to_json () const
{
	std::string result;
	serialize_json (result);
	return result;
}

size_t ysu::block::size (ysu::block_type type_a)
{
	size_t result (0);
	switch (type_a)
	{
		case ysu::block_type::invalid:
		case ysu::block_type::not_a_block:
			debug_assert (false);
			break;
		case ysu::block_type::send:
			result = ysu::send_block::size;
			break;
		case ysu::block_type::receive:
			result = ysu::receive_block::size;
			break;
		case ysu::block_type::change:
			result = ysu::change_block::size;
			break;
		case ysu::block_type::open:
			result = ysu::open_block::size;
			break;
		case ysu::block_type::state:
			result = ysu::state_block::size;
			break;
	}
	return result;
}

ysu::work_version ysu::block::work_version () const
{
	return ysu::work_version::work_1;
}

uint64_t ysu::block::difficulty () const
{
	return ysu::work_difficulty (this->work_version (), this->root (), this->block_work ());
}

ysu::block_hash ysu::block::generate_hash () const
{
	ysu::block_hash result;
	blake2b_state hash_l;
	auto status (blake2b_init (&hash_l, sizeof (result.bytes)));
	debug_assert (status == 0);
	hash (hash_l);
	status = blake2b_final (&hash_l, result.bytes.data (), sizeof (result.bytes));
	debug_assert (status == 0);
	return result;
}

void ysu::block::refresh ()
{
	if (!cached_hash.is_zero ())
	{
		cached_hash = generate_hash ();
	}
}

ysu::block_hash const & ysu::block::hash () const
{
	if (!cached_hash.is_zero ())
	{
		// Once a block is created, it should not be modified (unless using refresh ())
		// This would invalidate the cache; check it hasn't changed.
		debug_assert (cached_hash == generate_hash ());
	}
	else
	{
		cached_hash = generate_hash ();
	}

	return cached_hash;
}

ysu::block_hash ysu::block::full_hash () const
{
	ysu::block_hash result;
	blake2b_state state;
	blake2b_init (&state, sizeof (result.bytes));
	blake2b_update (&state, hash ().bytes.data (), sizeof (hash ()));
	auto signature (block_signature ());
	blake2b_update (&state, signature.bytes.data (), sizeof (signature));
	auto work (block_work ());
	blake2b_update (&state, &work, sizeof (work));
	blake2b_final (&state, result.bytes.data (), sizeof (result.bytes));
	return result;
}

ysu::block_sideband const & ysu::block::sideband () const
{
	debug_assert (sideband_m.is_initialized ());
	return *sideband_m;
}

void ysu::block::sideband_set (ysu::block_sideband const & sideband_a)
{
	sideband_m = sideband_a;
}

bool ysu::block::has_sideband () const
{
	return sideband_m.is_initialized ();
}

ysu::account const & ysu::block::representative () const
{
	static ysu::account rep{ 0 };
	return rep;
}

ysu::block_hash const & ysu::block::source () const
{
	static ysu::block_hash source{ 0 };
	return source;
}

ysu::link const & ysu::block::link () const
{
	static ysu::link link{ 0 };
	return link;
}

ysu::account const & ysu::block::account () const
{
	static ysu::account account{ 0 };
	return account;
}

ysu::qualified_root ysu::block::qualified_root () const
{
	return ysu::qualified_root (previous (), root ());
}

ysu::amount const & ysu::block::balance () const
{
	static ysu::amount amount{ 0 };
	return amount;
}

void ysu::send_block::visit (ysu::block_visitor & visitor_a) const
{
	visitor_a.send_block (*this);
}

void ysu::send_block::visit (ysu::mutable_block_visitor & visitor_a)
{
	visitor_a.send_block (*this);
}

void ysu::send_block::hash (blake2b_state & hash_a) const
{
	hashables.hash (hash_a);
}

uint64_t ysu::send_block::block_work () const
{
	return work;
}

void ysu::send_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

ysu::send_hashables::send_hashables (ysu::block_hash const & previous_a, ysu::account const & destination_a, ysu::amount const & balance_a) :
previous (previous_a),
destination (destination_a),
balance (balance_a)
{
}

ysu::send_hashables::send_hashables (bool & error_a, ysu::stream & stream_a)
{
	try
	{
		ysu::read (stream_a, previous.bytes);
		ysu::read (stream_a, destination.bytes);
		ysu::read (stream_a, balance.bytes);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

ysu::send_hashables::send_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto destination_l (tree_a.get<std::string> ("destination"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		error_a = previous.decode_hex (previous_l);
		if (!error_a)
		{
			error_a = destination.decode_account (destination_l);
			if (!error_a)
			{
				error_a = balance.decode_hex (balance_l);
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void ysu::send_hashables::hash (blake2b_state & hash_a) const
{
	auto status (blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes)));
	debug_assert (status == 0);
	status = blake2b_update (&hash_a, destination.bytes.data (), sizeof (destination.bytes));
	debug_assert (status == 0);
	status = blake2b_update (&hash_a, balance.bytes.data (), sizeof (balance.bytes));
	debug_assert (status == 0);
}

void ysu::send_block::serialize (ysu::stream & stream_a) const
{
	write (stream_a, hashables.previous.bytes);
	write (stream_a, hashables.destination.bytes);
	write (stream_a, hashables.balance.bytes);
	write (stream_a, signature.bytes);
	write (stream_a, work);
}

bool ysu::send_block::deserialize (ysu::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.previous.bytes);
		read (stream_a, hashables.destination.bytes);
		read (stream_a, hashables.balance.bytes);
		read (stream_a, signature.bytes);
		read (stream_a, work);
	}
	catch (std::exception const &)
	{
		error = true;
	}

	return error;
}

void ysu::send_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void ysu::send_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "send");
	std::string previous;
	hashables.previous.encode_hex (previous);
	tree.put ("previous", previous);
	tree.put ("destination", hashables.destination.to_account ());
	std::string balance;
	hashables.balance.encode_hex (balance);
	tree.put ("balance", balance);
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("work", ysu::to_string_hex (work));
	tree.put ("signature", signature_l);
}

bool ysu::send_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "send");
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto destination_l (tree_a.get<std::string> ("destination"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.previous.decode_hex (previous_l);
		if (!error)
		{
			error = hashables.destination.decode_account (destination_l);
			if (!error)
			{
				error = hashables.balance.decode_hex (balance_l);
				if (!error)
				{
					error = ysu::from_string_hex (work_l, work);
					if (!error)
					{
						error = signature.decode_hex (signature_l);
					}
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

ysu::send_block::send_block (ysu::block_hash const & previous_a, ysu::account const & destination_a, ysu::amount const & balance_a, ysu::raw_key const & prv_a, ysu::public_key const & pub_a, uint64_t work_a) :
hashables (previous_a, destination_a, balance_a),
signature (ysu::sign_message (prv_a, pub_a, hash ())),
work (work_a)
{
}

ysu::send_block::send_block (bool & error_a, ysu::stream & stream_a) :
hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			ysu::read (stream_a, signature.bytes);
			ysu::read (stream_a, work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

ysu::send_block::send_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto signature_l (tree_a.get<std::string> ("signature"));
			auto work_l (tree_a.get<std::string> ("work"));
			error_a = signature.decode_hex (signature_l);
			if (!error_a)
			{
				error_a = ysu::from_string_hex (work_l, work);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

bool ysu::send_block::operator== (ysu::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool ysu::send_block::valid_predecessor (ysu::block const & block_a) const
{
	bool result;
	switch (block_a.type ())
	{
		case ysu::block_type::send:
		case ysu::block_type::receive:
		case ysu::block_type::open:
		case ysu::block_type::change:
			result = true;
			break;
		default:
			result = false;
			break;
	}
	return result;
}

ysu::block_type ysu::send_block::type () const
{
	return ysu::block_type::send;
}

bool ysu::send_block::operator== (ysu::send_block const & other_a) const
{
	auto result (hashables.destination == other_a.hashables.destination && hashables.previous == other_a.hashables.previous && hashables.balance == other_a.hashables.balance && work == other_a.work && signature == other_a.signature);
	return result;
}

ysu::block_hash const & ysu::send_block::previous () const
{
	return hashables.previous;
}

ysu::root const & ysu::send_block::root () const
{
	return hashables.previous;
}

ysu::amount const & ysu::send_block::balance () const
{
	return hashables.balance;
}

ysu::signature const & ysu::send_block::block_signature () const
{
	return signature;
}

void ysu::send_block::signature_set (ysu::signature const & signature_a)
{
	signature = signature_a;
}

ysu::open_hashables::open_hashables (ysu::block_hash const & source_a, ysu::account const & representative_a, ysu::account const & account_a) :
source (source_a),
representative (representative_a),
account (account_a)
{
}

ysu::open_hashables::open_hashables (bool & error_a, ysu::stream & stream_a)
{
	try
	{
		ysu::read (stream_a, source.bytes);
		ysu::read (stream_a, representative.bytes);
		ysu::read (stream_a, account.bytes);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

ysu::open_hashables::open_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto source_l (tree_a.get<std::string> ("source"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto account_l (tree_a.get<std::string> ("account"));
		error_a = source.decode_hex (source_l);
		if (!error_a)
		{
			error_a = representative.decode_account (representative_l);
			if (!error_a)
			{
				error_a = account.decode_account (account_l);
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void ysu::open_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, source.bytes.data (), sizeof (source.bytes));
	blake2b_update (&hash_a, representative.bytes.data (), sizeof (representative.bytes));
	blake2b_update (&hash_a, account.bytes.data (), sizeof (account.bytes));
}

ysu::open_block::open_block (ysu::block_hash const & source_a, ysu::account const & representative_a, ysu::account const & account_a, ysu::raw_key const & prv_a, ysu::public_key const & pub_a, uint64_t work_a) :
hashables (source_a, representative_a, account_a),
signature (ysu::sign_message (prv_a, pub_a, hash ())),
work (work_a)
{
	debug_assert (!representative_a.is_zero ());
}

ysu::open_block::open_block (ysu::block_hash const & source_a, ysu::account const & representative_a, ysu::account const & account_a, std::nullptr_t) :
hashables (source_a, representative_a, account_a),
work (0)
{
	signature.clear ();
}

ysu::open_block::open_block (bool & error_a, ysu::stream & stream_a) :
hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			ysu::read (stream_a, signature);
			ysu::read (stream_a, work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

ysu::open_block::open_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto work_l (tree_a.get<std::string> ("work"));
			auto signature_l (tree_a.get<std::string> ("signature"));
			error_a = ysu::from_string_hex (work_l, work);
			if (!error_a)
			{
				error_a = signature.decode_hex (signature_l);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void ysu::open_block::hash (blake2b_state & hash_a) const
{
	hashables.hash (hash_a);
}

uint64_t ysu::open_block::block_work () const
{
	return work;
}

void ysu::open_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

ysu::block_hash const & ysu::open_block::previous () const
{
	static ysu::block_hash result{ 0 };
	return result;
}

ysu::account const & ysu::open_block::account () const
{
	return hashables.account;
}

void ysu::open_block::serialize (ysu::stream & stream_a) const
{
	write (stream_a, hashables.source);
	write (stream_a, hashables.representative);
	write (stream_a, hashables.account);
	write (stream_a, signature);
	write (stream_a, work);
}

bool ysu::open_block::deserialize (ysu::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.source);
		read (stream_a, hashables.representative);
		read (stream_a, hashables.account);
		read (stream_a, signature);
		read (stream_a, work);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

void ysu::open_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void ysu::open_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "open");
	tree.put ("source", hashables.source.to_string ());
	tree.put ("representative", representative ().to_account ());
	tree.put ("account", hashables.account.to_account ());
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("work", ysu::to_string_hex (work));
	tree.put ("signature", signature_l);
}

bool ysu::open_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "open");
		auto source_l (tree_a.get<std::string> ("source"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto account_l (tree_a.get<std::string> ("account"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.source.decode_hex (source_l);
		if (!error)
		{
			error = hashables.representative.decode_hex (representative_l);
			if (!error)
			{
				error = hashables.account.decode_hex (account_l);
				if (!error)
				{
					error = ysu::from_string_hex (work_l, work);
					if (!error)
					{
						error = signature.decode_hex (signature_l);
					}
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

void ysu::open_block::visit (ysu::block_visitor & visitor_a) const
{
	visitor_a.open_block (*this);
}

void ysu::open_block::visit (ysu::mutable_block_visitor & visitor_a)
{
	visitor_a.open_block (*this);
}

ysu::block_type ysu::open_block::type () const
{
	return ysu::block_type::open;
}

bool ysu::open_block::operator== (ysu::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool ysu::open_block::operator== (ysu::open_block const & other_a) const
{
	return hashables.source == other_a.hashables.source && hashables.representative == other_a.hashables.representative && hashables.account == other_a.hashables.account && work == other_a.work && signature == other_a.signature;
}

bool ysu::open_block::valid_predecessor (ysu::block const & block_a) const
{
	return false;
}

ysu::block_hash const & ysu::open_block::source () const
{
	return hashables.source;
}

ysu::root const & ysu::open_block::root () const
{
	return hashables.account;
}

ysu::account const & ysu::open_block::representative () const
{
	return hashables.representative;
}

ysu::signature const & ysu::open_block::block_signature () const
{
	return signature;
}

void ysu::open_block::signature_set (ysu::signature const & signature_a)
{
	signature = signature_a;
}

ysu::change_hashables::change_hashables (ysu::block_hash const & previous_a, ysu::account const & representative_a) :
previous (previous_a),
representative (representative_a)
{
}

ysu::change_hashables::change_hashables (bool & error_a, ysu::stream & stream_a)
{
	try
	{
		ysu::read (stream_a, previous);
		ysu::read (stream_a, representative);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

ysu::change_hashables::change_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		error_a = previous.decode_hex (previous_l);
		if (!error_a)
		{
			error_a = representative.decode_account (representative_l);
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void ysu::change_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&hash_a, representative.bytes.data (), sizeof (representative.bytes));
}

ysu::change_block::change_block (ysu::block_hash const & previous_a, ysu::account const & representative_a, ysu::raw_key const & prv_a, ysu::public_key const & pub_a, uint64_t work_a) :
hashables (previous_a, representative_a),
signature (ysu::sign_message (prv_a, pub_a, hash ())),
work (work_a)
{
}

ysu::change_block::change_block (bool & error_a, ysu::stream & stream_a) :
hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			ysu::read (stream_a, signature);
			ysu::read (stream_a, work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

ysu::change_block::change_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto work_l (tree_a.get<std::string> ("work"));
			auto signature_l (tree_a.get<std::string> ("signature"));
			error_a = ysu::from_string_hex (work_l, work);
			if (!error_a)
			{
				error_a = signature.decode_hex (signature_l);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void ysu::change_block::hash (blake2b_state & hash_a) const
{
	hashables.hash (hash_a);
}

uint64_t ysu::change_block::block_work () const
{
	return work;
}

void ysu::change_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

ysu::block_hash const & ysu::change_block::previous () const
{
	return hashables.previous;
}

void ysu::change_block::serialize (ysu::stream & stream_a) const
{
	write (stream_a, hashables.previous);
	write (stream_a, hashables.representative);
	write (stream_a, signature);
	write (stream_a, work);
}

bool ysu::change_block::deserialize (ysu::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.previous);
		read (stream_a, hashables.representative);
		read (stream_a, signature);
		read (stream_a, work);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

void ysu::change_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void ysu::change_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "change");
	tree.put ("previous", hashables.previous.to_string ());
	tree.put ("representative", representative ().to_account ());
	tree.put ("work", ysu::to_string_hex (work));
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("signature", signature_l);
}

bool ysu::change_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "change");
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.previous.decode_hex (previous_l);
		if (!error)
		{
			error = hashables.representative.decode_hex (representative_l);
			if (!error)
			{
				error = ysu::from_string_hex (work_l, work);
				if (!error)
				{
					error = signature.decode_hex (signature_l);
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

void ysu::change_block::visit (ysu::block_visitor & visitor_a) const
{
	visitor_a.change_block (*this);
}

void ysu::change_block::visit (ysu::mutable_block_visitor & visitor_a)
{
	visitor_a.change_block (*this);
}

ysu::block_type ysu::change_block::type () const
{
	return ysu::block_type::change;
}

bool ysu::change_block::operator== (ysu::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool ysu::change_block::operator== (ysu::change_block const & other_a) const
{
	return hashables.previous == other_a.hashables.previous && hashables.representative == other_a.hashables.representative && work == other_a.work && signature == other_a.signature;
}

bool ysu::change_block::valid_predecessor (ysu::block const & block_a) const
{
	bool result;
	switch (block_a.type ())
	{
		case ysu::block_type::send:
		case ysu::block_type::receive:
		case ysu::block_type::open:
		case ysu::block_type::change:
			result = true;
			break;
		default:
			result = false;
			break;
	}
	return result;
}

ysu::root const & ysu::change_block::root () const
{
	return hashables.previous;
}

ysu::account const & ysu::change_block::representative () const
{
	return hashables.representative;
}

ysu::signature const & ysu::change_block::block_signature () const
{
	return signature;
}

void ysu::change_block::signature_set (ysu::signature const & signature_a)
{
	signature = signature_a;
}

ysu::state_hashables::state_hashables (ysu::account const & account_a, ysu::block_hash const & previous_a, ysu::account const & representative_a, ysu::amount const & balance_a, ysu::link const & link_a) :
account (account_a),
previous (previous_a),
representative (representative_a),
balance (balance_a),
link (link_a)
{
}

ysu::state_hashables::state_hashables (bool & error_a, ysu::stream & stream_a)
{
	try
	{
		ysu::read (stream_a, account);
		ysu::read (stream_a, previous);
		ysu::read (stream_a, representative);
		ysu::read (stream_a, balance);
		ysu::read (stream_a, link);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

ysu::state_hashables::state_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto account_l (tree_a.get<std::string> ("account"));
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		auto link_l (tree_a.get<std::string> ("link"));
		error_a = account.decode_account (account_l);
		if (!error_a)
		{
			error_a = previous.decode_hex (previous_l);
			if (!error_a)
			{
				error_a = representative.decode_account (representative_l);
				if (!error_a)
				{
					error_a = balance.decode_dec (balance_l);
					if (!error_a)
					{
						error_a = link.decode_account (link_l) && link.decode_hex (link_l);
					}
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void ysu::state_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, account.bytes.data (), sizeof (account.bytes));
	blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&hash_a, representative.bytes.data (), sizeof (representative.bytes));
	blake2b_update (&hash_a, balance.bytes.data (), sizeof (balance.bytes));
	blake2b_update (&hash_a, link.bytes.data (), sizeof (link.bytes));
}

ysu::state_block::state_block (ysu::account const & account_a, ysu::block_hash const & previous_a, ysu::account const & representative_a, ysu::amount const & balance_a, ysu::link const & link_a, ysu::raw_key const & prv_a, ysu::public_key const & pub_a, uint64_t work_a) :
hashables (account_a, previous_a, representative_a, balance_a, link_a),
signature (ysu::sign_message (prv_a, pub_a, hash ())),
work (work_a)
{
}

ysu::state_block::state_block (bool & error_a, ysu::stream & stream_a) :
hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			ysu::read (stream_a, signature);
			ysu::read (stream_a, work);
			boost::endian::big_to_native_inplace (work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

ysu::state_block::state_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto type_l (tree_a.get<std::string> ("type"));
			auto signature_l (tree_a.get<std::string> ("signature"));
			auto work_l (tree_a.get<std::string> ("work"));
			error_a = type_l != "state";
			if (!error_a)
			{
				error_a = ysu::from_string_hex (work_l, work);
				if (!error_a)
				{
					error_a = signature.decode_hex (signature_l);
				}
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void ysu::state_block::hash (blake2b_state & hash_a) const
{
	ysu::uint256_union preamble (static_cast<uint64_t> (ysu::block_type::state));
	blake2b_update (&hash_a, preamble.bytes.data (), preamble.bytes.size ());
	hashables.hash (hash_a);
}

uint64_t ysu::state_block::block_work () const
{
	return work;
}

void ysu::state_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

ysu::block_hash const & ysu::state_block::previous () const
{
	return hashables.previous;
}

ysu::account const & ysu::state_block::account () const
{
	return hashables.account;
}

void ysu::state_block::serialize (ysu::stream & stream_a) const
{
	write (stream_a, hashables.account);
	write (stream_a, hashables.previous);
	write (stream_a, hashables.representative);
	write (stream_a, hashables.balance);
	write (stream_a, hashables.link);
	write (stream_a, signature);
	write (stream_a, boost::endian::native_to_big (work));
}

bool ysu::state_block::deserialize (ysu::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.account);
		read (stream_a, hashables.previous);
		read (stream_a, hashables.representative);
		read (stream_a, hashables.balance);
		read (stream_a, hashables.link);
		read (stream_a, signature);
		read (stream_a, work);
		boost::endian::big_to_native_inplace (work);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

void ysu::state_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void ysu::state_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "state");
	tree.put ("account", hashables.account.to_account ());
	tree.put ("previous", hashables.previous.to_string ());
	tree.put ("representative", representative ().to_account ());
	tree.put ("balance", hashables.balance.to_string_dec ());
	tree.put ("link", hashables.link.to_string ());
	tree.put ("link_as_account", hashables.link.to_account ());
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("signature", signature_l);
	tree.put ("work", ysu::to_string_hex (work));
}

bool ysu::state_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "state");
		auto account_l (tree_a.get<std::string> ("account"));
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto representative_l (tree_a.get<std::string> ("representative"));
		auto balance_l (tree_a.get<std::string> ("balance"));
		auto link_l (tree_a.get<std::string> ("link"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.account.decode_account (account_l);
		if (!error)
		{
			error = hashables.previous.decode_hex (previous_l);
			if (!error)
			{
				error = hashables.representative.decode_account (representative_l);
				if (!error)
				{
					error = hashables.balance.decode_dec (balance_l);
					if (!error)
					{
						error = hashables.link.decode_account (link_l) && hashables.link.decode_hex (link_l);
						if (!error)
						{
							error = ysu::from_string_hex (work_l, work);
							if (!error)
							{
								error = signature.decode_hex (signature_l);
							}
						}
					}
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

void ysu::state_block::visit (ysu::block_visitor & visitor_a) const
{
	visitor_a.state_block (*this);
}

void ysu::state_block::visit (ysu::mutable_block_visitor & visitor_a)
{
	visitor_a.state_block (*this);
}

ysu::block_type ysu::state_block::type () const
{
	return ysu::block_type::state;
}

bool ysu::state_block::operator== (ysu::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool ysu::state_block::operator== (ysu::state_block const & other_a) const
{
	return hashables.account == other_a.hashables.account && hashables.previous == other_a.hashables.previous && hashables.representative == other_a.hashables.representative && hashables.balance == other_a.hashables.balance && hashables.link == other_a.hashables.link && signature == other_a.signature && work == other_a.work;
}

bool ysu::state_block::valid_predecessor (ysu::block const & block_a) const
{
	return true;
}

ysu::root const & ysu::state_block::root () const
{
	if (!hashables.previous.is_zero ())
	{
		return hashables.previous;
	}
	else
	{
		return hashables.account;
	}
}

ysu::link const & ysu::state_block::link () const
{
	return hashables.link;
}

ysu::account const & ysu::state_block::representative () const
{
	return hashables.representative;
}

ysu::amount const & ysu::state_block::balance () const
{
	return hashables.balance;
}

ysu::signature const & ysu::state_block::block_signature () const
{
	return signature;
}

void ysu::state_block::signature_set (ysu::signature const & signature_a)
{
	signature = signature_a;
}

std::shared_ptr<ysu::block> ysu::deserialize_block_json (boost::property_tree::ptree const & tree_a, ysu::block_uniquer * uniquer_a)
{
	std::shared_ptr<ysu::block> result;
	try
	{
		auto type (tree_a.get<std::string> ("type"));
		bool error (false);
		std::unique_ptr<ysu::block> obj;
		if (type == "receive")
		{
			obj = std::make_unique<ysu::receive_block> (error, tree_a);
		}
		else if (type == "send")
		{
			obj = std::make_unique<ysu::send_block> (error, tree_a);
		}
		else if (type == "open")
		{
			obj = std::make_unique<ysu::open_block> (error, tree_a);
		}
		else if (type == "change")
		{
			obj = std::make_unique<ysu::change_block> (error, tree_a);
		}
		else if (type == "state")
		{
			obj = std::make_unique<ysu::state_block> (error, tree_a);
		}

		if (!error)
		{
			result = std::move (obj);
		}
	}
	catch (std::runtime_error const &)
	{
	}
	if (uniquer_a != nullptr)
	{
		result = uniquer_a->unique (result);
	}
	return result;
}

std::shared_ptr<ysu::block> ysu::deserialize_block (ysu::stream & stream_a)
{
	ysu::block_type type;
	auto error (try_read (stream_a, type));
	std::shared_ptr<ysu::block> result;
	if (!error)
	{
		result = ysu::deserialize_block (stream_a, type);
	}
	return result;
}

std::shared_ptr<ysu::block> ysu::deserialize_block (ysu::stream & stream_a, ysu::block_type type_a, ysu::block_uniquer * uniquer_a)
{
	std::shared_ptr<ysu::block> result;
	switch (type_a)
	{
		case ysu::block_type::receive:
		{
			result = ::deserialize_block<ysu::receive_block> (stream_a);
			break;
		}
		case ysu::block_type::send:
		{
			result = ::deserialize_block<ysu::send_block> (stream_a);
			break;
		}
		case ysu::block_type::open:
		{
			result = ::deserialize_block<ysu::open_block> (stream_a);
			break;
		}
		case ysu::block_type::change:
		{
			result = ::deserialize_block<ysu::change_block> (stream_a);
			break;
		}
		case ysu::block_type::state:
		{
			result = ::deserialize_block<ysu::state_block> (stream_a);
			break;
		}
		default:
#ifndef YSU_FUZZER_TEST
			debug_assert (false);
#endif
			break;
	}
	if (uniquer_a != nullptr)
	{
		result = uniquer_a->unique (result);
	}
	return result;
}

void ysu::receive_block::visit (ysu::block_visitor & visitor_a) const
{
	visitor_a.receive_block (*this);
}

void ysu::receive_block::visit (ysu::mutable_block_visitor & visitor_a)
{
	visitor_a.receive_block (*this);
}

bool ysu::receive_block::operator== (ysu::receive_block const & other_a) const
{
	auto result (hashables.previous == other_a.hashables.previous && hashables.source == other_a.hashables.source && work == other_a.work && signature == other_a.signature);
	return result;
}

void ysu::receive_block::serialize (ysu::stream & stream_a) const
{
	write (stream_a, hashables.previous.bytes);
	write (stream_a, hashables.source.bytes);
	write (stream_a, signature.bytes);
	write (stream_a, work);
}

bool ysu::receive_block::deserialize (ysu::stream & stream_a)
{
	auto error (false);
	try
	{
		read (stream_a, hashables.previous.bytes);
		read (stream_a, hashables.source.bytes);
		read (stream_a, signature.bytes);
		read (stream_a, work);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

void ysu::receive_block::serialize_json (std::string & string_a, bool single_line) const
{
	boost::property_tree::ptree tree;
	serialize_json (tree);
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree, !single_line);
	string_a = ostream.str ();
}

void ysu::receive_block::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("type", "receive");
	std::string previous;
	hashables.previous.encode_hex (previous);
	tree.put ("previous", previous);
	std::string source;
	hashables.source.encode_hex (source);
	tree.put ("source", source);
	std::string signature_l;
	signature.encode_hex (signature_l);
	tree.put ("work", ysu::to_string_hex (work));
	tree.put ("signature", signature_l);
}

bool ysu::receive_block::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto error (false);
	try
	{
		debug_assert (tree_a.get<std::string> ("type") == "receive");
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto source_l (tree_a.get<std::string> ("source"));
		auto work_l (tree_a.get<std::string> ("work"));
		auto signature_l (tree_a.get<std::string> ("signature"));
		error = hashables.previous.decode_hex (previous_l);
		if (!error)
		{
			error = hashables.source.decode_hex (source_l);
			if (!error)
			{
				error = ysu::from_string_hex (work_l, work);
				if (!error)
				{
					error = signature.decode_hex (signature_l);
				}
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

ysu::receive_block::receive_block (ysu::block_hash const & previous_a, ysu::block_hash const & source_a, ysu::raw_key const & prv_a, ysu::public_key const & pub_a, uint64_t work_a) :
hashables (previous_a, source_a),
signature (ysu::sign_message (prv_a, pub_a, hash ())),
work (work_a)
{
}

ysu::receive_block::receive_block (bool & error_a, ysu::stream & stream_a) :
hashables (error_a, stream_a)
{
	if (!error_a)
	{
		try
		{
			ysu::read (stream_a, signature);
			ysu::read (stream_a, work);
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

ysu::receive_block::receive_block (bool & error_a, boost::property_tree::ptree const & tree_a) :
hashables (error_a, tree_a)
{
	if (!error_a)
	{
		try
		{
			auto signature_l (tree_a.get<std::string> ("signature"));
			auto work_l (tree_a.get<std::string> ("work"));
			error_a = signature.decode_hex (signature_l);
			if (!error_a)
			{
				error_a = ysu::from_string_hex (work_l, work);
			}
		}
		catch (std::runtime_error const &)
		{
			error_a = true;
		}
	}
}

void ysu::receive_block::hash (blake2b_state & hash_a) const
{
	hashables.hash (hash_a);
}

uint64_t ysu::receive_block::block_work () const
{
	return work;
}

void ysu::receive_block::block_work_set (uint64_t work_a)
{
	work = work_a;
}

bool ysu::receive_block::operator== (ysu::block const & other_a) const
{
	return blocks_equal (*this, other_a);
}

bool ysu::receive_block::valid_predecessor (ysu::block const & block_a) const
{
	bool result;
	switch (block_a.type ())
	{
		case ysu::block_type::send:
		case ysu::block_type::receive:
		case ysu::block_type::open:
		case ysu::block_type::change:
			result = true;
			break;
		default:
			result = false;
			break;
	}
	return result;
}

ysu::block_hash const & ysu::receive_block::previous () const
{
	return hashables.previous;
}

ysu::block_hash const & ysu::receive_block::source () const
{
	return hashables.source;
}

ysu::root const & ysu::receive_block::root () const
{
	return hashables.previous;
}

ysu::signature const & ysu::receive_block::block_signature () const
{
	return signature;
}

void ysu::receive_block::signature_set (ysu::signature const & signature_a)
{
	signature = signature_a;
}

ysu::block_type ysu::receive_block::type () const
{
	return ysu::block_type::receive;
}

ysu::receive_hashables::receive_hashables (ysu::block_hash const & previous_a, ysu::block_hash const & source_a) :
previous (previous_a),
source (source_a)
{
}

ysu::receive_hashables::receive_hashables (bool & error_a, ysu::stream & stream_a)
{
	try
	{
		ysu::read (stream_a, previous.bytes);
		ysu::read (stream_a, source.bytes);
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

ysu::receive_hashables::receive_hashables (bool & error_a, boost::property_tree::ptree const & tree_a)
{
	try
	{
		auto previous_l (tree_a.get<std::string> ("previous"));
		auto source_l (tree_a.get<std::string> ("source"));
		error_a = previous.decode_hex (previous_l);
		if (!error_a)
		{
			error_a = source.decode_hex (source_l);
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}
}

void ysu::receive_hashables::hash (blake2b_state & hash_a) const
{
	blake2b_update (&hash_a, previous.bytes.data (), sizeof (previous.bytes));
	blake2b_update (&hash_a, source.bytes.data (), sizeof (source.bytes));
}

ysu::block_details::block_details (ysu::epoch const epoch_a, bool const is_send_a, bool const is_receive_a, bool const is_epoch_a) :
epoch (epoch_a), is_send (is_send_a), is_receive (is_receive_a), is_epoch (is_epoch_a)
{
}

bool ysu::block_details::operator== (ysu::block_details const & other_a) const
{
	return epoch == other_a.epoch && is_send == other_a.is_send && is_receive == other_a.is_receive && is_epoch == other_a.is_epoch;
}

uint8_t ysu::block_details::packed () const
{
	std::bitset<8> result (static_cast<uint8_t> (epoch));
	result.set (7, is_send);
	result.set (6, is_receive);
	result.set (5, is_epoch);
	return static_cast<uint8_t> (result.to_ulong ());
}

void ysu::block_details::unpack (uint8_t details_a)
{
	constexpr std::bitset<8> epoch_mask{ 0b00011111 };
	auto as_bitset = static_cast<std::bitset<8>> (details_a);
	is_send = as_bitset.test (7);
	is_receive = as_bitset.test (6);
	is_epoch = as_bitset.test (5);
	epoch = static_cast<ysu::epoch> ((as_bitset & epoch_mask).to_ulong ());
}

void ysu::block_details::serialize (ysu::stream & stream_a) const
{
	ysu::write (stream_a, packed ());
}

bool ysu::block_details::deserialize (ysu::stream & stream_a)
{
	bool result (false);
	try
	{
		uint8_t packed{ 0 };
		ysu::read (stream_a, packed);
		unpack (packed);
	}
	catch (std::runtime_error &)
	{
		result = true;
	}

	return result;
}

std::string ysu::state_subtype (ysu::block_details const details_a)
{
	debug_assert (details_a.is_epoch + details_a.is_receive + details_a.is_send <= 1);
	if (details_a.is_send)
	{
		return "send";
	}
	else if (details_a.is_receive)
	{
		return "receive";
	}
	else if (details_a.is_epoch)
	{
		return "epoch";
	}
	else
	{
		return "change";
	}
}

ysu::block_sideband::block_sideband (ysu::account const & account_a, ysu::block_hash const & successor_a, ysu::amount const & balance_a, uint64_t const height_a, uint64_t const timestamp_a, ysu::block_details const & details_a, ysu::epoch const source_epoch_a) :
successor (successor_a),
account (account_a),
balance (balance_a),
height (height_a),
timestamp (timestamp_a),
details (details_a),
source_epoch (source_epoch_a)
{
}

ysu::block_sideband::block_sideband (ysu::account const & account_a, ysu::block_hash const & successor_a, ysu::amount const & balance_a, uint64_t const height_a, uint64_t const timestamp_a, ysu::epoch const epoch_a, bool const is_send, bool const is_receive, bool const is_epoch, ysu::epoch const source_epoch_a) :
successor (successor_a),
account (account_a),
balance (balance_a),
height (height_a),
timestamp (timestamp_a),
details (epoch_a, is_send, is_receive, is_epoch),
source_epoch (source_epoch_a)
{
}

size_t ysu::block_sideband::size (ysu::block_type type_a)
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
		static_assert (sizeof (ysu::epoch) == ysu::block_details::size (), "block_details is larger than the epoch enum");
		result += ysu::block_details::size () + sizeof (ysu::epoch);
	}
	return result;
}

void ysu::block_sideband::serialize (ysu::stream & stream_a, ysu::block_type type_a) const
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
		ysu::write (stream_a, static_cast<uint8_t> (source_epoch));
	}
}

bool ysu::block_sideband::deserialize (ysu::stream & stream_a, ysu::block_type type_a)
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
			uint8_t source_epoch_uint8_t{ 0 };
			ysu::read (stream_a, source_epoch_uint8_t);
			source_epoch = static_cast<ysu::epoch> (source_epoch_uint8_t);
		}
	}
	catch (std::runtime_error &)
	{
		result = true;
	}

	return result;
}

std::shared_ptr<ysu::block> ysu::block_uniquer::unique (std::shared_ptr<ysu::block> block_a)
{
	auto result (block_a);
	if (result != nullptr)
	{
		ysu::uint256_union key (block_a->full_hash ());
		ysu::lock_guard<std::mutex> lock (mutex);
		auto & existing (blocks[key]);
		if (auto block_l = existing.lock ())
		{
			result = block_l;
		}
		else
		{
			existing = block_a;
		}
		release_assert (std::numeric_limits<CryptoPP::word32>::max () > blocks.size ());
		for (auto i (0); i < cleanup_count && !blocks.empty (); ++i)
		{
			auto random_offset (ysu::random_pool::generate_word32 (0, static_cast<CryptoPP::word32> (blocks.size () - 1)));
			auto existing (std::next (blocks.begin (), random_offset));
			if (existing == blocks.end ())
			{
				existing = blocks.begin ();
			}
			if (existing != blocks.end ())
			{
				if (auto block_l = existing->second.lock ())
				{
					// Still live
				}
				else
				{
					blocks.erase (existing);
				}
			}
		}
	}
	return result;
}

size_t ysu::block_uniquer::size ()
{
	ysu::lock_guard<std::mutex> lock (mutex);
	return blocks.size ();
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (block_uniquer & block_uniquer, const std::string & name)
{
	auto count = block_uniquer.size ();
	auto sizeof_element = sizeof (block_uniquer::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "blocks", count, sizeof_element }));
	return composite;
}
