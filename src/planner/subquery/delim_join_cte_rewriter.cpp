//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/subquery/delim_join_cte_rewriter.cpp
//
//
//===----------------------------------------------------------------------===//

#include "duckdb/planner/subquery/delim_join_cte_rewriter.hpp"

#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/planner/expression_nullability.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/list.hpp"

#include <algorithm>

namespace duckdb {

static constexpr const char *CTE_DELIMINATOR_PROFILER_KEY = "optimizer.deliminator";

static void VerifyNoDelim(LogicalOperator &op) {
	// Verify that there are no delim joins or delim scans in the plan, as these should have been rewritten to CTEs at
	// this point.
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		throw InternalException("Found DELIM_JOIN after flattening dependent joins");
	}
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		throw InternalException("Found DELIM_GET after flattening dependent joins");
	}
	for (auto &child : op.children) {
		VerifyNoDelim(*child);
	}
}

static vector<Identifier> GenerateCTEColumnNames(idx_t column_count, const string &prefix) {
	vector<Identifier> result;
	result.reserve(column_count);
	for (idx_t i = 0; i < column_count; i++) {
		result.push_back(Identifier(prefix + to_string(i)));
	}
	return result;
}

static BindingReplacementGraph CreateConstructedBindingReplacements(const vector<ColumnBinding> &old_bindings,
                                                                    const vector<ColumnBinding> &new_bindings) {
	D_ASSERT(old_bindings.size() == new_bindings.size());
	BindingReplacementGraph result;
	for (idx_t i = 0; i < old_bindings.size(); i++) {
		if (old_bindings[i] != new_bindings[i]) {
			result.Add(old_bindings[i], new_bindings[i]);
		}
	}
	return result;
}

static unique_ptr<LogicalOperator> CreateIdentityProjection(Binder &binder, unique_ptr<LogicalOperator> child,
                                                            BindingReplacementGraph &replacements) {
	auto projection = LogicalProjection::CreateIdentity(binder.GenerateTableIndex(), std::move(child));
	projection->ResolveOperatorTypes();
	auto bindings = projection->children[0]->GetColumnBindings();
	replacements = CreateConstructedBindingReplacements(bindings, projection->GetColumnBindings());
	return std::move(projection);
}

static idx_t RewriteDelimScanReferences(unique_ptr<LogicalOperator> &op, TableIndex delim_scan_index) {
	if (op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		if (!op->children.empty()) {
			return RewriteDelimScanReferences(op->children[0], delim_scan_index);
		}
		return 0;
	}
	idx_t rewritten_count = 0;
	for (auto &child : op->children) {
		rewritten_count += RewriteDelimScanReferences(child, delim_scan_index);
	}
	if (op->type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		auto &delim_get = op->Cast<LogicalDelimGet>();
		auto delim_scan_names = GenerateCTEColumnNames(delim_get.chunk_types.size(), "__duckdb_delim_scan_");
		auto cte_scan =
		    make_uniq<LogicalCTERef>(delim_get.table_index, delim_scan_index, delim_get.chunk_types, delim_scan_names);
		op = std::move(cte_scan);
		rewritten_count++;
	}
	return rewritten_count;
}

static optional_idx FindBindingIndex(const vector<ColumnBinding> &bindings, const ColumnBinding &binding) {
	auto entry = std::find(bindings.begin(), bindings.end(), binding);
	if (entry == bindings.end()) {
		return optional_idx();
	}
	return NumericCast<idx_t>(entry - bindings.begin());
}

static void AddFilterToOperator(unique_ptr<LogicalOperator> &child, unique_ptr<Expression> filter) {
	if (child->type == LogicalOperatorType::LOGICAL_FILTER && !child->HasProjectionMap()) {
		child->Cast<LogicalFilter>().expressions.push_back(std::move(filter));
		return;
	}

	auto new_filter = make_uniq<LogicalFilter>();
	new_filter->expressions.push_back(std::move(filter));
	new_filter->children.push_back(std::move(child));
	child = std::move(new_filter);
}

static bool GetExpressionColumnBindings(Expression &expr, column_binding_set_t &bindings) {
	bool depth_zero = true;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		if (colref.Depth() == 0) {
			bindings.insert(colref.Binding());
		} else {
			depth_zero = false;
		}
	});
	return depth_zero;
}

static bool ChildContainsBindings(LogicalOperator &child, const column_binding_set_t &bindings) {
	column_binding_set_t child_bindings;
	for (auto &binding : child.GetColumnBindings()) {
		child_bindings.insert(binding);
	}
	for (auto &binding : bindings) {
		if (child_bindings.find(binding) == child_bindings.end()) {
			return false;
		}
	}
	return true;
}

static bool FilterReferencesDelimInput(LogicalComparisonJoin &delim_join, Expression &filter) {
	D_ASSERT(delim_join.type == LogicalOperatorType::LOGICAL_DELIM_JOIN);
	column_binding_set_t filter_bindings;
	if (!GetExpressionColumnBindings(filter, filter_bindings)) {
		return false;
	}
	if (filter_bindings.empty()) {
		return false;
	}
	return ChildContainsBindings(*delim_join.children[0], filter_bindings);
}

static bool ExpressionReferencesChild(Expression &expr, LogicalOperator &child) {
	column_binding_set_t expr_bindings;
	if (!GetExpressionColumnBindings(expr, expr_bindings)) {
		return false;
	}
	if (expr_bindings.empty()) {
		return false;
	}
	column_binding_set_t child_bindings;
	for (auto &binding : child.GetColumnBindings()) {
		child_bindings.insert(binding);
	}
	for (auto &binding : expr_bindings) {
		if (child_bindings.find(binding) != child_bindings.end()) {
			return true;
		}
	}
	return false;
}

static bool ExpressionNullRejectsDelimJoinRHS(Expression &expr, LogicalComparisonJoin &delim_join) {
	auto &rhs = *delim_join.children[1];
	if (!ExpressionReferencesChild(expr, rhs)) {
		return false;
	}
	if (BoundComparisonExpression::IsComparison(expr)) {
		return expr.PropagatesNullValues();
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR &&
	    expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) {
		auto rhs_output = rhs.GetColumnBindings();
		column_binding_set_t rhs_bindings(rhs_output.begin(), rhs_output.end());
		auto column_becomes_null = [&](const BoundColumnRefExpression &column) {
			return column.Depth() == 0 && rhs_bindings.find(column.Binding()) != rhs_bindings.end();
		};
		bool null_propagating_child = false;
		ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) {
			null_propagating_child |= ExpressionBecomesNull(child, column_becomes_null);
		});
		return null_propagating_child;
	}
	return false;
}

static bool FilterNullRejectsDelimJoinRHS(LogicalFilter &filter, LogicalComparisonJoin &delim_join) {
	if (filter.HasProjectionMap() || delim_join.join_type != JoinType::SINGLE) {
		return false;
	}
	for (auto &expr : filter.expressions) {
		if (ExpressionNullRejectsDelimJoinRHS(*expr, delim_join)) {
			return true;
		}
	}
	return false;
}

static bool ExpressionIsMarkerRef(Expression &expr, TableIndex mark_index) {
	if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	auto &colref = expr.Cast<BoundColumnRefExpression>();
	return colref.Depth() == 0 && colref.Binding().table_index == mark_index;
}

static bool FilterNegatesDelimJoinMarker(LogicalFilter &filter, LogicalComparisonJoin &delim_join) {
	if (filter.HasProjectionMap() || delim_join.join_type != JoinType::MARK) {
		return false;
	}
	for (auto &expr : filter.expressions) {
		if (expr->GetExpressionType() != ExpressionType::OPERATOR_NOT) {
			continue;
		}
		auto &op = expr->Cast<BoundOperatorExpression>();
		if (!op.GetChildren().empty() && ExpressionIsMarkerRef(*op.GetChildren()[0], delim_join.mark_index)) {
			return true;
		}
	}
	return false;
}

static void CollectDelimJoinMarkerIndexes(LogicalOperator &op, set<TableIndex> &marker_indexes) {
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.join_type == JoinType::MARK) {
			marker_indexes.insert(join.mark_index);
		}
	}
	for (auto &child : op.children) {
		CollectDelimJoinMarkerIndexes(*child, marker_indexes);
	}
}

struct MarkerBindingInfo {
	set<TableIndex> marker_indexes;
	bool simple = false;
};

using marker_binding_map_t = column_binding_map_t<MarkerBindingInfo>;

static void CollectExpressionMarkerReferences(Expression &expr, const set<TableIndex> &marker_indexes,
                                              set<TableIndex> &referenced_markers) {
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		if (colref.Depth() == 0 && marker_indexes.find(colref.Binding().table_index) != marker_indexes.end()) {
			referenced_markers.insert(colref.Binding().table_index);
		}
	});
}

static marker_binding_map_t CreateMarkerBindingMap(const set<TableIndex> &marker_indexes) {
	marker_binding_map_t result;
	for (auto marker_index : marker_indexes) {
		MarkerBindingInfo info;
		info.marker_indexes.insert(marker_index);
		info.simple = true;
		result.emplace(ColumnBinding(marker_index, ProjectionIndex(0)), std::move(info));
	}
	return result;
}

static void CollectExpressionMarkerReferences(Expression &expr, const marker_binding_map_t &marker_bindings,
                                              set<TableIndex> &referenced_markers) {
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		if (colref.Depth() != 0) {
			return;
		}
		auto entry = marker_bindings.find(colref.Binding());
		if (entry == marker_bindings.end()) {
			return;
		}
		referenced_markers.insert(entry->second.marker_indexes.begin(), entry->second.marker_indexes.end());
	});
}

static bool ExpressionIsSimpleMarkerBinding(Expression &expr, const marker_binding_map_t &marker_bindings) {
	if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	auto &colref = expr.Cast<BoundColumnRefExpression>();
	if (colref.Depth() != 0) {
		return false;
	}
	auto entry = marker_bindings.find(colref.Binding());
	return entry != marker_bindings.end() && entry->second.simple;
}

static bool ExpressionIsSimpleMarkerReference(Expression &expr, const marker_binding_map_t &marker_bindings) {
	if (ExpressionIsSimpleMarkerBinding(expr, marker_bindings)) {
		return true;
	}
	if (expr.GetExpressionType() != ExpressionType::OPERATOR_NOT) {
		return false;
	}
	auto &not_expr = expr.Cast<BoundOperatorExpression>();
	return not_expr.GetChildren().size() == 1 &&
	       ExpressionIsSimpleMarkerBinding(*not_expr.GetChildren()[0], marker_bindings);
}

static MarkerBindingInfo GetMarkerBindingInfo(Expression &expr, const marker_binding_map_t &marker_bindings) {
	MarkerBindingInfo result;
	CollectExpressionMarkerReferences(expr, marker_bindings, result.marker_indexes);
	result.simple = !result.marker_indexes.empty() && ExpressionIsSimpleMarkerReference(expr, marker_bindings);
	return result;
}

static void AddProjectionMarkerBindings(LogicalProjection &projection, marker_binding_map_t &marker_bindings) {
	auto projection_bindings = projection.GetColumnBindings();
	D_ASSERT(projection_bindings.size() == projection.expressions.size());
	for (idx_t expr_idx = 0; expr_idx < projection.expressions.size(); expr_idx++) {
		auto info = GetMarkerBindingInfo(*projection.expressions[expr_idx], marker_bindings);
		if (info.marker_indexes.empty()) {
			continue;
		}
		marker_bindings.emplace(projection_bindings[expr_idx], std::move(info));
	}
}

static void CollectMarkerReferences(LogicalOperator &op, marker_binding_map_t &marker_bindings,
                                    set<TableIndex> &preserve_marker_indexes, set<TableIndex> &simple_marker_indexes) {
	for (auto &child : op.children) {
		CollectMarkerReferences(*child, marker_bindings, preserve_marker_indexes, simple_marker_indexes);
	}
	vector<reference<Expression>> expressions;
	LogicalOperatorVisitor::EnumerateExpressions(
	    op, [&](unique_ptr<Expression> *expr) { expressions.emplace_back(**expr); });
	set<TableIndex> simple_filter_markers;
	bool has_complex_marker_reference = false;
	for (auto &expr : expressions) {
		auto info = GetMarkerBindingInfo(expr.get(), marker_bindings);
		if (info.marker_indexes.empty()) {
			continue;
		}
		if (!info.simple) {
			preserve_marker_indexes.insert(info.marker_indexes.begin(), info.marker_indexes.end());
			has_complex_marker_reference = true;
		} else if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
			simple_filter_markers.insert(info.marker_indexes.begin(), info.marker_indexes.end());
		} else {
			simple_marker_indexes.insert(info.marker_indexes.begin(), info.marker_indexes.end());
		}
	}
	if (has_complex_marker_reference) {
		simple_marker_indexes.insert(simple_filter_markers.begin(), simple_filter_markers.end());
	}
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		AddProjectionMarkerBindings(op.Cast<LogicalProjection>(), marker_bindings);
	}
}

static bool RewriteFilterThroughProjections(unique_ptr<Expression> &expr,
                                            const vector<reference<LogicalProjection>> &projections) {
	for (auto &projection : projections) {
		bool can_push = true;
		ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
		    expr, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &) {
			    if (colref.Depth() != 0) {
				    can_push = false;
				    return;
			    }
			    auto &projected = projection.get().GetExpression(colref.Binding());
			    if (projected.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				    can_push = false;
				    return;
			    }
			    auto &input = projected.Cast<BoundColumnRefExpression>();
			    if (input.Depth() != 0) {
				    can_push = false;
				    return;
			    }
			    colref.BindingMutable() = input.Binding();
		    });
		if (!can_push) {
			return false;
		}
	}
	return true;
}

