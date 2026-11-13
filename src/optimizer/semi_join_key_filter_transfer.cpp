#include "duckdb/optimizer/semi_join_key_filter_transfer.hpp"

#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

//! The search follows one column, but the equalities of an INNER join fork it: the bound keeps it linear
static constexpr idx_t MAX_SEARCH_STEPS = 256;

static optional_ptr<const BoundColumnRefExpression> PlainColumn(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	auto &column_ref = expr.Cast<BoundColumnRefExpression>();
	if (column_ref.Depth() != 0) {
		return nullptr;
	}
	return &column_ref;
}

static bool IsKey(const Expression &expr, const ColumnBinding &key) {
	auto column_ref = PlainColumn(expr);
	return column_ref && column_ref->Binding() == key;
}

static bool IsConstant(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT;
}

static bool HasBinding(LogicalOperator &op, const ColumnBinding &binding) {
	for (auto &op_binding : op.GetColumnBindings()) {
		if (op_binding == binding) {
			return true;
		}
	}
	return false;
}

//! The CTE reference directly below a filter that passes every column on
static optional_ptr<const LogicalCTERef> FilteredCTEReference(const LogicalOperator &op) {
	if (op.type != LogicalOperatorType::LOGICAL_FILTER || op.Cast<LogicalFilter>().HasProjectionMap() ||
	    op.children[0]->type != LogicalOperatorType::LOGICAL_CTE_REF) {
		return nullptr;
	}
	return &op.children[0]->Cast<LogicalCTERef>();
}

static bool IsFilteringJoin(const LogicalOperator &op, JoinType join_type) {
	return op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN && op.children.size() == 2 &&
	       op.Cast<LogicalComparisonJoin>().join_type == join_type;
}

static bool IsEquality(const JoinCondition &condition) {
	return condition.IsComparison() && (condition.GetComparisonType() == ExpressionType::COMPARE_EQUAL ||
	                                    condition.GetComparisonType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM);
}

//! Whether the expression is a predicate over the column `key` alone that depends only on the value of the key under
//! the comparison order and cannot throw: a comparison with constants, a NULL test, or AND / OR over those
static bool IsKeyPredicate(const Expression &expr, const ColumnBinding &key) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &children = expr.Cast<BoundConjunctionExpression>().GetChildren();
		if (children.empty()) {
			return false;
		}
		for (auto &child : children) {
			if (!IsKeyPredicate(*child, key)) {
				return false;
			}
		}
		return true;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		auto &function = expr.Cast<BoundFunctionExpression>();
		if (expr.GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
			return BoundBetweenExpression::HasValidBindData(function) &&
			       IsKey(BoundBetweenExpression::Input(function), key) &&
			       IsConstant(BoundBetweenExpression::LowerBound(function)) &&
			       IsConstant(BoundBetweenExpression::UpperBound(function));
		}
		if (!BoundComparisonExpression::IsComparison(expr)) {
			return false;
		}
		switch (expr.GetExpressionType()) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOTEQUAL:
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			break;
		default:
			return false;
		}
		auto &left = BoundComparisonExpression::Left(function);
		auto &right = BoundComparisonExpression::Right(function);
		return (IsKey(left, key) && IsConstant(right)) || (IsConstant(left) && IsKey(right, key));
	}
	case ExpressionClass::BOUND_OPERATOR: {
		auto &children = expr.Cast<BoundOperatorExpression>().GetChildren();
		switch (expr.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			return children.size() == 1 && IsKey(*children[0], key);
		case ExpressionType::COMPARE_IN:
		case ExpressionType::COMPARE_NOT_IN: {
			if (children.size() < 2 || !IsKey(*children[0], key)) {
				return false;
			}
			for (idx_t child_idx = 1; child_idx < children.size(); child_idx++) {
				if (!IsConstant(*children[child_idx])) {
					return false;
				}
			}
			return true;
		}
		default:
			return false;
		}
	}
	default:
		return false;
	}
}

bool SemiJoinKeyFilterTransfer::IsColumnConstantPredicate(const Expression &expr) {
	optional_ptr<const BoundColumnRefExpression> column;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &ref) {
		if (!column) {
			column = &ref;
		}
	});
	return column && column->Depth() == 0 && IsKeyPredicate(expr, column->Binding());
}

//! Points every reference to `from` at `to`
static void Rebind(unique_ptr<Expression> &expr, const ColumnBinding &from, const ColumnBinding &to) {
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &column_ref = expr->Cast<BoundColumnRefExpression>();
		if (column_ref.Binding() == from) {
			column_ref.BindingMutable() = to;
		}
	}
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { Rebind(child, from, to); });
}

