#include "diff.hpp"
#include "memoize.hpp"

#include <set>
#include <filesystem>
#include <cstring>
#include <cerrno>

namespace fs = std::filesystem;

std::ostream& operator <<(std::ostream& out, diff_status status) {
	switch(status) {
		case diff_status::unknown:   out << "unknown";   break;
		case diff_status::matching:  out << "matching";  break;
		case diff_status::different: out << "different"; break;
		case diff_status::leftonly:  out << "leftonly";  break;
		case diff_status::rightonly: out << "rightonly"; break;
	}
	return out;
}

diff_status diff_file(file_info& left, file_info& right) {
	if(left.ftype == fs::file_type::not_found)
		return diff_status::rightonly;
	if(right.ftype == fs::file_type::not_found)
		return diff_status::leftonly;
	if(left.ftype == fs::file_type::symlink) {
		auto left1 = get_file_info(resolve_symlink(left.fpath));
		return diff_file(left1, right);
	}
	if(right.ftype == fs::file_type::symlink) {
		auto right1 = get_file_info(resolve_symlink(right.fpath));
		return diff_file(left, right1);
	}
	if(left.ftype != right.ftype)
		return diff_status::different;
	switch(left.ftype) {
		case fs::file_type::regular: {
			if(left.fsize != right.fsize)
				return diff_status::different;
			if(left.mtime == right.mtime)
				return diff_status::matching;
			if(left.hash_init() != right.hash_init())
				return diff_status::different;
			if(left.hash_whole() != right.hash_whole())
				return diff_status::different;
			return diff_status::matching;
		}
		case fs::file_type::directory: {
			std::set<fs::path> lefts, rights;
			for(auto file : fs::directory_iterator(left.fpath))
				lefts.insert(file.path().filename());
			for(auto file : fs::directory_iterator(right.fpath))
				rights.insert(file.path().filename());
			if(lefts != rights)
				return diff_status::different;
			auto status = diff_status::matching;
			for(auto file : lefts) {
				auto left_info = get_file_info(left.fpath / file);
				auto right_info = get_file_info(right.fpath / file);
				switch(diff_file(left_info, right_info)) {
					case diff_status::unknown:
						status = diff_status::unknown;
					case diff_status::different:
						return diff_status::different;
					default:
						break;
				}
			}
			return status;
		}
		default:
			return diff_status::unknown;
	}
}
