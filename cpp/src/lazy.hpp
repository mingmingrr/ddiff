#pragma once

#include <variant>
#include <functional>

template<typename T>
struct lazy : public std::variant<T, std::function<T()>> {
	public:
	using std::variant<T, std::function<T()>>::variant;

	T& operator () () {
		if(this->index() == 1)
			*this = std::get<1>(*this)();
		return std::get<0>(*this);
	}
};

