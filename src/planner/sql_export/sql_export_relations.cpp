#include "duckdb/planner/operator/logical_unnest.hpp"
#include "duckdb/planner/sql_export/bound_expression_sql_exporter_internal.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/sql_export/logical_plan_sql_exporter_internal.hpp"
#include "duckdb/function/scalar/compressed_materialization_utils.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_sample.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"

namespace duckdb {

static LogicalType SemanticExpressionType(const Expression &expression,
                                          const BoundExpressionSQLExportContext &context) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF && context.resolve_binding) {
		auto &column = expression.Cast<BoundColumnRefExpression>();
		auto resolved = context.resolve_binding(column.Binding());
		if (resolved) {
			const bool matches_optimizer_type =
			    resolved->optimizer_type && *resolved->optimizer_type == column.GetReturnType();
			if (resolved->type == column.GetReturnType() || matches_optimizer_type) {
				return resolved->type;
			}
		}
	}
	if (context.discard_optimizer_metadata) {
		if (auto wrapped = CMUtils::GetWrappedInput(expression)) {
			return SemanticExpressionType(*wrapped, context);
		}
	}
	return expression.GetReturnType();
}

static void ApplySemanticType(LogicalPlanSQLExportField &field, const Expression &expression,
                              const BoundExpressionSQLExportContext &context) {
	auto semantic_type = SemanticExpressionType(expression, context);
	if (semantic_type == field.type) {
		return;
	}
	field.optimizer_type = field.type;
	field.type = std::move(semantic_type);
}

static bool HasSafePredicates(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_EXPRESSION_GET) {
		return !LogicalPlanSQLExportHelpers::HasEffectfulExpressions(op) &&
		       (op.children[0]->type == LogicalOperatorType::LOGICAL_DUMMY_SCAN || HasSafePredicates(*op.children[0]));
	}
	if (op.type != LogicalOperatorType::LOGICAL_FILTER && op.type != LogicalOperatorType::LOGICAL_PROJECTION) {
		return false;
	}
	for (auto &expression : op.expressions) {
		if (expression->IsVolatile() || expression->CanThrow()) {
			return false;
		}
	}
	return HasSafePredicates(*op.children[0]);
}

template <class EXPRESSION, class EXPORTER>
static LogicalPlanSQLExportResult ExportContextExpressions(LogicalOperator &op, LogicalPlanSQLExportContext &context,
                                                           const LogicalPlanVerificationPath &path,
                                                           vector<LogicalPlanSQLExportField> fields,
                                                           EXPORTER exporter) {
	D_ASSERT(op.children.size() == 1);
	auto child = context.ExportChild(*op.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return LogicalPlanSQLExportResult::Failure(child);
	}
	LogicalPlanSQLExportHelpers::PropagateSemanticTypes(fields, {child.GetValue()});
	auto plain = LogicalPlanSQLExportHelpers::PlainScope(*child.GetValue().relation.query);
	auto select = make_uniq<SelectNode>();
	for (idx_t i = 0; i < child.GetValue().relation.fields.size(); i++) {
		select->select_list.push_back(LogicalPlanSQLExportHelpers::ChildColumn(child.GetValue(), i, plain));
	}
	auto expression_context =
	    LogicalPlanSQLExportHelpers::CreateBindingContext(context.GetClientContext(), {child.GetValue()}, {plain});
	for (idx_t i = 0; i < op.expressions.size(); i++) {
		auto expression_path = LogicalPlanSQLExportHelpers::PlanExpressionPath(path, i);
		auto expression = exporter(op.expressions[i]->Cast<EXPRESSION>(), expression_context, expression_path);
		if (expression.HasError()) {
			return LogicalPlanSQLExportResult::Failure(expression);
		}
		expression.GetValue()->SetAlias(LogicalPlanSQLExportHelpers::FieldIdentifier(select->select_list.size()));
		select->select_list.push_back(std::move(expression.GetValue()));
	}
	LogicalPlanSQLExportHelpers::SetChildScope(*select, std::move(child.GetValue()), plain);
	return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields)});
}

