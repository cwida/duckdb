#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/join/physical_join.hpp"
#include "duckdb/execution/operator/set/physical_trampoline.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_trampoline.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

namespace {

enum class TrampolineInputShape : uint8_t { NO_SCAN, LINEAR, NONLINEAR };

//! Whether the given operator is additive in the child subtree at child_idx:
//! op(..., W1 UNION ALL W2, ...) == op(..., W1, ...) UNION ALL op(..., W2, ...)
bool OperatorIsAdditiveInChild(const PhysicalOperator &op, idx_t child_idx) {
	switch (op.type) {
	case PhysicalOperatorType::PROJECTION:
	case PhysicalOperatorType::FILTER:
		return true;
	case PhysicalOperatorType::CROSS_PRODUCT:
		return true;
	case PhysicalOperatorType::HASH_JOIN:
	case PhysicalOperatorType::NESTED_LOOP_JOIN:
	case PhysicalOperatorType::BLOCKWISE_NL_JOIN:
	case PhysicalOperatorType::PIECEWISE_MERGE_JOIN:
	case PhysicalOperatorType::IE_JOIN: {
		const auto join_type = op.Cast<PhysicalJoin>().join_type;
		switch (join_type) {
		case JoinType::INNER:
			return true;
		case JoinType::LEFT:
		case JoinType::SEMI:
		case JoinType::ANTI:
		case JoinType::MARK:
		case JoinType::SINGLE:
			return child_idx == 0;
		case JoinType::RIGHT:
		case JoinType::RIGHT_SEMI:
		case JoinType::RIGHT_ANTI:
			return child_idx == 1;
		default:
			return false;
		}
	}
	case PhysicalOperatorType::ASOF_JOIN: {
		// nearest-match semantics are not additive in the matched side
		const auto join_type = op.Cast<PhysicalJoin>().join_type;
		return (join_type == JoinType::INNER || join_type == JoinType::LEFT) && child_idx == 0;
	}
	default:
		return false;
	}
}

//! Classifies how a branch plan consumes the trampoline input. A branch is linear when evaluating
//! it per wave and over the bag union of all waves provably produces the same bag of rows.
TrampolineInputShape AnalyzeBranchShape(const PhysicalOperator &op, TableIndex cte_index) {
	if (op.type == PhysicalOperatorType::RECURSIVE_CTE_SCAN) {
		auto &scan = op.Cast<PhysicalColumnDataScan>();
		if (scan.cte_index == cte_index) {
			return TrampolineInputShape::LINEAR;
		}
	}
	auto children = op.GetChildren();
	optional_idx scan_child;
	TrampolineInputShape child_shape = TrampolineInputShape::NO_SCAN;
	for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
		const auto shape = AnalyzeBranchShape(children[child_idx].get(), cte_index);
		if (shape == TrampolineInputShape::NO_SCAN) {
			continue;
		}
		if (scan_child.IsValid()) {
			// several inputs read the trampoline: cross terms between waves are possible
			return TrampolineInputShape::NONLINEAR;
		}
		scan_child = child_idx;
		child_shape = shape;
	}
	if (!scan_child.IsValid()) {
		return TrampolineInputShape::NO_SCAN;
	}
	if (child_shape == TrampolineInputShape::NONLINEAR) {
		return TrampolineInputShape::NONLINEAR;
	}
	return OperatorIsAdditiveInChild(op, scan_child.GetIndex()) ? TrampolineInputShape::LINEAR
	                                                            : TrampolineInputShape::NONLINEAR;
}

bool BranchIsLinear(const PhysicalOperator &branch, TableIndex cte_index) {
	// a branch without a trampoline scan produces rows once per invocation, so wave
	// boundaries change its output multiplicity: only a scanning, linear branch may merge
	return AnalyzeBranchShape(branch, cte_index) == TrampolineInputShape::LINEAR;
}

bool LogicalSubtreeIsVolatile(const LogicalOperator &op) {
	bool found_volatile = false;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](const unique_ptr<Expression> *child) {
		if (*child && (*child)->IsVolatile()) {
			found_volatile = true;
		}
	});
	if (found_volatile) {
		return true;
	}
	for (auto &child : op.children) {
		if (LogicalSubtreeIsVolatile(*child)) {
			return true;
		}
	}
	return false;
}

