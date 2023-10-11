#include "lazy.hpp"
#include "filetype.hpp"
#include "trace.hpp"
#include "natkey.hpp"
#include "opts.hpp"
#include "memoize.hpp"

#include <bits/stdc++.h>
#include <unistd.h>
#include <sys/stat.h>

#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/process.hpp>
#include <boost/json.hpp>
#include <boost/asio.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/component/captured_mouse.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>

using namespace std::chrono_literals;

namespace fs = std::filesystem;
namespace proc = boost::process;
namespace opt = boost::program_options;
namespace json = boost::json;
namespace asio = boost::asio;

std::string shell_quote(const std::string& str) {
	if(str.size() == 0) return "''";
	if(!std::regex_search(str, std::regex("[^\\w@%+=:,./-]"))) return str;
	return "'" + std::regex_replace(str, std::regex("'"), "'\"'\"'") + "'";
}

enum struct diff_status {
	unknown,
	matching,
	different,
	leftonly,
	rightonly,
};

typedef std::function<diff_status(
	const fs::path& left,
	const fs::path& right
)> diff_file_func;

bool operator ==(const struct timespec& x, const struct timespec y) {
	return (x.tv_sec == y.tv_sec) && (x.tv_nsec == y.tv_nsec);
}

const diff_file_func diff_file = memoize(
	[](const fs::path& left, const fs::path& right) -> diff_status {
		const file_type left_type = file_type_of(left);
		if(left_type.first == fs::file_type::symlink)
			return diff_file(resolve_symlink(left), right);
		const file_type right_type = file_type_of(right);
		if(right_type.first == fs::file_type::symlink)
			return diff_file(left, resolve_symlink(right));
		if(left_type != right_type)
			return diff_status::different;
		switch(left_type.first) {
			case fs::file_type::regular: {
				struct stat left_stat, right_stat;
				if(stat(left.c_str(), &left_stat))
					throw std::runtime_error(std::strerror(errno));
				if(stat(right.c_str(), &right_stat))
					throw std::runtime_error(std::strerror(errno));
				if(left_stat.st_size != right_stat.st_size)
					return diff_status::different;
				if(left_stat.st_mtim == right_stat.st_mtim)
					return diff_status::matching;
				return diff_status::matching;
			}
			case fs::file_type::directory:
				return diff_status::matching;
			default: return diff_status::unknown;
		}
	}, [](auto& x, auto& y) { return true; });

struct file_entry {
	std::string name;
	diff_status status;
	file_type left;
	file_type right;
};

enum struct app_side {
	left,
	right,
};

struct app_state {
	const app_options opts;
	ftxui::ScreenInteractive screen;
	fs::path cwd;
	std::vector<file_entry> files;
	std::vector<std::string> indexes;
	int index;
	struct {
		bool help;
		bool confirm;
		std::string confirm_message;
		std::function<void()> confirm_continuation;
	} modal ;
	asio::thread_pool pool;
};

void change_directory(app_state& st) {
	st.index = 0;
	st.indexes.clear();
	st.files.clear();
	std::vector<natural_key_type> names;
	for(auto file : fs::directory_iterator(st.opts.left / st.cwd))
		names.push_back(natural_key(file.path().filename()));
	for(auto file : fs::directory_iterator(st.opts.right / st.cwd))
		names.push_back(natural_key(file.path().filename()));
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	trace("--------------------");
	for(auto [ _, name ] : names) {
		auto left = file_type_of(st.opts.left / st.cwd / name);
		auto right = file_type_of(st.opts.right / st.cwd / name);
		trace(tracemanip, std::setw(40), name, "-",
			left.first, left.second, "-", right.first, right.second);
		auto status = diff_status::unknown;
		if(left == file_type_names.at("mi"))
			status = diff_status::rightonly;
		else if(right == file_type_names.at("mi"))
			status = diff_status::leftonly;
		st.files.push_back(file_entry{.name = name,
			.status = status, .left = left, .right = right });
	}
	trace("--------------------");
	for(size_t i = 0; i < st.files.size(); ++i)
		st.indexes.push_back(std::to_string(i));
}

ftxui::Element row_of(
	ftxui::Element left,
	ftxui::Element right,
	const int width
) {
	return ftxui::hbox(
		{ left | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, (width - 1) / 2)
		, ftxui::separator()
		, right | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width / 2)
		});
};

ftxui::ButtonOption button_simple() {
	ftxui::ButtonOption option;
	option.transform = [](const ftxui::EntryState& s) {
		auto element = ftxui::text(s.label);
		if(s.focused) element |= ftxui::inverted;
		return element;
	};
	return option;
}

