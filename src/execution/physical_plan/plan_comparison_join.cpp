#include "duckdb/execution/operator/join/physical_blockwise_nl_join.hpp"
#include "duckdb/execution/operator/join/physical_cross_product.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/execution/operator/join/physical_iejoin.hpp"
#include "duckdb/execution/operator/join/physical_join.hpp"
#include "duckdb/execution/operator/join/physical_nested_loop_join.hpp"
#include "duckdb/execution/operator/join/physical_piecewise_merge_join.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/operator/join/physical_recursive_cte_key_join.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/planner/joinside.hpp"

namespace duckdb {
static void RewriteJoinCondition(unique_ptr<Expression> &root_expr, idx_t offset) {
	ExpressionIterator::VisitExpressionMutable<BoundReferenceExpression>(
	    root_expr, [&](BoundReferenceExpression &ref, unique_ptr<Expression> &expr) { ref.IndexMutable() += offset; });
}

static optional_ptr<PhysicalRecursiveCTEStateScan>
FindDirectRecursiveStateScan(PhysicalOperator &op,
                             const unordered_map<TableIndex, RecursiveCTEPlanningInfo> &recursive_cte_planning) {
	if (op.type != PhysicalOperatorType::RECURSIVE_RECURRING_CTE_SCAN) {
		return nullptr;
	}
	for (auto &entry : recursive_cte_planning) {
		for (auto &scan_ref : entry.second.state_scans) {
			if (&scan_ref.get() == &op) {
				return scan_ref.get();
			}
		}
	}
	return nullptr;
}

static unique_ptr<Expression> CreateRecursiveKeyNormalizer(ClientContext &context, const LogicalType &type,
                                                           idx_t reference_idx,
                                                           optional_ptr<bool> normalized = nullptr) {
	unique_ptr<Expression> result = make_uniq<BoundReferenceExpression>(type, reference_idx);
	const auto normalization_applied = ExpressionBinder::PushCollation(context, result, type);
	if (normalized) {
		*normalized = normalization_applied;
	}
	return result;
}

static optional_idx MatchRecursiveStateKey(ClientContext &context, const PhysicalRecursiveCTEStateScan &state_scan,
                                           const Expression &expression) {
	for (idx_t key_idx = 0; key_idx < state_scan.distinct_idx.size(); key_idx++) {
		const auto state_idx = state_scan.distinct_idx[key_idx];
		if (state_idx >= state_scan.GetTypes().size()) {
			return optional_idx();
		}
		auto expected = CreateRecursiveKeyNormalizer(context, state_scan.GetTypes()[state_idx], state_idx);
		if (Expression::Equals(expression, *expected)) {
			return key_idx;
		}
	}
	return optional_idx();
}

static optional_idx MatchRecursiveProbeKey(ClientContext &context, const PhysicalOperator &probe,
                                           const LogicalType &key_type, const Expression &expression) {
	for (idx_t probe_idx = 0; probe_idx < probe.GetTypes().size(); probe_idx++) {
		if (probe.GetTypes()[probe_idx] != key_type) {
			continue;
		}
		auto expected = CreateRecursiveKeyNormalizer(context, key_type, probe_idx);
		if (Expression::Equals(expression, *expected)) {
			return probe_idx;
		}
	}
	return optional_idx();
}

struct RecursiveKeyProbe {
	idx_t state_key_idx;
	idx_t probe_key_idx;
	ExpressionType comparison;
};

static bool TryGetRecursiveKeyProbe(ClientContext &context, LogicalComparisonJoin &op, PhysicalOperator &left,
                                    PhysicalOperator &right, optional_ptr<PhysicalRecursiveCTEStateScan> left_state,
                                    optional_ptr<PhysicalRecursiveCTEStateScan> right_state,
                                    vector<idx_t> &state_key_indices, vector<idx_t> &probe_key_indices,
                                    vector<ExpressionType> &key_comparisons, bool &state_on_left) {
	if (op.join_type != JoinType::INNER || (left_state && right_state)) {
		return false;
	}
	auto state_scan = left_state ? left_state : right_state;
	if (!state_scan) {
		return false;
	}
	state_on_left = left_state != nullptr;
	auto &probe = state_on_left ? right : left;
	if (op.conditions.empty() || op.conditions.size() > state_scan->distinct_idx.size()) {
		return false;
	}

	vector<RecursiveKeyProbe> key_probes;
	for (auto &condition : op.conditions) {
		if (!condition.IsComparison()) {
			return false;
		}
		const auto comparison = condition.GetComparisonType();
		if (comparison != ExpressionType::COMPARE_EQUAL && comparison != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			return false;
		}
		auto &state_expr = state_on_left ? condition.GetLHS() : condition.GetRHS();
		auto &probe_expr = state_on_left ? condition.GetRHS() : condition.GetLHS();
		auto state_key_idx = MatchRecursiveStateKey(context, *state_scan, state_expr);
		if (!state_key_idx.IsValid()) {
			return false;
		}
		const auto key_idx = state_key_idx.GetIndex();
		const auto state_idx = state_scan->distinct_idx[key_idx];
		const auto &key_type = state_scan->GetTypes()[state_idx];
		auto probe_key_idx = MatchRecursiveProbeKey(context, probe, key_type, probe_expr);
		if (!probe_key_idx.IsValid()) {
			return false;
		}
		if (key_idx >= state_scan->key_requires_normalization.size()) {
			return false;
		}
		for (auto &key_probe : key_probes) {
			if (key_probe.state_key_idx == key_idx) {
				return false;
			}
		}
		if (key_type.IsNested()) {
			return false;
		}
		key_probes.push_back({key_idx, probe_key_idx.GetIndex(), comparison});
	}
	std::sort(key_probes.begin(), key_probes.end(), [](const RecursiveKeyProbe &left, const RecursiveKeyProbe &right) {
		return left.state_key_idx < right.state_key_idx;
	});
	for (auto &key_probe : key_probes) {
		state_key_indices.push_back(key_probe.state_key_idx);
		probe_key_indices.push_back(key_probe.probe_key_idx);
		key_comparisons.push_back(key_probe.comparison);
	}
	return true;
}

static vector<unique_ptr<Expression>>
CreateRecursiveKeyProbeNormalizers(ClientContext &context, const PhysicalRecursiveCTEStateScan &state_scan,
                                   const vector<idx_t> &state_key_indices) {
	vector<unique_ptr<Expression>> normalizers;
	bool requires_normalization = false;
	for (idx_t join_key_idx = 0; join_key_idx < state_key_indices.size(); join_key_idx++) {
		const auto state_key_idx = state_key_indices[join_key_idx];
		const auto state_column_idx = state_scan.distinct_idx[state_key_idx];
		const auto &raw_type = state_scan.GetTypes()[state_column_idx];
		bool normalized;
		auto normalizer = CreateRecursiveKeyNormalizer(context, raw_type, join_key_idx, normalized);
		if (normalized != state_scan.key_requires_normalization[state_key_idx] ||
		    normalizer->GetReturnType() != state_scan.hash_key_types[state_key_idx]) {
			throw InternalException("Inconsistent USING KEY probe collation normalization");
		}
		requires_normalization = requires_normalization || normalized;
		normalizers.push_back(std::move(normalizer));
	}
	if (!requires_normalization) {
		normalizers.clear();
	}
	return normalizers;
}

PhysicalOperator &PhysicalPlanGenerator::PlanComparisonJoin(LogicalComparisonJoin &op) {
	// now visit the children
	D_ASSERT(op.children.size() == 2);
	idx_t lhs_cardinality = op.children[0]->EstimateCardinality(context);
	idx_t rhs_cardinality = op.children[1]->EstimateCardinality(context);
	auto &left = CreatePlan(*op.children[0]);
	auto &right = CreatePlan(*op.children[1]);
	left.estimated_cardinality = lhs_cardinality;
	right.estimated_cardinality = rhs_cardinality;
	auto left_state = FindDirectRecursiveStateScan(left, recursive_cte_planning);
	auto right_state = FindDirectRecursiveStateScan(right, recursive_cte_planning);
	vector<idx_t> state_key_indices;
	vector<idx_t> probe_key_indices;
	vector<ExpressionType> key_comparisons;
	bool state_on_left;
	if (TryGetRecursiveKeyProbe(context, op, left, right, left_state, right_state, state_key_indices, probe_key_indices,
	                            key_comparisons, state_on_left)) {
		auto &state_scan = state_on_left ? *left_state : *right_state;
		auto &probe = state_on_left ? right : left;
		auto left_projection_map = PhysicalJoin::FillProjectionMap(left, op.left_projection_map);
		auto right_projection_map = PhysicalJoin::FillProjectionMap(right, op.right_projection_map);
		auto probe_key_normalizers = CreateRecursiveKeyProbeNormalizers(context, state_scan, state_key_indices);
		if (state_key_indices.size() < state_scan.distinct_idx.size()) {
			RecursiveCTEPartialKeySpec new_spec(state_key_indices, state_scan.distinct_idx.size());
			bool found = false;
			for (auto &spec : state_scan.partial_key_index_specs) {
				found = found || spec == new_spec;
			}
			if (!found) {
				state_scan.partial_key_index_specs.push_back(std::move(new_spec));
			}
		}
		return Make<PhysicalRecursiveCTEKeyJoin>(op, probe, state_scan, state_on_left, std::move(state_key_indices),
		                                         std::move(probe_key_indices), std::move(key_comparisons),
		                                         std::move(probe_key_normalizers), std::move(left_projection_map),
		                                         std::move(right_projection_map), op.estimated_cardinality);
	}

	if (op.conditions.empty()) {
		// no conditions: insert a cross product
		return Make<PhysicalCrossProduct>(op.types, left, right, op.estimated_cardinality);
	}

	idx_t has_range = 0;
	bool has_equality = op.HasEquality(has_range);
	bool can_merge = has_range > 0;
	bool can_iejoin = has_range >= 2 && recursive_cte_tables.empty();
	switch (op.join_type) {
	case JoinType::SEMI:
	case JoinType::ANTI:
	case JoinType::MARK:
		can_merge = can_merge && op.conditions.size() == 1;
		break;
	case JoinType::RIGHT_ANTI:
	case JoinType::RIGHT_SEMI:
		can_merge = can_merge && op.conditions.size() == 1;
		can_iejoin = false;
		break;
	default:
		break;
	}

	//	TODO: Extend PWMJ to handle all comparisons and projection maps
	bool prefer_range_joins = Settings::Get<PreferRangeJoinsSetting>(context);
	prefer_range_joins = prefer_range_joins && can_iejoin;
	if (has_equality && !prefer_range_joins) {
		// pass separately to PhysicalHashJoin
		auto &join = Make<PhysicalHashJoin>(op, left, right, std::move(op.conditions), op.join_type,
		                                    op.left_projection_map, op.right_projection_map, std::move(op.mark_types),
		                                    op.estimated_cardinality, std::move(op.filter_pushdown));
		return join;
	}

	// the IE join applies both projection maps, the nested loop join only applies the left one of a MARK join,
	// and the piecewise merge and blockwise joins emit full child layouts
	idx_t nested_loop_join_threshold = Settings::Get<NestedLoopJoinThresholdSetting>(context);
	if (left.estimated_cardinality < nested_loop_join_threshold ||
	    right.estimated_cardinality < nested_loop_join_threshold) {
		can_iejoin = false;
		can_merge = false;
	}

	if (can_merge && can_iejoin) {
		idx_t merge_join_threshold = Settings::Get<MergeJoinThresholdSetting>(context);
		if (left.estimated_cardinality < merge_join_threshold || right.estimated_cardinality < merge_join_threshold) {
			can_iejoin = false;
		}
	}

	if (can_iejoin) {
		return Make<PhysicalIEJoin>(op, left, right, std::move(op.conditions), op.join_type, op.estimated_cardinality,
		                            std::move(op.filter_pushdown));
	}
	if (can_merge) {
		// range join: use piecewise merge join, which emits full child layouts
		auto pruned = ExtractPrunedJoinProjection(op, left, right, true, true);
		auto &join = Make<PhysicalPiecewiseMergeJoin>(op, left, right, std::move(op.conditions), op.join_type,
		                                              op.estimated_cardinality, std::move(op.filter_pushdown));
		return RestorePrunedJoinProjection(op, join, std::move(pruned));
	}
	if (PhysicalNestedLoopJoin::IsSupported(op.conditions, op.join_type)) {
		// inequality join: use nested loop, which only applies the left map of a MARK join
		auto pruned = ExtractPrunedJoinProjection(op, left, right, op.join_type != JoinType::MARK, true);
		D_ASSERT(op.left_projection_map.empty() || op.join_type == JoinType::MARK);
		auto &join = Make<PhysicalNestedLoopJoin>(op, left, right, std::move(op.conditions), op.join_type,
		                                          op.estimated_cardinality, std::move(op.filter_pushdown));
		return RestorePrunedJoinProjection(op, join, std::move(pruned));
	}

	for (auto &cond : op.conditions) {
		if (cond.IsComparison()) {
			RewriteJoinCondition(cond.RightReference(), left.types.size());
		}
	}
	auto pruned = ExtractPrunedJoinProjection(op, left, right, true, true);
	D_ASSERT(op.left_projection_map.empty());
	auto condition = JoinCondition::CreateExpression(std::move(op.conditions));
	auto &join =
	    Make<PhysicalBlockwiseNLJoin>(op, left, right, std::move(condition), op.join_type, op.estimated_cardinality);
	return RestorePrunedJoinProjection(op, join, std::move(pruned));
}

static bool IsPruningMap(const vector<ProjectionIndex> &projection_map, idx_t input_width) {
	if (projection_map.empty()) {
		return false;
	}
	if (projection_map.size() != input_width) {
		return true;
	}
	for (idx_t i = 0; i < projection_map.size(); i++) {
		if (projection_map[i].GetIndex() != i) {
			return true;
		}
	}
	return false;
}

PhysicalPlanGenerator::PrunedJoinProjection
PhysicalPlanGenerator::ExtractPrunedJoinProjection(LogicalComparisonJoin &op, const PhysicalOperator &left,
                                                   const PhysicalOperator &right, bool expand_left, bool expand_right) {
	PrunedJoinProjection info;
	const auto left_width = left.GetTypes().size();
	const auto right_width = right.GetTypes().size();
	const bool emits_left = op.join_type != JoinType::RIGHT_SEMI && op.join_type != JoinType::RIGHT_ANTI;
	const bool emits_right =
	    op.join_type != JoinType::SEMI && op.join_type != JoinType::ANTI && op.join_type != JoinType::MARK;
	info.expanded_left = expand_left && emits_left && IsPruningMap(op.left_projection_map, left_width);
	info.expanded_right = expand_right && emits_right && IsPruningMap(op.right_projection_map, right_width);
	// a full identity map selects everything: drop it for operators that require an empty map
	if (expand_left && !info.expanded_left && !op.left_projection_map.empty()) {
		op.left_projection_map.clear();
	}
	if (expand_right && !info.expanded_right && !op.right_projection_map.empty()) {
		op.right_projection_map.clear();
	}
	if (!info.expanded_left && !info.expanded_right) {
		return info;
	}
	info.active = true;
	info.original_types = op.types;
	if (info.expanded_left) {
		info.left_map = std::move(op.left_projection_map);
		op.left_projection_map.clear();
	}
	if (info.expanded_right) {
		info.right_map = std::move(op.right_projection_map);
		op.right_projection_map.clear();
	}
	// emitted layout: per side either the full input or the original mapped columns,
	// with the MARK column in between where the join type produces one
	vector<LogicalType> emitted;
	idx_t original_pos = 0;
	if (emits_left) {
		if (info.expanded_left) {
			auto &left_types = left.GetTypes();
			emitted.insert(emitted.end(), left_types.begin(), left_types.end());
			original_pos += info.left_map.size();
		} else {
			idx_t count = op.left_projection_map.empty() ? left_width : op.left_projection_map.size();
			emitted.insert(emitted.end(), info.original_types.begin(),
			               info.original_types.begin() + NumericCast<int64_t>(count));
			original_pos += count;
		}
	}
	if (op.join_type == JoinType::MARK) {
		emitted.push_back(info.original_types[original_pos]);
		original_pos++;
	}
	if (emits_right) {
		if (info.expanded_right) {
			auto &right_types = right.GetTypes();
			emitted.insert(emitted.end(), right_types.begin(), right_types.end());
		} else {
			emitted.insert(emitted.end(), info.original_types.begin() + NumericCast<int64_t>(original_pos),
			               info.original_types.end());
		}
	}
	op.types = std::move(emitted);
	return info;
}

PhysicalOperator &PhysicalPlanGenerator::RestorePrunedJoinProjection(LogicalComparisonJoin &op, PhysicalOperator &join,
                                                                     PrunedJoinProjection info) {
	if (!info.active) {
		return join;
	}
	auto &emitted_types = join.GetTypes();
	const bool emits_left = op.join_type != JoinType::RIGHT_SEMI && op.join_type != JoinType::RIGHT_ANTI;
	const bool emits_right =
	    op.join_type != JoinType::SEMI && op.join_type != JoinType::ANTI && op.join_type != JoinType::MARK;
	vector<unique_ptr<Expression>> select_list;
	select_list.reserve(info.original_types.size());
	idx_t emitted_pos = 0;
	if (emits_left) {
		idx_t emitted_left_width;
		if (info.expanded_left) {
			emitted_left_width = join.children[0].get().GetTypes().size();
			for (auto &entry : info.left_map) {
				auto index = entry.GetIndex();
				select_list.push_back(make_uniq<BoundReferenceExpression>(emitted_types[index], index));
			}
		} else {
			emitted_left_width = op.left_projection_map.empty() ? join.children[0].get().GetTypes().size()
			                                                    : op.left_projection_map.size();
			for (idx_t i = 0; i < emitted_left_width; i++) {
				select_list.push_back(make_uniq<BoundReferenceExpression>(emitted_types[i], i));
			}
		}
		emitted_pos += emitted_left_width;
	}
	if (op.join_type == JoinType::MARK) {
		select_list.push_back(make_uniq<BoundReferenceExpression>(emitted_types[emitted_pos], emitted_pos));
		emitted_pos++;
	}
	if (emits_right) {
		if (info.expanded_right) {
			for (auto &entry : info.right_map) {
				auto index = emitted_pos + entry.GetIndex();
				select_list.push_back(make_uniq<BoundReferenceExpression>(emitted_types[index], index));
			}
		} else {
			for (idx_t i = emitted_pos; i < emitted_types.size(); i++) {
				select_list.push_back(make_uniq<BoundReferenceExpression>(emitted_types[i], i));
			}
		}
	}
	D_ASSERT(select_list.size() == info.original_types.size());
	auto &projection =
	    Make<PhysicalProjection>(info.original_types, std::move(select_list), join.estimated_cardinality);
	projection.children.emplace_back(join);
	// callers of CreatePlan read the logical schema afterwards: restore it
	op.types = std::move(info.original_types);
	return projection;
}

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalComparisonJoin &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
		return PlanAsOfJoin(op);
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		return PlanComparisonJoin(op);
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
		return PlanDelimJoin(op);
	default:
		throw InternalException("Unrecognized operator type for LogicalComparisonJoin");
	}
}

} // namespace duckdb
