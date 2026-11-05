#include "duckdb/planner/logical_operator_repeatability.hpp"

#include "duckdb/function/window_function.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_sample.hpp"

namespace duckdb {

static bool ExpressionIsRepeatable(unique_ptr<Expression> &expression) {
	if (expression->IsVolatile()) {
		return false;
	}
	bool repeatable = true;
	ExpressionIterator::VisitExpression<BoundAggregateExpression>(*expression, [&](const auto &aggregate) {
		repeatable = repeatable && aggregate.Function().GetStability() != FunctionStability::VOLATILE;
	});
	ExpressionIterator::VisitExpression<BoundWindowExpression>(*expression, [&](const auto &window) {
		repeatable =
		    repeatable &&
		    (!window.AggregateFunction() ||
		     window.AggregateFunction()->GetStability() != FunctionStability::VOLATILE) &&
		    (!window.WindowFunction() || window.WindowFunction()->GetStability() != FunctionStability::VOLATILE);
	});
	return repeatable;
}

static bool SampleIsRepeatable(const optional_ptr<SampleOptions> sample) {
	return !sample || sample->repeatable;
}

static LogicalOperatorRepeatability ClassifyOperatorType(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
	case LogicalOperatorType::LOGICAL_UNNEST:
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_DISTINCT:
	case LogicalOperatorType::LOGICAL_WINDOW:
	case LogicalOperatorType::LOGICAL_PIVOT:
	case LogicalOperatorType::LOGICAL_CHUNK_GET:
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
	case LogicalOperatorType::LOGICAL_CTE_REF:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT:
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
	case LogicalOperatorType::LOGICAL_SECURE_VIEW:
		return LogicalOperatorRepeatability::REPEATABLE;
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		if (!SampleIsRepeatable(get.extra_info.sample_options.get()) || get.dynamic_filters) {
			return LogicalOperatorRepeatability::NON_REPEATABLE;
		}
		if (get.GetTable()) {
			return LogicalOperatorRepeatability::REPEATABLE;
		}
		return get.function.stability == FunctionStability::VOLATILE ? LogicalOperatorRepeatability::NON_REPEATABLE
		                                                             : LogicalOperatorRepeatability::REPEATABLE;
	}
	case LogicalOperatorType::LOGICAL_SAMPLE: {
		auto &sample = op.Cast<LogicalSample>();
		return SampleIsRepeatable(sample.sample_options.get()) ? LogicalOperatorRepeatability::REPEATABLE
		                                                       : LogicalOperatorRepeatability::NON_REPEATABLE;
	}
	case LogicalOperatorType::LOGICAL_INVALID:
	case LogicalOperatorType::LOGICAL_LIMIT:
	case LogicalOperatorType::LOGICAL_TOP_N:
	case LogicalOperatorType::LOGICAL_COPY_TO_FILE:
	case LogicalOperatorType::LOGICAL_COPY_DATABASE:
	case LogicalOperatorType::LOGICAL_DELIM_GET:
	case LogicalOperatorType::LOGICAL_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN:
	case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN:
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
	case LogicalOperatorType::LOGICAL_INSERT:
	case LogicalOperatorType::LOGICAL_DELETE:
	case LogicalOperatorType::LOGICAL_UPDATE:
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
	case LogicalOperatorType::LOGICAL_TRIGGER:
	case LogicalOperatorType::LOGICAL_ALTER:
	case LogicalOperatorType::LOGICAL_CREATE_TABLE:
	case LogicalOperatorType::LOGICAL_CREATE_INDEX:
	case LogicalOperatorType::LOGICAL_CREATE_SEQUENCE:
	case LogicalOperatorType::LOGICAL_CREATE_VIEW:
	case LogicalOperatorType::LOGICAL_CREATE_SCHEMA:
	case LogicalOperatorType::LOGICAL_CREATE_MACRO:
	case LogicalOperatorType::LOGICAL_DROP:
	case LogicalOperatorType::LOGICAL_PRAGMA:
	case LogicalOperatorType::LOGICAL_TRANSACTION:
	case LogicalOperatorType::LOGICAL_CREATE_TYPE:
	case LogicalOperatorType::LOGICAL_ATTACH:
	case LogicalOperatorType::LOGICAL_DETACH:
	case LogicalOperatorType::LOGICAL_CREATE_TRIGGER:
	case LogicalOperatorType::LOGICAL_EXPLAIN:
	case LogicalOperatorType::LOGICAL_PREPARE:
	case LogicalOperatorType::LOGICAL_EXECUTE:
	case LogicalOperatorType::LOGICAL_EXPORT:
	case LogicalOperatorType::LOGICAL_VACUUM:
	case LogicalOperatorType::LOGICAL_SET:
	case LogicalOperatorType::LOGICAL_LOAD:
	case LogicalOperatorType::LOGICAL_RESET:
	case LogicalOperatorType::LOGICAL_UPDATE_EXTENSIONS:
	case LogicalOperatorType::LOGICAL_CONNECT:
	case LogicalOperatorType::LOGICAL_DISCONNECT:
	case LogicalOperatorType::LOGICAL_EXTERNAL_RESOURCE:
	case LogicalOperatorType::LOGICAL_CREATE_SECRET:
	case LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR:
		return LogicalOperatorRepeatability::NON_REPEATABLE;
	}
	return LogicalOperatorRepeatability::UNKNOWN;
}

LogicalOperatorRepeatability ClassifyLogicalOperatorRepeatability(LogicalOperator &op) {
	if (op.HasSideEffects()) {
		return LogicalOperatorRepeatability::NON_REPEATABLE;
	}
	auto classification = ClassifyOperatorType(op);
	if (classification != LogicalOperatorRepeatability::REPEATABLE) {
		return classification;
	}
	bool expressions_repeatable = true;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expression) {
		expressions_repeatable = expressions_repeatable && ExpressionIsRepeatable(*expression);
	});
	return expressions_repeatable ? LogicalOperatorRepeatability::REPEATABLE
	                              : LogicalOperatorRepeatability::NON_REPEATABLE;
}

bool LogicalSubtreeIsRepeatable(LogicalOperator &op) {
	if (ClassifyLogicalOperatorRepeatability(op) != LogicalOperatorRepeatability::REPEATABLE) {
		return false;
	}
	for (auto &child : op.children) {
		if (!LogicalSubtreeIsRepeatable(*child)) {
			return false;
		}
	}
	return true;
}

} // namespace duckdb
