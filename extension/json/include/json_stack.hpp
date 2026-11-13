#pragma once
#include "json_common.hpp"

namespace duckdb {
template <class STACK_ITEM> // preferably a struct
class Stack {
public:
	void Push(STACK_ITEM item) {
		stack.push_back(std::move(item));
	}
	STACK_ITEM Pop() {
		STACK_ITEM item = stack.back();
		stack.pop_back();
		return item;
	}
	bool Empty() const {
		return stack.empty();
	}

private:
	vector<STACK_ITEM> stack;
};

} // namespace duckdb
