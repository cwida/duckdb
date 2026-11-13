#include "duckdb/optimizer/filter_pushdown.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/constraints/foreign_key_constraint.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_nullability.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

using Filter = FilterPushdown::Filter;

struct BaseTableColumn {
	optional_ptr<LogicalGet> get;
	idx_t physical_index;
};

struct ForeignKeyJoinColumn {
	ForeignKeyJoinColumn(idx_t foreign_key_index_p, idx_t primary_key_index_p)
	    : foreign_key_index(foreign_key_index_p), primary_key_index(primary_key_index_p) {
	}

	idx_t foreign_key_index;
	idx_t primary_key_index;
};

static bool GetBaseTableColumn(LogicalOperator &op, const Expression &expr, BaseTableColumn &result) {
	if (expr.GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	auto &column_ref = expr.Cast<BoundColumnRefExpression>();
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &projection = op.Cast<LogicalProjection>();
		if (column_ref.Binding().table_index != projection.table_index) {
			return false;
		}
		return GetBaseTableColumn(*projection.children[0], projection.GetExpression(column_ref.Binding()), result);
	}
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		if (column_ref.Binding().table_index != get.table_index) {
			return false;
		}
		auto table = get.GetTable();
		if (!table) {
			return false;
		}
		auto &column_index = get.GetColumnIndex(column_ref.Binding());
		if (!column_index.HasPrimaryIndex() || column_index.HasChildren() || column_index.IsVirtualColumn()) {
			return false;
		}
		result.get = get;
		result.physical_index = table->GetColumn(column_index.ToLogical()).Physical().index;
		return true;
	}
	default:
		return false;
	}
}

static bool MatchesForeignKey(const ForeignKeyInfo &info, const TableCatalogEntry &fk_table,
                              const TableCatalogEntry &pk_table, const vector<ForeignKeyJoinColumn> &join_columns) {
	if (info.type != ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE &&
	    info.type != ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE) {
		return false;
	}
	if (&fk_table.ParentCatalog() != &pk_table.ParentCatalog() ||
	    &fk_table.ParentSchema() != &pk_table.ParentSchema() || info.table != pk_table.name) {
		return false;
	}
	if (info.fk_keys.size() != join_columns.size() || info.pk_keys.size() != join_columns.size()) {
		return false;
	}
	vector<bool> matched(join_columns.size(), false);
	for (idx_t key_idx = 0; key_idx < info.fk_keys.size(); ++key_idx) {
		bool found = false;
		for (idx_t join_idx = 0; join_idx < join_columns.size(); ++join_idx) {
			if (matched[join_idx] || join_columns[join_idx].foreign_key_index != info.fk_keys[key_idx].index ||
			    join_columns[join_idx].primary_key_index != info.pk_keys[key_idx].index) {
				continue;
			}
			matched[join_idx] = true;
			found = true;
			break;
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

static unique_ptr<Expression> CreateForeignKeyNullFilter(LogicalComparisonJoin &join) {
	if (join.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN || join.conditions.empty()) {
		return nullptr;
	}
	optional_ptr<LogicalGet> fk_get;
	optional_ptr<LogicalGet> pk_get;
	vector<ForeignKeyJoinColumn> join_columns;
	join_columns.reserve(join.conditions.size());
	for (const auto &condition : join.conditions) {
		if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
			return nullptr;
		}
		BaseTableColumn fk_column;
		BaseTableColumn pk_column;
		if (!GetBaseTableColumn(*join.children[0], condition.GetLHS(), fk_column) ||
		    !GetBaseTableColumn(*join.children[1], condition.GetRHS(), pk_column)) {
			return nullptr;
		}
		if ((fk_get && fk_get != fk_column.get) || (pk_get && pk_get != pk_column.get)) {
			return nullptr;
		}
		fk_get = fk_column.get;
		pk_get = pk_column.get;
		join_columns.emplace_back(/*foreign_key_index=*/fk_column.physical_index,
		                          /*primary_key_index=*/pk_column.physical_index);
	}
	D_ASSERT(fk_get);
	D_ASSERT(pk_get);
	auto fk_table = fk_get->GetTable();
	auto pk_table = pk_get->GetTable();
	D_ASSERT(fk_table);
	D_ASSERT(pk_table);
	if (pk_get->table_filters.HasFilters()) {
		return nullptr;
	}
	for (const auto &constraint : fk_table->GetConstraints()) {
		if (constraint->type != ConstraintType::FOREIGN_KEY) {
			continue;
		}
		const auto &foreign_key = constraint->Cast<ForeignKeyConstraint>();
		if (!MatchesForeignKey(foreign_key.info, *fk_table, *pk_table, join_columns)) {
			continue;
		}
		unique_ptr<Expression> result;
		for (const auto &condition : join.conditions) {
			auto is_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL,
			                                                  LogicalType {LogicalTypeId::BOOLEAN});
			is_null->GetChildrenMutable().push_back(condition.GetLHS().Copy());
			if (!result) {
				result = std::move(is_null);
			} else {
				result = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(result),
				                                               std::move(is_null));
			}
		}
		return result;
	}
	return nullptr;
}

