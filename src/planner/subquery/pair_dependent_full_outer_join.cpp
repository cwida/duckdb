#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/planner/subquery/pair_dependent_full_outer_join.hpp"
#include "duckdb/function/window/rows_functions.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/subquery/recursive_dependent_join_planner.hpp"

namespace duckdb {

static vector<Identifier> GenerateInternalColumnNames(idx_t column_count, const string &prefix) {
	vector<Identifier> result;
	result.reserve(column_count);
	for (idx_t i = 0; i < column_count; i++) {
		result.push_back(Identifier(prefix + to_string(i)));
	}
	return result;
}

// FULL OUTER pair-dependent predicates use the paper's D_R x D_S construction. Each side is projected to the
// bindings referenced by the predicate and independently deduplicated before the domain product is formed.
// Volatile predicates additionally use row identity so every physical input pair remains a distinct domain pair.

struct PairDependentJoinSide {
	TableIndex cte_index;
	vector<ColumnBinding> bindings;
	vector<LogicalType> types;
	vector<Identifier> names;
	vector<idx_t> domain_positions;
	idx_t payload_count;
	unique_ptr<LogicalOperator> source;
};

struct PairDependentDomain {
	unique_ptr<LogicalOperator> plan;
	vector<ColumnBinding> original_bindings;
	vector<ColumnBinding> bindings;
	vector<LogicalType> types;
};

struct PairDependentSideRef {
	unique_ptr<LogicalOperator> plan;
	vector<ColumnBinding> payload_bindings;
	vector<ColumnBinding> domain_bindings;
};

static void AddCurrentJoinBinding(const ColumnBinding &binding, const unordered_set<TableIndex> &left_bindings,
                                  const unordered_set<TableIndex> &right_bindings,
                                  column_binding_set_t &left_references, column_binding_set_t &right_references) {
	if (left_bindings.count(binding.table_index)) {
		left_references.insert(binding);
	} else if (right_bindings.count(binding.table_index)) {
		right_references.insert(binding);
	}
}

static void CollectPairDependentBindings(Expression &expression, const unordered_set<TableIndex> &left_bindings,
                                         const unordered_set<TableIndex> &right_bindings,
                                         column_binding_set_t &left_references,
                                         column_binding_set_t &right_references) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expression.Cast<BoundColumnRefExpression>();
		if (colref.Depth() == 0) {
			AddCurrentJoinBinding(colref.Binding(), left_bindings, right_bindings, left_references, right_references);
		}
		return;
	}
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		auto &subquery = expression.Cast<BoundSubqueryExpression>();
		for (auto &column : subquery.GetBinder()->correlated_columns) {
			if (column.depth == 1) {
				AddCurrentJoinBinding(column.binding, left_bindings, right_bindings, left_references, right_references);
			}
		}
	}
	ExpressionIterator::EnumerateChildren(expression, [&](Expression &child) {
		CollectPairDependentBindings(child, left_bindings, right_bindings, left_references, right_references);
	});
}

static unique_ptr<LogicalOperator> AddRowIdentity(Binder &binder, unique_ptr<LogicalOperator> source) {
	auto window = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
	auto row_number = GetBuiltinWindowFunction(binder.context, RowNumberFun::Name, {})->Bind(binder.context);
	row_number->WindowStartMutable() = WindowBoundary::UNBOUNDED_PRECEDING;
	row_number->WindowEndMutable() = WindowBoundary::CURRENT_ROW_ROWS;
	row_number->SetAlias("__duckdb_pair_rowid");
	window->expressions.push_back(std::move(row_number));
	window->children.push_back(std::move(source));
	return std::move(window);
}

static PairDependentJoinSide PreparePairDependentJoinSide(Binder &binder, unique_ptr<LogicalOperator> source,
                                                          const column_binding_set_t &domain_bindings,
                                                          const string &name_prefix, bool preserve_row_identity) {
	source->ResolveOperatorTypes();
	PairDependentJoinSide result;
	result.payload_count = source->GetColumnBindings().size();
	if (preserve_row_identity) {
		source = AddRowIdentity(binder, std::move(source));
		source->ResolveOperatorTypes();
	}
	result.bindings = source->GetColumnBindings();
	result.types = source->types;
	result.names = GenerateInternalColumnNames(result.types.size(), name_prefix);
	result.cte_index = binder.GenerateTableIndex();
	result.source = std::move(source);
	for (idx_t i = 0; i < result.payload_count; i++) {
		if (domain_bindings.count(result.bindings[i])) {
			result.domain_positions.push_back(i);
		}
	}
	if (result.domain_positions.empty()) {
		throw InternalException("Pair-dependent FULL OUTER join side has no domain bindings");
	}
	if (preserve_row_identity) {
		result.domain_positions.push_back(result.bindings.size() - 1);
	}
	return result;
}