static bool PushEligibleFilterExpressionsIntoDelimJoinInputs(unique_ptr<LogicalOperator> &plan) {
	auto &filter = plan->Cast<LogicalFilter>();
	if (filter.HasProjectionMap()) {
		return false;
	}
	reference<LogicalOperator> input = *filter.children[0];
	vector<reference<LogicalProjection>> projections;
	while (input.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = input.get().Cast<LogicalProjection>();
		projections.push_back(projection);
		input = *projection.children[0];
	}
	if (input.get().type != LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		return false;
	}

	bool changed = false;
	auto &delim_join = input.get().Cast<LogicalComparisonJoin>();
	if (!projections.empty() && PropagatesBuildSide(delim_join.join_type)) {
		return false;
	}
	for (auto &projection : projections) {
		for (auto &expr : projection.get().expressions) {
			if (ExpressionBarrier::Required(*expr) || ExpressionBarrier::Contains(*expr)) {
				return false;
			}
		}
	}
	vector<unique_ptr<Expression>> remaining_expressions;
	auto expressions = std::move(filter.expressions);
	LogicalFilter::SplitPredicates(expressions);
	for (auto &expr : expressions) {
		if (!projections.empty()) {
			if (!ExpressionBarrier::Required(*expr) && !ExpressionBarrier::Contains(*expr)) {
				auto rewritten = expr->Copy();
				if (RewriteFilterThroughProjections(rewritten, projections) &&
				    FilterReferencesDelimInput(delim_join, *rewritten)) {
					AddFilterToOperator(delim_join.children[0], std::move(rewritten));
					changed = true;
					continue;
				}
			}
			remaining_expressions.push_back(std::move(expr));
			continue;
		}
		if (FilterReferencesDelimInput(delim_join, *expr)) {
			AddFilterToOperator(delim_join.children[0], std::move(expr));
			changed = true;
			continue;
		}
		remaining_expressions.push_back(std::move(expr));
	}

	if (remaining_expressions.empty()) {
		plan = std::move(filter.children[0]);
	} else {
		filter.expressions = std::move(remaining_expressions);
	}
	return changed;
}

static bool PushEligibleFiltersIntoDelimJoinInputs(unique_ptr<LogicalOperator> &plan) {
	bool changed = false;
	for (auto &child : plan->children) {
		changed = PushEligibleFiltersIntoDelimJoinInputs(child) || changed;
	}
	if (plan->type == LogicalOperatorType::LOGICAL_FILTER) {
		changed = PushEligibleFilterExpressionsIntoDelimJoinInputs(plan) || changed;
	}
	return changed;
}

static bool IsColumnEqualityPredicate(Expression &expr) {
	if (!BoundComparisonExpression::IsComparison(expr)) {
		return false;
	}
	switch (expr.GetExpressionType()) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		break;
	default:
		return false;
	}
	auto &comparison = expr.Cast<BoundFunctionExpression>();
	auto &lhs = BoundComparisonExpression::Left(comparison);
	auto &rhs = BoundComparisonExpression::Right(comparison);
	if (lhs.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF ||
	    rhs.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	return lhs.Cast<BoundColumnRefExpression>().Depth() == 0 && rhs.Cast<BoundColumnRefExpression>().Depth() == 0;
}

static bool IsNonSelectiveJoinPredicate(Expression &expr) {
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		bool all_children_non_selective = true;
		ExpressionIterator::EnumerateChildren(
		    expr, [&](Expression &child) { all_children_non_selective &= IsNonSelectiveJoinPredicate(child); });
		return all_children_non_selective;
	}
	return IsColumnEqualityPredicate(expr);
}

static bool IsSubqueryEvidenceSide(const LogicalOperator &op, idx_t child_idx) {
	if (op.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN && op.type != LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		return false;
	}
	auto &join = op.Cast<LogicalComparisonJoin>();
	switch (join.join_type) {
	case JoinType::MARK:
	case JoinType::SEMI:
	case JoinType::ANTI:
		return child_idx == 1;
	case JoinType::RIGHT_SEMI:
	case JoinType::RIGHT_ANTI:
		return child_idx == 0;
	default:
		return false;
	}
}

static void ReplaceExpressionBindings(unique_ptr<Expression> &expr, const vector<ColumnBinding> &bindings,
                                      const vector<unique_ptr<Expression>> &expressions);

static vector<unique_ptr<Expression>>
TraceCorrelatedColumnsIntoChild(const LogicalOperator &op, idx_t child_idx,
                                const vector<unique_ptr<Expression>> &correlated_columns) {
	vector<unique_ptr<Expression>> result;
	result.reserve(correlated_columns.size());
	for (auto &column : correlated_columns) {
		result.push_back(column->Copy());
	}
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION && child_idx == 0) {
		auto &projection = op.Cast<LogicalProjection>();
		auto projection_bindings =
		    LogicalOperator::GenerateColumnBindings(projection.table_index, projection.expressions.size());
		for (auto &column : result) {
			ReplaceExpressionBindings(column, projection_bindings, projection.expressions);
		}
	} else if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY && child_idx == 0) {
		auto &aggregate = op.Cast<LogicalAggregate>();
		auto group_bindings = LogicalOperator::GenerateColumnBindings(aggregate.group_index, aggregate.groups.size());
		for (auto &column : result) {
			ReplaceExpressionBindings(column, group_bindings, aggregate.groups);
		}
	}
	return result;
}

static bool CorrelatedColumnsReferenceOnlyChild(const vector<unique_ptr<Expression>> &correlated_columns,
                                                LogicalOperator &child) {
	if (correlated_columns.empty()) {
		return false;
	}
	for (auto &column : correlated_columns) {
		column_binding_set_t bindings;
		if (!GetExpressionColumnBindings(*column, bindings) || bindings.empty() ||
		    !ChildContainsBindings(child, bindings)) {
			return false;
		}
	}
	return true;
}

static bool IsUnrelatedNullableJoinSide(const LogicalOperator &op, idx_t child_idx,
                                        const vector<unique_ptr<Expression>> &correlated_columns) {
	if (op.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN && op.type != LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		return false;
	}
	auto &join = op.Cast<LogicalComparisonJoin>();
	if ((join.join_type == JoinType::LEFT || join.join_type == JoinType::SINGLE) && child_idx == 1) {
		return CorrelatedColumnsReferenceOnlyChild(correlated_columns, *op.children[0]);
	}
	if (join.join_type == JoinType::RIGHT && child_idx == 0) {
		return CorrelatedColumnsReferenceOnlyChild(correlated_columns, *op.children[1]);
	}
	return false;
}

static bool HasSelection(const LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		for (const auto &entry : get.table_filters) {
			auto &expr_filter =
			    ExpressionFilter::GetExpressionFilter(entry.Filter(), "DelimJoinCTERewriter::HasSelection");
			auto &expr = *expr_filter.expr;
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_OPERATOR ||
			    expr.GetExpressionType() != ExpressionType::OPERATOR_IS_NOT_NULL) {
				return true;
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_FILTER: {
		auto &filter = op.Cast<LogicalFilter>();
		for (auto &expr : filter.expressions) {
			if (!IsNonSelectiveJoinPredicate(*expr)) {
				return true;
			}
		}
		break;
	}
	default:
		break;
	}

	for (auto &child : op.children) {
		if (HasSelection(*child)) {
			return true;
		}
	}
	return false;
}

static bool HasDelimiterDomainSelection(const LogicalOperator &op, const set<TableIndex> &marker_indexes,
                                        bool ignore_marker_filters,
                                        const vector<unique_ptr<Expression>> &correlated_columns) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		for (const auto &entry : get.table_filters) {
			auto &expr_filter = ExpressionFilter::GetExpressionFilter(
			    entry.Filter(), "DelimJoinCTERewriter::HasDelimiterDomainSelection");
			auto &expr = *expr_filter.expr;
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_OPERATOR ||
			    expr.GetExpressionType() != ExpressionType::OPERATOR_IS_NOT_NULL) {
				return true;
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_FILTER: {
		auto &filter = op.Cast<LogicalFilter>();
		for (auto &expr : filter.expressions) {
			if (ignore_marker_filters) {
				set<TableIndex> referenced_markers;
				CollectExpressionMarkerReferences(*expr, marker_indexes, referenced_markers);
				if (!referenced_markers.empty()) {
					continue;
				}
			}
			if (!IsNonSelectiveJoinPredicate(*expr)) {
				return true;
			}
		}
		break;
	}
	default:
		break;
	}
	for (idx_t child_idx = 0; child_idx < op.children.size(); child_idx++) {
		if (IsSubqueryEvidenceSide(op, child_idx) || IsUnrelatedNullableJoinSide(op, child_idx, correlated_columns)) {
			continue;
		}
		auto child_correlated_columns = TraceCorrelatedColumnsIntoChild(op, child_idx, correlated_columns);
		if (HasDelimiterDomainSelection(*op.children[child_idx], marker_indexes, ignore_marker_filters,
		                                child_correlated_columns)) {
			return true;
		}
	}
	return false;
}

static bool IsEvidenceSide(LogicalOperator &op, idx_t child_idx) {
	if (op.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &join = op.Cast<LogicalComparisonJoin>();
	switch (join.join_type) {
	case JoinType::MARK:
	case JoinType::ANTI:
		return child_idx == 1;
	case JoinType::RIGHT_ANTI:
		return child_idx == 0;
	default:
		return false;
	}
}

static bool HasEvidenceSide(JoinType join_type) {
	switch (join_type) {
	case JoinType::MARK:
	case JoinType::ANTI:
	case JoinType::RIGHT_ANTI:
		return true;
	default:
		return false;
	}
}

static bool ContainsSubqueryJoin(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN || op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		switch (join.join_type) {
		case JoinType::MARK:
		case JoinType::SEMI:
		case JoinType::ANTI:
		case JoinType::RIGHT_SEMI:
		case JoinType::RIGHT_ANTI:
		case JoinType::SINGLE:
			return true;
		default:
			break;
		}
	}
	for (auto &child : op.children) {
		if (ContainsSubqueryJoin(*child)) {
			return true;
		}
	}
	return false;
}

static bool NestedDelimJoinUsesAdditionalCorrelation(const LogicalOperator &op,
                                                     const vector<unique_ptr<Expression>> &correlated_columns) {
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		for (auto &nested_column : join.duplicate_eliminated_columns) {
			bool found = false;
			for (auto &column : correlated_columns) {
				if (Expression::Equals(*nested_column, *column)) {
					found = true;
					break;
				}
			}
			if (!found) {
				return true;
			}
		}
	}
	for (idx_t child_idx = 0; child_idx < op.children.size(); child_idx++) {
		if (IsSubqueryEvidenceSide(op, child_idx) || IsUnrelatedNullableJoinSide(op, child_idx, correlated_columns)) {
			continue;
		}
		auto child_correlated_columns = TraceCorrelatedColumnsIntoChild(op, child_idx, correlated_columns);
		if (NestedDelimJoinUsesAdditionalCorrelation(*op.children[child_idx], child_correlated_columns)) {
			return true;
		}
	}
	return false;
}

struct GeneratedDedupRef {
	optional_ptr<LogicalCTERef> cte_ref;
	vector<ColumnBinding> output_bindings;
	vector<unique_ptr<Expression>> output_expressions;
	vector<unique_ptr<Expression>> filters;
	bool has_projection = false;
};

static bool IsEqualityComparison(ExpressionType comparison_type) {
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return true;
	default:
		return false;
	}
}

static bool IsEqualityJoinCondition(const JoinCondition &cond) {
	return cond.IsComparison() && IsEqualityComparison(cond.GetComparisonType());
}

static bool HasSingleEqualityJoinCondition(const LogicalComparisonJoin &join) {
	if (join.conditions.size() != 1) {
		return false;
	}
	auto &condition = join.conditions[0];
	return condition.IsComparison() && (condition.GetComparisonType() == ExpressionType::COMPARE_EQUAL ||
	                                    condition.GetComparisonType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM);
}

static void CollectEligibleSimpleMarkerIndexes(LogicalOperator &op, const set<TableIndex> &simple_marker_indexes,
                                               set<TableIndex> &preserve_marker_indexes) {
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.join_type == JoinType::MARK &&
		    simple_marker_indexes.find(join.mark_index) != simple_marker_indexes.end() &&
		    HasSingleEqualityJoinCondition(join)) {
			preserve_marker_indexes.insert(join.mark_index);
		}
	}
	for (auto &child : op.children) {
		CollectEligibleSimpleMarkerIndexes(*child, simple_marker_indexes, preserve_marker_indexes);
	}
}

static bool IsTautologicalNotDistinctComparison(const Expression &expr) {
	if (expr.GetExpressionType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM ||
	    !BoundComparisonExpression::IsComparison(expr)) {
		return false;
	}
	auto &comparison = expr.Cast<BoundFunctionExpression>();
	auto &lhs = BoundComparisonExpression::Left(comparison);
	auto &rhs = BoundComparisonExpression::Right(comparison);
	if (lhs.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF ||
	    rhs.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	auto &lhs_colref = lhs.Cast<BoundColumnRefExpression>();
	auto &rhs_colref = rhs.Cast<BoundColumnRefExpression>();
	return lhs_colref.Depth() == rhs_colref.Depth() && lhs_colref.Binding() == rhs_colref.Binding();
}

static bool FindAndReplaceBindings(vector<ColumnBinding> &traced_bindings,
                                   const vector<unique_ptr<Expression>> &expressions,
                                   const vector<ColumnBinding> &current_bindings) {
	for (auto &binding : traced_bindings) {
		idx_t current_idx;
		for (current_idx = 0; current_idx < expressions.size(); current_idx++) {
			if (binding == current_bindings[current_idx]) {
				break;
			}
		}

		if (current_idx == expressions.size() ||
		    expressions[current_idx]->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return false;
		}

		auto &colref = expressions[current_idx]->Cast<BoundColumnRefExpression>();
		binding = colref.Binding();
	}
	return true;
}

class ExpressionBindingReplacer : public LogicalOperatorVisitor {
public:
	ExpressionBindingReplacer(const vector<ColumnBinding> &bindings, const vector<unique_ptr<Expression>> &expressions)
	    : bindings(bindings), expressions(expressions) {
		D_ASSERT(bindings.size() == expressions.size());
	}

	ExpressionBindingReplacer(const vector<ColumnBinding> &bindings, const vector<unique_ptr<Expression>> &expressions,
	                          optional_ptr<LogicalOperator> stop_operator)
	    : bindings(bindings), expressions(expressions), stop_operator(stop_operator) {
		D_ASSERT(bindings.size() == expressions.size());
	}

	void VisitOperator(LogicalOperator &op) override {
		if (stop_operator && stop_operator.get() == &op) {
			return;
		}
		VisitOperatorChildren(op);
		VisitOperatorExpressions(op);
	}

	unique_ptr<Expression> VisitReplace(BoundColumnRefExpression &expr, unique_ptr<Expression> *expr_ptr) override {
		if (expr.Depth() != 0) {
			return nullptr;
		}
		for (idx_t idx = 0; idx < bindings.size(); idx++) {
			if (expr.Binding() == bindings[idx]) {
				return expressions[idx]->Copy();
			}
		}
		return nullptr;
	}

private:
	const vector<ColumnBinding> &bindings;
	const vector<unique_ptr<Expression>> &expressions;
	optional_ptr<LogicalOperator> stop_operator;
};

static void ReplaceExpressionBindings(unique_ptr<Expression> &expr, const vector<ColumnBinding> &bindings,
                                      const vector<unique_ptr<Expression>> &expressions) {
	if (bindings.empty()) {
		return;
	}
	ExpressionBindingReplacer replacer(bindings, expressions);
	replacer.VisitExpression(&expr);
}

static bool GetBoundColumnRefBinding(const Expression &expr, ColumnBinding &binding) {
	if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	auto &colref = expr.Cast<BoundColumnRefExpression>();
	if (colref.Depth() != 0) {
		return false;
	}
	binding = colref.Binding();
	return true;
}

static void MergeBindingExpressionReplacements(BindingReplacementGraph &replacements,
                                               const vector<ColumnBinding> &bindings,
                                               const vector<unique_ptr<Expression>> &expressions) {
	D_ASSERT(bindings.size() == expressions.size());
	for (idx_t i = 0; i < bindings.size(); i++) {
		ColumnBinding target;
		if (GetBoundColumnRefBinding(*expressions[i], target)) {
			replacements.Add(ReplacementBinding(bindings[i], target, expressions[i]->GetReturnType()));
		}
	}
}

static bool ExpressionReferencesBinding(Expression &expr, const vector<ColumnBinding> &bindings) {
	bool found = false;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		if (colref.Depth() == 0 && FindBindingIndex(bindings, colref.Binding()).IsValid()) {
			found = true;
		}
	});
	return found;
}

