#include "core_functions/scalar/generic_functions.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"

namespace duckdb {

namespace {
struct AliasBindData final : public FunctionData {
	explicit AliasBindData(Identifier alias_p) : alias(std::move(alias_p)) {
	}

	Identifier alias;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<AliasBindData>(alias);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<AliasBindData>();
		return alias == other.alias;
	}
};

unique_ptr<FunctionData> AliasBind(BindScalarFunctionInput &input) {
	return make_uniq<AliasBindData>(input.GetArguments()[0]->GetName());
}

void AliasSerialize(Serializer &serializer, const optional_ptr<FunctionData> data, const BoundScalarFunction &) {
	serializer.WriteProperty(100, "alias", data->Cast<AliasBindData>().alias);
}

unique_ptr<FunctionData> AliasDeserialize(Deserializer &deserializer, BoundScalarFunction &) {
	return make_uniq<AliasBindData>(deserializer.ReadProperty<Identifier>(100, "alias"));
}
unique_ptr<ParsedExpression> AliasUnbind(FunctionUnbindInput &input) {
	auto &expression = input.expression;
	if (input.children.size() != 1 || (expression.GetAlias().empty() && !expression.BindInfo())) {
		return nullptr;
	}
	auto value = Value(expression.GetAlias().empty() ? expression.BindInfo()->Cast<AliasBindData>().alias
	                                                 : expression.GetAlias());
	auto call = make_uniq<FunctionExpression>(expression.Function().GetDefinition()->GetQualifiedName(),
	                                          std::move(input.children));
	auto result = make_uniq<CaseExpression>();
	result->CaseChecksMutable().push_back(
	    {make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL, std::move(call)),
	     ConstantExpression::FromValue(value)});
	result->ElseMutable() = ConstantExpression::FromValue(value);
	return std::move(result);
}
} // namespace

static void AliasFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &bind_data = func_expr.BindInfo()->Cast<AliasBindData>();
	Value v(state.expr.GetAlias().empty() ? bind_data.alias : state.expr.GetAlias());
	result.Reference(v, count_t(args.size()));
}

ScalarFunction AliasFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::VARCHAR, AliasFunction, AliasBind);
	fun.GetSignature().AddParameter("expr", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.GetProperties().SetRequiresExpressionNames(true);
	fun.SetSerializeCallback(AliasSerialize);
	fun.SetDeserializeCallback(AliasDeserialize);
	fun.SetUnbindCallback(AliasUnbind);
	return fun;
}

} // namespace duckdb
