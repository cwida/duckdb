#include "duckdb/optimizer/cte_join_filter_collector.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

CTEJoinFilterCollector::CTEJoinFilterCollector(const unordered_map<TableIndex, TableIndex> &targets)
    : join_targets(targets) {
}

vector<CTEJoinFilter> CTEJoinFilterCollector::Collect(LogicalOperator &op,
                                                      const unordered_map<TableIndex, TableIndex> &targets) {
	CTEJoinFilterCollector collector(targets);
	collector.VisitOperator(op);
	return std::move(collector.join_filters);
}

static bool CTEJoinKeyExpressionIsSafe(const Expression &expr) {
	if (expr.IsVolatile() || expr.CanThrow() || ExpressionBarrier::Contains(expr)) {
		return false;
	}
	bool safe = true;
	ExpressionIterator::VisitExpression<BoundFunctionExpression>(
	    expr, [&](const BoundFunctionExpression &func) { safe &= !func.Function().RequiresOrderedExecution(); });
	return safe;
}

static optional_ptr<const BoundColumnRefExpression> GetSingleColumnRef(const Expression &expr) {
	optional_ptr<const BoundColumnRefExpression> result;
	idx_t count = 0;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &ref) {
		count++;
		result = &ref;
	});
	return count == 1 ? result : nullptr;
}

static void ReplaceCTEJoinKeyColumnRef(unique_ptr<Expression> &expr, unique_ptr<Expression> replacement) {
	ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
	    expr, [&](BoundColumnRefExpression &, unique_ptr<Expression> &slot) {
		    D_ASSERT(replacement);
		    slot = std::move(replacement);
	    });
}

CTEJoinFilterKey CTEJoinFilterKey::Copy() const {
	return CTEJoinFilterKey {expression->Copy(), column};
}

unique_ptr<Expression> CTEJoinFilterKey::BindExpression(const LogicalType &type, ColumnBinding binding) const {
	auto result = expression->Copy();
	ReplaceCTEJoinKeyColumnRef(result, make_uniq<BoundColumnRefExpression>(type, binding));
	return result;
}

static bool CanPassCTEJoinFilter(LogicalOperator &op) {
	bool safe = true;
	LogicalOperatorVisitor::EnumerateExpressions(
	    op, [&](unique_ptr<Expression> *expr) { safe &= CTEJoinKeyExpressionIsSafe(**expr); });
	return safe;
}

//! Rebind a key to the expression its column maps to in a child operator
static bool MapCTEJoinKey(CTEJoinFilterCollector::CTEJoinKey &key, const Expression &mapped) {
	if (!CTEJoinKeyExpressionIsSafe(mapped)) {
		return false;
	}
	auto column_ref = GetSingleColumnRef(mapped);
	if (!column_ref) {
		return false;
	}
	key.binding = column_ref->Binding();
	ReplaceCTEJoinKeyColumnRef(key.expression, mapped.Copy());
	return true;
}

static bool ContainsCTEJoinKeys(LogicalOperator &op, const vector<CTEJoinFilterCollector::CTEJoinKey> &keys) {
	auto bindings = op.GetColumnBindings();
	for (auto &key : keys) {
		if (std::find(bindings.begin(), bindings.end(), key.binding) == bindings.end()) {
			return false;
		}
	}
	return true;
}

static bool MapCTEJoinProjection(LogicalProjection &projection, vector<CTEJoinFilterCollector::CTEJoinKey> &keys) {
	for (auto &key : keys) {
		if (key.binding.table_index != projection.table_index) {
			return false;
		}
		if (!MapCTEJoinKey(key, projection.GetExpression(key.binding))) {
			return false;
		}
	}
	return true;
}

//! Group columns of an aggregate carry the distinct values of their input expressions
static bool MapCTEJoinAggregate(LogicalAggregate &aggregate, vector<CTEJoinFilterCollector::CTEJoinKey> &keys) {
	for (auto &key : keys) {
		if (key.binding.table_index != aggregate.group_index) {
			return false;
		}
		for (auto &grouping_set : aggregate.grouping_sets) {
			if (!grouping_set.count(key.binding.column_index)) {
				return false;
			}
		}
		if (!MapCTEJoinKey(key, aggregate.GetGroupExpression(key.binding.column_index))) {
			return false;
		}
	}
	return true;
}

