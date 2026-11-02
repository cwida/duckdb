#include "duckdb/optimizer/apply_functional_dependencies.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_binder/base_select_binder.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

void ApplyFunctionalDependencies::VisitLogicalGet(LogicalGet &get) {
	VisitOperatorChildren(get);

	auto table = get.GetTable();
	if (!table || !table->IsDuckTable() || !get.projected_input.empty() || get.GetColumnIds().empty()) {
		return;
	}
	unordered_map<idx_t, ColumnBinding> column_bindings;
	for (auto &binding : get.GetColumnBindings()) {
		auto &column_index = get.GetColumnIndex(binding);
		if (!column_index.HasPrimaryIndex() || column_index.HasChildren() || column_index.IsVirtualColumn()) {
			continue;
		}
		column_bindings.emplace(column_index.GetPrimaryIndex(), binding);
	}
	auto &columns = table->GetColumns();
	auto &constraints = table->GetConstraints();
	unordered_set<idx_t> not_null_columns;
	for (auto &constraint : constraints) {
		if (constraint->type == ConstraintType::NOT_NULL) {
			not_null_columns.insert(constraint->Cast<NotNullConstraint>().index.index);
		}
	}
	for (auto &constraint : constraints) {
		if (constraint->type != ConstraintType::UNIQUE) {
			continue;
		}
		auto &unique = constraint->Cast<UniqueConstraint>();
		if (unique.IsDeferred()) {
			continue;
		}
		auto &names = unique.GetColumnNames();
		if (!unique.HasIndex() && !std::all_of(names.begin(), names.end(),
		                                       [&](const Identifier &name) { return columns.ColumnExists(name); })) {
			continue;
		}
		column_binding_set_t key;
		bool usable = true;
		for (auto &column : unique.GetLogicalIndexes(columns)) {
			auto entry = column_bindings.find(column.index);
			if (entry == column_bindings.end() ||
			    (!unique.IsPrimaryKey() && not_null_columns.find(column.index) == not_null_columns.end())) {
				usable = false;
				break;
			}
			key.insert(entry->second);
		}
		if (usable && !key.empty()) {
			result.push_back(std::move(key));
		}
	}
}

void ApplyFunctionalDependencies::VisitLogicalAggregate(LogicalAggregate &aggr) {
	VisitOperatorChildren(aggr);

	//	TODO: use child FDs to simplify groupings.
	result.clear();

	if (aggr.groups.empty() || aggr.grouping_sets.size() > 1 || !aggr.grouping_functions.empty()) {
		return;
	}
	column_binding_set_t key;
	if (aggr.grouping_sets.empty()) {
		for (auto group_idx : ProjectionIndex::GetIndexes(aggr.groups.size())) {
			key.insert(ColumnBinding(aggr.group_index, group_idx));
		}
	} else {
		for (auto group_idx : aggr.grouping_sets[0]) {
			if (group_idx.GetIndex() >= aggr.groups.size()) {
				return;
			}
			key.insert(ColumnBinding(aggr.group_index, group_idx));
		}
	}
	if (!key.empty()) {
		result.push_back(std::move(key));
	}
}

void ApplyFunctionalDependencies::VisitLogicalDistinct(LogicalDistinct &distinct) {
	VisitOperatorChildren(distinct);

	//	TODO: use child FDs to simplify groupings.
	result.clear();

	column_binding_set_t key;
	bool all_columns = true;
	for (auto &target : distinct.distinct_targets) {
		if (target->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			all_columns = false;
			break;
		}
		key.insert(target->Cast<BoundColumnRefExpression>().Binding());
	}
	if (all_columns && !key.empty()) {
		result.push_back(std::move(key));
	}
}

void ApplyFunctionalDependencies::VisitLogicalProjection(LogicalProjection &projection) {
	VisitOperatorChildren(projection);

	vector<column_binding_set_t> child_sets;
	child_sets.swap(result);
	if (child_sets.empty()) {
		return;
	}
	column_binding_map_t<ColumnBinding> forwarded;
	for (auto expr_idx : ProjectionIndex::GetIndexes(projection.expressions.size())) {
		auto &expr = *projection.expressions[expr_idx];
		if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			forwarded.emplace(expr.Cast<BoundColumnRefExpression>().Binding(),
			                  ColumnBinding(projection.table_index, expr_idx));
		}
	}
	for (auto &child_set : child_sets) {
		column_binding_set_t key;
		bool complete = true;
		for (auto &binding : child_set) {
			auto entry = forwarded.find(binding);
			if (entry == forwarded.end()) {
				complete = false;
				break;
			}
			key.insert(entry->second);
		}
		if (complete) {
			result.push_back(std::move(key));
		}
	}
}

void ApplyFunctionalDependencies::VisitLogicalOrder(LogicalOrder &order) {
	VisitOperatorChildren(order);

	VisitOrderBys(order.orders);
}

