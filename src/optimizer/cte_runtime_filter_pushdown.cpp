#include "duckdb/optimizer/join_filter_pushdown_optimizer.hpp"

#include "duckdb/execution/operator/join/physical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"

namespace duckdb {

static bool ContainsRecursiveCTE(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsRecursiveCTE(*child)) {
			return true;
		}
	}
	return false;
}

void JoinFilterPushdownOptimizer::Optimize(LogicalOperator &op) {
	if (!ContainsRecursiveCTE(op)) {
		CollectCTEFilters(op);
	}
	VisitOperator(op);
}

void JoinFilterPushdownOptimizer::CollectCTEFilters(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		CollectCTEFilters(*op.children[1]);
		GenerateCTEFilters(op.Cast<LogicalMaterializedCTE>());
		CollectCTEFilters(*op.children[0]);
		return;
	}
	for (auto &child : op.children) {
		CollectCTEFilters(*child);
	}
}

static void CollectCTEConsumers(LogicalOperator &op, TableIndex cte_index, vector<reference<LogicalCTERef>> &references,
                                vector<reference<LogicalComparisonJoin>> &joins) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &ref = op.Cast<LogicalCTERef>();
		if (ref.cte_index == cte_index) {
			references.push_back(ref);
		}
	} else if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		joins.push_back(op.Cast<LogicalComparisonJoin>());
	}
	for (auto &child : op.children) {
		CollectCTEConsumers(*child, cte_index, references, joins);
	}
}

struct CTEFilterContribution {
	LogicalComparisonJoin &join;
	idx_t consumer_idx;
	JoinFilterPushdownColumn column;
};

void JoinFilterPushdownOptimizer::GenerateCTEFilters(LogicalMaterializedCTE &cte) {
	vector<reference<LogicalCTERef>> references;
	vector<reference<LogicalComparisonJoin>> joins;
	CollectCTEConsumers(*cte.children[1], cte.table_index, references, joins);
	if (references.size() < 2 || joins.empty()) {
		return;
	}
	cte.children[0]->ResolveOperatorTypes();
	auto bindings = cte.children[0]->GetColumnBindings();
	unordered_map<TableIndex, idx_t> consumer_indexes;
	for (auto &ref : references) {
		auto &consumer = ref.get();
		if (consumer.is_recurring || consumer.correlated_columns != 0 ||
		    consumer.chunk_types != cte.children[0]->types ||
		    !consumer_indexes.emplace(consumer.table_index, consumer_indexes.size()).second) {
			return;
		}
	}

	reference_map_t<LogicalGet, map<ProjectionIndex, vector<CTEFilterContribution>>> contributions;
	for (auto &join_ref : joins) {
		auto &join = join_ref.get();
		if (!CanGenerateJoinFilters(join)) {
			continue;
		}
		PhysicalComparisonJoin::ReorderConditions(join.conditions);
		vector<JoinFilterPushdownColumn> columns;
		for (idx_t i = 0; i < join.conditions.size(); i++) {
			auto &condition = join.conditions[i];
			JoinFilterPushdownColumn column;
			if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL ||
			    !JoinFilterPushdownUtil::PushdownJoinFilterExpression(condition.GetLHS(), column)) {
				continue;
			}
			column.join_filter_idx = i;
			columns.push_back(std::move(column));
		}
		reference_map_t<LogicalCTERef, vector<JoinFilterPushdownColumn>> targets;
		GetCTEFilterTargets(*join.children[0], std::move(columns), targets);
		for (auto &target : targets) {
			auto &ref = target.first.get();
			auto consumer = consumer_indexes.find(ref.table_index);
			if (ref.cte_index != cte.table_index || consumer == consumer_indexes.end()) {
				continue;
			}
			for (auto &column : target.second) {
				column.probe_column_index = bindings.at(column.probe_column_index.column_index.GetIndex());
				vector<PushdownFilterTarget> scans;
				GetPushdownFilterTargets(*cte.children[0], {std::move(column)}, scans);
				for (auto &scan : scans) {
					for (auto &scan_column : scan.columns) {
						auto index = scan_column.probe_column_index.column_index;
						contributions[scan.get][index].push_back({join, consumer->second, std::move(scan_column)});
					}
				}
			}
		}
	}
	for (auto &scan : contributions) {
		auto &get = scan.first.get();
		for (auto &entry : scan.second) {
			vector<bool> covered(references.size(), false);
			for (auto &contribution : entry.second) {
				covered[contribution.consumer_idx] = true;
			}
			if (std::find(covered.begin(), covered.end(), false) != covered.end()) {
				continue;
			}
			if (!get.dynamic_filters) {
				get.dynamic_filters = make_shared_ptr<DynamicTableFilterSet>();
			}
			auto alternatives = get.dynamic_filters->AddDisjunction(entry.first, references.size());
			for (auto &contribution : entry.second) {
				JoinFilterPushdownFilter filter;
				filter.dynamic_filters = alternatives[contribution.consumer_idx];
				filter.columns.push_back(std::move(contribution.column));
				filter.estimated_scan_cardinality = get.has_estimated_cardinality ? get.estimated_cardinality : 0;
				cte_filters[contribution.join].push_back(std::move(filter));
			}
		}
	}
}

} // namespace duckdb
