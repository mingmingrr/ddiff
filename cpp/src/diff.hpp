#pragma once

#include "fileio.hpp"

#include <filesystem>

enum struct diff_status {
	unknown,
	matching,
	different,
	leftonly,
	rightonly,
};

std::ostream& operator <<(std::ostream& out, diff_status status);

diff_status diff_file(file_info& left, file_info& right);