//! Whether the branch's selector output is provably the constant 0 on every row:
//! the root is a projection whose selector expression folds to 0
bool BranchOnlyEmits(ClientContext &context, const PhysicalOperator &branch, idx_t selector_idx) {
	if (branch.type != PhysicalOperatorType::PROJECTION) {
		return false;
	}
	auto &projection = branch.Cast<PhysicalProjection>();
	if (selector_idx >= projection.select_list.size()) {
		return false;
	}
	auto &selector_expr = *projection.select_list[selector_idx];
	if (!selector_expr.IsFoldable()) {
		return false;
	}
	Value selector_value;
	if (!ExpressionExecutor::TryEvaluateScalar(context, selector_expr, selector_value)) {
		return false;
	}
	if (selector_value.IsNull()) {
		return false;
	}
	auto as_bigint = selector_value.DefaultTryCastAs(LogicalType::BIGINT);
	if (!as_bigint || as_bigint->IsNull()) {
		return false;
	}
	return BigIntValue::Get(*as_bigint) == 0;
}

bool PhysicalSubtreeIsRepeatable(const PhysicalOperator &op,
                                 const reference_set_t<const PhysicalOperator> &non_repeatable_operators) {
	if (non_repeatable_operators.find(op) != non_repeatable_operators.end()) {
		return false;
	}
	for (auto child : op.GetChildren()) {
		if (!PhysicalSubtreeIsRepeatable(child.get(), non_repeatable_operators)) {
			return false;
		}
	}
	return true;
}

} // namespace

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalTrampoline &op) {
	D_ASSERT(op.children.size() >= 2);

	// placeholder the branch scans are planned against; the operator rebinds them per wave
	auto working_table = make_shared_ptr<ColumnDataCollection>(context, op.types);
	recursive_cte_tables[op.table_index] = working_table;

	auto &seed = CreatePlan(*op.children[0]);

	vector<reference<PhysicalOperator>> branches;
	planning_recursive_cte_depth++;
	for (idx_t branch_idx = 1; branch_idx < op.children.size(); branch_idx++) {
		branches.push_back(CreatePlan(*op.children[branch_idx]));
	}
	planning_recursive_cte_depth--;

	auto &trampoline = Make<PhysicalTrampoline>(op.ctename, op.table_index, op.types, op.selector_idx, seed, branches,
	                                            op.estimated_cardinality)
	                       .Cast<PhysicalTrampoline>();
	trampoline.working_table = working_table;
	trampoline.non_repeatable_operators = non_repeatable_operators;

	// merging waves changes how often a branch is invoked, which is observable through
	// volatile expressions and non-repeatable subplans, so those branches stay per-wave
	vector<bool> fusable(branches.size(), false);
	vector<bool> emit_only(branches.size(), false);
	bool all_linear = true;
	for (idx_t branch_idx = 0; branch_idx < branches.size(); branch_idx++) {
		fusable[branch_idx] = BranchIsLinear(branches[branch_idx].get(), op.table_index) &&
		                      !LogicalSubtreeIsVolatile(*op.children[branch_idx + 1]) &&
		                      PhysicalSubtreeIsRepeatable(branches[branch_idx].get(), non_repeatable_operators);
		all_linear = all_linear && fusable[branch_idx];
		emit_only[branch_idx] =
		    fusable[branch_idx] && BranchOnlyEmits(context, branches[branch_idx].get(), op.selector_idx);
	}
	// an emit-only branch's output never reaches another branch, so no consumer can
	// observe how its input was partitioned into waves; merging them is sound even
	// next to nonlinear branches (invocation counts stay unobservable through the
	// volatility and repeatability gates above)
	trampoline.branch_wave_merge.resize(branches.size());
	for (idx_t branch_idx = 0; branch_idx < branches.size(); branch_idx++) {
		trampoline.branch_wave_merge[branch_idx] = all_linear || emit_only[branch_idx];
	}
	return trampoline;
}

} // namespace duckdb
