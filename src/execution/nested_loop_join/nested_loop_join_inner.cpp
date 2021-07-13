#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/execution/nested_loop_join.hpp"

namespace duckdb {

struct InitialNestedLoopJoin {
	template <class T, class OP>
	static idx_t Operation(Vector &left, Vector &right, idx_t left_size, idx_t right_size, idx_t &lpos, idx_t &rpos,
	                       SelectionVector &lvector, SelectionVector &rvector, idx_t current_match_count) {
		// initialize phase of nested loop join
		// fill lvector and rvector with matches from the base vectors
		VectorData left_data, right_data;
		left.Orrify(left_size, left_data);
		right.Orrify(right_size, right_data);

		auto ldata = (T *)left_data.data;
		auto rdata = (T *)right_data.data;
		idx_t result_count = 0;
		for (; rpos < right_size; rpos++) {
			idx_t right_position = right_data.sel->get_index(rpos);
			if (!right_data.validity.RowIsValid(right_position)) {
				continue;
			}
			for (; lpos < left_size; lpos++) {
				if (result_count == STANDARD_VECTOR_SIZE) {
					// out of space!
					return result_count;
				}
				idx_t left_position = left_data.sel->get_index(lpos);
				if (!left_data.validity.RowIsValid(left_position)) {
					continue;
				}
				if (OP::Operation(ldata[left_position], rdata[right_position])) {
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

	template <class T, class OP>
	static idx_t DistinctOperation(Vector &left, Vector &right, idx_t left_size, idx_t right_size, idx_t &lpos,
	                               idx_t &rpos, SelectionVector &lvector, SelectionVector &rvector,
	                               idx_t current_match_count) {
		// initialize phase of nested loop join
		// fill lvector and rvector with matches from the base vectors
		VectorData left_data, right_data;
		left.Orrify(left_size, left_data);
		right.Orrify(right_size, right_data);

		auto ldata = (T *)left_data.data;
		auto rdata = (T *)right_data.data;
		idx_t result_count = 0;
		for (; rpos < right_size; rpos++) {
			idx_t right_position = right_data.sel->get_index(rpos);
			for (; lpos < left_size; lpos++) {
				if (result_count == STANDARD_VECTOR_SIZE) {
					// out of space!
					return result_count;
				}
				idx_t left_position = left_data.sel->get_index(lpos);
				if (OP::Operation(ldata[left_position], rdata[right_position],
				                  !left_data.validity.RowIsValid(left_position),
				                  !right_data.validity.RowIsValid(right_position))) {
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
	static idx_t Operation(Vector &left, Vector &right, idx_t left_size, idx_t right_size, idx_t &lpos, idx_t &rpos,
	                       SelectionVector &lvector, SelectionVector &rvector, idx_t current_match_count) {
		VectorData left_data, right_data;
		left.Orrify(left_size, left_data);
		right.Orrify(right_size, right_data);

		// refine phase of the nested loop join
		// refine lvector and rvector based on matches of subsequent conditions (in case there are multiple conditions
		// in the join)
		D_ASSERT(current_match_count > 0);
		auto ldata = (T *)left_data.data;
		auto rdata = (T *)right_data.data;
		idx_t result_count = 0;
		for (idx_t i = 0; i < current_match_count; i++) {
			auto lidx = lvector.get_index(i);
			auto ridx = rvector.get_index(i);
			auto left_idx = left_data.sel->get_index(lidx);
			auto right_idx = right_data.sel->get_index(ridx);
			if (!left_data.validity.RowIsValid(left_idx) || !right_data.validity.RowIsValid(right_idx)) {
				continue;
			}
			if (OP::Operation(ldata[left_idx], rdata[right_idx])) {
				lvector.set_index(result_count, lidx);
				rvector.set_index(result_count, ridx);
				result_count++;
			}
		}
		return result_count;
	}

	template <class T, class OP>
	static idx_t DistinctOperation(Vector &left, Vector &right, idx_t left_size, idx_t right_size, idx_t &lpos,
	                               idx_t &rpos, SelectionVector &lvector, SelectionVector &rvector,
	                               idx_t current_match_count) {
		VectorData left_data, right_data;
		left.Orrify(left_size, left_data);
		right.Orrify(right_size, right_data);

		// refine phase of the nested loop join
		// refine lvector and rvector based on matches of subsequent conditions (in case there are multiple conditions
		// in the join)
		D_ASSERT(current_match_count > 0);
		auto ldata = (T *)left_data.data;
		auto rdata = (T *)right_data.data;
		idx_t result_count = 0;
		for (idx_t i = 0; i < current_match_count; i++) {
			auto lidx = lvector.get_index(i);
			auto ridx = rvector.get_index(i);
			auto left_idx = left_data.sel->get_index(lidx);
			auto right_idx = right_data.sel->get_index(ridx);
			// null values should be filtered out before
			if (OP::Operation(ldata[left_idx], rdata[right_idx], !left_data.validity.RowIsValid(left_idx),
			                  !right_data.validity.RowIsValid(right_idx))) {
				lvector.set_index(result_count, lidx);
				rvector.set_index(result_count, ridx);
				result_count++;
			}
		}
		return result_count;
	}
};

template <class NLTYPE, class OP>
static idx_t NestedLoopJoinTypeSwitch(Vector &left, Vector &right, idx_t left_size, idx_t right_size, idx_t &lpos,
                                      idx_t &rpos, SelectionVector &lvector, SelectionVector &rvector,
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
		throw InternalException("Unimplemented type for join!");
	}
}

template <class NLTYPE, class OP>
static idx_t DistinctNestedLoopJoinTypeSwitch(Vector &left, Vector &right, idx_t left_size, idx_t right_size,
                                              idx_t &lpos, idx_t &rpos, SelectionVector &lvector,
                                              SelectionVector &rvector, idx_t current_match_count) {
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return NLTYPE::template DistinctOperation<int8_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                      rvector, current_match_count);
	case PhysicalType::INT16:
		return NLTYPE::template DistinctOperation<int16_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                       rvector, current_match_count);
	case PhysicalType::INT32:
		return NLTYPE::template DistinctOperation<int32_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                       rvector, current_match_count);
	case PhysicalType::INT64:
		return NLTYPE::template DistinctOperation<int64_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                       rvector, current_match_count);
	case PhysicalType::UINT8:
		return NLTYPE::template DistinctOperation<uint8_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                       rvector, current_match_count);
	case PhysicalType::UINT16:
		return NLTYPE::template DistinctOperation<uint16_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                        rvector, current_match_count);
	case PhysicalType::UINT32:
		return NLTYPE::template DistinctOperation<uint32_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                        rvector, current_match_count);
	case PhysicalType::UINT64:
		return NLTYPE::template DistinctOperation<uint64_t, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                        rvector, current_match_count);
	case PhysicalType::INT128:
		return NLTYPE::template DistinctOperation<hugeint_t, OP>(left, right, left_size, right_size, lpos, rpos,
		                                                         lvector, rvector, current_match_count);
	case PhysicalType::FLOAT:
		return NLTYPE::template DistinctOperation<float, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                     rvector, current_match_count);
	case PhysicalType::DOUBLE:
		return NLTYPE::template DistinctOperation<double, OP>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                      rvector, current_match_count);
	default:
		throw InternalException("Unimplemented type for join!");
	}
}