void action_enter(app_state& st) {
	auto file = st.files[st.index];
	if(file.left.first == fs::file_type::directory
		&& file.right.first == fs::file_type::directory
	) {
		st.cwd = st.cwd / file.name;
		change_directory(st);
		return;
	}
	st.screen.WithRestoredIO([&]() {
		std::string call = st.opts.editor
			+ " " + shell_quote(st.opts.left / st.cwd / file.name)
			+ " " + shell_quote(st.opts.right / st.cwd / file.name);
		proc::system(proc::search_path("bash"), "-c", call,
			proc::std_out > stdout, proc::std_err > stderr, proc::std_in < stdin);
	})();
}

void action_leave(app_state& st) {
	st.cwd = st.cwd.parent_path();
	change_directory(st);
}

std::function<void(app_state&)> action_refresh(bool reset) {
	return [reset](app_state& st) {
		change_directory(st);
	};
}

std::function<void(app_state&)> action_copy(app_side side) {
	return [side](app_state& st) {
		trace(now, "event: copy", side == app_side::left ? "left" : "right");
	};
}

std::function<void(app_state&)> action_delete(app_side side) {
	return [side](app_state& st) {
		trace(now, "event: delete", side == app_side::left ? "left" : "right");
	};
}

std::function<void(app_state&)> action_shell(app_side side) {
	return [side](app_state& st) {
		const char* shell = std::getenv("SHELL");
		if(shell == NULL) shell = "sh";
		std::string cwd = (side == app_side::left
			? st.opts.left : st.opts.right) / st.cwd;
		st.screen.WithRestoredIO([&]() {
			proc::system(proc::search_path(shell),
				proc::std_out > stdout, proc::std_err > stderr, proc::std_in < stdin,
				proc::env["DDIFF_LEFT"] = fs::absolute(st.opts.left / st.cwd),
				proc::env["DDIFF_RIGHT"] = fs::absolute(st.opts.right / st.cwd),
				proc::start_dir(cwd));
		})();
	};
}

struct button_def {
	std::string key;
	std::string name;
	std::string desc;
	std::function<void(app_state&)> func;
};

std::vector<std::pair<std::string, button_def>> button_defs =
	{ { "?", { "?", "close", "close this window",
		[](app_state& st) { st.modal.help ^= true; } } }
	, { "q", { "q", "quit", "quit the app",
		[](app_state& st) { st.screen.Exit(); } } }
	, { "\x1b[C", { "▶", "enter", "enter directory / open files in editor",
		action_enter } }
	, { "\x1b[D", { "◀", "leave", "leave the current directory",
		action_leave } }
	, { "r", { "r", "refresh", "refresh files and diffs",
		action_refresh(false) } }
	, { "R", { "W", "reset", "reset diff cache",
		action_refresh(true) } }
	, { "s", { "s", "shell L", "open shell in the left directory",
		action_shell(app_side::left) } }
	, { "S", { "S", "shell R", "open shell in the right directory",
		action_shell(app_side::right) } }
	, { "c", { "c", "copy L", "copy right to left side",
		action_copy(app_side::left) } }
	, { "C", { "C", "copy R", "copy left to right side",
		action_copy(app_side::right) } }
	, { "d", { "d", "delete L", "delete the left file",
		action_delete(app_side::left) } }
	, { "D", { "D", "delete R", "delete the right file",
		action_delete(app_side::right) } }
	};

std::map<std::string, button_def> button_map =
	std::map(button_defs.begin(), button_defs.end());

ftxui::ComponentDecorator with_buttons(
	app_state* st,
	const std::set<std::string>& buttons
) {
	return ftxui::CatchEvent([=](const ftxui::Event& event) {
		if(event.is_mouse()) return false;
		if(!buttons.contains(event.input())) return false;
		button_map.at(event.input()).func(*st);
		return true;
	});
}

