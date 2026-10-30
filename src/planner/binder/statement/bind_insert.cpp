#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/collate_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/insert_binder.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression_binder/where_binder.hpp"
#include "duckdb/planner/expression_binder/update_binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/planner/expression/bound_default_expression.hpp"
#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/bound_ref_wrapper.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"

namespace duckdb {

void Binder::CheckInsertColumnCountMismatch(idx_t expected_columns, idx_t result_columns, bool columns_provided,
                                            const Identifier &tname) {
	if (result_columns != expected_columns) {
		string msg = StringUtil::Format(!columns_provided ? "table %s has %lld columns but %lld values were supplied"
		                                                  : "Column name/value mismatch for insert on %s: "
		                                                    "expected %lld columns but %lld values were supplied",
		                                tname, expected_columns, result_columns);
		throw BinderException(msg);
	}
}

unique_ptr<ParsedExpression> ExpandDefaultExpression(const ColumnDefinition &column) {
	if (column.HasDefaultValue()) {
		return column.DefaultValue().Copy();
	} else {
		return ConstantExpression::FromValue(Value(column.Type()));
	}
}

void Binder::TryReplaceDefaultExpression(unique_ptr<ParsedExpression> &expr, const ColumnDefinition &column) {
	if (expr->GetExpressionType() != ExpressionType::VALUE_DEFAULT) {
		return;
	}
	expr = ExpandDefaultExpression(column);
}

void Binder::ExpandDefaultInValuesList(InsertQueryNode &node, TableCatalogEntry &table,
                                       optional_ptr<ExpressionListRef> values_list,
                                       const vector<LogicalIndex> &named_column_map) {
	if (!values_list) {
		return;
	}
	idx_t expected_columns = node.columns.empty() ? table.GetColumns().PhysicalColumnCount() : node.columns.size();

	// special case: check if we are inserting from a VALUES statement
	auto &expr_list = values_list->Cast<ExpressionListRef>();
	expr_list.expected_types.resize(expected_columns);
	expr_list.expected_names.resize(expected_columns);

	D_ASSERT(!expr_list.values.empty());
	CheckInsertColumnCountMismatch(expected_columns, expr_list.values[0].size(), !node.columns.empty(), table.name);

	// VALUES list!
	for (idx_t col_idx = 0; col_idx < expected_columns; col_idx++) {
		D_ASSERT(named_column_map.size() >= col_idx);
		auto &table_col_idx = named_column_map[col_idx];

		// set the expected types as the types for the INSERT statement
		auto &column = table.GetColumn(table_col_idx);
		expr_list.expected_types[col_idx] = table.GetExpectedTypeForInsert(column);
		expr_list.expected_names[col_idx] = column.Name();

		// now replace any DEFAULT values with the corresponding default expression
		for (idx_t list_idx = 0; list_idx < expr_list.values.size(); list_idx++) {
			TryReplaceDefaultExpression(expr_list.values[list_idx][col_idx], column);
		}
	}
}

unique_ptr<LogicalOperator> Binder::ResolveInputProjection(LogicalInsert &insert,
                                                           const IndexVector<idx_t, PhysicalIndex> &column_index_map,
                                                           unique_ptr<LogicalOperator> root,
                                                           const vector<LogicalType> &source_types) {
	auto &table = insert.table;
	auto source_bindings = root->GetColumnBindings();
	vector<unique_ptr<Expression>> select_list;
	for (auto &col : table.GetColumns().Physical()) {
		auto storage_idx = col.StorageOid();
		auto mapped_index = column_index_map.empty() ? storage_idx : column_index_map[col.Physical()];
		if (mapped_index == DConstants::INVALID_INDEX) {
			// Push default value
			select_list.push_back(std::move(insert.bound_defaults[storage_idx]));
			continue;
		}
		auto &original_type = source_types[mapped_index];
		auto source_binding = source_bindings[mapped_index];
		auto expression = table.GetDefaultExpressionForColumn(context, original_type, col.Type(), source_binding,
		                                                      *insert.bound_defaults[storage_idx]);
		if (!expression->HasQueryLocation() && root->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			expression->SetQueryLocation(root->expressions[mapped_index]->GetQueryLocation());
		}
		select_list.push_back(std::move(expression));
	}

	bool can_inline_projection = root->type == LogicalOperatorType::LOGICAL_PROJECTION;
	if (can_inline_projection) {
		for (auto &expression : root->expressions) {
			if (expression->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
				can_inline_projection = false;
				break;
			}
		}
	}
	if (can_inline_projection) {
		auto &child_projection = root->Cast<LogicalProjection>();
		for (auto &expression : select_list) {
			ExpressionIterator::EnumerateExpression(expression, [&](unique_ptr<Expression> &child) {
				if (child->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
					return;
				}
				auto &column_ref = child->Cast<BoundColumnRefExpression>();
				if (column_ref.Binding().table_index != child_projection.table_index) {
					return;
				}
				child = child_projection.expressions[column_ref.Binding().column_index]->Copy();
			});
		}
		root = std::move(child_projection.children[0]);
	}

	auto projection = make_uniq<LogicalProjection>(GenerateTableIndex(), std::move(select_list));
	projection->AddChild(std::move(root));
	return std::move(projection);
}

void DoUpdateSetQualify(unique_ptr<ParsedExpression> &expr, const string &table_name,
                        vector<identifier_set_t> &lambda_params);

void DoUpdateSetQualifyInLambda(FunctionExpression &function, const string &table_name,
                                vector<identifier_set_t> &lambda_params) {
	for (auto &child : function.GetArgumentsMutable()) {
		if (child.GetExpression().GetExpressionClass() != ExpressionClass::LAMBDA) {
			DoUpdateSetQualify(child.GetExpressionMutable(), table_name, lambda_params);
			continue;
		}

		// Special-handling for LHS lambda parameters.
		// We do not qualify them, and we add them to the lambda_params vector.
		auto &lambda_expr = child.GetExpressionMutable()->Cast<LambdaExpression>();
		string error_message;
		auto column_ref_expressions = lambda_expr.ExtractColumnRefExpressions(error_message);

		if (!error_message.empty()) {
			// Possibly a JSON function, qualify both LHS and RHS.
			ParsedExpressionIterator::EnumerateChildren(
			    *lambda_expr.LeftMutable(),
			    [&](unique_ptr<ParsedExpression> &child) { DoUpdateSetQualify(child, table_name, lambda_params); });
			ParsedExpressionIterator::EnumerateChildren(
			    *lambda_expr.RightMutable(),
			    [&](unique_ptr<ParsedExpression> &child) { DoUpdateSetQualify(child, table_name, lambda_params); });
			continue;
		}

		// Push the lambda parameter names of this level.
		lambda_params.emplace_back();
		for (const auto &column_ref_expr : column_ref_expressions) {
			const auto &column_ref = column_ref_expr.get().Cast<ColumnRefExpression>();
			lambda_params.back().emplace(column_ref.GetName());
		}

		// Only qualify in the RHS of the expression.
		ParsedExpressionIterator::EnumerateChildren(
		    *lambda_expr.RightMutable(),
		    [&](unique_ptr<ParsedExpression> &child) { DoUpdateSetQualify(child, table_name, lambda_params); });

		lambda_params.pop_back();
	}
}

void DoUpdateSetQualify(unique_ptr<ParsedExpression> &expr, const string &table_name,
                        vector<identifier_set_t> &lambda_params) {
	// We avoid ambiguity with EXCLUDED columns by qualifying all column references.
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		auto &col_ref = expr->Cast<ColumnRefExpression>();
		if (col_ref.IsQualified()) {
			return;
		}

		// Don't qualify lambda parameters.
		if (LambdaExpression::IsLambdaParameter(lambda_params, col_ref.GetColumnName())) {
			return;
		}

		// Qualify the column reference.
		expr = make_uniq<ColumnRefExpression>(col_ref.GetColumnName(), Identifier(table_name));
		return;
	}
	case ExpressionClass::FUNCTION: {
		// Special-handling for lambdas, which are inside function expressions.
		auto &function = expr->Cast<FunctionExpression>();
		if (function.IsLambdaFunction()) {
			return DoUpdateSetQualifyInLambda(function, table_name, lambda_params);
		}
		break;
	}
	case ExpressionClass::SUBQUERY: {
		throw BinderException("DO UPDATE SET clause cannot contain a subquery");
	}
	default:
		break;
	}

	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { DoUpdateSetQualify(child, table_name, lambda_params); });
}

