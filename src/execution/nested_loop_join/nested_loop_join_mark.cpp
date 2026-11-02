#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/mark_join_row_comparison.hpp"
#include "duckdb/execution/nested_loop_join.hpp"

namespace duckdb {

template <class T, class OP>
static void TemplatedMarkJoin(const Vector &left, const Vector &right, idx_t lcount, idx_t rcount, bool found_match[]) {
	using MATCH_OP = ComparisonOperationWrapper<OP>;

	auto left_entries = left.Values<T>();
	auto right_entries = right.Values<T>();
	for (idx_t i = 0; i < lcount; i++) {
		if (found_match[i]) {
			continue;
		}
		auto left_entry = left_entries[i];
		const auto left_null = !left_entry.IsValid();
		if (!MATCH_OP::COMPARE_NULL && left_null) {
			continue;
		}
		for (idx_t j = 0; j < rcount; j++) {
			auto right_entry = right_entries[j];
			const auto right_null = !right_entry.IsValid();
			if (!MATCH_OP::COMPARE_NULL && right_null) {
				continue;
			}
			if (MATCH_OP::template Operation<T>(left_entries.GetValueUnsafe(i), right_entries.GetValueUnsafe(j),
			                                    left_null, right_null)) {
				found_match[i] = true;
				break;
			}
		}
	}
}

static void MarkJoinNested(const Vector &left, const Vector &right, idx_t lcount, idx_t rcount, bool found_match[],
                           ExpressionType comparison_type, optional_ptr<bool> found_unknown) {
	if (left.GetType().id() == LogicalTypeId::TUPLE &&
	    (comparison_type == ExpressionType::COMPARE_EQUAL || comparison_type == ExpressionType::COMPARE_NOTEQUAL)) {
		for (idx_t left_row = 0; left_row < lcount; left_row++) {
			if (found_match[left_row]) {
				continue;
			}
			bool equality_is_false[STANDARD_VECTOR_SIZE] = {false};
			bool equality_is_unknown[STANDARD_VECTOR_SIZE] = {false};
			MarkJoinRowComparison::CompareEquality(left, left_row, lcount, right, rcount, equality_is_false,
			                                       equality_is_unknown);
			for (idx_t right_row = 0; right_row < rcount; right_row++) {
				const bool is_match = comparison_type == ExpressionType::COMPARE_EQUAL
				                          ? !equality_is_false[right_row] && !equality_is_unknown[right_row]
				                          : equality_is_false[right_row];
				if (is_match) {
					found_match[left_row] = true;
					break;
				}
				if (found_unknown && equality_is_unknown[right_row]) {
					found_unknown.get()[left_row] = true;
				}
			}
		}
		return;
	}

	Vector left_reference(left.GetType());
	for (idx_t i = 0; i < lcount; i++) {
		if (found_match[i]) {
			continue;
		}
		ConstantVector::Reference(left_reference, count_t(rcount), left, i, lcount);
		idx_t count;
		ValidityMask null_mask(rcount);
		null_mask.SetAllValid(rcount);
		switch (comparison_type) {
		case ExpressionType::COMPARE_EQUAL:
			count = VectorOperations::Equals(left_reference, right, nullptr, rcount, nullptr, nullptr, &null_mask);
			break;
		case ExpressionType::COMPARE_NOTEQUAL:
			count = VectorOperations::NotEquals(left_reference, right, nullptr, rcount, nullptr, nullptr, &null_mask);
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			count = VectorOperations::LessThan(left_reference, right, nullptr, rcount, nullptr, nullptr, &null_mask);
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			count = VectorOperations::GreaterThan(left_reference, right, nullptr, rcount, nullptr, nullptr, &null_mask);
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			count =
			    VectorOperations::LessThanEquals(left_reference, right, nullptr, rcount, nullptr, nullptr, &null_mask);
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			count = VectorOperations::GreaterThanEquals(left_reference, right, nullptr, rcount, nullptr, nullptr,
			                                            &null_mask);
			break;
		case ExpressionType::COMPARE_DISTINCT_FROM:
			count = VectorOperations::DistinctFrom(left_reference, right, nullptr, rcount, nullptr, nullptr);
			break;
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
			count = VectorOperations::NotDistinctFrom(left_reference, right, nullptr, rcount, nullptr, nullptr);
			break;
		default:
			throw InternalException("Unsupported comparison type for MarkJoinNested");
		}
		if (count > 0) {
			found_match[i] = true;
		} else if (found_unknown && null_mask.CountValid(rcount) < rcount) {
			found_unknown.get()[i] = true;
		}
	}
}

template <class OP>
static void MarkJoinSwitch(const Vector &left, const Vector &right, idx_t lcount, idx_t rcount, bool found_match[]) {
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return TemplatedMarkJoin<int8_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::INT16:
		return TemplatedMarkJoin<int16_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::INT32:
		return TemplatedMarkJoin<int32_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::INT64:
		return TemplatedMarkJoin<int64_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::INT128:
		return TemplatedMarkJoin<hugeint_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::UINT8:
		return TemplatedMarkJoin<uint8_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::UINT16:
		return TemplatedMarkJoin<uint16_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::UINT32:
		return TemplatedMarkJoin<uint32_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::UINT64:
		return TemplatedMarkJoin<uint64_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::UINT128:
		return TemplatedMarkJoin<uhugeint_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::FLOAT:
		return TemplatedMarkJoin<float, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::DOUBLE:
		return TemplatedMarkJoin<double, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::VARCHAR:
		return TemplatedMarkJoin<string_t, OP>(left, right, lcount, rcount, found_match);
	default:
		throw NotImplementedException("Unimplemented type for mark join!");
	}
}

