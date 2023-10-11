#pragma once

#include "lazy.hpp"

#include <filesystem>
#include <map>
#include <sys/stat.h>

enum struct file_extra {
	normal,
	orphan,
	sticky,
	write,
	sticky_write,
	setuid,
	setgid,
	executable,
	multi_link,
};

struct file_info {
	std::filesystem::path fpath;
	std::timespec mtime;
	std::filesystem::file_type ftype;
	file_extra extra;
	off_t fsize;
	lazy<size_t> hash_init;
	lazy<size_t> hash_whole;
};

typedef std::pair<std::filesystem::file_type, file_extra> file_type;

std::ostream& operator <<(
	std::ostream& output,
	const std::filesystem::file_type& type
);

std::ostream& operator <<(
	std::ostream& output,
	const enum file_extra& type
);

extern std::map<std::string, file_type> file_type_names;

std::filesystem::path resolve_symlink(const std::filesystem::path& path);

file_type file_type_of(const std::filesystem::path& path);

file_info get_file_info(const std::filesystem::path& path);