static optional_ptr<LogicalCTERef> FindCTEJoinSource(LogicalOperator &op,
                                                     vector<CTEJoinFilterCollector::CTEJoinKey> &keys) {
	if (!ContainsCTEJoinKeys(op, keys)) {
		return nullptr;
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_CTE_REF: {
		auto &ref = op.Cast<LogicalCTERef>();
		return ref.is_recurring ? nullptr : &ref;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION:
		if (!MapCTEJoinProjection(op.Cast<LogicalProjection>(), keys)) {
			return nullptr;
		}
		break;
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		if (!MapCTEJoinAggregate(op.Cast<LogicalAggregate>(), keys)) {
			return nullptr;
		}
		break;
	case LogicalOperatorType::LOGICAL_DISTINCT:
		if (op.Cast<LogicalDistinct>().distinct_type != DistinctType::DISTINCT) {
			return nullptr;
		}
		break;
	case LogicalOperatorType::LOGICAL_FILTER:
		break;
	default:
		return nullptr;
	}
	return FindCTEJoinSource(*op.children[0], keys);
}

static vector<CTEJoinFilterCollector::CTEJoinKey>
CopyCTEJoinKeys(const vector<CTEJoinFilterCollector::CTEJoinKey> &keys) {
	vector<CTEJoinFilterCollector::CTEJoinKey> result;
	result.reserve(keys.size());
	for (auto &key : keys) {
		result.push_back(CTEJoinFilterCollector::CTEJoinKey {key.expression->Copy(), key.binding});
	}
	return result;
}

static bool MakeCTEJoinKey(const Expression &expr, CTEJoinFilterCollector::CTEJoinKey &key) {
	if (!CTEJoinKeyExpressionIsSafe(expr)) {
		return false;
	}
	auto column_ref = GetSingleColumnRef(expr);
	if (!column_ref) {
		return false;
	}
	key.expression = expr.Copy();
	key.binding = column_ref->Binding();
	return true;
}

//! Row-deterministic, side-effect free subtrees that can be re-executed as a copy
static bool CanCopyCTEJoinSource(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET:
		if (!op.Cast<LogicalGet>().GetTable()) {
			return false;
		}
		break;
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_PROJECTION:
		break;
	default:
		return false;
	}
	if (!CanPassCTEJoinFilter(op)) {
		return false;
	}
	for (auto &child : op.children) {
		if (!CanCopyCTEJoinSource(*child)) {
			return false;
		}
	}
	return true;
}

void CTEJoinFilterCollector::VisitOperator(LogicalOperator &op) {
	for (auto &child : op.children) {
		VisitOperator(*child);
	}
	if (op.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	    op.Cast<LogicalComparisonJoin>().join_type != JoinType::INNER || !CanPassCTEJoinFilter(op)) {
		return;
	}
	auto &join = op.Cast<LogicalComparisonJoin>();
	vector<CTEJoinKey> left_keys, right_keys;
	vector<ExpressionType> comparisons;
	for (auto &condition : join.conditions) {
		if (!condition.IsComparison() || (condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL &&
		                                  condition.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM)) {
			continue;
		}
		CTEJoinKey left_key, right_key;
		if (!MakeCTEJoinKey(condition.GetLHS(), left_key) || !MakeCTEJoinKey(condition.GetRHS(), right_key)) {
			continue;
		}
		left_keys.push_back(std::move(left_key));
		right_keys.push_back(std::move(right_key));
		comparisons.push_back(condition.GetComparisonType());
	}
	if (comparisons.empty()) {
		return;
	}
	for (idx_t side = 0; side < 2; side++) {
		auto source_keys = CopyCTEJoinKeys(side == 0 ? left_keys : right_keys);
		auto source_ref = FindCTEJoinSource(*op.children[side], source_keys);
		if (source_ref) {
			CTEJoinSource source {source_ref.get(), nullptr, 0};
			PushFilter(*op.children[1 - side], source, source_keys, CopyCTEJoinKeys(side == 0 ? right_keys : left_keys),
			           comparisons);
			continue;
		}
		if (!CanCopyCTEJoinSource(*op.children[side])) {
			continue;
		}
		// Rebind the keys to output positions of the copyable subtree
		source_keys = CopyCTEJoinKeys(side == 0 ? left_keys : right_keys);
		auto child_bindings = op.children[side]->GetColumnBindings();
		bool found_keys = true;
		for (auto &key : source_keys) {
			auto entry = std::find(child_bindings.begin(), child_bindings.end(), key.binding);
			if (entry == child_bindings.end()) {
				found_keys = false;
				break;
			}
			key.binding =
			    ColumnBinding(TableIndex(0), ProjectionIndex(NumericCast<idx_t>(entry - child_bindings.begin())));
		}
		if (!found_keys) {
			continue;
		}
		CTEJoinSource source {nullptr, &op, side};
		PushFilter(*op.children[1 - side], source, source_keys, CopyCTEJoinKeys(side == 0 ? right_keys : left_keys),
		           comparisons);
	}
}

