#include "duckdb/common/types/variant/variant_builder.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/function/scalar/variant_functions.hpp"
#include "duckdb/function/scalar/variant_path_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

namespace {

struct VariantValueIndexBindData : public FunctionData {
public:
	explicit VariantValueIndexBindData(int64_t index) : index(index) {
	}

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<VariantValueIndexBindData>(index);
	}
	bool Equals(const FunctionData &other) const override {
		return index == other.Cast<VariantValueIndexBindData>().index;
	}

public:
	//! 0-based, negative indexes count from the end of the array
	int64_t index;
};

//! Whether the selected value is returned, OBJECT, ARRAY and VARIANT_NULL values are not
bool IsScalarValue(const VariantNode &node) {
	if (node.IsNull() || node.IsMissing()) {
		return false;
	}
	switch (node.GetTypeId()) {
	case VariantLogicalType::VARIANT_NULL:
	case VariantLogicalType::OBJECT:
	case VariantLogicalType::ARRAY:
		return false;
	default:
		return true;
	}
}

optional<VariantNode> ResolveIndex(const VariantNode &node, int64_t index) {
	if (node.IsNull() || node.IsMissing() || node.GetTypeId() != VariantLogicalType::ARRAY) {
		return {};
	}
	const auto elements = node.GetArrayChildren();
	const auto length = elements.size();
	if (index >= 0) {
		if (static_cast<idx_t>(index) >= length) {
			return {};
		}
		return elements[static_cast<idx_t>(index)];
	}
	// -(index + 1) cannot overflow for the minimum int64_t
	const auto from_end = static_cast<idx_t>(-(index + 1));
	if (from_end >= length) {
		return {};
	}
	return elements[length - 1 - from_end];
}

//! Emits the scalar element at the index of each row, or a NULL if there is none
struct VariantValueIndexSource {
	bool Emit(idx_t row, VariantBuilder &builder) const {
		if (!variants.RowIsValid(row)) {
			return true;
		}
		const auto node = ResolveIndex(variants[row], index);
		if (!node || !IsScalarValue(*node)) {
			return true;
		}
		EmitIterator(*node, builder);
		return false;
	}

	const VectorIterator<VectorVariantType> &variants;
	int64_t index;
};

void VariantValuePathFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	VariantPathFunction::ExecuteVariant(input, state, result, IsScalarValue);
}

void VariantValueIndexFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 2);
	D_ASSERT(input.data[1].GetVectorType() == VectorType::CONSTANT_VECTOR);
	const auto count = input.size();
	const VectorIterator<VectorVariantType> variants(input.data[0]);
	const auto index = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<VariantValueIndexBindData>().index;

	VariantValueIndexSource source {variants, index};
	result.Initialize(VectorDataInitialization::UNINITIALIZED, count);
	BuildVariant(source, count, result);
}

unique_ptr<FunctionData> VariantValueIndexBind(BindScalarFunctionInput &input) {
	return make_uniq<VariantValueIndexBindData>(input.GetNonNullConstant(1).GetValue<int64_t>());
}

} // namespace

ScalarFunctionSet VariantValueFun::GetFunctions() {
	auto set = VariantPathFunction::CreateFunctionSet("variant_value", VariantValuePathFunction, LogicalType::VARIANT(),
	                                                  false);

	ScalarFunction index_fun({}, LogicalType::VARIANT(), VariantValueIndexFunction, VariantValueIndexBind);
	index_fun.GetSignature()
	    .AddParameter("input_variant", LogicalType::VARIANT())
	    .AddParameter("index", LogicalType::BIGINT);
	set.AddFunction(std::move(index_fun));
	return set;
}

} // namespace duckdb
