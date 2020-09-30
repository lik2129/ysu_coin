#pragma once

#include <ysu/lib/blocks.hpp>
#include <ysu/secure/common.hpp>

struct MDB_val;

namespace ysu
{
class pending_info_v14 final
{
public:
	pending_info_v14 () = default;
	pending_info_v14 (ysu::account const &, ysu::amount const &, ysu::epoch);
	size_t db_size () const;
	bool deserialize (ysu::stream &);
	bool operator== (ysu::pending_info_v14 const &) const;
	ysu::account source{ 0 };
	ysu::amount amount{ 0 };
	ysu::epoch epoch{ ysu::epoch::epoch_0 };
};
class account_info_v14 final
{
public:
	account_info_v14 () = default;
	account_info_v14 (ysu::block_hash const &, ysu::block_hash const &, ysu::block_hash const &, ysu::amount const &, uint64_t, uint64_t, uint64_t, ysu::epoch);
	size_t db_size () const;
	ysu::block_hash head{ 0 };
	ysu::block_hash rep_block{ 0 };
	ysu::block_hash open_block{ 0 };
	ysu::amount balance{ 0 };
	uint64_t modified{ 0 };
	uint64_t block_count{ 0 };
	uint64_t confirmation_height{ 0 };
	ysu::epoch epoch{ ysu::epoch::epoch_0 };
};
class block_sideband_v14 final
{
public:
	block_sideband_v14 () = default;
	block_sideband_v14 (ysu::block_type, ysu::account const &, ysu::block_hash const &, ysu::amount const &, uint64_t, uint64_t);
	void serialize (ysu::stream &) const;
	bool deserialize (ysu::stream &);
	static size_t size (ysu::block_type);
	ysu::block_type type{ ysu::block_type::invalid };
	ysu::block_hash successor{ 0 };
	ysu::account account{ 0 };
	ysu::amount balance{ 0 };
	uint64_t height{ 0 };
	uint64_t timestamp{ 0 };
};
class state_block_w_sideband_v14
{
public:
	std::shared_ptr<ysu::state_block> state_block;
	ysu::block_sideband_v14 sideband;
};
class block_sideband_v18 final
{
public:
	block_sideband_v18 () = default;
	block_sideband_v18 (ysu::account const &, ysu::block_hash const &, ysu::amount const &, uint64_t, uint64_t, ysu::block_details const &);
	block_sideband_v18 (ysu::account const &, ysu::block_hash const &, ysu::amount const &, uint64_t, uint64_t, ysu::epoch, bool is_send, bool is_receive, bool is_epoch);
	void serialize (ysu::stream &, ysu::block_type) const;
	bool deserialize (ysu::stream &, ysu::block_type);
	static size_t size (ysu::block_type);
	ysu::block_hash successor{ 0 };
	ysu::account account{ 0 };
	ysu::amount balance{ 0 };
	uint64_t height{ 0 };
	uint64_t timestamp{ 0 };
	ysu::block_details details;
};
}