void ApplyFunctionalDependencies::VisitOrderBys(vector<BoundOrderByNode> &orders) const {
	if (orders.size() < 2 || read_only) {
		return;
	}

	//	Remove orderings that are FD on the prefix
	vector<reference<Expression>> refs;
	refs.emplace_back(*orders[0].expression);
	for (idx_t i = 1; i < orders.size();) {
		auto &expr = orders[i].expression;
		if (BaseSelectBinder::IsFunctionallyDependent(expr, refs)) {
			orders.erase(orders.begin() + NumericCast<int64_t>(i));
			continue;
		}
		refs.emplace_back(*expr);
		++i;
	}

	//	Remove trailing orderings that are FD on a primary key
	if (result.empty()) {
		return;
	}

	column_binding_set_t prefix;
	for (idx_t i = 0; i + 1 < orders.size(); i++) {
		auto &expr = *orders[i].expression;
		if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			continue;
		}
		prefix.insert(expr.Cast<BoundColumnRefExpression>().Binding());
		for (auto &unique_set : result) {
			bool covered = true;
			for (auto &binding : unique_set) {
				if (prefix.find(binding) == prefix.end()) {
					covered = false;
					break;
				}
			}
			if (!covered) {
				continue;
			}
			orders.erase(orders.begin() + NumericCast<int64_t>(i + 1), orders.end());
			return;
		}
	}
}

void ApplyFunctionalDependencies::VisitPartitioning(vector<unique_ptr<Expression>> &partitions) const {
	if (read_only) {
		return;
	}

	//	Remove partitioning keys that are FD on the others
	for (idx_t i = 0; i < partitions.size();) {
		vector<reference<Expression>> refs;
		for (idx_t j = 0; j < partitions.size(); ++j) {
			if (i != j) {
				refs.emplace_back(*partitions[j]);
			}
		}
		if (BaseSelectBinder::IsFunctionallyDependent(partitions[i], refs)) {
			partitions.erase(partitions.begin() + NumericCast<int64_t>(i));
		} else {
			++i;
		}
	}

	//	Extract the partitioning references
	column_binding_set_t partition_bindings;
	for (auto &expr : partitions) {
		if (expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			continue;
		}
		partition_bindings.insert(expr->Cast<BoundColumnRefExpression>().Binding());
	}

	//	Find the smallest unique set that is covered by the bindings
	optional_idx smallest;
	for (idx_t i = 0; i < result.size(); ++i) {
		auto &unique_set = result.at(i);
		bool covered = true;
		for (auto &binding : unique_set) {
			if (!partition_bindings.count(binding)) {
				covered = false;
				break;
			}
		}
		if (!covered) {
			continue;
		}
		if (!smallest.IsValid() || unique_set.size() < result.at(smallest.GetIndex()).size()) {
			smallest = i;
		}
	}
	if (smallest.IsValid()) {
		//	Replace the partitioning with the unique bindings
		auto &unique_set = result.at(smallest.GetIndex());
		vector<unique_ptr<Expression>> reduced;
		for (auto &expr : partitions) {
			if (expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
				continue;
			}
			const auto &ref = expr->Cast<BoundColumnRefExpression>();
			if (!unique_set.count(ref.Binding())) {
				continue;
			}
			reduced.emplace_back(ref.Copy());
		}
		std::swap(partitions, reduced);
	}
}

void ApplyFunctionalDependencies::VisitWindowExpression(BoundWindowExpression &wexpr) const {
	//	Equivalence 1: Reduce partitioning to minimal functional dependency
	VisitPartitioning(wexpr.PartitionsMutable());

	//	Equivalence 2: Truncate ordering at minimal functional dependency
	VisitOrderBys(wexpr.OrderByMutable());

	//	Equivalence 3: Remove a single ORDER BY if it is FD on the partitioning
	//	Note that this implies that the ordering expression is constant on the partition,
	//	so there is only one peer group, which is the same as having no ORDER BY clause.
	auto &partition_bys = wexpr.Partitions();
	auto &order_bys = wexpr.OrderByMutable();
	if (!read_only && !partition_bys.empty() && order_bys.size() == 1) {
		vector<reference<Expression>> refs;
		for (auto &arg : partition_bys) {
			refs.emplace_back(*arg);
		}
		if (BaseSelectBinder::IsFunctionallyDependent(order_bys[0].expression, refs)) {
			order_bys.clear();
		}
	}
}

void ApplyFunctionalDependencies::VisitExpression(unique_ptr<Expression> *expression) {
	switch ((*expression)->GetExpressionClass()) {
	case ExpressionClass::BOUND_WINDOW:
		VisitWindowExpression((*expression)->Cast<BoundWindowExpression>());
		break;
	default:
		break;
	}
}

void ApplyFunctionalDependencies::VisitOperator(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_ORDER_BY:
		VisitLogicalOrder(op.Cast<LogicalOrder>());
		break;
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_LIMIT:
		VisitOperatorChildren(op);
		break;
	case LogicalOperatorType::LOGICAL_WINDOW:
		VisitOperatorChildren(op);
		VisitOperatorExpressions(op);
		//	Windowing only adds bindings
		break;
	case LogicalOperatorType::LOGICAL_PROJECTION:
		VisitLogicalProjection(op.Cast<LogicalProjection>());
		break;
	case LogicalOperatorType::LOGICAL_GET:
		VisitLogicalGet(op.Cast<LogicalGet>());
		break;
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		VisitLogicalAggregate(op.Cast<LogicalAggregate>());
		break;
	case LogicalOperatorType::LOGICAL_DISTINCT:
		VisitLogicalDistinct(op.Cast<LogicalDistinct>());
		break;
	default:
		VisitOperatorChildren(op);
		//	Clear the keys because we don't know if the operator preserves them.
		result.clear();
		break;
	}
}

} // namespace duckdb