static bool CoversAllBindings(const vector<ColumnBinding> &all_bindings,
                              const vector<ColumnBinding> &covered_bindings) {
	if (all_bindings.size() != covered_bindings.size()) {
		return false;
	}
	for (auto &binding : all_bindings) {
		idx_t match_count = 0;
		for (auto &covered_binding : covered_bindings) {
			if (binding == covered_binding) {
				match_count++;
			}
		}
		if (match_count != 1) {
			return false;
		}
	}
	return true;
}

static bool AddExpressionReplacement(vector<ColumnBinding> &bindings, vector<unique_ptr<Expression>> &expressions,
                                     ColumnBinding binding, unique_ptr<Expression> expression) {
	for (idx_t binding_idx = 0; binding_idx < bindings.size(); binding_idx++) {
		if (bindings[binding_idx] != binding) {
			continue;
		}
		return expressions[binding_idx]->Equals(*expression);
	}
	bindings.push_back(binding);
	expressions.push_back(std::move(expression));
	return true;
}

static void ReplaceOperatorBindings(LogicalOperator &op, const vector<ColumnBinding> &bindings,
                                    const vector<unique_ptr<Expression>> &expressions) {
	if (bindings.empty()) {
		return;
	}
	ExpressionBindingReplacer replacer(bindings, expressions);
	replacer.VisitOperator(op);
}

static void ReplaceOperatorBindings(LogicalOperator &op, const vector<ColumnBinding> &bindings,
                                    const vector<unique_ptr<Expression>> &expressions,
                                    optional_ptr<LogicalOperator> stop_operator) {
	if (bindings.empty()) {
		return;
	}
	ExpressionBindingReplacer replacer(bindings, expressions, stop_operator);
	replacer.VisitOperator(op);
}

class GeneratedDedupRefEliminator {
public:
	GeneratedDedupRefEliminator(ClientContext &context, unique_ptr<LogicalOperator> &delim_join_op,
	                            TableIndex dedup_cte_index, idx_t dedup_ref_count, LogicalOperator &rewrite_root,
	                            bool preserve_evidence_side, bool preserve_nested_evidence_side);

	idx_t Remove();

private:
	unique_ptr<GeneratedDedupRef> GetGeneratedDedupRef(LogicalOperator &op, bool collect_filters = false,
	                                                   bool allow_projection = false) const;
	bool ExpressionReferencesGeneratedDedupRef(const Expression &expr, const GeneratedDedupRef &dedup_ref) const;
	bool CoversAllDedupColumns(const GeneratedDedupRef &dedup_ref, const vector<ColumnBinding> &bindings) const;
	optional_idx FindGeneratedOutputBinding(const Expression &expr, const GeneratedDedupRef &dedup_ref) const;
	bool ExpressionReferencesGeneratedSide(const Expression &expr, const GeneratedDedupRef &dedup_ref) const;
	bool FilterIsGeneratedDedupCrossProduct(LogicalOperator &op) const;
	bool RewriteSubtree(unique_ptr<LogicalOperator> &op, bool preserve_selected_domain,
	                    BindingReplacementGraph &replacements, bool under_aggregate = false,
	                    bool under_evidence_side = false);
	idx_t CountGeneratedDedupRefs(LogicalOperator &op) const;
	bool RemoveInequalityJoinConditions(LogicalOperator &target_op, const vector<JoinCondition> &join_conditions,
	                                    idx_t dedup_idx);
	bool PreserveJoinAsSemi(unique_ptr<LogicalOperator> &join, BindingReplacementGraph &replacements);
	bool PreserveFilterCrossProductAsSemi(unique_ptr<LogicalOperator> &filter_op,
	                                      BindingReplacementGraph &replacements);
	bool RemoveJoin(unique_ptr<LogicalOperator> &join, BindingReplacementGraph &replacements);
	bool RemoveFilterCrossProduct(unique_ptr<LogicalOperator> &filter_op, BindingReplacementGraph &replacements);

private:
	ClientContext &context;
	unique_ptr<LogicalOperator> &delim_join_op;
	LogicalComparisonJoin &delim_join;
	TableIndex dedup_cte_index;
	idx_t dedup_ref_count;
	LogicalOperator &rewrite_root;
	bool preserve_evidence_side;
	bool preserve_nested_evidence_side;
};

GeneratedDedupRefEliminator::GeneratedDedupRefEliminator(ClientContext &context,
                                                         unique_ptr<LogicalOperator> &delim_join_op,
                                                         TableIndex dedup_cte_index, idx_t dedup_ref_count,
                                                         LogicalOperator &rewrite_root, bool preserve_evidence_side,
                                                         bool preserve_nested_evidence_side)
    : context(context), delim_join_op(delim_join_op), delim_join(delim_join_op->Cast<LogicalComparisonJoin>()),
      dedup_cte_index(dedup_cte_index), dedup_ref_count(dedup_ref_count), rewrite_root(rewrite_root),
      preserve_evidence_side(preserve_evidence_side), preserve_nested_evidence_side(preserve_nested_evidence_side) {
}

unique_ptr<GeneratedDedupRef> GeneratedDedupRefEliminator::GetGeneratedDedupRef(LogicalOperator &op,
                                                                                bool collect_filters,
                                                                                bool allow_projection) const {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cteref = op.Cast<LogicalCTERef>();
		if (cteref.cte_index == dedup_cte_index) {
			auto result = make_uniq<GeneratedDedupRef>();
			result->cte_ref = cteref;
			result->output_bindings = cteref.GetColumnBindings();
			result->output_expressions.reserve(result->output_bindings.size());
			for (idx_t col_idx = 0; col_idx < result->output_bindings.size(); col_idx++) {
				result->output_expressions.push_back(
				    make_uniq<BoundColumnRefExpression>(cteref.chunk_types[col_idx], result->output_bindings[col_idx]));
			}
			return result;
		}
		return nullptr;
	}
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &filter = op.Cast<LogicalFilter>();
		if (filter.HasProjectionMap() || filter.children.size() != 1) {
			return nullptr;
		}
		auto result = GetGeneratedDedupRef(*filter.children[0], collect_filters, allow_projection);
		if (!result) {
			return nullptr;
		}
		if (collect_filters) {
			for (auto &expr : filter.expressions) {
				auto filter_expr = expr->Copy();
				ReplaceExpressionBindings(filter_expr, result->output_bindings, result->output_expressions);
				result->filters.push_back(std::move(filter_expr));
			}
		}
		return result;
	}
	if (allow_projection && op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		if (projection.children.size() != 1) {
			return nullptr;
		}
		auto result = GetGeneratedDedupRef(*projection.children[0], collect_filters, allow_projection);
		if (!result) {
			return nullptr;
		}

		auto child_bindings = result->output_bindings;
		auto child_expressions = std::move(result->output_expressions);
		result->output_bindings = projection.GetColumnBindings();
		result->output_expressions.clear();
		result->output_expressions.reserve(projection.expressions.size());
		for (auto &expr : projection.expressions) {
			auto rewritten_expr = expr->Copy();
			ReplaceExpressionBindings(rewritten_expr, child_bindings, child_expressions);
			result->output_expressions.push_back(std::move(rewritten_expr));
		}
		result->has_projection = true;
		return result;
	}
	return nullptr;
}

bool GeneratedDedupRefEliminator::ExpressionReferencesGeneratedDedupRef(const Expression &expr,
                                                                        const GeneratedDedupRef &dedup_ref) const {
	bool found = false;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		if (colref.Depth() == 0 && colref.Binding().table_index == dedup_ref.cte_ref->table_index) {
			found = true;
		}
	});
	return found;
}

bool GeneratedDedupRefEliminator::CoversAllDedupColumns(const GeneratedDedupRef &dedup_ref,
                                                        const vector<ColumnBinding> &bindings) const {
	auto cte_bindings =
	    LogicalOperator::GenerateColumnBindings(dedup_ref.cte_ref->table_index, dedup_ref.cte_ref->chunk_types.size());
	if (bindings.size() != cte_bindings.size()) {
		return false;
	}
	for (auto &cte_binding : cte_bindings) {
		idx_t match_count = 0;
		for (auto &binding : bindings) {
			if (binding == cte_binding) {
				match_count++;
			}
		}
		if (match_count != 1) {
			return false;
		}
	}
	return true;
}

optional_idx GeneratedDedupRefEliminator::FindGeneratedOutputBinding(const Expression &expr,
                                                                     const GeneratedDedupRef &dedup_ref) const {
	ColumnBinding binding;
	if (!GetBoundColumnRefBinding(expr, binding)) {
		return optional_idx();
	}
	return FindBindingIndex(dedup_ref.output_bindings, binding);
}

bool GeneratedDedupRefEliminator::ExpressionReferencesGeneratedSide(const Expression &expr,
                                                                    const GeneratedDedupRef &dedup_ref) const {
	if (ExpressionReferencesGeneratedDedupRef(expr, dedup_ref)) {
		return true;
	}
	bool found = false;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		if (colref.Depth() != 0) {
			return;
		}
		if (FindBindingIndex(dedup_ref.output_bindings, colref.Binding()).IsValid()) {
			found = true;
		}
	});
	return found;
}

bool GeneratedDedupRefEliminator::FilterIsGeneratedDedupCrossProduct(LogicalOperator &op) const {
	if (op.type != LogicalOperatorType::LOGICAL_FILTER || op.HasProjectionMap()) {
		return false;
	}
	auto &filter = op.Cast<LogicalFilter>();
	if (filter.children.size() != 1 || filter.children[0]->type != LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		return false;
	}
	auto &cross_product = *filter.children[0];
	return cross_product.children.size() == 2 &&
	       (GetGeneratedDedupRef(*cross_product.children[0]) || GetGeneratedDedupRef(*cross_product.children[1]));
}

bool GeneratedDedupRefEliminator::RewriteSubtree(unique_ptr<LogicalOperator> &op, bool preserve_selected_domain,
                                                 BindingReplacementGraph &replacements, bool under_aggregate,
                                                 bool under_evidence_side) {
	if (op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		if (!op->children.empty()) {
			return RewriteSubtree(op->children[0], preserve_selected_domain, replacements, under_aggregate,
			                      under_evidence_side);
		}
		return false;
	}

	bool rewritten = false;
	auto child_under_aggregate = under_aggregate || op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY;
	for (idx_t child_idx = 0; child_idx < op->children.size(); child_idx++) {
		auto old_child_bindings = op->children[child_idx]->GetColumnBindings();
		BindingReplacementGraph child_replacements;
		if (RewriteSubtree(op->children[child_idx], preserve_selected_domain, child_replacements, child_under_aggregate,
		                   under_evidence_side)) {
			ColumnBindingRewrite::ApplyToChild(op, child_idx, std::move(old_child_bindings), child_replacements);
			replacements.Merge(child_replacements.ScopedToBoundary(op->children[child_idx]->GetColumnBindings()));
			rewritten = true;
		}
	}

	bool filter_cross_product = false;
	if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
	    (GetGeneratedDedupRef(*op->children[0], false, true) || GetGeneratedDedupRef(*op->children[1], false, true))) {
	} else if (FilterIsGeneratedDedupCrossProduct(*op)) {
		filter_cross_product = true;
	} else {
		return rewritten;
	}

	bool local_rewrite;
	BindingReplacementGraph local_replacements;
	if (preserve_selected_domain && (under_aggregate || under_evidence_side)) {
		// This join is a semijoin reduction for a grouped RHS or an existence-check evidence side. Removing it is
		// valid, but can turn a selective correlated subquery into a global aggregate or full evidence scan.
		local_rewrite = filter_cross_product ? PreserveFilterCrossProductAsSemi(op, local_replacements)
		                                     : PreserveJoinAsSemi(op, local_replacements);
	} else {
		local_rewrite = filter_cross_product ? RemoveFilterCrossProduct(op, local_replacements)
		                                     : RemoveJoin(op, local_replacements);
	}
	if (local_rewrite) {
		replacements.Merge(local_replacements);
	}
	return rewritten || local_rewrite;
}

idx_t GeneratedDedupRefEliminator::CountGeneratedDedupRefs(LogicalOperator &op) const {
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		if (!op.children.empty()) {
			return CountGeneratedDedupRefs(*op.children[0]);
		}
		return 0;
	}

	idx_t count = 0;
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF && op.Cast<LogicalCTERef>().cte_index == dedup_cte_index) {
		count++;
	}
	for (auto &child : op.children) {
		count += CountGeneratedDedupRefs(*child);
	}
	return count;
}

