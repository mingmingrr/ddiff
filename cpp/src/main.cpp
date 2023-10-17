#include "lazy.hpp"
#include "fileio.hpp"
#include "trace.hpp"
#include "natkey.hpp"
#include "opts.hpp"
#include "memoize.hpp"
#include "diff.hpp"

#include <string>
#include <filesystem>
#include <chrono>
#include <regex>
#include <vector>
#include <functional>
#include <algorithm>
#include <map>
#include <set>
#include <cstdlib>
#include <utility>
#include <iostream>
#include <shared_mutex>
#include <mutex>

#include <boost/algorithm/string.hpp>
#include <boost/process.hpp>
#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/component/captured_mouse.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>

using namespace std::chrono_literals;

namespace fs = std::filesystem;
namespace proc = boost::process;
namespace asio = boost::asio;

std::string shell_quote(const std::string& str) {
	if(str.size() == 0) return "''";
	if(!std::regex_search(str, std::regex("[^\\w@%+=:,./-]"))) return str;
	return "'" + std::regex_replace(str, std::regex("'"), "'\"'\"'") + "'";
}

bool operator ==(const struct timespec& x, const struct timespec y) {
	return (x.tv_sec == y.tv_sec) && (x.tv_nsec == y.tv_nsec);
}

struct file_entry {
	std::string name;
	diff_status status;
	file_info left;
	file_info right;
};

enum struct app_side {
	left,
	right,
};

struct app_state {
	const app_options opts;
	ftxui::ScreenInteractive screen;
	fs::path cwd = "";
	std::vector<file_entry> files = {};
	std::vector<std::string> indexes = {};
	int index = 0;
	std::map<fs::path, int> indexmap = {};
	struct {
		bool help = false;
		bool confirm = false;
		std::string confirm_message = "";
		std::function<void(app_state&)> confirm_continuation = [](app_state&){};
	} modal ;
	mutable asio::thread_pool pool;
};

void refresh_directory(app_state& st) {
	auto index = st.indexmap.find(st.cwd);
	if(index == st.indexmap.end())
		st.indexmap[st.cwd] = st.index = trace("not found", 0);
	else
		st.index = trace("found", index->second);
	st.indexes.clear();
	st.files.clear();
	std::vector<natural_key_type> names;
	for(auto file : fs::directory_iterator(st.opts.left / st.cwd))
		names.push_back(natural_key(file.path().filename()));
	for(auto file : fs::directory_iterator(st.opts.right / st.cwd))
		names.push_back(natural_key(file.path().filename()));
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	st.files.reserve(names.size());
	for(auto [ _, name ] : names) {
		auto left = get_file_info(st.opts.left / st.cwd / name);
		auto right = get_file_info(st.opts.right / st.cwd / name);
		st.files.push_back(file_entry{
			.name = name, .status = diff_status::unknown,
			.left = left, .right = right });
		if(left.ftype == fs::file_type::not_found)
			st.files.back().status = diff_status::rightonly;
		else if(right.ftype == fs::file_type::not_found)
			st.files.back().status = diff_status::leftonly;
		else
			asio::post(st.pool, [&st, file=&st.files.back()] () {
				file->status = diff_file(file->left, file->right);
				st.screen.Post(ftxui::Event::Custom);
			});
	}
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
	option.transform = [] (const ftxui::EntryState& s) {
		auto element = ftxui::text(s.label);
		if(s.focused) element |= ftxui::inverted;
		return element;
	};
	return option;
}

void action_enter(app_state& st) {
	auto file = st.files[st.index];
	if(file.left.ftype == fs::file_type::directory
		&& file.right.ftype == fs::file_type::directory
	) {
		st.cwd = st.cwd / file.name;
		refresh_directory(st);
		return;
	}
	st.screen.WithRestoredIO([&] () {
		std::string call = st.opts.editor
			+ " " + shell_quote(st.opts.left / st.cwd / file.name)
			+ " " + shell_quote(st.opts.right / st.cwd / file.name);
		proc::system(proc::search_path("bash"), "-c", call,
			proc::std_out > stdout, proc::std_err > stderr, proc::std_in < stdin);
	})();
}

void action_leave(app_state& st) {
	st.cwd = st.cwd.parent_path();
	refresh_directory(st);
}

std::function<void(app_state&)> action_refresh(bool reset) {
	return [reset] (app_state& st) {
		if(reset) {
			const std::unique_lock<std::shared_mutex> lock(get_file_info.mutex);
			get_file_info.cache.clear();
		}
		refresh_directory(st);
	};
}