int main(int argc, const char* argv[]) {
	trace("------------------------------------------------------------");
	trace(now, "pid", getpid());

	auto opts = get_opts(argc, argv);
	if(std::holds_alternative<int>(opts))
		return std::get<int>(opts);

	app_state st =
		{ .opts = std::move(std::get<app_options>(opts))
		, .screen = ftxui::ScreenInteractive::Fullscreen()
		, .cwd = fs::path()
		, .files = {}
		, .indexes = {}
		, .index = 0
		, .modal = { false, }
		};

	auto menuopt = ftxui::MenuOption::Vertical();
	menuopt.entries_option.transform = [&](ftxui::EntryState entry) {
		auto file = st.files[std::stoi(entry.label)];
		std::string left_marker, right_marker;
		ftxui::Color left_color, right_color;
		switch(file.status) {
			case diff_status::unknown:
				left_marker  = "?";
				right_marker = "?";
				left_color   = ftxui::Color::Palette16(4);
				right_color  = ftxui::Color::Palette16(4);
				break;
			case diff_status::matching:
				left_marker  = " ";
				right_marker = " ";
				left_color   = ftxui::Color::Palette16(0);
				right_color  = ftxui::Color::Palette16(0);
				break;
			case diff_status::different:
				left_marker  = "*";
				right_marker = "*";
				left_color   = ftxui::Color::Palette16(3);
				right_color  = ftxui::Color::Palette16(3);
				break;
			case diff_status::leftonly:
				left_marker  = "+";
				right_marker = "-";
				left_color   = ftxui::Color::Palette16(2);
				right_color  = ftxui::Color::Palette16(1);
				break;
			case diff_status::rightonly:
				left_marker  = "-";
				right_marker = "+";
				left_color   = ftxui::Color::Palette16(1);
				right_color  = ftxui::Color::Palette16(2);
				break;
		}
		std::string cursor = entry.active ? "▶" : entry.focused ? "▸" : " ";
		ftxui::Decorator left_style = ftxui::nothing, right_style = ftxui::nothing;
		if(st.opts.ext_styles.contains(fs::path(file.name).extension()))
			left_style = right_style = st.opts.ext_styles.at(fs::path(file.name).extension());
		else {
			if(st.opts.ft_styles.contains(file.left))
				left_style = st.opts.ft_styles.at(file.left);
			if(st.opts.ft_styles.contains(file.right))
				right_style = st.opts.ft_styles.at(file.right);
		}
		auto elem = row_of
			( ftxui::hbox(
				{ ftxui::text(left_marker) | ftxui::bgcolor(left_color) | ftxui::bold
				, ftxui::text(cursor)
				, ftxui::text(file.name) | left_style
				, ftxui::filler()
				})
			, ftxui::hbox(
				{ ftxui::text(right_marker) | ftxui::bgcolor(right_color) | ftxui::bold
				, ftxui::text(cursor)
				, ftxui::text(file.name) | right_style
				, ftxui::filler()
				})
			, st.screen.dimx() );
		return elem;
	};
	menuopt.on_change = [&]() { trace(now, "on_change"); };
	menuopt.on_enter = [&]() { action_enter(st); };
	auto menu_component = ftxui::Menu(&st.indexes, &st.index, menuopt);

	change_directory(st);

	ftxui::Components buttons =
		{ ftxui::Button("q Quit", [&]() { button_map["q"].func(st); }, button_simple())
		, ftxui::Button("? Help", [&]() { button_map["?"].func(st); }, button_simple())
		};
	auto footer_component = ftxui::Renderer(
		ftxui::Container::Horizontal(buttons), [&]() {
			ftxui::Elements elems = { buttons[0]->Render() };
			for(size_t i = 1; i < buttons.size(); ++i) {
				elems.push_back(ftxui::text(" "));
				elems.push_back(buttons[i]->Render());
			}
			return ftxui::hbox(elems);
		}
	);

	std::thread([&]() {
		int n = 0;
		for(; n < 5; ++n) {
			trace(now, "timer", n);
			st.screen.Post(ftxui::Event::Custom);
			std::this_thread::sleep_for(1s);
		}
		while(true) {
			trace(now, "timer", n);
			st.screen.Post(ftxui::Event::Custom);
			std::this_thread::sleep_for(30s);
			n += 30;
		}
	}).detach();

	auto main_layout = ftxui::Renderer(
		ftxui::Container::Vertical(
			{ menu_component | with_buttons(&st, { "\x1b[C", "\x1b[D", })
			, footer_component }),
		[&]() { return ftxui::vbox(
			{ row_of
				( ftxui::hbox(
					{ ftxui::text(st.opts.left) | ftxui::bold
					, ftxui::text("/" / st.cwd) })
				, ftxui::hbox(
					{ ftxui::text(st.opts.right) | ftxui::bold
					, ftxui::text("/" / st.cwd) })
				, st.screen.dimx() )
			, ftxui::separator()
			, menu_component->Render() | ftxui::yframe
				| ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, st.screen.dimy() - 4)
			, ftxui::separator()
			, footer_component->Render() | ftxui::xframe
			});
		});

	ftxui::Components help_buttons;
	auto mkfunc = [&](std::function<void(app_state&)> func)
		{ return [func, &st]() { func(st); }; };
	auto button_name_width = std::accumulate(
		button_defs.begin(), button_defs.end(), 0,
		[&](size_t x, auto y) { return std::max(x, y.second.name.size()); });
	std::transform(button_defs.begin(), button_defs.end(),
		std::back_inserter(help_buttons), [&](auto& button) {
			std::ostringstream name;
			name << button.second.key << ' ' << std::setw(button_name_width)
				<< button.second.name << "  " << button.second.desc;
			return ftxui::Button(name.str(),
				mkfunc(button.second.func), button_simple());
		});

	auto help_modal = ftxui::Modal(
		ftxui::Container::Vertical(help_buttons) | ftxui::border,
		&st.modal.help);

	st.screen.Loop(main_layout | help_modal
		| with_buttons(&st, { "?", "q", "r", "R", "s", "S", "c", "C", "d", "D" })
		// | ftxui::CatchEvent([&](ftxui::Event event) {
			// trace(now, event, json::serialize(event.input()));
			// return false;
			// })
		);
}

