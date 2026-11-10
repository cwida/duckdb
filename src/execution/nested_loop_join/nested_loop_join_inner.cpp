#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/execution/nested_loop_join.hpp"

namespace duckdb {

struct InitialNestedLoopJoin {
	template <class T, class OP>
	static idx_t Operation(const Vector &left, const Vector &right, idx_t left_size, idx_t right_size, idx_t &lpos,
	                       idx_t &rpos, SelectionVector &lvector, SelectionVector &rvector, idx_t current_match_count) {
		using MATCH_OP = ComparisonOperationWrapper<OP>;

		// initialize phase of nested loop join
		// fill lvector and rvector with matches from the base vectors
		auto left_entries = left.Values<T>();
		auto right_entries = right.Values<T>();

		idx_t result_count = 0;
		for (; rpos < right_size; rpos++) {
			auto right_entry = right_entries[rpos];
			bool right_is_valid = right_entry.IsValid();
			for (; lpos < left_size; lpos++) {
				if (result_count == STANDARD_VECTOR_SIZE) {
					// out of space!
					return result_count;
				}
				auto left_entry = left_entries[lpos];
				bool left_is_valid = left_entry.IsValid();
				if (MATCH_OP::Operation(left_entries.GetValueUnsafe(lpos), right_entries.GetValueUnsafe(rpos),
				                        !left_is_valid, !right_is_valid)) {
					// emit tuple
					lvector.set_index(result_count, lpos);
					rvector.set_index(result_count, rpos);
					result_count++;
				}
			}
			lpos = 0;
		}
		return result_count;
	}
};

struct RefineNestedLoopJoin {
	template <class T, class OP>
	static idx_t Operation(const Vector &left, const Vector &right, idx_t left_size, idx_t right_size, idx_t &lpos,
	                       idx_t &rpos, SelectionVector &lvector, SelectionVector &rvector, idx_t current_match_count) {
		using MATCH_OP = ComparisonOperationWrapper<OP>;

		auto left_entries = left.Values<T>();
		auto right_entries = right.Values<T>();

		// refine phase of the nested loop join
		// refine lvector and rvector based on matches of subsequent conditions (in case there are multiple conditions
		// in the join)
		D_ASSERT(current_match_count > 0);
		idx_t result_count = 0;
		for (idx_t i = 0; i < current_match_count; i++) {
			auto lidx = lvector.get_index(i);
			auto ridx = rvector.get_index(i);
			auto left_entry = left_entries[lidx];
			auto right_entry = right_entries[ridx];
			bool left_is_valid = left_entry.IsValid();
			bool right_is_valid = right_entry.IsValid();
			if (MATCH_OP::Operation(left_entries.GetValueUnsafe(lidx), right_entries.GetValueUnsafe(ridx),
			                        !left_is_valid, !right_is_valid)) {
				lvector.set_index(result_count, lidx);
				rvector.set_index(result_count, ridx);
				result_count++;
			}
		}
		return result_count;
	}
};

template <class NLTYPE, class OP>
static idx_t NestedLoopJoinTypeSwitch(const Vector &left, const Vector &right, idx_t left_size, idx_t right_size,
                                      idx_t &lpos, idx_t &rpos, SelectionVector &lvector, SelectionVector &rvector,
                                      idx_t current_match_count) {
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return NLTYPE::template Operation<int8_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector, rvector,
		                                              current_match_count);
	case PhysicalType::INT16:
		return NLTYPE::template Operation<int16_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector, rvector,
		                                               current_match_count);
	case PhysicalType::INT32:
		return NLTYPE::template Operation<int32_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector, rvector,
		                                               current_match_count);
	case PhysicalType::INT64:
		return NLTYPE::template Operation<int64_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector, rvector,
		                                               current_match_count);
	case PhysicalType::UINT8:
		return NLTYPE::template Operation<uint8_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector, rvector,
		                                               current_match_count);
	case PhysicalType::UINT16:
		return NLTYPE::template Operation<uint16_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                rvector, current_match_count);
	case PhysicalType::UINT32:
		return NLTYPE::template Operation<uint32_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                rvector, current_match_count);
	case PhysicalType::UINT64:
		return NLTYPE::template Operation<uint64_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                rvector, current_match_count);
	case PhysicalType::INT128:
		return NLTYPE::template Operation<hugeint_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                 rvector, current_match_count);
	case PhysicalType::UINT128:
		return NLTYPE::template Operation<uhugeint_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                  rvector, current_match_count);
	case PhysicalType::FLOAT:
		return NLTYPE::template Operation<float, OP>(left, right, left_size, right_size, lpos, rpos, lvector, rvector,
		                                             current_match_count);
	case PhysicalType::DOUBLE:
		return NLTYPE::template Operation<double, OP>(left, right, left_size, right_size, lpos, rpos, lvector, rvector,
		                                              current_match_count);
	case PhysicalType::INTERVAL:
		return NLTYPE::template Operation<interval_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                  rvector, current_match_count);
	case PhysicalType::VARCHAR:
		return NLTYPE::template Operation<string_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                rvector, current_match_count);
	default:
		InternalException::Throw("Unimplemented type for join!");
	}
}