LogicalPlanSQLExportResult LogicalFilter::ToSQL(LogicalPlanSQLExportContext &export_context,
                                                const LogicalPlanVerificationPath &path) {
	auto &filter = *this;
	D_ASSERT(filter.children.size() == 1);
#ifdef D_ASSERT_IS_ENABLED
	for (auto &expression : filter.expressions) {
		D_ASSERT(expression && expression->GetReturnType() == LogicalType::BOOLEAN);
	}
#endif
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(filter, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	auto child = export_context.ExportChild(*filter.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return LogicalPlanSQLExportResult::Failure(child);
	}
	LogicalPlanSQLExportHelpers::PropagateSemanticTypes(fields.GetValue(), {child.GetValue()});
	auto plain = LogicalPlanSQLExportHelpers::PlainScope(*child.GetValue().relation.query);
	if (plain && plain->where_clause && !HasSafePredicates(*filter.children[0])) {
		plain = nullptr;
	}
	if (plain && plain->where_clause) {
		for (auto &predicate : filter.expressions) {
			if (predicate->IsVolatile() || predicate->CanThrow()) {
				plain = nullptr;
				break;
			}
		}
	}
	vector<reference<const LogicalPlanSQLExportedChild>> child_references {child.GetValue()};
	auto expression_context =
	    LogicalPlanSQLExportHelpers::CreateBindingContext(export_context.GetClientContext(), child_references, {plain});
	optional_ptr<const SelectNode> composed;
	auto expressions = LogicalPlanSQLExportHelpers::CollectExpressions(filter);
	vector<unique_ptr<ParsedExpression>> predicates;
	{
		BoundExpressionSQLExportState composition(expression_context);
		composed = plain ? nullptr : LogicalPlanSQLExportHelpers::ComposeInput(filter, child.GetValue(), composition);
		for (idx_t expression_index = 0; expression_index < filter.expressions.size(); expression_index++) {
			auto predicate = export_context.ExportExpression(filter, expressions, expression_index, expression_context,
			                                                 path, composed ? &composition : nullptr);
			if (predicate.HasError()) {
				return LogicalPlanSQLExportResult::Failure(predicate);
			}
			predicates.push_back(std::move(predicate.GetValue()));
		}
	}
	auto select = make_uniq<SelectNode>();
	for (idx_t field_index = 0; field_index < fields.GetValue().size(); field_index++) {
		auto child_field_index =
		    filter.projection_map.empty() ? field_index : filter.projection_map[field_index].GetIndexUnsafe();
		auto expression =
		    LogicalPlanSQLExportHelpers::ChildColumn(child.GetValue(), child_field_index, composed ? composed : plain);
		expression->SetAlias(LogicalPlanSQLExportHelpers::FieldIdentifier(field_index));
		select->select_list.push_back(std::move(expression));
	}
	select->where_clause = SQLExportHelpers::Conjoin(std::move(predicates));
	LogicalPlanSQLExportHelpers::SetChildScope(*select, std::move(child.GetValue()), composed ? composed : plain);
	LogicalPlanSQLExportRelation relation {std::move(select), std::move(fields.GetValue())};
	return LogicalPlanSQLExportResult::Success(std::move(relation));
}

LogicalPlanSQLExportResult LogicalProjection::ToSQL(LogicalPlanSQLExportContext &export_context,
                                                    const LogicalPlanVerificationPath &path) {
	auto &projection = *this;
	D_ASSERT(projection.children.size() == 1 && !projection.expressions.empty());
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(projection, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	D_ASSERT(projection.expressions.size() == fields.GetValue().size());
	bool fromless = projection.children[0]->type == LogicalOperatorType::LOGICAL_DUMMY_SCAN &&
	                !export_context.FindSource(*projection.children[0]);
	auto child =
	    export_context.ExportChild(*projection.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return LogicalPlanSQLExportResult::Failure(child);
	}
	if (LogicalPlanSQLExportHelpers::IsIdentityProjection(projection, child.GetValue().relation.fields)) {
		return LogicalPlanSQLExportResult::Success(
		    {std::move(child.GetValue().relation.query), std::move(fields.GetValue())});
	}
	auto plain = LogicalPlanSQLExportHelpers::PlainScope(*child.GetValue().relation.query);
	vector<reference<const LogicalPlanSQLExportedChild>> child_references {child.GetValue()};
	auto expression_context =
	    LogicalPlanSQLExportHelpers::CreateBindingContext(export_context.GetClientContext(), child_references, {plain});
	auto expressions = LogicalPlanSQLExportHelpers::CollectExpressions(projection);
	auto select = make_uniq<SelectNode>();
	for (idx_t expression_index = 0; expression_index < projection.expressions.size(); expression_index++) {
		fromless = fromless && projection.expressions[expression_index]->IsScalar();
		ApplySemanticType(fields.GetValue()[expression_index], *projection.expressions[expression_index],
		                  expression_context);
		auto expression =
		    export_context.ExportExpression(projection, expressions, expression_index, expression_context, path);
		if (expression.HasError()) {
			return LogicalPlanSQLExportResult::Failure(expression);
		}
		expression.GetValue()->SetAlias(LogicalPlanSQLExportHelpers::FieldIdentifier(expression_index));
		select->select_list.push_back(std::move(expression.GetValue()));
	}
	if (!plain && !fromless &&
	    LogicalPlanSQLExportHelpers::ComposeProjection(projection, child.GetValue(), *select, expression_context,
	                                                   path)) {
		return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields.GetValue())});
	}
	if (fromless) {
		select->from_table = make_uniq<EmptyTableRef>();
	} else {
		LogicalPlanSQLExportHelpers::SetChildScope(*select, std::move(child.GetValue()), plain);
	}
	LogicalPlanSQLExportRelation relation {std::move(select), std::move(fields.GetValue())};
	return LogicalPlanSQLExportResult::Success(std::move(relation));
}

