#include "duckdb/planner/sql_export/logical_plan_sql_exporter_internal.hpp"
#include "duckdb/planner/sql_export/bound_expression_sql_exporter_internal.hpp"
#include "duckdb/function/scalar/compressed_materialization_utils.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

bool LogicalPlanSQLExportHelpers::HasSimpleGroups(const LogicalAggregate &aggregate) {
	if (!aggregate.grouping_functions.empty() || aggregate.grouping_sets.size() > 1) {
		return false;
	}
	if (!aggregate.grouping_sets.empty() && aggregate.grouping_sets[0].size() != aggregate.groups.size()) {
		return false;
	}
	column_binding_set_t bindings;
	for (auto &group : aggregate.groups) {
		if (group->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		auto &column = group->Cast<BoundColumnRefExpression>();
		if (column.Depth() || !bindings.insert(column.Binding()).second) {
			return false;
		}
	}
	return true;
}

static idx_t ExpressionSize(const ParsedExpression &expression) {
	idx_t result = 1;
	for (auto &child : expression.Children()) {
		result += ExpressionSize(child);
	}
	return result;
}

static bool IsLeaf(const ParsedExpression &expression) {
	// Bare integers would become positional references in GROUP BY and ORDER BY.
	return expression.GetExpressionClass() == ExpressionClass::COLUMN_REF ||
	       (expression.GetExpressionClass() == ExpressionClass::CONSTANT &&
	        expression.Cast<ConstantExpression>().GetLiteral().IsNull());
}

static optional_ptr<SelectNode> CompositionScope(QueryNode &query) {
	if (query.type != QueryNodeType::SELECT_NODE || !query.cte_map.map.empty()) {
		return nullptr;
	}
	auto &select = query.Cast<SelectNode>();
	if (!select.from_table || select.sample || select.from_table->sample || select.having || select.qualify ||
	    select.aggregate_handling != AggregateHandling::STANDARD_HANDLING) {
		return nullptr;
	}
	return select;
}

static void MoveScope(SelectNode &target, SelectNode &source) {
	target.select_list = std::move(source.select_list);
	target.from_table = std::move(source.from_table);
	target.where_clause = std::move(source.where_clause);
	target.groups = std::move(source.groups);
	target.having = std::move(source.having);
	target.qualify = std::move(source.qualify);
	target.sample = std::move(source.sample);
	target.modifiers = std::move(source.modifiers);
	target.aggregate_handling = source.aggregate_handling;
}

static optional_ptr<const Expression> SemanticInput(optional_ptr<const Expression> expression) {
	while (auto wrapped = CMUtils::GetWrappedInput(*expression)) {
		expression = wrapped;
	}
	return expression;
}

struct CompositionProperties {
	bool safe = false;
	bool aggregate = false;
	bool volatile_expression = true;
};

static CompositionProperties OutputProperties(LogicalOperator &op, const ColumnBinding &binding);

static bool SafeExpression(const Expression &expression, LogicalOperator &input) {
	auto &semantic = *SemanticInput(expression);
	if (semantic.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &column = semantic.Cast<BoundColumnRefExpression>();
		return !column.Depth() && OutputProperties(input, column.Binding()).safe;
	}
	if (semantic.IsVolatile() || semantic.CanThrow() ||
	    semantic.GetExpressionClass() == ExpressionClass::BOUND_LAMBDA ||
	    semantic.GetExpressionClass() == ExpressionClass::BOUND_WINDOW ||
	    semantic.GetExpressionClass() == ExpressionClass::BOUND_UNNEST) {
		return false;
	}
	bool safe = true;
	ExpressionIterator::EnumerateChildren(
	    semantic, [&](const Expression &child) { safe = safe && SafeExpression(child, input); });
	return safe;
}

static CompositionProperties OutputProperties(LogicalOperator &op, const ColumnBinding &binding) {
	auto bindings = op.GetColumnBindings();
	for (idx_t i = 0; i < bindings.size(); i++) {
		if (bindings[i] != binding) {
			continue;
		}
		if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto &expression = *SemanticInput(*op.expressions[i]);
			if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				return OutputProperties(*op.children[0], expression.Cast<BoundColumnRefExpression>().Binding());
			}
			return {SafeExpression(expression, *op.children[0]), false, expression.IsVolatile()};
		}
		if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
			auto &aggregate = op.Cast<LogicalAggregate>();
			if (i < aggregate.groups.size()) {
				return {SafeExpression(*aggregate.groups[i], *op.children[0]), false,
				        aggregate.groups[i]->IsVolatile()};
			}
			i -= aggregate.groups.size();
			if (i < aggregate.expressions.size()) {
				return {SafeExpression(*aggregate.expressions[i], *op.children[0]), true,
				        aggregate.expressions[i]->IsVolatile()};
			}
			return {};
		}
		if (op.type == LogicalOperatorType::LOGICAL_GET || op.type == LogicalOperatorType::LOGICAL_DUMMY_SCAN) {
			return {true, false, false};
		}
		if (op.children.size() == 1 &&
		    (op.type == LogicalOperatorType::LOGICAL_FILTER || op.type == LogicalOperatorType::LOGICAL_ORDER_BY)) {
			return OutputProperties(*op.children[0], binding);
		}
		return {};
	}
	return {};
}