template <class NLTYPE>
idx_t NestedLoopJoinComparisonSwitch(const Vector &left, const Vector &right, idx_t left_size, idx_t right_size,
                                     idx_t &lpos, idx_t &rpos, SelectionVector &lvector, SelectionVector &rvector,
                                     idx_t current_match_count, ExpressionType comparison_type) {
	D_ASSERT(left.GetType() == right.GetType());
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
		return NestedLoopJoinTypeSwitch<NLTYPE, Equals>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                rvector, current_match_count);
	case ExpressionType::COMPARE_NOTEQUAL:
		return NestedLoopJoinTypeSwitch<NLTYPE, NotEquals>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                   rvector, current_match_count);
	case ExpressionType::COMPARE_LESSTHAN:
		return NestedLoopJoinTypeSwitch<NLTYPE, LessThan>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                  rvector, current_match_count);
	case ExpressionType::COMPARE_GREATERTHAN:
		return NestedLoopJoinTypeSwitch<NLTYPE, GreaterThan>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                     rvector, current_match_count);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return NestedLoopJoinTypeSwitch<NLTYPE, LessThanEquals>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                        rvector, current_match_count);
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return NestedLoopJoinTypeSwitch<NLTYPE, GreaterThanEquals>(left, right, left_size, right_size, lpos, rpos,
		                                                           lvector, rvector, current_match_count);
	case ExpressionType::COMPARE_DISTINCT_FROM:
		return NestedLoopJoinTypeSwitch<NLTYPE, DistinctFrom>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                      rvector, current_match_count);
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return NestedLoopJoinTypeSwitch<NLTYPE, NotDistinctFrom>(left, right, left_size, right_size, lpos, rpos,
		                                                         lvector, rvector, current_match_count);
	default:
		NotImplementedException::Throw("Unimplemented comparison type for join!");
	}
}

idx_t NestedLoopJoinInner::Perform(idx_t &lpos, idx_t &rpos, DataChunk &left_conditions, DataChunk &right_conditions,
                                   SelectionVector &lvector, SelectionVector &rvector,
                                   const vector<JoinCondition> &conditions) {
	D_ASSERT(left_conditions.ColumnCount() == right_conditions.ColumnCount());
	if (lpos >= left_conditions.size() || rpos >= right_conditions.size()) {
		return 0;
	}
	// for the first condition, lvector and rvector are not set yet
	// we initialize them using the InitialNestedLoopJoin
	idx_t match_count = NestedLoopJoinComparisonSwitch<InitialNestedLoopJoin>(
	    left_conditions.data[0], right_conditions.data[0], left_conditions.size(), right_conditions.size(), lpos, rpos,
	    lvector, rvector, 0, conditions[0].GetComparisonType());
	// now resolve the rest of the conditions
	for (idx_t i = 1; i < conditions.size(); i++) {
		// check if we have run out of tuples to compare
		if (match_count == 0) {
			return 0;
		}
		// if not, get the vectors to compare
		Vector &l = left_conditions.data[i];
		Vector &r = right_conditions.data[i];
		// then we refine the currently obtained results using the RefineNestedLoopJoin
		match_count = NestedLoopJoinComparisonSwitch<RefineNestedLoopJoin>(
		    l, r, left_conditions.size(), right_conditions.size(), lpos, rpos, lvector, rvector, match_count,
		    conditions[i].GetComparisonType());
	}
	return match_count;
}

} // namespace duckdb
