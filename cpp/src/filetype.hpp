#pragma once

#include <filesystem>
#include <map>
#include <shared_mutex>
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
};

struct file_info {
	std::filesystem::path fpath;
	std::filesystem::file_time_type mtime;
	std::filesystem::file_type ftype;
	file_extra extra;
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

extern std::shared_mutex file_type_mutex;

extern std::map<std::filesystem::path, file_type> file_type_cache;

file_type file_type_of(const std::filesystem::path& path);

