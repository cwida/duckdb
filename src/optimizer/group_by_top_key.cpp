#include "duckdb/optimizer/group_by_top_key.hpp"

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/expression_heuristics.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/topn_optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_top_key_filter.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"

namespace duckdb {

namespace {

//! The largest LIMIT + OFFSET of the rewrite: every thread keeps that many distinct keys
constexpr idx_t MAX_TOP_KEYS = 10000;

//! The largest cost of a key that is an expression over the group keys (ORDER BY k + 1): the filter evaluates it for
//! every input row, whereas the query evaluates it once per group. A key that is a group key costs nothing extra.
constexpr idx_t MAX_DERIVED_KEY_COST = 50;

//! The dynamic filter in the scans only pays off if it removes most rows: the limit has to be a small part of the
//! groups. Without it the filter operator sees every row and stops filtering on its own if too many rows pass.
constexpr idx_t MIN_GROUPS_PER_KEY = 8;

//! Whether the limit is a small part of the estimated number of groups of the aggregate
bool LimitIsSelective(const LogicalAggregate &aggregate, idx_t limit) {
	if (!aggregate.has_estimated_cardinality) {
		return true;
	}
	return limit <= aggregate.estimated_cardinality / MIN_GROUPS_PER_KEY;
}

bool IsInputColumn(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
	       expr.Cast<BoundColumnRefExpression>().Depth() == 0;
}

//! Whether the expression has the same value whenever it is evaluated on the same input
bool IsDeterministic(const Expression &expr) {
	return !expr.IsVolatile() && expr.IsConsistent() && !expr.HasSubquery() && !expr.IsAggregate() &&
	       !expr.IsWindow() && !expr.HasParameter();
}

//! Whether the aggregate groups by all of its group keys as one set
bool IsSingleGroupingSet(const LogicalAggregate &aggregate) {
	if (aggregate.grouping_sets.empty()) {
		return true;
	}
	return aggregate.grouping_sets.size() == 1 && aggregate.grouping_sets[0].size() == aggregate.groups.size();
}

bool IsPlainGroupedAggregate(const LogicalAggregate &aggregate) {
	return !aggregate.groups.empty() && IsSingleGroupingSet(aggregate) && aggregate.grouping_functions.empty();
}

//! The column that a projection expression forwards in a way that preserves its order: the column itself, an integral
//! cast of it or the decompression of a compressed materialization (which preserves the order)
bool TryGetForwardedColumn(const Expression &expr, ColumnBinding &result) {
	if (IsInputColumn(expr)) {
		result = expr.Cast<BoundColumnRefExpression>().Binding();
		return true;
	}
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<BoundFunctionExpression>();
	if (BoundCastExpression::IsCast(expr)) {
		if (BoundCastExpression::IsTryCast(function) || !BoundCastExpression::SourceType(function).IsIntegral() ||
		    !BoundCastExpression::TargetType(function).IsIntegral()) {
			return false;
		}
	} else if (!StringUtil::StartsWith(function.Function().GetName().GetIdentifierName(), "__internal_decompress_")) {
		return false;
	}
	if (function.GetChildren().empty() || !IsInputColumn(*function.GetChildren()[0])) {
		return false;
	}
	result = function.GetChildren()[0]->Cast<BoundColumnRefExpression>().Binding();
	return true;
}

//! The group key that the binding - an output column of the aggregate - is, as an expression over the input of the
//! aggregate
unique_ptr<Expression> GetGroupKey(const LogicalAggregate &aggregate, const ColumnBinding &binding) {
	if (binding.table_index != aggregate.group_index || binding.column_index.GetIndex() >= aggregate.groups.size()) {
		return nullptr;
	}
	auto &group = *aggregate.groups[binding.column_index.GetIndex()];
	if (!IsInputColumn(group) && !IsDeterministic(group)) {
		return nullptr;
	}
	return group.Copy();
}

//! The group key that the binding - an output column of projections[level] - forwards through order-preserving
//! projection expressions only
unique_ptr<Expression> ResolveColumn(ColumnBinding binding, idx_t level,
                                     const vector<reference<LogicalOperator>> &projections,
                                     const LogicalAggregate &aggregate) {
	for (idx_t i = level; i < projections.size(); i++) {
		auto &projection = projections[i].get().Cast<LogicalProjection>();
		if (binding.table_index != projection.table_index ||
		    binding.column_index.GetIndex() >= projection.expressions.size()) {
			return nullptr;
		}
		if (!TryGetForwardedColumn(*projection.expressions[binding.column_index.GetIndex()], binding)) {
			return nullptr;
		}
	}
	return GetGroupKey(aggregate, binding);
}

//! The expression - over the output of projections[level] - as an expression over the input of the aggregate: a column
//! is replaced by the deterministic projection expression that computes it and, at the aggregate, by its group key
unique_ptr<Expression> Substitute(const Expression &expr, idx_t level,
                                  const vector<reference<LogicalOperator>> &projections,
                                  const LogicalAggregate &aggregate) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr.Cast<BoundColumnRefExpression>();
		if (colref.Depth() != 0) {
			return nullptr;
		}
		auto &binding = colref.Binding();
		if (level >= projections.size()) {
			return GetGroupKey(aggregate, binding);
		}
		auto &projection = projections[level].get().Cast<LogicalProjection>();
		if (binding.table_index != projection.table_index ||
		    binding.column_index.GetIndex() >= projection.expressions.size()) {
			return nullptr;
		}
		auto &computed = *projection.expressions[binding.column_index.GetIndex()];
		if (!IsDeterministic(computed)) {
			return nullptr;
		}
		return Substitute(computed, level + 1, projections, aggregate);
	}
	if (!IsDeterministic(expr)) {
		return nullptr;
	}
	auto result = expr.Copy();
	bool resolved = true;
	ExpressionIterator::EnumerateChildren(*result, [&](unique_ptr<Expression> &child) {
		if (!resolved) {
			return;
		}
		auto key = Substitute(*child, level, projections, aggregate);
		if (!key) {
			resolved = false;
			return;
		}
		child = std::move(key);
	});
	return resolved ? std::move(result) : nullptr;
}

} // namespace

