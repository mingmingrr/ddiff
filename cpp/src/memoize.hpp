#pragma once

#include <map>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <memory>

template <class F>
struct extract_call : extract_call<decltype(&F::operator())> {};

template <class C, class R, class... Args>
struct extract_call<R (C::*)(Args...) const> {
	using arg_t = std::tuple<Args...>;
	using ret_t = R;
};

template <class C, class R, class... Args>
struct extract_call<R (C::*)(Args...)> {
	using arg_t = std::tuple<Args...>;
	using ret_t = R;
};

template<typename F, typename V>
auto memoize(F&& fn, V&& valid) {
	using call_t = extract_call<std::decay_t<F>>;
	using arg_t = typename call_t::arg_t;
	using ret_t = typename call_t::ret_t;
	std::map<arg_t, ret_t> cache;
	std::shared_ptr<std::shared_mutex> lock(new std::shared_mutex());
	return [=]<std::size_t ...I>(std::index_sequence<I...>) {
		return [=](typename std::tuple_element<I, arg_t>::type... args) mutable {
			arg_t key(args...);
			bool found = false;
			ret_t val;
			{
				const std::shared_lock<std::shared_mutex> _l(*lock);
				typename decltype(cache)::const_iterator find = cache.find(key);
				if(find != cache.end()) {
					found = true;
					val = find->second;
				}
			}
			if(found && valid(key, val))
				return val;
			val = fn(args...);
			{
				const std::unique_lock<std::shared_mutex> _l(*lock);
				cache[std::move(key)] = std::move(val);
			}
			return val;
		};
	}(std::make_index_sequence<std::tuple_size_v<arg_t>>{});
}