static void MarkJoinComparisonSwitch(const Vector &left, const Vector &right, idx_t lcount, idx_t rcount,
                                     bool found_match[], ExpressionType comparison_type,
                                     optional_ptr<bool> found_unknown) {
	switch (left.GetType().InternalType()) {
	case PhysicalType::STRUCT:
	case PhysicalType::LIST:
	case PhysicalType::ARRAY:
		return MarkJoinNested(left, right, lcount, rcount, found_match, comparison_type, found_unknown);
	default:
		break;
	}
	D_ASSERT(left.GetType() == right.GetType());
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
		return MarkJoinSwitch<Equals>(left, right, lcount, rcount, found_match);
	case ExpressionType::COMPARE_NOTEQUAL:
		return MarkJoinSwitch<NotEquals>(left, right, lcount, rcount, found_match);
	case ExpressionType::COMPARE_LESSTHAN:
		return MarkJoinSwitch<LessThan>(left, right, lcount, rcount, found_match);
	case ExpressionType::COMPARE_GREATERTHAN:
		return MarkJoinSwitch<GreaterThan>(left, right, lcount, rcount, found_match);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return MarkJoinSwitch<LessThanEquals>(left, right, lcount, rcount, found_match);
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return MarkJoinSwitch<GreaterThanEquals>(left, right, lcount, rcount, found_match);
	case ExpressionType::COMPARE_DISTINCT_FROM:
		return MarkJoinSwitch<DistinctFrom>(left, right, lcount, rcount, found_match);
	default:
		throw NotImplementedException("Unimplemented comparison type for join!");
	}
}

void NestedLoopJoinMark::Perform(DataChunk &left, ColumnDataCollection &right, bool found_match[],
                                 const vector<JoinCondition> &conditions, optional_ptr<bool> found_unknown) {
	D_ASSERT(conditions.size() == 1);
	// initialize a new temporary selection vector for the left chunk
	// loop over all chunks in the RHS
	ColumnDataScanState scan_state;
	right.InitializeScan(scan_state);

	DataChunk scan_chunk;
	right.InitializeScanChunk(scan_chunk);

	while (right.Scan(scan_state, scan_chunk)) {
		MarkJoinComparisonSwitch(left.data[0], scan_chunk.data[0], left.size(), scan_chunk.size(), found_match,
		                         conditions[0].GetComparisonType(), found_unknown);
	}
}

} // namespace duckdb