std::function<void(app_state&)> action_copy(app_side side) {
	return [side] (app_state& st) {
		if(st.index >= st.files.size()) return;
		trace(now, "event: copy", side == app_side::left ? "left" : "right");
		fs::path source = st.opts.left / st.cwd / st.files[st.index].name;
		fs::path target = st.opts.right / st.cwd / st.files[st.index].name;
		if(side == app_side::left) std::swap(source, target);
		if(!fs::exists(source)) return;
		st.modal.confirm_message = "Copy\n " + std::string(source)
			+ "\nto\n " + std::string(target);
		st.modal.confirm_continuation = [=] (app_state& st) {
			fs::copy(source, target,
				fs::copy_options::recursive |
				fs::copy_options::overwrite_existing );
			refresh_directory(st);
		};
		st.modal.confirm = true;
	};
}

std::function<void(app_state&)> action_delete(app_side side) {
	return [side] (app_state& st) {
		if(st.index >= st.files.size()) return;
		trace(now, "event: delete", side == app_side::left ? "left" : "right");
		fs::path target = (side == app_side::left ? st.opts.left : st.opts.right)
			/ st.cwd / st.files[st.index].name;
		if(!fs::exists(target)) return;
		st.modal.confirm_message = "Delete\n " + std::string(target);
		st.modal.confirm_continuation = [=] (app_state& st) {
			fs::remove_all(target);
			refresh_directory(st);
		};
		st.modal.confirm = true;
	};
}