static bool HasCompositionBarrier(const Expression &expression) {
	if (expression.GetExpressionType() == ExpressionType::OPERATOR_TRY ||
	    expression.GetExpressionType() == ExpressionType::OPERATOR_HAS_ERROR ||
	    expression.GetExpressionType() == ExpressionType::OPERATOR_COALESCE ||
	    expression.GetExpressionClass() == ExpressionClass::BOUND_CASE ||
	    expression.GetExpressionClass() == ExpressionClass::BOUND_LAMBDA ||
	    expression.GetExpressionClass() == ExpressionClass::BOUND_WINDOW ||
	    expression.GetExpressionClass() == ExpressionClass::BOUND_UNNEST) {
		return true;
	}
	bool barrier = false;
	ExpressionIterator::EnumerateChildren(
	    expression, [&](const Expression &child) { barrier = barrier || HasCompositionBarrier(child); });
	return barrier;
}

static bool HasStageBarrier(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION || op.type == LogicalOperatorType::LOGICAL_ORDER_BY) {
		for (auto &expression : op.expressions) {
			if (HasCompositionBarrier(*expression)) {
				return true;
			}
		}
		return HasStageBarrier(*op.children[0]);
	}
	return op.type != LogicalOperatorType::LOGICAL_GET && op.type != LogicalOperatorType::LOGICAL_FILTER &&
	       op.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY;
}

static bool HasAggregateStage(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		return HasAggregateStage(*op.children[0]);
	}
	return op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY;
}

static void AddSubstitutions(BoundExpressionSQLExportState &state, const LogicalPlanSQLExportedChild &child,
                             const SelectNode &source) {
	for (idx_t i = 0; i < child.relation.fields.size(); i++) {
		state.substitutions.emplace(child.relation.fields[i].source_binding, *source.select_list[i]);
	}
}

optional_ptr<const SelectNode> LogicalPlanSQLExportHelpers::ComposeInput(LogicalOperator &op,
                                                                         const LogicalPlanSQLExportedChild &child,
                                                                         BoundExpressionSQLExportState &state) {
	auto source = CompositionScope(*child.relation.query);
	if (!source || !source->modifiers.empty() || !source->groups.group_expressions.empty() ||
	    !source->groups.grouping_sets.empty() || op.children[0]->type != LogicalOperatorType::LOGICAL_PROJECTION ||
	    HasStageBarrier(*op.children[0]) || HasAggregateStage(*op.children[0])) {
		return nullptr;
	}
	column_binding_map_t<idx_t> uses;
	for (auto &expression : CollectExpressions(op)) {
		if (HasCompositionBarrier(expression)) {
			return nullptr;
		}
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    expression, [&](const BoundColumnRefExpression &ref) { uses[ref.Binding()]++; });
	}
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		// Group keys occur in both the SELECT list and GROUP BY.
		for (auto &group : op.Cast<LogicalAggregate>().groups) {
			ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
			    *group, [&](const BoundColumnRefExpression &ref) { uses[ref.Binding()]++; });
		}
	}
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		// Combining filters must not expose either predicate to additional rows.
		if (source->where_clause) {
			return nullptr;
		}
		for (auto &binding : op.GetColumnBindings()) {
			uses[binding]++;
		}
	}
	for (idx_t i = 0; i < child.relation.fields.size(); i++) {
		if (IsLeaf(*source->select_list[i])) {
			continue;
		}
		auto binding = child.relation.fields[i].source_binding;
		auto properties = OutputProperties(*op.children[0], binding);
		if (!properties.safe || properties.aggregate || uses[binding] > 1) {
			return nullptr;
		}
	}
	AddSubstitutions(state, child, *source);
	return *source;
}

