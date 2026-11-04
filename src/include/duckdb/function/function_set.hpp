//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/function_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function_set_base.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "duckdb/function/window_function.hpp"

namespace duckdb {

class ScalarFunctionSet : public FunctionSet<ScalarFunction> {
public:
	DUCKDB_API explicit ScalarFunctionSet();
	DUCKDB_API explicit ScalarFunctionSet(Identifier name);
	DUCKDB_API explicit ScalarFunctionSet(ScalarFunction fun);

	DUCKDB_API shared_ptr<const ScalarFunction> GetFunctionByArguments(ClientContext &context,
	                                                                   const vector<LogicalType> &arguments);

	//! Mark every overload in the set as fallible (can throw runtime errors)
	void SetFallible() {
		ApplyToFunctions([](ScalarFunction &fun) { fun.SetFallible(); });
	}
	//! Set the stability of every overload in the set
	void SetStability(FunctionStability stability) {
		ApplyToFunctions([&](ScalarFunction &fun) { fun.SetStability(stability); });
	}

	//! Apply the same per-arg property to every overload in the set.
	void SetArgProperties(idx_t arg_idx, ArgProperties props) {
		ApplyToFunctions([&](ScalarFunction &fun) { fun.SetArgProperties(arg_idx, props); });
	}
	void SetArgProperties(const vector<ArgProperties> &props) {
		ApplyToFunctions([&](ScalarFunction &fun) { fun.SetArgProperties(props); });
	}
	void SetUnaryArgProperties(ArgProperties props) {
		ApplyToFunctions([&](ScalarFunction &fun) { fun.SetUnaryArgProperties(props); });
	}
	void SetStatisticsCallback(function_statistics_t callback) {
		ApplyToFunctions([&](ScalarFunction &fun) { fun.SetStatisticsCallback(callback); });
	}
};

class AggregateFunctionSet : public FunctionSet<AggregateFunction> {
public:
	DUCKDB_API explicit AggregateFunctionSet();
	DUCKDB_API explicit AggregateFunctionSet(Identifier name);
	DUCKDB_API explicit AggregateFunctionSet(AggregateFunction fun);

	DUCKDB_API shared_ptr<const AggregateFunction> GetFunctionByArguments(ClientContext &context,
	                                                                      const vector<LogicalType> &arguments);
};

class WindowFunctionSet : public FunctionSet<WindowFunction> {
public:
	DUCKDB_API explicit WindowFunctionSet();
	DUCKDB_API explicit WindowFunctionSet(Identifier name);
	DUCKDB_API explicit WindowFunctionSet(WindowFunction fun);

	DUCKDB_API shared_ptr<const WindowFunction> GetFunctionByArguments(ClientContext &context,
	                                                                   const vector<LogicalType> &arguments);
};

class TableFunctionSet : public FunctionSet<TableFunction> {
public:
	DUCKDB_API explicit TableFunctionSet(Identifier name);
	DUCKDB_API explicit TableFunctionSet(TableFunction fun);

	DUCKDB_API shared_ptr<const TableFunction> GetFunctionByArguments(ClientContext &context,
	                                                                  const vector<LogicalType> &arguments);
};

class PragmaFunctionSet : public FunctionSet<PragmaFunction> {
public:
	DUCKDB_API explicit PragmaFunctionSet(Identifier name);
	DUCKDB_API explicit PragmaFunctionSet(PragmaFunction fun);
};

} // namespace duckdb
