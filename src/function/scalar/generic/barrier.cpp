#include "duckdb/function/scalar/generic_functions.hpp"

#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

namespace {

void BarrierFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(args.data[0]);
}

unique_ptr<FunctionData> BarrierBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	bound_function.SetReturnType(arguments[0]->GetReturnType());
	return make_uniq<VariableReturnBindData>(bound_function.GetReturnType());
}

unique_ptr<BaseStatistics> BarrierStats(ClientContext &context, FunctionStatisticsInput &input) {
	// the barrier returns its argument unchanged - the statistics of the child carry over
	return input.child_stats[0].ToUnique();
}

ScalarFunction GetBarrierFunction(const LogicalType &type) {
	ScalarFunction fun(BarrierFun::Name, {}, type, BarrierFunction);
	fun.GetSignature().AddParameter("arg", type);
	fun.SetBindCallback(BarrierBind);
	fun.SetStatisticsCallback(BarrierStats);
	// default null handling replaces the call with a NULL constant when the argument is typed as NULL - that would
	// swallow the very expression the barrier exists to preserve, e.g. __internal_barrier(error('boom'))
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// the barrier itself never throws, but marking it fallible keeps the expression out of every optimization that
	// evaluates an expression on more rows than it would otherwise see (dictionary execution, filter reordering)
	fun.SetFallible();
	return fun;
}

} // namespace

ScalarFunction BarrierFun::GetFunction() {
	return GetBarrierFunction(LogicalType::ANY);
}

bool ExpressionBarrier::IsBarrier(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	return expr.Cast<BoundFunctionExpression>().Function().GetName() == BarrierFun::Name;
}

bool ExpressionBarrier::Contains(const Expression &expr) {
	if (IsBarrier(expr)) {
		return true;
	}
	bool contains = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) { contains |= Contains(child); });
	return contains;
}

bool ExpressionBarrier::Required(const Expression &expr) {
	return expr.CanThrow() || expr.IsVolatile() || expr.HasSubquery();
}

unique_ptr<Expression> ExpressionBarrier::Wrap(unique_ptr<Expression> expr) {
	auto return_type = expr->GetReturnType();
	auto function = GetBarrierFunction(return_type);

	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(expr));
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(function), std::move(arguments),
	                                          make_uniq<VariableReturnBindData>(return_type));
}

} // namespace duckdb
