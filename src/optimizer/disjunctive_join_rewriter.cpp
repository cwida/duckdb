#include "duckdb/optimizer/disjunctive_join_rewriter.hpp"

#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator_repeatability.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

namespace duckdb {

DisjunctiveJoinRewriter::DisjunctiveJoinRewriter(Binder &binder) : binder(binder) {
}

TableIndex DisjunctiveJoinRewriter::NewTableIndex() {
	return binder.GenerateTableIndex();
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::Optimize(unique_ptr<LogicalOperator> op) {
	op->ResolveOperatorTypes();
	BindingReplacementGraph replacements;
	return OptimizeInternal(std::move(op), replacements);
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::OptimizeInternal(unique_ptr<LogicalOperator> op,
                                                                      BindingReplacementGraph &replacements) {
	for (idx_t child_idx = 0; child_idx < op->children.size(); child_idx++) {
		ColumnBindingRewrite::RewriteChild(
		    op, child_idx, [&](unique_ptr<LogicalOperator> &child, BindingReplacementGraph &child_replacements) {
			    child = OptimizeInternal(std::move(child), child_replacements);
			    replacements.Merge(child_replacements);
		    });
	}

	if (op->type != LogicalOperatorType::LOGICAL_ANY_JOIN) {
		return op;
	}
	auto &join = op->Cast<LogicalAnyJoin>();

	unordered_set<TableIndex> left_tables, right_tables;
	for (auto &b : join.children[0]->GetColumnBindings()) {
		left_tables.insert(b.table_index);
	}
	for (auto &b : join.children[1]->GetColumnBindings()) {
		right_tables.insert(b.table_index);
	}

	vector<Branch> branches;
	if (!ShouldRewrite(join, left_tables, right_tables, branches)) {
		return op;
	}

	auto orig_bindings = join.GetColumnBindings();
	auto orig_types = join.types;
	auto left_child = std::move(op->children[0]);
	auto right_child = std::move(op->children[1]);

	TableIndex left_cte_idx = NewTableIndex();
	TableIndex right_cte_idx = NewTableIndex();

	CTEInfo left_cte {left_cte_idx, left_child->types, left_child->GetColumnBindings()};
	CTEInfo right_cte {right_cte_idx, right_child->types, right_child->GetColumnBindings()};

	unique_ptr<LogicalOperator> epilogue;
	switch (join.join_type) {
	case JoinType::INNER:
		epilogue = BuildInnerJoin(left_cte, right_cte, branches);
		break;
	case JoinType::LEFT:
		epilogue = BuildLeftJoin(left_cte, right_cte, branches);
		break;
	case JoinType::RIGHT:
		epilogue = BuildRightJoin(left_cte, right_cte, branches);
		break;
	case JoinType::OUTER:
		epilogue = BuildFullJoin(left_cte, right_cte, branches);
		break;
	case JoinType::SEMI:
		epilogue = BuildSemiJoin(left_cte, right_cte, branches);
		break;
	case JoinType::ANTI:
		epilogue = BuildAntiJoin(left_cte, right_cte, branches);
		break;
	default:
		return op;
	}

	epilogue = NormalizeOutput(std::move(epilogue), orig_bindings, orig_types, left_cte, right_cte, join.join_type,
	                           replacements);

	// Every branch must consume the same input occurrences unless copying is proven safe.
	auto right_materialize = LogicalSubtreeIsRepeatable(*right_child) ? CTEMaterialize::CTE_MATERIALIZE_DEFAULT
	                                                                  : CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	auto left_materialize = LogicalSubtreeIsRepeatable(*left_child) ? CTEMaterialize::CTE_MATERIALIZE_DEFAULT
	                                                                : CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
	epilogue = make_uniq<LogicalMaterializedCTE>("rhs_cte", right_cte.table_index, right_cte.output_types.size(),
	                                             std::move(right_child), std::move(epilogue), right_materialize);

	epilogue = make_uniq<LogicalMaterializedCTE>("lhs_cte", left_cte.table_index, left_cte.output_types.size(),
	                                             std::move(left_child), std::move(epilogue), left_materialize);

	epilogue->ResolveOperatorTypes();
	ColumnBindingRewrite::ValidateOutput(orig_bindings, epilogue->GetColumnBindings(), replacements);
	D_ASSERT(epilogue->types == orig_types);
	return epilogue;
}

bool DisjunctiveJoinRewriter::ShouldRewrite(const LogicalOperator &join, const unordered_set<TableIndex> &left_tables,
                                            const unordered_set<TableIndex> &right_tables,
                                            vector<Branch> &out_branches) const {
	auto &any_join = join.Cast<LogicalAnyJoin>();
	switch (any_join.join_type) {
	case JoinType::INNER:
	case JoinType::LEFT:
	case JoinType::RIGHT:
	case JoinType::OUTER:
	case JoinType::SEMI:
	case JoinType::ANTI:
		break;
	default:
		return false;
	}

	if (!any_join.condition) {
		return false;
	}

	const Expression &expr = *any_join.condition;
	if (expr.GetExpressionType() != ExpressionType::CONJUNCTION_OR) {
		return false;
	}

	if (expr.IsVolatile() || expr.CanThrow()) {
		return false;
	}

	if (!FlattenOR(expr, left_tables, right_tables, out_branches)) {
		return false;
	}

	return out_branches.size() >= 2;
}

bool DisjunctiveJoinRewriter::FlattenOR(const Expression &expr, const unordered_set<TableIndex> &left_tables,
                                        const unordered_set<TableIndex> &right_tables, vector<Branch> &out) const {
	// Bound the quadratic exclusion expansion and the cost of optimizing the generated plan.
	static constexpr idx_t MAX_BRANCH_COUNT = 16;
	if (out.size() >= MAX_BRANCH_COUNT) {
		return false;
	}

	if (expr.GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		for (const auto &child : conj.GetChildren()) {
			if (!FlattenOR(*child, left_tables, right_tables, out)) {
				return false;
			}
		}
		return true;
	}

	if (expr.GetExpressionType() != ExpressionType::COMPARE_EQUAL &&
	    expr.GetExpressionType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
		return false;
	}

	const auto &comp = expr.Cast<BoundFunctionExpression>();
	auto &left = BoundComparisonExpression::Left(comp);
	auto &right = BoundComparisonExpression::Right(comp);

	auto l_side = JoinSide::GetJoinSide(left, left_tables, right_tables);
	auto r_side = JoinSide::GetJoinSide(right, left_tables, right_tables);

	if (l_side == JoinSide::BOTH || l_side == JoinSide::NONE || r_side == JoinSide::BOTH || r_side == JoinSide::NONE ||
	    l_side == r_side) {
		return false;
	}

	Branch b;
	if (l_side == JoinSide::LEFT) {
		b.left_expr = left.Copy();
		b.right_expr = right.Copy();
	} else {
		b.left_expr = right.Copy();
		b.right_expr = left.Copy();
	}
	b.comparison_type = expr.GetExpressionType();
	out.push_back(std::move(b));
	return true;
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::BuildInnerJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
                                                                    const vector<Branch> &branches) {
	auto children = CreateMatchedBranches(left_cte, right_cte, branches);
	return CreateUnionAll(std::move(children), left_cte.output_types.size() + right_cte.output_types.size());
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::BuildLeftJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
                                                                   const vector<Branch> &branches) {
	auto children = CreateMatchedBranches(left_cte, right_cte, branches);
	children.push_back(CreateUnmatchedProbeSide(left_cte, right_cte, branches));
	return CreateUnionAll(std::move(children), left_cte.output_types.size() + right_cte.output_types.size());
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::BuildRightJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
                                                                    const vector<Branch> &branches) {
	return BuildLeftJoin(right_cte, left_cte, SwapBranches(branches));
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::BuildFullJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
                                                                   const vector<Branch> &branches) {
	auto children = CreateMatchedBranches(left_cte, right_cte, branches);
	children.push_back(CreateUnmatchedProbeSide(left_cte, right_cte, branches));
	children.push_back(CreateUnmatchedBuildSide(left_cte, right_cte, branches));
	return CreateUnionAll(std::move(children), left_cte.output_types.size() + right_cte.output_types.size());
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::BuildSemiJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
                                                                   const vector<Branch> &branches) {
	TableIndex probe_ref = NewTableIndex();
	auto current = CreateCTERef(left_cte, probe_ref);
	ColumnBindingReplacer probe_replacer;
	probe_replacer.AddReplacements(left_cte.original_bindings, current->GetColumnBindings());
	vector<unique_ptr<Expression>> marker_refs;

	for (idx_t i = 0; i < branches.size(); i++) {
		TableIndex build_ref = NewTableIndex();
		TableIndex mark_tbl = NewTableIndex();

		auto build_scan = CreateCTERef(right_cte, build_ref);
		ColumnBindingReplacer build_replacer;
		build_replacer.AddReplacements(right_cte.original_bindings, build_scan->GetColumnBindings());

		auto mark_join = make_uniq<LogicalComparisonJoin>(JoinType::MARK);
		mark_join->conditions.push_back(CreateJoinCondition(branches[i], probe_replacer, build_replacer));
		mark_join->mark_index = mark_tbl;
		mark_join->AddChild(std::move(current));
		mark_join->AddChild(std::move(build_scan));

		current = std::move(mark_join);
		marker_refs.push_back(
		    CreateColRef(ColumnBinding(mark_tbl, ProjectionIndex(0)), LogicalType::BOOLEAN, "mark_" + to_string(i)));
	}

	auto filter = make_uniq<LogicalFilter>();
	filter->expressions.push_back(
	    BoundConjunctionExpression::Create(ExpressionType::CONJUNCTION_OR, std::move(marker_refs)));
	filter->AddChild(std::move(current));

	return std::move(filter);
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::BuildAntiJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
                                                                   const vector<Branch> &branches) {
	TableIndex probe_ref = NewTableIndex();
	return CreateAntiJoinChain(left_cte, right_cte, branches, probe_ref);
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::CreateBranchJoin(const CTEInfo &left_cte, const CTEInfo &right_cte,
                                                                      const vector<Branch> &branches,
                                                                      idx_t branch_idx) {
	auto left_scan = CreateCTERef(left_cte, NewTableIndex());
	auto right_scan = CreateCTERef(right_cte, NewTableIndex());
	ColumnBindingReplacer left_replacer, right_replacer;
	left_replacer.AddReplacements(left_cte.original_bindings, left_scan->GetColumnBindings());
	right_replacer.AddReplacements(right_cte.original_bindings, right_scan->GetColumnBindings());

	auto join = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
	join->conditions.push_back(CreateJoinCondition(branches[branch_idx], left_replacer, right_replacer));
	for (idx_t i = 0; i < branch_idx; i++) {
		auto previous = CreateJoinCondition(branches[i], left_replacer, right_replacer);
		vector<unique_ptr<Expression>> exclusions;
		if (previous.GetComparisonType() == ExpressionType::COMPARE_EQUAL) {
			// Preserve UNKNOWN, with separate NULL checks that statistics can simplify.
			auto left_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
			left_null->GetChildrenMutable().push_back(previous.GetLHS().Copy());
			exclusions.push_back(std::move(left_null));
			auto right_null =
			    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
			right_null->GetChildrenMutable().push_back(previous.GetRHS().Copy());
			exclusions.push_back(std::move(right_null));
		}
		exclusions.push_back(BoundComparisonExpression::Create(NegateComparisonExpression(previous.GetComparisonType()),
		                                                       std::move(previous.LeftReference()),
		                                                       std::move(previous.RightReference())));
		join->conditions.emplace_back(
		    BoundConjunctionExpression::Create(ExpressionType::CONJUNCTION_OR, std::move(exclusions)));
	}

	join->AddChild(std::move(left_scan));
	join->AddChild(std::move(right_scan));

	return std::move(join);
}

vector<unique_ptr<LogicalOperator>> DisjunctiveJoinRewriter::CreateMatchedBranches(const CTEInfo &left_cte,
                                                                                   const CTEInfo &right_cte,
                                                                                   const vector<Branch> &branches) {
	vector<unique_ptr<LogicalOperator>> union_children;

	for (idx_t i = 0; i < branches.size(); i++) {
		union_children.push_back(CreateBranchJoin(left_cte, right_cte, branches, i));
	}
	return union_children;
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::CreateUnmatchedProbeSide(const CTEInfo &probe_cte,
                                                                              const CTEInfo &build_cte,
                                                                              const vector<Branch> &branches) {
	TableIndex probe_ref = NewTableIndex();
	auto anti_chain = CreateAntiJoinChain(probe_cte, build_cte, branches, probe_ref);

	vector<unique_ptr<Expression>> proj_exprs;
	for (idx_t col = 0; col < probe_cte.output_types.size(); col++) {
		proj_exprs.push_back(CreateColRef(ColumnBinding(probe_ref, ProjectionIndex(col)), probe_cte.output_types[col]));
	}
	for (idx_t col = 0; col < build_cte.output_types.size(); col++) {
		proj_exprs.push_back(CreateNullExpr(build_cte.output_types[col]));
	}
	return CreateProjection(std::move(anti_chain), std::move(proj_exprs));
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::CreateUnmatchedBuildSide(const CTEInfo &left_cte,
                                                                              const CTEInfo &right_cte,
                                                                              const vector<Branch> &branches) {
	TableIndex rhs_ref = NewTableIndex();
	auto anti_chain = CreateAntiJoinChain(right_cte, left_cte, SwapBranches(branches), rhs_ref);
	auto null_exprs = CreateNullExprs(left_cte.output_types);
	auto right_exprs = CreateColRefs(rhs_ref, right_cte.output_types);

	null_exprs.insert(null_exprs.end(), std::make_move_iterator(right_exprs.begin()),
	                  std::make_move_iterator(right_exprs.end()));

	return CreateProjection(std::move(anti_chain), std::move(null_exprs));
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::CreateAntiJoinChain(const CTEInfo &probe_cte,
                                                                         const CTEInfo &build_cte,
                                                                         const vector<Branch> &branches,
                                                                         TableIndex probe_ref_idx) {
	auto current = CreateCTERef(probe_cte, probe_ref_idx);
	ColumnBindingReplacer probe_replacer;
	probe_replacer.AddReplacements(probe_cte.original_bindings, current->GetColumnBindings());

	for (idx_t i = 0; i < branches.size(); i++) {
		TableIndex build_ref = NewTableIndex();
		auto build_scan = CreateCTERef(build_cte, build_ref);
		ColumnBindingReplacer build_replacer;
		build_replacer.AddReplacements(build_cte.original_bindings, build_scan->GetColumnBindings());

		auto anti_join = make_uniq<LogicalComparisonJoin>(JoinType::ANTI);
		anti_join->conditions.push_back(CreateJoinCondition(branches[i], probe_replacer, build_replacer));
		anti_join->AddChild(std::move(current));
		anti_join->AddChild(std::move(build_scan));

		current = std::move(anti_join);
	}
	return current;
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::CreateUnionAll(vector<unique_ptr<LogicalOperator>> children,
                                                                    idx_t total_columns) {
	TableIndex union_tbl = NewTableIndex();
	return make_uniq<LogicalSetOperation>(union_tbl, total_columns, std::move(children),
	                                      LogicalOperatorType::LOGICAL_UNION, true, true);
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::NormalizeOutput(
    unique_ptr<LogicalOperator> plan, const vector<ColumnBinding> &orig_bindings, const vector<LogicalType> &orig_types,
    const CTEInfo &left_cte, const CTEInfo &right_cte, JoinType join_type, BindingReplacementGraph &replacements) {
	auto current_bindings = plan->GetColumnBindings();
	auto &first_cte = join_type == JoinType::RIGHT ? right_cte : left_cte;
	auto &second_cte = join_type == JoinType::RIGHT ? left_cte : right_cte;
	column_binding_map_t<idx_t> output_positions;
	for (idx_t i = 0; i < first_cte.original_bindings.size(); i++) {
		output_positions.emplace(first_cte.original_bindings[i], i);
	}
	if (join_type != JoinType::SEMI && join_type != JoinType::ANTI) {
		for (idx_t i = 0; i < second_cte.original_bindings.size(); i++) {
			output_positions.emplace(second_cte.original_bindings[i], first_cte.original_bindings.size() + i);
		}
	}

	vector<unique_ptr<Expression>> proj_exprs;
	proj_exprs.reserve(orig_bindings.size());
	for (idx_t i = 0; i < orig_bindings.size(); i++) {
		proj_exprs.push_back(CreateColRef(current_bindings[output_positions.at(orig_bindings[i])], orig_types[i]));
	}

	TableIndex norm_tbl = NewTableIndex();
	auto proj = make_uniq<LogicalProjection>(norm_tbl, std::move(proj_exprs));
	proj->AddChild(std::move(plan));

	for (idx_t i = 0; i < orig_bindings.size(); i++) {
		replacements.Add(
		    ReplacementBinding(orig_bindings[i], ColumnBinding(norm_tbl, ProjectionIndex(i)), orig_types[i]));
	}
	return std::move(proj);
}

JoinCondition DisjunctiveJoinRewriter::CreateJoinCondition(const Branch &branch, ColumnBindingReplacer &left_replacer,
                                                           ColumnBindingReplacer &right_replacer) {
	auto left_expr = branch.left_expr->Copy();
	auto right_expr = branch.right_expr->Copy();
	left_replacer.VisitExpression(&left_expr);
	right_replacer.VisitExpression(&right_expr);
	return JoinCondition(std::move(left_expr), std::move(right_expr), branch.comparison_type);
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::CreateCTERef(const CTEInfo &cte, TableIndex ref_idx) {
	vector<Identifier> bound_columns;
	bound_columns.reserve(cte.output_types.size());
	for (idx_t i = 0; i < cte.output_types.size(); ++i) {
		bound_columns.emplace_back("col_" + to_string(i));
	}
	return make_uniq<LogicalCTERef>(TableIndex(ref_idx), TableIndex(cte.table_index), cte.output_types,
	                                std::move(bound_columns));
}

unique_ptr<LogicalOperator> DisjunctiveJoinRewriter::CreateProjection(unique_ptr<LogicalOperator> child,
                                                                      vector<unique_ptr<Expression>> expressions) {
	TableIndex proj_tbl = NewTableIndex();
	auto proj = make_uniq<LogicalProjection>(proj_tbl, std::move(expressions));
	proj->AddChild(std::move(child));
	return std::move(proj);
}

unique_ptr<Expression> DisjunctiveJoinRewriter::CreateColRef(ColumnBinding binding, const LogicalType &type,
                                                             const string &alias) {
	return make_uniq<BoundColumnRefExpression>(Identifier(alias), type, binding);
}

unique_ptr<Expression> DisjunctiveJoinRewriter::CreateNullExpr(const LogicalType &type) {
	return make_uniq<BoundConstantExpression>(Value(type));
}

vector<unique_ptr<Expression>> DisjunctiveJoinRewriter::CreateColRefs(TableIndex table_index,
                                                                      const vector<LogicalType> &types) {
	vector<unique_ptr<Expression>> exprs;
	exprs.reserve(types.size());
	for (idx_t i = 0; i < types.size(); i++) {
		exprs.push_back(CreateColRef(ColumnBinding(table_index, ProjectionIndex(i)), types[i]));
	}
	return exprs;
}

vector<unique_ptr<Expression>> DisjunctiveJoinRewriter::CreateNullExprs(const vector<LogicalType> &types) {
	vector<unique_ptr<Expression>> exprs;
	exprs.reserve(types.size());
	for (idx_t i = 0; i < types.size(); i++) {
		exprs.push_back(make_uniq<BoundConstantExpression>(Value(types[i])));
	}
	return exprs;
}

vector<DisjunctiveJoinRewriter::Branch> DisjunctiveJoinRewriter::SwapBranches(const vector<Branch> &branches) {
	vector<Branch> swapped;
	for (auto &branch : branches) {
		Branch b;
		b.left_expr = branch.right_expr->Copy();
		b.right_expr = branch.left_expr->Copy();
		b.comparison_type = branch.comparison_type;
		swapped.push_back(std::move(b));
	}
	return swapped;
}

} // namespace duckdb
