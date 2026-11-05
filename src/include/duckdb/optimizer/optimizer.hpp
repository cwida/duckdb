//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/common/enums/optimizer_type.hpp"

#include <functional>

namespace duckdb {
class Binder;
class FilterStatisticsOptimizer;
class SQLStatement;
struct MetricsTimer;

class Optimizer {
public:
	Optimizer(Binder &binder, ClientContext &context);

	//! Optimize a plan by running specialized optimizers
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> plan);
	//! Lower aggregates whose ordinary grouped execution requires an explicit relational plan.
	unique_ptr<LogicalOperator> LowerMandatoryAggregateRewrites(unique_ptr<LogicalOperator> plan);
	//! Return a reference to the client context of this optimizer
	ClientContext &GetContext();
	//! Whether the specific optimizer is disabled
	bool OptimizerDisabled(OptimizerType type);
	static bool OptimizerDisabled(ClientContext &context, OptimizerType type);

	//! Pre-binder statement-level optimization pass
	void OptimizeStatement(unique_ptr<SQLStatement> &statement);

public:
	ClientContext &context;
	Binder &binder;
	ExpressionRewriter rewriter;

private:
	friend class FilterStatisticsOptimizer;

	void RunBuiltInOptimizers();
	void RunOptimizer(OptimizerType type, const std::function<void()> &callback);
	MetricsTimer StartProfiler(OptimizerType type);
	void Verify(LogicalOperator &op);

public:
	// helper functions
	unique_ptr<Expression> BindScalarFunction(const Identifier &name, unique_ptr<Expression> c1);
	unique_ptr<Expression> BindScalarFunction(const Identifier &name, unique_ptr<Expression> c1,
	                                          unique_ptr<Expression> c2);
	unique_ptr<Expression> BindScalarFunction(const Identifier &name, unique_ptr<Expression> c1,
	                                          unique_ptr<Expression> c2, unique_ptr<Expression> c3);

private:
	unique_ptr<LogicalOperator> plan;

private:
	unique_ptr<Expression> BindScalarFunction(const Identifier &name, vector<unique_ptr<Expression>> children);
};

} // namespace duckdb