std::function<void(app_state&)> action_shell(app_side side) {
	return [side] (app_state& st) {
		const char* shell = std::getenv("SHELL");
		if(shell == NULL) shell = "sh";
		std::string cwd = (side == app_side::left
			? st.opts.left : st.opts.right) / st.cwd;
		st.screen.WithRestoredIO([&] () {
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
		[] (app_state& st) { st.modal.help ^= true; } } }
	, { "q", { "q", "quit", "quit the app",
		[] (app_state& st) { st.screen.Exit(); } } }
	, { "\x1b[C", { "▶", "enter", "enter directory / open files in editor",
		action_enter } }
	, { "\x1b[D", { "◀", "leave", "leave the current directory",
		action_leave } }
	, { "r", { "r", "refresh", "refresh files and diffs",
		action_refresh(false) } }
	, { "R", { "R", "reset", "reset diff cache",
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
	return ftxui::CatchEvent([=] (const ftxui::Event& event) {
		if(event.is_mouse()) return false;
		auto button = buttons.find(event.input());
		if(button == buttons.end()) return false;
		button_map.at(event.input()).func(*st);
		return true;
	});
}

ftxui::Color operator""_rgb216(unsigned long long rgb) {
	return ftxui::Color::Palette256(16
		+ 36*(rgb/100%10) + 6*(rgb/10%10) + rgb%10);
}

decltype(ftxui::MenuEntryOption::transform) render_entry(app_state& st) {
	return [&] (const ftxui::EntryState& entry) {
		auto file = st.files[std::stoi(entry.label)];
		std::string left_marker, right_marker;
		ftxui::Color left_color, right_color;
		switch(file.status) {
			case diff_status::unknown:
				left_marker  = "?";
				right_marker = "?";
				left_color   = 12_rgb216;
				right_color  = 12_rgb216;
				break;
			case diff_status::matching:
				left_marker  = " ";
				right_marker = " ";
				left_color   = 0_rgb216;
				right_color  = 0_rgb216;
				break;
			case diff_status::different:
				left_marker  = "*";
				right_marker = "*";
				left_color   = 210_rgb216;
				right_color  = 210_rgb216;
				break;
			case diff_status::leftonly:
				left_marker  = "+";
				right_marker = "-";
				left_color   =  30_rgb216;
				right_color  = 300_rgb216;
				break;
			case diff_status::rightonly:
				left_marker  = "-";
				right_marker = "+";
				left_color   = 300_rgb216;
				right_color  =  30_rgb216;
				break;
		}
		std::string cursor = entry.active ? "▶" : entry.focused ? "▸" : " ";
		ftxui::Decorator left_style = ftxui::nothing, right_style = ftxui::nothing;
		auto ext_style = st.opts.ext_styles.find(fs::path(file.name).extension());
		if(ext_style != st.opts.ext_styles.end())
			left_style = right_style = ext_style->second;
		else {
			auto left = st.opts.ft_styles.find(file.left.file_type());
			if(left != st.opts.ft_styles.end())
				left_style = left->second;
			auto right = st.opts.ft_styles.find(file.right.file_type());
			if(right != st.opts.ft_styles.end())
				right_style = right->second;
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
}
	
int main(int argc, const char* argv[]) {
	trace("------------------------------------------------------------");
	trace(now, "pid", getpid());

	auto opts_alt = get_opts(argc, argv);
	if(std::holds_alternative<int>(opts_alt))
		return std::get<int>(opts_alt);
	auto opts = std::get<app_options>(opts_alt);

	app_state st =
		{ .opts = opts
		, .screen = ftxui::ScreenInteractive::Fullscreen()
		, .cwd = fs::path()
		, .files = {}
		, .indexes = {}
		, .index = 0
		, .indexmap = {}
		, .modal =
			{ .help = false
			, .confirm = false
			, .confirm_message = ""
			, .confirm_continuation = [](app_state&){}
			}
		, .pool = asio::thread_pool(std::max(1u, opts.threads))
		};

	auto menuopt = ftxui::MenuOption::Vertical();
	menuopt.entries_option.transform = render_entry(st);
	menuopt.on_change = [&] () { st.indexmap[st.cwd] = trace("on_change", st.cwd, st.index); };
	menuopt.on_enter = [&] () { action_enter(st); };
	auto menu_component = ftxui::Menu(&st.indexes, &st.index, menuopt);

	refresh_directory(st);

	ftxui::Components buttons =
		{ ftxui::Button("q Quit", [&] () { button_map["q"].func(st); }, button_simple())
		, ftxui::Button("? Help", [&] () { button_map["?"].func(st); }, button_simple())
		};
	auto footer_component = ftxui::Renderer(
		ftxui::Container::Horizontal(buttons), [&] () {
			ftxui::Elements elems = { buttons[0]->Render() };
			for(size_t i = 1; i < buttons.size(); ++i) {
				elems.push_back(ftxui::text(" "));
				elems.push_back(buttons[i]->Render());
			}
			return ftxui::hbox(elems);
		}
	);

	auto main_layout = ftxui::Renderer(
		ftxui::Container::Vertical(
			{ menu_component | with_buttons(&st, { "\x1b[C", "\x1b[D" })
			, footer_component }),
		[&] () { return ftxui::vbox(
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
	auto mkfunc = [&] (std::function<void(app_state&)> func)
		{ return [func, &st] () { func(st); }; };
	auto button_name_width = std::accumulate(
		button_defs.begin(), button_defs.end(), 0,
		[&] (size_t x, auto y) { return std::max(x, y.second.name.size()); });
	std::transform(button_defs.begin(), button_defs.end(),
		std::back_inserter(help_buttons), [&] (auto& button) {
			std::ostringstream name;
			name << button.second.key << ' ' << std::setw(button_name_width)
				<< button.second.name << "  " << button.second.desc;
			return ftxui::Button(name.str(),
				mkfunc(button.second.func), button_simple());
		});

	auto help_modal = ftxui::Modal(
		ftxui::Container::Vertical(help_buttons) | ftxui::border,
		&st.modal.help);

	ftxui::Component confirm_component;
	ftxui::Components confirm_buttons =
		{ ftxui::Button("Cancel", [&] () {
			st.modal.confirm = false;
			st.modal.confirm_continuation = [](app_state&){};
			}, button_simple())
		, ftxui::Button("Confirm", [&] () {
			st.modal.confirm = false;
			st.modal.confirm_continuation(st);
			st.modal.confirm_continuation = [](app_state&){};
			confirm_component->SetActiveChild(confirm_buttons[0]);
			}, button_simple())
		};
	confirm_component = ftxui::Container::Horizontal(confirm_buttons);

	auto confirm_modal = ftxui::Modal(ftxui::Renderer(
		confirm_component, [&] () {
			std::vector<std::string> lines;
			boost::split(lines, st.modal.confirm_message, boost::is_any_of("\n"));
			ftxui::Elements texts;
			for(auto line : lines)
				texts.push_back(ftxui::text(line));
			return ftxui::vbox(
				{ ftxui::vbox(texts)
				, ftxui::hbox(
					{ confirm_buttons[0]->Render()
					, ftxui::text(" ")
					, confirm_buttons[1]->Render()
					})
				}) | ftxui::border;
		}), &st.modal.confirm);

	st.screen.Loop(main_layout | help_modal | confirm_modal
		| with_buttons(&st, { "?", "q", "r", "R", "s", "S", "c", "C", "d", "D" })
		);

	return 0;
}

