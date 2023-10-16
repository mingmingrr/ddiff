#pragma once

#include <unordered_map>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <memory>

#include <boost/functional/hash.hpp>

template<typename Tuple, size_t Index = std::tuple_size<Tuple>::value - 1>
struct HashTuple {
	static void hash_tuple_impl(size_t& seed, Tuple const& tuple) {
		HashTuple<Tuple, Index - 1>::apply(seed, tuple);
		boost::hash_combine(seed, std::get<Index>(tuple));
	}
};

template<typename Tuple>
struct HashTuple<Tuple, 0> {
	static void apply(size_t& seed, Tuple const& tuple) {
		boost::hash_combine(seed, std::get<0>(tuple));
	}
};

template<typename ...Ts>
struct hash_tuple {
	size_t operator()(std::tuple<Ts...> const& ts) const {
		size_t seed = 0;
		HashTuple<std::tuple<Ts...> >::apply(seed, ts);
		return seed;
	}
};

template<typename Val, typename Tok, typename ...Args>
struct memoized {
	typedef std::tuple<typename std::remove_reference<Args>::type...> key_type;

	std::function<Tok(Args...)> init
		= [] (const auto&...) { return Tok(); };
	std::function<bool(Val&,Tok&,Args...)> valid
		= [] (const auto&...) { return true; };
	std::function<Val(Tok&,Args...)> func;

	std::shared_mutex mutex = std::shared_mutex();
	std::unordered_map<key_type, Val, hash_tuple<Args...>> cache = {};

	Val operator ()(Args... args) {
		Tok init = this->init(args...);
		key_type key(args...);
		bool found = false;
		Val val;
		{
			const std::shared_lock<std::shared_mutex> lock(this->mutex);
			typename decltype(cache)::const_iterator find = this->cache.find(key);
			if(find != this->cache.end()) {
				found = true;
				val = find->second;
			}
		}
		if(found && valid(val, init, args...))
			return val;
		val = func(init, args...);
		{
			const std::unique_lock<std::shared_mutex> lock(this->mutex);
			this->cache[key] = val;
		}
		return val;
	}

};

