#pragma once

#include <iostream>
#include <string>
#include <vector>

namespace ysu
{
class config_key_value_pair
{
public:
	std::string key;
	std::string value;
};

std::vector<std::string> config_overrides (std::vector<config_key_value_pair> const & key_value_pairs_a);

std::istream & operator>> (std::istream & is, ysu::config_key_value_pair & into);
}
