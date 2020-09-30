#pragma once

#include <ysu/lib/config.hpp>
#include <ysu/lib/errors.hpp>

namespace ysu
{
class jsonconfig;
class tomlconfig;
namespace websocket
{
	/** websocket configuration */
	class config final
	{
	public:
		config ();
		ysu::error deserialize_json (ysu::jsonconfig & json_a);
		ysu::error serialize_json (ysu::jsonconfig & json) const;
		ysu::error deserialize_toml (ysu::tomlconfig & toml_a);
		ysu::error serialize_toml (ysu::tomlconfig & toml) const;
		ysu::network_constants network_constants;
		bool enabled{ false };
		uint16_t port;
		std::string address;
	};
}
}
