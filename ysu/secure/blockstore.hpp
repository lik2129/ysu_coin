#pragma once

#include <ysu/crypto_lib/random_pool.hpp>
#include <ysu/lib/diagnosticsconfig.hpp>
#include <ysu/lib/lmdbconfig.hpp>
#include <ysu/lib/logger_mt.hpp>
#include <ysu/lib/memory.hpp>
#include <ysu/lib/rocksdbconfig.hpp>
#include <ysu/secure/buffer.hpp>
#include <ysu/secure/common.hpp>
#include <ysu/secure/versioning.hpp>

#include <boost/endian/conversion.hpp>
#include <boost/polymorphic_cast.hpp>

#include <stack>

namespace ysu
{
// Move to versioning with a specific version if required for a future upgrade
template <typename T>
class block_w_sideband_v18
{
public:
	std::shared_ptr<T> block;
	ysu::block_sideband_v18 sideband;
};

class block_w_sideband
{
public:
	std::shared_ptr<ysu::block> block;
	ysu::block_sideband sideband;
};

/**
 * Encapsulates database specific container
 */
template <typename Val>
class db_val
{
public:
	db_val (Val const & value_a) :
	value (value_a)
	{
	}

	db_val () :
	db_val (0, nullptr)
	{
	}

	db_val (std::nullptr_t) :
	db_val (0, this)
	{
	}

	db_val (ysu::uint128_union const & val_a) :
	db_val (sizeof (val_a), const_cast<ysu::uint128_union *> (&val_a))
	{
	}

	db_val (ysu::uint256_union const & val_a) :
	db_val (sizeof (val_a), const_cast<ysu::uint256_union *> (&val_a))
	{
	}

	db_val (ysu::account_info const & val_a) :
	db_val (val_a.db_size (), const_cast<ysu::account_info *> (&val_a))
	{
	}

	db_val (ysu::account_info_v14 const & val_a) :
	db_val (val_a.db_size (), const_cast<ysu::account_info_v14 *> (&val_a))
	{
	}

	db_val (ysu::pending_info const & val_a) :
	db_val (val_a.db_size (), const_cast<ysu::pending_info *> (&val_a))
	{
		static_assert (std::is_standard_layout<ysu::pending_info>::value, "Standard layout is required");
	}

	db_val (ysu::pending_info_v14 const & val_a) :
	db_val (val_a.db_size (), const_cast<ysu::pending_info_v14 *> (&val_a))
	{
		static_assert (std::is_standard_layout<ysu::pending_info_v14>::value, "Standard layout is required");
	}

	db_val (ysu::pending_key const & val_a) :
	db_val (sizeof (val_a), const_cast<ysu::pending_key *> (&val_a))
	{
		static_assert (std::is_standard_layout<ysu::pending_key>::value, "Standard layout is required");
	}

	db_val (ysu::unchecked_info const & val_a) :
	buffer (std::make_shared<std::vector<uint8_t>> ())
	{
		{
			ysu::vectorstream stream (*buffer);
			val_a.serialize (stream);
		}
		convert_buffer_to_value ();
	}

	db_val (ysu::unchecked_key const & val_a) :
	db_val (sizeof (val_a), const_cast<ysu::unchecked_key *> (&val_a))
	{
		static_assert (std::is_standard_layout<ysu::unchecked_key>::value, "Standard layout is required");
	}

	db_val (ysu::confirmation_height_info const & val_a) :
	buffer (std::make_shared<std::vector<uint8_t>> ())
	{
		{
			ysu::vectorstream stream (*buffer);
			val_a.serialize (stream);
		}
		convert_buffer_to_value ();
	}

	db_val (ysu::block_info const & val_a) :
	db_val (sizeof (val_a), const_cast<ysu::block_info *> (&val_a))
	{
		static_assert (std::is_standard_layout<ysu::block_info>::value, "Standard layout is required");
	}

	db_val (ysu::endpoint_key const & val_a) :
	db_val (sizeof (val_a), const_cast<ysu::endpoint_key *> (&val_a))
	{
		static_assert (std::is_standard_layout<ysu::endpoint_key>::value, "Standard layout is required");
	}

