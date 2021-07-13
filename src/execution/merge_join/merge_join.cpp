#include "duckdb/execution/merge_join.hpp"

#include "duckdb/parser/expression/comparison_expression.hpp"

namespace duckdb {

template <class MJ, class L_ARG, class R_ARG>
static idx_t MergeJoinSwitch(L_ARG &l, R_ARG &r) {
	switch (l.type.InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return MJ::template Operation<int8_t>(l, r);
	case PhysicalType::INT16:
		return MJ::template Operation<int16_t>(l, r);
	case PhysicalType::INT32:
		return MJ::template Operation<int32_t>(l, r);
	case PhysicalType::INT64:
		return MJ::template Operation<int64_t>(l, r);
	case PhysicalType::UINT8:
		return MJ::template Operation<uint8_t>(l, r);
	case PhysicalType::UINT16:
		return MJ::template Operation<uint16_t>(l, r);
	case PhysicalType::UINT32:
		return MJ::template Operation<uint32_t>(l, r);
	case PhysicalType::UINT64:
		return MJ::template Operation<uint64_t>(l, r);
	case PhysicalType::INT128:
		return MJ::template Operation<hugeint_t>(l, r);
	case PhysicalType::FLOAT:
		return MJ::template Operation<float>(l, r);
	case PhysicalType::DOUBLE:
		return MJ::template Operation<double>(l, r);
	case PhysicalType::INTERVAL:
		return MJ::template Operation<interval_t>(l, r);
	case PhysicalType::VARCHAR:
		return MJ::template Operation<string_t>(l, r);
	default:
		throw InternalException("Type not implemented for merge join!");
	}
}

template <class T, class L_ARG, class R_ARG>
static idx_t MergeJoinComparisonSwitch(L_ARG &l, R_ARG &r, ExpressionType comparison_type) {
	switch (comparison_type) {
	case ExpressionType::COMPARE_LESSTHAN:
		return MergeJoinSwitch<typename T::LessThan, L_ARG, R_ARG>(l, r);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return MergeJoinSwitch<typename T::LessThanEquals, L_ARG, R_ARG>(l, r);
	case ExpressionType::COMPARE_GREATERTHAN:
		return MergeJoinSwitch<typename T::GreaterThan, L_ARG, R_ARG>(l, r);
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return MergeJoinSwitch<typename T::GreaterThanEquals, L_ARG, R_ARG>(l, r);
	default:
		throw InternalException("Unimplemented comparison type for merge join!");
	}
}

idx_t MergeJoinComplex::Perform(MergeInfo &l, MergeInfo &r, ExpressionType comparison_type) {
	D_ASSERT(l.info_type == MergeInfoType::SCALAR_MERGE_INFO && r.info_type == MergeInfoType::SCALAR_MERGE_INFO);
	auto &left = (ScalarMergeInfo &)l;
	auto &right = (ScalarMergeInfo &)r;
	D_ASSERT(left.type == right.type);
	if (left.order.count == 0 || right.order.count == 0) {
		return 0;
	}
	return MergeJoinComparisonSwitch<MergeJoinComplex, ScalarMergeInfo, ScalarMergeInfo>(left, right, comparison_type);
}

idx_t MergeJoinSimple::Perform(MergeInfo &l, MergeInfo &r, ExpressionType comparison_type) {
	D_ASSERT(l.info_type == MergeInfoType::SCALAR_MERGE_INFO && r.info_type == MergeInfoType::CHUNK_MERGE_INFO);
	auto &left = (ScalarMergeInfo &)l;
	auto &right = (ChunkMergeInfo &)r;
	D_ASSERT(left.type == right.type);
	if (left.order.count == 0 || right.data_chunks.Count() == 0) {
		return 0;
	}
	return MergeJoinComparisonSwitch<MergeJoinSimple, ScalarMergeInfo, ChunkMergeInfo>(left, right, comparison_type);
}

} // namespace duckdb