bool GeneratedDedupRefEliminator::RemoveInequalityJoinConditions(LogicalOperator &target_op,
                                                                 const vector<JoinCondition> &join_conditions,
                                                                 idx_t dedup_idx) {
	auto &delim_conditions = delim_join.conditions;
	if (dedup_ref_count != 1 || delim_conditions.size() != join_conditions.size()) {
		return false;
	}
	if (delim_join.join_type != JoinType::ANTI && delim_join.join_type != JoinType::MARK &&
	    delim_join.join_type != JoinType::SEMI && delim_join.join_type != JoinType::SINGLE) {
		return false;
	}

	if (delim_join.join_type == JoinType::SINGLE || delim_join.join_type == JoinType::MARK) {
		bool has_one_equality = false;
		for (auto &cond : join_conditions) {
			has_one_equality = has_one_equality || IsEqualityJoinCondition(cond);
		}
		if (!has_one_equality) {
			return false;
		}
	}
	NotNullExpressionAnalyzer nullability(context, rewrite_root);

	vector<ColumnBinding> traced_bindings;
	for (const auto &cond : delim_conditions) {
		if (!cond.IsComparison() || cond.GetRHS().GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return false;
		}
		auto &colref = cond.GetRHS().Cast<BoundColumnRefExpression>();
		traced_bindings.emplace_back(colref.Binding());
	}

	reference<LogicalOperator> current_op = *delim_join.children[1];
	while (&current_op.get() != &target_op) {
		if (current_op.get().children.size() != 1) {
			return false;
		}

		switch (current_op.get().type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
			if (!FindAndReplaceBindings(traced_bindings, current_op.get().expressions,
			                            current_op.get().GetColumnBindings())) {
				return false;
			}
			break;
		case LogicalOperatorType::LOGICAL_FILTER:
			break;
		default:
			return false;
		}
		current_op = *current_op.get().children[0];
	}

	vector<JoinCondition> rewritten_conditions;
	rewritten_conditions.reserve(delim_conditions.size());
	for (auto &condition : delim_conditions) {
		rewritten_conditions.push_back(condition.Copy());
	}

	bool found_all = true;
	for (idx_t cond_idx = 0; cond_idx < rewritten_conditions.size(); cond_idx++) {
		auto &delim_condition = rewritten_conditions[cond_idx];
		if (!delim_condition.IsComparison()) {
			continue;
		}
		const auto &traced_binding = traced_bindings[cond_idx];

		bool found = false;
		for (auto &join_condition : join_conditions) {
			if (!join_condition.IsComparison()) {
				continue;
			}
			auto &dedup_side = dedup_idx == 0 ? join_condition.GetLHS() : join_condition.GetRHS();
			if (dedup_side.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
				continue;
			}
			auto &colref = dedup_side.Cast<BoundColumnRefExpression>();
			if (colref.Binding() == traced_binding) {
				auto join_comparison = join_condition.GetComparisonType();
				auto original_join_comparison = join_condition.GetComparisonType();
				// DISTINCT FROM changes regular inequality semantics when the MARK probe key can be NULL.
				if (delim_join.join_type == JoinType::MARK &&
				    original_join_comparison == ExpressionType::COMPARE_NOTEQUAL) {
					if (!nullability.IsNotNull(*delim_join.children[0], delim_condition.GetLHS())) {
						return false;
					}
				}
				if (delim_condition.GetComparisonType() == ExpressionType::COMPARE_DISTINCT_FROM ||
				    delim_condition.GetComparisonType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
					if (join_comparison == ExpressionType::COMPARE_EQUAL) {
						join_comparison = ExpressionType::COMPARE_NOT_DISTINCT_FROM;
					} else if (join_comparison == ExpressionType::COMPARE_NOTEQUAL) {
						join_comparison = ExpressionType::COMPARE_DISTINCT_FROM;
					} else if (join_comparison != ExpressionType::COMPARE_DISTINCT_FROM &&
					           join_comparison != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
						found = false;
						break;
					}
				}
				auto left_copy = delim_condition.LeftReference()->Copy();
				auto right_copy = delim_condition.RightReference()->Copy();

				rewritten_conditions[cond_idx] = JoinCondition(std::move(left_copy), std::move(right_copy),
				                                               FlipComparisonExpression(join_comparison));
				if (delim_join.join_type != JoinType::MARK &&
				    original_join_comparison != ExpressionType::COMPARE_DISTINCT_FROM &&
				    original_join_comparison != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
					auto final_comparison = rewritten_conditions[cond_idx].GetComparisonType();
					if (final_comparison == ExpressionType::COMPARE_DISTINCT_FROM) {
						final_comparison = ExpressionType::COMPARE_NOTEQUAL;
					} else if (final_comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
						final_comparison = ExpressionType::COMPARE_EQUAL;
					}
					rewritten_conditions[cond_idx] =
					    JoinCondition(rewritten_conditions[cond_idx].LeftReference()->Copy(),
					                  rewritten_conditions[cond_idx].RightReference()->Copy(), final_comparison);
				}
				found = true;
				break;
			}
		}
		found_all = found_all && found;
	}

	if (!found_all) {
		return false;
	}
	delim_join.conditions = std::move(rewritten_conditions);
	return true;
}

bool GeneratedDedupRefEliminator::PreserveJoinAsSemi(unique_ptr<LogicalOperator> &join,
                                                     BindingReplacementGraph &replacements) {
	auto &comparison_join = join->Cast<LogicalComparisonJoin>();
	if (comparison_join.join_type != JoinType::INNER && comparison_join.join_type != JoinType::SEMI) {
		return false;
	}
	if (comparison_join.HasProjectionMap()) {
		return false;
	}

	auto left_is_generated = GetGeneratedDedupRef(*join->children[0], false, true) != nullptr;
	auto right_is_generated = GetGeneratedDedupRef(*join->children[1], false, true) != nullptr;
	if (left_is_generated == right_is_generated) {
		return false;
	}
	const idx_t dedup_idx = left_is_generated ? 0 : 1;
	if (comparison_join.join_type == JoinType::SEMI && dedup_idx == 0) {
		return false;
	}

	auto dedup_ref = GetGeneratedDedupRef(*join->children[dedup_idx], false, true);
	if (!dedup_ref) {
		return false;
	}

	BindingReplacementGraph replacement_bindings;
	vector<ColumnBinding> covered_dedup_bindings;
	covered_dedup_bindings.reserve(comparison_join.conditions.size());
	vector<ColumnBinding> base_replacement_bindings;
	vector<unique_ptr<Expression>> base_replacement_expressions;
	vector<JoinCondition> semi_conditions;
	semi_conditions.reserve(comparison_join.conditions.size());

	for (auto &cond : comparison_join.conditions) {
		if (!cond.IsComparison() || !IsEqualityJoinCondition(cond)) {
			return false;
		}

		auto lhs_generated_idx = FindGeneratedOutputBinding(cond.GetLHS(), *dedup_ref);
		auto rhs_generated_idx = FindGeneratedOutputBinding(cond.GetRHS(), *dedup_ref);
		if (lhs_generated_idx.IsValid() == rhs_generated_idx.IsValid()) {
			return false;
		}
		auto generated_idx = lhs_generated_idx.IsValid() ? lhs_generated_idx.GetIndex() : rhs_generated_idx.GetIndex();
		auto &generated_binding = dedup_ref->output_bindings[generated_idx];
		auto &generated_expression = *dedup_ref->output_expressions[generated_idx];
		auto &other_side = lhs_generated_idx.IsValid() ? cond.GetRHS() : cond.GetLHS();

		ColumnBinding other_binding;
		if (!GetBoundColumnRefBinding(other_side, other_binding)) {
			return false;
		}
		if (!replacement_bindings.TryAdd(
		        ReplacementBinding(generated_binding, other_binding, generated_expression.GetReturnType()))) {
			return false;
		}

		ColumnBinding base_binding;
		if (GetBoundColumnRefBinding(generated_expression, base_binding) &&
		    base_binding.table_index == dedup_ref->cte_ref->table_index) {
			if (!replacement_bindings.TryAdd(ReplacementBinding(base_binding, other_binding))) {
				return false;
			}
			covered_dedup_bindings.emplace_back(base_binding);
			base_replacement_bindings.push_back(base_binding);
			base_replacement_expressions.push_back(other_side.Copy());
		}

		auto generated_expr = lhs_generated_idx.IsValid() ? cond.GetLHS().Copy() : cond.GetRHS().Copy();
		auto other_expr = lhs_generated_idx.IsValid() ? cond.GetRHS().Copy() : cond.GetLHS().Copy();
		auto comparison_type =
		    lhs_generated_idx.IsValid() ? FlipComparisonExpression(cond.GetComparisonType()) : cond.GetComparisonType();
		semi_conditions.emplace_back(std::move(other_expr), std::move(generated_expr), comparison_type);
	}
	if (!CoversAllDedupColumns(*dedup_ref, covered_dedup_bindings)) {
		return false;
	}

	vector<unique_ptr<Expression>> generated_output_replacements;
	generated_output_replacements.reserve(dedup_ref->output_expressions.size());
	for (auto &expr : dedup_ref->output_expressions) {
		auto rewritten_expr = expr->Copy();
		ReplaceExpressionBindings(rewritten_expr, base_replacement_bindings, base_replacement_expressions);
		if (ExpressionReferencesGeneratedSide(*rewritten_expr, *dedup_ref)) {
			return false;
		}
		generated_output_replacements.push_back(std::move(rewritten_expr));
	}

	if (dedup_idx == 0) {
		std::swap(comparison_join.children[0], comparison_join.children[1]);
	}
	comparison_join.join_type = JoinType::SEMI;
	comparison_join.conditions = std::move(semi_conditions);
	comparison_join.left_projection_map.clear();
	comparison_join.right_projection_map.clear();
	comparison_join.ResolveOperatorTypes();

	ColumnBindingReplacer replacer;
	replacement_bindings.AddTo(replacer);
	replacer.BuildIndex();
	replacer.stop_operator = join.get();
	replacer.VisitOperator(rewrite_root);
	ReplaceOperatorBindings(rewrite_root, dedup_ref->output_bindings, generated_output_replacements, join.get());
	replacements.Merge(replacement_bindings);
	MergeBindingExpressionReplacements(replacements, dedup_ref->output_bindings, generated_output_replacements);
	return true;
}

bool GeneratedDedupRefEliminator::PreserveFilterCrossProductAsSemi(unique_ptr<LogicalOperator> &filter_op,
                                                                   BindingReplacementGraph &replacements) {
	auto &filter = filter_op->Cast<LogicalFilter>();
	if (filter.HasProjectionMap() || filter.children.size() != 1 ||
	    filter.children[0]->type != LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		return false;
	}
	auto &cross_product = *filter.children[0];
	if (cross_product.children.size() != 2) {
		return false;
	}

	const idx_t dedup_idx = GetGeneratedDedupRef(*cross_product.children[0], false, true) ? 0 : 1;
	auto dedup_ref = GetGeneratedDedupRef(*cross_product.children[dedup_idx], false, true);
	if (!dedup_ref) {
		return false;
	}

	filter.SplitPredicates();
	vector<bool> consumed(filter.expressions.size(), false);
	vector<ColumnBinding> covered_dedup_bindings;
	covered_dedup_bindings.reserve(dedup_ref->output_bindings.size());
	vector<ColumnBinding> base_replacement_bindings;
	vector<unique_ptr<Expression>> base_replacement_expressions;
	vector<JoinCondition> semi_conditions;
	BindingReplacementGraph replacement_bindings;

	for (idx_t expr_idx = 0; expr_idx < filter.expressions.size(); expr_idx++) {
		auto &expr = *filter.expressions[expr_idx];
		if (!BoundComparisonExpression::IsComparison(expr) || !IsEqualityComparison(expr.GetExpressionType())) {
			continue;
		}
		auto &comparison = expr.Cast<BoundFunctionExpression>();
		auto &lhs = BoundComparisonExpression::Left(comparison);
		auto &rhs = BoundComparisonExpression::Right(comparison);

		auto lhs_generated_idx = FindGeneratedOutputBinding(lhs, *dedup_ref);
		auto rhs_generated_idx = FindGeneratedOutputBinding(rhs, *dedup_ref);
		if (lhs_generated_idx.IsValid() == rhs_generated_idx.IsValid()) {
			continue;
		}

		auto generated_idx = lhs_generated_idx.IsValid() ? lhs_generated_idx.GetIndex() : rhs_generated_idx.GetIndex();
		auto &generated_binding = dedup_ref->output_bindings[generated_idx];
		auto &generated_expression = *dedup_ref->output_expressions[generated_idx];
		auto &other_side = lhs_generated_idx.IsValid() ? rhs : lhs;

		ColumnBinding other_binding;
		if (!GetBoundColumnRefBinding(other_side, other_binding)) {
			return false;
		}
		if (!replacement_bindings.TryAdd(
		        ReplacementBinding(generated_binding, other_binding, generated_expression.GetReturnType()))) {
			return false;
		}

		ColumnBinding base_binding;
		if (GetBoundColumnRefBinding(generated_expression, base_binding) &&
		    base_binding.table_index == dedup_ref->cte_ref->table_index) {
			if (!replacement_bindings.TryAdd(ReplacementBinding(base_binding, other_binding))) {
				return false;
			}
			covered_dedup_bindings.emplace_back(base_binding);
			base_replacement_bindings.push_back(base_binding);
			base_replacement_expressions.push_back(other_side.Copy());
		}

		auto generated_expr = lhs_generated_idx.IsValid() ? lhs.Copy() : rhs.Copy();
		auto other_expr = lhs_generated_idx.IsValid() ? rhs.Copy() : lhs.Copy();
		auto comparison_type =
		    lhs_generated_idx.IsValid() ? FlipComparisonExpression(expr.GetExpressionType()) : expr.GetExpressionType();
		semi_conditions.emplace_back(std::move(other_expr), std::move(generated_expr), comparison_type);
		consumed[expr_idx] = true;
	}

	if (semi_conditions.empty() || !CoversAllDedupColumns(*dedup_ref, covered_dedup_bindings)) {
		return false;
	}
	for (idx_t expr_idx = 0; expr_idx < filter.expressions.size(); expr_idx++) {
		if (!consumed[expr_idx] && ExpressionReferencesGeneratedSide(*filter.expressions[expr_idx], *dedup_ref)) {
			return false;
		}
	}

	vector<unique_ptr<Expression>> generated_output_replacements;
	generated_output_replacements.reserve(dedup_ref->output_expressions.size());
	for (auto &expr : dedup_ref->output_expressions) {
		auto rewritten_expr = expr->Copy();
		ReplaceExpressionBindings(rewritten_expr, base_replacement_bindings, base_replacement_expressions);
		if (ExpressionReferencesGeneratedSide(*rewritten_expr, *dedup_ref)) {
			return false;
		}
		generated_output_replacements.push_back(std::move(rewritten_expr));
	}

	auto semi_join = make_uniq<LogicalComparisonJoin>(JoinType::SEMI);
	semi_join->conditions = std::move(semi_conditions);
	semi_join->children.push_back(std::move(cross_product.children[1 - dedup_idx]));
	semi_join->children.push_back(std::move(cross_product.children[dedup_idx]));
	semi_join->ResolveOperatorTypes();

	unique_ptr<LogicalOperator> replacement_op = std::move(semi_join);
	for (idx_t expr_idx = 0; expr_idx < filter.expressions.size(); expr_idx++) {
		if (consumed[expr_idx]) {
			continue;
		}
		AddFilterToOperator(replacement_op, std::move(filter.expressions[expr_idx]));
	}

	filter_op = std::move(replacement_op);
	ColumnBindingReplacer replacer;
	replacement_bindings.AddTo(replacer);
	replacer.BuildIndex();
	replacer.stop_operator = filter_op.get();
	replacer.VisitOperator(rewrite_root);
	ReplaceOperatorBindings(rewrite_root, dedup_ref->output_bindings, generated_output_replacements, filter_op.get());
	replacements.Merge(replacement_bindings);
	MergeBindingExpressionReplacements(replacements, dedup_ref->output_bindings, generated_output_replacements);
	return true;
}

