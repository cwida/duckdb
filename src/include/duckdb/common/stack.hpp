//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/stack.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stack>

namespace duckdb {
using std::stack;

//! Version of std::stack that can iterate through the elements (used for debugging)
template <typename T, typename Container = std::deque<T>>
struct InspectableStack : public std::stack<T, Container> {
	using std::stack<T, Container>::stack;

	// expose the underlying container for iteration/inspection
	auto begin() const {
		return this->c.begin();
	}
	auto end() const {
		return this->c.end();
	}
	auto size() const {
		return this->c.size();
	}

	const T &operator[](typename Container::size_type i) const {
		return this->c[i];
	}
};

} // namespace duckdb
