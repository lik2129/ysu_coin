#pragma once

#include <ysu/lib/errors.hpp>
#include <ysu/lib/numbers.hpp>

#include <string>

namespace ysu
{
class tomlconfig;

/** Configuration options for the Qt wallet */
class wallet_config final
{
public:
	wallet_config ();
	/** Update this instance by parsing the given wallet and account */
	ysu::error parse (std::string const & wallet_a, std::string const & account_a);
	ysu::error serialize_toml (ysu::tomlconfig & toml_a) const;
	ysu::error deserialize_toml (ysu::tomlconfig & toml_a);
	ysu::wallet_id wallet;
	ysu::account account{ 0 };
};
}
