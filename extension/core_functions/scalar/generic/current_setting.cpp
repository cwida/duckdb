#include "core_functions/scalar/generic_functions.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception/parser_exception.hpp"

namespace duckdb {

namespace {
struct CurrentSettingBindData : public FunctionData {
	explicit CurrentSettingBindData(Value value_p) : value(std::move(value_p)) {
	}

	Value value;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CurrentSettingBindData>(value);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<CurrentSettingBindData>();
		return Value::NotDistinctFrom(value, other.value);
	}
};

void CurrentSettingFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<CurrentSettingBindData>();
	result.Reference(info.value, count_t(args.size()));
}

unique_ptr<FunctionData> CurrentSettingBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();

	auto key_val = input.GetNonNullConstant(0);
	auto key = key_val.GetValue<Identifier>();
	if (key.empty()) {
		throw ParserException("Key name for current_setting must not be empty");
	}

	Value val;
	if (!context.TryGetCurrentSetting(key, val)) {
		auto extension_name = Catalog::AutoloadExtensionByConfigName(context, key);
		// If autoloader didn't throw, the config is now available
		context.TryGetCurrentSetting(key, val);
	}

	bound_function.SetReturnType(val.type());
	return make_uniq<CurrentSettingBindData>(val);
}

} // namespace

ScalarFunction CurrentSettingFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::ANY, CurrentSettingFunction, CurrentSettingBind);
	fun.GetSignature().AddParameter("setting_name", LogicalType::VARCHAR);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// the value of the setting can change between queries
	fun.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	return fun;
}

} // namespace duckdb
