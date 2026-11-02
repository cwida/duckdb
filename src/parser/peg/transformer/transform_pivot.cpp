#include "duckdb/parser/peg/ast/unpivot_name_values.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"

namespace duckdb {

void PEGTransformerFactory::AddPivotEntry(PEGTransformer &transformer, string enum_name, unique_ptr<SelectNode> base,
                                          unique_ptr<ParsedExpression> column, unique_ptr<QueryNode> subquery,
                                          bool has_parameters) {
	auto result = make_uniq<CreatePivotEntry>();
	result->enum_name = std::move(enum_name);
	result->base = std::move(base);
	result->column = std::move(column);
	result->subquery = std::move(subquery);
	result->has_parameters = has_parameters;
	transformer.pivot_entries.push_back(std::move(result));
}

vector<PivotColumn> PEGTransformerFactory::TransformPivotOn(PEGTransformer &transformer,
                                                            vector<PivotColumn> pivot_column_list) {
	return pivot_column_list;
}

vector<PivotColumn> PEGTransformerFactory::TransformPivotColumnList(PEGTransformer &transformer,
                                                                    vector<PivotColumn> pivot_column_entry) {
	for (auto &col : pivot_column_entry) {
		// Re-wrap multiple pivot expressions into a single row() only when IN entries are
		// scalar (size 1). For tuple IN entries the unpacked expressions must stay separate
		// so that each value maps to its corresponding pivot expression.
		if (col.pivot_expressions.size() > 1) {
			bool has_tuple_entries = false;
			for (auto &entry : col.entries) {
				if (entry.values.size() > 1) {
					has_tuple_entries = true;
					break;
				}
			}
			if (!has_tuple_entries) {
				auto row_function = make_uniq<FunctionExpression>("row", std::move(col.pivot_expressions));
				col.pivot_expressions.push_back(std::move(row_function));
			}
		}
		for (auto &expr : col.pivot_expressions) {
			if (expr->IsScalar()) {
				throw ParserException(expr->GetQueryLocation(), "Cannot pivot on constant value \"%s\"",
				                      expr->ToString());
			}
			if (expr->HasSubquery()) {
				throw ParserException(expr->GetQueryLocation(), "Cannot pivot on subquery \"%s\"", expr->ToString());
			}
		}
	}
	return pivot_column_entry;
}

PivotColumn PEGTransformerFactory::TransformPivotColumnSubquery(PEGTransformer &transformer,
                                                                unique_ptr<ParsedExpression> base_expression,
                                                                unique_ptr<SelectStatement> select_statement_internal) {
	PivotColumn result;
	result.pivot_expressions.push_back(std::move(base_expression));
	result.subquery = std::move(select_statement_internal->node);
	return result;
}

PivotColumn PEGTransformerFactory::TransformPivotColumnExpression(PEGTransformer &transformer,
                                                                  unique_ptr<ParsedExpression> expression) {
	PivotColumn result;
	result.pivot_expressions.push_back(std::move(expression));
	return result;
}

void PEGTransformerFactory::InitializePivotStatementTrampoline(PEGTransformer &transformer,
                                                               GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	process.manual_state = transformer.ParamCount();
	process.ReserveChildSlots(5);
	process.PushChild({list_pr.GetChild(1)}, 0);
}

arena_ptr<TransformResultValue>
PEGTransformerFactory::FinalizePivotStatementTrampoline(PEGTransformer &transformer,
                                                        GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	auto &pivot_columns = list_pr.Child<OptionalParseResult>(2);
	auto &pivot_aggregates = list_pr.Child<OptionalParseResult>(3);
	auto &pivot_group = list_pr.Child<OptionalParseResult>(4);
	if (!pivot_columns.HasResult() && !pivot_aggregates.HasResult() && !pivot_group.HasResult()) {
		throw ParserException("PIVOT requires an ON, USING or GROUP BY clause");
	}
	if (!process.child_results[4]) {
		bool has_parameters = transformer.ParamCount() > process.manual_state;
		process.SetChildResult(4, transformer.MakeResult<bool>(has_parameters));
		bool pushed_child = false;
		if (pivot_group.HasResult()) {
			process.PushChild({pivot_group.GetResult()}, 3);
			pushed_child = true;
		}
		if (pivot_aggregates.HasResult()) {
			process.PushChild({pivot_aggregates.GetResult()}, 2);
			pushed_child = true;
		}
		if (pivot_columns.HasResult()) {
			process.PushChild({pivot_columns.GetResult()}, 1);
			pushed_child = true;
		}
		if (pushed_child) {
			return nullptr;
		}
	}
	auto source = process.TakeResult<unique_ptr<TableRef>>(0);
	auto has_parameters = process.TakeResult<bool>(4);
	auto select_node = make_uniq<SelectNode>();
	if (!pivot_columns.HasResult()) {
		select_node->from_table = std::move(source);
		if (pivot_group.HasResult()) {
			auto pivot_group_list = process.TakeResult<vector<string>>(3);
			GroupingSet set;
			for (idx_t gr = 0; gr < pivot_group_list.size(); gr++) {
				auto &group = pivot_group_list[gr];
				auto colref = make_uniq<ColumnRefExpression>(Identifier(group));
				select_node->select_list.push_back(colref->Copy());
				select_node->groups.group_expressions.push_back(std::move(colref));
				set.insert(ProjectionIndex(gr));
			}
			select_node->groups.grouping_sets.push_back(std::move(set));
		}
		auto &pivot_using_opt = list_pr.Child<OptionalParseResult>(3);
		if (pivot_using_opt.HasResult()) {
			auto pivot_using = process.TakeResult<vector<unique_ptr<ParsedExpression>>>(2);
			for (auto &col : pivot_using) {
				select_node->select_list.push_back(std::move(col));
			}
		}
		auto result = make_uniq<SelectStatement>();
		result->node = std::move(select_node);
		return transformer.MakeResult<unique_ptr<SelectStatement>>(std::move(result));
	}
	auto columns = process.TakeResult<vector<PivotColumn>>(1);
	for (idx_t c = 0; c < columns.size(); c++) {
		auto &col = columns[c];
		if (!col.pivot_enum.empty() || !col.entries.empty()) {
			continue;
		}
		if (col.pivot_expressions.size() != 1) {
			throw InternalException("PIVOT statement with multiple names in pivot entry!?");
		}
		auto enum_name = "__pivot_enum_" + UUID::ToString(UUID::GenerateRandomUUID());

		auto new_select = make_uniq<SelectNode>();
		transformer.ExtractCTEsRecursive(new_select->cte_map);
		new_select->from_table = source->Copy();
		AddPivotEntry(transformer, enum_name, std::move(new_select), col.pivot_expressions[0]->Copy(),
		              std::move(col.subquery), has_parameters);
		col.pivot_enum = Identifier(enum_name);
	}

	select_node->select_list.push_back(make_uniq<StarExpression>());
	auto pivot_ref = make_uniq<PivotRef>();
	pivot_ref->source = std::move(source);
	if (pivot_aggregates.HasResult()) {
		pivot_ref->aggregates = process.TakeResult<vector<unique_ptr<ParsedExpression>>>(2);
	} else {
		vector<unique_ptr<ParsedExpression>> children;
		auto function = make_uniq<FunctionExpression>("count_star", std::move(children));
		pivot_ref->aggregates.push_back(std::move(function));
	}
	if (pivot_group.HasResult()) {
		pivot_ref->groups = StringsToIdentifiers(process.TakeResult<vector<string>>(3));
	}
	pivot_ref->pivots = std::move(columns);
	select_node->from_table = std::move(pivot_ref);
	auto result = make_uniq<SelectStatement>();
	result->node = std::move(select_node);
	return transformer.MakeResult<unique_ptr<SelectStatement>>(std::move(result));
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformPivotUsing(PEGTransformer &transformer,
                                           vector<unique_ptr<ParsedExpression>> target_list) {
	return target_list;
}

vector<string> PEGTransformerFactory::TransformUnpivotHeaderSingle(PEGTransformer &transformer,
                                                                   const Identifier &col_id_or_string) {
	vector<string> result;
	result.push_back(col_id_or_string.GetIdentifierName());
	return result;
}

vector<string> PEGTransformerFactory::TransformUnpivotHeaderList(PEGTransformer &transformer,
                                                                 const vector<Identifier> &col_id_or_string) {
	return IdentifiersToStrings(col_id_or_string);
}

vector<string> PEGTransformerFactory::TransformParenthesizedNameList(PEGTransformer &transformer,
                                                                     const vector<Identifier> &col_id_or_string) {
	return IdentifiersToStrings(col_id_or_string);
}

vector<string> PEGTransformerFactory::TransformBareNameList(PEGTransformer &transformer,
                                                            const vector<Identifier> &col_id_or_string) {
	return IdentifiersToStrings(col_id_or_string);
}

bool PEGTransformerFactory::TransformIncludeNulls(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformExcludeNulls(PEGTransformer &transformer) {
	return false;
}

void PEGTransformerFactory::InitializeUnpivotStatementTrampoline(PEGTransformer &transformer,
                                                                 GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	process.manual_state = transformer.ParamCount();
	process.ReserveChildSlots(4);
	process.PushChild({list_pr.GetChild(1)}, 0);
}

arena_ptr<TransformResultValue>
PEGTransformerFactory::FinalizeUnpivotStatementTrampoline(PEGTransformer &transformer,
                                                          GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	if (!process.child_results[3]) {
		bool has_parameters = transformer.ParamCount() > process.manual_state;
		process.SetChildResult(3, transformer.MakeResult<bool>(has_parameters));
		auto &unpivot_names_opt = list_pr.Child<OptionalParseResult>(4);
		if (unpivot_names_opt.HasResult()) {
			process.PushChild({unpivot_names_opt.GetResult()}, 2);
		}
		process.PushChild({list_pr.GetChild(3)}, 1);
		return nullptr;
	}
	auto source = process.TakeResult<unique_ptr<TableRef>>(0);
	auto has_parameters = process.TakeResult<bool>(3);
	auto target_list = process.TakeResult<vector<unique_ptr<ParsedExpression>>>(1);
	auto &unpivot_names_opt = list_pr.Child<OptionalParseResult>(4);
	vector<PivotColumn> columns;
	UnpivotNameValues name_and_values;
	if (!unpivot_names_opt.HasResult()) {
		PivotColumn col;
		col.unpivot_names.push_back("name");
		for (auto &expr : target_list) {
			PivotColumnEntry entry;
			entry.alias = expr->GetAlias();
			entry.expr = std::move(expr);
			col.entries.push_back(std::move(entry));
		}
		columns.push_back(std::move(col));
	} else {
		name_and_values = process.TakeResult<UnpivotNameValues>(2);
		auto unpivot_list_size = name_and_values.column.unpivot_names.size();
		if (unpivot_list_size != 1) {
			throw NotImplementedException("Only expected a single value after NAME, got %d instead.",
			                              unpivot_list_size);
		}
		auto &col = name_and_values.column;
		for (auto &expr : target_list) {
			PivotColumnEntry entry;
			entry.alias = expr->GetAlias();
			entry.expr = std::move(expr);
			col.entries.push_back(std::move(entry));
		}
		columns.push_back(std::move(col));
	}

	for (idx_t c = 0; c < columns.size(); c++) {
		auto &col = columns[c];
		if (!col.pivot_enum.empty() || !col.entries.empty()) {
			continue;
		}
		if (col.pivot_expressions.size() != 1) {
			throw InternalException("PIVOT statement with multiple names in pivot entry!?");
		}
		auto enum_name = "__pivot_enum_" + UUID::ToString(UUID::GenerateRandomUUID());

		auto new_select = make_uniq<SelectNode>();
		transformer.ExtractCTEsRecursive(new_select->cte_map);
		new_select->from_table = source->Copy();
		AddPivotEntry(transformer, enum_name, std::move(new_select), col.pivot_expressions[0]->Copy(),
		              std::move(col.subquery), has_parameters);
		col.pivot_enum = Identifier(enum_name);
	}

	auto pivot_ref = make_uniq<PivotRef>();
	pivot_ref->source = std::move(source);
	if (!unpivot_names_opt.HasResult()) {
		pivot_ref->unpivot_names.push_back("value");
	} else {
		pivot_ref->unpivot_names = name_and_values.unpivot_names;
	}

	pivot_ref->pivots = std::move(columns);
	auto select_node = make_uniq<SelectNode>();
	select_node->from_table = std::move(pivot_ref);
	select_node->select_list.push_back(make_uniq<StarExpression>());
	auto result = make_uniq<SelectStatement>();
	result->node = std::move(select_node);
	return transformer.MakeResult<unique_ptr<SelectStatement>>(std::move(result));
}

UnpivotNameValues PEGTransformerFactory::TransformIntoNameValues(PEGTransformer &transformer,
                                                                 const Identifier &col_id_or_string,
                                                                 const vector<string> &optional_parens_name_list) {
	UnpivotNameValues result;
	PivotColumn column;
	column.unpivot_names.push_back(Identifier(col_id_or_string));
	result.column = std::move(column);
	result.unpivot_names = StringsToIdentifiers(optional_parens_name_list);
	return result;
}
} // namespace duckdb