static PairDependentDomain CreatePairDependentDomain(Binder &binder, const PairDependentJoinSide &side) {
	auto cte_ref = make_uniq<LogicalCTERef>(binder.GenerateTableIndex(), side.cte_index, side.types, side.names);
	auto cte_bindings = cte_ref->GetColumnBindings();

	PairDependentDomain result;
	vector<unique_ptr<Expression>> domain_expressions;
	for (auto position : side.domain_positions) {
		result.original_bindings.push_back(side.bindings[position]);
		result.types.push_back(side.types[position]);
		domain_expressions.push_back(make_uniq<BoundColumnRefExpression>(side.types[position], cte_bindings[position]));
	}
	auto group_index = binder.GenerateTableIndex();
	auto aggregate_index = binder.GenerateTableIndex();
	vector<unique_ptr<Expression>> aggregates;
	auto distinct = make_uniq<LogicalAggregate>(group_index, aggregate_index, std::move(aggregates));
	distinct->groups = std::move(domain_expressions);
	distinct->children.push_back(std::move(cte_ref));
	result.plan = std::move(distinct);
	result.bindings = result.plan->GetColumnBindings();
	return result;
}

static PairDependentSideRef CreatePairDependentSideRef(Binder &binder, const PairDependentJoinSide &side) {
	auto cte_ref = make_uniq<LogicalCTERef>(binder.GenerateTableIndex(), side.cte_index, side.types, side.names);
	auto bindings = cte_ref->GetColumnBindings();
	PairDependentSideRef result;
	result.payload_bindings.insert(result.payload_bindings.end(), bindings.begin(),
	                               bindings.begin() +
	                                   NumericCast<vector<ColumnBinding>::difference_type>(side.payload_count));
	for (auto position : side.domain_positions) {
		result.domain_bindings.push_back(bindings[position]);
	}
	result.plan = std::move(cte_ref);
	return result;
}

static void AddNotDistinctConditions(vector<JoinCondition> &conditions, const vector<LogicalType> &types,
                                     const vector<ColumnBinding> &left_bindings,
                                     const vector<ColumnBinding> &right_bindings) {
	D_ASSERT(types.size() == left_bindings.size());
	D_ASSERT(types.size() == right_bindings.size());
	for (idx_t i = 0; i < types.size(); i++) {
		conditions.emplace_back(make_uniq<BoundColumnRefExpression>(types[i], left_bindings[i]),
		                        make_uniq<BoundColumnRefExpression>(types[i], right_bindings[i]),
		                        ExpressionType::COMPARE_NOT_DISTINCT_FROM);
	}
}

static void AddMarkerCondition(vector<JoinCondition> &conditions, ColumnBinding marker_binding) {
	auto marker = make_uniq<BoundColumnRefExpression>(LogicalType::BOOLEAN, marker_binding);
	auto marker_true = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
	conditions.emplace_back(BoundComparisonExpression::Create(ExpressionType::COMPARE_NOT_DISTINCT_FROM,
	                                                          std::move(marker), std::move(marker_true)));
}

class PairDependentFullOuterJoinBuilder {
public:
	PairDependentFullOuterJoinBuilder(Binder &binder, unique_ptr<Expression> condition,
	                                  unique_ptr<LogicalOperator> left, unique_ptr<LogicalOperator> right,
	                                  const unordered_set<TableIndex> &left_bindings,
	                                  const unordered_set<TableIndex> &right_bindings)
	    : binder(binder), condition(std::move(condition)), left(std::move(left)), right(std::move(right)),
	      left_table_bindings(left_bindings), right_table_bindings(right_bindings) {
	}

	PairDependentJoinPlan Build();

private:
	Binder &binder;
	unique_ptr<Expression> condition;
	unique_ptr<LogicalOperator> left;
	unique_ptr<LogicalOperator> right;
	const unordered_set<TableIndex> &left_table_bindings;
	const unordered_set<TableIndex> &right_table_bindings;
};

