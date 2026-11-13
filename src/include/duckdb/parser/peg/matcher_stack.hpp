//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/peg/matcher_stack.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/parser/peg/matcher.hpp"

namespace duckdb {

struct MatchStackFrame;

class MatchStack {
public:
	MatchStack();
	~MatchStack();

	MatcherResult Execute(MatchInput input);

private:
	friend struct MatchStackFrame;
	static constexpr idx_t INITIAL_FRAME_CAPACITY = 64;

	template <bool SINGLE_STEP>
	static MatchStep ExecuteInternal(optional_ptr<MatchStack> stack, MatchStackFrame &initial_frame,
	                                 optional<MatcherResult> child_result);
	MatcherResult ExecuteAtomicMatcher(MatchInput input);
	optional<MatcherResult> PushFrame(MatchInput input);
	void DestroyTopFrame();
	void Clear();

private:
	vector<arena_ptr<MatchStackFrame>> frames;
};

} // namespace duckdb
