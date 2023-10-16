#include "opts.hpp"

#include <iostream>

#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>

namespace opt = boost::program_options;

std::map<int, ftxui::Decorator> ansi_styles =
	{ { 1  , ftxui::bold       }
	, { 2  , ftxui::dim        }
	// , { 3  , ftxui::italic     }
	, { 4  , ftxui::underlined }
	, { 5  , ftxui::blink      }
	// , { 6  , ftxui::blink2     }
	, { 7  , ftxui::inverted   }
	// , { 8  , ftxui::conceal    }
	, { 9  , ftxui::strikethrough }
	, { 21 , ftxui::underlinedDouble }
	// , { 51 , ftxui::frame      }
	// , { 52 , ftxui::encircle   }
	// , { 53 , ftxui::overline   }
	};

ftxui::Decorator parse_ls_color(const std::string& lscolor) {
	std::vector<std::string> numberstrs;
	boost::split(numberstrs, lscolor, boost::is_any_of(";"));
	std::vector<int> numbers;
	std::transform(numberstrs.begin(), numberstrs.end(),
		std::back_inserter(numbers), [] (const auto& x) { return std::stoi(x); });
	ftxui::Decorator style = ftxui::nothing;
	for(size_t i = 0; i < numbers.size(); ++i) {
		auto n = numbers[i];
		auto f = ansi_styles.find(n);
		if(f != ansi_styles.end())
			style = style | f->second;
		else if(30 <= n && n <= 37)
			style = style | ftxui::color(
				ftxui::Color(ftxui::Color::Palette16(n - 30)));
		else if(90 <= n && n <= 97)
			style = style | ftxui::color(
				ftxui::Color(ftxui::Color::Palette16(n - 90 + 8)));
		else if(40 <= n && n <= 47)
			style = style | ftxui::bgcolor(
				ftxui::Color(ftxui::Color::Palette16(n - 40)));
		else if(100 <= n && n <= 107)
			style = style | ftxui::bgcolor(
				ftxui::Color(ftxui::Color::Palette16(n - 100 + 8)));
		else if(n == 38 || n == 48 || n == 58) {
			ftxui::Color color;
			switch(numbers.at(i + 1)) {
				case 2:
					color = ftxui::Color(numbers.at(i + 2),
						numbers.at(i + 3), numbers.at(i + 4));
					i += 4;
					break;
				case 5: {
					auto n = numbers.at(i + 2);
					if(n < 16) color = ftxui::Color(ftxui::Color::Palette16(n));
					else color = ftxui::Color(ftxui::Color::Palette16(n));
					i += 2;
				} break;
				default:
					throw std::invalid_argument("unknown color");
			}
			switch(n) {
				case 38: style = style | ftxui::color(color); break;
				case 48: style = style | ftxui::bgcolor(color); break;
			}
		}
	}
	return style;
}

std::variant<int,app_options> get_opts(int argc, const char* argv[]) {
	opt::options_description opts_desc(
		"usage: ddiff.py [options] LEFT RIGHT\noptions");
	opts_desc.add_options()
		( "help,h"
		, "show this help message" )
		( "editor,e"
		, opt::value<std::string>()->default_value("$EDITOR -d")
		, "program used to diff two files" )
		( "threads,j"
		, opt::value<unsigned>()->default_value(4)
		, "number of diff threads" )
		( "exclude,x"
		, opt::value<std::vector<std::string>>()->composing()
		, "ignore files matching regex" )
		( "left"
		, opt::value<std::string>()->required()
		, "base directory for left side" )
		( "right"
		, opt::value<std::string>()->required()
		, "base directory for right side" )
		;
	opt::positional_options_description posn_desc;
	posn_desc.add("left", 1);
	posn_desc.add("right", 1);

	opt::variables_map args;
	try {
		opt::store(opt::command_line_parser(argc, argv)
			.options(opts_desc).positional(posn_desc).run(), args);
		opt::notify(args);
	} catch(opt::error& err) {
		std::cout << err.what() << '\n' << opts_desc << std::flush;
		return 1;
	}
	auto help = args.find("help");
	if(help != args.end()) {
		std::cout << opts_desc << std::flush;
		return 0;
	}

	app_options opts
		{ .left = args["left"].as<std::string>()
		, .right = args["right"].as<std::string>()
		, .editor = args["editor"].as<std::string>()
		, .threads = args["threads"].as<unsigned>()
		};
	auto exclude = args.find("exclude");
	if(exclude != args.end())
		for(const auto exc : exclude->second.as<std::vector<std::string>>())
			opts.excludes.push_back(std::regex(exc));
	opts.ft_styles = {{file_type_names.at("fi"), ftxui::nothing}};

	const char* ls_colors = std::getenv("LS_COLORS");
	if(ls_colors == NULL) ls_colors =
		"rs=0:di=01;34:ln=01;36:mh=00:pi=40;33:so=01;35:do=01;35:"
		"bd=40;33;01:cd=40;33;01:or=40;31;01:mi=02;90:su=37;41:sg=30;43:"
		"ca=00:tw=30;42:ow=34;42:st=37;44:ex=01;32:";

	std::vector<std::string> ls_color_bits;
	boost::split(ls_color_bits, ls_colors, boost::is_any_of(":"));
	for(const auto color : ls_color_bits) {
		std::size_t equals = color.find('=');
		if(equals == std::string::npos) continue;
		std::string key = color.substr(0, equals);
		std::string value = color.substr(equals + 1);
		if(key.empty() || value.empty()) continue;
		auto ft_style = file_type_names.find(key);
		if((key.at(0) != '*') && (ft_style == file_type_names.end())) continue;
		ftxui::Decorator style = parse_ls_color(value);
		if(key.at(0) == '*') {
			key = key.substr(1);
			std::reverse(key.begin(), key.end());
			opts.ext_styles.insert_or_assign(key, style);
		} else
			opts.ft_styles.insert_or_assign(file_type_names.at(key), style);
	}

	auto regular_file_style =
		opts.ft_styles.at(file_type_names.at("fi"));
	for(const auto [name, type] : file_type_names)
		if(type.second == file_extra::normal)
			opts.ft_styles.emplace(type, regular_file_style);
	for(const auto [name, type] : file_type_names)
		if(type.second != file_extra::normal)
			opts.ft_styles.emplace(type,
				opts.ft_styles.at({type.first, file_extra::normal}));

	return opts;
}