bool GeneratedDedupRefEliminator::RemoveJoin(unique_ptr<LogicalOperator> &join, BindingReplacementGraph &replacements) {
	auto &comparison_join = join->Cast<LogicalComparisonJoin>();
	if (comparison_join.join_type != JoinType::INNER && comparison_join.join_type != JoinType::SEMI) {
		return false;
	}

	auto left_is_generated = GetGeneratedDedupRef(*join->children[0], false, true) != nullptr;
	auto right_is_generated = GetGeneratedDedupRef(*join->children[1], false, true) != nullptr;
	if (left_is_generated == right_is_generated) {
		return false;
	}
	const idx_t dedup_idx = left_is_generated ? 0 : 1;

	auto dedup_ref = GetGeneratedDedupRef(*join->children[dedup_idx], true, true);
	if (!dedup_ref) {
		return false;
	}

	BindingReplacementGraph replacement_bindings;
	bool all_equality_conditions = true;
	vector<ColumnBinding> covered_dedup_bindings;
	covered_dedup_bindings.reserve(comparison_join.conditions.size());
	vector<ColumnBinding> base_replacement_bindings;
	vector<unique_ptr<Expression>> base_replacement_expressions;
	vector<unique_ptr<Expression>> join_filter_expressions;
	vector<unique_ptr<Expression>> not_null_filter_expressions;

	for (auto &cond : comparison_join.conditions) {
		if (!cond.IsComparison()) {
			return false;
		}
		all_equality_conditions = all_equality_conditions && IsEqualityJoinCondition(cond);

		auto lhs_generated_idx = FindGeneratedOutputBinding(cond.GetLHS(), *dedup_ref);
		auto rhs_generated_idx = FindGeneratedOutputBinding(cond.GetRHS(), *dedup_ref);
		if (lhs_generated_idx.IsValid() == rhs_generated_idx.IsValid()) {
			return false;
		}
		auto generated_idx = lhs_generated_idx.IsValid() ? lhs_generated_idx.GetIndex() : rhs_generated_idx.GetIndex();
		auto &generated_binding = dedup_ref->output_bindings[generated_idx];
		auto &generated_expression = *dedup_ref->output_expressions[generated_idx];
		auto &other_side = lhs_generated_idx.IsValid() ? cond.GetRHS() : cond.GetLHS();

		ColumnBinding other_binding;
		if (!GetBoundColumnRefBinding(other_side, other_binding)) {
			return false;
		}
		if (!replacement_bindings.TryAdd(
		        ReplacementBinding(generated_binding, other_binding, generated_expression.GetReturnType()))) {
			return false;
		}

		ColumnBinding base_binding;
		if (GetBoundColumnRefBinding(generated_expression, base_binding) &&
		    base_binding.table_index == dedup_ref->cte_ref->table_index) {
			if (!replacement_bindings.TryAdd(ReplacementBinding(base_binding, other_binding))) {
				return false;
			}
			covered_dedup_bindings.emplace_back(base_binding);
			base_replacement_bindings.push_back(base_binding);
			base_replacement_expressions.push_back(other_side.Copy());
		}

		join_filter_expressions.push_back(
		    BoundComparisonExpression::Create(cond.GetComparisonType(), cond.GetLHS().Copy(), cond.GetRHS().Copy()));
		if (cond.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM &&
		    cond.GetComparisonType() != ExpressionType::COMPARE_DISTINCT_FROM) {
			auto is_not_null_expr =
			    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
			is_not_null_expr->GetChildrenMutable().push_back(other_side.Copy());
			not_null_filter_expressions.push_back(std::move(is_not_null_expr));
		}
	}
	if (!CoversAllDedupColumns(*dedup_ref, covered_dedup_bindings)) {
		return false;
	}

	vector<unique_ptr<Expression>> generated_output_replacements;
	generated_output_replacements.reserve(dedup_ref->output_expressions.size());
	for (auto &expr : dedup_ref->output_expressions) {
		auto rewritten_expr = expr->Copy();
		ReplaceExpressionBindings(rewritten_expr, base_replacement_bindings, base_replacement_expressions);
		if (ExpressionReferencesGeneratedSide(*rewritten_expr, *dedup_ref)) {
			return false;
		}
		generated_output_replacements.push_back(std::move(rewritten_expr));
	}

	vector<unique_ptr<Expression>> filter_expressions;
	if (all_equality_conditions) {
		for (auto &expr : dedup_ref->filters) {
			ReplaceExpressionBindings(expr, base_replacement_bindings, base_replacement_expressions);
			if (ExpressionReferencesGeneratedSide(*expr, *dedup_ref)) {
				return false;
			}
			filter_expressions.push_back(std::move(expr));
		}
		for (auto &expr : join_filter_expressions) {
			ReplaceExpressionBindings(expr, dedup_ref->output_bindings, generated_output_replacements);
			ReplaceExpressionBindings(expr, base_replacement_bindings, base_replacement_expressions);
			if (ExpressionReferencesGeneratedSide(*expr, *dedup_ref)) {
				return false;
			}
			if (IsTautologicalNotDistinctComparison(*expr)) {
				continue;
			}
			filter_expressions.push_back(std::move(expr));
		}
	} else {
		if (dedup_ref->has_projection ||
		    !RemoveInequalityJoinConditions(*join, comparison_join.conditions, dedup_idx)) {
			return false;
		}
		filter_expressions = std::move(dedup_ref->filters);
		for (auto &expr : not_null_filter_expressions) {
			filter_expressions.push_back(std::move(expr));
		}
	}

	unique_ptr<LogicalOperator> replacement_op = std::move(comparison_join.children[1 - dedup_idx]);
	if (!filter_expressions.empty()) {
		auto new_filter = make_uniq<LogicalFilter>();
		new_filter->expressions = std::move(filter_expressions);
		new_filter->children.emplace_back(std::move(replacement_op));
		replacement_op = std::move(new_filter);
	}

	join = std::move(replacement_op);

	ColumnBindingReplacer replacer;
	replacement_bindings.AddTo(replacer);
	replacer.BuildIndex();
	replacer.VisitOperator(rewrite_root);
	ReplaceOperatorBindings(rewrite_root, dedup_ref->output_bindings, generated_output_replacements);
	replacements.Merge(replacement_bindings);
	MergeBindingExpressionReplacements(replacements, dedup_ref->output_bindings, generated_output_replacements);
	return true;
}

bool GeneratedDedupRefEliminator::RemoveFilterCrossProduct(unique_ptr<LogicalOperator> &filter_op,
                                                           BindingReplacementGraph &replacements) {
	auto &filter = filter_op->Cast<LogicalFilter>();
	D_ASSERT(filter.children.size() == 1);
	auto &cross_product = *filter.children[0];
	D_ASSERT(cross_product.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT);

	const idx_t dedup_idx = GetGeneratedDedupRef(*cross_product.children[0]) ? 0 : 1;
	auto dedup_ref = GetGeneratedDedupRef(*cross_product.children[dedup_idx], true);
	if (!dedup_ref) {
		return false;
	}

	vector<unique_ptr<Expression>> generated_filter_expressions = std::move(dedup_ref->filters);

	filter.SplitPredicates();
	vector<bool> consumed(filter.expressions.size(), false);
	vector<JoinCondition> join_conditions;
	BindingReplacementGraph replacement_bindings;
	bool all_equality_conditions = true;
	vector<ColumnBinding> covered_dedup_bindings;

	for (idx_t expr_idx = 0; expr_idx < filter.expressions.size(); expr_idx++) {
		auto &expr = *filter.expressions[expr_idx];
		if (!BoundComparisonExpression::IsComparison(expr)) {
			continue;
		}
		auto &comparison = expr.Cast<BoundFunctionExpression>();
		auto &lhs = BoundComparisonExpression::Left(comparison);
		auto &rhs = BoundComparisonExpression::Right(comparison);
		if (lhs.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF ||
		    rhs.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			continue;
		}

		auto &lhs_colref = lhs.Cast<BoundColumnRefExpression>();
		auto &rhs_colref = rhs.Cast<BoundColumnRefExpression>();
		auto lhs_dedup = lhs_colref.Binding().table_index == dedup_ref->cte_ref->table_index;
		auto rhs_dedup = rhs_colref.Binding().table_index == dedup_ref->cte_ref->table_index;
		if (lhs_dedup == rhs_dedup) {
			continue;
		}

		auto comparison_type = expr.GetExpressionType();
		if (lhs_dedup) {
			if (!replacement_bindings.TryAdd(ReplacementBinding(lhs_colref.Binding(), rhs_colref.Binding()))) {
				return false;
			}
			covered_dedup_bindings.emplace_back(lhs_colref.Binding());
			if (dedup_idx == 0) {
				join_conditions.emplace_back(lhs.Copy(), rhs.Copy(), comparison_type);
			} else {
				join_conditions.emplace_back(rhs.Copy(), lhs.Copy(), FlipComparisonExpression(comparison_type));
			}
			if (comparison_type != ExpressionType::COMPARE_NOT_DISTINCT_FROM &&
			    comparison_type != ExpressionType::COMPARE_DISTINCT_FROM) {
				auto is_not_null_expr =
				    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
				is_not_null_expr->GetChildrenMutable().push_back(rhs.Copy());
				generated_filter_expressions.push_back(std::move(is_not_null_expr));
			}
		} else {
			if (!replacement_bindings.TryAdd(ReplacementBinding(rhs_colref.Binding(), lhs_colref.Binding()))) {
				return false;
			}
			covered_dedup_bindings.emplace_back(rhs_colref.Binding());
			if (dedup_idx == 0) {
				join_conditions.emplace_back(rhs.Copy(), lhs.Copy(), FlipComparisonExpression(comparison_type));
			} else {
				join_conditions.emplace_back(lhs.Copy(), rhs.Copy(), comparison_type);
			}
			if (comparison_type != ExpressionType::COMPARE_NOT_DISTINCT_FROM &&
			    comparison_type != ExpressionType::COMPARE_DISTINCT_FROM) {
				auto is_not_null_expr =
				    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
				is_not_null_expr->GetChildrenMutable().push_back(lhs.Copy());
				generated_filter_expressions.push_back(std::move(is_not_null_expr));
			}
		}
		all_equality_conditions = all_equality_conditions && IsEqualityJoinCondition(join_conditions.back());
		consumed[expr_idx] = true;
	}

	if (join_conditions.size() != dedup_ref->cte_ref->chunk_types.size() ||
	    !CoversAllDedupColumns(*dedup_ref, covered_dedup_bindings)) {
		return false;
	}
	for (idx_t expr_idx = 0; expr_idx < filter.expressions.size(); expr_idx++) {
		if (!consumed[expr_idx] && ExpressionReferencesGeneratedDedupRef(*filter.expressions[expr_idx], *dedup_ref)) {
			return false;
		}
	}

	if (!all_equality_conditions && !RemoveInequalityJoinConditions(*filter_op, join_conditions, dedup_idx)) {
		return false;
	}

	unique_ptr<LogicalOperator> replacement_op = std::move(cross_product.children[1 - dedup_idx]);
	for (idx_t expr_idx = 0; expr_idx < filter.expressions.size(); expr_idx++) {
		if (!consumed[expr_idx]) {
			generated_filter_expressions.push_back(std::move(filter.expressions[expr_idx]));
		}
	}
	if (!generated_filter_expressions.empty()) {
		auto new_filter = make_uniq<LogicalFilter>();
		new_filter->expressions = std::move(generated_filter_expressions);
		new_filter->children.emplace_back(std::move(replacement_op));
		replacement_op = std::move(new_filter);
	}

	filter_op = std::move(replacement_op);

	ColumnBindingReplacer replacer;
	replacement_bindings.AddTo(replacer);
	replacer.BuildIndex();
	replacer.VisitOperator(rewrite_root);
	replacements.Merge(replacement_bindings);
	return true;
}

idx_t GeneratedDedupRefEliminator::Remove() {
	set<TableIndex> marker_indexes;
	auto preserve_selected_domain = HasDelimiterDomainSelection(*delim_join.children[0], marker_indexes, false,
	                                                            delim_join.duplicate_eliminated_columns);
	auto selected_evidence_side = preserve_selected_domain && preserve_evidence_side &&
	                              HasEvidenceSide(delim_join.join_type) &&
	                              (!ContainsSubqueryJoin(*delim_join.children[0]) ||
	                               (preserve_nested_evidence_side && HasSingleEqualityJoinCondition(delim_join)));
	auto old_right_bindings = delim_join.children[1]->GetColumnBindings();
	BindingReplacementGraph right_replacements;
	if (RewriteSubtree(delim_join.children[1], preserve_selected_domain, right_replacements, false,
	                   selected_evidence_side)) {
		ColumnBindingRewrite::ApplyToChild(delim_join_op, 1, std::move(old_right_bindings), right_replacements);
	}
	dedup_ref_count = CountGeneratedDedupRefs(*delim_join_op->children[1]);
	return dedup_ref_count;
}

struct GeneratedDomainRef {
	optional_ptr<LogicalCTERef> cte_ref;
	vector<ColumnBinding> source_bindings;
	vector<ColumnBinding> output_bindings;
	vector<unique_ptr<Expression>> output_expressions;
	vector<unique_ptr<Expression>> filters;
	bool has_selection = false;
};

class GeneratedDomainJoinEliminator {
public:
	GeneratedDomainJoinEliminator(unique_ptr<LogicalOperator> &rewrite_root,
	                              const vector<TableIndex> &generated_dedup_cte_indexes,
	                              const set<TableIndex> &preserve_marker_indexes);

	bool Rewrite();

private:
	void CollectCTEs(LogicalOperator &op);
	optional_ptr<LogicalCTE> FindCTE(TableIndex cte_index) const;
	bool TryRewriteOnce(unique_ptr<LogicalOperator> &op, BindingReplacementGraph &replacements,
	                    bool under_aggregate = false, bool under_evidence_side = false,
	                    bool negated_marker_filter_above = false);

