#include "fileio.hpp"
#include "memoize.hpp"

#include <fstream>
#include <sys/stat.h>
#include <boost/functional/hash.hpp>

namespace fs = std::filesystem;

std::pair<std::filesystem::file_type, file_extra> file_info::file_type() {
	return { this->ftype, this->extra };
}

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
		case file_extra::multi_link:   output << "multi_link";   break;
	}
	return output;
}

const std::map<std::string, file_type> file_type_names =
	{ { "fi", { fs::file_type::regular   , file_extra::normal       } }
	, { "su", { fs::file_type::regular   , file_extra::setuid       } }
	, { "sg", { fs::file_type::regular   , file_extra::setgid       } }
	, { "ex", { fs::file_type::regular   , file_extra::executable   } }
	, { "mh", { fs::file_type::regular   , file_extra::multi_link   } }
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

bool operator ==(const struct timespec& x, const struct timespec& y) {
	return (x.tv_sec == y.tv_sec) && (x.tv_nsec == y.tv_nsec);
}

memoized<
	file_info,
	std::optional<struct stat>,
	const fs::path&
> get_file_info {
	.init = [] (const fs::path& path) -> std::optional<struct stat> {
		struct stat fstat;
		if(lstat(path.c_str(), &fstat)) {
			if(errno == ENOENT)
				return std::nullopt;
			throw std::system_error(errno, std::generic_category());
		}
		return fstat;
	},
	.valid = [] (file_info& info, std::optional<struct stat>& fstat, const fs::path& path) {
		if(!fstat.has_value())
			return info.mtime == (struct timespec){ 0, 0 };
		return info.mtime == fstat->st_mtim;
	},
	.func = [] (std::optional<struct stat>& fstat, const fs::path& path) {
		if(!fstat.has_value())
			return file_info
				{ .fpath = path
				, .mtime = { 0, 0 }
				, .ftype = fs::file_type::not_found
				, .extra = file_extra::normal
				, .fsize = 0
				, .hash_init = size_t(0)
				, .hash_whole = size_t(0)
				};
		file_info info =
			{ .fpath = path
			, .mtime = fstat->st_mtim
			, .extra = file_extra::normal
			, .fsize = fstat->st_size
			, .hash_init = [=] () {
				std::ifstream file(path, std::ios::in | std::ios::binary);
				char buffer[4096];
				file.read(buffer, 4096);
				return boost::hash_range(buffer, buffer + file.gcount());
				}
			, .hash_whole = [=] () {
				std::ifstream file(path, std::ios::in | std::ios::binary);
				char buffer[4096];
				size_t hash = 0;
				while(!file.eof()) {
					file.read(buffer, 4096);
					boost::hash_combine(hash,
						boost::hash_range(buffer, buffer + file.gcount()));
				}
				return hash;
				}
			};
		switch(fstat->st_mode & S_IFMT) {
			case S_IFREG: {
				info.ftype = fs::file_type::regular;
				if(fstat->st_mode & S_ISUID)
					info.extra = file_extra::setuid;
				else if(fstat->st_mode & S_ISGID)
					info.extra = file_extra::setgid;
				else if(fstat->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))
					info.extra = file_extra::executable;
				else if(fstat->st_nlink > 1)
					info.extra = file_extra::multi_link;
			} break;
			case S_IFDIR: {
				info.ftype = fs::file_type::directory;
				if((fstat->st_mode & S_ISVTX) && (fstat->st_mode & S_IWOTH))
					info.extra = file_extra::sticky_write;
				else if(fstat->st_mode & S_ISVTX)
					info.extra = file_extra::sticky;
				else if(fstat->st_mode & S_IWOTH)
					info.extra = file_extra::write;
			} break;
			case S_IFLNK:
				info.ftype = fs::file_type::symlink;
				if(!fs::exists(resolve_symlink(path)))
					info.extra = file_extra::orphan;
				break;
			case S_IFBLK:
				info.ftype = fs::file_type::block;
				break;
			case S_IFCHR:
				info.ftype = fs::file_type::character;
				break;
			case S_IFIFO:
				info.ftype = fs::file_type::fifo;
				break;
			case S_IFSOCK:
				info.ftype = fs::file_type::socket;
				break;
			default:
				info.ftype = fs::file_type::unknown;
				break;
		}
		return info;
	}
};