PairDependentJoinPlan PairDependentFullOuterJoinBuilder::Build() {
	column_binding_set_t left_references;
	column_binding_set_t right_references;
	CollectPairDependentBindings(*condition, left_table_bindings, right_table_bindings, left_references,
	                             right_references);
	auto preserve_row_identity = condition->IsVolatile();

	auto left_side = PreparePairDependentJoinSide(binder, std::move(left), left_references, "__duckdb_full_l_",
	                                              preserve_row_identity);
	auto right_side = PreparePairDependentJoinSide(binder, std::move(right), right_references, "__duckdb_full_r_",
	                                               preserve_row_identity);
	auto left_domain = CreatePairDependentDomain(binder, left_side);
	auto right_domain = CreatePairDependentDomain(binder, right_side);

	CorrelatedColumnBindingReplacer condition_replacer;
	condition_replacer.AddReplacements(left_domain.original_bindings, left_domain.bindings);
	condition_replacer.AddReplacements(right_domain.original_bindings, right_domain.bindings);
	condition_replacer.VisitExpression(&condition);

	auto match_root = LogicalCrossProduct::Create(std::move(left_domain.plan), std::move(right_domain.plan));
	unique_ptr<LogicalOperator> match_projection =
	    LogicalProjection::CreateIdentity(binder.GenerateTableIndex(), std::move(match_root));
	match_projection->expressions.push_back(std::move(condition));
	RecursiveDependentJoinPlanner::Plan(binder, match_projection);
	match_projection->ResolveOperatorTypes();
	auto match_bindings = match_projection->GetColumnBindings();
	auto left_domain_count = NumericCast<vector<ColumnBinding>::difference_type>(left_domain.bindings.size());
	auto left_match_bindings =
	    vector<ColumnBinding>(match_bindings.begin(), match_bindings.begin() + left_domain_count);
	auto right_match_bindings =
	    vector<ColumnBinding>(match_bindings.begin() + left_domain_count, match_bindings.end() - 1);
	auto marker_binding = match_bindings.back();

	// S LEFT JOIN match emits every matching S row for each R domain and emits an unmatched S row only once.
	auto right_ref = CreatePairDependentSideRef(binder, right_side);
	vector<JoinCondition> right_match_conditions;
	AddNotDistinctConditions(right_match_conditions, right_domain.types, right_ref.domain_bindings,
	                         right_match_bindings);
	AddMarkerCondition(right_match_conditions, marker_binding);
	auto right_with_match =
	    LogicalComparisonJoin::CreateJoin(JoinType::LEFT, JoinRefType::REGULAR, std::move(right_ref.plan),
	                                      std::move(match_projection), std::move(right_match_conditions));

	// R FULL JOIN the matching domain restores matching pairs and the unmatched R rows.
	auto left_ref = CreatePairDependentSideRef(binder, left_side);
	vector<JoinCondition> final_conditions;
	AddNotDistinctConditions(final_conditions, left_domain.types, left_ref.domain_bindings, left_match_bindings);
	AddMarkerCondition(final_conditions, marker_binding);
	auto final_join = LogicalComparisonJoin::CreateJoin(JoinType::OUTER, JoinRefType::REGULAR, std::move(left_ref.plan),
	                                                    std::move(right_with_match), std::move(final_conditions));

	vector<unique_ptr<Expression>> output_expressions;
	for (idx_t i = 0; i < left_ref.payload_bindings.size(); i++) {
		output_expressions.push_back(
		    make_uniq<BoundColumnRefExpression>(left_side.types[i], left_ref.payload_bindings[i]));
	}
	for (idx_t i = 0; i < right_ref.payload_bindings.size(); i++) {
		output_expressions.push_back(
		    make_uniq<BoundColumnRefExpression>(right_side.types[i], right_ref.payload_bindings[i]));
	}
	auto output = make_uniq<LogicalProjection>(binder.GenerateTableIndex(), std::move(output_expressions));
	output->children.push_back(std::move(final_join));
	auto output_bindings = output->GetColumnBindings();

	PairDependentJoinPlan result;
	for (idx_t i = 0; i < left_side.payload_count; i++) {
		result.output_replacements.Add(left_side.bindings[i], output_bindings[i]);
	}
	for (idx_t i = 0; i < right_side.payload_count; i++) {
		result.output_replacements.Add(right_side.bindings[i], output_bindings[left_side.payload_count + i]);
	}

	unique_ptr<LogicalOperator> plan = std::move(output);
	plan =
	    make_uniq<LogicalMaterializedCTE>(Identifier("__duckdb_pair_right_" + to_string(right_side.cte_index.index)),
	                                      right_side.cte_index, right_side.types.size(), std::move(right_side.source),
	                                      std::move(plan), CTEMaterialize::CTE_MATERIALIZE_DEFAULT);
	plan = make_uniq<LogicalMaterializedCTE>(Identifier("__duckdb_pair_left_" + to_string(left_side.cte_index.index)),
	                                         left_side.cte_index, left_side.types.size(), std::move(left_side.source),
	                                         std::move(plan), CTEMaterialize::CTE_MATERIALIZE_DEFAULT);
	result.plan = std::move(plan);
	return result;
}

PairDependentJoinPlan PairDependentFullOuterJoinPlanner::Plan(Binder &binder, unique_ptr<Expression> condition,
                                                              unique_ptr<LogicalOperator> left,
                                                              unique_ptr<LogicalOperator> right,
                                                              const unordered_set<TableIndex> &left_bindings,
                                                              const unordered_set<TableIndex> &right_bindings) {
	PairDependentFullOuterJoinBuilder builder(binder, std::move(condition), std::move(left), std::move(right),
	                                          left_bindings, right_bindings);
	return builder.Build();
}

} // namespace duckdb
