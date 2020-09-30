#include <ysu/lib/threading.hpp>
#include <ysu/secure/blockstore.hpp>

ysu::representative_visitor::representative_visitor (ysu::transaction const & transaction_a, ysu::block_store & store_a) :
transaction (transaction_a),
store (store_a),
result (0)
{
}

void ysu::representative_visitor::compute (ysu::block_hash const & hash_a)
{
	current = hash_a;
	while (result.is_zero ())
	{
		auto block (store.block_get (transaction, current));
		debug_assert (block != nullptr);
		block->visit (*this);
	}
}

void ysu::representative_visitor::send_block (ysu::send_block const & block_a)
{
	current = block_a.previous ();
}

void ysu::representative_visitor::receive_block (ysu::receive_block const & block_a)
{
	current = block_a.previous ();
}

void ysu::representative_visitor::open_block (ysu::open_block const & block_a)
{
	result = block_a.hash ();
}

void ysu::representative_visitor::change_block (ysu::change_block const & block_a)
{
	result = block_a.hash ();
}

void ysu::representative_visitor::state_block (ysu::state_block const & block_a)
{
	result = block_a.hash ();
}

ysu::read_transaction::read_transaction (std::unique_ptr<ysu::read_transaction_impl> read_transaction_impl) :
impl (std::move (read_transaction_impl))
{
}

void * ysu::read_transaction::get_handle () const
{
	return impl->get_handle ();
}

void ysu::read_transaction::reset () const
{
	impl->reset ();
}

void ysu::read_transaction::renew () const
{
	impl->renew ();
}

void ysu::read_transaction::refresh () const
{
	reset ();
	renew ();
}

ysu::write_transaction::write_transaction (std::unique_ptr<ysu::write_transaction_impl> write_transaction_impl) :
impl (std::move (write_transaction_impl))
{
	/*
	 * For IO threads, we do not want them to block on creating write transactions.
	 */
	debug_assert (ysu::thread_role::get () != ysu::thread_role::name::io);
}

void * ysu::write_transaction::get_handle () const
{
	return impl->get_handle ();
}

void ysu::write_transaction::commit () const
{
	impl->commit ();
}

void ysu::write_transaction::renew ()
{
	impl->renew ();
}

bool ysu::write_transaction::contains (ysu::tables table_a) const
{
	return impl->contains (table_a);
}
