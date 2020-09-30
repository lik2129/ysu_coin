#pragma once

#include <ysu/crypto/blake2/blake2.h>
#include <ysu/lib/epoch.hpp>
#include <ysu/lib/errors.hpp>
#include <ysu/lib/numbers.hpp>
#include <ysu/lib/optional_ptr.hpp>
#include <ysu/lib/stream.hpp>
#include <ysu/lib/utility.hpp>
#include <ysu/lib/work.hpp>

#include <boost/property_tree/ptree_fwd.hpp>

#include <unordered_map>

namespace ysu
{
class block_visitor;
class mutable_block_visitor;
enum class block_type : uint8_t
{
	invalid = 0,
	not_a_block = 1,
	send = 2,
	receive = 3,
	open = 4,
	change = 5,
	state = 6
};
class block_details
{
	static_assert (std::is_same<std::underlying_type<ysu::epoch>::type, uint8_t> (), "Epoch enum is not the proper type");
	static_assert (static_cast<uint8_t> (ysu::epoch::max) < (1 << 5), "Epoch max is too large for the sideband");

public:
	block_details () = default;
	block_details (ysu::epoch const epoch_a, bool const is_send_a, bool const is_receive_a, bool const is_epoch_a);
	static constexpr size_t size ()
	{
		return 1;
	}
	bool operator== (block_details const & other_a) const;
	void serialize (ysu::stream &) const;
	bool deserialize (ysu::stream &);
	ysu::epoch epoch{ ysu::epoch::epoch_0 };
	bool is_send{ false };
	bool is_receive{ false };
	bool is_epoch{ false };

private:
	uint8_t packed () const;
	void unpack (uint8_t);
};

std::string state_subtype (ysu::block_details const);

class block_sideband final
{
public:
	block_sideband () = default;
	block_sideband (ysu::account const &, ysu::block_hash const &, ysu::amount const &, uint64_t const, uint64_t const, ysu::block_details const &, ysu::epoch const source_epoch_a);
	block_sideband (ysu::account const &, ysu::block_hash const &, ysu::amount const &, uint64_t const, uint64_t const, ysu::epoch const epoch_a, bool const is_send, bool const is_receive, bool const is_epoch, ysu::epoch const source_epoch_a);
	void serialize (ysu::stream &, ysu::block_type) const;
	bool deserialize (ysu::stream &, ysu::block_type);
	static size_t size (ysu::block_type);
	ysu::block_hash successor{ 0 };
	ysu::account account{ 0 };
	ysu::amount balance{ 0 };
	uint64_t height{ 0 };
	uint64_t timestamp{ 0 };
	ysu::block_details details;
	ysu::epoch source_epoch{ ysu::epoch::epoch_0 };
};
class block
{
public:
	// Return a digest of the hashables in this block.
	ysu::block_hash const & hash () const;
	// Return a digest of hashables and non-hashables in this block.
	ysu::block_hash full_hash () const;
	ysu::block_sideband const & sideband () const;
	void sideband_set (ysu::block_sideband const &);
	bool has_sideband () const;
	std::string to_json () const;
	virtual void hash (blake2b_state &) const = 0;
	virtual uint64_t block_work () const = 0;
	virtual void block_work_set (uint64_t) = 0;
	virtual ysu::account const & account () const;
	// Previous block in account's chain, zero for open block
	virtual ysu::block_hash const & previous () const = 0;
	// Source block for open/receive blocks, zero otherwise.
	virtual ysu::block_hash const & source () const;
	// Previous block or account number for open blocks
	virtual ysu::root const & root () const = 0;
	// Qualified root value based on previous() and root()
	virtual ysu::qualified_root qualified_root () const;
	// Link field for state blocks, zero otherwise.
	virtual ysu::link const & link () const;
	virtual ysu::account const & representative () const;
	virtual ysu::amount const & balance () const;
	virtual void serialize (ysu::stream &) const = 0;
	virtual void serialize_json (std::string &, bool = false) const = 0;
	virtual void serialize_json (boost::property_tree::ptree &) const = 0;
	virtual void visit (ysu::block_visitor &) const = 0;
	virtual void visit (ysu::mutable_block_visitor &) = 0;
	virtual bool operator== (ysu::block const &) const = 0;
	virtual ysu::block_type type () const = 0;
	virtual ysu::signature const & block_signature () const = 0;
	virtual void signature_set (ysu::signature const &) = 0;
	virtual ~block () = default;
	virtual bool valid_predecessor (ysu::block const &) const = 0;
	static size_t size (ysu::block_type);
	virtual ysu::work_version work_version () const;
	uint64_t difficulty () const;
	// If there are any changes to the hashables, call this to update the cached hash
	void refresh ();

protected:
	mutable ysu::block_hash cached_hash{ 0 };
	/**
	 * Contextual details about a block, some fields may or may not be set depending on block type.
	 * This field is set via sideband_set in ledger processing or deserializing blocks from the database.
	 * Otherwise it may be null (for example, an old block or fork).
	 */
	ysu::optional_ptr<ysu::block_sideband> sideband_m;

private:
	ysu::block_hash generate_hash () const;
};
class send_hashables
{
public:
	send_hashables () = default;
	send_hashables (ysu::block_hash const &, ysu::account const &, ysu::amount const &);
	send_hashables (bool &, ysu::stream &);
	send_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	ysu::block_hash previous;
	ysu::account destination;
	ysu::amount balance;
	static size_t constexpr size = sizeof (previous) + sizeof (destination) + sizeof (balance);
};
class send_block : public ysu::block
{
public:
	send_block () = default;
	send_block (ysu::block_hash const &, ysu::account const &, ysu::amount const &, ysu::raw_key const &, ysu::public_key const &, uint64_t);
	send_block (bool &, ysu::stream &);
	send_block (bool &, boost::property_tree::ptree const &);
	virtual ~send_block () = default;
	using ysu::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	ysu::block_hash const & previous () const override;
	ysu::root const & root () const override;
	ysu::amount const & balance () const override;
	void serialize (ysu::stream &) const override;
	bool deserialize (ysu::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (ysu::block_visitor &) const override;
	void visit (ysu::mutable_block_visitor &) override;
	ysu::block_type type () const override;
	ysu::signature const & block_signature () const override;
	void signature_set (ysu::signature const &) override;
	bool operator== (ysu::block const &) const override;
	bool operator== (ysu::send_block const &) const;
	bool valid_predecessor (ysu::block const &) const override;
	send_hashables hashables;
	ysu::signature signature;
	uint64_t work;
	static size_t constexpr size = ysu::send_hashables::size + sizeof (signature) + sizeof (work);
};
class receive_hashables
{
public:
	receive_hashables () = default;
	receive_hashables (ysu::block_hash const &, ysu::block_hash const &);
	receive_hashables (bool &, ysu::stream &);
	receive_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	ysu::block_hash previous;
	ysu::block_hash source;
	static size_t constexpr size = sizeof (previous) + sizeof (source);
};
class receive_block : public ysu::block
{
public:
	receive_block () = default;
	receive_block (ysu::block_hash const &, ysu::block_hash const &, ysu::raw_key const &, ysu::public_key const &, uint64_t);
	receive_block (bool &, ysu::stream &);
	receive_block (bool &, boost::property_tree::ptree const &);
	virtual ~receive_block () = default;
	using ysu::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	ysu::block_hash const & previous () const override;
	ysu::block_hash const & source () const override;
	ysu::root const & root () const override;
	void serialize (ysu::stream &) const override;
	bool deserialize (ysu::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (ysu::block_visitor &) const override;
	void visit (ysu::mutable_block_visitor &) override;
	ysu::block_type type () const override;
	ysu::signature const & block_signature () const override;
	void signature_set (ysu::signature const &) override;
	bool operator== (ysu::block const &) const override;
	bool operator== (ysu::receive_block const &) const;
	bool valid_predecessor (ysu::block const &) const override;
	receive_hashables hashables;
	ysu::signature signature;
	uint64_t work;
	static size_t constexpr size = ysu::receive_hashables::size + sizeof (signature) + sizeof (work);
};
class open_hashables
{
public:
	open_hashables () = default;
	open_hashables (ysu::block_hash const &, ysu::account const &, ysu::account const &);
	open_hashables (bool &, ysu::stream &);
	open_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	ysu::block_hash source;
	ysu::account representative;
	ysu::account account;
	static size_t constexpr size = sizeof (source) + sizeof (representative) + sizeof (account);
};
class open_block : public ysu::block
{
public:
	open_block () = default;
	open_block (ysu::block_hash const &, ysu::account const &, ysu::account const &, ysu::raw_key const &, ysu::public_key const &, uint64_t);
	open_block (ysu::block_hash const &, ysu::account const &, ysu::account const &, std::nullptr_t);
	open_block (bool &, ysu::stream &);
	open_block (bool &, boost::property_tree::ptree const &);
	virtual ~open_block () = default;
	using ysu::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	ysu::block_hash const & previous () const override;
	ysu::account const & account () const override;
	ysu::block_hash const & source () const override;
	ysu::root const & root () const override;
	ysu::account const & representative () const override;
	void serialize (ysu::stream &) const override;
	bool deserialize (ysu::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (ysu::block_visitor &) const override;
	void visit (ysu::mutable_block_visitor &) override;
	ysu::block_type type () const override;
	ysu::signature const & block_signature () const override;
	void signature_set (ysu::signature const &) override;
	bool operator== (ysu::block const &) const override;
	bool operator== (ysu::open_block const &) const;
	bool valid_predecessor (ysu::block const &) const override;
	ysu::open_hashables hashables;
	ysu::signature signature;
	uint64_t work;
	static size_t constexpr size = ysu::open_hashables::size + sizeof (signature) + sizeof (work);
};
class change_hashables
{
public:
	change_hashables () = default;
	change_hashables (ysu::block_hash const &, ysu::account const &);
	change_hashables (bool &, ysu::stream &);
	change_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	ysu::block_hash previous;
	ysu::account representative;
	static size_t constexpr size = sizeof (previous) + sizeof (representative);
};
class change_block : public ysu::block
{
public:
	change_block () = default;
	change_block (ysu::block_hash const &, ysu::account const &, ysu::raw_key const &, ysu::public_key const &, uint64_t);
	change_block (bool &, ysu::stream &);
	change_block (bool &, boost::property_tree::ptree const &);
	virtual ~change_block () = default;
	using ysu::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	ysu::block_hash const & previous () const override;
	ysu::root const & root () const override;
	ysu::account const & representative () const override;
	void serialize (ysu::stream &) const override;
	bool deserialize (ysu::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (ysu::block_visitor &) const override;
	void visit (ysu::mutable_block_visitor &) override;
	ysu::block_type type () const override;
	ysu::signature const & block_signature () const override;
	void signature_set (ysu::signature const &) override;
	bool operator== (ysu::block const &) const override;
	bool operator== (ysu::change_block const &) const;
	bool valid_predecessor (ysu::block const &) const override;
	ysu::change_hashables hashables;
	ysu::signature signature;
	uint64_t work;
	static size_t constexpr size = ysu::change_hashables::size + sizeof (signature) + sizeof (work);
};
class state_hashables
{
public:
	state_hashables () = default;
	state_hashables (ysu::account const &, ysu::block_hash const &, ysu::account const &, ysu::amount const &, ysu::link const &);
	state_hashables (bool &, ysu::stream &);
	state_hashables (bool &, boost::property_tree::ptree const &);
	void hash (blake2b_state &) const;
	// Account# / public key that operates this account
	// Uses:
	// Bulk signature validation in advance of further ledger processing
	// Arranging uncomitted transactions by account
	ysu::account account;
	// Previous transaction in this chain
	ysu::block_hash previous;
	// Representative of this account
	ysu::account representative;
	// Current balance of this account
	// Allows lookup of account balance simply by looking at the head block
	ysu::amount balance;
	// Link field contains source block_hash if receiving, destination account if sending
	ysu::link link;
	// Serialized size
	static size_t constexpr size = sizeof (account) + sizeof (previous) + sizeof (representative) + sizeof (balance) + sizeof (link);
};
class state_block : public ysu::block
{
public:
	state_block () = default;
	state_block (ysu::account const &, ysu::block_hash const &, ysu::account const &, ysu::amount const &, ysu::link const &, ysu::raw_key const &, ysu::public_key const &, uint64_t);
	state_block (bool &, ysu::stream &);
	state_block (bool &, boost::property_tree::ptree const &);
	virtual ~state_block () = default;
	using ysu::block::hash;
	void hash (blake2b_state &) const override;
	uint64_t block_work () const override;
	void block_work_set (uint64_t) override;
	ysu::block_hash const & previous () const override;
	ysu::account const & account () const override;
	ysu::root const & root () const override;
	ysu::link const & link () const override;
	ysu::account const & representative () const override;
	ysu::amount const & balance () const override;
	void serialize (ysu::stream &) const override;
	bool deserialize (ysu::stream &);
	void serialize_json (std::string &, bool = false) const override;
	void serialize_json (boost::property_tree::ptree &) const override;
	bool deserialize_json (boost::property_tree::ptree const &);
	void visit (ysu::block_visitor &) const override;
	void visit (ysu::mutable_block_visitor &) override;
	ysu::block_type type () const override;
	ysu::signature const & block_signature () const override;
	void signature_set (ysu::signature const &) override;
	bool operator== (ysu::block const &) const override;
	bool operator== (ysu::state_block const &) const;
	bool valid_predecessor (ysu::block const &) const override;
	ysu::state_hashables hashables;
	ysu::signature signature;
	uint64_t work;
	static size_t constexpr size = ysu::state_hashables::size + sizeof (signature) + sizeof (work);
};
class block_visitor
{
public:
	virtual void send_block (ysu::send_block const &) = 0;
	virtual void receive_block (ysu::receive_block const &) = 0;
	virtual void open_block (ysu::open_block const &) = 0;
	virtual void change_block (ysu::change_block const &) = 0;
	virtual void state_block (ysu::state_block const &) = 0;
	virtual ~block_visitor () = default;
};
class mutable_block_visitor
{
public:
	virtual void send_block (ysu::send_block &) = 0;
	virtual void receive_block (ysu::receive_block &) = 0;
	virtual void open_block (ysu::open_block &) = 0;
	virtual void change_block (ysu::change_block &) = 0;
	virtual void state_block (ysu::state_block &) = 0;
	virtual ~mutable_block_visitor () = default;
};
/**
 * This class serves to find and return unique variants of a block in order to minimize memory usage
 */
class block_uniquer
{
public:
	using value_type = std::pair<const ysu::uint256_union, std::weak_ptr<ysu::block>>;

	std::shared_ptr<ysu::block> unique (std::shared_ptr<ysu::block>);
	size_t size ();

private:
	std::mutex mutex;
	std::unordered_map<std::remove_const_t<value_type::first_type>, value_type::second_type> blocks;
	static unsigned constexpr cleanup_count = 2;
};

std::unique_ptr<container_info_component> collect_container_info (block_uniquer & block_uniquer, const std::string & name);

std::shared_ptr<ysu::block> deserialize_block (ysu::stream &);
std::shared_ptr<ysu::block> deserialize_block (ysu::stream &, ysu::block_type, ysu::block_uniquer * = nullptr);
std::shared_ptr<ysu::block> deserialize_block_json (boost::property_tree::ptree const &, ysu::block_uniquer * = nullptr);
void serialize_block (ysu::stream &, ysu::block const &);
void block_memory_pool_purge ();
}
