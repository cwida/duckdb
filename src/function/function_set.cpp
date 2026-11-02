#include "duckdb/function/function_set.hpp"
#include "duckdb/function/function_binder.hpp"

namespace duckdb {

ScalarFunctionSet::ScalarFunctionSet() : FunctionSet("") {
}

ScalarFunctionSet::ScalarFunctionSet(Identifier name) : FunctionSet(std::move(name)) {
}

ScalarFunctionSet::ScalarFunctionSet(ScalarFunction fun) : FunctionSet(fun.name) {
	AddFunction(std::move(fun));
}

shared_ptr<const ScalarFunction> ScalarFunctionSet::GetFunctionByArguments(ClientContext &context,
                                                                           const vector<LogicalType> &arguments) {
	auto index = FunctionBinder::SelectFunction(context, *this, arguments);
	if (!index.IsSuccess()) {
		throw BinderException("Failed to find function %s(%s)\n%s", name, StringUtil::ToString(arguments, ","),
		                      index.GetError().RawMessage());
	}
	return index.GetBinding().GetFunction<ScalarFunction>();
}

AggregateFunctionSet::AggregateFunctionSet() : FunctionSet("") {
}

AggregateFunctionSet::AggregateFunctionSet(Identifier name) : FunctionSet(std::move(name)) {
}

AggregateFunctionSet::AggregateFunctionSet(AggregateFunction fun) : FunctionSet(fun.name) {
	AddFunction(std::move(fun));
}

shared_ptr<const AggregateFunction> AggregateFunctionSet::GetFunctionByArguments(ClientContext &context,
                                                                                 const vector<LogicalType> &arguments) {
	auto index = FunctionBinder::SelectFunction(context, *this, arguments);
	if (!index.IsSuccess()) {
		throw BinderException("Failed to find function %s(%s)\n%s", name, StringUtil::ToString(arguments, ","),
		                      index.GetError().RawMessage());
	}
	return index.GetBinding().GetFunction<AggregateFunction>();
}

WindowFunctionSet::WindowFunctionSet() : FunctionSet("") {
}

WindowFunctionSet::WindowFunctionSet(Identifier name) : FunctionSet(std::move(name)) {
}

WindowFunctionSet::WindowFunctionSet(WindowFunction fun) : FunctionSet(fun.name) {
	AddFunction(std::move(fun));
}

shared_ptr<const WindowFunction> WindowFunctionSet::GetFunctionByArguments(ClientContext &context,
                                                                           const vector<LogicalType> &arguments) {
	auto index = FunctionBinder::SelectFunction(context, *this, arguments);
	if (!index.IsSuccess()) {
		throw BinderException("Failed to find function %s(%s)\n%s", name, StringUtil::ToString(arguments, ","),
		                      index.GetError().RawMessage());
	}
	return index.GetBinding().GetFunction<WindowFunction>();
}

TableFunctionSet::TableFunctionSet(Identifier name) : FunctionSet(std::move(name)) {
}

TableFunctionSet::TableFunctionSet(TableFunction fun) : FunctionSet(fun.name) {
	AddFunction(std::move(fun));
}

shared_ptr<const TableFunction> TableFunctionSet::GetFunctionByArguments(ClientContext &context,
                                                                         const vector<LogicalType> &arguments) {
	auto index = FunctionBinder::SelectFunction(context, *this, arguments);
	if (!index.IsSuccess()) {
		throw BinderException("Failed to find function %s(%s)\n%s", name, StringUtil::ToString(arguments, ","),
		                      index.GetError().RawMessage());
	}
	return index.GetBinding().GetFunction<TableFunction>();
}

PragmaFunctionSet::PragmaFunctionSet(Identifier name) : FunctionSet(std::move(name)) {
}

PragmaFunctionSet::PragmaFunctionSet(PragmaFunction fun) : FunctionSet(fun.name) {
	AddFunction(std::move(fun));
}

} // namespace duckdb
