#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

namespace {

struct ConstantOrNullBindData : public FunctionData {
	explicit ConstantOrNullBindData(Value val) : value(std::move(val)) {
	}

	Value value;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ConstantOrNullBindData>(value);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ConstantOrNullBindData>();
		return value == other.value;
	}
};

static void ConstantOrNullFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<ConstantOrNullBindData>();
	result.Reference(info.value, count_t(args.size()));
	for (idx_t idx = 1; idx < args.ColumnCount(); idx++) {
		switch (args.data[idx].GetVectorType()) {
		case VectorType::FLAT_VECTOR: {
			auto &input_mask = FlatVector::ValidityMutable(args.data[idx]);
			if (input_mask.CanHaveNull()) {
				// there are null values: need to merge them into the result
				result.Flatten();
				auto &result_mask = FlatVector::ValidityMutable(result);
				result_mask.EnsureWritable();
				result_mask.Combine(input_mask, args.size());
			}
			break;
		}
		case VectorType::CONSTANT_VECTOR: {
			if (ConstantVector::IsNull(args.data[idx])) {
				// input is constant null, return constant null
				auto &result_mask = ConstantVector::Validity(result);
				auto &input_mask = ConstantVector::Validity(args.data[idx]);
				result_mask.Initialize(input_mask);
				ConstantVector::SetNull(result, count_t(args.size()));
				return;
			}
			break;
		}
		default: {
			auto entries = args.data[idx].Validity();
			if (entries.CanHaveNull()) {
				result.Flatten();
				auto &result_mask = FlatVector::ValidityMutable(result);
				for (idx_t i = 0; i < args.size(); i++) {
					if (!entries.IsValid(i)) {
						result_mask.SetInvalid(i);
					}
				}
			}
			break;
		}
		}
	}
}

//! Resolves the type of the leading constant before binding, so that a call folded on a typed NULL keeps it
void ConstantOrNullResolveTypes(ResolveScalarFunctionTypesInput &input) {
	D_ASSERT(input.GetArgumentCount() >= 2);
	input.GetBoundFunction().SetReturnType(input.GetArgumentType(0));
}

unique_ptr<FunctionData> ConstantOrNullBind(BindScalarFunctionInput &input) {
	return make_uniq<ConstantOrNullBindData>(input.GetConstant(0));
}

} // namespace

bool ConstantOrNull::IsConstantOrNull(BoundFunctionExpression &expr, const Value &val) {
	if (expr.Function().GetName() != "constant_or_null") {
		return false;
	}
	D_ASSERT(expr.BindInfo());
	auto &bind_data = expr.BindInfo()->Cast<ConstantOrNullBindData>();
	D_ASSERT(bind_data.value.type() == val.type());
	return bind_data.value == val;
}

ScalarFunction ConstantOrNullFun::GetFunction() {
	auto fun = ScalarFunction("constant_or_null", {}, LogicalType::ANY, ConstantOrNullFunction);
	fun.GetSignature().AddParameter("arg1", LogicalType::ANY).AddParameter("arg2", LogicalType::ANY);
	fun.SetResolveTypesCallback(ConstantOrNullResolveTypes);
	fun.SetBindCallback(ConstantOrNullBind);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	return fun;
}

} // namespace duckdb