//! Adds the conjuncts of the expression that are key predicates on `key`
static void PushKeyPredicates(const Expression &expr, const ColumnBinding &key,
                              vector<unique_ptr<Expression>> &result) {
	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		for (auto &child : expr.Cast<BoundConjunctionExpression>().GetChildren()) {
			PushKeyPredicates(*child, key, result);
		}
		return;
	}
	if (IsKeyPredicate(expr, key)) {
		result.push_back(expr.Copy());
	}
}

void SemiJoinKeyFilterTransfer::CollectKeyPredicates(LogicalOperator &op, const BoundColumnRefExpression &key,
                                                     idx_t &steps, vector<unique_ptr<Expression>> &result) {
	if (++steps > MAX_SEARCH_STEPS) {
		return;
	}
	// the predicates of `column` below `child`, as predicates over `key`
	auto follow = [&](LogicalOperator &child, const BoundColumnRefExpression &column) {
		if (column.GetReturnType() != key.GetReturnType()) {
			return;
		}
		vector<unique_ptr<Expression>> child_result;
		CollectKeyPredicates(child, column, steps, child_result);
		for (auto &predicate : child_result) {
			Rebind(predicate, column.Binding(), key.Binding());
			result.push_back(std::move(predicate));
		}
	};
	auto &binding = key.Binding();
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_FILTER:
		for (auto &expr : op.expressions) {
			PushKeyPredicates(*expr, binding, result);
		}
		follow(*op.children[0], key);
		break;
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &projection = op.Cast<LogicalProjection>();
		if (binding.table_index != projection.table_index || binding.column_index.GetIndex() >= op.expressions.size()) {
			break;
		}
		auto source = PlainColumn(*op.expressions[binding.column_index.GetIndex()]);
		if (source) {
			follow(*op.children[0], *source);
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		auto &aggregate = op.Cast<LogicalAggregate>();
		if (binding.table_index != aggregate.group_index || aggregate.grouping_sets.size() > 1 ||
		    binding.column_index.GetIndex() >= aggregate.groups.size()) {
			break;
		}
		auto source = PlainColumn(*aggregate.groups[binding.column_index.GetIndex()]);
		if (source) {
			follow(*op.children[0], *source);
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		if (binding.table_index != get.table_index || !op.children.empty()) {
			break;
		}
		auto filter = get.table_filters.TryGetFilterByColumnIndex(binding.column_index);
		// an optional filter is a hint the scan may ignore
		if (!filter || ExpressionFilter::IsOptionalFilter(*filter)) {
			break;
		}
		auto expr = filter->ToExpression(key);
		PushKeyPredicates(*expr, binding, result);
		break;
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (op.children.size() != 2) {
			break;
		}
		optional_idx side;
		for (idx_t child_idx = 0; child_idx < 2; child_idx++) {
			if (HasBinding(*op.children[child_idx], binding)) {
				side = child_idx;
				break;
			}
		}
		if (!side.IsValid()) {
			break;
		}
		const auto side_idx = side.GetIndex();
		bool preserved;
		switch (join.join_type) {
		case JoinType::INNER:
			preserved = true;
			break;
		case JoinType::LEFT:
		case JoinType::SEMI:
		case JoinType::ANTI:
		case JoinType::MARK:
			preserved = side_idx == 0;
			break;
		case JoinType::RIGHT:
			preserved = side_idx == 1;
			break;
		default:
			preserved = false;
			break;
		}
		if (!preserved) {
			break;
		}
		follow(*op.children[side_idx], key);
		if (join.join_type != JoinType::INNER) {
			break;
		}
		// an output row has equal keys on both sides (or two NULLs): the predicates of the other side hold as well
		for (auto &condition : join.conditions) {
			if (!IsEquality(condition)) {
				continue;
			}
			auto left = PlainColumn(condition.GetLHS());
			auto right = PlainColumn(condition.GetRHS());
			if (!left || !right) {
				continue;
			}
			auto &own = side_idx == 0 ? *left : *right;
			auto &other = side_idx == 0 ? *right : *left;
			if (own.Binding() == binding && HasBinding(*op.children[1 - side_idx], other.Binding())) {
				follow(*op.children[1 - side_idx], other);
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		for (auto &child : op.children) {
			if (HasBinding(*child, binding)) {
				follow(*child, key);
				break;
			}
		}
		break;
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_LIMIT:
	case LogicalOperatorType::LOGICAL_TOP_N:
	case LogicalOperatorType::LOGICAL_DISTINCT:
	case LogicalOperatorType::LOGICAL_WINDOW:
		// rows are removed, reordered or kept, the columns of the child pass through under their own bindings
		if (op.children.size() == 1 && HasBinding(*op.children[0], binding)) {
			follow(*op.children[0], key);
		}
		break;
	default:
		// the null-extended side of an outer join, set operations and CTE references end the search
		break;
	}
}

void SemiJoinKeyFilterTransfer::CountReferences(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		references.emplace(op.Cast<LogicalMaterializedCTE>().table_index, CTEReferences());
	}
	auto filtered = FilteredCTEReference(op);
	optional_ptr<const LogicalCTERef> reference = filtered;
	if (!reference && op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		reference = &op.Cast<LogicalCTERef>();
	}
	if (reference) {
		auto entry = references.find(reference->cte_index);
		if (entry != references.end()) {
			entry->second.total++;
			if (filtered) {
				entry->second.filtered++;
			}
		}
		return;
	}
	for (auto &child : op.children) {
		CountReferences(*child);
	}
}

void SemiJoinKeyFilterTransfer::Transfer(LogicalOperator &join_op) {
	auto &join = join_op.Cast<LogicalComparisonJoin>();
	auto &left_op = *join_op.children[0];
	auto filtered = FilteredCTEReference(left_op);
	optional_ptr<const LogicalCTERef> cte_ref = filtered;
	if (!cte_ref && left_op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		cte_ref = &left_op.Cast<LogicalCTERef>();
	}
	if (!cte_ref || cte_ref->is_recurring || cte_ref->correlated_columns != 0) {
		return;
	}
	auto entry = references.find(cte_ref->cte_index);
	if (entry == references.end()) {
		return;
	}
	auto &cte_references = entry->second;
	if (cte_references.total != cte_references.filtered + (filtered ? 0 : 1)) {
		// another reference stays unfiltered: the CTE keeps all its rows whatever this join drops
		return;
	}
	vector<unique_ptr<Expression>> transferred;
	for (auto &condition : join.conditions) {
		if (!IsEquality(condition)) {
			continue;
		}
		auto left = PlainColumn(condition.GetLHS());
		auto right = PlainColumn(condition.GetRHS());
		if (!left || !right || left->GetReturnType() != right->GetReturnType() ||
		    !HasBinding(left_op, left->Binding()) || !HasBinding(*join_op.children[1], right->Binding())) {
			continue;
		}
		idx_t steps = 0;
		vector<unique_ptr<Expression>> predicates;
		CollectKeyPredicates(*join_op.children[1], *right, steps, predicates);
		if (predicates.empty()) {
			continue;
		}
		steps = 0;
		vector<unique_ptr<Expression>> present;
		CollectKeyPredicates(left_op, *left, steps, present);
		const bool rejects_null = condition.GetComparisonType() == ExpressionType::COMPARE_EQUAL;
		for (auto &predicate : predicates) {
			// l = r drops a NULL l by itself
			if (rejects_null && predicate->GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) {
				continue;
			}
			Rebind(predicate, right->Binding(), left->Binding());
			bool exists = false;
			for (auto &existing : present) {
				exists = exists || existing->Equals(*predicate);
			}
			for (auto &existing : transferred) {
				exists = exists || existing->Equals(*predicate);
			}
			if (!exists) {
				transferred.push_back(std::move(predicate));
			}
		}
	}
	if (transferred.empty()) {
		return;
	}
	filtered_ctes.insert(cte_ref->cte_index);
	if (filtered) {
		for (auto &predicate : transferred) {
			left_op.expressions.push_back(std::move(predicate));
		}
		return;
	}
	auto filter = make_uniq<LogicalFilter>();
	filter->expressions = std::move(transferred);
	filter->children.push_back(std::move(join_op.children[0]));
	join_op.children[0] = std::move(filter);
	cte_references.filtered++;
}

void SemiJoinKeyFilterTransfer::VisitOperator(unique_ptr<LogicalOperator> &op) {
	// children first, so that a nested join has restricted its left side before the join above searches that side
	for (auto &child : op->children) {
		VisitOperator(child);
	}
	if (IsFilteringJoin(*op, JoinType::SEMI)) {
		Transfer(*op);
		return;
	}
	if (op->type != LogicalOperatorType::LOGICAL_FILTER || !IsFilteringJoin(*op->children[0], JoinType::MARK)) {
		return;
	}
	// a MARK join filters only under a conjunct that is the bare mark
	auto mark_index = op->children[0]->Cast<LogicalComparisonJoin>().mark_index;
	for (auto &expr : op->expressions) {
		auto column_ref = PlainColumn(*expr);
		if (column_ref && column_ref->Binding().table_index == mark_index) {
			Transfer(*op->children[0]);
			return;
		}
	}
}

void SemiJoinKeyFilterTransfer::Optimize(unique_ptr<LogicalOperator> &plan) {
	CountReferences(*plan);
	if (references.empty()) {
		return;
	}
	VisitOperator(plan);
}

} // namespace duckdb
