#include <ysu/lib/blocks.hpp>
#include <ysu/lib/numbers.hpp>
#include <ysu/node/ipc/flatbuffers_util.hpp>
#include <ysu/secure/common.hpp>

std::unique_ptr<ysuapi::BlockStateT> ysu::ipc::flatbuffers_builder::from (ysu::state_block const & block_a, ysu::amount const & amount_a, bool is_state_send_a)
{
	static ysu::network_params params;
	auto block (std::make_unique<ysuapi::BlockStateT> ());
	block->account = block_a.account ().to_account ();
	block->hash = block_a.hash ().to_string ();
	block->previous = block_a.previous ().to_string ();
	block->representative = block_a.representative ().to_account ();
	block->balance = block_a.balance ().to_string_dec ();
	block->link = block_a.link ().to_string ();
	block->link_as_account = block_a.link ().to_account ();
	block_a.signature.encode_hex (block->signature);
	block->work = ysu::to_string_hex (block_a.work);

	if (is_state_send_a)
	{
		block->subtype = ysuapi::BlockSubType::BlockSubType_send;
	}
	else if (block_a.link ().is_zero ())
	{
		block->subtype = ysuapi::BlockSubType::BlockSubType_change;
	}
	else if (amount_a == 0 && params.ledger.epochs.is_epoch_link (block_a.link ()))
	{
		block->subtype = ysuapi::BlockSubType::BlockSubType_epoch;
	}
	else
	{
		block->subtype = ysuapi::BlockSubType::BlockSubType_receive;
	}
	return block;
}

std::unique_ptr<ysuapi::BlockSendT> ysu::ipc::flatbuffers_builder::from (ysu::send_block const & block_a)
{
	auto block (std::make_unique<ysuapi::BlockSendT> ());
	block->hash = block_a.hash ().to_string ();
	block->balance = block_a.balance ().to_string_dec ();
	block->destination = block_a.hashables.destination.to_account ();
	block->previous = block_a.previous ().to_string ();
	block_a.signature.encode_hex (block->signature);
	block->work = ysu::to_string_hex (block_a.work);
	return block;
}

std::unique_ptr<ysuapi::BlockReceiveT> ysu::ipc::flatbuffers_builder::from (ysu::receive_block const & block_a)
{
	auto block (std::make_unique<ysuapi::BlockReceiveT> ());
	block->hash = block_a.hash ().to_string ();
	block->source = block_a.source ().to_string ();
	block->previous = block_a.previous ().to_string ();
	block_a.signature.encode_hex (block->signature);
	block->work = ysu::to_string_hex (block_a.work);
	return block;
}

std::unique_ptr<ysuapi::BlockOpenT> ysu::ipc::flatbuffers_builder::from (ysu::open_block const & block_a)
{
	auto block (std::make_unique<ysuapi::BlockOpenT> ());
	block->hash = block_a.hash ().to_string ();
	block->source = block_a.source ().to_string ();
	block->account = block_a.account ().to_account ();
	block->representative = block_a.representative ().to_account ();
	block_a.signature.encode_hex (block->signature);
	block->work = ysu::to_string_hex (block_a.work);
	return block;
}

std::unique_ptr<ysuapi::BlockChangeT> ysu::ipc::flatbuffers_builder::from (ysu::change_block const & block_a)
{
	auto block (std::make_unique<ysuapi::BlockChangeT> ());
	block->hash = block_a.hash ().to_string ();
	block->previous = block_a.previous ().to_string ();
	block->representative = block_a.representative ().to_account ();
	block_a.signature.encode_hex (block->signature);
	block->work = ysu::to_string_hex (block_a.work);
	return block;
}

ysuapi::BlockUnion ysu::ipc::flatbuffers_builder::block_to_union (ysu::block const & block_a, ysu::amount const & amount_a, bool is_state_send_a)
{
	ysuapi::BlockUnion u;
	switch (block_a.type ())
	{
		case ysu::block_type::state:
		{
			u.Set (*from (dynamic_cast<ysu::state_block const &> (block_a), amount_a, is_state_send_a));
			break;
		}
		case ysu::block_type::send:
		{
			u.Set (*from (dynamic_cast<ysu::send_block const &> (block_a)));
			break;
		}
		case ysu::block_type::receive:
		{
			u.Set (*from (dynamic_cast<ysu::receive_block const &> (block_a)));
			break;
		}
		case ysu::block_type::open:
		{
			u.Set (*from (dynamic_cast<ysu::open_block const &> (block_a)));
			break;
		}
		case ysu::block_type::change:
		{
			u.Set (*from (dynamic_cast<ysu::change_block const &> (block_a)));
			break;
		}

		default:
			debug_assert (false);
	}
	return u;
}
