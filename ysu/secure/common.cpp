#include <ysu/crypto_lib/random_pool.hpp>
#include <ysu/lib/config.hpp>
#include <ysu/lib/numbers.hpp>
#include <ysu/secure/blockstore.hpp>
#include <ysu/secure/common.hpp>

#include <crypto/cryptopp/words.h>

#include <boost/endian/conversion.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/variant/get.hpp>

#include <limits>
#include <queue>

#include <crypto/ed25519-donna/ed25519.h>

size_t constexpr ysu::send_block::size;
size_t constexpr ysu::receive_block::size;
size_t constexpr ysu::open_block::size;
size_t constexpr ysu::change_block::size;
size_t constexpr ysu::state_block::size;

ysu::ysu_networks ysu::network_constants::active_network = ysu::ysu_networks::ACTIVE_NETWORK;

namespace
{
char const * dev_private_key_data = "34F0A37AAD20F4A260F0A5B3CB3D7FB50673212263E58A380BC10474BB039CE4";
char const * dev_public_key_data = "B0311EA55708D6A53C75CDBF88300259C6D018522FE3D4D0A242E431F9E8B6D0"; // xrb_3e3j5tkog48pnny9dmfzj1r16pg8t1e76dz5tmac6iq689wyjfpiij4txtdo
char const * beta_public_key_data = "A59A439B34662385D48F7FF9CA50030F889BAA9AC320EA5A85AAD777CF82B088"; // ysu_3betagfmasj5iqcayzzssba185wamgobois1xbfadcpqgz9r7e6a1zwztn5o
char const * live_public_key_data = "E89208DD038FBB269987689621D52292AE9C35941A7484756ECCED92A65093BA"; // xrb_3t6k35gi95xu6tergt6p69ck76ogmitsa8mnijtpxm9fkcm736xtoncuohr3
char const * test_public_key_data = "45C6FF9D1706D61F0821327752671BDA9F9ED2DA40326B01935AB566FB9E08ED"; // ysu_1jg8zygjg3pp5w644emqcbmjqpnzmubfni3kfe1s8pooeuxsw49fdq1mco9j
char const * dev_genesis_data = R"%%%({
	"type": "open",
	"source": "B0311EA55708D6A53C75CDBF88300259C6D018522FE3D4D0A242E431F9E8B6D0",
	"representative": "xrb_3e3j5tkog48pnny9dmfzj1r16pg8t1e76dz5tmac6iq689wyjfpiij4txtdo",
	"account": "xrb_3e3j5tkog48pnny9dmfzj1r16pg8t1e76dz5tmac6iq689wyjfpiij4txtdo",
	"work": "7b42a00ee91d5810",
	"signature": "ECDA914373A2F0CA1296475BAEE40500A7F0A7AD72A5A80C81D7FAB7F6C802B2CC7DB50F5DD0FB25B2EF11761FA7344A158DD5A700B21BD47DE5BD0F63153A02"
	})%%%";

char const * beta_genesis_data = R"%%%({
	"type": "open",
	"source": "A59A439B34662385D48F7FF9CA50030F889BAA9AC320EA5A85AAD777CF82B088",
	"representative": "ysu_3betagfmasj5iqcayzzssba185wamgobois1xbfadcpqgz9r7e6a1zwztn5o",
	"account": "ysu_3betagfmasj5iqcayzzssba185wamgobois1xbfadcpqgz9r7e6a1zwztn5o",
	"work": "a870b0e9331cf477",
	"signature": "2F4D72B8E973C979E4D6815CB34C2F426AD997FB8BC6BD94C92541E7F35879594A392AA0B28D0A865EA4C73DB2DE56893E947FD0AD76AB847A2BB5AEDFBF0E00"
	})%%%";