static LogicalPlanVerificationResult<unique_ptr<OrderModifier>>
ExportOrderModifier(LogicalOperator &op, LogicalPlanSQLExportContext &context, const LogicalPlanVerificationPath &path,
                    const vector<BoundOrderByNode> &orders, const BoundExpressionSQLExportContext &expression_context,
                    idx_t expression_ordinal) {
	using Result = LogicalPlanVerificationResult<unique_ptr<OrderModifier>>;
	auto expressions = LogicalPlanSQLExportHelpers::CollectExpressions(op);
	auto modifier = make_uniq<OrderModifier>();
	for (auto &order : orders) {
		auto expression = context.ExportExpression(op, expressions, expression_ordinal++, expression_context, path);
		if (expression.HasError()) {
			return Result::Failure(expression);
		}
		if (SQLExportHelpers::IsConstantOrderKey(*order.expression, *expression.GetValue())) {
			continue;
		}
		modifier->orders.emplace_back(
		    order.type, order.null_order,
		    SQLExportHelpers::OrderExpression(SemanticExpressionType(*order.expression, expression_context),
		                                      std::move(expression.GetValue())));
	}
	return Result::Success(std::move(modifier));
}

static LogicalPlanSQLExportResult ExportOrderedRelation(LogicalOperator &op, LogicalPlanSQLExportContext &context,
                                                        const LogicalPlanVerificationPath &path,
                                                        const vector<BoundOrderByNode> &orders) {
	D_ASSERT(op.children.size() == 1);
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(op, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	auto child = context.ExportChild(*op.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return LogicalPlanSQLExportResult::Failure(child);
	}
	LogicalPlanSQLExportHelpers::PropagateSemanticTypes(fields.GetValue(), {child.GetValue()});
	auto plain = LogicalPlanSQLExportHelpers::PlainScope(*child.GetValue().relation.query);
	auto expression_context =
	    LogicalPlanSQLExportHelpers::CreateBindingContext(context.GetClientContext(), {child.GetValue()}, {plain});
	auto select = context.ForwardFields(child.GetValue(), fields.GetValue(), plain);
	auto modifier = ExportOrderModifier(op, context, path, orders, expression_context, 0);
	if (modifier.HasError()) {
		return LogicalPlanSQLExportResult::Failure(modifier);
	}
	if (modifier.GetValue()->orders.empty()) {
		LogicalPlanSQLExportHelpers::SetChildScope(*select, std::move(child.GetValue()), plain);
		return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields.GetValue())});
	}
	select->modifiers.push_back(std::move(modifier.GetValue()));
	if (!plain && LogicalPlanSQLExportHelpers::ComposeOrder(op, child.GetValue(), *select, expression_context, path)) {
		return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields.GetValue())});
	}
	LogicalPlanSQLExportHelpers::SetChildScope(*select, std::move(child.GetValue()), plain);
	return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields.GetValue())});
}

