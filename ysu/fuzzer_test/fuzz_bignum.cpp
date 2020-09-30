#include <ysu/node/common.hpp>

/** Fuzz decimal, hex and account parsing */
void fuzz_bignum_parsers (const uint8_t * Data, size_t Size)
{
	try
	{
		auto data (std::string (reinterpret_cast<char *> (const_cast<uint8_t *> (Data)), Size));
		ysu::uint128_union u128;
		u128.decode_dec (data);
		u128.decode_hex (data);

		ysu::uint256_union u256;
		u256.decode_dec (data);
		u256.decode_hex (data);

		ysu::uint512_union u512;
		u512.decode_hex (data);

		ysu::public_key pkey;
		pkey.decode_account (data);

		uint64_t out;
		ysu::from_string_hex (data, out);
	}
	catch (std::out_of_range const &)
	{
	}
}

/** Fuzzer entry point */
extern "C" int LLVMFuzzerTestOneInput (const uint8_t * Data, size_t Size)
{
	fuzz_bignum_parsers (Data, Size);
	return 0;
}