bool LogicalPlanSQLExportHelpers::ComposeProjection(LogicalProjection &projection, LogicalPlanSQLExportedChild &child,
                                                    SelectNode &select, const BoundExpressionSQLExportContext &context,
                                                    const LogicalPlanVerificationPath &path) {
	auto source = CompositionScope(*child.relation.query);
	if (!source) {
		return false;
	}
	bool identity = select.select_list.size() == child.relation.fields.size();
	for (idx_t i = 0; identity && i < select.select_list.size(); i++) {
		identity = select.select_list[i]->Equals(*ChildColumn(child, i));
	}
	if (identity) {
		MoveScope(select, *source);
		return true;
	}
	if (!source->modifiers.empty() || HasStageBarrier(*projection.children[0])) {
		return false;
	}
	column_binding_map_t<idx_t> uses;
	column_binding_set_t forwarded;
	const bool parent_effects = HasEffectfulExpressions(projection);
	for (auto &expression : projection.expressions) {
		if (HasCompositionBarrier(*expression)) {
			return false;
		}
		auto &semantic = *SemanticInput(*expression);
		if (semantic.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			forwarded.insert(semantic.Cast<BoundColumnRefExpression>().Binding());
		}
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    semantic, [&](const BoundColumnRefExpression &ref) { uses[ref.Binding()]++; });
	}
	idx_t old_size = 0;
	bool retains_aggregate = !source->groups.group_expressions.empty() || !source->groups.grouping_sets.empty();
	for (idx_t i = 0; i < child.relation.fields.size(); i++) {
		auto binding = child.relation.fields[i].source_binding;
		auto properties = OutputProperties(*projection.children[0], binding);
		auto count = uses[binding];
		if (!IsLeaf(*source->select_list[i])) {
			// Aggregate implementations can throw independently of their arguments.
			if (!count) {
				return false;
			}
			if (properties.volatile_expression || (!properties.safe && (count != 1 || !forwarded.count(binding)))) {
				return false;
			}
			if ((!properties.safe && !properties.aggregate && parent_effects) || (count > 1 && !properties.aggregate)) {
				return false;
			}
		}
		retains_aggregate = retains_aggregate || (properties.aggregate && count);
		old_size += ExpressionSize(*source->select_list[i]);
	}
	// A global aggregate must still produce one row for empty input.
	if (HasAggregateStage(*projection.children[0]) && !retains_aggregate) {
		return false;
	}
	vector<unique_ptr<ParsedExpression>> composed;
	idx_t new_size = 0;
	{
		BoundExpressionSQLExportState state(context);
		AddSubstitutions(state, child, *source);
		for (idx_t i = 0; i < projection.expressions.size(); i++) {
			old_size += ExpressionSize(*select.select_list[i]);
			auto expression = state.Export(*projection.expressions[i], PlanExpressionPath(path, i));
			if (expression.HasError()) {
				return false;
			}
			new_size += ExpressionSize(*expression.GetValue());
			expression.GetValue()->SetAlias(FieldIdentifier(i));
			composed.push_back(std::move(expression.GetValue()));
		}
	}
	if (new_size > old_size) {
		return false;
	}
	MoveScope(select, *source);
	select.select_list = std::move(composed);
	return true;
}

bool LogicalPlanSQLExportHelpers::ComposeOrder(LogicalOperator &op, LogicalPlanSQLExportedChild &child,
                                               SelectNode &select, const BoundExpressionSQLExportContext &context,
                                               const LogicalPlanVerificationPath &path) {
	auto source = CompositionScope(*child.relation.query);
	if (!source || !source->modifiers.empty() || HasStageBarrier(*op.children[0]) ||
	    select.select_list.size() != source->select_list.size() ||
	    (op.type == LogicalOperatorType::LOGICAL_TOP_N && HasAggregateStage(*op.children[0]))) {
		return false;
	}
	for (idx_t i = 0; i < child.relation.fields.size(); i++) {
		if (!select.select_list[i]->Equals(*ChildColumn(child, i))) {
			return false;
		}
		if (op.type == LogicalOperatorType::LOGICAL_TOP_N &&
		    !OutputProperties(*op.children[0], child.relation.fields[i].source_binding).safe) {
			return false;
		}
	}
	BoundExpressionSQLExportState state(context);
	AddSubstitutions(state, child, *source);
	auto expressions = CollectExpressions(op);
	auto &modifier = select.modifiers[0]->Cast<OrderModifier>();
	if (modifier.orders.size() != expressions.size()) {
		return false;
	}
	vector<unique_ptr<ParsedExpression>> orders;
	for (idx_t i = 0; i < expressions.size(); i++) {
		auto &expression = *SemanticInput(expressions[i].get());
		if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		auto &column = expression.Cast<BoundColumnRefExpression>();
		auto replacement = state.substitutions.find(column.Binding());
		// Literal order keys are positional or require order_by_non_integer_literal.
		if (replacement == state.substitutions.end() ||
		    replacement->second.get().GetExpressionClass() != ExpressionClass::COLUMN_REF ||
		    !OutputProperties(*op.children[0], column.Binding()).safe) {
			return false;
		}
		auto exported = state.Export(expressions[i], PlanExpressionPath(path, i));
		if (exported.HasError()) {
			return false;
		}
		auto resolved = context.resolve_binding(column.Binding());
		if (!resolved) {
			return false;
		}
		orders.push_back(SQLExportHelpers::OrderExpression(resolved->type, std::move(exported.GetValue())));
	}
	for (idx_t i = 0; i < orders.size(); i++) {
		modifier.orders[i].expression = std::move(orders[i]);
	}
	auto modifiers = std::move(select.modifiers);
	MoveScope(select, *source);
	select.modifiers = std::move(modifiers);
	return true;
}

} // namespace duckdb
