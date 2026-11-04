#include "core_functions/scalar/map_functions.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {
namespace {
struct SwitchFunctionBindData : FunctionData {
	SwitchFunctionBindData(Value cases_p, idx_t map_index_p) : cases(std::move(cases_p)), map_index(map_index_p) {
	}

	//! The constant MAP of (WHEN, THEN) pairs
	Value cases;
	idx_t map_index;

	const LogicalType &ReturnType() const {
		return MapType::ValueType(cases.type());
	}

	bool Equals(const FunctionData &other_p) const override {
		const auto &other = other_p.Cast<SwitchFunctionBindData>();
		return cases == other.cases && map_index == other.map_index;
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<SwitchFunctionBindData>(cases, map_index);
	}
};

//! Which argument holds the cases map is a property of the overload that was resolved, not of the
//! argument types: a MAP-typed key argument makes the types ambiguous. Each variation binds its own index.
template <idx_t MAP_INDEX>
unique_ptr<FunctionData> SwitchBindReturnType(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	constexpr idx_t map_index = MAP_INDEX;
	D_ASSERT(map_index < arguments.size());
	// the cases are a constant MAP - usually folded to a constant by the function binder already, otherwise a
	// foldable expression (including the cast to the argument type of the function) that is evaluated here
	auto &cases = *arguments[map_index];
	if (!cases.IsFoldable()) {
		throw BinderException("SWITCH expected a constant map for the cases");
	}
	auto map_value = ExpressionExecutor::EvaluateScalar(context, cases);
	if (map_value.IsNull()) {
		throw BinderException("SWITCH expected a constant map for the cases");
	}
	if (MapValue::GetChildren(map_value).empty()) {
		throw BinderException("No values provided for SWITCH expression");
	}
	return make_uniq<SwitchFunctionBindData>(std::move(map_value), map_index);
}

unique_ptr<Expression> SwitchBindExpression(FunctionBindExpressionInput &input) {
	auto &function_data = input.bind_data->Cast<SwitchFunctionBindData>();
	auto &return_type = function_data.ReturnType();
	auto result = make_uniq<BoundCaseExpression>(return_type);
	idx_t map_index = function_data.map_index;
	unique_ptr<Expression> base_expr = nullptr;
	unique_ptr<Expression> default_expr = nullptr;

	if (map_index == 1) {
		base_expr = std::move(input.children[0]);
	}

	if (input.children.size() > map_index + 1) {
		// If there is an argument after the map_index, we have a default expression
		default_expr = std::move(input.children[map_index + 1]);
	}

	// every entry of the cases map becomes a WHEN key THEN value check
	auto &entries = MapValue::GetChildren(function_data.cases);
	result->CaseChecksMutable().reserve(entries.size());
	for (auto &entry : entries) {
		auto &key_value = StructValue::GetChildren(entry)[0];
		auto &then_value = StructValue::GetChildren(entry)[1];
		auto key = make_uniq<BoundConstantExpression>(key_value);
		BoundCaseCheck case_check;
		if (base_expr) {
			auto max_type =
			    LogicalType::MaxLogicalType(input.context, base_expr->GetReturnType(), key->GetReturnType());
			case_check.when_expr = BoundComparisonExpression::Create(
			    ExpressionType::COMPARE_EQUAL, base_expr->Copy(),
			    BoundCastExpression::AddCastToType(input.context, std::move(key), max_type));
		} else {
			case_check.when_expr =
			    BoundCastExpression::AddCastToType(input.context, std::move(key), LogicalType::BOOLEAN);
		}
		case_check.then_expr = make_uniq<BoundConstantExpression>(then_value);
		result->CaseChecksMutable().push_back(std::move(case_check));
	}
	if (default_expr) {
		result->ElseMutable() = std::move(default_expr);
	} else {
		result->ElseMutable() =
		    BoundCastExpression::AddCastToType(input.context, make_uniq<BoundConstantExpression>(Value()), return_type);
	}
	return std::move(result);
}

} // namespace

ScalarFunctionSet SwitchFun::GetFunctions() {
	auto key_type = LogicalType::TEMPLATE("K");
	auto val_type = LogicalType::TEMPLATE("V");
	ScalarFunctionSet func_set;

	// each variation is paired with the bind function matching the position of its MAP(K, V) parameter
	vector<pair<vector<pair<Identifier, LogicalType>>, bind_scalar_function_t>> function_variations = {
	    {{{"key", key_type}, {"map", LogicalType::MAP(key_type, val_type)}}, SwitchBindReturnType<1>},
	    {{{"key", key_type}, {"map", LogicalType::MAP(key_type, val_type)}, {"value", val_type}},
	     SwitchBindReturnType<1>},
	    {{{"map", LogicalType::MAP(key_type, val_type)}, {"value", val_type}}, SwitchBindReturnType<0>},
	    {{{"map", LogicalType::MAP(key_type, val_type)}}, SwitchBindReturnType<0>}};

	for (const auto &variation : function_variations) {
		auto switch_expression = ScalarFunction(vector<LogicalType> {}, val_type, nullptr, variation.second, nullptr);
		for (const auto &param : variation.first) {
			switch_expression.GetSignature().AddParameter(param.first, param.second);
		}
		switch_expression.SetBindExpressionCallback(SwitchBindExpression);
		// switch is rewritten into a CASE expression, so NULL arguments do not make the result NULL
		switch_expression.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
		func_set.AddFunction(std::move(switch_expression));
	}

	return func_set;
}
} // namespace duckdb
