#include "duckdb/common/vector/union_vector.hpp"
#include "core_functions/scalar/union_functions.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"

namespace duckdb {

namespace {

struct UnionValueBindData : public FunctionData {
	UnionValueBindData() {
	}

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<UnionValueBindData>();
	}
	bool Equals(const FunctionData &other_p) const override {
		return true;
	}
};

void UnionValueFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// Assign the new entries to the result vector
	UnionVector::GetMember(result, 0).Reference(args.data[0]);

	// Set the result tag vector to a constant value
	auto &tag_vector = UnionVector::GetTags(result);
	tag_vector.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<union_tag_t>(tag_vector)[0] = 0;
}

unique_ptr<FunctionData> UnionValueBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.size() != 1) {
		throw BinderException("union_value takes exactly one argument");
	}
	child_list_t<LogicalType> union_members;
	union_members.emplace_back(make_pair(input.GetArgumentName(0), arguments[0]->GetReturnType()));

	bound_function.SetReturnType(LogicalType::UNION(std::move(union_members)));
	return make_uniq<VariableReturnBindData>(bound_function.GetReturnType());
}

unique_ptr<ParsedExpression> UnionValueUnbind(FunctionUnbindInput &input) {
	auto &expression = input.expression;
	D_ASSERT(input.children.size() == 1);
	D_ASSERT(UnionType::GetMemberCount(expression.GetReturnType()) == 1);
	vector<FunctionArgument> arguments;
	arguments.emplace_back(UnionType::GetMemberName(expression.GetReturnType(), 0), std::move(input.children[0]));
	return make_uniq<FunctionExpression>(expression.Function().GetDefinition()->GetQualifiedName(),
	                                     std::move(arguments));
}

} // namespace

ScalarFunction UnionValueFun::GetFunction() {
	ScalarFunction fun("union_value", {}, LogicalTypeId::UNION, UnionValueFunction, UnionValueBind, nullptr, nullptr);
	fun.GetSignature().AddKwargs("kwargs", LogicalType::ANY);
	fun.GetProperties().SetRequiresExpressionNames(true);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetSerializeCallback(VariableReturnBindData::Serialize);
	fun.SetDeserializeCallback(VariableReturnBindData::Deserialize);
	fun.SetUnbindCallback(UnionValueUnbind);
	return fun;
}

} // namespace duckdb
