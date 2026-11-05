#include "duckdb/function/builtin_function_lookup.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/window_function_catalog_entry.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

namespace {

//! Built-ins are always looked up fully qualified in the system catalog, so a macro or user-defined function of the
//! same name in the search path can never be selected instead.
QualifiedName BuiltinName(Catalog &catalog, const Identifier &name) {
	return QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), name);
}

} // namespace

shared_ptr<const ScalarFunction> GetBuiltinScalarFunction(ClientContext &context, const Identifier &name,
                                                          const vector<LogicalType> &arguments) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(context, BuiltinName(catalog, name));
	return entry.functions.GetFunctionByArguments(context, arguments);
}

shared_ptr<const AggregateFunction> GetBuiltinAggregateFunction(ClientContext &context, const Identifier &name,
                                                                const vector<LogicalType> &arguments) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<AggregateFunctionCatalogEntry>(context, BuiltinName(catalog, name));
	return entry.functions.GetFunctionByArguments(context, arguments);
}

shared_ptr<const AggregateFunction> TryGetBuiltinAggregateFunction(ClientContext &context, const Identifier &name,
                                                                   const vector<LogicalType> &arguments) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<AggregateFunctionCatalogEntry>(context, BuiltinName(catalog, name));

	auto index = FunctionBinder::SelectFunction(context, entry.functions, arguments);
	if (!index.IsSuccess()) {
		return nullptr;
	}
	return index.GetBinding().GetFunction<AggregateFunction>();
}

shared_ptr<const WindowFunction> GetBuiltinWindowFunction(ClientContext &context, const Identifier &name,
                                                          const vector<LogicalType> &arguments) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<WindowFunctionCatalogEntry>(context, BuiltinName(catalog, name));
	return entry.functions.GetFunctionByArguments(context, arguments);
}

unique_ptr<Expression> BindBuiltinScalarExpression(ClientContext &context, const Identifier &name,
                                                   vector<unique_ptr<Expression>> children) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(context, BuiltinName(catalog, name));

	// Bind the children as the arguments of a call, so that functions that capture the aliases of their arguments
	// (e.g. struct_pack) receive them as named arguments
	vector<pair<Identifier, unique_ptr<Expression>>> arguments;
	arguments.reserve(children.size());
	for (auto &child : children) {
		arguments.emplace_back(Identifier(), std::move(child));
	}

	ErrorData error;
	FunctionBinder function_binder(context);
	auto expr = function_binder.BindScalarFunction(entry, std::move(arguments), error);
	if (!expr) {
		error.Throw();
	}
	return expr;
}

unique_ptr<BoundFunctionExpression> BindBuiltinScalarFunction(ClientContext &context, const Identifier &name,
                                                              vector<unique_ptr<Expression>> children) {
	auto expr = BindBuiltinScalarExpression(context, name, std::move(children));
	if (expr->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		throw InvalidInputException("BindBuiltinScalarFunction did not return a BoundFunctionExpression");
	}
	return unique_ptr_cast<Expression, BoundFunctionExpression>(std::move(expr));
}

} // namespace duckdb