static void SimplifyNullSafeSemiJoinConditions(ClientContext &context, LogicalComparisonJoin &join) {
	D_ASSERT(join.join_type == JoinType::SEMI);
	NotNullExpressionAnalyzer analyzer(context);
	for (auto &cond : join.conditions) {
		if (!cond.IsComparison() || cond.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			continue;
		}
		// Once a MARK join is reduced to SEMI, a null-safe equality is equivalent to regular equality if either
		// join key is known not to be NULL. Regular equality unlocks the existing runtime-filter infrastructure.
		if (!analyzer.IsNotNull(*join.children[0], cond.GetLHS()) &&
		    !analyzer.IsNotNull(*join.children[1], cond.GetRHS())) {
			continue;
		}
		cond =
		    JoinCondition(cond.LeftReference()->Copy(), cond.RightReference()->Copy(), ExpressionType::COMPARE_EQUAL);
	}
}

unique_ptr<LogicalOperator> FilterPushdown::PushdownMarkJoin(unique_ptr<LogicalOperator> op,
                                                             unordered_set<TableIndex> &left_bindings,
                                                             unordered_set<TableIndex> &right_bindings) {
	auto op_bindings = op->GetColumnBindings();
	auto &join = op->Cast<LogicalJoin>();
	auto &comp_join = op->Cast<LogicalComparisonJoin>();
	D_ASSERT(join.join_type == JoinType::MARK);
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	         op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN || op->type == LogicalOperatorType::LOGICAL_ASOF_JOIN);

	right_bindings.insert(comp_join.mark_index);
	FilterPushdown left_pushdown(optimizer, convert_mark_joins, projection_mode);
	FilterPushdown right_pushdown(optimizer, convert_mark_joins, projection_mode);
#ifdef DEBUG
	bool simplified_mark_join = false;
#endif
	bool added_foreign_key_filter = false;
	// now check the set of filters
	for (idx_t i = 0; i < filters.size(); i++) {
		auto side = JoinSide::GetJoinSide(filters[i]->bindings, left_bindings, right_bindings);
		if (side == JoinSide::LEFT) {
			// bindings match left side: push into left
			left_pushdown.filters.push_back(std::move(filters[i]));
			// erase the filter from the list of filters
			filters.erase_at(i);
			i--;
		} else if (side == JoinSide::RIGHT) {
#ifdef DEBUG
			D_ASSERT(!simplified_mark_join);
#endif
			// this filter references the marker
			// we can turn this into a SEMI join if the filter is on only the marker
			if (filters[i]->filter->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF && convert_mark_joins &&
			    comp_join.convert_mark_to_semi) {
				// filter just references the marker: turn into semi join
#ifdef DEBUG
				simplified_mark_join = true;
#endif
				join.join_type = JoinType::SEMI;
				filters.erase_at(i);
				i--;
				continue;
			}
			// if the filter is on NOT(marker) AND the join conditions are all set to "null_values_are_equal" we can
			// turn this into an ANTI join if all join conditions have null_values_are_equal=true, then the result of
			// the MARK join is always TRUE or FALSE, and never NULL this happens in the case of a correlated EXISTS
			// clause
			if (filters[i]->filter->GetExpressionType() == ExpressionType::OPERATOR_NOT) {
				auto &op_expr = filters[i]->filter->Cast<BoundOperatorExpression>();
				if (op_expr.GetChildren()[0]->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
					auto &marker = op_expr.GetChildren()[0]->Cast<BoundColumnRefExpression>();
					if (!added_foreign_key_filter && marker.Binding().table_index == comp_join.mark_index) {
						auto null_filter = CreateForeignKeyNullFilter(comp_join);
						if (null_filter) {
							auto filter = make_uniq<Filter>(std::move(null_filter));
							filter->ExtractBindings();
							left_pushdown.filters.push_back(std::move(filter));
							added_foreign_key_filter = true;
						}
					}
					// the filter is NOT(marker), check the join conditions
					bool all_null_values_are_equal = true;
					for (auto &cond : comp_join.conditions) {
						if (!cond.IsComparison()) {
							continue;
						}
						if (cond.GetComparisonType() != ExpressionType::COMPARE_DISTINCT_FROM &&
						    cond.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
							all_null_values_are_equal = false;
							break;
						}
					}
					if (all_null_values_are_equal && convert_mark_joins && comp_join.convert_mark_to_semi) {
#ifdef DEBUG
						simplified_mark_join = true;
#endif
						// all null values are equal, convert to ANTI join
						join.join_type = JoinType::ANTI;
						filters.erase_at(i);
						i--;
						continue;
					}
				}
			}
		}
	}
	left_pushdown.RewriteChild(*op, 0);
	right_pushdown.RewriteChild(*op, 1);
	if (join.join_type == JoinType::SEMI) {
		SimplifyNullSafeSemiJoinConditions(GetContext(), comp_join);
	}
	return PushFinalFilters(std::move(op));
}

} // namespace duckdb
