#pragma once

#include <utility>
#include <variant>
#include <string>
#include <vector>

typedef std::variant<long long, std::string> natural_key_bit;
typedef std::pair<std::vector<natural_key_bit>, std::string> natural_key_type;

natural_key_type natural_key(const std::string& str);