unique_ptr<UpdateSetInfo> CreateSetInfoForReplace(TableCatalogEntry &table, InsertQueryNode &insert,
                                                  const TableStorageInfo &storage_info) {
	auto set_info = make_uniq<UpdateSetInfo>();

	auto &columns = set_info->columns;
	// REPLACE is rewritten to UPDATE. Conflict-key columns are used to match existing rows and
	// should not be part of the SET list.
	unordered_set<column_t> conflict_columns;
	for (auto &index : storage_info.index_info) {
		if (!index.is_unique) {
			continue;
		}
		for (auto &column_id : index.column_set) {
			conflict_columns.insert(column_id);
		}
	}

	auto &column_list = table.GetColumns();
	if (insert.columns.empty()) {
		for (auto &column : column_list.Physical()) {
			// FIXME: can these column names be aliased somehow?
			if (conflict_columns.count(column.Physical().index)) {
				continue;
			}
			columns.emplace_back(column.Name());
		}
	} else {
		// a list of columns was explicitly supplied, only update those
		for (auto &name : insert.columns) {
			auto &column = column_list.GetColumn(name);
			if (conflict_columns.count(column.Physical().index)) {
				continue;
			}
			columns.push_back(name);
		}
	}

	// Create 'excluded' qualified column references of these columns
	for (auto &column : columns) {
		set_info->expressions.push_back(make_uniq<ColumnRefExpression>(column, "excluded"));
	}

	return set_info;
}

