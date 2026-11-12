#include "duckdb/optimizer/join_order/cost_model.hpp"
#include "duckdb/optimizer/build_probe_side_optimizer.hpp"
#include "duckdb/optimizer/join_order/join_order_operator.hpp"
#include "duckdb/optimizer/join_order/relation_manager.hpp"
#include "duckdb/optimizer/join_order/join_predicate.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

CostModel::CostModel(CardinalityEstimator &cardinality_estimator) : cardinality_estimator(cardinality_estimator) {
}

CardinalityEstimator &CostModel::GetCardinalityEstimator() {
	return cardinality_estimator;
}

double CostModel::ComputeCost(const DPJoinNode &left, const DPJoinNode &right,
                              optional_ptr<JoinOrderOperator> join_operator, double output_rows) {
	auto input_cost =
	    join_operator && join_operator->type == JoinOrderOperatorType::LEFT ? right.cardinality.GetDouble() : 0;
	return left.cost + right.cost + input_cost + output_rows;
}

void CostModel::InitializeRecursiveMemory(ClientContext &context, RelationManager &relations) {
	if (!relations.HasRecursiveRelations() || Settings::Get<PreferRangeJoinsSetting>(context)) {
		return;
	}
	for (idx_t i = 0; i < relations.NumRelations(); i++) {
		if (relations.GetRelation(RelationIndex(i)).recursive_properties.build.correlation_sensitive) {
			return;
		}
	}
	recursive_relations = relations;
	memory_budget = static_cast<double>(BufferManager::GetBufferManager(context).GetOperatorMemoryLimit());
}

double CostModel::GetBuildSize(const DPJoinNode &node) {
	auto entry = row_widths.find(node.set);
	if (entry == row_widths.end()) {
		vector<LogicalType> types;
		for (idx_t i = 0; i < node.set.count; i++) {
			auto &relation = recursive_relations->GetRelation(node.set.relations[i]);
			types.insert(types.end(), relation.op.types.begin(), relation.op.types.end());
		}
		entry = row_widths.emplace(node.set, BuildProbeSideOptimizer::GetBuildSize(std::move(types), 1)).first;
	}
	return entry->second * node.cardinality.GetDouble();
}

static bool HasHashKey(const DPJoinNode &node, const DPJoinNode &left, const DPJoinNode &right) {
	for (auto &predicate_ref : node.predicates) {
		auto &predicate = predicate_ref.get();
		if (predicate.GetComparisonType() != ExpressionType::COMPARE_EQUAL &&
		    predicate.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			continue;
		}
		auto lhs = predicate.GetLeftSetOptional();
		auto rhs = predicate.GetRightSetOptional();
		if (!lhs || !rhs || lhs->Empty() || rhs->Empty()) {
			continue;
		}
		if ((JoinRelationSet::IsSubset(left.set, *lhs) && JoinRelationSet::IsSubset(right.set, *rhs)) ||
		    (JoinRelationSet::IsSubset(right.set, *lhs) && JoinRelationSet::IsSubset(left.set, *rhs))) {
			return true;
		}
	}
	return false;
}

void CostModel::UpdateBuildMemory(DPJoinNode &result, const DPJoinNode &left, const DPJoinNode &right) {
	auto &properties = result.recursive_properties;
	const auto &left_properties = left.recursive_properties;
	const auto &right_properties = right.recursive_properties;
	properties.depends_on_recursive_input =
	    left_properties.depends_on_recursive_input || right_properties.depends_on_recursive_input;
	properties.build.correlation_sensitive =
	    left_properties.build.correlation_sensitive || right_properties.build.correlation_sensitive;
	properties.build.repeatable = left_properties.build.repeatable && right_properties.build.repeatable;
	for (auto &predicate : result.predicates) {
		properties.build.repeatable &= !predicate.get().GetFilter().filter->IsVolatile();
	}
	if (!recursive_relations) {
		return;
	}
	result.retained_build_size = left.retained_build_size + right.retained_build_size;
	double build_size = 0;
	if (HasHashKey(result, left, right)) {
		optional_ptr<const DPJoinNode> build;
		if (CanPlaceRecursiveProbe(JoinType::INNER, left_properties, right_properties)) {
			build = right;
		} else if (CanPlaceRecursiveProbe(JoinType::INNER, right_properties, left_properties)) {
			build = left;
		} else {
			build = GetBuildSize(left) < GetBuildSize(right) ? left : right;
		}
		build_size = GetBuildSize(*build);
		if (!build->recursive_properties.depends_on_recursive_input && build->recursive_properties.build.repeatable) {
			result.retained_build_size += build_size;
		}
	}
	result.peak_build_size = build_size + MaxValue(left.peak_build_size + right.retained_build_size,
	                                               right.peak_build_size + left.retained_build_size);
}

JoinPlanCost CostModel::GetCost(const DPJoinNode &node) const {
	// Prefer plans whose estimated live hash builds fit the configured budget.
	auto excess = recursive_relations ? MaxValue(0.0, node.peak_build_size - memory_budget) : 0;
	return {node.cost, excess};
}

} // namespace duckdb
