#include <ysu/crypto_lib/random_pool.hpp>

#include <crypto/cryptopp/osrng.h>

std::mutex ysu::random_pool::mutex;

void ysu::random_pool::generate_block (unsigned char * output, size_t size)
{
	auto & pool = get_pool ();
	std::lock_guard<std::mutex> guard (mutex);
	pool.GenerateBlock (output, size);
}

unsigned ysu::random_pool::generate_word32 (unsigned min, unsigned max)
{
	auto & pool = get_pool ();
	std::lock_guard<std::mutex> guard (mutex);
	return pool.GenerateWord32 (min, max);
}

unsigned char ysu::random_pool::generate_byte ()
{
	auto & pool = get_pool ();
	std::lock_guard<std::mutex> guard (mutex);
	return pool.GenerateByte ();
}

CryptoPP::AutoSeededRandomPool & ysu::random_pool::get_pool ()
{
	static CryptoPP::AutoSeededRandomPool pool;
	return pool;
}
