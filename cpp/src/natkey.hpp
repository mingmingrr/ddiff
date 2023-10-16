#pragma once

#include <utility>
#include <variant>
#include <string>
#include <vector>
#include <boost/multiprecision/integer.hpp>

typedef std::variant<std::string, boost::multiprecision::cpp_int> natural_key_bit;
typedef std::pair<std::vector<natural_key_bit>, std::string> natural_key_type;

natural_key_type natural_key(const std::string& str);

