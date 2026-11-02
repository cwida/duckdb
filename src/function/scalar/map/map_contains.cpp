#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/list/contains_or_position.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/function/scalar/map_functions.hpp"

namespace duckdb {

static void MapContainsFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	const auto count = input.size();

	const auto &map_vec = input.data[0];
	const auto &key_vec = MapVector::GetKeys(map_vec);
	const auto &arg_vec = input.data[1];

	ListSearchOp<bool>(map_vec, key_vec, arg_vec, result, count);
}

static bool TypeHasCollation(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::VARCHAR:
		return !type.HasAlias() && !StringType::GetCollation(type).empty();
	case LogicalTypeId::LIST:
		return TypeHasCollation(ListType::GetChildType(type));
	case LogicalTypeId::ARRAY:
		return TypeHasCollation(ArrayType::GetChildType(type));
	default:
		return false;
	}
}

static unique_ptr<Expression> BindMapContainsExpression(FunctionBindExpressionInput &input) {
	const auto &key_type = input.children[1]->GetReturnType();
	const auto &map_key_type = MapType::KeyType(input.children[0]->GetReturnType());
	if (!TypeHasCollation(key_type) && !TypeHasCollation(map_key_type)) {
		return nullptr;
	}

	FunctionBinder function_binder(input.context);
	ErrorData error;
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(input.children[0]));
	auto keys = function_binder.BindScalarFunction(Identifier::DefaultSchema(), "map_keys", std::move(children), error);
	if (!keys) {
		error.Throw();
	}
	children.clear();
	children.push_back(std::move(keys));
	children.push_back(std::move(input.children[1]));
	auto result =
	    function_binder.BindScalarFunction(Identifier::DefaultSchema(), "list_contains", std::move(children), error);
	if (!result) {
		error.Throw();
	}
	return result;
}

ScalarFunction MapContainsFun::GetFunction() {
	auto key_type = LogicalType::TEMPLATE("K");
	auto val_type = LogicalType::TEMPLATE("V");

	ScalarFunction fun("map_contains", {}, LogicalType::BOOLEAN, MapContainsFunction);
	fun.GetSignature().AddParameter("map", LogicalType::MAP(key_type, val_type)).AddParameter("key", key_type);
	fun.SetBindExpressionCallback(BindMapContainsExpression);
	return fun;
}

} // namespace duckdb
