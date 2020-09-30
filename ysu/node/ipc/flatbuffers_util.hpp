#pragma once

#include <ysu/ipc_flatbuffers_lib/generated/flatbuffers/ysuapi_generated.h>

#include <memory>

namespace ysu
{
class amount;
class block;
class send_block;
class receive_block;
class change_block;
class open_block;
class state_block;
namespace ipc
{
	/**
	 * Utilities to convert between blocks and Flatbuffers equivalents
	 */
	class flatbuffers_builder
	{
	public:
		static ysuapi::BlockUnion block_to_union (ysu::block const & block_a, ysu::amount const & amount_a, bool is_state_send_a = false);
		static std::unique_ptr<ysuapi::BlockStateT> from (ysu::state_block const & block_a, ysu::amount const & amount_a, bool is_state_send_a);
		static std::unique_ptr<ysuapi::BlockSendT> from (ysu::send_block const & block_a);
		static std::unique_ptr<ysuapi::BlockReceiveT> from (ysu::receive_block const & block_a);
		static std::unique_ptr<ysuapi::BlockOpenT> from (ysu::open_block const & block_a);
		static std::unique_ptr<ysuapi::BlockChangeT> from (ysu::change_block const & block_a);
	};
}
}
