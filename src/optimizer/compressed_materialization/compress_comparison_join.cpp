#include "duckdb/optimizer/compressed_materialization.hpp"
#include "duckdb/function/cast/cast_statistics.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/storage/statistics/variant_stats.hpp"

namespace duckdb {

static void PopulateBindingMap(CompressedMaterializationInfo &info, const vector<ColumnBinding> &bindings_out,
                               const vector<LogicalType> &types, LogicalOperator &op_in) {
	const auto bindings_in = op_in.GetColumnBindings();
	for (const auto &binding : bindings_in) {
		// Joins do not change bindings, input binding is output binding
		for (idx_t col_idx_out = 0; col_idx_out < bindings_out.size(); col_idx_out++) {
			const auto &binding_out = bindings_out[col_idx_out];
			if (binding_out == binding) {
				// This one is projected out, add it to map
				info.binding_map.emplace(binding, CMBindingInfo(binding_out, types[col_idx_out]));
			}
		}
	}
}

#ifndef DEBUG
static bool HasVariantType(LogicalOperator &op) {
	for (const auto &type : op.types) {
		if (type.id() == LogicalTypeId::VARIANT) {
			return true;
		}
	}
	return false;
}
#endif

bool CompressedMaterialization::TryGetVariantJoinKeyInfo(const Expression &expr, VariantJoinKeyInfo &result) {
	auto child = TryGetVariantWrapperColumnRef(expr);
	if (!child) {
		return false;
	}
	auto stats_it = statistics_map.find(child->Binding());
	if (stats_it == statistics_map.end() || !stats_it->second) {
		return false;
	}

	auto &variant_stats = *stats_it->second;
	if (variant_stats.GetType().id() != LogicalTypeId::VARIANT || !VariantStats::IsShredded(variant_stats)) {
		return false;
	}
	auto &shredded_stats = VariantStats::GetShreddedStats(variant_stats);
	if (!VariantShreddedStats::IsFullyShredded(shredded_stats)) {
		return false;
	}

	auto shredded_type = VariantStats::GetShreddedStructuredType(variant_stats);
	if (!shredded_type.IsIntegral()) {
		return false;
	}
	auto &typed_stats = VariantStats::GetTypedStats(shredded_stats);
	if (typed_stats.GetType() != shredded_type) { // LCOV_EXCL_START
		return false;
	} // LCOV_EXCL_STOP

	result.child = child.get();
	result.shredded_type = std::move(shredded_type);
	result.typed_stats = typed_stats.ToUnique();
	if (variant_stats.CanHaveNull()) {
		result.typed_stats->Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	}
	return true;
}

unique_ptr<BaseStatistics> CompressedMaterialization::CastVariantJoinKeyStats(const VariantJoinKeyInfo &key_info,
                                                                              const LogicalType &target_type) {
	if (key_info.shredded_type == target_type) {
		return key_info.typed_stats->ToUnique();
	}
	return CastStatistics::TryPropagate(*key_info.typed_stats, key_info.shredded_type, target_type);
}

unique_ptr<Expression> CompressedMaterialization::CreateVariantJoinKeyCast(unique_ptr<Expression> input,
                                                                           const LogicalType &shredded_type,
                                                                           const LogicalType &target_type) {
	input = BoundCastExpression::AddCastToType(context, std::move(input), shredded_type);
	if (shredded_type == target_type) {
		return input;
	}
	return BoundCastExpression::AddCastToType(context, std::move(input), target_type);
}

bool CompressedMaterialization::TryCompressVariantComparisonJoinKey(JoinCondition &condition) {
	VariantJoinKeyInfo left;
	VariantJoinKeyInfo right;
	if (!TryGetVariantJoinKeyInfo(condition.GetLHS(), left) || !TryGetVariantJoinKeyInfo(condition.GetRHS(), right)) {
		return false;
	}

	LogicalType target_type;
	if (!LogicalType::TryGetMaxLogicalType(context, left.shredded_type, right.shredded_type, target_type) ||
	    !target_type.IsIntegral()) {
		return false;
	}

	auto left_stats = CastVariantJoinKeyStats(left, target_type);
	auto right_stats = CastVariantJoinKeyStats(right, target_type);
	if (!left_stats || !right_stats) {
		return false;
	}

	condition.LeftReference() = CreateVariantJoinKeyCast(left.child->Copy(), left.shredded_type, target_type);
	condition.RightReference() = CreateVariantJoinKeyCast(right.child->Copy(), right.shredded_type, target_type);
	condition.SetLeftStats(std::move(left_stats));
	condition.SetRightStats(std::move(right_stats));
	return true;
}

void CompressedMaterialization::CompressComparisonJoin(unique_ptr<LogicalOperator> &op) {
	auto &join = op->Cast<LogicalComparisonJoin>();
	if (join.join_type == JoinType::MARK) {
		// Tricky to get bindings right. RHS binding stays the same even though it changes type. Skip for now
		return;
	}

	auto &left_child = *join.children[0];
	auto &right_child = *join.children[1];

#ifndef DEBUG
	// In debug mode, we always apply compressed materialization to joins regardless of cardinalities,
	// so that it is well-tested. In release mode, we use the thresholds defined in the header
	// If any of the inputs has VARIANT, we skip these checks: compressing is assumed to always be better
	if (!HasVariantType(left_child) && !HasVariantType(right_child)) {
		const auto build_cardinality = right_child.has_estimated_cardinality ? right_child.estimated_cardinality
		                                                                     : right_child.EstimateCardinality(context);
		if (build_cardinality < JOIN_BUILD_CARDINALITY_THRESHOLD) {
			return;
		}

		if (right_child.types.size() < JOIN_BUILD_COLUMN_COUNT_THRESHOLD) {
			const auto join_cardinality =
			    join.has_estimated_cardinality ? join.estimated_cardinality : join.EstimateCardinality(context);
			const double ratio = static_cast<double>(join_cardinality) / static_cast<double>(build_cardinality);
			if (ratio > JOIN_CARDINALITY_RATIO_THRESHOLD) {
				return;
			}
		}
	}
#endif

	// Find all bindings referenced by non-colref expressions in the conditions
	// These are excluded from compression by projection
	// But we can try to compress the expression directly
	column_binding_set_t probe_compress_bindings;
	column_binding_set_t referenced_bindings;

	// mark predicate expressions as not compressible
	idx_t has_range = 0;
	join.HasEquality(has_range);
	if (join.HasArbitraryConditions()) {
		for (const auto &condition : join.conditions) {
			if (!condition.IsComparison()) {
				GetReferencedBindings(condition.GetJoinExpression(), referenced_bindings);
			}
		}
	}

	for (auto &condition : join.conditions) {
		if (join.conditions.size() == 1 && join.type != LogicalOperatorType::LOGICAL_DELIM_JOIN) {
			const bool compressed_variant_key =
			    condition.IsComparison() && TryCompressVariantComparisonJoinKey(condition);
			if (compressed_variant_key) {
				// The comparator wrapper has been lowered to casts over the VARIANT columns.
				// The referenced bindings are marked below, so no child projection compression is needed.
			} else if (condition.IsComparison() &&
			           condition.GetLHS().GetExpressionType() == ExpressionType::BOUND_COLUMN_REF &&
			           condition.GetRHS().GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
				// We only try to compress the join condition cols if there's one join condition.
				// Else it gets messy with the stats if one column shows up in multiple conditions.
				// check if either side is referenced in residual predicate
				auto &lhs_colref = condition.GetLHS().Cast<BoundColumnRefExpression>();
				auto &rhs_colref = condition.GetRHS().Cast<BoundColumnRefExpression>();
				bool lhs_referenced = referenced_bindings.count(lhs_colref.Binding()) > 0;
				bool rhs_referenced = referenced_bindings.count(rhs_colref.Binding()) > 0;

				if (!lhs_referenced && !rhs_referenced) {
					// Both are bound column refs, see if both can be compressed generically to the same type
					auto lhs_it = statistics_map.find(lhs_colref.Binding());
					auto rhs_it = statistics_map.find(rhs_colref.Binding());
					if (lhs_it != statistics_map.end() && rhs_it != statistics_map.end() && lhs_it->second &&
					    rhs_it->second) {
						// For joins we need to compress both using the same statistics, otherwise comparisons don't
						// work
						auto merged_stats = lhs_it->second->Copy();
						merged_stats.Merge(*rhs_it->second);

						// If one can be compressed, both can (same stats)
						auto compress_expr = GetCompressExpression(condition.GetLHS().Copy(), merged_stats);
						if (compress_expr) {
							D_ASSERT(GetCompressExpression(condition.GetRHS().Copy(), merged_stats));
							// This will be compressed generically, but we have to merge the stats
							const auto lhs_can_have_null = lhs_it->second->CanHaveNull();
							const auto rhs_can_have_null = rhs_it->second->CanHaveNull();
							lhs_it->second->Merge(merged_stats);
							rhs_it->second->Merge(merged_stats);
							// the compression only depends on the merged values, a side without NULLs still has none
							if (!lhs_can_have_null) {
								lhs_it->second->Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);
							}
							if (!rhs_can_have_null) {
								rhs_it->second->Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);
							}
							probe_compress_bindings.insert(lhs_colref.Binding());
							continue;
						}
					}
				}
			}
		}
		if (condition.IsComparison()) {
			GetReferencedBindings(condition.GetLHS(), referenced_bindings);
			GetReferencedBindings(condition.GetRHS(), referenced_bindings);
		}
	}

	if (join.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		for (auto &dec : join.duplicate_eliminated_columns) {
			GetReferencedBindings(*dec, referenced_bindings);
		}
	}

	// We mark the probe-side bindings that do not show up in the join condition as "referenced"
	// We don't want to compress these because they are streamed
	const auto probe_bindings = left_child.GetColumnBindings();
	for (auto &binding : probe_bindings) {
		if (probe_compress_bindings.find(binding) == probe_compress_bindings.end()) {
			referenced_bindings.insert(binding);
		}
	}

	// Create info for compression
	CompressedMaterializationInfo info(*op, {0, 1}, referenced_bindings);

	const auto bindings_out = join.GetColumnBindings();
	const auto &types = join.types;
	PopulateBindingMap(info, bindings_out, types, left_child);
	PopulateBindingMap(info, bindings_out, types, right_child);

	// Now try to compress
	CreateProjections(op, info);

	// Update join statistics
	UpdateComparisonJoinStats(op);
}