char const * live_genesis_data = R"%%%({
	"type": "open",
	"source": "E89208DD038FBB269987689621D52292AE9C35941A7484756ECCED92A65093BA",
	"representative": "xrb_3t6k35gi95xu6tergt6p69ck76ogmitsa8mnijtpxm9fkcm736xtoncuohr3",
	"account": "xrb_3t6k35gi95xu6tergt6p69ck76ogmitsa8mnijtpxm9fkcm736xtoncuohr3",
	"work": "62f05417dd3fb691",
	"signature": "9F0C933C8ADE004D808EA1985FA746A7E95BA2A38F867640F53EC8F180BDFE9E2C1268DEAD7C2664F356E37ABA362BC58E46DBA03E523A7B5A19E4B6EB12BB02"
	})%%%";

char const * test_genesis_data = R"%%%({
	"type": "open",
	"source": "45C6FF9D1706D61F0821327752671BDA9F9ED2DA40326B01935AB566FB9E08ED",
	"representative": "ysu_1jg8zygjg3pp5w644emqcbmjqpnzmubfni3kfe1s8pooeuxsw49fdq1mco9j",
	"account": "ysu_1jg8zygjg3pp5w644emqcbmjqpnzmubfni3kfe1s8pooeuxsw49fdq1mco9j",
	"work": "bc1ef279c1a34eb1",
	"signature": "15049467CAEE3EC768639E8E35792399B6078DA763DA4EBA8ECAD33B0EDC4AF2E7403893A5A602EB89B978DABEF1D6606BB00F3C0EE11449232B143B6E07170E"
	})%%%";

std::shared_ptr<ysu::block> parse_block_from_genesis_data (std::string const & genesis_data_a)
{
	boost::property_tree::ptree tree;
	std::stringstream istream (genesis_data_a);
	boost::property_tree::read_json (istream, tree);
	return ysu::deserialize_block_json (tree);
}
}

ysu::network_params::network_params () :
network_params (network_constants::active_network)
{
}

ysu::network_params::network_params (ysu::ysu_networks network_a) :
network (network_a), ledger (network), voting (network), node (network), portmapping (network), bootstrap (network)
{
	unsigned constexpr kdf_full_work = 64 * 1024;
	unsigned constexpr kdf_dev_work = 8;
	kdf_work = network.is_dev_network () ? kdf_dev_work : kdf_full_work;
	header_magic_number = network.is_dev_network () ? std::array<uint8_t, 2>{ { 'R', 'A' } } : network.is_beta_network () ? std::array<uint8_t, 2>{ { 'N', 'B' } } : network.is_live_network () ? std::array<uint8_t, 2>{ { 'R', 'C' } } : std::array<uint8_t, 2>{ { 'R', 'X' } };
}

uint8_t ysu::protocol_constants::protocol_version_min (bool use_epoch_2_min_version_a) const
{
	return use_epoch_2_min_version_a ? protocol_version_min_epoch_2 : protocol_version_min_pre_epoch_2;
}

ysu::ledger_constants::ledger_constants (ysu::network_constants & network_constants) :
ledger_constants (network_constants.network ())
{
}