	db_val (std::shared_ptr<ysu::block> const & val_a) :
	buffer (std::make_shared<std::vector<uint8_t>> ())
	{
		{
			ysu::vectorstream stream (*buffer);
			ysu::serialize_block (stream, *val_a);
		}
		convert_buffer_to_value ();
	}

	db_val (uint64_t val_a) :
	buffer (std::make_shared<std::vector<uint8_t>> ())
	{
		{
			boost::endian::native_to_big_inplace (val_a);
			ysu::vectorstream stream (*buffer);
			ysu::write (stream, val_a);
		}
		convert_buffer_to_value ();
	}

	explicit operator ysu::account_info () const
	{
		ysu::account_info result;
		debug_assert (size () == result.db_size ());
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + result.db_size (), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator ysu::account_info_v14 () const
	{
		ysu::account_info_v14 result;
		debug_assert (size () == result.db_size ());
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + result.db_size (), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator ysu::block_info () const
	{
		ysu::block_info result;
		debug_assert (size () == sizeof (result));
		static_assert (sizeof (ysu::block_info::account) + sizeof (ysu::block_info::balance) == sizeof (result), "Packed class");
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + sizeof (result), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator ysu::pending_info_v14 () const
	{
		ysu::pending_info_v14 result;
		debug_assert (size () == result.db_size ());
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + result.db_size (), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator ysu::pending_info () const
	{
		ysu::pending_info result;
		debug_assert (size () == result.db_size ());
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + result.db_size (), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator ysu::pending_key () const
	{
		ysu::pending_key result;
		debug_assert (size () == sizeof (result));
		static_assert (sizeof (ysu::pending_key::account) + sizeof (ysu::pending_key::hash) == sizeof (result), "Packed class");
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + sizeof (result), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator ysu::confirmation_height_info () const
	{
		ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		ysu::confirmation_height_info result;
		bool error (result.deserialize (stream));
		(void)error;
		debug_assert (!error);
		return result;
	}

	explicit operator ysu::unchecked_info () const
	{
		ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		ysu::unchecked_info result;
		bool error (result.deserialize (stream));
		(void)error;
		debug_assert (!error);
		return result;
	}

	explicit operator ysu::unchecked_key () const
	{
		ysu::unchecked_key result;
		debug_assert (size () == sizeof (result));
		static_assert (sizeof (ysu::unchecked_key::previous) + sizeof (ysu::pending_key::hash) == sizeof (result), "Packed class");
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + sizeof (result), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	explicit operator ysu::uint128_union () const
	{
		return convert<ysu::uint128_union> ();
	}

	explicit operator ysu::amount () const
	{
		return convert<ysu::amount> ();
	}

	explicit operator ysu::block_hash () const
	{
		return convert<ysu::block_hash> ();
	}

	explicit operator ysu::public_key () const
	{
		return convert<ysu::public_key> ();
	}

	explicit operator ysu::uint256_union () const
	{
		return convert<ysu::uint256_union> ();
	}

	explicit operator std::array<char, 64> () const
	{
		ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		std::array<char, 64> result;
		auto error = ysu::try_read (stream, result);
		(void)error;
		debug_assert (!error);
		return result;
	}

	explicit operator ysu::endpoint_key () const
	{
		ysu::endpoint_key result;
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + sizeof (result), reinterpret_cast<uint8_t *> (&result));
		return result;
	}

	template <class Block>
	explicit operator block_w_sideband_v18<Block> () const
	{
		ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		auto error (false);
		block_w_sideband_v18<Block> block_w_sideband;
		block_w_sideband.block = std::make_shared<Block> (error, stream);
		release_assert (!error);

		error = block_w_sideband.sideband.deserialize (stream, block_w_sideband.block->type ());
		release_assert (!error);

		return block_w_sideband;
	}

	explicit operator block_w_sideband () const
	{
		ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		ysu::block_w_sideband block_w_sideband;
		block_w_sideband.block = (ysu::deserialize_block (stream));
		auto error = block_w_sideband.sideband.deserialize (stream, block_w_sideband.block->type ());
		release_assert (!error);
		return block_w_sideband;
	}

	explicit operator state_block_w_sideband_v14 () const
	{
		ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		auto error (false);
		ysu::state_block_w_sideband_v14 block_w_sideband;
		block_w_sideband.state_block = std::make_shared<ysu::state_block> (error, stream);
		debug_assert (!error);

		block_w_sideband.sideband.type = ysu::block_type::state;
		error = block_w_sideband.sideband.deserialize (stream);
		debug_assert (!error);

		return block_w_sideband;
	}

	explicit operator ysu::no_value () const
	{
		return no_value::dummy;
	}

	explicit operator std::shared_ptr<ysu::block> () const
	{
		ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		std::shared_ptr<ysu::block> result (ysu::deserialize_block (stream));
		return result;
	}

	template <typename Block>
	std::shared_ptr<Block> convert_to_block () const
	{
		ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		auto error (false);
		auto result (std::make_shared<Block> (error, stream));
		debug_assert (!error);
		return result;
	}

	explicit operator std::shared_ptr<ysu::send_block> () const
	{
		return convert_to_block<ysu::send_block> ();
	}

	explicit operator std::shared_ptr<ysu::receive_block> () const
	{
		return convert_to_block<ysu::receive_block> ();
	}

	explicit operator std::shared_ptr<ysu::open_block> () const
	{
		return convert_to_block<ysu::open_block> ();
	}

	explicit operator std::shared_ptr<ysu::change_block> () const
	{
		return convert_to_block<ysu::change_block> ();
	}

	explicit operator std::shared_ptr<ysu::state_block> () const
	{
		return convert_to_block<ysu::state_block> ();
	}

	explicit operator std::shared_ptr<ysu::vote> () const
	{
		ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		auto error (false);
		auto result (ysu::make_shared<ysu::vote> (error, stream));
		debug_assert (!error);
		return result;
	}

	explicit operator uint64_t () const
	{
		uint64_t result;
		ysu::bufferstream stream (reinterpret_cast<uint8_t const *> (data ()), size ());
		auto error (ysu::try_read (stream, result));
		(void)error;
		debug_assert (!error);
		boost::endian::big_to_native_inplace (result);
		return result;
	}

	operator Val * () const
	{
		// Allow passing a temporary to a non-c++ function which doesn't have constness
		return const_cast<Val *> (&value);
	}

	operator Val const & () const
	{
		return value;
	}

	// Must be specialized
	void * data () const;
	size_t size () const;
	db_val (size_t size_a, void * data_a);
	void convert_buffer_to_value ();

	Val value;
	std::shared_ptr<std::vector<uint8_t>> buffer;

private:
	template <typename T>
	T convert () const
	{
		T result;
		debug_assert (size () == sizeof (result));
		std::copy (reinterpret_cast<uint8_t const *> (data ()), reinterpret_cast<uint8_t const *> (data ()) + sizeof (result), result.bytes.data ());
		return result;
	}
};

class transaction;
class block_store;

/**
 * Determine the representative for this block
 */
class representative_visitor final : public ysu::block_visitor
{
public:
	representative_visitor (ysu::transaction const & transaction_a, ysu::block_store & store_a);
	~representative_visitor () = default;
	void compute (ysu::block_hash const & hash_a);
	void send_block (ysu::send_block const & block_a) override;
	void receive_block (ysu::receive_block const & block_a) override;
	void open_block (ysu::open_block const & block_a) override;
	void change_block (ysu::change_block const & block_a) override;
	void state_block (ysu::state_block const & block_a) override;
	ysu::transaction const & transaction;
	ysu::block_store & store;
	ysu::block_hash current;
	ysu::block_hash result;
};
template <typename T, typename U>
class store_iterator_impl
{
public:
	virtual ~store_iterator_impl () = default;
	virtual ysu::store_iterator_impl<T, U> & operator++ () = 0;
	virtual bool operator== (ysu::store_iterator_impl<T, U> const & other_a) const = 0;
	virtual bool is_end_sentinal () const = 0;
	virtual void fill (std::pair<T, U> &) const = 0;
	ysu::store_iterator_impl<T, U> & operator= (ysu::store_iterator_impl<T, U> const &) = delete;
	bool operator== (ysu::store_iterator_impl<T, U> const * other_a) const
	{
		return (other_a != nullptr && *this == *other_a) || (other_a == nullptr && is_end_sentinal ());
	}
	bool operator!= (ysu::store_iterator_impl<T, U> const & other_a) const
	{
		return !(*this == other_a);
	}
};
/**
 * Iterates the key/value pairs of a transaction
 */
template <typename T, typename U>
class store_iterator final
{
public:
	store_iterator (std::nullptr_t)
	{
	}
	store_iterator (std::unique_ptr<ysu::store_iterator_impl<T, U>> impl_a) :
	impl (std::move (impl_a))
	{
		impl->fill (current);
	}
	store_iterator (ysu::store_iterator<T, U> && other_a) :
	current (std::move (other_a.current)),
	impl (std::move (other_a.impl))
	{
	}
	ysu::store_iterator<T, U> & operator++ ()
	{
		++*impl;
		impl->fill (current);
		return *this;
	}
	ysu::store_iterator<T, U> & operator= (ysu::store_iterator<T, U> && other_a) noexcept
	{
		impl = std::move (other_a.impl);
		current = std::move (other_a.current);
		return *this;
	}
	ysu::store_iterator<T, U> & operator= (ysu::store_iterator<T, U> const &) = delete;
	std::pair<T, U> * operator-> ()
	{
		return &current;
	}
	bool operator== (ysu::store_iterator<T, U> const & other_a) const
	{
		return (impl == nullptr && other_a.impl == nullptr) || (impl != nullptr && *impl == other_a.impl.get ()) || (other_a.impl != nullptr && *other_a.impl == impl.get ());
	}
	bool operator!= (ysu::store_iterator<T, U> const & other_a) const
	{
		return !(*this == other_a);
	}

private:
	std::pair<T, U> current;
	std::unique_ptr<ysu::store_iterator_impl<T, U>> impl;
};

// Keep this in alphabetical order
enum class tables
{
	accounts,
	blocks,
	confirmation_height,
	default_unused, // RocksDB only
	frontiers,
	meta,
	online_weight,
	peers,
	pending,
	pruned,
	unchecked,
	vote
};

class transaction_impl
{
public:
	virtual ~transaction_impl () = default;
	virtual void * get_handle () const = 0;
};

class read_transaction_impl : public transaction_impl
{
public:
	virtual void reset () = 0;
	virtual void renew () = 0;
};

class write_transaction_impl : public transaction_impl
{
public:
	virtual void commit () const = 0;
	virtual void renew () = 0;
	virtual bool contains (ysu::tables table_a) const = 0;
};

class transaction
{
public:
	virtual ~transaction () = default;
	virtual void * get_handle () const = 0;
};

/**
 * RAII wrapper of a read MDB_txn where the constructor starts the transaction
 * and the destructor aborts it.
 */
class read_transaction final : public transaction
{
public:
	explicit read_transaction (std::unique_ptr<ysu::read_transaction_impl> read_transaction_impl);
	void * get_handle () const override;
	void reset () const;
	void renew () const;
	void refresh () const;

private:
	std::unique_ptr<ysu::read_transaction_impl> impl;
};

/**
 * RAII wrapper of a read-write MDB_txn where the constructor starts the transaction
 * and the destructor commits it.
 */
class write_transaction final : public transaction
{
public:
	explicit write_transaction (std::unique_ptr<ysu::write_transaction_impl> write_transaction_impl);
	void * get_handle () const override;
	void commit () const;
	void renew ();
	bool contains (ysu::tables table_a) const;

private:
	std::unique_ptr<ysu::write_transaction_impl> impl;
};

class ledger_cache;

/**
 * Manages block storage and iteration
 */
class block_store
{
public:
	virtual ~block_store () = default;
	virtual void initialize (ysu::write_transaction const &, ysu::genesis const &, ysu::ledger_cache &) = 0;
	virtual void block_put (ysu::write_transaction const &, ysu::block_hash const &, ysu::block const &) = 0;
	virtual ysu::block_hash block_successor (ysu::transaction const &, ysu::block_hash const &) const = 0;
	virtual void block_successor_clear (ysu::write_transaction const &, ysu::block_hash const &) = 0;
	virtual std::shared_ptr<ysu::block> block_get (ysu::transaction const &, ysu::block_hash const &) const = 0;
	virtual std::shared_ptr<ysu::block> block_get_no_sideband (ysu::transaction const &, ysu::block_hash const &) const = 0;
	virtual std::shared_ptr<ysu::block> block_random (ysu::transaction const &) = 0;
	virtual void block_del (ysu::write_transaction const &, ysu::block_hash const &) = 0;
	virtual bool block_exists (ysu::transaction const &, ysu::block_hash const &) = 0;
	virtual uint64_t block_count (ysu::transaction const &) = 0;
	virtual bool root_exists (ysu::transaction const &, ysu::root const &) = 0;
	virtual ysu::account block_account (ysu::transaction const &, ysu::block_hash const &) const = 0;
	virtual ysu::account block_account_calculated (ysu::block const &) const = 0;

	virtual void frontier_put (ysu::write_transaction const &, ysu::block_hash const &, ysu::account const &) = 0;
	virtual ysu::account frontier_get (ysu::transaction const &, ysu::block_hash const &) const = 0;
	virtual void frontier_del (ysu::write_transaction const &, ysu::block_hash const &) = 0;

	virtual void account_put (ysu::write_transaction const &, ysu::account const &, ysu::account_info const &) = 0;
	virtual bool account_get (ysu::transaction const &, ysu::account const &, ysu::account_info &) = 0;
	virtual void account_del (ysu::write_transaction const &, ysu::account const &) = 0;
	virtual bool account_exists (ysu::transaction const &, ysu::account const &) = 0;
	virtual size_t account_count (ysu::transaction const &) = 0;
	virtual void confirmation_height_clear (ysu::write_transaction const &, ysu::account const &, uint64_t) = 0;
	virtual void confirmation_height_clear (ysu::write_transaction const &) = 0;
	virtual ysu::store_iterator<ysu::account, ysu::account_info> accounts_begin (ysu::transaction const &, ysu::account const &) const = 0;
	virtual ysu::store_iterator<ysu::account, ysu::account_info> accounts_begin (ysu::transaction const &) const = 0;
	virtual ysu::store_iterator<ysu::account, ysu::account_info> accounts_end () const = 0;

	virtual void pending_put (ysu::write_transaction const &, ysu::pending_key const &, ysu::pending_info const &) = 0;
	virtual void pending_del (ysu::write_transaction const &, ysu::pending_key const &) = 0;
	virtual bool pending_get (ysu::transaction const &, ysu::pending_key const &, ysu::pending_info &) = 0;
	virtual bool pending_exists (ysu::transaction const &, ysu::pending_key const &) = 0;
	virtual bool pending_any (ysu::transaction const &, ysu::account const &) = 0;
	virtual ysu::store_iterator<ysu::pending_key, ysu::pending_info> pending_begin (ysu::transaction const &, ysu::pending_key const &) = 0;
	virtual ysu::store_iterator<ysu::pending_key, ysu::pending_info> pending_begin (ysu::transaction const &) = 0;
	virtual ysu::store_iterator<ysu::pending_key, ysu::pending_info> pending_end () = 0;

	virtual ysu::uint128_t block_balance (ysu::transaction const &, ysu::block_hash const &) = 0;
	virtual ysu::uint128_t block_balance_calculated (std::shared_ptr<ysu::block> const &) const = 0;
	virtual ysu::epoch block_version (ysu::transaction const &, ysu::block_hash const &) = 0;

	virtual void unchecked_clear (ysu::write_transaction const &) = 0;
	virtual void unchecked_put (ysu::write_transaction const &, ysu::unchecked_key const &, ysu::unchecked_info const &) = 0;
	virtual void unchecked_put (ysu::write_transaction const &, ysu::block_hash const &, std::shared_ptr<ysu::block> const &) = 0;
	virtual std::vector<ysu::unchecked_info> unchecked_get (ysu::transaction const &, ysu::block_hash const &) = 0;
	virtual bool unchecked_exists (ysu::transaction const & transaction_a, ysu::unchecked_key const & unchecked_key_a) = 0;
	virtual void unchecked_del (ysu::write_transaction const &, ysu::unchecked_key const &) = 0;
	virtual ysu::store_iterator<ysu::unchecked_key, ysu::unchecked_info> unchecked_begin (ysu::transaction const &) const = 0;
	virtual ysu::store_iterator<ysu::unchecked_key, ysu::unchecked_info> unchecked_begin (ysu::transaction const &, ysu::unchecked_key const &) const = 0;
	virtual ysu::store_iterator<ysu::unchecked_key, ysu::unchecked_info> unchecked_end () const = 0;
	virtual size_t unchecked_count (ysu::transaction const &) = 0;

	// Return latest vote for an account from store
	virtual std::shared_ptr<ysu::vote> vote_get (ysu::transaction const &, ysu::account const &) = 0;
	// Populate vote with the next sequence number
	virtual std::shared_ptr<ysu::vote> vote_generate (ysu::transaction const &, ysu::account const &, ysu::raw_key const &, std::shared_ptr<ysu::block>) = 0;
	virtual std::shared_ptr<ysu::vote> vote_generate (ysu::transaction const &, ysu::account const &, ysu::raw_key const &, std::vector<ysu::block_hash>) = 0;
	// Return either vote or the stored vote with a higher sequence number
	virtual std::shared_ptr<ysu::vote> vote_max (ysu::transaction const &, std::shared_ptr<ysu::vote>) = 0;
	// Return latest vote for an account considering the vote cache
	virtual std::shared_ptr<ysu::vote> vote_current (ysu::transaction const &, ysu::account const &) = 0;
	virtual void flush (ysu::write_transaction const &) = 0;
	virtual ysu::store_iterator<ysu::account, std::shared_ptr<ysu::vote>> vote_begin (ysu::transaction const &) = 0;
	virtual ysu::store_iterator<ysu::account, std::shared_ptr<ysu::vote>> vote_end () = 0;

	virtual void online_weight_put (ysu::write_transaction const &, uint64_t, ysu::amount const &) = 0;
	virtual void online_weight_del (ysu::write_transaction const &, uint64_t) = 0;
	virtual ysu::store_iterator<uint64_t, ysu::amount> online_weight_begin (ysu::transaction const &) const = 0;
	virtual ysu::store_iterator<uint64_t, ysu::amount> online_weight_end () const = 0;
	virtual size_t online_weight_count (ysu::transaction const &) const = 0;
	virtual void online_weight_clear (ysu::write_transaction const &) = 0;

	virtual void version_put (ysu::write_transaction const &, int) = 0;
	virtual int version_get (ysu::transaction const &) const = 0;

	virtual void pruned_put (ysu::write_transaction const & transaction_a, ysu::block_hash const & hash_a) = 0;
	virtual void pruned_del (ysu::write_transaction const & transaction_a, ysu::block_hash const & hash_a) = 0;
	virtual bool pruned_exists (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const = 0;
	virtual bool block_or_pruned_exists (ysu::transaction const &, ysu::block_hash const &) = 0;
	virtual ysu::block_hash pruned_random (ysu::transaction const & transaction_a) = 0;
	virtual size_t pruned_count (ysu::transaction const & transaction_a) const = 0;
	virtual void pruned_clear (ysu::write_transaction const &) = 0;

	virtual void peer_put (ysu::write_transaction const & transaction_a, ysu::endpoint_key const & endpoint_a) = 0;
	virtual void peer_del (ysu::write_transaction const & transaction_a, ysu::endpoint_key const & endpoint_a) = 0;
	virtual bool peer_exists (ysu::transaction const & transaction_a, ysu::endpoint_key const & endpoint_a) const = 0;
	virtual size_t peer_count (ysu::transaction const & transaction_a) const = 0;
	virtual void peer_clear (ysu::write_transaction const & transaction_a) = 0;
	virtual ysu::store_iterator<ysu::endpoint_key, ysu::no_value> peers_begin (ysu::transaction const & transaction_a) const = 0;
	virtual ysu::store_iterator<ysu::endpoint_key, ysu::no_value> peers_end () const = 0;

	virtual void confirmation_height_put (ysu::write_transaction const & transaction_a, ysu::account const & account_a, ysu::confirmation_height_info const & confirmation_height_info_a) = 0;
	virtual bool confirmation_height_get (ysu::transaction const & transaction_a, ysu::account const & account_a, ysu::confirmation_height_info & confirmation_height_info_a) = 0;
	virtual bool confirmation_height_exists (ysu::transaction const & transaction_a, ysu::account const & account_a) const = 0;
	virtual void confirmation_height_del (ysu::write_transaction const & transaction_a, ysu::account const & account_a) = 0;
	virtual uint64_t confirmation_height_count (ysu::transaction const & transaction_a) = 0;
	virtual ysu::store_iterator<ysu::account, ysu::confirmation_height_info> confirmation_height_begin (ysu::transaction const & transaction_a, ysu::account const & account_a) const = 0;
	virtual ysu::store_iterator<ysu::account, ysu::confirmation_height_info> confirmation_height_begin (ysu::transaction const & transaction_a) const = 0;
	virtual ysu::store_iterator<ysu::account, ysu::confirmation_height_info> confirmation_height_end () const = 0;

	virtual ysu::store_iterator<ysu::block_hash, std::shared_ptr<ysu::block>> blocks_begin (ysu::transaction const & transaction_a) const = 0;
	virtual ysu::store_iterator<ysu::block_hash, std::shared_ptr<ysu::block>> blocks_end () const = 0;

	virtual void accounts_for_each_par (std::function<void(ysu::read_transaction const &, ysu::store_iterator<ysu::account, ysu::account_info>, ysu::store_iterator<ysu::account, ysu::account_info>)> const &) = 0;
	virtual void confirmation_height_for_each_par (std::function<void(ysu::read_transaction const &, ysu::store_iterator<ysu::account, ysu::confirmation_height_info>, ysu::store_iterator<ysu::account, ysu::confirmation_height_info>)> const &) = 0;
	virtual void pending_for_each_par (std::function<void(ysu::read_transaction const &, ysu::store_iterator<ysu::pending_key, ysu::pending_info>, ysu::store_iterator<ysu::pending_key, ysu::pending_info>)> const &) = 0;

	virtual uint64_t block_account_height (ysu::transaction const & transaction_a, ysu::block_hash const & hash_a) const = 0;
	virtual std::mutex & get_cache_mutex () = 0;

	virtual unsigned max_block_write_batch_num () const = 0;

	virtual bool copy_db (boost::filesystem::path const & destination) = 0;
	virtual void rebuild_db (ysu::write_transaction const & transaction_a) = 0;

	/** Not applicable to all sub-classes */
	virtual void serialize_mdb_tracker (boost::property_tree::ptree &, std::chrono::milliseconds, std::chrono::milliseconds){};
	virtual void serialize_memory_stats (boost::property_tree::ptree &) = 0;

	virtual bool init_error () const = 0;

	/** Start read-write transaction */
	virtual ysu::write_transaction tx_begin_write (std::vector<ysu::tables> const & tables_to_lock = {}, std::vector<ysu::tables> const & tables_no_lock = {}) = 0;

	/** Start read-only transaction */
	virtual ysu::read_transaction tx_begin_read () = 0;

	virtual std::string vendor_get () const = 0;
};

std::unique_ptr<ysu::block_store> make_store (ysu::logger_mt & logger, boost::filesystem::path const & path, bool open_read_only = false, bool add_db_postfix = false, ysu::rocksdb_config const & rocksdb_config = ysu::rocksdb_config{}, ysu::txn_tracking_config const & txn_tracking_config_a = ysu::txn_tracking_config{}, std::chrono::milliseconds block_processor_batch_max_time_a = std::chrono::milliseconds (5000), ysu::lmdb_config const & lmdb_config_a = ysu::lmdb_config{}, bool backup_before_upgrade = false, bool rocksdb_backend = false);
}

namespace std
{
template <>
struct hash<::ysu::tables>
{
	size_t operator() (::ysu::tables const & table_a) const
	{
		return static_cast<size_t> (table_a);
	}
};
}