GroupByTopKey::GroupByTopKey(Optimizer &optimizer) : optimizer(optimizer) {
}

//===--------------------------------------------------------------------===//
// LIMIT over a grouped aggregate
//===--------------------------------------------------------------------===//
unique_ptr<LogicalOperator> GroupByTopKey::LimitToTopN(unique_ptr<LogicalOperator> op) {
	auto &limit = op->Cast<LogicalLimit>();
	if (limit.limit_val.Type() != LimitNodeType::CONSTANT_VALUE) {
		return op;
	}
	idx_t offset_value = 0;
	if (limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
		offset_value = limit.offset_val.GetConstantValue();
	} else if (limit.offset_val.Type() != LimitNodeType::UNSET) {
		return op;
	}
	const auto limit_value = limit.limit_val.GetConstantValue();
	if (limit_value == 0 || limit_value > MAX_TOP_KEYS || offset_value > MAX_TOP_KEYS - limit_value) {
		return op;
	}
	// the projections down to the aggregate: a filter (HAVING) or anything else ends the match
	reference<LogicalOperator> child = *op->children[0];
	while (child.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		child = *child.get().children[0];
	}
	if (child.get().type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return op;
	}
	auto &aggregate = child.get().Cast<LogicalAggregate>();
	// an aggregate without aggregate functions is a DISTINCT: the LIMIT stops it early
	if (!IsPlainGroupedAggregate(aggregate) || aggregate.expressions.empty() ||
	    aggregate.children[0]->type == LogicalOperatorType::LOGICAL_TOP_KEY_FILTER) {
		return op;
	}
	// order by the first group key that is a column, preferring one that the scans can filter on
	optional_idx key_idx;
	for (idx_t group_idx = 0; group_idx < aggregate.groups.size(); group_idx++) {
		auto &group = *aggregate.groups[group_idx];
		if (!IsInputColumn(group)) {
			continue;
		}
		auto &type = group.GetReturnType();
		if (TypeIsNumeric(type.InternalType()) || type.id() == LogicalTypeId::VARCHAR) {
			key_idx = group_idx;
			break;
		}
		if (!key_idx.IsValid()) {
			key_idx = group_idx;
		}
	}
	if (!key_idx.IsValid()) {
		return op;
	}

	// detach the projections and the aggregate
	vector<unique_ptr<LogicalOperator>> projections;
	auto node = std::move(op->children[0]);
	while (node->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto next = std::move(node->children[0]);
		projections.push_back(std::move(node));
		node = std::move(next);
	}
	vector<BoundOrderByNode> orders;
	orders.emplace_back(
	    OrderType::ASCENDING, OrderByNullType::NULLS_LAST,
	    make_uniq<BoundColumnRefExpression>(aggregate.groups[key_idx.GetIndex()]->GetReturnType(),
	                                        ColumnBinding(aggregate.group_index, ProjectionIndex(key_idx.GetIndex()))));
	auto topn = make_uniq<LogicalTopN>(std::move(orders), limit_value, offset_value);
	topn->unpruned_offset = limit.unpruned_offset;
	auto cardinality = limit_value;
	if (node->has_estimated_cardinality && node->estimated_cardinality < limit_value) {
		cardinality = node->estimated_cardinality;
	}
	topn->AddChild(std::move(node));
	topn->SetEstimatedCardinality(cardinality);
	unique_ptr<LogicalOperator> result = std::move(topn);
	// the projections are one row in, one row out: they move above the Top-N
	while (!projections.empty()) {
		auto projection = std::move(projections.back());
		projections.pop_back();
		projection->children[0] = std::move(result);
		result = std::move(projection);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Top-N over a grouped aggregate
//===--------------------------------------------------------------------===//
bool GroupByTopKey::TryMatch(LogicalOperator &op, TopKeyMatch &match) {
	auto &topn = op.Cast<LogicalTopN>();
	if (topn.orders.empty() || topn.limit == 0 || topn.limit > MAX_TOP_KEYS ||
	    topn.offset > MAX_TOP_KEYS - topn.limit) {
		return false;
	}
	match.limit = topn.limit + topn.offset;
	// the projections down to the aggregate: a filter (HAVING) or anything else ends the match
	vector<reference<LogicalOperator>> projections;
	reference<LogicalOperator> child = *op.children[0];
	while (child.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		projections.push_back(child);
		child = *child.get().children[0];
	}
	if (child.get().type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &aggregate = child.get().Cast<LogicalAggregate>();
	if (!IsPlainGroupedAggregate(aggregate) ||
	    aggregate.children[0]->type == LogicalOperatorType::LOGICAL_TOP_KEY_FILTER) {
		return false;
	}
	// the longest prefix of the ORDER BY that consists of keys over the group keys
	for (auto &order : topn.orders) {
		unique_ptr<Expression> key;
		if (IsInputColumn(*order.expression)) {
			// forwarded by order-preserving projections only: the group key itself, in the order of the Top-N
			key =
			    ResolveColumn(order.expression->Cast<BoundColumnRefExpression>().Binding(), 0, projections, aggregate);
		}
		if (!key) {
			key = Substitute(*order.expression, 0, projections, aggregate);
		}
		if (!key) {
			break;
		}
		if (!IsInputColumn(*key)) {
			bool is_group_key = false;
			for (auto &group : aggregate.groups) {
				is_group_key = is_group_key || group->Equals(*key);
			}
			// an expression over the group keys is evaluated for every input row: only cheap ones
			if (!is_group_key && ExpressionHeuristics::Cost(*key) > MAX_DERIVED_KEY_COST) {
				break;
			}
		}
		const auto order_type = order.type == OrderType::DESCENDING ? OrderType::DESCENDING : OrderType::ASCENDING;
		const auto null_order = order.null_order == OrderByNullType::NULLS_FIRST ? OrderByNullType::NULLS_FIRST
		                                                                         : OrderByNullType::NULLS_LAST;
		match.prefix.emplace_back(order_type, null_order, std::move(key));
	}
	match.depth = projections.size();
	return !match.prefix.empty();
}

unique_ptr<LogicalOperator> GroupByTopKey::MaterializeKeys(LogicalAggregate &aggregate,
                                                           unique_ptr<LogicalOperator> input,
                                                           vector<BoundOrderByNode> &keys) {
	const auto projection_index = optimizer.binder.GenerateTableIndex();
	input->ResolveOperatorTypes();
	const auto input_bindings = input->GetColumnBindings();
	D_ASSERT(input_bindings.size() == input->types.size());
	// forward every column of the input
	vector<unique_ptr<Expression>> expressions;
	ColumnBindingReplacer replacer;
	for (idx_t i = 0; i < input_bindings.size(); i++) {
		expressions.push_back(make_uniq<BoundColumnRefExpression>(input->types[i], input_bindings[i]));
		replacer.replacement_bindings.emplace_back(input_bindings[i],
		                                           ColumnBinding(projection_index, ProjectionIndex(i)));
	}
	// append the keys that are expressions
	for (auto &key : keys) {
		if (IsInputColumn(*key.expression)) {
			replacer.VisitExpression(&key.expression);
			continue;
		}
		ColumnBinding key_binding(projection_index, ProjectionIndex(expressions.size()));
		auto key_ref = make_uniq<BoundColumnRefExpression>(key.expression->GetReturnType(), key_binding);
		// a group key that is the same expression reads the column instead of computing it a second time
		for (auto &group : aggregate.groups) {
			if (group->Equals(*key.expression)) {
				group = key_ref->Copy();
			}
		}
		expressions.push_back(std::move(key.expression));
		key.expression = std::move(key_ref);
	}
	// the aggregate reads the columns of the projection
	replacer.VisitOperatorBindings(aggregate);
	auto projection = make_uniq<LogicalProjection>(projection_index, std::move(expressions));
	if (input->has_estimated_cardinality) {
		projection->SetEstimatedCardinality(input->estimated_cardinality);
	}
	projection->children.push_back(std::move(input));
	projection->ResolveOperatorTypes();
	return std::move(projection);
}

void GroupByTopKey::Rewrite(LogicalAggregate &aggregate, TopKeyMatch &match) {
	auto input = std::move(aggregate.children[0]);
	bool needs_projection = false;
	for (auto &key : match.prefix) {
		needs_projection = needs_projection || !IsInputColumn(*key.expression);
	}
	if (needs_projection) {
		input = MaterializeKeys(aggregate, std::move(input), match.prefix);
	}
	input->ResolveOperatorTypes();

	auto filter = make_uniq<LogicalTopKeyFilter>(std::move(match.prefix), match.limit);
	if (input->has_estimated_cardinality) {
		filter->SetEstimatedCardinality(input->estimated_cardinality);
	}
	filter->children.push_back(std::move(input));
	filter->ResolveOperatorTypes();

	// the dynamic filter on the first key: for the types that the Top-N supports
	auto &first = filter->orders[0];
	auto &type = first.expression->GetReturnType();
	if (LimitIsSelective(aggregate, filter->limit) &&
	    (TypeIsNumeric(type.InternalType()) || type.id() == LogicalTypeId::VARCHAR)) {
		// the bound is the n-th key itself and the rows of its group pass: always <= (>= if descending)
		const auto comparison_type = first.type == OrderType::ASCENDING ? ExpressionType::COMPARE_LESSTHANOREQUALTO
		                                                                : ExpressionType::COMPARE_GREATERTHANOREQUALTO;
		filter->dynamic_filter = TopN::PushDynamicFilterIntoScans(
		    optimizer.context, *filter->children[0], first.expression->Cast<BoundColumnRefExpression>().Binding(), type,
		    first.null_order == OrderByNullType::NULLS_FIRST, comparison_type);
	}
	aggregate.children[0] = std::move(filter);
}

unique_ptr<LogicalOperator> GroupByTopKey::Optimize(unique_ptr<LogicalOperator> op) {
	if (op->type == LogicalOperatorType::LOGICAL_LIMIT) {
		op = LimitToTopN(std::move(op));
	}
	if (op->type == LogicalOperatorType::LOGICAL_TOP_N) {
		TopKeyMatch match;
		if (TryMatch(*op, match)) {
			reference<LogicalOperator> aggregate = *op->children[0];
			for (idx_t i = 0; i < match.depth; i++) {
				aggregate = *aggregate.get().children[0];
			}
			Rewrite(aggregate.get().Cast<LogicalAggregate>(), match);
		}
	}
	for (auto &child : op->children) {
		child = Optimize(std::move(child));
	}
	return op;
}

} // namespace duckdb
