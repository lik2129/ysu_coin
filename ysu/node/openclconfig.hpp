#pragma once

#include <ysu/lib/errors.hpp>

namespace ysu
{
class jsonconfig;
class tomlconfig;
class opencl_config
{
public:
	opencl_config () = default;
	opencl_config (unsigned, unsigned, unsigned);
	ysu::error serialize_json (ysu::jsonconfig &) const;
	ysu::error deserialize_json (ysu::jsonconfig &);
	ysu::error serialize_toml (ysu::tomlconfig &) const;
	ysu::error deserialize_toml (ysu::tomlconfig &);
	unsigned platform{ 0 };
	unsigned device{ 0 };
	unsigned threads{ 1024 * 1024 };
};
}
