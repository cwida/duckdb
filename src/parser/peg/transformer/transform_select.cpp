#include "duckdb/common/enum_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/parser/peg/ast/distinct_clause.hpp"
#include "duckdb/parser/peg/ast/join_prefix.hpp"
#include "duckdb/parser/peg/ast/join_qualifier.hpp"
#include "duckdb/parser/peg/ast/limit_percent_result.hpp"
#include "duckdb/parser/peg/ast/table_alias.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/match_recognize_ref.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/tableref/at_clause.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/copy_statement.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/query_node/copy_query_node.hpp"

namespace duckdb {

static bool IsDMLCTEQueryNode(QueryNodeType type) {
	return type == QueryNodeType::INSERT_QUERY_NODE || type == QueryNodeType::UPDATE_QUERY_NODE ||
	       type == QueryNodeType::DELETE_QUERY_NODE;
}

static void ValidateRecursiveCTEQueryNode(const QueryNode &query_node) {
	if (query_node.type == QueryNodeType::COPY_QUERY_NODE) {
		throw ParserException("Recursive CTEs with COPY statements are not supported");
	}
	if (IsDMLCTEQueryNode(query_node.type)) {
		throw ParserException("Recursive CTEs with DML statements are not supported");
	}
}

unique_ptr<SQLStatement>
PEGTransformerFactory::TransformSelectStatement(PEGTransformer &transformer,
                                                unique_ptr<SelectStatement> select_statement_internal) {
	return std::move(select_statement_internal);
}

static void PushSelectStatementInternalRemainder(GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	auto &result_modifiers_opt = list_pr.Child<OptionalParseResult>(2);
	if (result_modifiers_opt.HasResult()) {
		process.PushChild({result_modifiers_opt.GetResult()}, 2);
	}
	process.PushChild({list_pr.GetChild(1)}, 1);
}

void PEGTransformerFactory::InitializeSelectStatementInternalTrampoline(PEGTransformer &transformer,
                                                                        GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	process.ReserveChildSlots(3);
	auto &with_clause_opt = list_pr.Child<OptionalParseResult>(0);
	if (with_clause_opt.HasResult()) {
		process.manual_state = 0;
		process.PushChild({with_clause_opt.GetResult()}, 0);
		return;
	}
	process.manual_state = 1;
	PushSelectStatementInternalRemainder(process);
}

arena_ptr<TransformResultValue>
PEGTransformerFactory::FinalizeSelectStatementInternalTrampoline(PEGTransformer &transformer,
                                                                 GeneratedTransformProcess &process) {
	if (process.manual_state == 0) {
		auto &cte_map = process.GetResult<CommonTableExpressionMap>(0);
		if (!cte_map.map.empty()) {
			transformer.stored_cte_map.push_back(cte_map);
		}
		process.manual_state = 1;
		PushSelectStatementInternalRemainder(process);
		return nullptr;
	}

	CommonTableExpressionMap cte_map;
	if (process.child_results[0]) {
		if (!process.GetResult<CommonTableExpressionMap>(0).map.empty()) {
			// the CTEs are visible while transforming the select statement and its result modifiers
			transformer.stored_cte_map.pop_back();
		}
		cte_map = process.TakeResult<CommonTableExpressionMap>(0);
	}
	auto select_statement = process.TakeResult<unique_ptr<SelectStatement>>(1);
	if (!cte_map.map.empty()) {
		select_statement->node->cte_map = std::move(cte_map);
	}
	if (process.child_results[2]) {
		auto result_modifiers = process.TakeResult<vector<unique_ptr<ResultModifier>>>(2);
		for (auto &result_modifier : result_modifiers) {
			select_statement->node->modifiers.push_back(std::move(result_modifier));
		}
	}
	if (select_statement->node->type == QueryNodeType::SELECT_NODE) {
		auto &select_node = select_statement->node->Cast<SelectNode>();
		if (select_node.from_table->type == TableReferenceType::SHOW_REF) {
			auto &show_ref = select_node.from_table->Cast<ShowRef>();
			if (!select_statement->node->cte_map.map.empty()) {
				throw ParserException("%s with CTE not allowed - wrap the statement in a subquery instead",
				                      EnumUtil::ToString(show_ref.show_type));
			}
			if (!select_statement->node->modifiers.empty()) {
				throw ParserException("%s with ORDER BY not allowed - wrap the statement in a subquery instead",
				                      EnumUtil::ToString(show_ref.show_type));
			}
		}
	}
	return transformer.MakeResult<unique_ptr<SelectStatement>>(std::move(select_statement));
}

unique_ptr<SelectStatement> PEGTransformerFactory::TransformSelectSetOpChain(
    PEGTransformer &transformer, unique_ptr<SelectStatement> intersect_chain,
    optional<vector<pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>>> select_set_op_chain_tail) {
	auto select = std::move(intersect_chain);
	if (!select_set_op_chain_tail) {
		return select;
	}
	for (auto &tail : *select_set_op_chain_tail) {
		auto setop_result = std::move(tail.first);
		auto right_select = std::move(tail.second);
		if (select->node->type == QueryNodeType::SET_OPERATION_NODE && select->node->modifiers.empty() &&
		    select->node->cte_map.map.empty()) {
			auto &existing = select->node->Cast<SetOperationNode>();
			if (existing.setop_type == setop_result->setop_type && existing.setop_all == setop_result->setop_all &&
			    (existing.setop_type == SetOperationType::UNION ||
			     existing.setop_type == SetOperationType::UNION_BY_NAME)) {
				existing.children.push_back(std::move(right_select->node));
				continue;
			}
		}
		setop_result->children.push_back(std::move(select->node));
		setop_result->children.push_back(std::move(right_select->node));
		select->node = std::move(setop_result);
	}
	return select;
}

pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>
PEGTransformerFactory::TransformSelectSetOpChainTail(PEGTransformer &transformer,
                                                     unique_ptr<SetOperationNode> setop_clause,
                                                     unique_ptr<SelectStatement> intersect_chain) {
	return make_pair(std::move(setop_clause), std::move(intersect_chain));
}

unique_ptr<SelectStatement> PEGTransformerFactory::TransformIntersectChain(
    PEGTransformer &transformer, unique_ptr<SelectStatement> select_atom,
    optional<vector<pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>>> intersect_chain_tail) {
	auto select = std::move(select_atom);
	if (!intersect_chain_tail) {
		return select;
	}
	for (auto &tail : *intersect_chain_tail) {
		auto intersect_node = std::move(tail.first);
		auto right_select = std::move(tail.second);
		intersect_node->children.push_back(std::move(select->node));
		intersect_node->children.push_back(std::move(right_select->node));
		select->node = std::move(intersect_node);
	}
	return select;
}

pair<unique_ptr<SetOperationNode>, unique_ptr<SelectStatement>>
PEGTransformerFactory::TransformIntersectChainTail(PEGTransformer &transformer,
                                                   unique_ptr<SetOperationNode> set_intersect_clause,
                                                   unique_ptr<SelectStatement> select_atom) {
	return make_pair(std::move(set_intersect_clause), std::move(select_atom));
}

unique_ptr<SetOperationNode> PEGTransformerFactory::TransformSetIntersectClause(PEGTransformer &transformer,
                                                                                const optional<bool> &distinct_or_all) {
	auto result = make_uniq<SetOperationNode>();
	result->setop_type = SetOperationType::INTERSECT;
	if (distinct_or_all) {
		result->setop_all = !*distinct_or_all;
	}
	return result;
}

unique_ptr<SetOperationNode> PEGTransformerFactory::TransformSetopClause(PEGTransformer &transformer,
                                                                         const SetOperationType &setop_type,
                                                                         const optional<bool> &distinct_or_all,
                                                                         const bool &has_result) {
	auto result = make_uniq<SetOperationNode>();
	result->setop_type = setop_type;
	if (distinct_or_all) {
		result->setop_all = !*distinct_or_all;
	}
	if (has_result) {
		if (result->setop_type == SetOperationType::UNION) {
			result->setop_type = SetOperationType::UNION_BY_NAME;
		} else {
			throw ParserException("Invalid combination of %s and BY NAME", EnumUtil::ToString(result->setop_type));
		}
	}
	return result;
}

SetOperationType PEGTransformerFactory::TransformSetopUnion(PEGTransformer &transformer) {
	return SetOperationType::UNION;
}

SetOperationType PEGTransformerFactory::TransformSetopExcept(PEGTransformer &transformer) {
	return SetOperationType::EXCEPT;
}

bool PEGTransformerFactory::TransformLateral(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformWithOrdinality(PEGTransformer &transformer) {
	return true;
}

static void RegisterWindowClause(PEGTransformer &transformer, const Identifier &window_name,
                                 WindowExpression &window_function) {
	if (transformer.window_clauses.empty()) {
		throw InternalException("WINDOW clause registered outside of a SELECT");
	}
	auto &current_windows = transformer.window_clauses.back();
	auto it = current_windows.find(window_name);
	if (it != current_windows.end()) {
		throw ParserException("window %s is already defined", window_name);
	}
	current_windows[window_name] = unique_ptr_cast<ParsedExpression, WindowExpression>(window_function.Copy());
}

static void PushSimpleSelectRemainder(GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	auto &sample_clause_opt = list_pr.Child<OptionalParseResult>(6);
	if (sample_clause_opt.HasResult()) {
		process.PushChild({sample_clause_opt.GetResult()}, 6);
	}
	auto &qualify_clause_opt = list_pr.Child<OptionalParseResult>(5);
	if (qualify_clause_opt.HasResult()) {
		process.PushChild({qualify_clause_opt.GetResult()}, 5);
	}
	auto &having_clause_opt = list_pr.Child<OptionalParseResult>(3);
	if (having_clause_opt.HasResult()) {
		process.PushChild({having_clause_opt.GetResult()}, 3);
	}
	auto &group_by_clause_opt = list_pr.Child<OptionalParseResult>(2);
	if (group_by_clause_opt.HasResult()) {
		process.PushChild({group_by_clause_opt.GetResult()}, 2);
	}
	auto &where_clause_opt = list_pr.Child<OptionalParseResult>(1);
	if (where_clause_opt.HasResult()) {
		process.PushChild({where_clause_opt.GetResult()}, 1);
	}
	process.PushChild({list_pr.GetChild(0)}, 0);
}

void PEGTransformerFactory::InitializeSimpleSelectTrampoline(PEGTransformer &transformer,
                                                             GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	process.ReserveChildSlots(7);
	transformer.window_clauses.emplace_back();
	auto &window_clause_opt = list_pr.Child<OptionalParseResult>(4);
	if (window_clause_opt.HasResult()) {
		process.manual_state = 0;
		process.PushChild({window_clause_opt.GetResult()}, 4);
		return;
	}
	process.manual_state = 1;
	PushSimpleSelectRemainder(process);
}

arena_ptr<TransformResultValue>
PEGTransformerFactory::FinalizeSimpleSelectTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process) {
	if (process.manual_state == 0) {
		process.TakeResult<vector<unique_ptr<ParsedExpression>>>(4);
		process.manual_state = 1;
		PushSimpleSelectRemainder(process);
		return nullptr;
	}

	auto select_node = process.TakeResult<unique_ptr<SelectNode>>(0);
	if (process.child_results[1]) {
		select_node->where_clause = process.TakeResult<unique_ptr<ParsedExpression>>(1);
	}
	if (process.child_results[2]) {
		auto group_by_node = process.TakeResult<GroupByNode>(2);
		if (group_by_node.group_expressions.size() == 1 && ExpressionIsEmptyStar(*group_by_node.group_expressions[0])) {
			select_node->aggregate_handling = AggregateHandling::FORCE_AGGREGATES;
			group_by_node.group_expressions.clear();
			group_by_node.grouping_sets.clear();
		}
		select_node->groups = std::move(group_by_node);
	}
	if (process.child_results[3]) {
		select_node->having = process.TakeResult<unique_ptr<ParsedExpression>>(3);
	}
	if (process.child_results[5]) {
		select_node->qualify = process.TakeResult<unique_ptr<ParsedExpression>>(5);
	}
	if (process.child_results[6]) {
		select_node->sample = process.TakeResult<unique_ptr<SampleOptions>>(6);
	}
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(select_node);
	D_ASSERT(!transformer.window_clauses.empty());
	transformer.window_clauses.pop_back();
	return transformer.MakeResult<unique_ptr<SelectStatement>>(std::move(select_statement));
}

FunctionArgument PEGTransformerFactory::TransformNamedFunctionArgument(PEGTransformer &transformer,
                                                                       const Identifier &type_func_name,
                                                                       unique_ptr<ParsedExpression> expression) {
	expression->SetAlias(type_func_name);
	return FunctionArgument(type_func_name, std::move(expression));
}

FunctionArgument PEGTransformerFactory::TransformPositionalFunctionArgument(PEGTransformer &transformer,
                                                                            unique_ptr<ParsedExpression> expression) {
	return FunctionArgument(std::move(expression));
}

MacroParameter PEGTransformerFactory::TransformNamedParameter(PEGTransformer &transformer,
                                                              const Identifier &type_func_name,
                                                              const optional<LogicalType> &type,
                                                              unique_ptr<ParsedExpression> expression) {
	MacroParameter parameter;
	parameter.expression = std::move(expression);
	parameter.name = type_func_name;
	parameter.is_default = true;
	if (type) {
		parameter.type = *type;
	}
	return parameter;
}

unique_ptr<BaseTableRef> PEGTransformerFactory::TransformUnqualifiedBaseTableName(PEGTransformer &transformer,
                                                                                  const Identifier &table_name) {
	const auto description = TableDescription(QualifiedName(table_name));
	return make_uniq<BaseTableRef>(description);
}

Identifier PEGTransformerFactory::TransformSchemaQualification(PEGTransformer &transformer,
                                                               const Identifier &schema_name) {
	return schema_name;
}

Identifier PEGTransformerFactory::TransformReservedSchemaQualification(PEGTransformer &transformer,
                                                                       const Identifier &reserved_schema_name) {
	return reserved_schema_name;
}

Identifier PEGTransformerFactory::TransformCatalogQualification(PEGTransformer &transformer,
                                                                const Identifier &catalog_name) {
	return catalog_name;
}

QualifiedName PEGTransformerFactory::TransformCatalogReservedSchemaIdentifier(
    PEGTransformer &transformer, const Identifier &catalog_qualification,
    const vector<Identifier> &reserved_schema_qualification, const Identifier &reserved_identifier_or_string_literal) {
	vector<Identifier> qualification;
	qualification.push_back(catalog_qualification);
	for (auto &schema : reserved_schema_qualification) {
		qualification.push_back(schema);
	}
	return QualifiedName(std::move(qualification), reserved_identifier_or_string_literal);
}

QualifiedName PEGTransformerFactory::TransformSchemaReservedIdentifierOrStringLiteral(
    PEGTransformer &transformer, const Identifier &schema_qualification,
    const Identifier &reserved_identifier_or_string_literal) {
	QualifiedName result({schema_qualification}, reserved_identifier_or_string_literal);
	return result;
}

void PEGTransformerFactory::InitializeTableRefTrampoline(PEGTransformer &transformer,
                                                         GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	auto &join_or_pivot_opt = list_pr.Child<OptionalParseResult>(1);
	idx_t join_count = 0;
	if (join_or_pivot_opt.HasResult()) {
		join_count = join_or_pivot_opt.GetResult().Cast<RepeatParseResult>().GetChildren().size();
	}
	process.ReserveChildSlots(1 + join_count);
	if (join_count > 0) {
		auto repeat_children = join_or_pivot_opt.GetResult().Cast<RepeatParseResult>().GetChildren();
		for (idx_t i = repeat_children.size(); i > 0; i--) {
			auto child_idx = i - 1;
			process.PushChild({repeat_children[child_idx].get()}, 1 + child_idx);
		}
	}
	process.PushChild({list_pr.GetChild(0)}, 0);
}

arena_ptr<TransformResultValue> PEGTransformerFactory::FinalizeTableRefTrampoline(PEGTransformer &transformer,
                                                                                  GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	auto inner_table_ref = process.TakeResult<unique_ptr<TableRef>>(0);
	auto &join_or_pivot_opt = list_pr.Child<OptionalParseResult>(1);
	if (!join_or_pivot_opt.HasResult()) {
		return transformer.MakeResult<unique_ptr<TableRef>>(std::move(inner_table_ref));
	}
	auto &repeat_join_or_pivot = join_or_pivot_opt.GetResult().Cast<RepeatParseResult>();
	auto repeat_children = repeat_join_or_pivot.GetChildren();
	for (idx_t i = 0; i < repeat_children.size(); i++) {
		auto transform_join_or_pivot = process.TakeResult<unique_ptr<TableRef>>(1 + i);
		if (transform_join_or_pivot->type == TableReferenceType::JOIN) {
			auto &join_ref = transform_join_or_pivot->Cast<JoinRef>();
			join_ref.left = std::move(inner_table_ref);
			inner_table_ref = std::move(transform_join_or_pivot);
		} else if (transform_join_or_pivot->type == TableReferenceType::PIVOT) {
			auto &pivot_ref = transform_join_or_pivot->Cast<PivotRef>();
			pivot_ref.source = std::move(inner_table_ref);
			inner_table_ref = std::move(transform_join_or_pivot);
		} else if (transform_join_or_pivot->type == TableReferenceType::MATCH_RECOGNIZE) {
			auto &match_recognize_ref = transform_join_or_pivot->Cast<MatchRecognizeRef>();
			match_recognize_ref.input = std::move(inner_table_ref);
			inner_table_ref = std::move(transform_join_or_pivot);
		} else {
			throw NotImplementedException("Unsupported TableRef type encountered: %s",
			                              EnumUtil::ToString(transform_join_or_pivot->type));
		}
	}
	return transformer.MakeResult<unique_ptr<TableRef>>(std::move(inner_table_ref));
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTableUnpivotClauseBody(PEGTransformer &transformer,
                                                                            const vector<string> &unpivot_header,
                                                                            vector<PivotColumn> unpivot_value_list) {
	auto result = make_uniq<PivotRef>();
	result->unpivot_names = StringsToIdentifiers(unpivot_header);
	result->pivots = std::move(unpivot_value_list);
	if (result->pivots.size() > 1) {
		throw ParserException("UNPIVOT requires a single pivot element");
	}
	return std::move(result);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTableUnpivotClause(PEGTransformer &transformer,
                                                                        const optional<bool> &include_or_exclude_nulls,
                                                                        unique_ptr<TableRef> table_unpivot_clause_body,
                                                                        const optional<TableAlias> &table_alias) {
	auto &result = table_unpivot_clause_body->Cast<PivotRef>();
	result.include_nulls = include_or_exclude_nulls.value_or(false);
	if (table_alias) {
		result.alias = table_alias->name;
		result.column_name_alias = table_alias->column_name_alias;
	}
	return table_unpivot_clause_body;
}

void PEGTransformerFactory::GetValueFromExpression(unique_ptr<ParsedExpression> &expr, vector<Value> &result) {
	if (expr->GetExpressionClass() == ExpressionClass::CONSTANT) {
		auto &const_expr = expr->Cast<ConstantExpression>();
		result.push_back(const_expr.GetLiteral().ToValue());
	} else if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &col_ref_expr = expr->Cast<ColumnRefExpression>();
		for (auto &col : col_ref_expr.ColumnNames()) {
			result.push_back(Value(col));
		}
	} else if (expr->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func_expr = expr->Cast<FunctionExpression>();
		if (func_expr.FunctionName() == "row") {
			for (auto &col : func_expr.GetArgumentsMutable()) {
				GetValueFromExpression(col.GetExpressionMutable(), result);
			}
		}
	}
}

bool PEGTransformerFactory::TransformPivotInList(unique_ptr<ParsedExpression> &expr, PivotColumnEntry &entry) {
	auto initial_size = entry.values.size();
	switch (expr->GetExpressionType()) {
	case ExpressionType::COLUMN_REF: {
		auto &colref = expr->Cast<ColumnRefExpression>();
		if (colref.IsQualified()) {
			throw ParserException(expr->GetQueryLocation(), "PIVOT IN list cannot contain qualified column references");
		}
		entry.values.emplace_back(colref.GetColumnName());
		return true;
	}
	case ExpressionType::FUNCTION: {
		auto &function = expr->Cast<FunctionExpression>();
		if (function.FunctionName() != "row" || function.GetArgumentsMutable().empty()) {
			return false;
		}
		for (auto &child : function.GetArgumentsMutable()) {
			if (!TransformPivotInList(child.GetExpressionMutable(), entry)) {
				entry.values.resize(initial_size);
				return false;
			}
		}
		return true;
	}
	default:
		// constants and other expressions are kept as-is and folded by the binder
		return false;
	}
}

static bool PivotEntryIsTuple(const PivotColumnEntry &entry) {
	if (entry.values.size() > 1) {
		return true;
	}
	if (!entry.expr || entry.expr->GetExpressionType() != ExpressionType::FUNCTION) {
		return false;
	}
	auto &function = entry.expr->Cast<FunctionExpression>();
	return function.FunctionName() == "row";
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTablePivotClauseBody(
    PEGTransformer &transformer, vector<unique_ptr<ParsedExpression>> target_list, vector<PivotColumn> pivot_value_list,
    const optional<vector<string>> &pivot_group_by_list) {
	auto result = make_uniq<PivotRef>();
	result->aggregates = std::move(target_list);
	result->pivots = std::move(pivot_value_list);
	if (pivot_group_by_list) {
		result->groups = StringsToIdentifiers(*pivot_group_by_list);
	}
	return std::move(result);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTablePivotClause(PEGTransformer &transformer,
                                                                      unique_ptr<TableRef> table_pivot_clause_body,
                                                                      const optional<TableAlias> &table_alias) {
	auto &result = table_pivot_clause_body->Cast<PivotRef>();
	if (table_alias) {
		result.alias = table_alias->name;
		result.column_name_alias = table_alias->column_name_alias;
	}
	return table_pivot_clause_body;
}

PivotColumn PEGTransformerFactory::TransformPivotEnumTarget(PEGTransformer &transformer, const Identifier &identifier) {
	PivotColumn result;
	result.pivot_enum = identifier;
	return result;
}

PivotColumn PEGTransformerFactory::TransformPivotListTarget(PEGTransformer &transformer,
                                                            vector<PivotColumnEntry> pivot_target_list) {
	PivotColumn result;
	result.entries = std::move(pivot_target_list);
	return result;
}

PivotColumn PEGTransformerFactory::TransformPivotValueList(PEGTransformer &transformer,
                                                           unique_ptr<ParsedExpression> pivot_header,
                                                           PivotColumn pivot_value_target) {
	auto result = std::move(pivot_value_target);
	auto pivot_expression = std::move(pivot_header);
	if (pivot_expression->GetExpressionClass() != ExpressionClass::FUNCTION) {
		result.pivot_expressions.push_back(std::move(pivot_expression));
		return result;
	}
	auto &func_expr = pivot_expression->Cast<FunctionExpression>();
	if (func_expr.FunctionName() != "row") {
		result.pivot_expressions.push_back(std::move(pivot_expression));
		return result;
	}
	// Unpack row() only when IN list entries are tuples (multi-value).
	// For scalar IN entries like IN ('xx'), keep row() as a single compound expression
	// so pivot_expressions.size() matches entry.values.size() (both 1).
	bool has_tuple_entries = false;
	for (auto &entry : result.entries) {
		if (PivotEntryIsTuple(entry)) {
			has_tuple_entries = true;
			break;
		}
	}
	if (has_tuple_entries) {
		for (auto &child : func_expr.GetArgumentsMutable()) {
			result.pivot_expressions.emplace_back(std::move(child.GetExpressionMutable()));
		}
	} else {
		result.pivot_expressions.push_back(std::move(pivot_expression));
	}
	return result;
}

unique_ptr<TableRef> PEGTransformerFactory::TransformRegularJoinClause(PEGTransformer &transformer,
                                                                       const optional<bool> &asof,
                                                                       const optional<JoinType> &join_type,
                                                                       unique_ptr<TableRef> table_ref,
                                                                       JoinQualifier join_qualifier) {
	auto result = make_uniq<JoinRef>();
	if (asof.value_or(false)) {
		result->ref_type = JoinRefType::ASOF;
	}
	result->type = join_type.value_or(JoinType::INNER);
	result->right = std::move(table_ref);
	if (join_qualifier.on_clause) {
		result->condition = std::move(join_qualifier.on_clause);
	} else if (!join_qualifier.using_columns.empty()) {
		result->using_columns = std::move(join_qualifier.using_columns);
	} else {
		throw InternalException("Invalid join qualifier found.");
	}
	return std::move(result);
}

//! Shared builder for both NEAREST BY alternatives (aliased and bare target).
static unique_ptr<TableRef> BuildNearestJoin(const optional<JoinType> &join_type, unique_ptr<TableRef> table_ref,
                                             const optional<bool> &approx_or_exact,
                                             optional<unique_ptr<ParsedExpression>> number_literal,
                                             const OrderType &distance_or_similarity,
                                             unique_ptr<ParsedExpression> expression) {
	auto result = make_uniq<JoinRef>(JoinRefType::NEAREST);
	result->type = join_type.value_or(JoinType::INNER);
	if (result->type != JoinType::INNER && result->type != JoinType::LEFT) {
		throw ParserException("NEAREST BY only supports INNER and LEFT OUTER joins, not %s",
		                      EnumUtil::ToString(result->type));
	}
	if (number_literal) {
		auto &literal = (*number_literal)->Cast<ConstantExpression>().GetLiteral();
		auto literal_text = literal.ToValue().ToString();
		int64_t count = 0;
		literal.TryGetInt64(count);
		if (count < 1) {
			throw ParserException("NEAREST expects a positive integer literal, got \"%s\"", literal_text);
		}
		result->nearest_count = NumericCast<idx_t>(count);
	}
	result->nearest_order_type = distance_or_similarity;
	result->nearest_approx = approx_or_exact.value_or(false);
	result->ranking_expression = std::move(expression);
	result->right = std::move(table_ref);
	return std::move(result);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformNearestJoinAliased(
    PEGTransformer &transformer, const optional<JoinType> &join_type, unique_ptr<TableRef> table_ref,
    const optional<bool> &approx_or_exact, optional<unique_ptr<ParsedExpression>> number_literal,
    const OrderType &distance_or_similarity, unique_ptr<ParsedExpression> expression) {
	return BuildNearestJoin(join_type, std::move(table_ref), approx_or_exact, std::move(number_literal),
	                        distance_or_similarity, std::move(expression));
}

unique_ptr<TableRef> PEGTransformerFactory::TransformNearestJoinBare(
    PEGTransformer &transformer, const optional<JoinType> &join_type, unique_ptr<TableRef> nearest_bare_table_ref,
    const optional<bool> &approx_or_exact, optional<unique_ptr<ParsedExpression>> number_literal,
    const OrderType &distance_or_similarity, unique_ptr<ParsedExpression> expression) {
	return BuildNearestJoin(join_type, std::move(nearest_bare_table_ref), approx_or_exact, std::move(number_literal),
	                        distance_or_similarity, std::move(expression));
}

unique_ptr<TableRef> PEGTransformerFactory::TransformNearestValuesRef(PEGTransformer &transformer,
                                                                      unique_ptr<SelectStatement> values_clause) {
	return make_uniq<SubqueryRef>(std::move(values_clause));
}

unique_ptr<TableRef> PEGTransformerFactory::TransformNearestTableFunction(
    PEGTransformer &transformer, const optional<bool> &lateral, const QualifiedName &qualified_table_function,
    vector<FunctionArgument> table_function_arguments, const optional<bool> &with_ordinality) {
	auto result = make_uniq<TableFunctionRef>();
	result->with_ordinality =
	    with_ordinality.value_or(false) ? OrdinalityType::WITH_ORDINALITY : OrdinalityType::WITHOUT_ORDINALITY;
	result->function = make_uniq<FunctionExpression>(qualified_table_function, std::move(table_function_arguments));
	return std::move(result);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformNearestTableSubquery(PEGTransformer &transformer,
                                                                          const optional<bool> &lateral,
                                                                          unique_ptr<TableRef> subquery_reference) {
	return subquery_reference;
}

unique_ptr<TableRef> PEGTransformerFactory::TransformNearestBaseTableRef(
    PEGTransformer &transformer, unique_ptr<BaseTableRef> base_table_name, optional<unique_ptr<AtClause>> at_clause,
    optional<unique_ptr<SampleOptions>> sample_clause) {
	if (at_clause) {
		base_table_name->at_clause = std::move(*at_clause);
	}
	if (sample_clause) {
		base_table_name->sample = std::move(*sample_clause);
	}
	return std::move(base_table_name);
}

unique_ptr<TableRef>
PEGTransformerFactory::TransformNearestParensTableRef(PEGTransformer &transformer, unique_ptr<TableRef> table_ref,
                                                      optional<unique_ptr<SampleOptions>> sample_clause) {
	if (!sample_clause) {
		return table_ref;
	}
	auto select_statement = make_uniq<SelectStatement>();
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(table_ref);
	select_statement->node = std::move(select_node);
	auto subquery = make_uniq<SubqueryRef>(std::move(select_statement));
	subquery->sample = std::move(*sample_clause);
	return std::move(subquery);
}

bool PEGTransformerFactory::TransformNearestApprox(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformNearestExact(PEGTransformer &transformer) {
	return false;
}

OrderType PEGTransformerFactory::TransformNearestDistance(PEGTransformer &transformer) {
	return OrderType::ASCENDING;
}

OrderType PEGTransformerFactory::TransformNearestSimilarity(PEGTransformer &transformer) {
	return OrderType::DESCENDING;
}

unique_ptr<TableRef> PEGTransformerFactory::TransformJoinByClause(PEGTransformer &transformer, const string &col_label,
                                                                  unique_ptr<TableRef> table_ref,
                                                                  JoinQualifier join_qualifier) {
	auto result = make_uniq<JoinRef>();
	// resolve the join type name against the JoinType enum (case-insensitive); accept an optional `_join` suffix,
	// so e.g. `mark` and `mark_join` are equivalent. EnumUtil::FromString throws on an unknown name.
	auto type_name = col_label;
	if (StringUtil::CIEndsWith(type_name, "_join")) {
		type_name = type_name.substr(0, type_name.size() - 5);
	}
	result->type = EnumUtil::FromString<JoinType>(type_name);
	if (result->type == JoinType::INVALID) {
		throw ParserException("\"%s\" is not a valid join type for JOIN BY", col_label);
	}
	result->right = std::move(table_ref);
	if (join_qualifier.on_clause) {
		result->condition = std::move(join_qualifier.on_clause);
	} else if (!join_qualifier.using_columns.empty()) {
		result->using_columns = std::move(join_qualifier.using_columns);
	} else {
		throw InternalException("Invalid join qualifier found.");
	}
	return std::move(result);
}

bool PEGTransformerFactory::TransformAsof(PEGTransformer &transformer) {
	return true;
}

JoinType PEGTransformerFactory::TransformFullJoin(PEGTransformer &transformer, const bool &has_result) {
	return JoinType::OUTER;
}

JoinType PEGTransformerFactory::TransformLeftJoin(PEGTransformer &transformer, const bool &has_result) {
	return JoinType::LEFT;
}

JoinType PEGTransformerFactory::TransformRightJoin(PEGTransformer &transformer, const bool &has_result) {
	return JoinType::RIGHT;
}

JoinType PEGTransformerFactory::TransformSemiJoin(PEGTransformer &transformer) {
	return JoinType::SEMI;
}

JoinType PEGTransformerFactory::TransformAntiJoin(PEGTransformer &transformer) {
	return JoinType::ANTI;
}

JoinType PEGTransformerFactory::TransformInnerJoin(PEGTransformer &transformer) {
	return JoinType::INNER;
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTableFunctionLateralOpt(
    PEGTransformer &transformer, const optional<bool> &lateral, const QualifiedName &qualified_table_function,
    vector<FunctionArgument> table_function_arguments, const optional<bool> &with_ordinality,
    const optional<TableAlias> &table_alias, optional<unique_ptr<SampleOptions>> sample_clause) {
	if (lateral.value_or(false) && sample_clause) {
		throw ParserException("TABLESAMPLE cannot be applied to a LATERAL table function");
	}
	auto result = make_uniq<TableFunctionRef>();

	result->with_ordinality =
	    with_ordinality.value_or(false) ? OrdinalityType::WITH_ORDINALITY : OrdinalityType::WITHOUT_ORDINALITY;
	result->function = make_uniq<FunctionExpression>(qualified_table_function, std::move(table_function_arguments));
	if (table_alias) {
		result->alias = table_alias->name;
		result->column_name_alias = table_alias->column_name_alias;
	}
	if (sample_clause) {
		result->sample = std::move(*sample_clause);
	}
	return std::move(result);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTableFunctionAliasColon(
    PEGTransformer &transformer, const Identifier &table_alias_colon, const QualifiedName &qualified_table_function,
    vector<FunctionArgument> table_function_arguments, const optional<bool> &with_ordinality,
    optional<unique_ptr<SampleOptions>> sample_clause) {
	auto result = make_uniq<TableFunctionRef>();
	result->with_ordinality =
	    with_ordinality.value_or(false) ? OrdinalityType::WITH_ORDINALITY : OrdinalityType::WITHOUT_ORDINALITY;
	result->function = make_uniq<FunctionExpression>(qualified_table_function, std::move(table_function_arguments));
	result->alias = table_alias_colon;
	if (sample_clause) {
		result->sample = std::move(*sample_clause);
	}
	return std::move(result);
}

string PEGTransformerFactory::TransformVersionAtUnit(PEGTransformer &transformer) {
	return "VERSION";
}

string PEGTransformerFactory::TransformTimestampAtUnit(PEGTransformer &transformer) {
	return "TIMESTAMP";
}

OrderType PEGTransformerFactory::TransformDescendingOrder(PEGTransformer &transformer) {
	return OrderType::DESCENDING;
}

OrderType PEGTransformerFactory::TransformAscendingOrder(PEGTransformer &transformer) {
	return OrderType::ASCENDING;
}

OrderByNullType PEGTransformerFactory::TransformNullsFirst(PEGTransformer &transformer) {
	return OrderByNullType::NULLS_FIRST;
}

OrderByNullType PEGTransformerFactory::TransformNullsLast(PEGTransformer &transformer) {
	return OrderByNullType::NULLS_LAST;
}

unique_ptr<ResultModifier> PEGTransformerFactory::VerifyLimitOffset(LimitPercentResult &limit,
                                                                    LimitPercentResult &offset) {
	if (offset.is_percent) {
		throw ParserException("Percentage for offsets are not supported.");
	}
	auto result = make_uniq<LimitModifier>();
	result->limit_type = limit.is_percent ? LimitValueType::PERCENTAGE : LimitValueType::ROW_COUNT;
	if (limit.expression) {
		result->limit = std::move(limit.expression);
	}
	if (offset.expression) {
		result->offset = std::move(offset.expression);
	}
	if (!result->limit && !result->offset) {
		return nullptr;
	}
	return std::move(result);
}

LimitPercentResult PEGTransformerFactory::TransformLimitExpression(PEGTransformer &transformer,
                                                                   unique_ptr<ParsedExpression> expression,
                                                                   const bool &has_result) {
	LimitPercentResult result;
	result.expression = std::move(expression);
	result.is_percent = has_result;
	return result;
}

struct GroupingExpressionMap {
	parsed_expression_map_t<ProjectionIndex> map;
};

static void CheckGroupingSetMax(idx_t count) {
	static constexpr const idx_t MAX_GROUPING_SETS = 65535;
	if (count > MAX_GROUPING_SETS) {
		throw ParserException("Maximum grouping set count of %d exceeded", MAX_GROUPING_SETS);
	}
}

static void CheckGroupingSetCubes(idx_t current_count, idx_t cube_count) {
	idx_t combinations = 1;
	for (idx_t i = 0; i < cube_count; i++) {
		combinations *= 2;
		CheckGroupingSetMax(current_count + combinations);
	}
}

static GroupingSet VectorToGroupingSet(vector<ProjectionIndex> &indexes) {
	GroupingSet result;
	for (idx_t i = 0; i < indexes.size(); i++) {
		result.insert(indexes[i]);
	}
	return result;
}

void PEGTransformerFactory::AddGroupByExpression(unique_ptr<ParsedExpression> expression, GroupingExpressionMap &map,
                                                 GroupByNode &result, vector<ProjectionIndex> &result_set) {
	if (expression->GetExpressionType() == ExpressionType::FUNCTION) {
		auto &func = expression->Cast<FunctionExpression>();
		if (func.FunctionName() == "row") {
			for (auto &child : func.GetArgumentsMutable()) {
				AddGroupByExpression(std::move(child.GetExpressionMutable()), map, result, result_set);
			}
			return;
		}
	}
	auto entry = map.map.find(*expression);
	ProjectionIndex result_idx;
	if (entry == map.map.end()) {
		result_idx = ProjectionIndex(result.group_expressions.size());
		map.map[*expression] = result_idx;
		result.group_expressions.push_back(std::move(expression));
	} else {
		result_idx = entry->second;
	}
	result_set.push_back(result_idx);
}

static void AddCubeSets(const GroupingSet &current_set, vector<GroupingSet> &cube_sets,
                        vector<GroupingSet> &result_sets, idx_t start_idx = 0) {
	CheckGroupingSetMax(result_sets.size());
	result_sets.push_back(current_set);
	for (idx_t k = start_idx; k < cube_sets.size(); k++) {
		auto child_set = current_set;
		child_set.insert(cube_sets[k].begin(), cube_sets[k].end());
		AddCubeSets(child_set, cube_sets, result_sets, k + 1);
	}
}

vector<GroupingSet> PEGTransformerFactory::GroupByExpressionUnfolding(GroupByExpressionInfo &group_by_expr,
                                                                      GroupingExpressionMap &map, GroupByNode &result) {
	vector<GroupingSet> result_sets;
	if (group_by_expr.type == GroupByExpressionInfoType::EMPTY) {
		result_sets.emplace_back();

	} else if (group_by_expr.type == GroupByExpressionInfoType::EXPRESSION) {
		vector<ProjectionIndex> indexes;
		AddGroupByExpression(std::move(group_by_expr.expression), map, result, indexes);
		result_sets.push_back(VectorToGroupingSet(indexes));
	} else if (group_by_expr.type == GroupByExpressionInfoType::GROUPING_SETS) {
		for (auto &child_expr : group_by_expr.children) {
			auto child_sets = GroupByExpressionUnfolding(child_expr, map, result);
			result_sets.insert(result_sets.end(), child_sets.begin(), child_sets.end());
			CheckGroupingSetMax(result_sets.size());
		}
	} else if (group_by_expr.type == GroupByExpressionInfoType::CUBE ||
	           group_by_expr.type == GroupByExpressionInfoType::ROLLUP) {
		if (group_by_expr.expressions.empty()) {
			throw ParserException("CUBE or ROLLUP column list cannot be empty");
		}

		vector<GroupingSet> unfolding_sets;
		for (auto &expr : group_by_expr.expressions) {
			vector<ProjectionIndex> indexes;
			AddGroupByExpression(std::move(expr), map, result, indexes);

			GroupingSet s;
			for (auto idx : indexes) {
				s.insert(idx);
			}
			unfolding_sets.push_back(std::move(s));
		}

		if (group_by_expr.type == GroupByExpressionInfoType::CUBE) {
			CheckGroupingSetCubes(result_sets.size(), unfolding_sets.size());
			GroupingSet current_set;
			AddCubeSets(current_set, unfolding_sets, result_sets, 0);
		} else {
			GroupingSet current_set;
			result_sets.push_back(current_set);
			for (idx_t i = 0; i < unfolding_sets.size(); i++) {
				current_set.insert(unfolding_sets[i].begin(), unfolding_sets[i].end());
				result_sets.push_back(current_set);
			}
		}
	}
	return result_sets;
}

string PEGTransformerFactory::TransformCubeKeyword(PEGTransformer &transformer) {
	return "CUBE";
}

string PEGTransformerFactory::TransformRollupKeyword(PEGTransformer &transformer) {
	return "ROLLUP";
}

GroupByNode PEGTransformerFactory::TransformGroupByList(PEGTransformer &transformer,
                                                        vector<GroupByExpressionInfo> group_by_expression) {
	GroupByNode result;
	GroupingExpressionMap map;

	for (auto &group_by_expr : group_by_expression) {
		vector<GroupingSet> next_sets = GroupByExpressionUnfolding(group_by_expr, map, result);

		if (result.grouping_sets.empty()) {
			result.grouping_sets = std::move(next_sets);
		} else {
			vector<GroupingSet> new_sets;
			idx_t grouping_set_count = result.grouping_sets.size() * next_sets.size();
			CheckGroupingSetMax(grouping_set_count);
			new_sets.reserve(grouping_set_count);

			for (auto &current_set : result.grouping_sets) {
				for (auto &next_set : next_sets) {
					GroupingSet combined_set;
					combined_set.insert(current_set.begin(), current_set.end());
					combined_set.insert(next_set.begin(), next_set.end());
					new_sets.push_back(std::move(combined_set));
				}
			}
			result.grouping_sets = std::move(new_sets);
		}
	}
	return result;
}

GroupByExpressionInfo PEGTransformerFactory::TransformGroupByBaseExpression(PEGTransformer &transformer,
                                                                            unique_ptr<ParsedExpression> expression) {
	GroupByExpressionInfo result;
	result.type = GroupByExpressionInfoType::EXPRESSION;
	result.expression = std::move(expression);
	return result;
}

GroupByExpressionInfo PEGTransformerFactory::TransformEmptyGroupingItem(PEGTransformer &transformer) {
	GroupByExpressionInfo result;
	result.type = GroupByExpressionInfoType::EMPTY;
	return result;
}

GroupByExpressionInfo
PEGTransformerFactory::TransformCubeOrRollupClause(PEGTransformer &transformer, const string &cube_or_rollup,
                                                   optional<vector<unique_ptr<ParsedExpression>>> expression) {
	GroupByExpressionInfo result;
	result.type = StringUtil::CIEquals(cube_or_rollup, "CUBE") ? GroupByExpressionInfoType::CUBE
	                                                           : GroupByExpressionInfoType::ROLLUP;
	if (expression) {
		result.expressions = std::move(*expression);
	}
	return result;
}

GroupByExpressionInfo
PEGTransformerFactory::TransformGroupingSetsClause(PEGTransformer &transformer,
                                                   vector<GroupByExpressionInfo> group_by_expression) {
	GroupByExpressionInfo result;
	result.type = GroupByExpressionInfoType::GROUPING_SETS;
	result.children = std::move(group_by_expression);
	return result;
}

void PEGTransformerFactory::InitializeWithClauseTrampoline(PEGTransformer &transformer,
                                                           GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	auto with_statement_list = ExtractParseResultsFromList(list_pr.Child<ListParseResult>(2));
	process.ReserveChildSlots(with_statement_list.size());
	for (idx_t i = with_statement_list.size(); i > 0; i--) {
		auto child_idx = i - 1;
		process.PushChild({with_statement_list[child_idx].get()}, child_idx);
	}
}

arena_ptr<TransformResultValue>
PEGTransformerFactory::FinalizeWithClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	bool is_recursive = list_pr.Child<OptionalParseResult>(1).HasResult();
	CommonTableExpressionMap result;
	for (idx_t entry_idx = 0; entry_idx < process.child_results.size(); entry_idx++) {
		auto with_entry = process.TakeResult<pair<Identifier, unique_ptr<CommonTableExpressionInfo>>>(entry_idx);
		if (is_recursive) {
			auto &query_node = with_entry.second->query_node;
			if (!query_node) {
				throw ParserException("Recursive CTEs with DML statements are not supported");
			}
			ValidateRecursiveCTEQueryNode(*query_node);
			query_node = ToRecursiveCTE(std::move(query_node), with_entry.first, with_entry.second->aliases,
			                            with_entry.second->key_targets);
		}
		auto &cte_name = with_entry.first;
		auto it = result.map.find(cte_name);
		if (it != result.map.end()) {
			throw ParserException("Duplicate CTE name %s", cte_name);
		}
		result.map.insert(with_entry.first, std::move(with_entry.second));
	}
	return transformer.MakeResult<CommonTableExpressionMap>(std::move(result));
}

pair<Identifier, unique_ptr<CommonTableExpressionInfo>>
PEGTransformerFactory::TransformWithStatement(PEGTransformer &transformer, const Identifier &col_id_or_string,
                                              const optional<vector<string>> &column_aliases,
                                              optional<vector<unique_ptr<ParsedExpression>>> using_key,
                                              const optional<bool> &materialized, unique_ptr<TableRef> cte_body) {
	auto result = make_uniq<CommonTableExpressionInfo>();
	auto cte_name = col_id_or_string;
	if (column_aliases) {
		result->aliases = StringsToIdentifiers(*column_aliases);
	}
	if (using_key) {
		result->key_targets = std::move(*using_key);
	}
	if (materialized) {
		// If this has a result, we know it is either NEVER or ALWAYS
		if (*materialized) {
			result->materialized = CTEMaterialize::CTE_MATERIALIZE_NEVER;
		} else {
			result->materialized = CTEMaterialize::CTE_MATERIALIZE_ALWAYS;
		}
	}
	D_ASSERT(cte_body->type == TableReferenceType::SUBQUERY);
	auto subquery_ref = unique_ptr_cast<TableRef, SubqueryRef>(std::move(cte_body));
	result->query_node = std::move(subquery_ref->subquery->node);
	return make_pair(cte_name, std::move(result));
}

unique_ptr<TableRef>
PEGTransformerFactory::TransformCTESelectBody(PEGTransformer &transformer,
                                              unique_ptr<SelectStatement> select_statement_internal) {
	return make_uniq<SubqueryRef>(std::move(select_statement_internal));
}

unique_ptr<TableRef> PEGTransformerFactory::TransformCTEDMLBody(PEGTransformer &transformer,
                                                                unique_ptr<SQLStatement> statement) {
	unique_ptr<QueryNode> query_node;
	switch (statement->type) {
	case StatementType::INSERT_STATEMENT:
		query_node = unique_ptr_cast<InsertQueryNode, QueryNode>(std::move(statement->Cast<InsertStatement>().node));
		break;
	case StatementType::UPDATE_STATEMENT:
		query_node = unique_ptr_cast<UpdateQueryNode, QueryNode>(std::move(statement->Cast<UpdateStatement>().node));
		break;
	case StatementType::DELETE_STATEMENT:
		query_node = unique_ptr_cast<DeleteQueryNode, QueryNode>(std::move(statement->Cast<DeleteStatement>().node));
		break;
	case StatementType::COPY_STATEMENT: {
		auto &copy = statement->Cast<CopyStatement>();
		if (copy.info->is_from) {
			throw ParserException("COPY FROM cannot be used as a CTE body");
		}
		query_node = make_uniq<CopyQueryNode>(std::move(copy.info));
		break;
	}
	default:
		throw ParserException("A CTE body must be a SELECT, INSERT, UPDATE, DELETE, or COPY TO statement");
	}
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(query_node);
	return make_uniq<SubqueryRef>(std::move(select_statement));
}

bool PEGTransformerFactory::TransformMaterialized(PEGTransformer &transformer, const bool &has_result) {
	return has_result;
}

void PEGTransformerFactory::InitializeWindowDefinitionTrampoline(PEGTransformer &transformer,
                                                                 GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	transformer.in_window_definition = true;
	process.ReserveChildSlots(1);
	process.PushChild({list_pr.GetChild(2)}, 0);
}

arena_ptr<TransformResultValue>
PEGTransformerFactory::FinalizeWindowDefinitionTrampoline(PEGTransformer &transformer,
                                                          GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	auto window_function = process.TakeResult<unique_ptr<WindowExpression>>(0);
	transformer.in_window_definition = false;
	auto window_name = list_pr.Child<IdentifierParseResult>(0).identifier;
	RegisterWindowClause(transformer, window_name, *window_function);
	unique_ptr<ParsedExpression> result = std::move(window_function);
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(result));
}

unique_ptr<SampleOptions> PEGTransformerFactory::TransformSampleEntryFunction(
    PEGTransformer &transformer, const optional<SampleMethod> &sample_function, unique_ptr<SampleOptions> sample_count,
    const optional<optional_idx> &repeatable_sample) {
	if (sample_function) {
		sample_count->method = *sample_function;
	}
	if (repeatable_sample) {
		sample_count->seed = *repeatable_sample;
		sample_count->repeatable = true;
	}
	return sample_count;
}

unique_ptr<SampleOptions>
PEGTransformerFactory::TransformSampleEntryCount(PEGTransformer &transformer, unique_ptr<SampleOptions> sample_count,
                                                 const optional<pair<SampleMethod, optional_idx>> &sample_properties) {
	if (sample_properties) {
		sample_count->method = sample_properties->first;
		sample_count->seed = sample_properties->second;
		if (sample_count->seed.IsValid()) {
			sample_count->repeatable = true;
		}
	}
	return sample_count;
}

bool PEGTransformerFactory::TransformSamplePercentage(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformSampleRows(PEGTransformer &transformer) {
	return false;
}

unique_ptr<SelectStatement>
PEGTransformerFactory::TransformSelectParens(PEGTransformer &transformer,
                                             unique_ptr<SelectStatement> select_statement_internal) {
	return select_statement_internal;
}

vector<unique_ptr<ResultModifier>>
PEGTransformerFactory::TransformResultModifiers(PEGTransformer &transformer,
                                                optional<vector<OrderByNode>> order_by_clause,
                                                optional<unique_ptr<ResultModifier>> limit_offset) {
	vector<unique_ptr<ResultModifier>> result;
	if (order_by_clause) {
		auto order_modifier = make_uniq<OrderModifier>();
		order_modifier->orders = std::move(*order_by_clause);
		result.push_back(std::move(order_modifier));
	}
	if (limit_offset) {
		result.push_back(std::move(*limit_offset));
	}
	return result;
}

unique_ptr<ResultModifier>
PEGTransformerFactory::TransformLimitOffsetClause(PEGTransformer &transformer, LimitPercentResult limit_clause,
                                                  optional<LimitPercentResult> offset_clause) {
	LimitPercentResult empty_offset;
	return VerifyLimitOffset(limit_clause, offset_clause ? *offset_clause : empty_offset);
}

unique_ptr<ResultModifier>
PEGTransformerFactory::TransformOffsetLimitClause(PEGTransformer &transformer, LimitPercentResult offset_clause,
                                                  optional<LimitPercentResult> limit_clause) {
	LimitPercentResult empty_limit;
	return VerifyLimitOffset(limit_clause ? *limit_clause : empty_limit, offset_clause);
}

unique_ptr<ResultModifier> PEGTransformerFactory::TransformOffsetFetchClause(PEGTransformer &transformer,
                                                                             LimitPercentResult offset_clause,
                                                                             LimitPercentResult fetch_clause) {
	return VerifyLimitOffset(fetch_clause, offset_clause);
}

unique_ptr<ResultModifier> PEGTransformerFactory::TransformFetchOffsetClause(PEGTransformer &transformer,
                                                                             LimitPercentResult fetch_clause,
                                                                             LimitPercentResult offset_clause) {
	return VerifyLimitOffset(fetch_clause, offset_clause);
}

unique_ptr<ResultModifier> PEGTransformerFactory::TransformFetchOnlyClause(PEGTransformer &transformer,
                                                                           LimitPercentResult fetch_clause) {
	LimitPercentResult empty_offset;
	return VerifyLimitOffset(fetch_clause, empty_offset);
}

unique_ptr<SelectStatement> PEGTransformerFactory::TransformTableStatement(PEGTransformer &transformer,
                                                                           unique_ptr<BaseTableRef> base_table_name) {
	auto result = make_uniq<SelectStatement>();
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(make_uniq<StarExpression>());
	node->from_table = std::move(base_table_name);
	result->node = std::move(node);
	return result;
}

unique_ptr<SelectStatement>
PEGTransformerFactory::TransformSimpleSelectParens(PEGTransformer &transformer,
                                                   unique_ptr<SelectStatement> simple_select) {
	return simple_select;
}

unique_ptr<SelectNode> PEGTransformerFactory::TransformSelectFromClause(PEGTransformer &transformer,
                                                                        unique_ptr<SelectNode> select_clause,
                                                                        optional<unique_ptr<TableRef>> from_clause) {
	if (from_clause) {
		select_clause->from_table = std::move(*from_clause);
	} else {
		select_clause->from_table = make_uniq<EmptyTableRef>();
	}
	return select_clause;
}

unique_ptr<SelectNode>
PEGTransformerFactory::TransformFromSelectClause(PEGTransformer &transformer, unique_ptr<TableRef> from_clause,
                                                 optional<unique_ptr<SelectNode>> select_clause) {
	unique_ptr<SelectNode> result;
	if (!select_clause) {
		result = make_uniq<SelectNode>();
		result->select_list.push_back(make_uniq<StarExpression>());
	} else {
		result = std::move(*select_clause);
	}
	result->from_table = std::move(from_clause);
	return result;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformUsingKey(PEGTransformer &transformer,
                                         vector<unique_ptr<ParsedExpression>> target_list) {
	return target_list;
}

unique_ptr<SelectNode>
PEGTransformerFactory::TransformSelectClause(PEGTransformer &transformer, optional<DistinctClause> distinct_clause,
                                             optional<vector<unique_ptr<ParsedExpression>>> target_list) {
	auto result = make_uniq<SelectNode>();
	if (distinct_clause && distinct_clause->is_distinct) {
		auto distinct_modifier = make_uniq<DistinctModifier>();
		for (auto &distinct_on : distinct_clause->distinct_targets) {
			distinct_modifier->distinct_on_targets.push_back(distinct_on->Copy());
		}
		result->modifiers.push_back(std::move(distinct_modifier));
	}
	if (!target_list) {
		throw ParserException("SELECT clause without selection list");
	}
	for (auto &expr_ptr : *target_list) {
		result->select_list.push_back(std::move(expr_ptr));
	}
	return result;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformTargetList(PEGTransformer &transformer,
                                           vector<unique_ptr<ParsedExpression>> aliased_expression) {
	return aliased_expression;
}

vector<string> PEGTransformerFactory::TransformColumnAliases(PEGTransformer &transformer,
                                                             const vector<Identifier> &col_id_or_string) {
	return IdentifiersToStrings(col_id_or_string);
}

DistinctClause PEGTransformerFactory::TransformDistinctAll(PEGTransformer &transformer) {
	DistinctClause result;
	result.is_distinct = false;
	return result;
}

DistinctClause
PEGTransformerFactory::TransformDistinctOn(PEGTransformer &transformer,
                                           optional<vector<unique_ptr<ParsedExpression>>> distinct_on_targets) {
	DistinctClause result;
	result.is_distinct = true;
	if (distinct_on_targets) {
		result.distinct_targets = std::move(*distinct_on_targets);
	}
	return result;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformDistinctOnTargets(PEGTransformer &transformer,
                                                  vector<unique_ptr<ParsedExpression>> expression) {
	return expression;
}

unique_ptr<TableRef> PEGTransformerFactory::TransformTableSubquery(PEGTransformer &transformer,
                                                                   const optional<Identifier> &table_alias_colon,
                                                                   const optional<bool> &lateral,
                                                                   unique_ptr<TableRef> subquery_reference,
                                                                   const optional<TableAlias> &table_alias,
                                                                   optional<unique_ptr<SampleOptions>> sample_clause) {
	if (table_alias_colon && table_alias) {
		throw ParserException("Table reference %s cannot have two aliases", subquery_reference->ToString());
	}
	if (lateral.value_or(false) && sample_clause) {
		throw ParserException("TABLESAMPLE cannot be applied to a LATERAL subquery");
	}
	if (table_alias_colon) {
		subquery_reference->alias = *table_alias_colon;
	}
	if (table_alias) {
		subquery_reference->alias = table_alias->name;
		subquery_reference->column_name_alias = table_alias->column_name_alias;
	}
	if (sample_clause) {
		subquery_reference->sample = std::move(*sample_clause);
	}
	return subquery_reference;
}

unique_ptr<TableRef> PEGTransformerFactory::TransformBaseTableRef(PEGTransformer &transformer,
                                                                  const optional<Identifier> &table_alias_colon,
                                                                  unique_ptr<BaseTableRef> base_table_name,
                                                                  const optional<TableAlias> &table_alias,
                                                                  optional<unique_ptr<AtClause>> at_clause,
                                                                  optional<unique_ptr<SampleOptions>> sample_clause) {
	if (table_alias_colon) {
		base_table_name->alias = *table_alias_colon;
	}
	if (table_alias && table_alias_colon) {
		throw ParserException("Table reference %s cannot have two aliases", base_table_name->ToString());
	}
	if (table_alias) {
		base_table_name->alias = table_alias->name;
		base_table_name->column_name_alias = table_alias->column_name_alias;
	}
	if (at_clause) {
		base_table_name->at_clause = std::move(*at_clause);
	}
	if (sample_clause) {
		base_table_name->sample = std::move(*sample_clause);
	}
	return std::move(base_table_name);
}

Identifier PEGTransformerFactory::TransformTableAliasColon(PEGTransformer &transformer,
                                                           const Identifier &col_id_or_string) {
	return col_id_or_string;
}

unique_ptr<TableRef> PEGTransformerFactory::TransformValuesRef(PEGTransformer &transformer,
                                                               const optional<Identifier> &table_alias_colon,
                                                               unique_ptr<SelectStatement> values_clause,
                                                               const optional<TableAlias> &table_alias,
                                                               optional<unique_ptr<SampleOptions>> sample_clause) {
	if (table_alias_colon && table_alias) {
		throw ParserException("Table reference cannot have two aliases");
	}
	auto subquery_ref = make_uniq<SubqueryRef>(std::move(values_clause));
	if (table_alias_colon) {
		subquery_ref->alias = *table_alias_colon;
	}
	if (table_alias) {
		subquery_ref->alias = table_alias->name;
		subquery_ref->column_name_alias = table_alias->column_name_alias;
	}
	if (sample_clause) {
		subquery_ref->sample = std::move(*sample_clause);
	}
	return std::move(subquery_ref);
}

unique_ptr<TableRef> PEGTransformerFactory::TransformParensTableRef(PEGTransformer &transformer,
                                                                    const optional<Identifier> &table_alias_colon,
                                                                    unique_ptr<TableRef> table_ref,
                                                                    const optional<TableAlias> &table_alias,
                                                                    optional<unique_ptr<SampleOptions>> sample_clause) {
	if (table_alias_colon && table_alias) {
		throw ParserException("Table reference %s cannot have two aliases", table_ref->ToString());
	}
	if (!table_alias_colon && !table_alias && !sample_clause) {
		return table_ref;
	}
	auto select_statement = make_uniq<SelectStatement>();
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(table_ref);
	select_statement->node = std::move(select_node);
	auto subquery = make_uniq<SubqueryRef>(std::move(select_statement));
	if (table_alias_colon) {
		subquery->alias = *table_alias_colon;
	}
	if (table_alias) {
		subquery->alias = table_alias->name;
		subquery->column_name_alias = table_alias->column_name_alias;
	}
	if (sample_clause) {
		subquery->sample = std::move(*sample_clause);
	}
	return std::move(subquery);
}

vector<string> PEGTransformerFactory::TransformPivotGroupByList(PEGTransformer &transformer,
                                                                const vector<string> &optional_parens_name_list) {
	return optional_parens_name_list;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformPivotHeader(PEGTransformer &transformer,
                                                                         unique_ptr<ParsedExpression> base_expression) {
	return base_expression;
}

PivotColumn PEGTransformerFactory::TransformUnpivotValueList(PEGTransformer &transformer,
                                                             const vector<string> &unpivot_header,
                                                             vector<PivotColumnEntry> unpivot_target_list) {
	PivotColumn result;
	result.unpivot_names = StringsToIdentifiers(unpivot_header);
	if (result.unpivot_names.size() != 1) {
		throw ParserException("UNPIVOT requires a single column name for the PIVOT IN clause");
	}
	result.entries = std::move(unpivot_target_list);
	return result;
}

vector<PivotColumnEntry>
PEGTransformerFactory::TransformPivotTargetList(PEGTransformer &transformer,
                                                vector<unique_ptr<ParsedExpression>> target_list) {
	vector<PivotColumnEntry> result;
	for (auto &target : target_list) {
		PivotColumnEntry pivot_entry;
		pivot_entry.alias = target->GetAlias();
		bool transformed = TransformPivotInList(target, pivot_entry);
		if (!transformed || pivot_entry.values.empty()) {
			pivot_entry.expr = std::move(target);
		}
		result.push_back(std::move(pivot_entry));
	}
	return result;
}

vector<PivotColumnEntry>
PEGTransformerFactory::TransformUnpivotTargetList(PEGTransformer &transformer,
                                                  vector<unique_ptr<ParsedExpression>> target_list) {
	vector<PivotColumnEntry> result;
	for (auto &target : target_list) {
		PivotColumnEntry pivot_entry;
		pivot_entry.alias = target->GetAlias();
		pivot_entry.expr = std::move(target);
		result.push_back(std::move(pivot_entry));
	}
	return result;
}

unique_ptr<BaseTableRef> PEGTransformerFactory::TransformSchemaReservedTable(PEGTransformer &transformer,
                                                                             const Identifier &schema_qualification,
                                                                             const Identifier &reserved_table_name) {
	const auto description = TableDescription(QualifiedName({schema_qualification}, reserved_table_name));
	return make_uniq<BaseTableRef>(description);
}

unique_ptr<BaseTableRef> PEGTransformerFactory::TransformCatalogReservedSchemaTable(
    PEGTransformer &transformer, const Identifier &catalog_qualification,
    const vector<Identifier> &reserved_schema_qualification, const Identifier &reserved_table_name) {
	vector<Identifier> qualification;
	qualification.push_back(catalog_qualification);
	for (auto &schema : reserved_schema_qualification) {
		qualification.push_back(schema);
	}
	const auto description = TableDescription(QualifiedName(std::move(qualification), reserved_table_name));
	return make_uniq<BaseTableRef>(description);
}

QualifiedName PEGTransformerFactory::TransformQualifiedTableFunction(
    PEGTransformer &transformer, const optional<Identifier> &catalog_qualification,
    const optional<vector<Identifier>> &schema_qualification, const Identifier &table_function_name) {
	vector<Identifier> qualification;
	if (catalog_qualification) {
		qualification.push_back(*catalog_qualification);
	}
	if (schema_qualification) {
		for (auto &schema : *schema_qualification) {
			qualification.push_back(schema);
		}
	}
	return QualifiedName(std::move(qualification), table_function_name);
}

vector<FunctionArgument>
PEGTransformerFactory::TransformTableFunctionArguments(PEGTransformer &transformer,
                                                       optional<vector<FunctionArgument>> function_argument) {
	if (function_argument) {
		return std::move(*function_argument);
	}
	return {};
}

TableAlias PEGTransformerFactory::TransformTableAliasAs(PEGTransformer &transformer,
                                                        const QualifiedName &identifier_or_string_literal,
                                                        const optional<vector<string>> &column_aliases) {
	TableAlias result;
	result.name = identifier_or_string_literal.Name();
	if (column_aliases) {
		result.column_name_alias = StringsToIdentifiers(*column_aliases);
	}
	return result;
}

TableAlias PEGTransformerFactory::TransformTableAliasWithoutAs(PEGTransformer &transformer,
                                                               const Identifier &identifier,
                                                               const optional<vector<string>> &column_aliases) {
	TableAlias result;
	result.name = identifier;
	if (column_aliases) {
		result.column_name_alias = StringsToIdentifiers(*column_aliases);
	}
	return result;
}

unique_ptr<AtClause> PEGTransformerFactory::TransformAtClause(PEGTransformer &transformer,
                                                              unique_ptr<AtClause> at_specifier) {
	return at_specifier;
}

unique_ptr<AtClause> PEGTransformerFactory::TransformAtSpecifier(PEGTransformer &transformer, const string &at_unit,
                                                                 unique_ptr<ParsedExpression> expression) {
	return make_uniq<AtClause>(Identifier(at_unit), std::move(expression));
}

unique_ptr<TableRef> PEGTransformerFactory::TransformJoinWithoutOnClause(PEGTransformer &transformer,
                                                                         const JoinPrefix &join_prefix,
                                                                         unique_ptr<TableRef> inner_table_ref) {
	auto result = make_uniq<JoinRef>();
	result->ref_type = join_prefix.ref_type;
	result->type = join_prefix.join_type;
	result->right = std::move(inner_table_ref);
	return std::move(result);
}

JoinQualifier PEGTransformerFactory::TransformOnClause(PEGTransformer &transformer,
                                                       unique_ptr<ParsedExpression> expression) {
	JoinQualifier result;
	result.on_clause = std::move(expression);
	return result;
}

JoinQualifier PEGTransformerFactory::TransformUsingClause(PEGTransformer &transformer,
                                                          const vector<Identifier> &column_name) {
	JoinQualifier result;
	for (auto &col_identifier : column_name) {
		if (col_identifier.empty()) {
			throw ParserException("Column identifier cannot be empty");
		}
		result.using_columns.push_back(col_identifier);
	}
	return result;
}

JoinPrefix PEGTransformerFactory::TransformCrossJoinPrefix(PEGTransformer &transformer) {
	JoinPrefix result;
	result.ref_type = JoinRefType::CROSS;
	return result;
}

JoinPrefix PEGTransformerFactory::TransformNaturalJoinPrefix(PEGTransformer &transformer,
                                                             const optional<JoinType> &join_type) {
	JoinPrefix result;
	result.ref_type = JoinRefType::NATURAL;
	result.join_type = join_type.value_or(JoinType::INNER);
	return result;
}

JoinPrefix PEGTransformerFactory::TransformPositionalJoinPrefix(PEGTransformer &transformer) {
	JoinPrefix result;
	result.ref_type = JoinRefType::POSITIONAL;
	return result;
}

unique_ptr<TableRef> PEGTransformerFactory::TransformFromClause(PEGTransformer &transformer,
                                                                vector<unique_ptr<TableRef>> table_ref) {
	auto result_table_ref = std::move(table_ref[0]);
	if (table_ref.size() == 1) {
		return result_table_ref;
	}
	for (idx_t i = 1; i < table_ref.size(); i++) {
		auto cross_product = make_uniq<JoinRef>();
		cross_product->left = std::move(result_table_ref);
		cross_product->right = std::move(table_ref[i]);
		cross_product->ref_type = JoinRefType::CROSS;
		cross_product->is_implicit = true;
		result_table_ref = std::move(cross_product);
	}
	return result_table_ref;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformWhereClause(PEGTransformer &transformer,
                                                                         unique_ptr<ParsedExpression> expression) {
	return expression;
}

GroupByNode PEGTransformerFactory::TransformGroupByClause(PEGTransformer &transformer,
                                                          GroupByNode group_by_expressions) {
	return group_by_expressions;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformHavingClause(PEGTransformer &transformer,
                                                                          unique_ptr<ParsedExpression> expression) {
	return expression;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformQualifyClause(PEGTransformer &transformer,
                                                                           unique_ptr<ParsedExpression> expression) {
	return expression;
}

unique_ptr<SampleOptions> PEGTransformerFactory::TransformSampleClause(PEGTransformer &transformer,
                                                                       unique_ptr<SampleOptions> sample_entry) {
	return sample_entry;
}

unique_ptr<SampleOptions> PEGTransformerFactory::TransformSelectSampleClause(PEGTransformer &transformer,
                                                                             unique_ptr<SampleOptions> sample_entry) {
	return sample_entry;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformWindowClause(PEGTransformer &transformer,
                                             vector<unique_ptr<ParsedExpression>> window_definition) {
	return window_definition;
}

SampleMethod PEGTransformerFactory::TransformSampleFunction(PEGTransformer &transformer, const Identifier &col_id) {
	return EnumUtil::FromString<SampleMethod>(col_id.GetIdentifierName());
}

pair<SampleMethod, optional_idx>
PEGTransformerFactory::TransformSampleProperties(PEGTransformer &transformer, const Identifier &col_id,
                                                 const optional<optional_idx> &sample_seed) {
	return make_pair(EnumUtil::FromString<SampleMethod>(col_id.GetIdentifierName()),
	                 sample_seed ? *sample_seed : optional_idx());
}

optional_idx PEGTransformerFactory::TransformRepeatableSample(PEGTransformer &transformer,
                                                              const optional_idx &sample_seed) {
	return sample_seed;
}

optional_idx PEGTransformerFactory::TransformSampleSeed(PEGTransformer &transformer,
                                                        unique_ptr<ParsedExpression> number_literal) {
	auto &const_expr = number_literal->Cast<ConstantExpression>();
	auto seed = const_expr.GetLiteral().ToValue().GetValue<idx_t>();
	if (seed > static_cast<idx_t>(NumericLimits<int64_t>::Maximum())) {
		// larger seeds are seen as "no seed" and give a different sample every time
		throw ParserException("REPEATABLE seed %llu is out of range, the maximum is %lld", seed,
		                      NumericLimits<int64_t>::Maximum());
	}
	return optional_idx(seed);
}

unique_ptr<SampleOptions> PEGTransformerFactory::TransformSampleCount(PEGTransformer &transformer,
                                                                      unique_ptr<ParsedExpression> sample_value,
                                                                      const optional<bool> &sample_unit) {
	auto result = make_uniq<SampleOptions>();
	if (sample_value->GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw ParserException(sample_value->GetQueryLocation(),
		                      "Only constants are supported in sample clause currently");
	}
	auto &const_expr = sample_value->Cast<ConstantExpression>();
	auto sample_value_const = const_expr.GetLiteral().ToValue();
	result->is_percentage = sample_unit.value_or(false);
	if (result->is_percentage) {
		auto percentage = sample_value_const.GetValue<double>();
		if (percentage < 0 || percentage > 100) {
			throw ParserException("Sample sample_size %llf out of range, must be between 0 and 100", percentage);
		}
		result->sample_size = Value::DOUBLE(percentage);
		result->method = SampleMethod::SYSTEM_SAMPLE;
	} else {
		auto rows = sample_value_const.GetValue<int64_t>();
		if (rows < 0 || sample_value_const.GetValue<uint64_t>() > SampleOptions::MAX_SAMPLE_ROWS) {
			throw ParserException("Sample rows %lld out of range, must be between 0 and %lld", rows,
			                      SampleOptions::MAX_SAMPLE_ROWS);
		}
		result->sample_size = Value::BIGINT(rows);
		result->method = SampleMethod::RESERVOIR_SAMPLE;
	}
	return result;
}

GroupByNode PEGTransformerFactory::TransformGroupByAll(PEGTransformer &transformer) {
	GroupByNode result;
	result.group_expressions.push_back(make_uniq<StarExpression>());
	return result;
}

unique_ptr<TableRef>
PEGTransformerFactory::TransformSubqueryReference(PEGTransformer &transformer,
                                                  unique_ptr<SelectStatement> select_statement_internal) {
	return make_uniq<SubqueryRef>(std::move(select_statement_internal));
}

OrderByNode PEGTransformerFactory::TransformOrderByExpression(PEGTransformer &transformer,
                                                              unique_ptr<ParsedExpression> expression,
                                                              const optional<OrderType> &desc_or_asc,
                                                              const optional<OrderByNullType> &nulls_first_or_last) {
	return OrderByNode(desc_or_asc.value_or(OrderType::ORDER_DEFAULT),
	                   nulls_first_or_last.value_or(OrderByNullType::ORDER_DEFAULT), std::move(expression));
}

vector<OrderByNode> PEGTransformerFactory::TransformOrderByClause(PEGTransformer &transformer,
                                                                  vector<OrderByNode> order_by_expressions) {
	return order_by_expressions;
}

vector<OrderByNode> PEGTransformerFactory::TransformOrderByExpressionList(PEGTransformer &transformer,
                                                                          vector<OrderByNode> order_by_expression) {
	return order_by_expression;
}

vector<OrderByNode> PEGTransformerFactory::TransformOrderByAll(PEGTransformer &transformer,
                                                               const optional<OrderType> &desc_or_asc,
                                                               const optional<OrderByNullType> &nulls_first_or_last) {
	vector<OrderByNode> result;
	auto star_expr = make_uniq<StarExpression>();
	star_expr->IsColumnsMutable() = true;
	result.push_back(OrderByNode(desc_or_asc.value_or(OrderType::ORDER_DEFAULT),
	                             nulls_first_or_last.value_or(OrderByNullType::ORDER_DEFAULT), std::move(star_expr)));
	return result;
}

LimitPercentResult PEGTransformerFactory::TransformLimitClause(PEGTransformer &transformer,
                                                               LimitPercentResult limit_value) {
	return limit_value;
}

LimitPercentResult PEGTransformerFactory::TransformOffsetClause(PEGTransformer &transformer,
                                                                LimitPercentResult offset_value) {
	return offset_value;
}

LimitPercentResult PEGTransformerFactory::TransformOffsetValue(PEGTransformer &transformer,
                                                               unique_ptr<ParsedExpression> expression,
                                                               const bool &has_result) {
	LimitPercentResult result;
	result.expression = std::move(expression);
	return result;
}

LimitPercentResult PEGTransformerFactory::TransformLimitAll(PEGTransformer &transformer) {
	LimitPercentResult result;
	result.expression = ConstantExpression::Null();
	result.is_percent = false;
	return result;
}

LimitPercentResult PEGTransformerFactory::TransformLimitLiteralPercent(PEGTransformer &transformer,
                                                                       unique_ptr<ParsedExpression> number_literal) {
	LimitPercentResult result;
	result.expression = std::move(number_literal);
	result.is_percent = true;
	return result;
}

LimitPercentResult PEGTransformerFactory::TransformFetchValue(PEGTransformer &transformer,
                                                              unique_ptr<ParsedExpression> expression) {
	LimitPercentResult result;
	result.expression = std::move(expression);
	return result;
}

LimitPercentResult PEGTransformerFactory::TransformFetchClauseWithoutValue(PEGTransformer &transformer) {
	LimitPercentResult result;
	result.expression = ConstantExpression::Integer(1);
	return result;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformColIdExpression(PEGTransformer &transformer,
                                                                             const Identifier &col_id,
                                                                             unique_ptr<ParsedExpression> expression) {
	expression->SetAlias(col_id);
	return expression;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformExpressionAsCollabel(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> expression, const Identifier &col_label_or_string) {
	expression->SetAlias(col_label_or_string);
	return expression;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformExpressionOptIdentifier(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> expression, const optional<Identifier> &identifier) {
	if (identifier) {
		expression->SetAlias(*identifier);
	}
	return expression;
}

unique_ptr<SelectStatement>
PEGTransformerFactory::TransformValuesClause(PEGTransformer &transformer,
                                             vector<vector<unique_ptr<ParsedExpression>>> values_expressions) {
	if (values_expressions.size() > 1) {
		const auto expected_size = values_expressions[0].size();
		for (idx_t i = 1; i < values_expressions.size(); i++) {
			if (values_expressions[i].size() != expected_size) {
				throw ParserException(
				    *values_expressions[i][0],
				    "VALUES lists must all be the same length, expected %d %s but found a list with %d %s",
				    expected_size, expected_size == 1 ? "entry" : "entries", values_expressions[i].size(),
				    values_expressions[i].size() == 1 ? "entry" : "entries");
			}
		}
	}
	auto result = make_uniq<ExpressionListRef>();
	result->alias = "valueslist";
	result->values = std::move(values_expressions);

	auto select_node = make_uniq<SelectNode>();
	select_node->from_table = std::move(result);
	select_node->select_list.push_back(make_uniq<StarExpression>());
	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(select_node);
	return select_statement;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformValuesExpressions(PEGTransformer &transformer,
                                                  vector<unique_ptr<ParsedExpression>> expression) {
	return expression;
}

} // namespace duckdb