ysu::ledger_constants::ledger_constants (ysu::ysu_networks network_a) :
zero_key ("0"),
dev_genesis_key (dev_private_key_data),
ysu_dev_account (dev_public_key_data),
ysu_beta_account (beta_public_key_data),
ysu_live_account (live_public_key_data),
ysu_test_account (test_public_key_data),
ysu_dev_genesis (dev_genesis_data),
ysu_beta_genesis (beta_genesis_data),
ysu_live_genesis (live_genesis_data),
ysu_test_genesis (test_genesis_data),
genesis_account (network_a == ysu::ysu_networks::ysu_dev_network ? ysu_dev_account : network_a == ysu::ysu_networks::ysu_beta_network ? ysu_beta_account : network_a == ysu::ysu_networks::ysu_test_network ? ysu_test_account : ysu_live_account),
genesis_block (network_a == ysu::ysu_networks::ysu_dev_network ? ysu_dev_genesis : network_a == ysu::ysu_networks::ysu_beta_network ? ysu_beta_genesis : network_a == ysu::ysu_networks::ysu_test_network ? ysu_test_genesis : ysu_live_genesis),
genesis_hash (parse_block_from_genesis_data (genesis_block)->hash ()),
genesis_amount (std::numeric_limits<ysu::uint128_t>::max ()),
burn_account (0)
{
	ysu::link epoch_link_v1;
	const char * epoch_message_v1 ("epoch v1 block");
	strncpy ((char *)epoch_link_v1.bytes.data (), epoch_message_v1, epoch_link_v1.bytes.size ());
	epochs.add (ysu::epoch::epoch_1, genesis_account, epoch_link_v1);

	ysu::link epoch_link_v2;
	ysu::account ysu_live_epoch_v2_signer;
	auto error (ysu_live_epoch_v2_signer.decode_account ("ysu_3qb6o6i1tkzr6jwr5s7eehfxwg9x6eemitdinbpi7u8bjjwsgqfj4wzser3x"));
	debug_assert (!error);
	auto epoch_v2_signer (network_a == ysu::ysu_networks::ysu_dev_network ? ysu_dev_account : network_a == ysu::ysu_networks::ysu_beta_network ? ysu_beta_account : network_a == ysu::ysu_networks::ysu_test_network ? ysu_test_account : ysu_live_epoch_v2_signer);
	const char * epoch_message_v2 ("epoch v2 block");
	strncpy ((char *)epoch_link_v2.bytes.data (), epoch_message_v2, epoch_link_v2.bytes.size ());
	epochs.add (ysu::epoch::epoch_2, epoch_v2_signer, epoch_link_v2);
}

ysu::random_constants::random_constants ()
{
	ysu::random_pool::generate_block (not_an_account.bytes.data (), not_an_account.bytes.size ());
	ysu::random_pool::generate_block (random_128.bytes.data (), random_128.bytes.size ());
}

ysu::node_constants::node_constants (ysu::network_constants & network_constants)
{
	period = network_constants.is_dev_network () ? std::chrono::seconds (1) : std::chrono::seconds (60);
	half_period = network_constants.is_dev_network () ? std::chrono::milliseconds (500) : std::chrono::milliseconds (30 * 1000);
	idle_timeout = network_constants.is_dev_network () ? period * 15 : period * 2;
	cutoff = period * 5;
	syn_cookie_cutoff = std::chrono::seconds (5);
	backup_interval = std::chrono::minutes (5);
	bootstrap_interval = std::chrono::seconds (15 * 60);
	search_pending_interval = network_constants.is_dev_network () ? std::chrono::seconds (1) : std::chrono::seconds (5 * 60);
	peer_interval = search_pending_interval;
	unchecked_cleaning_interval = std::chrono::minutes (30);
	process_confirmed_interval = network_constants.is_dev_network () ? std::chrono::milliseconds (50) : std::chrono::milliseconds (500);
	max_peers_per_ip = network_constants.is_dev_network () ? 10 : 5;
	max_weight_samples = (network_constants.is_live_network () || network_constants.is_test_network ()) ? 4032 : 288;
	weight_period = 5 * 60; // 5 minutes
}

ysu::voting_constants::voting_constants (ysu::network_constants & network_constants)
{
	max_cache = network_constants.is_dev_network () ? 256 : 128 * 1024;
}

ysu::portmapping_constants::portmapping_constants (ysu::network_constants & network_constants)
{
	lease_duration = std::chrono::seconds (1787); // ~30 minutes
	health_check_period = std::chrono::seconds (53);
}

ysu::bootstrap_constants::bootstrap_constants (ysu::network_constants & network_constants)
{
	lazy_max_pull_blocks = network_constants.is_dev_network () ? 2 : 512;
	lazy_min_pull_blocks = network_constants.is_dev_network () ? 1 : 32;
	frontier_retry_limit = network_constants.is_dev_network () ? 2 : 16;
	lazy_retry_limit = network_constants.is_dev_network () ? 2 : frontier_retry_limit * 10;
	lazy_destinations_retry_limit = network_constants.is_dev_network () ? 1 : frontier_retry_limit / 4;
	gap_cache_bootstrap_start_interval = network_constants.is_dev_network () ? std::chrono::milliseconds (5) : std::chrono::milliseconds (30 * 1000);
}

