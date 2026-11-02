//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/filter_combiner.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/planner/expression.hpp"

#include "duckdb/storage/data_table.hpp"
#include <functional>
#include <map>

namespace duckdb {
class LogicalGet;
class Optimizer;

enum class ValueComparisonResult { PRUNE_LEFT, PRUNE_RIGHT, UNSATISFIABLE_CONDITION, PRUNE_NOTHING };
enum class FilterResult { UNSATISFIABLE, SUCCESS, UNSUPPORTED };
enum class FilterPushdownResult { NO_PUSHDOWN, PUSHED_DOWN_PARTIALLY, PUSHED_DOWN_FULLY };

//! The FilterCombiner combines several filters and generates a logically equivalent set that is more efficient
//! Amongst others:
//! (1) it prunes obsolete filter conditions: i.e. [X > 5 and X > 7] => [X > 7]
//! (2) it generates new filters for expressions in the same equivalence set: i.e. [X = Y and X = 500] => [Y = 500]
//! (3) it prunes branches that have unsatisfiable filters: i.e. [X = 5 AND X > 6] => FALSE, prune branch
class FilterCombiner {
public:
	explicit FilterCombiner(ClientContext &context);
	explicit FilterCombiner(Optimizer &optimizer);

	ClientContext &context;

public:
	struct ExpressionValueInformation {
		Value constant;
		ExpressionType comparison_type;
	};

	FilterResult AddFilter(unique_ptr<Expression> expr);
	//! Splits on AND and adds each conjunct
	FilterResult AddConjuncts(unique_ptr<Expression> expr);

	//! Returns whether or not a set of integral values is a dense range (i.e. 1, 2, 3, 4, 5)
	//! If this returns true - this sorts "in_list" as a side-effect
	static bool IsDenseRange(vector<Value> &in_list);
	static bool ContainsNull(vector<Value> &in_list);

	void GenerateFilters(const std::function<void(unique_ptr<Expression> filter)> &callback);
	bool HasFilters();
	void GenerateEquivalentFilters(const Expression &filter,
	                               const std::function<void(unique_ptr<Expression> filter)> &callback);
	TableFilterSet GenerateTableScanFilters(const vector<ColumnIndex> &column_ids,
	                                        vector<FilterPushdownResult> &pushdown_results);

	FilterPushdownResult TryPushdownGenericExpression(LogicalGet &get, Expression &expr);

private:
	FilterResult AddFilter(Expression &expr);
	FilterResult AddBoundComparisonFilter(Expression &expr);
	FilterResult AddTransitiveFilters(BoundFunctionExpression &comparison, bool is_root = true);
	unique_ptr<Expression> FindTransitiveFilter(const Expression &expr);
	Expression &GetNode(Expression &expr);
	idx_t GetEquivalenceSet(Expression &expr);
	FilterResult AddConstantComparison(vector<ExpressionValueInformation> &info_list, ExpressionValueInformation info);

	FilterPushdownResult TryPushdownConstantFilter(TableFilterSet &table_filters, const vector<ColumnIndex> &column_ids,
	                                               column_t column_id, vector<ExpressionValueInformation> &info_list);
	FilterPushdownResult TryPushdownExpression(TableFilterSet &table_filters, const vector<ColumnIndex> &column_ids,
	                                           Expression &expr);
	FilterPushdownResult TryPushdownPrefixFilter(TableFilterSet &table_filters, const vector<ColumnIndex> &column_ids,
	                                             Expression &expr);
	FilterPushdownResult TryPushdownLikeFilter(TableFilterSet &table_filters, const vector<ColumnIndex> &column_ids,
	                                           Expression &expr);
	FilterPushdownResult TryPushdownInFilter(TableFilterSet &table_filters, const vector<ColumnIndex> &column_ids,
	                                         Expression &expr);
	FilterPushdownResult TryPushdownOrClause(TableFilterSet &table_filters, const vector<ColumnIndex> &column_ids,
	                                         Expression &expr);
	FilterPushdownResult TryPushdownTemporalCastFilter(TableFilterSet &table_filters,
	                                                   const vector<ColumnIndex> &column_ids, Expression &expr);
	void TryPushdownRelaxedFilter(TableFilterSet &table_filters, const vector<ColumnIndex> &column_ids,
	                              vector<FilterPushdownResult> &pushdown_results, column_t expr_id,
	                              vector<ExpressionValueInformation> &info_list);

private:
	vector<unique_ptr<Expression>> remaining_filters;

	expression_map_t<unique_ptr<Expression>> stored_expressions;
	expression_map_t<idx_t> equivalence_set_map;
	map<idx_t, vector<ExpressionValueInformation>> constant_values;
	map<idx_t, vector<reference<Expression>>> equivalence_map;
	idx_t set_index = 0;
};

} // namespace duckdb