	unique_ptr<GeneratedDedupRef> GetGeneratedDedupRef(LogicalOperator &op, bool collect_filters = false,
	                                                   bool allow_projection = false) const;
	unique_ptr<GeneratedDomainRef> GetGeneratedDomainDefinition(LogicalOperator &op) const;
	unique_ptr<GeneratedDomainRef> GetGeneratedDomainRef(LogicalOperator &op, bool collect_filters = false,
	                                                     bool allow_projection = false) const;
	bool CTEHasSelection(TableIndex cte_index, vector<TableIndex> &seen_ctes) const;
	bool OperatorHasSelection(LogicalOperator &op, vector<TableIndex> &seen_ctes) const;
	bool GeneratedDedupRefHasSelection(const GeneratedDedupRef &dedup_ref) const;

	optional_idx FindOutputBinding(Expression &expr, const vector<ColumnBinding> &bindings) const;
	bool ContainsRecursiveCTERef(LogicalOperator &op) const;

	bool RemoveGeneratedDedupJoin(unique_ptr<LogicalOperator> &join, BindingReplacementGraph &replacements,
	                              bool under_aggregate, bool under_evidence_side);
	bool RemoveGeneratedDomainJoin(unique_ptr<LogicalOperator> &join, BindingReplacementGraph &replacements,
	                               bool under_aggregate, bool under_evidence_side);

private:
	unique_ptr<LogicalOperator> &rewrite_root;
	const vector<TableIndex> &generated_dedup_cte_indexes;
	const set<TableIndex> &preserve_marker_indexes;
	vector<reference<LogicalCTE>> ctes;
};

GeneratedDomainJoinEliminator::GeneratedDomainJoinEliminator(unique_ptr<LogicalOperator> &rewrite_root,
                                                             const vector<TableIndex> &generated_dedup_cte_indexes,
                                                             const set<TableIndex> &preserve_marker_indexes)
    : rewrite_root(rewrite_root), generated_dedup_cte_indexes(generated_dedup_cte_indexes),
      preserve_marker_indexes(preserve_marker_indexes) {
}

void GeneratedDomainJoinEliminator::CollectCTEs(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE ||
	    op.type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		ctes.push_back(op.Cast<LogicalCTE>());
	}
	for (auto &child : op.children) {
		CollectCTEs(*child);
	}
}

optional_ptr<LogicalCTE> GeneratedDomainJoinEliminator::FindCTE(TableIndex cte_index) const {
	for (auto &cte : ctes) {
		if (cte.get().table_index == cte_index) {
			return cte.get();
		}
	}
	return nullptr;
}

bool GeneratedDomainJoinEliminator::CTEHasSelection(TableIndex cte_index, vector<TableIndex> &seen_ctes) const {
	if (std::find(seen_ctes.begin(), seen_ctes.end(), cte_index) != seen_ctes.end()) {
		return false;
	}
	seen_ctes.push_back(cte_index);
	auto cte = FindCTE(cte_index);
	if (!cte || cte->children.empty()) {
		return false;
	}
	return OperatorHasSelection(*cte->children[0], seen_ctes);
}

bool GeneratedDomainJoinEliminator::OperatorHasSelection(LogicalOperator &op, vector<TableIndex> &seen_ctes) const {
	if (HasSelection(op)) {
		return true;
	}
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		return CTEHasSelection(op.Cast<LogicalCTERef>().cte_index, seen_ctes);
	}
	for (auto &child : op.children) {
		if (OperatorHasSelection(*child, seen_ctes)) {
			return true;
		}
	}
	return false;
}

bool GeneratedDomainJoinEliminator::GeneratedDedupRefHasSelection(const GeneratedDedupRef &dedup_ref) const {
	if (!dedup_ref.cte_ref) {
		return false;
	}
	vector<TableIndex> seen_ctes;
	return CTEHasSelection(dedup_ref.cte_ref->cte_index, seen_ctes);
}

unique_ptr<GeneratedDedupRef> GeneratedDomainJoinEliminator::GetGeneratedDedupRef(LogicalOperator &op,
                                                                                  bool collect_filters,
                                                                                  bool allow_projection) const {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cteref = op.Cast<LogicalCTERef>();
		if (std::find(generated_dedup_cte_indexes.begin(), generated_dedup_cte_indexes.end(), cteref.cte_index) ==
		    generated_dedup_cte_indexes.end()) {
			return nullptr;
		}

		auto result = make_uniq<GeneratedDedupRef>();
		result->cte_ref = cteref;
		result->output_bindings = cteref.GetColumnBindings();
		result->output_expressions.reserve(result->output_bindings.size());
		for (idx_t col_idx = 0; col_idx < result->output_bindings.size(); col_idx++) {
			result->output_expressions.push_back(
			    make_uniq<BoundColumnRefExpression>(cteref.chunk_types[col_idx], result->output_bindings[col_idx]));
		}
		return result;
	}
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &filter = op.Cast<LogicalFilter>();
		if (filter.HasProjectionMap() || filter.children.size() != 1) {
			return nullptr;
		}
		auto result = GetGeneratedDedupRef(*filter.children[0], collect_filters, allow_projection);
		if (!result) {
			return nullptr;
		}
		if (collect_filters) {
			for (auto &expr : filter.expressions) {
				auto filter_expr = expr->Copy();
				ReplaceExpressionBindings(filter_expr, result->output_bindings, result->output_expressions);
				result->filters.push_back(std::move(filter_expr));
			}
		}
		return result;
	}
	if (allow_projection && op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		if (projection.children.size() != 1) {
			return nullptr;
		}
		auto result = GetGeneratedDedupRef(*projection.children[0], collect_filters, allow_projection);
		if (!result) {
			return nullptr;
		}

		auto child_bindings = result->output_bindings;
		auto child_expressions = std::move(result->output_expressions);
		result->output_bindings = projection.GetColumnBindings();
		result->output_expressions.clear();
		result->output_expressions.reserve(projection.expressions.size());
		for (auto &expr : projection.expressions) {
			auto rewritten_expr = expr->Copy();
			ReplaceExpressionBindings(rewritten_expr, child_bindings, child_expressions);
			result->output_expressions.push_back(std::move(rewritten_expr));
		}
		result->has_projection = true;
		return result;
	}
	return nullptr;
}

unique_ptr<GeneratedDomainRef> GeneratedDomainJoinEliminator::GetGeneratedDomainDefinition(LogicalOperator &op) const {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cteref = op.Cast<LogicalCTERef>();
		if (std::find(generated_dedup_cte_indexes.begin(), generated_dedup_cte_indexes.end(), cteref.cte_index) ==
		    generated_dedup_cte_indexes.end()) {
			return nullptr;
		}

		auto result = make_uniq<GeneratedDomainRef>();
		result->source_bindings = cteref.GetColumnBindings();
		result->output_bindings = result->source_bindings;
		result->output_expressions.reserve(result->output_bindings.size());
		for (idx_t col_idx = 0; col_idx < result->output_bindings.size(); col_idx++) {
			result->output_expressions.push_back(
			    make_uniq<BoundColumnRefExpression>(cteref.chunk_types[col_idx], result->output_bindings[col_idx]));
		}
		return result;
	}
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &filter = op.Cast<LogicalFilter>();
		if (filter.HasProjectionMap() || filter.children.size() != 1) {
			return nullptr;
		}
		auto result = GetGeneratedDomainDefinition(*filter.children[0]);
		if (!result) {
			return nullptr;
		}
		for (auto &expr : filter.expressions) {
			auto filter_expr = expr->Copy();
			ReplaceExpressionBindings(filter_expr, result->output_bindings, result->output_expressions);
			result->filters.push_back(std::move(filter_expr));
		}
		return result;
	}
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		if (projection.children.size() != 1) {
			return nullptr;
		}
		auto result = GetGeneratedDomainDefinition(*projection.children[0]);
		if (!result) {
			return nullptr;
		}

		auto child_bindings = result->output_bindings;
		auto child_expressions = std::move(result->output_expressions);
		result->output_bindings = projection.GetColumnBindings();
		result->output_expressions.clear();
		result->output_expressions.reserve(projection.expressions.size());
		for (auto &expr : projection.expressions) {
			auto rewritten_expr = expr->Copy();
			ReplaceExpressionBindings(rewritten_expr, child_bindings, child_expressions);
			result->output_expressions.push_back(std::move(rewritten_expr));
		}
		return result;
	}
	return nullptr;
}

unique_ptr<GeneratedDomainRef> GeneratedDomainJoinEliminator::GetGeneratedDomainRef(LogicalOperator &op,
                                                                                    bool collect_filters,
                                                                                    bool allow_projection) const {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cteref = op.Cast<LogicalCTERef>();
		if (std::find(generated_dedup_cte_indexes.begin(), generated_dedup_cte_indexes.end(), cteref.cte_index) !=
		    generated_dedup_cte_indexes.end()) {
			return nullptr;
		}

		auto cte = FindCTE(cteref.cte_index);
		if (!cte || cte->children.empty()) {
			return nullptr;
		}
		if (cte->type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
			return nullptr;
		}
		auto result = GetGeneratedDomainDefinition(*cte->children[0]);
		if (!result || result->output_expressions.size() != cteref.chunk_types.size()) {
			return nullptr;
		}

		result->cte_ref = cteref;
		result->output_bindings = cteref.GetColumnBindings();
		vector<TableIndex> seen_ctes;
		result->has_selection = CTEHasSelection(cteref.cte_index, seen_ctes);
		return result;
	}
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &filter = op.Cast<LogicalFilter>();
		if (filter.HasProjectionMap() || filter.children.size() != 1) {
			return nullptr;
		}
		auto result = GetGeneratedDomainRef(*filter.children[0], collect_filters, allow_projection);
		if (!result) {
			return nullptr;
		}
		if (collect_filters) {
			for (auto &expr : filter.expressions) {
				auto filter_expr = expr->Copy();
				ReplaceExpressionBindings(filter_expr, result->output_bindings, result->output_expressions);
				result->filters.push_back(std::move(filter_expr));
			}
		}
		return result;
	}
	if (allow_projection && op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		if (projection.children.size() != 1) {
			return nullptr;
		}
		auto result = GetGeneratedDomainRef(*projection.children[0], collect_filters, allow_projection);
		if (!result) {
			return nullptr;
		}

		auto child_bindings = result->output_bindings;
		auto child_expressions = std::move(result->output_expressions);
		result->output_bindings = projection.GetColumnBindings();
		result->output_expressions.clear();
		result->output_expressions.reserve(projection.expressions.size());
		for (auto &expr : projection.expressions) {
			auto rewritten_expr = expr->Copy();
			ReplaceExpressionBindings(rewritten_expr, child_bindings, child_expressions);
			result->output_expressions.push_back(std::move(rewritten_expr));
		}
		return result;
	}
	return nullptr;
}

optional_idx GeneratedDomainJoinEliminator::FindOutputBinding(Expression &expr,
                                                              const vector<ColumnBinding> &bindings) const {
	ColumnBinding binding;
	if (!GetBoundColumnRefBinding(expr, binding)) {
		return optional_idx();
	}
	return FindBindingIndex(bindings, binding);
}

bool GeneratedDomainJoinEliminator::ContainsRecursiveCTERef(LogicalOperator &op) const {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cteref = op.Cast<LogicalCTERef>();
		for (auto &cte : ctes) {
			if (cte.get().table_index == cteref.cte_index) {
				return cte.get().type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE;
			}
		}
	}
	for (auto &child : op.children) {
		if (ContainsRecursiveCTERef(*child)) {
			return true;
		}
	}
	return false;
}

bool GeneratedDomainJoinEliminator::RemoveGeneratedDedupJoin(unique_ptr<LogicalOperator> &join,
                                                             BindingReplacementGraph &replacements,
                                                             bool under_aggregate, bool under_evidence_side) {
	auto &comparison_join = join->Cast<LogicalComparisonJoin>();
	if (comparison_join.join_type != JoinType::INNER &&
	    (comparison_join.join_type != JoinType::SEMI || !GetGeneratedDedupRef(*join->children[1], false, true))) {
		return false;
	}

	auto left_is_generated = GetGeneratedDedupRef(*join->children[0], false, true) != nullptr;
	auto right_is_generated = GetGeneratedDedupRef(*join->children[1], false, true) != nullptr;
	if (left_is_generated == right_is_generated) {
		return false;
	}
	const idx_t dedup_idx = left_is_generated ? 0 : 1;
	if (ContainsRecursiveCTERef(*join->children[1 - dedup_idx])) {
		return false;
	}
	auto dedup_ref = GetGeneratedDedupRef(*join->children[dedup_idx], true, true);
	if (!dedup_ref) {
		return false;
	}
	if ((under_aggregate || under_evidence_side) && GeneratedDedupRefHasSelection(*dedup_ref)) {
		// The first CTE rewrite can preserve selected domains below aggregates or existence checks as regular joins.
		// Do not substitute that selected generated CTE away in the cleanup pass.
		return false;
	}

	vector<ColumnBinding> covered_dedup_bindings;
	vector<ColumnBinding> base_replacement_bindings;
	vector<unique_ptr<Expression>> base_replacement_expressions;
	vector<unique_ptr<Expression>> join_filter_expressions;
	BindingReplacementGraph replacement_bindings;
	for (auto &cond : comparison_join.conditions) {
		if (!cond.IsComparison() || !IsEqualityJoinCondition(cond)) {
			return false;
		}
		auto lhs_generated_idx = FindOutputBinding(cond.GetLHS(), dedup_ref->output_bindings);
		auto rhs_generated_idx = FindOutputBinding(cond.GetRHS(), dedup_ref->output_bindings);
		if (lhs_generated_idx.IsValid() == rhs_generated_idx.IsValid()) {
			return false;
		}
		auto generated_idx = lhs_generated_idx.IsValid() ? lhs_generated_idx.GetIndex() : rhs_generated_idx.GetIndex();
		auto &generated_binding = dedup_ref->output_bindings[generated_idx];
		auto &generated_expression = *dedup_ref->output_expressions[generated_idx];
		auto &other_side = lhs_generated_idx.IsValid() ? cond.GetRHS() : cond.GetLHS();

		ColumnBinding other_binding;
		if (!GetBoundColumnRefBinding(other_side, other_binding)) {
			return false;
		}
		if (!replacement_bindings.TryAdd(ReplacementBinding(generated_binding, other_binding))) {
			return false;
		}

		ColumnBinding base_binding;
		if (GetBoundColumnRefBinding(generated_expression, base_binding) &&
		    base_binding.table_index == dedup_ref->cte_ref->table_index) {
			if (!replacement_bindings.TryAdd(ReplacementBinding(base_binding, other_binding))) {
				return false;
			}
			covered_dedup_bindings.emplace_back(base_binding);
			if (!AddExpressionReplacement(base_replacement_bindings, base_replacement_expressions, base_binding,
			                              other_side.Copy())) {
				return false;
			}
		}

		join_filter_expressions.push_back(
		    BoundComparisonExpression::Create(cond.GetComparisonType(), cond.GetLHS().Copy(), cond.GetRHS().Copy()));
	}

	auto cte_bindings = LogicalOperator::GenerateColumnBindings(dedup_ref->cte_ref->table_index,
	                                                            dedup_ref->cte_ref->chunk_types.size());
	if (!CoversAllBindings(cte_bindings, covered_dedup_bindings)) {
		return false;
	}

	vector<unique_ptr<Expression>> generated_output_replacements;
	generated_output_replacements.reserve(dedup_ref->output_expressions.size());
	for (auto &expr : dedup_ref->output_expressions) {
		auto rewritten_expr = expr->Copy();
		ReplaceExpressionBindings(rewritten_expr, base_replacement_bindings, base_replacement_expressions);
		if (ExpressionReferencesBinding(*rewritten_expr, dedup_ref->output_bindings) ||
		    ExpressionReferencesBinding(*rewritten_expr, cte_bindings)) {
			return false;
		}
		generated_output_replacements.push_back(std::move(rewritten_expr));
	}

	vector<unique_ptr<Expression>> filter_expressions;
	for (auto &expr : dedup_ref->filters) {
		ReplaceExpressionBindings(expr, base_replacement_bindings, base_replacement_expressions);
		if (ExpressionReferencesBinding(*expr, dedup_ref->output_bindings) ||
		    ExpressionReferencesBinding(*expr, cte_bindings)) {
			return false;
		}
		filter_expressions.push_back(std::move(expr));
	}
	for (auto &expr : join_filter_expressions) {
		ReplaceExpressionBindings(expr, dedup_ref->output_bindings, generated_output_replacements);
		ReplaceExpressionBindings(expr, base_replacement_bindings, base_replacement_expressions);
		if (ExpressionReferencesBinding(*expr, dedup_ref->output_bindings) ||
		    ExpressionReferencesBinding(*expr, cte_bindings)) {
			return false;
		}
		if (IsTautologicalNotDistinctComparison(*expr)) {
			continue;
		}
		filter_expressions.push_back(std::move(expr));
	}

	unique_ptr<LogicalOperator> replacement_op = std::move(comparison_join.children[1 - dedup_idx]);
	for (auto &expr : filter_expressions) {
		AddFilterToOperator(replacement_op, std::move(expr));
	}
	join = std::move(replacement_op);

	ColumnBindingReplacer column_replacer;
	replacement_bindings.AddTo(column_replacer);
	column_replacer.BuildIndex();
	column_replacer.VisitOperator(*rewrite_root);
	ReplaceOperatorBindings(*rewrite_root, dedup_ref->output_bindings, generated_output_replacements);
	replacements.Merge(replacement_bindings);
	MergeBindingExpressionReplacements(replacements, dedup_ref->output_bindings, generated_output_replacements);
	return true;
}