void CTEJoinFilterCollector::PushFilter(LogicalOperator &op, const CTEJoinSource &source,
                                        const vector<CTEJoinKey> &source_keys, vector<CTEJoinKey> target_keys,
                                        const vector<ExpressionType> &comparisons) {
	if (!ContainsCTEJoinKeys(op, target_keys) || !CanPassCTEJoinFilter(op)) {
		return;
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_CTE_REF:
		AddFilter(source, op.Cast<LogicalCTERef>(), source_keys, target_keys, comparisons);
		return;
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
		PushFilter(*op.children[1], source, source_keys, std::move(target_keys), comparisons);
		return;
	case LogicalOperatorType::LOGICAL_PROJECTION:
		if (!MapCTEJoinProjection(op.Cast<LogicalProjection>(), target_keys)) {
			return;
		}
		break;
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_ORDER_BY:
		break;
	case LogicalOperatorType::LOGICAL_DISTINCT:
		if (op.Cast<LogicalDistinct>().distinct_type != DistinctType::DISTINCT) {
			return;
		}
		break;
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		if (!MapCTEJoinAggregate(op.Cast<LogicalAggregate>(), target_keys)) {
			return;
		}
		break;
	case LogicalOperatorType::LOGICAL_UNION: {
		auto bindings = op.GetColumnBindings();
		for (auto &child : op.children) {
			auto child_bindings = child->GetColumnBindings();
			auto child_keys = CopyCTEJoinKeys(target_keys);
			for (auto &key : child_keys) {
				auto position =
				    NumericCast<idx_t>(std::find(bindings.begin(), bindings.end(), key.binding) - bindings.begin());
				key.binding = child_bindings[position];
			}
			PushFilter(*child, source, source_keys, std::move(child_keys), comparisons);
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.join_type != JoinType::INNER && join.join_type != JoinType::LEFT && join.join_type != JoinType::SEMI &&
		    join.join_type != JoinType::ANTI && join.join_type != JoinType::MARK) {
			return;
		}
		if (ContainsCTEJoinKeys(*op.children[0], target_keys)) {
			PushFilter(*op.children[0], source, source_keys, CopyCTEJoinKeys(target_keys), comparisons);
		}
		if (join.join_type == JoinType::INNER && ContainsCTEJoinKeys(*op.children[1], target_keys)) {
			PushFilter(*op.children[1], source, source_keys, std::move(target_keys), comparisons);
		}
		return;
	}
	default:
		return;
	}
	PushFilter(*op.children[0], source, source_keys, std::move(target_keys), comparisons);
}

void CTEJoinFilterCollector::AddFilter(const CTEJoinSource &source, const LogicalCTERef &target,
                                       const vector<CTEJoinKey> &source_keys, const vector<CTEJoinKey> &target_keys,
                                       const vector<ExpressionType> &comparisons) {
	D_ASSERT(!comparisons.empty() && source_keys.size() == comparisons.size() &&
	         target_keys.size() == comparisons.size());
	auto target_entry = join_targets.find(target.cte_index);
	if (target.is_recurring || target_entry == join_targets.end() || target_entry->second != target.table_index) {
		return;
	}
	CTEJoinFilter filter;
	if (source.ref) {
		if (source.ref->is_recurring || source.ref->cte_index == target.cte_index) {
			return;
		}
		filter.source = source.ref->cte_index;
	} else {
		filter.source_parent = source.parent;
		filter.source_child = source.child_index;
	}
	filter.target = target.cte_index;
	filter.target_scan = target.table_index;
	filter.comparisons = comparisons;
	for (idx_t i = 0; i < comparisons.size(); i++) {
		D_ASSERT(!source.ref || source_keys[i].binding.table_index == source.ref->table_index);
		D_ASSERT(target_keys[i].binding.table_index == target.table_index);
		filter.source_keys.push_back(
		    CTEJoinFilterKey {source_keys[i].expression->Copy(), ProjectionIndex(source_keys[i].binding.column_index)});
		filter.target_keys.push_back(
		    CTEJoinFilterKey {target_keys[i].expression->Copy(), ProjectionIndex(target_keys[i].binding.column_index)});
	}
	auto keys_match = [](const vector<CTEJoinFilterKey> &left, const vector<CTEJoinFilterKey> &right) {
		for (idx_t i = 0; i < left.size(); i++) {
			if (left[i].column != right[i].column || !left[i].expression->Equals(*right[i].expression)) {
				return false;
			}
		}
		return true;
	};
	for (auto &existing : join_filters) {
		if ((filter.source_parent
		         ? existing.source_parent == filter.source_parent && existing.source_child == filter.source_child
		         : !existing.source_parent && existing.source == filter.source) &&
		    existing.target == filter.target && existing.target_scan == filter.target_scan &&
		    existing.comparisons == filter.comparisons && existing.source_keys.size() == filter.source_keys.size() &&
		    keys_match(existing.source_keys, filter.source_keys) &&
		    keys_match(existing.target_keys, filter.target_keys)) {
			return;
		}
	}
	join_filters.push_back(std::move(filter));
}

} // namespace duckdb
