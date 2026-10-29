#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/join/physical_join.hpp"
#include "duckdb/execution/operator/set/physical_trampoline.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_trampoline.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

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

//! Collects every constant the expression can evaluate to: foldable expressions,
//! CASE arms of constants, and casts thereof. Returns false when the set is unbounded.
bool CollectSelectorConstants(ClientContext &context, const Expression &expr, vector<Value> &constants) {
	if (expr.IsFoldable()) {
		Value value;
		if (!ExpressionExecutor::TryEvaluateScalar(context, expr, value)) {
			return false;
		}
		constants.push_back(std::move(value));
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CASE) {
		auto &case_expr = expr.Cast<BoundCaseExpression>();
		for (auto &check : case_expr.CaseChecks()) {
			if (!CollectSelectorConstants(context, *check.then_expr, constants)) {
				return false;
			}
		}
		return CollectSelectorConstants(context, case_expr.Else(), constants);
	}
	if (BoundCastExpression::IsCast(expr)) {
		auto &cast_expr = expr.Cast<BoundFunctionExpression>();
		vector<Value> inner;
		if (!CollectSelectorConstants(context, BoundCastExpression::Child(cast_expr), inner)) {
			return false;
		}
		for (auto &value : inner) {
			// evaluate through the actual bound cast: a registered cast may map
			// constants differently from the default cast registry
			auto evaluated = expr.Copy();
			auto &copy_cast = evaluated->Cast<BoundFunctionExpression>();
			BoundCastExpression::ChildMutable(copy_cast) = make_uniq<BoundConstantExpression>(value);
			Value cast_value;
			if (!evaluated->IsFoldable() || !ExpressionExecutor::TryEvaluateScalar(context, *evaluated, cast_value)) {
				return false;
			}
			constants.push_back(std::move(cast_value));
		}
		return true;
	}
	return false;
}

//! The set of selector values a branch can route to, when statically bounded: the root
//! must be a projection whose selector expression yields a bounded set of constants.
//! NULL and out-of-range constants raise at execution time, so they make the set unbounded.
bool AnalyzeSelectorTargets(ClientContext &context, const PhysicalOperator &branch, idx_t selector_idx,
                            idx_t branch_count, set<idx_t> &targets) {
	if (branch.type != PhysicalOperatorType::PROJECTION) {
		return false;
	}
	auto &projection = branch.Cast<PhysicalProjection>();
	if (selector_idx >= projection.select_list.size()) {
		return false;
	}
	vector<Value> constants;
	if (!CollectSelectorConstants(context, *projection.select_list[selector_idx], constants)) {
		return false;
	}
	for (auto &value : constants) {
		if (value.IsNull()) {
			return false;
		}
		auto as_bigint = value.DefaultTryCastAs(LogicalType::BIGINT);
		if (!as_bigint || as_bigint->IsNull()) {
			return false;
		}
		const auto target = BigIntValue::Get(*as_bigint);
		if (target < 0 || target > NumericCast<int64_t>(branch_count)) {
			return false;
		}
		targets.insert(NumericCast<idx_t>(target));
	}
	return true;
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
	auto frontier_table = make_shared_ptr<ColumnDataCollection>(context, op.types);
	trampoline_frontier_tables[op.table_index] = frontier_table;

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
	trampoline.branch_tee = op.branch_tee;
	trampoline.branch_tee.resize(branches.size(), false);
	trampoline.reads_frontier = op.reads_frontier;
	trampoline.frontier_table = frontier_table;
	trampoline.branch_distinct = op.branch_distinct;
	trampoline.branch_distinct.resize(branches.size(), false);
	for (auto distinct : trampoline.branch_distinct) {
		trampoline.has_distinct_branch = trampoline.has_distinct_branch || distinct;
	}
	trampoline.emit_distinct = op.emit_distinct;

	// merging waves changes how often a branch is invoked, which is observable through
	// volatile expressions and non-repeatable subplans, so those branches stay per-wave
	vector<bool> fusable(branches.size(), false);
	vector<set<idx_t>> targets(branches.size());
	vector<bool> targets_known(branches.size(), false);
	for (idx_t branch_idx = 0; branch_idx < branches.size(); branch_idx++) {
		fusable[branch_idx] = BranchIsLinear(branches[branch_idx].get(), op.table_index) &&
		                      !LogicalSubtreeIsVolatile(*op.children[branch_idx + 1]) &&
		                      PhysicalSubtreeIsRepeatable(branches[branch_idx].get(), non_repeatable_operators);
		targets_known[branch_idx] = AnalyzeSelectorTargets(context, branches[branch_idx].get(), op.selector_idx,
		                                                   branches.size(), targets[branch_idx]);
	}
	// merging a branch coarsens the wave partitioning of every branch its rows can reach,
	// transitively: a linear branch maps one input wave to one output wave, so coarsened
	// input means coarsened output for its own targets. A branch may therefore merge only
	// when every branch in its forward routing closure is itself unable to observe the
	// partitioning. Unsafety propagates backward along the routing graph; a statically
	// unbounded selector routes everywhere. The greatest fixpoint keeps cyclic all-safe
	// routing graphs merged, and an emit-only branch (targets {0}) merges unconditionally
	// since no branch can observe how its input was partitioned.
	vector<bool> merge = fusable;
	bool changed = true;
	while (changed) {
		changed = false;
		for (idx_t branch_idx = 0; branch_idx < branches.size(); branch_idx++) {
			if (!merge[branch_idx]) {
				continue;
			}
			bool safe = true;
			if (!targets_known[branch_idx]) {
				for (idx_t other_idx = 0; other_idx < branches.size(); other_idx++) {
					safe = safe && merge[other_idx];
				}
			} else {
				for (auto target : targets[branch_idx]) {
					safe = safe && (target == 0 || merge[target - 1]);
				}
			}
			if (!safe) {
				merge[branch_idx] = false;
				changed = true;
			}
		}
	}
	if (trampoline.RequiresRounds()) {
		// a readable frontier makes pending input observable, and distinct admission
		// is defined at the round boundary, so every branch keeps per-wave identity
		// within the lockstep rounds
		std::fill(merge.begin(), merge.end(), false);
	}
	trampoline.branch_wave_merge = std::move(merge);
	return trampoline;
}

} // namespace duckdb