bool GeneratedDomainJoinEliminator::RemoveGeneratedDomainJoin(unique_ptr<LogicalOperator> &join,
                                                              BindingReplacementGraph &replacements,
                                                              bool under_aggregate, bool under_evidence_side) {
	auto &comparison_join = join->Cast<LogicalComparisonJoin>();
	if (comparison_join.join_type != JoinType::INNER) {
		return false;
	}

	auto left_generated = GetGeneratedDedupRef(*join->children[0], false, true);
	auto right_generated = GetGeneratedDedupRef(*join->children[1], false, true);
	auto left_domain = GetGeneratedDomainRef(*join->children[0], false, true);
	auto right_domain = GetGeneratedDomainRef(*join->children[1], false, true);
	auto left_is_generated = left_generated != nullptr;
	auto right_is_generated = right_generated != nullptr;
	auto left_is_domain = left_domain != nullptr;
	auto right_is_domain = right_domain != nullptr;
	if (left_is_generated == right_is_generated || left_is_domain == right_is_domain) {
		return false;
	}
	if (left_is_generated == left_is_domain) {
		return false;
	}

	const idx_t generated_idx = left_is_generated ? 0 : 1;
	const idx_t domain_idx = 1 - generated_idx;
	auto generated_ref = GetGeneratedDedupRef(*join->children[generated_idx], true, true);
	auto domain_ref = GetGeneratedDomainRef(*join->children[domain_idx], true, true);
	if (!generated_ref || !domain_ref) {
		return false;
	}
	if ((under_aggregate || under_evidence_side) && (domain_ref->has_selection || !domain_ref->filters.empty())) {
		// Same invariant as above: selected domains below aggregates or existence checks are part of the physical
		// reduction.
		return false;
	}

	vector<ColumnBinding> source_replacement_bindings;
	vector<unique_ptr<Expression>> source_replacement_expressions;
	vector<ColumnBinding> covered_source_bindings;
	vector<unique_ptr<Expression>> join_filter_expressions;
	for (auto &cond : comparison_join.conditions) {
		if (!cond.IsComparison() || !IsEqualityJoinCondition(cond)) {
			return false;
		}

		auto lhs_generated_idx = FindOutputBinding(cond.GetLHS(), generated_ref->output_bindings);
		auto rhs_generated_idx = FindOutputBinding(cond.GetRHS(), generated_ref->output_bindings);
		auto lhs_domain_idx = FindOutputBinding(cond.GetLHS(), domain_ref->output_bindings);
		auto rhs_domain_idx = FindOutputBinding(cond.GetRHS(), domain_ref->output_bindings);

		const bool lhs_generated = lhs_generated_idx.IsValid();
		const bool rhs_generated = rhs_generated_idx.IsValid();
		const bool lhs_domain = lhs_domain_idx.IsValid();
		const bool rhs_domain = rhs_domain_idx.IsValid();
		if (lhs_generated == rhs_generated || lhs_domain == rhs_domain || lhs_generated == lhs_domain) {
			return false;
		}

		auto domain_output_idx = lhs_domain ? lhs_domain_idx.GetIndex() : rhs_domain_idx.GetIndex();
		auto &domain_expression = *domain_ref->output_expressions[domain_output_idx];
		ColumnBinding source_binding;
		if (GetBoundColumnRefBinding(domain_expression, source_binding) &&
		    FindBindingIndex(domain_ref->source_bindings, source_binding).IsValid()) {
			auto generated_expression = lhs_generated ? cond.GetLHS().Copy() : cond.GetRHS().Copy();
			if (!AddExpressionReplacement(source_replacement_bindings, source_replacement_expressions, source_binding,
			                              std::move(generated_expression))) {
				return false;
			}
			covered_source_bindings.emplace_back(source_binding);
		}

		join_filter_expressions.push_back(
		    BoundComparisonExpression::Create(cond.GetComparisonType(), cond.GetLHS().Copy(), cond.GetRHS().Copy()));
	}
	if (!CoversAllBindings(domain_ref->source_bindings, covered_source_bindings)) {
		return false;
	}

	vector<unique_ptr<Expression>> domain_output_replacements;
	domain_output_replacements.reserve(domain_ref->output_expressions.size());
	for (auto &expr : domain_ref->output_expressions) {
		auto rewritten_expr = expr->Copy();
		ReplaceExpressionBindings(rewritten_expr, source_replacement_bindings, source_replacement_expressions);
		if (ExpressionReferencesBinding(*rewritten_expr, domain_ref->output_bindings) ||
		    ExpressionReferencesBinding(*rewritten_expr, domain_ref->source_bindings)) {
			return false;
		}
		domain_output_replacements.push_back(std::move(rewritten_expr));
	}

	vector<unique_ptr<Expression>> filter_expressions;
	for (auto &expr : domain_ref->filters) {
		ReplaceExpressionBindings(expr, source_replacement_bindings, source_replacement_expressions);
		if (ExpressionReferencesBinding(*expr, domain_ref->output_bindings) ||
		    ExpressionReferencesBinding(*expr, domain_ref->source_bindings)) {
			return false;
		}
		filter_expressions.push_back(std::move(expr));
	}
	for (auto &expr : join_filter_expressions) {
		ReplaceExpressionBindings(expr, domain_ref->output_bindings, domain_output_replacements);
		ReplaceExpressionBindings(expr, source_replacement_bindings, source_replacement_expressions);
		if (ExpressionReferencesBinding(*expr, domain_ref->output_bindings) ||
		    ExpressionReferencesBinding(*expr, domain_ref->source_bindings)) {
			return false;
		}
		if (IsTautologicalNotDistinctComparison(*expr)) {
			continue;
		}
		filter_expressions.push_back(std::move(expr));
	}

	unique_ptr<LogicalOperator> replacement_op = std::move(comparison_join.children[generated_idx]);
	for (auto &expr : filter_expressions) {
		AddFilterToOperator(replacement_op, std::move(expr));
	}
	join = std::move(replacement_op);

	ReplaceOperatorBindings(*rewrite_root, domain_ref->output_bindings, domain_output_replacements);
	MergeBindingExpressionReplacements(replacements, domain_ref->output_bindings, domain_output_replacements);
	return true;
}

bool GeneratedDomainJoinEliminator::TryRewriteOnce(unique_ptr<LogicalOperator> &op,
                                                   BindingReplacementGraph &replacements, bool under_aggregate,
                                                   bool under_evidence_side, bool negated_marker_filter_above) {
	auto child_under_aggregate = under_aggregate || op->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY;
	for (idx_t child_idx = 0; child_idx < op->children.size(); child_idx++) {
		auto &child = op->children[child_idx];
		auto old_child_bindings = child->GetColumnBindings();
		auto child_under_evidence_side = under_evidence_side;
		if (negated_marker_filter_above && IsEvidenceSide(*op, child_idx)) {
			child_under_evidence_side = true;
		} else if (!under_evidence_side && op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
			auto &join = op->Cast<LogicalComparisonJoin>();
			const auto preserve_mark_evidence =
			    join.join_type == JoinType::MARK &&
			    preserve_marker_indexes.find(join.mark_index) != preserve_marker_indexes.end();
			if ((join.join_type == JoinType::ANTI || join.join_type == JoinType::RIGHT_ANTI ||
			     preserve_mark_evidence) &&
			    IsEvidenceSide(*op, child_idx)) {
				child_under_evidence_side = true;
			}
		}

		bool child_negated_marker_filter_above = false;
		if (op->type == LogicalOperatorType::LOGICAL_FILTER && child_idx == 0 &&
		    child->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
			auto &filter = op->Cast<LogicalFilter>();
			auto &join = child->Cast<LogicalComparisonJoin>();
			child_negated_marker_filter_above = FilterNegatesDelimJoinMarker(filter, join);
		}

		BindingReplacementGraph child_replacements;
		if (TryRewriteOnce(child, child_replacements, child_under_aggregate, child_under_evidence_side,
		                   child_negated_marker_filter_above)) {
			ColumnBindingRewrite::ApplyToChild(op, child_idx, std::move(old_child_bindings), child_replacements);
			replacements.Merge(child_replacements.ScopedToBoundary(op->children[child_idx]->GetColumnBindings()));
			return true;
		}
	}
	if (op->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	return RemoveGeneratedDomainJoin(op, replacements, under_aggregate, under_evidence_side) ||
	       RemoveGeneratedDedupJoin(op, replacements, under_aggregate, under_evidence_side);
}

bool GeneratedDomainJoinEliminator::Rewrite() {
	if (generated_dedup_cte_indexes.empty()) {
		return false;
	}

	bool changed = false;
	while (true) {
		ctes.clear();
		CollectCTEs(*rewrite_root);
		BindingReplacementGraph replacements;
		if (!TryRewriteOnce(rewrite_root, replacements)) {
			break;
		}
		changed = true;
	}
	return changed;
}

static bool SingleJoinRHSIsDeduplicated(LogicalComparisonJoin &join) {
	if (join.join_type != JoinType::SINGLE) {
		return false;
	}

	vector<ColumnBinding> join_bindings;
	for (const auto &cond : join.conditions) {
		if (!IsEqualityJoinCondition(cond)) {
			return false;
		}
		if (!cond.IsComparison() || cond.GetRHS().GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			return false;
		}
		auto &colref = cond.GetRHS().Cast<BoundColumnRefExpression>();
		join_bindings.emplace_back(colref.Binding());
	}

	reference<LogicalOperator> current_op = *join.children[1];
	while (current_op.get().type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		if (current_op.get().children.size() != 1) {
			return false;
		}

		switch (current_op.get().type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
			if (!FindAndReplaceBindings(join_bindings, current_op.get().expressions,
			                            current_op.get().GetColumnBindings())) {
				return false;
			}
			break;
		case LogicalOperatorType::LOGICAL_FILTER:
			break;
		default:
			return false;
		}
		current_op = *current_op.get().children[0];
	}

	auto &aggr = current_op.get().Cast<LogicalAggregate>();
	if (!aggr.grouping_functions.empty()) {
		return false;
	}
	if (aggr.grouping_sets.size() > 1) {
		// multiple grouping sets emit one row per set, so the groups do not determine a unique row
		return false;
	}

	for (idx_t group_idx = 0; group_idx < aggr.groups.size(); group_idx++) {
		if (std::find(join_bindings.begin(), join_bindings.end(),
		              ColumnBinding(aggr.group_index, ProjectionIndex(group_idx))) == join_bindings.end()) {
			return false;
		}
	}

	return true;
}

static bool HasPartitionedRecursiveReferences(LogicalOperator &op, const LogicalRecursiveCTE &cte) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &ref = op.Cast<LogicalCTERef>();
		if (ref.cte_index == cte.table_index && ref.correlated_columns != cte.correlated_columns.size()) {
			return false;
		}
	}
	for (auto &child : op.children) {
		if (!HasPartitionedRecursiveReferences(*child, cte)) {
			return false;
		}
	}
	return true;
}

