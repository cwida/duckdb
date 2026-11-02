#include "duckdb/planner/operator/logical_unconditional_join.hpp"
#include "duckdb/planner/operator/logical_join.hpp"
#include "duckdb/planner/sql_export/logical_plan_sql_exporter_internal.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"

namespace duckdb {

static string MarkConditionUnsupportedReason(const LogicalComparisonJoin &join) {
	if (join.HasNullSafeMarkJoinConditions()) {
		return string();
	}
	bool comparisons_only = !join.conditions.empty();
	bool all_equal = true;
	for (auto &condition : join.conditions) {
		if (!condition.IsComparison()) {
			comparisons_only = false;
			continue;
		}
		all_equal &= condition.GetComparisonType() == ExpressionType::COMPARE_EQUAL;
	}
	const bool has_supported_conjunction = join.conditions.size() == 1 || all_equal;
	if (!comparisons_only || !has_supported_conjunction) {
		return "The MARK condition requires conjunction execution semantics";
	}
	return string();
}

static bool CanReconstructMarkGroups(const LogicalComparisonJoin &join) {
	vector<LogicalType> group_types;
	if (!join.TryGetMarkJoinGroupTypes(group_types) || group_types.size() != join.mark_types.size()) {
		return false;
	}
	for (idx_t i = 0; i < group_types.size(); i++) {
		if (!group_types[i].EqualsIncludingCollation(join.mark_types[i])) {
			return false;
		}
	}
	return true;
}

static bool RequiresMarkGroupMetadata(const LogicalComparisonJoin &join) {
	if (join.join_type != JoinType::MARK || join.mark_types.empty()) {
		return false;
	}
	idx_t comparison_count = 0;
	bool tuple_comparison = false;
	bool all_equal = true;
	for (auto &condition : join.conditions) {
		if (!condition.IsComparison()) {
			continue;
		}
		comparison_count++;
		tuple_comparison |= condition.GetLHS().GetReturnType().id() == LogicalTypeId::TUPLE;
		all_equal &= condition.GetComparisonType() == ExpressionType::COMPARE_EQUAL;
	}
	if (comparison_count == join.mark_types.size() + 1) {
		return true;
	}
	// Retained types also disable uncorrelated row-equality NULL handling.
	return (comparison_count > 1 || tuple_comparison) && all_equal;
}

static LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>
ExportJoinCondition(LogicalJoin &op, LogicalPlanSQLExportContext &context,
                    const BoundExpressionSQLExportContext &expression_context,
                    const LogicalPlanVerificationPath &path) {
	using Result = LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>;
	unique_ptr<ParsedExpression> predicate;
	auto expressions = LogicalPlanSQLExportHelpers::CollectExpressions(op);
	if (op.type == LogicalOperatorType::LOGICAL_ANY_JOIN) {
		if (op.join_type == JoinType::MARK) {
			return Result::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
			    path, "mark_condition_semantics", "The MARK condition requires conjunction execution semantics")});
		}
		auto exported = context.ExportExpression(op, expressions, 0, expression_context, path);
		if (exported.HasError()) {
			return Result::Failure(exported);
		}
		predicate = std::move(exported.GetValue());
	} else {
		auto &comparison = op.Cast<LogicalComparisonJoin>();
		if (!comparison.duplicate_eliminated_columns.empty()) {
			return Result::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
			    path, "join_delim_state", "The join requires a duplicate-eliminated input scope")});
		}
		if (comparison.join_type == JoinType::MARK && !CanReconstructMarkGroups(comparison)) {
			auto reason = MarkConditionUnsupportedReason(comparison);
			if (!reason.empty()) {
				return Result::Failure(
				    {LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(path, "mark_condition_semantics", reason)});
			}
			if (RequiresMarkGroupMetadata(comparison)) {
				return Result::Failure({LogicalPlanSQLExportHelpers::PlanUnsupportedFeature(
				    path, "mark_group_null_semantics", "The MARK join requires its group-specific NULL semantics")});
			}
		}
		idx_t ordinal = 0;
		for (auto &condition : comparison.conditions) {
			auto lhs = context.ExportExpression(op, expressions, ordinal++, expression_context, path);
			if (lhs.HasError()) {
				return Result::Failure(lhs);
			}
			auto conjunct = std::move(lhs.GetValue());
			if (condition.IsComparison()) {
				auto rhs = context.ExportExpression(op, expressions, ordinal++, expression_context, path);
				if (rhs.HasError()) {
					return Result::Failure(rhs);
				}
				conjunct = make_uniq<ComparisonExpression>(condition.GetComparisonType(), std::move(conjunct),
				                                           std::move(rhs.GetValue()));
			}
			predicate = SQLExportHelpers::Conjoin(std::move(predicate), std::move(conjunct));
		}
		if (!predicate) {
			predicate = ConstantExpression::FromValue(Value::BOOLEAN(true));
		}
	}
	return Result::Success(std::move(predicate));
}

