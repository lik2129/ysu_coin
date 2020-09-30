#pragma once

#include <ysu/lib/errors.hpp>

#include <thread>

namespace ysu
{
class tomlconfig;

/** Configuration options for RocksDB */
class rocksdb_config final
{
public:
	ysu::error serialize_toml (ysu::tomlconfig & toml_a) const;
	ysu::error deserialize_toml (ysu::tomlconfig & toml_a);

	bool enable{ false };
	uint8_t memory_multiplier{ 2 };
	unsigned io_threads{ std::thread::hardware_concurrency () };
};
}