void Binder::BindInsertColumnList(TableCatalogEntry &table, vector<Identifier> &columns, bool default_values,
                                  vector<LogicalIndex> &named_column_map, vector<LogicalType> &expected_types,
                                  IndexVector<idx_t, PhysicalIndex> &column_index_map) {
	if (!columns.empty() || default_values) {
		// insertion statement specifies column list

		// create a mapping of (list index) -> (column index)
		identifier_map_t<idx_t> column_name_map;
		for (idx_t i = 0; i < columns.size(); i++) {
			auto entry = column_name_map.insert(make_pair(columns[i], i));
			if (!entry.second) {
				throw BinderException("Duplicate column name %s in INSERT", columns[i]);
			}
			auto column_index = table.GetColumnIndex(columns[i]);
			if (column_index.index == COLUMN_IDENTIFIER_ROW_ID) {
				throw BinderException("Cannot explicitly insert values into rowid column");
			}
			auto &col = table.GetColumn(column_index);
			if (col.Generated()) {
				throw BinderException("Cannot insert into a generated column");
			}
			expected_types.push_back(col.Type());
			named_column_map.push_back(column_index);
		}
		for (auto &col : table.GetColumns().Physical()) {
			auto entry = column_name_map.find(col.Name());
			if (entry == column_name_map.end()) {
				// column not specified, set index to DConstants::INVALID_INDEX
				column_index_map.push_back(DConstants::INVALID_INDEX);
			} else {
				// column was specified, set to the index
				column_index_map.push_back(entry->second);
			}
		}
	} else {
		// insert by position and no columns specified - insertion into all columns of the table
		// intentionally don't populate 'column_index_map' as an indication of this
		for (auto &col : table.GetColumns().Physical()) {
			named_column_map.push_back(col.Logical());
			expected_types.push_back(col.Type());
		}
	}
}