// Create a new random keypair
ysu::keypair::keypair ()
{
	random_pool::generate_block (prv.data.bytes.data (), prv.data.bytes.size ());
	ed25519_publickey (prv.data.bytes.data (), pub.bytes.data ());
}

// Create a keypair given a private key
ysu::keypair::keypair (ysu::raw_key && prv_a) :
prv (std::move (prv_a))
{
	ed25519_publickey (prv.data.bytes.data (), pub.bytes.data ());
}

// Create a keypair given a hex string of the private key
ysu::keypair::keypair (std::string const & prv_a)
{
	auto error (prv.data.decode_hex (prv_a));
	(void)error;
	debug_assert (!error);
	ed25519_publickey (prv.data.bytes.data (), pub.bytes.data ());
}

// Serialize a block prefixed with an 8-bit typecode
void ysu::serialize_block (ysu::stream & stream_a, ysu::block const & block_a)
{
	write (stream_a, block_a.type ());
	block_a.serialize (stream_a);
}

ysu::account_info::account_info (ysu::block_hash const & head_a, ysu::account const & representative_a, ysu::block_hash const & open_block_a, ysu::amount const & balance_a, uint64_t modified_a, uint64_t block_count_a, ysu::epoch epoch_a) :
head (head_a),
representative (representative_a),
open_block (open_block_a),
balance (balance_a),
modified (modified_a),
block_count (block_count_a),
epoch_m (epoch_a)
{
}

