#pragma once

#include <ftxui/component/event.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>

std::ostream& operator <<(
	std::ostream& out,
	ftxui::Event event
);

struct time { };
extern struct time now;

std::ostream& operator <<(
	std::ostream& output,
	const struct time& _
);

struct tracemod {};
extern struct tracemod tracemanip;

void tracef(std::ostream& out);

template<typename X, typename ...Xs>
void tracef(std::ostream& out, struct tracemod _, X&& x, Xs&& ...xs) {
	out << x;
	return tracef(out, xs...);
}

template<typename X, typename ...Xs>
void tracef(std::ostream& out, X&& x, Xs&& ...xs) {
	out << x << ' ';
	return tracef(out, xs...);
}

template<typename T>
decltype(auto) identity(T&& t) { return std::forward<T>(t); }

template<typename ...Xs>
decltype(auto) trace(Xs&& ...xs) {
	auto fifo = std::filesystem::path("ddiff.log");
	if(std::filesystem::exists(fifo)) {
		std::ofstream file(fifo);
		tracef(file, xs...);
	}
	return (identity(std::forward<Xs>(xs)), ...);
}