static identifier_set_t GetConflictColumnNames(const TableStorageInfo &storage_info, TableCatalogEntry &table) {
	unordered_set<column_t> conflict_column_ids;
	for (auto &index : storage_info.index_info) {
		if (!index.is_unique) {
			continue;
		}
		for (auto &col_id : index.column_set) {
			conflict_column_ids.insert(col_id);
		}
	}
	identifier_set_t conflict_column_names;
	for (auto &col : table.GetColumns().Physical()) {
		if (conflict_column_ids.count(col.Physical().index)) {
			conflict_column_names.insert(col.Name());
		}
	}
	return conflict_column_names;
}

static bool IsNotNullColumn(TableCatalogEntry &table, Identifier &column_name) {
	auto column_index = table.GetColumnIndex(column_name);
	for (auto &constraint : table.GetConstraints()) {
		if (constraint->type == ConstraintType::NOT_NULL &&
		    constraint->Cast<NotNullConstraint>().index == column_index) {
			return true;
		}
	}
	return false;
}

unique_ptr<MergeIntoStatement> Binder::GenerateMergeInto(InsertQueryNode &node, TableCatalogEntry &table) {
	D_ASSERT(node.on_conflict_info);

	auto &on_conflict_info = *node.on_conflict_info;
	auto merge_into = make_uniq<MergeIntoStatement>();
	// set up the target table
	string table_name = !node.table_ref->alias.empty() ? node.table_ref->alias.GetIdentifierName()
	                                                   : node.qualified_name.Name().GetIdentifierName();
	merge_into->node->target = std::move(node.table_ref);

	auto storage_info = table.GetStorageInfo(context);
	auto &columns = table.GetColumns();
	// set up the columns on which to join
	vector<vector<Identifier>> all_distinct_on_columns;
	if (on_conflict_info.indexed_columns.empty()) {
		// When omitting the conflict target, we derive the join columns from the primary key/unique constraints
		// traverse the primary key/unique constraints

		vector<unique_ptr<ParsedExpression>> join_conditions;
		// We check if there are any constraints on the table, if there aren't we throw an error.
		idx_t found_matching_indexes = 0;
		for (auto &index : storage_info.index_info) {
			if (!index.is_unique) {
				continue;
			}

			vector<unique_ptr<ParsedExpression>> and_children;
			auto &indexed_columns = index.column_set;
			vector<Identifier> distinct_on_columns;
			for (auto &column : columns.Physical()) {
				if (!indexed_columns.count(column.Physical().index)) {
					continue;
				}
				auto lhs = make_uniq<ColumnRefExpression>(column.Name(), Identifier(table_name));
				auto rhs = make_uniq<ColumnRefExpression>(column.Name(), "excluded");
				auto new_condition =
				    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(lhs), std::move(rhs));
				and_children.push_back(std::move(new_condition));
				distinct_on_columns.emplace_back(column.Name());
			}
			all_distinct_on_columns.push_back(std::move(distinct_on_columns));
			if (and_children.empty()) {
				continue;
			}
			unique_ptr<ParsedExpression> condition;
			if (and_children.size() == 1) {
				condition = std::move(and_children[0]);
			} else {
				// AND together
				condition = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(and_children));
			}
			join_conditions.push_back(std::move(condition));
			found_matching_indexes++;
		}
		unique_ptr<ParsedExpression> join_condition;
		if (join_conditions.size() == 1) {
			join_condition = std::move(join_conditions[0]);
		} else {
			// OR together
			join_condition =
			    make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(join_conditions));
		}
		merge_into->node->join_condition = std::move(join_condition);

		if (!found_matching_indexes) {
			throw BinderException("There are no UNIQUE/PRIMARY KEY constraints that refer to this table, specify ON "
			                      "CONFLICT columns manually");
		} else if (found_matching_indexes != 1) {
			if (on_conflict_info.action_type != OnConflictAction::NOTHING) {
				// When no conflict target is provided, and the action type is UPDATE,
				// we only allow the operation when only a single Index exists
				throw BinderException("Conflict target has to be provided for a DO UPDATE operation when the table has "
				                      "multiple UNIQUE/PRIMARY KEY constraints");
			}
		}
	} else {
		// when on conflict columns are explicitly provided - use them directly
		// first figure out if there is an index on the columns or not
		identifier_map_t<idx_t> specified_columns;
		for (idx_t i = 0; i < on_conflict_info.indexed_columns.size(); i++) {
			specified_columns[on_conflict_info.indexed_columns[i]] = i;
			auto column_index = table.GetColumnIndex(on_conflict_info.indexed_columns[i]);
			if (column_index.index == COLUMN_IDENTIFIER_ROW_ID) {
				throw BinderException("Cannot specify ROWID as ON CONFLICT target");
			}
			auto &col = columns.GetColumn(column_index);
			if (col.Generated()) {
				throw BinderException("Cannot specify a generated column as ON CONFLICT target");
			}
		}
		unordered_set<column_t> on_conflict_filter;
		for (auto &col : columns.Physical()) {
			auto entry = specified_columns.find(col.Name());
			if (entry != specified_columns.end()) {
				// column was specified, set to the index
				on_conflict_filter.insert(col.Physical().index);
			}
		}
		bool index_references_columns = false;
		for (auto &index : storage_info.index_info) {
			if (!index.is_unique) {
				continue;
			}
			bool index_matches = on_conflict_filter == index.column_set;
			if (index_matches) {
				index_references_columns = true;
				break;
			}
		}
		if (!index_references_columns) {
			// Same as before, this is essentially a no-op, turning this into a DO THROW instead
			// But since this makes no logical sense, it's probably better to throw an error
			throw BinderException("The specified columns as conflict target are not referenced by a UNIQUE/PRIMARY KEY "
			                      "CONSTRAINT or INDEX");
		}
		all_distinct_on_columns.emplace_back(on_conflict_info.indexed_columns);
		merge_into->node->using_columns = on_conflict_info.indexed_columns;
	}

	// the MERGE INTO only uses the column list as aliases of its source, so check it like a plain INSERT
	vector<LogicalIndex> named_column_map;
	auto values_list = node.GetValuesList();
	if (node.column_order == InsertColumnOrder::INSERT_BY_POSITION) {
		vector<LogicalType> expected_types;
		physical_index_vector_t<idx_t> column_index_map;
		BindInsertColumnList(table, node.columns, node.default_values, named_column_map, expected_types,
		                     column_index_map);
	}
	// expand any default values
	if (values_list) {
		if (named_column_map.empty()) {
			for (auto &col : columns.Physical()) {
				named_column_map.push_back(col.Logical());
			}
		}
		ExpandDefaultInValuesList(node, table, values_list, named_column_map);
	}
	// set up the data source
	unique_ptr<TableRef> source;
	if (node.select_statement && node.column_order == InsertColumnOrder::INSERT_BY_POSITION && !values_list) {
		// bind the SELECT once, to check its columns and keep its bind time effects single
		auto select_binder = Binder::CreateBinder(context, this);
		SubqueryRef select_ref(std::move(node.select_statement), "excluded");
		auto bound_select = select_binder->Bind(select_ref);
		auto expected_columns = node.columns.empty() ? columns.PhysicalColumnCount() : node.columns.size();
		CheckInsertColumnCountMismatch(expected_columns, bound_select.types.size(), !node.columns.empty(), table.name);
		// a parsed subquery around it takes the column aliases
		auto select_stmt = make_uniq<SelectStatement>();
		auto select_node = make_uniq<SelectNode>();
		select_node->select_list.push_back(make_uniq<StarExpression>());
		select_node->from_table = make_uniq<BoundRefWrapper>(std::move(bound_select), std::move(select_binder));
		select_stmt->node = std::move(select_node);
		source = make_uniq<SubqueryRef>(std::move(select_stmt), "excluded");
	} else if (node.select_statement) {
		source = make_uniq<SubqueryRef>(std::move(node.select_statement), "excluded");
	} else {
		source = make_uniq<EmptyTableRef>();
	}
	if (node.column_order == InsertColumnOrder::INSERT_BY_POSITION) {
		// if we are inserting by position add the columns of the target table as an alias to the source
		if (!node.columns.empty() || node.default_values) {
			// we are not emitting all columns - set the column set as the set of aliases
			source->column_name_alias = node.columns;

			// now push another subquery that adds the default columns
			auto select_stmt = make_uniq<SelectStatement>();
			auto select_node = make_uniq<SelectNode>();
			identifier_set_t set_columns;
			for (auto &set_col : node.columns) {
				set_columns.insert(set_col);
			}

			for (auto &column : columns.Physical()) {
				auto &name = column.Name();
				unique_ptr<ParsedExpression> expr;
				if (set_columns.find(name) == set_columns.end()) {
					// column is not specified - at the default value
					if (column.HasDefaultValue()) {
						expr = column.DefaultValue().Copy();
					} else {
						expr = ConstantExpression::FromValue(Value(column.Type()));
					}
				} else {
					// column is specified - add a reference to it
					expr = make_uniq<ColumnRefExpression>(name);
				}
				select_node->select_list.push_back(std::move(expr));
			}
			select_node->from_table = std::move(source);
			select_stmt->node = std::move(select_node);

			source = make_uniq<SubqueryRef>(std::move(select_stmt), "excluded");
		}
		// push all columns of the table as an alias
		for (auto &column : columns.Physical()) {
			source->column_name_alias.emplace_back(column.Name());
		}
	}
	// cast the conflict columns to the types of the table, so they are compared as they are inserted
	auto cast_star = make_uniq<StarExpression>();
	for (auto &distinct_on_columns : all_distinct_on_columns) {
		for (auto &col : distinct_on_columns) {
			auto &type = columns.GetColumn(col).Type();
			if (!TypeExpression::CanRepresent(type) || type.HasAlias() || type.id() == LogicalTypeId::ENUM) {
				// a parsed cast resolves these types again, by name or from all their values
				continue;
			}
			unique_ptr<ParsedExpression> key = make_uniq<CastExpression>(type, make_uniq<ColumnRefExpression>(col));
			if (type.id() == LogicalTypeId::VARCHAR) {
				// a cast from VARCHAR keeps the collation of its source
				key = make_uniq<CollateExpression>(StringType::GetCollation(type), std::move(key));
			}
			cast_star->ReplaceListMutable()[col] = std::move(key);
		}
	}
	if (!cast_star->ReplaceList().empty()) {
		auto cast_node = make_uniq<SelectNode>();
		cast_node->select_list.push_back(std::move(cast_star));
		cast_node->from_table = std::move(source);
		auto cast_stmt = make_uniq<SelectStatement>();
		cast_stmt->node = std::move(cast_node);
		source = make_uniq<SubqueryRef>(std::move(cast_stmt), "excluded");
	}

	// push DISTINCT ON(unique_columns)
	for (auto &distinct_on_columns : all_distinct_on_columns) {
		auto distinct_stmt = make_uniq<SelectStatement>();
		auto select_node = make_uniq<SelectNode>();
		auto distinct = make_uniq<DistinctModifier>();
		unique_ptr<ParsedExpression> any_key_null;
		for (auto &col : distinct_on_columns) {
			distinct->distinct_on_targets.push_back(make_uniq<ColumnRefExpression>(col));
			if (IsNotNullColumn(table, col)) {
				continue;
			}
			auto is_null =
			    make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL, make_uniq<ColumnRefExpression>(col));
			if (any_key_null) {
				any_key_null = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(any_key_null),
				                                                std::move(is_null));
			} else {
				any_key_null = std::move(is_null);
			}
		}
		if (any_key_null) {
			// rows with a NULL key column never conflict (NULLs are distinct in unique indexes) - make them distinct
			auto unique_null_key = make_uniq<CaseExpression>();
			CaseCheck null_check;
			null_check.when_expr = std::move(any_key_null);
			null_check.then_expr = make_uniq<FunctionExpression>("uuid", vector<unique_ptr<ParsedExpression>>());
			unique_null_key->CaseChecksMutable().push_back(std::move(null_check));
			unique_null_key->ElseMutable() = ConstantExpression::FromValue(Value());
			distinct->distinct_on_targets.push_back(std::move(unique_null_key));
		}
		select_node->modifiers.push_back(std::move(distinct));
		select_node->select_list.push_back(make_uniq<StarExpression>());
		select_node->from_table = std::move(source);
		distinct_stmt->node = std::move(select_node);
		source = make_uniq<SubqueryRef>(std::move(distinct_stmt), "excluded");
	}

	merge_into->node->source = std::move(source);

	if (on_conflict_info.action_type == OnConflictAction::REPLACE) {
		D_ASSERT(!on_conflict_info.set_info);
		// For BY POSITION, create explicit SET information
		// For BY NAME, leave it empty and let bind_merge_into handle it automatically
		if (node.column_order != InsertColumnOrder::INSERT_BY_NAME) {
			on_conflict_info.set_info = CreateSetInfoForReplace(table, node, storage_info);
		}
		on_conflict_info.action_type = OnConflictAction::UPDATE;
	}
	// now set up the merge actions
	// first set up the base (insert) action when not matched
	auto insert_action = make_uniq<MergeIntoAction>();
	insert_action->action_type = MergeActionType::MERGE_INSERT;
	insert_action->column_order = node.column_order;

	merge_into->node->actions[MergeActionCondition::WHEN_NOT_MATCHED_BY_TARGET].push_back(std::move(insert_action));

	if (on_conflict_info.condition) {
		throw BinderException("ON CONFLICT WHERE clause is only supported in DO UPDATE SET ... WHERE ...\nThe WHERE "
		                      "clause after the conflict columns is used for partial indexes which are not supported.");
	}
	if (on_conflict_info.action_type == OnConflictAction::UPDATE) {
		// when doing UPDATE set up the when matched action
		auto update_action = make_uniq<MergeIntoAction>();
		update_action->exclude_columns = GetConflictColumnNames(storage_info, table);
		update_action->action_type = MergeActionType::MERGE_UPDATE;
		update_action->column_order = node.column_order;
		if (on_conflict_info.set_info) {
			for (auto &col : on_conflict_info.set_info->expressions) {
				vector<identifier_set_t> lambda_params;
				DoUpdateSetQualify(col, table_name, lambda_params);
			}
			if (on_conflict_info.set_info->condition) {
				vector<identifier_set_t> lambda_params;
				DoUpdateSetQualify(on_conflict_info.set_info->condition, table_name, lambda_params);
				update_action->condition = std::move(on_conflict_info.set_info->condition);
			}
			update_action->update_info = std::move(on_conflict_info.set_info);
		}

		merge_into->node->actions[MergeActionCondition::WHEN_MATCHED].push_back(std::move(update_action));
	}

	// move over extra properties
	merge_into->node->returning_list = std::move(node.returning_list);
	return merge_into;
}