void CompressedMaterialization::UpdateComparisonJoinStats(unique_ptr<LogicalOperator> &op) {
	if (op->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return;
	}

	// Update join stats if compressed
	auto &compressed_join = op->children[0]->Cast<LogicalComparisonJoin>();
	for (idx_t condition_idx = 0; condition_idx < compressed_join.conditions.size(); condition_idx++) {
		auto &condition = compressed_join.conditions[condition_idx];
		if (!condition.IsComparison() || condition.GetLHS().GetExpressionType() != ExpressionType::BOUND_COLUMN_REF ||
		    condition.GetRHS().GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
			continue; // We definitely didn't compress these, nothing changed
		}

		auto &lhs_colref = condition.GetLHS().Cast<BoundColumnRefExpression>();
		auto &rhs_colref = condition.GetRHS().Cast<BoundColumnRefExpression>();
		auto lhs_it = statistics_map.find(lhs_colref.Binding());
		auto rhs_it = statistics_map.find(rhs_colref.Binding());
		if (lhs_it != statistics_map.end() && lhs_it->second) {
			condition.SetLeftStats(lhs_it->second->ToUnique());
		}
		if (rhs_it != statistics_map.end() && rhs_it->second) {
			condition.SetRightStats(rhs_it->second->ToUnique());
		}
	}
}

} // namespace duckdb
