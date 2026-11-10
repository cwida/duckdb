//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/cte_join_filter_collector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class LogicalOperator;
class LogicalCTERef;

//! A deterministic expression over a single output column of a materialized CTE
struct CTEJoinFilterKey {
	//! Expression containing exactly one column reference
	unique_ptr<Expression> expression;
	//! The CTE output column the expression references
	ProjectionIndex column;

	CTEJoinFilterKey Copy() const;
	//! Copy of the expression with its column reference rebound
	unique_ptr<Expression> BindExpression(const LogicalType &type, ColumnBinding binding) const;
};

struct CTEJoinFilter {
	//! Materialized CTE providing the key domain, unless source_parent is set
	TableIndex source;
	//! Operator whose child subtree can be copied to provide the key domain instead
	optional_ptr<LogicalOperator> source_parent;
	idx_t source_child = 0;
	TableIndex target;
	TableIndex target_scan;
	//! Key columns are CTE output columns, or output positions of the source subtree
	vector<CTEJoinFilterKey> source_keys;
	vector<CTEJoinFilterKey> target_keys;
	vector<ExpressionType> comparisons;
};

class CTEJoinFilterCollector {
public:
	static vector<CTEJoinFilter> Collect(LogicalOperator &op, const unordered_map<TableIndex, TableIndex> &targets);

	//! A filter key tracked while traversing the plan, bound to the keys' current position
	struct CTEJoinKey {
		//! Expression containing exactly one column reference
		unique_ptr<Expression> expression;
		//! Binding of that column reference at the current operator
		ColumnBinding binding;
	};

	//! The key domain of a join side: a materialized CTE ref, or a copyable child subtree
	struct CTEJoinSource {
		optional_ptr<const LogicalCTERef> ref;
		optional_ptr<LogicalOperator> parent;
		idx_t child_index = 0;
	};

private:
	explicit CTEJoinFilterCollector(const unordered_map<TableIndex, TableIndex> &targets);
	void VisitOperator(LogicalOperator &op);
	void PushFilter(LogicalOperator &op, const CTEJoinSource &source, const vector<CTEJoinKey> &source_keys,
	                vector<CTEJoinKey> target_keys, const vector<ExpressionType> &comparisons);
	void AddFilter(const CTEJoinSource &source, const LogicalCTERef &target, const vector<CTEJoinKey> &source_keys,
	               const vector<CTEJoinKey> &target_keys, const vector<ExpressionType> &comparisons);

private:
	const unordered_map<TableIndex, TableIndex> &join_targets;
	vector<CTEJoinFilter> join_filters;
};

} // namespace duckdb
