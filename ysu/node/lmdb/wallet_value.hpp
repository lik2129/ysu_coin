#pragma once

#include <ysu/lib/numbers.hpp>
#include <ysu/secure/blockstore.hpp>

#include <lmdb/libraries/liblmdb/lmdb.h>

namespace ysu
{
class wallet_value
{
public:
	wallet_value () = default;
	wallet_value (ysu::db_val<MDB_val> const &);
	wallet_value (ysu::uint256_union const &, uint64_t);
	ysu::db_val<MDB_val> val () const;
	ysu::uint256_union key;
	uint64_t work;
};
}