bool ysu::account_info::deserialize (ysu::stream & stream_a)
{
	auto error (false);
	try
	{
		ysu::read (stream_a, head.bytes);
		ysu::read (stream_a, representative.bytes);
		ysu::read (stream_a, open_block.bytes);
		ysu::read (stream_a, balance.bytes);
		ysu::read (stream_a, modified);
		ysu::read (stream_a, block_count);
		ysu::read (stream_a, epoch_m);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

bool ysu::account_info::operator== (ysu::account_info const & other_a) const
{
	return head == other_a.head && representative == other_a.representative && open_block == other_a.open_block && balance == other_a.balance && modified == other_a.modified && block_count == other_a.block_count && epoch () == other_a.epoch ();
}

bool ysu::account_info::operator!= (ysu::account_info const & other_a) const
{
	return !(*this == other_a);
}

size_t ysu::account_info::db_size () const
{
	debug_assert (reinterpret_cast<const uint8_t *> (this) == reinterpret_cast<const uint8_t *> (&head));
	debug_assert (reinterpret_cast<const uint8_t *> (&head) + sizeof (head) == reinterpret_cast<const uint8_t *> (&representative));
	debug_assert (reinterpret_cast<const uint8_t *> (&representative) + sizeof (representative) == reinterpret_cast<const uint8_t *> (&open_block));
	debug_assert (reinterpret_cast<const uint8_t *> (&open_block) + sizeof (open_block) == reinterpret_cast<const uint8_t *> (&balance));
	debug_assert (reinterpret_cast<const uint8_t *> (&balance) + sizeof (balance) == reinterpret_cast<const uint8_t *> (&modified));
	debug_assert (reinterpret_cast<const uint8_t *> (&modified) + sizeof (modified) == reinterpret_cast<const uint8_t *> (&block_count));
	debug_assert (reinterpret_cast<const uint8_t *> (&block_count) + sizeof (block_count) == reinterpret_cast<const uint8_t *> (&epoch_m));
	return sizeof (head) + sizeof (representative) + sizeof (open_block) + sizeof (balance) + sizeof (modified) + sizeof (block_count) + sizeof (epoch_m);
}

ysu::epoch ysu::account_info::epoch () const
{
	return epoch_m;
}

ysu::pending_info::pending_info (ysu::account const & source_a, ysu::amount const & amount_a, ysu::epoch epoch_a) :
source (source_a),
amount (amount_a),
epoch (epoch_a)
{
}

bool ysu::pending_info::deserialize (ysu::stream & stream_a)
{
	auto error (false);
	try
	{
		ysu::read (stream_a, source.bytes);
		ysu::read (stream_a, amount.bytes);
		ysu::read (stream_a, epoch);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

size_t ysu::pending_info::db_size () const
{
	return sizeof (source) + sizeof (amount) + sizeof (epoch);
}

bool ysu::pending_info::operator== (ysu::pending_info const & other_a) const
{
	return source == other_a.source && amount == other_a.amount && epoch == other_a.epoch;
}

ysu::pending_key::pending_key (ysu::account const & account_a, ysu::block_hash const & hash_a) :
account (account_a),
hash (hash_a)
{
}

bool ysu::pending_key::deserialize (ysu::stream & stream_a)
{
	auto error (false);
	try
	{
		ysu::read (stream_a, account.bytes);
		ysu::read (stream_a, hash.bytes);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

bool ysu::pending_key::operator== (ysu::pending_key const & other_a) const
{
	return account == other_a.account && hash == other_a.hash;
}

ysu::account const & ysu::pending_key::key () const
{
	return account;
}

ysu::unchecked_info::unchecked_info (std::shared_ptr<ysu::block> block_a, ysu::account const & account_a, uint64_t modified_a, ysu::signature_verification verified_a, bool confirmed_a) :
block (block_a),
account (account_a),
modified (modified_a),
verified (verified_a),
confirmed (confirmed_a)
{
}

void ysu::unchecked_info::serialize (ysu::stream & stream_a) const
{
	debug_assert (block != nullptr);
	ysu::serialize_block (stream_a, *block);
	ysu::write (stream_a, account.bytes);
	ysu::write (stream_a, modified);
	ysu::write (stream_a, verified);
}

bool ysu::unchecked_info::deserialize (ysu::stream & stream_a)
{
	block = ysu::deserialize_block (stream_a);
	bool error (block == nullptr);
	if (!error)
	{
		try
		{
			ysu::read (stream_a, account.bytes);
			ysu::read (stream_a, modified);
			ysu::read (stream_a, verified);
		}
		catch (std::runtime_error const &)
		{
			error = true;
		}
	}
	return error;
}

ysu::endpoint_key::endpoint_key (const std::array<uint8_t, 16> & address_a, uint16_t port_a) :
address (address_a), network_port (boost::endian::native_to_big (port_a))
{
}

const std::array<uint8_t, 16> & ysu::endpoint_key::address_bytes () const
{
	return address;
}

uint16_t ysu::endpoint_key::port () const
{
	return boost::endian::big_to_native (network_port);
}

ysu::confirmation_height_info::confirmation_height_info (uint64_t confirmation_height_a, ysu::block_hash const & confirmed_frontier_a) :
height (confirmation_height_a),
frontier (confirmed_frontier_a)
{
}

void ysu::confirmation_height_info::serialize (ysu::stream & stream_a) const
{
	ysu::write (stream_a, height);
	ysu::write (stream_a, frontier);
}

bool ysu::confirmation_height_info::deserialize (ysu::stream & stream_a)
{
	auto error (false);
	try
	{
		ysu::read (stream_a, height);
		ysu::read (stream_a, frontier);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}
	return error;
}

ysu::block_info::block_info (ysu::account const & account_a, ysu::amount const & balance_a) :
account (account_a),
balance (balance_a)
{
}

bool ysu::vote::operator== (ysu::vote const & other_a) const
{
	auto blocks_equal (true);
	if (blocks.size () != other_a.blocks.size ())
	{
		blocks_equal = false;
	}
	else
	{
		for (auto i (0); blocks_equal && i < blocks.size (); ++i)
		{
			auto block (blocks[i]);
			auto other_block (other_a.blocks[i]);
			if (block.which () != other_block.which ())
			{
				blocks_equal = false;
			}
			else if (block.which ())
			{
				if (boost::get<ysu::block_hash> (block) != boost::get<ysu::block_hash> (other_block))
				{
					blocks_equal = false;
				}
			}
			else
			{
				if (!(*boost::get<std::shared_ptr<ysu::block>> (block) == *boost::get<std::shared_ptr<ysu::block>> (other_block)))
				{
					blocks_equal = false;
				}
			}
		}
	}
	return sequence == other_a.sequence && blocks_equal && account == other_a.account && signature == other_a.signature;
}

bool ysu::vote::operator!= (ysu::vote const & other_a) const
{
	return !(*this == other_a);
}

void ysu::vote::serialize_json (boost::property_tree::ptree & tree) const
{
	tree.put ("account", account.to_account ());
	tree.put ("signature", signature.number ());
	tree.put ("sequence", std::to_string (sequence));
	boost::property_tree::ptree blocks_tree;
	for (auto block : blocks)
	{
		boost::property_tree::ptree entry;
		if (block.which ())
		{
			entry.put ("", boost::get<ysu::block_hash> (block).to_string ());
		}
		else
		{
			entry.put ("", boost::get<std::shared_ptr<ysu::block>> (block)->hash ().to_string ());
		}
		blocks_tree.push_back (std::make_pair ("", entry));
	}
	tree.add_child ("blocks", blocks_tree);
}

std::string ysu::vote::to_json () const
{
	std::stringstream stream;
	boost::property_tree::ptree tree;
	serialize_json (tree);
	boost::property_tree::write_json (stream, tree);
	return stream.str ();
}

ysu::vote::vote (ysu::vote const & other_a) :
sequence (other_a.sequence),
blocks (other_a.blocks),
account (other_a.account),
signature (other_a.signature)
{
}

ysu::vote::vote (bool & error_a, ysu::stream & stream_a, ysu::block_uniquer * uniquer_a)
{
	error_a = deserialize (stream_a, uniquer_a);
}

ysu::vote::vote (bool & error_a, ysu::stream & stream_a, ysu::block_type type_a, ysu::block_uniquer * uniquer_a)
{
	try
	{
		ysu::read (stream_a, account.bytes);
		ysu::read (stream_a, signature.bytes);
		ysu::read (stream_a, sequence);

		while (stream_a.in_avail () > 0)
		{
			if (type_a == ysu::block_type::not_a_block)
			{
				ysu::block_hash block_hash;
				ysu::read (stream_a, block_hash);
				blocks.push_back (block_hash);
			}
			else
			{
				std::shared_ptr<ysu::block> block (ysu::deserialize_block (stream_a, type_a, uniquer_a));
				if (block == nullptr)
				{
					throw std::runtime_error ("Block is null");
				}
				blocks.push_back (block);
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error_a = true;
	}

	if (blocks.empty ())
	{
		error_a = true;
	}
}

ysu::vote::vote (ysu::account const & account_a, ysu::raw_key const & prv_a, uint64_t sequence_a, std::shared_ptr<ysu::block> block_a) :
sequence (sequence_a),
blocks (1, block_a),
account (account_a),
signature (ysu::sign_message (prv_a, account_a, hash ()))
{
}

ysu::vote::vote (ysu::account const & account_a, ysu::raw_key const & prv_a, uint64_t sequence_a, std::vector<ysu::block_hash> const & blocks_a) :
sequence (sequence_a),
account (account_a)
{
	debug_assert (!blocks_a.empty ());
	debug_assert (blocks_a.size () <= 12);
	blocks.reserve (blocks_a.size ());
	std::copy (blocks_a.cbegin (), blocks_a.cend (), std::back_inserter (blocks));
	signature = ysu::sign_message (prv_a, account_a, hash ());
}

std::string ysu::vote::hashes_string () const
{
	std::string result;
	for (auto hash : *this)
	{
		result += hash.to_string ();
		result += ", ";
	}
	return result;
}

const std::string ysu::vote::hash_prefix = "vote ";

ysu::block_hash ysu::vote::hash () const
{
	ysu::block_hash result;
	blake2b_state hash;
	blake2b_init (&hash, sizeof (result.bytes));
	if (blocks.size () > 1 || (!blocks.empty () && blocks.front ().which ()))
	{
		blake2b_update (&hash, hash_prefix.data (), hash_prefix.size ());
	}
	for (auto block_hash : *this)
	{
		blake2b_update (&hash, block_hash.bytes.data (), sizeof (block_hash.bytes));
	}
	union
	{
		uint64_t qword;
		std::array<uint8_t, 8> bytes;
	};
	qword = sequence;
	blake2b_update (&hash, bytes.data (), sizeof (bytes));
	blake2b_final (&hash, result.bytes.data (), sizeof (result.bytes));
	return result;
}

ysu::block_hash ysu::vote::full_hash () const
{
	ysu::block_hash result;
	blake2b_state state;
	blake2b_init (&state, sizeof (result.bytes));
	blake2b_update (&state, hash ().bytes.data (), sizeof (hash ().bytes));
	blake2b_update (&state, account.bytes.data (), sizeof (account.bytes.data ()));
	blake2b_update (&state, signature.bytes.data (), sizeof (signature.bytes.data ()));
	blake2b_final (&state, result.bytes.data (), sizeof (result.bytes));
	return result;
}

void ysu::vote::serialize (ysu::stream & stream_a, ysu::block_type type) const
{
	write (stream_a, account);
	write (stream_a, signature);
	write (stream_a, sequence);
	for (auto const & block : blocks)
	{
		if (block.which ())
		{
			debug_assert (type == ysu::block_type::not_a_block);
			write (stream_a, boost::get<ysu::block_hash> (block));
		}
		else
		{
			if (type == ysu::block_type::not_a_block)
			{
				write (stream_a, boost::get<std::shared_ptr<ysu::block>> (block)->hash ());
			}
			else
			{
				boost::get<std::shared_ptr<ysu::block>> (block)->serialize (stream_a);
			}
		}
	}
}

void ysu::vote::serialize (ysu::stream & stream_a) const
{
	write (stream_a, account);
	write (stream_a, signature);
	write (stream_a, sequence);
	for (auto const & block : blocks)
	{
		if (block.which ())
		{
			write (stream_a, ysu::block_type::not_a_block);
			write (stream_a, boost::get<ysu::block_hash> (block));
		}
		else
		{
			ysu::serialize_block (stream_a, *boost::get<std::shared_ptr<ysu::block>> (block));
		}
	}
}

bool ysu::vote::deserialize (ysu::stream & stream_a, ysu::block_uniquer * uniquer_a)
{
	auto error (false);
	try
	{
		ysu::read (stream_a, account);
		ysu::read (stream_a, signature);
		ysu::read (stream_a, sequence);

		ysu::block_type type;

		while (true)
		{
			if (ysu::try_read (stream_a, type))
			{
				// Reached the end of the stream
				break;
			}

			if (type == ysu::block_type::not_a_block)
			{
				ysu::block_hash block_hash;
				ysu::read (stream_a, block_hash);
				blocks.push_back (block_hash);
			}
			else
			{
				std::shared_ptr<ysu::block> block (ysu::deserialize_block (stream_a, type, uniquer_a));
				if (block == nullptr)
				{
					throw std::runtime_error ("Block is empty");
				}

				blocks.push_back (block);
			}
		}
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	if (blocks.empty ())
	{
		error = true;
	}

	return error;
}

bool ysu::vote::validate () const
{
	return ysu::validate_message (account, hash (), signature);
}

ysu::block_hash ysu::iterate_vote_blocks_as_hash::operator() (boost::variant<std::shared_ptr<ysu::block>, ysu::block_hash> const & item) const
{
	ysu::block_hash result;
	if (item.which ())
	{
		result = boost::get<ysu::block_hash> (item);
	}
	else
	{
		result = boost::get<std::shared_ptr<ysu::block>> (item)->hash ();
	}
	return result;
}

boost::transform_iterator<ysu::iterate_vote_blocks_as_hash, ysu::vote_blocks_vec_iter> ysu::vote::begin () const
{
	return boost::transform_iterator<ysu::iterate_vote_blocks_as_hash, ysu::vote_blocks_vec_iter> (blocks.begin (), ysu::iterate_vote_blocks_as_hash ());
}

boost::transform_iterator<ysu::iterate_vote_blocks_as_hash, ysu::vote_blocks_vec_iter> ysu::vote::end () const
{
	return boost::transform_iterator<ysu::iterate_vote_blocks_as_hash, ysu::vote_blocks_vec_iter> (blocks.end (), ysu::iterate_vote_blocks_as_hash ());
}

ysu::vote_uniquer::vote_uniquer (ysu::block_uniquer & uniquer_a) :
uniquer (uniquer_a)
{
}

std::shared_ptr<ysu::vote> ysu::vote_uniquer::unique (std::shared_ptr<ysu::vote> vote_a)
{
	auto result (vote_a);
	if (result != nullptr && !result->blocks.empty ())
	{
		if (!result->blocks.front ().which ())
		{
			result->blocks.front () = uniquer.unique (boost::get<std::shared_ptr<ysu::block>> (result->blocks.front ()));
		}
		ysu::block_hash key (vote_a->full_hash ());
		ysu::lock_guard<std::mutex> lock (mutex);
		auto & existing (votes[key]);
		if (auto block_l = existing.lock ())
		{
			result = block_l;
		}
		else
		{
			existing = vote_a;
		}

		release_assert (std::numeric_limits<CryptoPP::word32>::max () > votes.size ());
		for (auto i (0); i < cleanup_count && !votes.empty (); ++i)
		{
			auto random_offset = ysu::random_pool::generate_word32 (0, static_cast<CryptoPP::word32> (votes.size () - 1));

			auto existing (std::next (votes.begin (), random_offset));
			if (existing == votes.end ())
			{
				existing = votes.begin ();
			}
			if (existing != votes.end ())
			{
				if (auto block_l = existing->second.lock ())
				{
					// Still live
				}
				else
				{
					votes.erase (existing);
				}
			}
		}
	}
	return result;
}

size_t ysu::vote_uniquer::size ()
{
	ysu::lock_guard<std::mutex> lock (mutex);
	return votes.size ();
}

std::unique_ptr<ysu::container_info_component> ysu::collect_container_info (vote_uniquer & vote_uniquer, const std::string & name)
{
	auto count = vote_uniquer.size ();
	auto sizeof_element = sizeof (vote_uniquer::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "votes", count, sizeof_element }));
	return composite;
}

ysu::genesis::genesis ()
{
	static ysu::network_params network_params;
	open = parse_block_from_genesis_data (network_params.ledger.genesis_block);
	debug_assert (open != nullptr);
}

ysu::block_hash ysu::genesis::hash () const
{
	return open->hash ();
}

ysu::wallet_id ysu::random_wallet_id ()
{
	ysu::wallet_id wallet_id;
	ysu::uint256_union dummy_secret;
	random_pool::generate_block (dummy_secret.bytes.data (), dummy_secret.bytes.size ());
	ed25519_publickey (dummy_secret.bytes.data (), wallet_id.bytes.data ());
	return wallet_id;
}

ysu::unchecked_key::unchecked_key (ysu::block_hash const & previous_a, ysu::block_hash const & hash_a) :
previous (previous_a),
hash (hash_a)
{
}

bool ysu::unchecked_key::deserialize (ysu::stream & stream_a)
{
	auto error (false);
	try
	{
		ysu::read (stream_a, previous.bytes);
		ysu::read (stream_a, hash.bytes);
	}
	catch (std::runtime_error const &)
	{
		error = true;
	}

	return error;
}

bool ysu::unchecked_key::operator== (ysu::unchecked_key const & other_a) const
{
	return previous == other_a.previous && hash == other_a.hash;
}

ysu::block_hash const & ysu::unchecked_key::key () const
{
	return previous;
}

void ysu::generate_cache::enable_all ()
{
	reps = true;
	cemented_count = true;
	unchecked_count = true;
	account_count = true;
	epoch_2 = true;
}
