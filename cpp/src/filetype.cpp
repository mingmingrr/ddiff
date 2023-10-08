#include "filetype.hpp"

#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

std::ostream& operator <<(std::ostream& output, const fs::file_type& type) {
	switch(type) {
		case fs::file_type::none:      output << "none";      break;
		case fs::file_type::not_found: output << "not_found"; break;
		case fs::file_type::regular:   output << "regular";   break;
		case fs::file_type::directory: output << "directory"; break;
		case fs::file_type::symlink:   output << "symlink";   break;
		case fs::file_type::block:     output << "block";     break;
		case fs::file_type::character: output << "character"; break;
		case fs::file_type::fifo:      output << "fifo";      break;
		case fs::file_type::socket:    output << "socket";    break;
		case fs::file_type::unknown:   output << "unknown";   break;
	}
	return output;
}

std::ostream& operator <<(std::ostream& output, const enum file_extra& type) {
	switch(type) {
		case file_extra::normal:       output << "normal";       break;
		case file_extra::orphan:       output << "orphan";       break;
		case file_extra::sticky:       output << "sticky";       break;
		case file_extra::write:        output << "write";        break;
		case file_extra::sticky_write: output << "sticky_write"; break;
		case file_extra::setuid:       output << "setuid";       break;
		case file_extra::setgid:       output << "setgid";       break;
		case file_extra::executable:   output << "executable";   break;
	}
	return output;
}

std::map<std::string, file_type> file_type_names =
	{ { "fi", { fs::file_type::regular   , file_extra::normal       } }
	, { "su", { fs::file_type::regular   , file_extra::setuid       } }
	, { "sg", { fs::file_type::regular   , file_extra::setgid       } }
	, { "ex", { fs::file_type::regular   , file_extra::executable   } }
	, { "ln", { fs::file_type::symlink   , file_extra::normal       } }
	, { "or", { fs::file_type::symlink   , file_extra::orphan       } }
	, { "di", { fs::file_type::directory , file_extra::normal       } }
	, { "st", { fs::file_type::directory , file_extra::sticky       } }
	, { "tw", { fs::file_type::directory , file_extra::sticky_write } }
	, { "ow", { fs::file_type::directory , file_extra::write        } }
	, { "bd", { fs::file_type::block     , file_extra::normal       } }
	, { "cd", { fs::file_type::character , file_extra::normal       } }
	, { "pi", { fs::file_type::fifo      , file_extra::normal       } }
	, { "so", { fs::file_type::socket    , file_extra::normal       } }
	, { "uk", { fs::file_type::unknown   , file_extra::normal       } }
	, { "mi", { fs::file_type::not_found , file_extra::normal       } }
	// do solaris door
	// ca file with capabilities
	// mh multi hardlink
	};

fs::path resolve_symlink(const fs::path& path) {
	auto link = fs::read_symlink(path);
	if(link.is_relative())
		link = path.parent_path() / link;
	return link;
}

file_type file_type_of(const fs::path& path) {
	auto status = fs::symlink_status(path);
	auto type = status.type();
	auto perm = status.permissions();
	auto extra = file_extra::normal;
	switch(type) {
		case fs::file_type::symlink: {
			if(!fs::exists(resolve_symlink(path)))
				extra = file_extra::orphan;
		} break;
		case fs::file_type::regular: {
			constexpr auto exec = fs::perms::owner_exec
				| fs::perms::group_exec | fs::perms::others_exec;
			if((perm & fs::perms::set_uid) == fs::perms::set_uid)
				extra = file_extra::setuid;
			else if((perm & fs::perms::set_gid) == fs::perms::set_gid)
				extra = file_extra::setgid;
			else if((perm & exec) != fs::perms::none)
				extra = file_extra::executable;
		} break;
		case fs::file_type::directory: {
			bool sticky = (perm & fs::perms::sticky_bit)
				== fs::perms::sticky_bit;
			bool write = (perm & fs::perms::others_write)
				== fs::perms::others_write;
			if(sticky && write)
				extra = file_extra::sticky_write;
			else if(sticky)
				extra = file_extra::sticky;
			else if(write)
				extra = file_extra::write;
		} break;
		default: break;
	}
	return {type, extra};
}

