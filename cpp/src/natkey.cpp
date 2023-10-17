#include "natkey.hpp"

#include <boost/algorithm/string.hpp>

natural_key_type natural_key(const std::string& str) {
	auto left = str.begin(), mid = left, right = str.end();
	std::vector<natural_key_bit> keys;
	while(mid != right && std::isspace(*mid)) ++mid;
	while((left = mid) != right) {
		if(std::isspace(*mid)) {
			while(mid != right && std::isspace(*mid)) ++mid;
			keys.push_back(" ");
		} else if(std::isdigit(*mid)) {
			while(left != right && *left == '0') ++left, ++mid;
			while(mid != right && std::isdigit(*mid)) ++mid;
			keys.push_back(boost::multiprecision::cpp_int(std::string(left, mid)));
		} else {
			while(mid != right && !std::isspace(*mid) && !std::isdigit(*mid)) ++mid;
			keys.push_back(std::string(left, mid));
			boost::algorithm::to_lower(std::get<std::string>(keys.back()));
		}
	}
	return std::pair{keys, str};
}