BoundStatement Binder::Bind(InsertStatement &stmt) {
	return Bind(*stmt.node);
}

BoundStatement Binder::BindNode(InsertQueryNode &node) {
	BoundStatement result;
	result.names = {"Count"};
	result.types = {LogicalType::BIGINT};

	node.qualified_name = BindTableName(node.qualified_name);
	EntryLookupInfo table_lookup(CatalogType::TABLE_ENTRY, node.qualified_name);
	auto &table_entry = *entry_retriever.GetEntry(table_lookup, OnEntryNotFound::THROW_EXCEPTION);
	if (table_entry.type != CatalogType::TABLE_ENTRY) {
		throw CatalogException("%s is not an %s", node.qualified_name.Name().GetIdentifierName(),
		                       TableCatalogEntry::Name);
	}
	auto &table = table_entry.Cast<TableCatalogEntry>();

	if (auto expanded = TryExpandTriggers(node, table, TriggerEventType::INSERT_EVENT)) {
		return std::move(*expanded);
	}
	if (auto expanded = TryExpandRowTriggers(node, node.returning_list, table, TriggerEventType::INSERT_EVENT)) {
		return std::move(*expanded);
	}

	if (node.column_order == InsertColumnOrder::INSERT_BY_NAME) {
		if (node.GetValuesList()) {
			throw BinderException("INSERT BY NAME can only be used when inserting from a SELECT statement");
		}
		if (node.default_values) {
			throw BinderException("INSERT BY NAME cannot be combined with with DEFAULT VALUES");
		}
		if (!node.columns.empty()) {
			throw BinderException("INSERT BY NAME cannot be combined with an explicit column list");
		}
	}
	if (node.on_conflict_info) {
		// generate a MERGE INTO statement and bind it instead
		auto merge_into = GenerateMergeInto(node, table);
		return Bind(*merge_into);
	}
	GetStatementProperties().RegisterDBModify(table.catalog, context, DatabaseModificationType::INSERT_DATA);

	auto insert = make_uniq<LogicalInsert>(table, GenerateTableIndex());

	auto values_list = node.GetValuesList();

	// bind the root select node (if any)
	BoundStatement root_select;
	if (node.column_order == InsertColumnOrder::INSERT_BY_NAME) {
		D_ASSERT(node.select_statement);
		// INSERT BY NAME - generate the columns from the names of the SELECT statement
		auto select_binder = Binder::CreateBinder(context, this);
		root_select = select_binder->Bind(*node.select_statement);
		MoveCorrelatedExpressions(*select_binder);

		node.columns = root_select.names;
	}

	physical_index_vector_t<idx_t> column_index_map;
	vector<LogicalIndex> named_column_map;
	BindInsertColumnList(table, node.columns, node.default_values, named_column_map, insert->expected_types,
	                     column_index_map);

	// bind the default values
	auto &catalog_name = table.ParentCatalog().GetName();
	BindDefaultValues(table.GetColumns(), insert->bound_defaults, catalog_name.GetIdentifierName(),
	                  table.ParentSchema().GetSchemaPath());
	insert->bound_constraints = BindConstraints(table);
	if (!node.select_statement && !node.default_values) {
		result.plan = std::move(insert);
		return result;
	}
	// Exclude the generated columns from this amount
	idx_t expected_columns = node.columns.empty() ? table.GetColumns().PhysicalColumnCount() : node.columns.size();
	ExpandDefaultInValuesList(node, table, values_list, named_column_map);

	// parse select statement and add to logical plan
	unique_ptr<LogicalOperator> root;
	if (node.select_statement) {
		if (node.column_order == InsertColumnOrder::INSERT_BY_POSITION) {
			auto select_binder = Binder::CreateBinder(context, this);
			root_select = select_binder->Bind(*node.select_statement);
			MoveCorrelatedExpressions(*select_binder);
		}
		// inserting from a select - check if the column count matches
		CheckInsertColumnCountMismatch(expected_columns, root_select.types.size(), !node.columns.empty(), table.name);

		auto source_types = root_select.types;
		auto target_types = insert->expected_types;
		root_select.plan = ResolveInputProjection(*insert, column_index_map, std::move(root_select.plan), source_types);
		target_types.clear();
		for (auto &column : table.GetColumns().Physical()) {
			target_types.push_back(column.Type());
		}
		source_types.clear();
		for (auto &expr : root_select.plan->expressions) {
			source_types.push_back(expr->GetReturnType());
		}
		root = CastLogicalOperatorToTypes(source_types, target_types, std::move(root_select.plan));
	} else {
		root = make_uniq<LogicalDummyScan>(GenerateTableIndex());
		if (node.default_values) {
			root = ResolveInputProjection(*insert, column_index_map, std::move(root), {});
		}
	}

	insert->AddChild(std::move(root));
	if (!node.returning_list.empty()) {
		insert->return_chunk = true;
		auto insert_table_index = GenerateTableIndex();
		insert->table_index = insert_table_index;
		unique_ptr<LogicalOperator> index_as_logicaloperator = std::move(insert);

		return BindReturning(std::move(node.returning_list), table,
		                     node.table_ref ? node.table_ref->alias : Identifier(), insert_table_index,
		                     std::move(index_as_logicaloperator));
	}

	D_ASSERT(result.types.size() == result.names.size());
	result.plan = std::move(insert);

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::CHANGED_ROWS;
	return result;
}

} // namespace duckdb