LogicalPlanSQLExportResult LogicalSample::ToSQL(LogicalPlanSQLExportContext &export_context,
                                                const LogicalPlanVerificationPath &path) {
	auto &sample = *this;
	D_ASSERT(sample.children.size() == 1 && sample.sample_options);
	auto &sampling = *sample.sample_options;
	if (sampling.seed.IsValid() != sampling.repeatable) {
		return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
		    path, "sample_repeatability", "SQL sampling seeds imply repeatable sampling")});
	}
	if (sampling.seed.IsValid() && sampling.seed.GetIndex() > idx_t(NumericLimits<int64_t>::Maximum())) {
		return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
		    path, "sample_seed", "The sampling seed has no SQL spelling")});
	}
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(sample, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	auto child = export_context.ExportChild(*sample.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return LogicalPlanSQLExportResult::Failure(child);
	}
	auto select = export_context.ForwardFields(child.GetValue(), fields.GetValue());
	select->sample = sampling.Copy();
	select->from_table = LogicalPlanSQLExportHelpers::CreateSubquery(std::move(child.GetValue()));
	return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields.GetValue())});
}

LogicalPlanSQLExportResult LogicalSetOperation::ToSQL(LogicalPlanSQLExportContext &export_context,
                                                      const LogicalPlanVerificationPath &path) {
	auto &op = *this;
	D_ASSERT(!op.children.empty());
	D_ASSERT(op.type == LogicalOperatorType::LOGICAL_UNION || op.children.size() == 2);
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(op, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	auto query = make_uniq<SetOperationNode>();
	query->setop_all = op.setop_all;
	query->setop_type = op.type == LogicalOperatorType::LOGICAL_UNION    ? SetOperationType::UNION
	                    : op.type == LogicalOperatorType::LOGICAL_EXCEPT ? SetOperationType::EXCEPT
	                                                                     : SetOperationType::INTERSECT;
	for (idx_t i = 0; i < op.children.size(); i++) {
		auto child = export_context.Export(*op.children[i], LogicalPlanSQLExportHelpers::PlanChildPath(path, i));
		if (child.HasError()) {
			return LogicalPlanSQLExportResult::Failure(child);
		}
		D_ASSERT(child.GetValue().fields.size() == fields.GetValue().size());
		query->children.push_back(std::move(child.GetValue().query));
	}
	if (op.children.size() == 1) {
		// Retain the UNION boundary when optimization removed every other arm.
		LogicalEmptyResult empty(op.types, op.GetColumnBindings());
		empty.ResolveOperatorTypes();
		auto exported = export_context.Export(empty, path);
		if (exported.HasError()) {
			return exported;
		}
		query->children.push_back(std::move(exported.GetValue().query));
	}
	return LogicalPlanSQLExportResult::Success({std::move(query), std::move(fields.GetValue())});
}

LogicalPlanSQLExportResult LogicalAggregate::ToSQL(LogicalPlanSQLExportContext &export_context,
                                                   const LogicalPlanVerificationPath &path) {
	auto &aggregate = *this;
	D_ASSERT(aggregate.children.size() == 1);
#ifdef D_ASSERT_IS_ENABLED
	for (auto &expression : aggregate.expressions) {
		D_ASSERT(expression && expression->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
	}
#endif
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(aggregate, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	D_ASSERT(aggregate.groups.size() + aggregate.expressions.size() + aggregate.grouping_functions.size() ==
	         fields.GetValue().size());
	auto child =
	    export_context.ExportChild(*aggregate.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return LogicalPlanSQLExportResult::Failure(child);
	}
	auto plain = LogicalPlanSQLExportHelpers::PlainScope(*child.GetValue().relation.query);
	vector<reference<const LogicalPlanSQLExportedChild>> child_references {child.GetValue()};
	auto expression_context =
	    LogicalPlanSQLExportHelpers::CreateBindingContext(export_context.GetClientContext(), child_references, {plain});
	auto expressions = LogicalPlanSQLExportHelpers::CollectExpressions(aggregate);
	for (idx_t group_index = 0; group_index < aggregate.groups.size(); group_index++) {
		ApplySemanticType(fields.GetValue()[group_index], *aggregate.groups[group_index], expression_context);
	}
	auto input_field_count = child.GetValue().relation.fields.size();
	const bool stage_groups = !LogicalPlanSQLExportHelpers::HasSimpleGroups(aggregate);
	if (stage_groups && !aggregate.groups.empty()) {
		// Equal group expressions can still occupy distinct grouping-set positions.
		auto input = make_uniq<SelectNode>();
		auto input_fields = child.GetValue().relation.fields;
		for (idx_t i = 0; i < input_field_count; i++) {
			auto expression = LogicalPlanSQLExportHelpers::ChildColumn(child.GetValue(), i, plain);
			expression->SetAlias(LogicalPlanSQLExportHelpers::FieldIdentifier(i));
			input->select_list.push_back(std::move(expression));
		}
		for (idx_t i = 0; i < aggregate.groups.size(); i++) {
			auto expression = export_context.ExportExpression(aggregate, expressions, i, expression_context, path);
			if (expression.HasError()) {
				return LogicalPlanSQLExportResult::Failure(expression);
			}
			expression.GetValue()->SetAlias(LogicalPlanSQLExportHelpers::FieldIdentifier(input_fields.size()));
			input->select_list.push_back(std::move(expression.GetValue()));
			input_fields.push_back(fields.GetValue()[i]);
		}
		LogicalPlanSQLExportHelpers::SetChildScope(*input, std::move(child.GetValue()), plain);
		child.GetValue() = {{std::move(input), std::move(input_fields)}, export_context.NextRelationAlias()};
		plain = nullptr;
		expression_context =
		    LogicalPlanSQLExportHelpers::CreateBindingContext(export_context.GetClientContext(), {child.GetValue()});
	}
	optional_ptr<const SelectNode> composed;
	auto select = make_uniq<SelectNode>();
	{
		BoundExpressionSQLExportState composition(expression_context);
		composed = !plain && !stage_groups
		               ? LogicalPlanSQLExportHelpers::ComposeInput(aggregate, child.GetValue(), composition)
		               : nullptr;
		for (idx_t expression_index = 0; expression_index < expressions.size(); expression_index++) {
			unique_ptr<ParsedExpression> result;
			if (expression_index < aggregate.groups.size() && stage_groups) {
				result =
				    LogicalPlanSQLExportHelpers::ChildColumn(child.GetValue(), input_field_count + expression_index);
			} else {
				auto expression =
				    export_context.ExportExpression(aggregate, expressions, expression_index, expression_context, path,
				                                    composed ? &composition : nullptr);
				if (expression.HasError()) {
					return LogicalPlanSQLExportResult::Failure(expression);
				}
				result = std::move(expression.GetValue());
			}
			if (expression_index < aggregate.groups.size()) {
				select->groups.group_expressions.push_back(result->Copy());
			}
			result->SetAlias(LogicalPlanSQLExportHelpers::FieldIdentifier(expression_index));
			select->select_list.push_back(std::move(result));
		}
	}
	select->groups.grouping_sets = aggregate.grouping_sets;
	if (select->groups.grouping_sets.empty() && !aggregate.groups.empty()) {
		GroupingSet all_groups;
		for (idx_t i = 0; i < aggregate.groups.size(); i++) {
			all_groups.insert(ProjectionIndex(i));
		}
		select->groups.grouping_sets.push_back(std::move(all_groups));
	}
	if (select->groups.grouping_sets.empty() && aggregate.expressions.empty()) {
		// GROUP BY () keeps an aggregate without groups or aggregates producing a single row
		select->groups.grouping_sets.emplace_back();
	}
	for (auto &grouping : aggregate.grouping_functions) {
		vector<unique_ptr<ParsedExpression>> arguments;
		for (auto index : grouping) {
			arguments.push_back(select->groups.group_expressions[index]->Copy());
		}
		auto expression = make_uniq<OperatorExpression>(ExpressionType::GROUPING_FUNCTION, std::move(arguments));
		expression->SetAlias(LogicalPlanSQLExportHelpers::FieldIdentifier(select->select_list.size()));
		select->select_list.push_back(std::move(expression));
	}
	LogicalPlanSQLExportHelpers::SetChildScope(*select, std::move(child.GetValue()), composed ? composed : plain);
	LogicalPlanSQLExportRelation relation {std::move(select), std::move(fields.GetValue())};
	return LogicalPlanSQLExportResult::Success(std::move(relation));
}

LogicalPlanSQLExportResult LogicalDistinct::ToSQL(LogicalPlanSQLExportContext &context,
                                                  const LogicalPlanVerificationPath &path) {
	auto &op = *this;
	D_ASSERT(op.children.size() == 1);
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(op, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	auto child = context.ExportChild(*op.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (child.HasError()) {
		return LogicalPlanSQLExportResult::Failure(child);
	}
	LogicalPlanSQLExportHelpers::PropagateSemanticTypes(fields.GetValue(), {child.GetValue()});
	auto expression_context =
	    LogicalPlanSQLExportHelpers::CreateBindingContext(context.GetClientContext(), {child.GetValue()});
	auto select = context.ForwardFields(child.GetValue(), fields.GetValue());
	auto expressions = LogicalPlanSQLExportHelpers::CollectExpressions(op);
	auto modifier = make_uniq<DistinctModifier>();
	idx_t expression_ordinal = 0;
	// DISTINCT targets include optimizer-pruned full-row DISTINCT.
	for (idx_t i = 0; i < distinct_targets.size(); i++) {
		auto expression = context.ExportExpression(op, expressions, expression_ordinal++, expression_context, path);
		if (expression.HasError()) {
			return LogicalPlanSQLExportResult::Failure(expression);
		}
		modifier->distinct_on_targets.push_back(std::move(expression.GetValue()));
	}
	select->modifiers.push_back(std::move(modifier));
	if (order_by) {
		auto order = ExportOrderModifier(op, context, path, order_by->orders, expression_context, expression_ordinal);
		if (order.HasError()) {
			return LogicalPlanSQLExportResult::Failure(order);
		}
		if (!order.GetValue()->orders.empty()) {
			select->modifiers.push_back(std::move(order.GetValue()));
		}
	}
	select->from_table = LogicalPlanSQLExportHelpers::CreateSubquery(std::move(child.GetValue()));
	return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields.GetValue())});
}

LogicalPlanSQLExportResult LogicalOrder::ToSQL(LogicalPlanSQLExportContext &context,
                                               const LogicalPlanVerificationPath &path) {
	return ExportOrderedRelation(*this, context, path, orders);
}

LogicalPlanSQLExportResult LogicalTopN::ToSQL(LogicalPlanSQLExportContext &context,
                                              const LogicalPlanVerificationPath &path) {
	auto result = ExportOrderedRelation(*this, context, path, orders);
	if (result.HasError()) {
		return result;
	}
	auto modifier = make_uniq<LimitModifier>();
	modifier->limit = ConstantExpression::FromValue(Value::BIGINT(NumericCast<int64_t>(limit)));
	auto original_offset = unpruned_offset.IsValid() ? unpruned_offset.GetIndex() : offset;
	modifier->offset = ConstantExpression::FromValue(Value::BIGINT(NumericCast<int64_t>(original_offset)));
	result.GetValue().query->modifiers.push_back(std::move(modifier));
	return result;
}

LogicalPlanSQLExportResult LogicalWindow::ToSQL(LogicalPlanSQLExportContext &context,
                                                const LogicalPlanVerificationPath &path) {
	auto &op = *this;
	D_ASSERT(children.size() == 1);
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(op, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	// A replaced input exports as a named source, so its ordinality fusion does not apply.
	if (op.children[0]->type == LogicalOperatorType::LOGICAL_GET && !context.FindSource(*op.children[0])) {
		auto &get = op.children[0]->Cast<LogicalGet>();
		if (get.source_ordinality == OrdinalityType::WITH_ORDINALITY && !get.ordinality_idx.IsValid()) {
			bool supported =
			    op.expressions.size() == 1 && !get.table_filters.HasFilters() && !get.extra_info.sample_options;
			if (supported) {
				auto &window = op.expressions[0]->Cast<BoundWindowExpression>();
				supported = window.GetExpressionType() == ExpressionType::WINDOW_ROW_NUMBER &&
				            window.Partitions().empty() && window.OrderBy().empty() && window.GetChildren().empty();
			}
			if (!supported) {
				return LogicalPlanSQLExportResult::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
				    path, "ordinality_window", "The source ordinality cannot be reconstructed through this window")});
			}
			return get.ExportSQLSource(context, LogicalPlanSQLExportHelpers::PlanChildPath(path, 0),
			                           &fields.GetValue().back());
		}
	}
	return ExportContextExpressions<BoundWindowExpression>(*this, context, path, std::move(fields.GetValue()),
	                                                       BoundExpressionSQLExporter::ExportWindowAtPath);
}

LogicalPlanSQLExportResult LogicalUnnest::ToSQL(LogicalPlanSQLExportContext &context,
                                                const LogicalPlanVerificationPath &path) {
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(*this, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	return ExportContextExpressions<BoundUnnestExpression>(*this, context, path, std::move(fields.GetValue()),
	                                                       BoundExpressionSQLExporter::ExportUnnestAtPath);
}

} // namespace duckdb