template <class NLTYPE>
idx_t NestedLoopJoinComparisonSwitch(Vector &left, Vector &right, idx_t left_size, idx_t right_size, idx_t &lpos,
                                     idx_t &rpos, SelectionVector &lvector, SelectionVector &rvector,
                                     idx_t current_match_count, ExpressionType comparison_type) {
	D_ASSERT(left.GetType() == right.GetType());
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
		return NestedLoopJoinTypeSwitch<NLTYPE, duckdb::Equals>(left, right, left_size, right_size, lpos, rpos, lvector,
		                                                        rvector, current_match_count);
	case ExpressionType::COMPARE_NOTEQUAL:
		return NestedLoopJoinTypeSwitch<NLTYPE, duckdb::NotEquals>(left, right, left_size, right_size, lpos, rpos,
		                                                           lvector, rvector, current_match_count);
	case ExpressionType::COMPARE_LESSTHAN:
		return NestedLoopJoinTypeSwitch<NLTYPE, duckdb::LessThan>(left, right, left_size, right_size, lpos, rpos,
		                                                          lvector, rvector, current_match_count);
	case ExpressionType::COMPARE_GREATERTHAN:
		return NestedLoopJoinTypeSwitch<NLTYPE, duckdb::GreaterThan>(left, right, left_size, right_size, lpos, rpos,
		                                                             lvector, rvector, current_match_count);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return NestedLoopJoinTypeSwitch<NLTYPE, duckdb::LessThanEquals>(left, right, left_size, right_size, lpos, rpos,
		                                                                lvector, rvector, current_match_count);
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return NestedLoopJoinTypeSwitch<NLTYPE, duckdb::GreaterThanEquals>(left, right, left_size, right_size, lpos,
		                                                                   rpos, lvector, rvector, current_match_count);
	case ExpressionType::COMPARE_DISTINCT_FROM:
		return DistinctNestedLoopJoinTypeSwitch<NLTYPE, duckdb::DistinctFrom>(
		    left, right, left_size, right_size, lpos, rpos, lvector, rvector, current_match_count);
	default:
		throw NotImplementedException("Unimplemented comparison type for join!");
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
	    lvector, rvector, 0, conditions[0].comparison);
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
		    conditions[i].comparison);
	}
	return match_count;
}

} // namespace duckdb
