#pragma once

#include "fileio.hpp"
#include "diff.hpp"

#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <regex>

#include <ftxui/dom/elements.hpp>

extern std::map<int, ftxui::Decorator> ansi_styles;

struct app_options {
	std::filesystem::path left;
	std::filesystem::path right;
	std::string editor;
	unsigned threads = 4;
	std::vector<std::regex> excludes = {};
	std::map<file_type, ftxui::Decorator> ft_styles = {};
	std::map<std::string, ftxui::Decorator> ext_styles = {};
	std::map<diff_status, ftxui::Decorator> diff_styles = {};
};

ftxui::Decorator parse_ls_color(const std::string& lscolor);

std::variant<int,app_options> get_opts(int argc, const char* argv[]);