static LogicalPlanSQLExportResult ExportJoin(LogicalOperator &op, LogicalPlanSQLExportContext &context,
                                             const LogicalPlanVerificationPath &path) {
	D_ASSERT(op.children.size() == 2);
	auto fields = LogicalPlanSQLExportHelpers::CreateFields(op, path);
	if (fields.HasError()) {
		return LogicalPlanSQLExportResult::Failure(fields);
	}
	auto left = context.ExportChild(*op.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
	if (left.HasError()) {
		return LogicalPlanSQLExportResult::Failure(left);
	}
	auto right = context.ExportChild(*op.children[1], LogicalPlanSQLExportHelpers::PlanChildPath(path, 1));
	if (right.HasError()) {
		return LogicalPlanSQLExportResult::Failure(right);
	}
	vector<reference<const LogicalPlanSQLExportedChild>> children {left.GetValue(), right.GetValue()};
	LogicalPlanSQLExportHelpers::PropagateSemanticTypes(fields.GetValue(), children);
	auto left_plain = LogicalPlanSQLExportHelpers::PlainScope(*left.GetValue().relation.query);
	auto right_plain = LogicalPlanSQLExportHelpers::PlainScope(*right.GetValue().relation.query);
	if (left_plain && left_plain->where_clause) {
		left_plain = nullptr;
	}
	if (right_plain && right_plain->where_clause) {
		right_plain = nullptr;
	}
	identifier_set_t left_aliases, right_aliases;
	if (left_plain) {
		LogicalPlanSQLExportHelpers::CollectScopeAliases(*left_plain->from_table, left_aliases);
		if (left_aliases.count(right.GetValue().relation_alias)) {
			left_plain = nullptr;
		}
	}
	if (right_plain) {
		LogicalPlanSQLExportHelpers::CollectScopeAliases(*right_plain->from_table, right_aliases);
		if (right_aliases.count(left.GetValue().relation_alias)) {
			right_plain = nullptr;
		}
	}
	if (left_plain && right_plain) {
		for (auto &alias : left_aliases) {
			if (right_aliases.count(alias)) {
				right_plain = nullptr;
				break;
			}
		}
	}
	auto expression_context = LogicalPlanSQLExportHelpers::CreateBindingContext(context.GetClientContext(), children,
	                                                                            {left_plain, right_plain});
	auto join = make_uniq<JoinRef>();
	optional_ptr<LogicalJoin> logical_join;
	if (op.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		join->ref_type = JoinRefType::CROSS;
	} else if (op.type == LogicalOperatorType::LOGICAL_POSITIONAL_JOIN) {
		join->ref_type = JoinRefType::POSITIONAL;
	} else {
		logical_join = op.Cast<LogicalJoin>();
		join->type = logical_join->join_type;
		if (op.type == LogicalOperatorType::LOGICAL_ASOF_JOIN) {
			join->ref_type = JoinRefType::ASOF;
		}
		auto condition = ExportJoinCondition(*logical_join, context, expression_context, path);
		if (condition.HasError()) {
			return LogicalPlanSQLExportResult::Failure(condition);
		}
		join->condition = std::move(condition.GetValue());
	}
	auto select = make_uniq<SelectNode>();
	for (auto &field : fields.GetValue()) {
		unique_ptr<ParsedExpression> expression;
		if (logical_join && logical_join->join_type == JoinType::MARK &&
		    field.source_binding.table_index == logical_join->mark_index) {
			expression = make_uniq<ColumnRefExpression>(Identifier("__mark_join_marker"));
		} else {
			auto resolved = expression_context.resolve_binding(field.source_binding);
			if (!resolved || resolved->type != field.type) {
				auto issue = SQLExportHelpers::MakeIssue(
				    LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT, LogicalPlanVerificationPhase::PLAN_EXPORT,
				    path, LogicalPlanVerificationConstructIdentity::LogicalOperator(op.type),
				    "Join output field has no matching SQL column binding");
				return LogicalPlanSQLExportResult::Failure({std::move(issue)});
			}
			expression = make_uniq<ColumnRefExpression>(resolved->names);
		}
		expression->SetAlias(LogicalPlanSQLExportHelpers::FieldIdentifier(select->select_list.size()));
		select->select_list.push_back(std::move(expression));
	}
	join->left = left_plain ? std::move(left.GetValue().relation.query->Cast<SelectNode>().from_table)
	                        : LogicalPlanSQLExportHelpers::CreateSubquery(std::move(left.GetValue()));
	join->right = right_plain ? std::move(right.GetValue().relation.query->Cast<SelectNode>().from_table)
	                          : LogicalPlanSQLExportHelpers::CreateSubquery(std::move(right.GetValue()));
	select->from_table = std::move(join);
	return LogicalPlanSQLExportResult::Success({std::move(select), std::move(fields.GetValue())});
}

LogicalPlanVerificationResult<LogicalPlanSQLExportRelation>
LogicalJoin::ToSQL(LogicalPlanSQLExportContext &context, const LogicalPlanVerificationPath &path) {
	if (type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN && type != LogicalOperatorType::LOGICAL_ANY_JOIN &&
	    type != LogicalOperatorType::LOGICAL_ASOF_JOIN) {
		return LogicalOperator::ToSQL(context, path);
	}
	return ExportJoin(*this, context, path);
}

LogicalPlanVerificationResult<LogicalPlanSQLExportRelation>
LogicalUnconditionalJoin::ToSQL(LogicalPlanSQLExportContext &context, const LogicalPlanVerificationPath &path) {
	if (type != LogicalOperatorType::LOGICAL_CROSS_PRODUCT && type != LogicalOperatorType::LOGICAL_POSITIONAL_JOIN) {
		return LogicalOperator::ToSQL(context, path);
	}
	return ExportJoin(*this, context, path);
}

} // namespace duckdb