static bool HasPartitionedRecursiveKeys(LogicalRecursiveCTE &cte) {
	if (cte.key_targets.empty() && !cte.ref_recurring) {
		return true;
	}
	if (cte.correlated_columns.empty() || cte.column_count < cte.correlated_columns.size()) {
		return false;
	}
	// Decorrelation appends the correlation tuple to both recursive rows and USING KEY targets.
	const auto offset = cte.column_count - cte.correlated_columns.size();
	for (idx_t i = offset; i < cte.column_count; i++) {
		bool found = false;
		for (auto &key : cte.key_targets) {
			if (key->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
			    key->Cast<BoundColumnRefExpression>().Binding() == ColumnBinding(cte.table_index, ProjectionIndex(i))) {
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}
	return HasPartitionedRecursiveReferences(*cte.children[1], cte);
}

// Decorrelation partitions the RHS by its domain. Only retain this fact for repeatable, locally owned inputs.
static bool CanRestrictCorrelationDomain(LogicalOperator &op, unordered_set<TableIndex> &local_ctes) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET:
		if (!op.Cast<LogicalGet>().GetTable()) {
			return false;
		}
		break;
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE: {
		auto &cte = op.Cast<LogicalRecursiveCTE>();
		if (!HasPartitionedRecursiveKeys(cte)) {
			return false;
		}
		local_ctes.insert(cte.table_index);
		break;
	}
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
		local_ctes.insert(op.Cast<LogicalMaterializedCTE>().table_index);
		break;
	case LogicalOperatorType::LOGICAL_CTE_REF:
		if (!local_ctes.count(op.Cast<LogicalCTERef>().cte_index)) {
			return false;
		}
		break;
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
	case LogicalOperatorType::LOGICAL_DISTINCT:
	case LogicalOperatorType::LOGICAL_WINDOW:
	case LogicalOperatorType::LOGICAL_LIMIT:
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
		break;
	default:
		return false;
	}
	bool repeatable = true;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expr) {
		repeatable &= !(*expr)->IsVolatile() && !ExpressionBarrier::Contains(**expr);
		ExpressionIterator::VisitExpression<BoundFunctionExpression>(**expr, [&](const BoundFunctionExpression &func) {
			repeatable &= !func.Function().RequiresOrderedExecution();
		});
	});
	if (!repeatable) {
		return false;
	}
	for (auto &child : op.children) {
		if (!CanRestrictCorrelationDomain(*child, local_ctes)) {
			return false;
		}
	}
	return true;
}

DelimJoinCTERewriter::DelimJoinCTERewriter(Binder &binder) : binder(binder) {
	auto &config = DBConfig::GetConfig(binder.context);
	cte_deliminator_enabled = Settings::Get<EnableOptimizerSetting>(binder.context) &&
	                          !config.IsOptimizerDisabled(OptimizerType::DELIMINATOR);
}

BindingReplacementGraph DelimJoinCTERewriter::MaterializeDelimJoinAsCTE(unique_ptr<LogicalOperator> &plan,
                                                                        LogicalOperator &rewrite_root,
                                                                        bool null_rejecting_filter_above,
                                                                        bool preserve_evidence_side,
                                                                        bool preserve_nested_evidence_side) {
	BindingReplacementGraph output_replacements;
	{
		auto &join = plan->Cast<LogicalComparisonJoin>();
		if (join.delim_flipped) {
			throw InternalException("Flatten dependent joins - flipped delim join CTE rewrite not supported");
		}
	}

	plan->type = LogicalOperatorType::LOGICAL_COMPARISON_JOIN;

	auto dedup_cte_index = binder.GenerateTableIndex();
	auto dedup_ref_count = RewriteDelimScanReferences(plan->children[1], dedup_cte_index);
	if (cte_deliminator_enabled) {
		auto cte_deliminator_timer =
		    QueryProfiler::Get(binder.context).StartTimerInternal(CTE_DELIMINATOR_PROFILER_KEY);
		GeneratedDedupRefEliminator eliminator(binder.context, plan, dedup_cte_index, dedup_ref_count, rewrite_root,
		                                       preserve_evidence_side, preserve_nested_evidence_side);
		dedup_ref_count = eliminator.Remove();
		auto &join = plan->Cast<LogicalComparisonJoin>();
		if (SingleJoinRHSIsDeduplicated(join)) {
			join.join_type = null_rejecting_filter_above ? JoinType::INNER : JoinType::LEFT;
		}
	}
	auto &join = plan->Cast<LogicalComparisonJoin>();
	if (dedup_ref_count == 0) {
		join.duplicate_eliminated_columns.clear();
		return {};
	}
	generated_dedup_cte_indexes.push_back(dedup_cte_index);
	unordered_set<TableIndex> local_ctes {dedup_cte_index};
	const bool requires_left_row = join.join_type == JoinType::INNER || join.join_type == JoinType::LEFT ||
	                               join.join_type == JoinType::SEMI || join.join_type == JoinType::ANTI ||
	                               join.join_type == JoinType::MARK;
	// Only RHS partitions are removed; ordinary filter pushdown handles the producer.
	const bool can_restrict_input = requires_left_row && CanRestrictCorrelationDomain(*plan->children[1], local_ctes);

	auto left_bindings = plan->children[0]->GetColumnBindings();
	auto visible_left_column_count = left_bindings.size();

	vector<idx_t> dedup_column_indices;
	vector<LogicalType> dedup_types;
	vector<Identifier> dedup_names;
	vector<unique_ptr<Expression>> extra_left_expressions;
	dedup_column_indices.reserve(join.duplicate_eliminated_columns.size());
	dedup_types.reserve(join.duplicate_eliminated_columns.size());
	dedup_names.reserve(join.duplicate_eliminated_columns.size());
	for (auto &expr : join.duplicate_eliminated_columns) {
		optional_idx binding_index;
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto &colref_expr = expr->Cast<BoundColumnRefExpression>();
			binding_index = FindBindingIndex(left_bindings, colref_expr.Binding());
		}
		if (binding_index.IsValid()) {
			dedup_column_indices.push_back(binding_index.GetIndex());
		} else {
			dedup_column_indices.push_back(left_bindings.size() + extra_left_expressions.size());
			extra_left_expressions.push_back(expr->Copy());
		}
		dedup_types.push_back(expr->GetReturnType());
		dedup_names.push_back(expr->GetName());
	}

	if (!extra_left_expressions.empty()) {
		auto old_left_bindings = left_bindings;
		auto projection = LogicalProjection::CreateIdentity(binder.GenerateTableIndex(), std::move(plan->children[0]));
		for (auto &expr : extra_left_expressions) {
			projection->expressions.push_back(std::move(expr));
		}
		plan->children[0] = std::move(projection);
		left_bindings = plan->children[0]->GetColumnBindings();
		vector<ColumnBinding> projected_left_bindings(
		    left_bindings.begin(),
		    left_bindings.begin() + NumericCast<vector<ColumnBinding>::difference_type>(old_left_bindings.size()));
		auto projection_replacements = CreateConstructedBindingReplacements(old_left_bindings, projected_left_bindings);
		CorrelatedColumnBindingReplacer projection_replacer;
		projection_replacements.AddTo(projection_replacer);
		projection_replacer.BuildIndex();
		projection_replacer.stop_operator = plan->children[1];
		projection_replacer.VisitOperator(*plan);
		output_replacements.Merge(projection_replacements);
		if (join.left_projection_map.empty()) {
			join.left_projection_map.reserve(visible_left_column_count);
			for (idx_t i = 0; i < visible_left_column_count; i++) {
				join.left_projection_map.emplace_back(i);
			}
		}
	}

	auto left_column_count = left_bindings.size();
	auto cte_source_bindings = left_bindings;
	auto cte_source = LogicalProjection::CreateIdentity(binder.GenerateTableIndex(), std::move(plan->children[0]));
	cte_source->ResolveOperatorTypes();
	auto left_types = cte_source->types;

	auto cte_index = binder.GenerateTableIndex();
	auto cte_name = Identifier("__duckdb_delim_" + to_string(cte_index.index));

	auto left_cte_ref_index = binder.GenerateTableIndex();
	auto left_cte_ref = make_uniq<LogicalCTERef>(left_cte_ref_index, cte_index, left_types,
	                                             GenerateCTEColumnNames(left_column_count, "__duckdb_delim_col_"));
	auto new_left_bindings = left_cte_ref->GetColumnBindings();
	auto binding_replacements = CreateConstructedBindingReplacements(cte_source_bindings, new_left_bindings);
	output_replacements.Merge(binding_replacements);

	plan->children[0] = std::move(left_cte_ref);
	ColumnBindingReplacer replacer;
	binding_replacements.AddTo(replacer);
	replacer.BuildIndex();
	replacer.stop_operator = plan->children[1];
	replacer.VisitOperator(*plan);

	join.duplicate_eliminated_columns.clear();

	auto dedup_group_index = binder.GenerateTableIndex();
	auto dedup_aggregate_index = binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> dedup_aggrs;
	auto dedup = make_uniq<LogicalAggregate>(dedup_group_index, dedup_aggregate_index, std::move(dedup_aggrs));
	auto dedup_child_index = binder.GenerateTableIndex();
	auto dedup_child = make_uniq<LogicalCTERef>(dedup_child_index, cte_index, left_types,
	                                            GenerateCTEColumnNames(left_column_count, "__duckdb_delim_col_"));
	auto dedup_child_bindings = dedup_child->GetColumnBindings();
	for (idx_t i = 0; i < dedup_column_indices.size(); i++) {
		auto colref = make_uniq<BoundColumnRefExpression>(dedup_names[i], dedup_types[i],
		                                                  dedup_child_bindings[dedup_column_indices[i]]);
		auto new_group_index = ColumnBinding::PushExpression(dedup->groups, std::move(colref));
		for (auto &set : dedup->grouping_sets) {
			set.insert(new_group_index);
		}
	}
	dedup->children.push_back(std::move(dedup_child));

	auto dedup_cte_name = Identifier("__duckdb_delim_dedup_" + to_string(dedup_cte_index.index));
	BindingReplacementGraph dedup_output_replacements;
	auto dedup_cte_child = CreateIdentityProjection(binder, std::move(plan), dedup_output_replacements);
	output_replacements.Merge(dedup_output_replacements);
	auto dedup_cte =
	    make_uniq<LogicalMaterializedCTE>(dedup_cte_name, dedup_cte_index, dedup_types.size(), std::move(dedup),
	                                      std::move(dedup_cte_child), CTEMaterialize::CTE_MATERIALIZE_DEFAULT);
	BindingReplacementGraph cte_output_replacements;
	auto cte_child = CreateIdentityProjection(binder, std::move(dedup_cte), cte_output_replacements);
	output_replacements.Merge(cte_output_replacements);
	auto cte = make_uniq<LogicalMaterializedCTE>(cte_name, cte_index, left_column_count, std::move(cte_source),
	                                             std::move(cte_child), CTEMaterialize::CTE_MATERIALIZE_DEFAULT);
	if (can_restrict_input) {
		cte->filter_dependency = make_uniq<CTEFilterDependency>(left_cte_ref_index, dedup_child_index);
	}
	plan = std::move(cte);
	return output_replacements;
}

BindingReplacementGraph DelimJoinCTERewriter::RewriteDelimJoinsToCTEs(unique_ptr<LogicalOperator> &plan,
                                                                      LogicalOperator &rewrite_root,
                                                                      bool null_rejecting_filter_above,
                                                                      bool preserve_evidence_side) {
	BindingReplacementGraph output_replacements;
	bool preserve_nested_evidence_side = false;
	bool has_non_subquery_selection = false;
	if (plan->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		auto &join = plan->Cast<LogicalComparisonJoin>();
		preserve_nested_evidence_side =
		    NestedDelimJoinUsesAdditionalCorrelation(*join.children[0], join.duplicate_eliminated_columns);
		has_non_subquery_selection =
		    HasDelimiterDomainSelection(*join.children[0], marker_indexes, true, join.duplicate_eliminated_columns);
	}
	for (idx_t child_idx = 0; child_idx < plan->children.size(); child_idx++) {
		auto &child = plan->children[child_idx];
		auto old_child_bindings = child->GetColumnBindings();
		bool child_null_rejecting_filter_above = false;
		bool child_preserve_evidence_side = false;
		if (plan->type == LogicalOperatorType::LOGICAL_FILTER &&
		    child->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
			auto &filter = plan->Cast<LogicalFilter>();
			auto &delim_join = child->Cast<LogicalComparisonJoin>();
			child_null_rejecting_filter_above = FilterNullRejectsDelimJoinRHS(filter, delim_join);
			child_preserve_evidence_side = FilterNegatesDelimJoinMarker(filter, delim_join);
		}
		auto child_replacements = RewriteDelimJoinsToCTEs(child, rewrite_root, child_null_rejecting_filter_above,
		                                                  child_preserve_evidence_side);
		ColumnBindingRewrite::ApplyToChild(plan, child_idx, std::move(old_child_bindings), child_replacements);
		// Scope to the child's output boundary: operators above cannot see bindings that no longer resolve into it
		output_replacements.Merge(child_replacements.ScopedToBoundary(plan->children[child_idx]->GetColumnBindings()));
	}
	if (plan->type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		auto &join = plan->Cast<LogicalComparisonJoin>();
		if (join.join_type == JoinType::MARK &&
		    preserve_marker_indexes.find(join.mark_index) != preserve_marker_indexes.end()) {
			preserve_evidence_side = true;
			preserve_nested_evidence_side =
			    preserve_nested_evidence_side || has_non_subquery_selection ||
			    preserve_nested_marker_indexes.find(join.mark_index) != preserve_nested_marker_indexes.end();
		}
		auto materialize_replacements = MaterializeDelimJoinAsCTE(
		    plan, rewrite_root, null_rejecting_filter_above, preserve_evidence_side, preserve_nested_evidence_side);
		output_replacements.Merge(materialize_replacements.ScopedToBoundary(plan->GetColumnBindings()));
	}
	return output_replacements;
}

void DelimJoinCTERewriter::Rewrite(Binder &binder, unique_ptr<LogicalOperator> &plan) {
	DelimJoinCTERewriter rewriter(binder);
	rewriter.Rewrite(plan);
}

void DelimJoinCTERewriter::Rewrite(unique_ptr<LogicalOperator> &plan) {
	marker_indexes.clear();
	preserve_marker_indexes.clear();
	preserve_nested_marker_indexes.clear();
	CollectDelimJoinMarkerIndexes(*plan, marker_indexes);
	auto marker_bindings = CreateMarkerBindingMap(marker_indexes);
	set<TableIndex> simple_marker_indexes;
	CollectMarkerReferences(*plan, marker_bindings, preserve_marker_indexes, simple_marker_indexes);
	CollectEligibleSimpleMarkerIndexes(*plan, simple_marker_indexes, preserve_nested_marker_indexes);
	preserve_marker_indexes.insert(preserve_nested_marker_indexes.begin(), preserve_nested_marker_indexes.end());
	bool filters_pushed;
	do {
		filters_pushed = PushEligibleFiltersIntoDelimJoinInputs(plan);
	} while (filters_pushed);
	RewriteDelimJoinsToCTEs(plan, *plan);
	if (cte_deliminator_enabled) {
		auto cte_deliminator_timer =
		    QueryProfiler::Get(binder.context).StartTimerInternal(CTE_DELIMINATOR_PROFILER_KEY);
		GeneratedDomainJoinEliminator generated_domain_join_eliminator(plan, generated_dedup_cte_indexes,
		                                                               preserve_marker_indexes);
		generated_domain_join_eliminator.Rewrite();
	}
	VerifyNoDelim(*plan);
}

} // namespace duckdb
