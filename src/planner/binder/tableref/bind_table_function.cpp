#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/table_function_binder.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/window/rows_functions.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

enum class TableFunctionBindType { STANDARD_TABLE_FUNCTION, TABLE_IN_OUT_FUNCTION, TABLE_PARAMETER_FUNCTION };

static TableFunctionBindType GetTableFunctionBindType(TableFunctionCatalogEntry &table_function,
                                                      vector<unique_ptr<ParsedExpression>> &expressions) {
	// first check if all expressions are scalar
	// if they are we always bind as a standard table function
	bool all_scalar = true;
	for (auto &expr : expressions) {
		if (!expr->IsScalar()) {
			all_scalar = false;
			break;
		}
	}
	if (all_scalar) {
		for (idx_t function_idx = 0; function_idx < table_function.functions.Size(); function_idx++) {
			const auto &function = *table_function.functions.GetFunctionByOffset(function_idx);
			for (auto &param : function.GetSignature().GetParameters()) {
				if (param.GetType().id() == LogicalTypeId::TABLE) {
					throw BinderException("Table function %s requires a subquery as its TABLE parameter",
					                      table_function.name);
				}
			}
		}
		return TableFunctionBindType::STANDARD_TABLE_FUNCTION;
	}
	// if we have non-scalar parameters - we need to look at the function definition to decide how to bind
	// if a function does not have an in_out_function defined, we need to bind as a standard table function regardless
	bool has_in_out_function = false;
	bool has_standard_table_function = false;
	bool has_table_parameter = false;
	for (idx_t function_idx = 0; function_idx < table_function.functions.Size(); function_idx++) {
		const auto &function = *table_function.functions.GetFunctionByOffset(function_idx);
		for (auto &param : function.GetSignature().GetParameters()) {
			if (param.GetType().id() == LogicalTypeId::TABLE) {
				has_table_parameter = true;
			}
		}
		if (function.in_out_function) {
			has_in_out_function = true;
		} else if (function.function || function.bind_replace || function.bind_operator) {
			has_standard_table_function = true;
		} else {
			throw InternalException("Function %s has neither in_out_function nor function defined",
			                        table_function.name);
		}
	}
	if (has_table_parameter) {
		if (table_function.functions.Size() != 1) {
			throw InternalException(
			    "Function %s has a TABLE parameter, and multiple function overloads - this is not supported",
			    table_function.name);
		}
		return TableFunctionBindType::TABLE_PARAMETER_FUNCTION;
	}
	if (has_in_out_function && has_standard_table_function) {
		throw InternalException("Function %s is both an in_out_function and a table function", table_function.name);
	}
	return has_in_out_function ? TableFunctionBindType::TABLE_IN_OUT_FUNCTION
	                           : TableFunctionBindType::STANDARD_TABLE_FUNCTION;
}

void Binder::BindTableInTableOutFunction(vector<unique_ptr<ParsedExpression>> &expressions, BoundStatement &subquery) {
	auto binder = Binder::CreateBinder(this->context, this);
	// generate a subquery and bind that (i.e. UNNEST([1,2,3]) becomes UNNEST((SELECT [1,2,3]))
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list = std::move(expressions);
	select_node->from_table = make_uniq<EmptyTableRef>();
	binder->SetCanContainNulls(true);
	subquery = binder->BindNode(*select_node);
	MoveCorrelatedExpressions(*binder);
}

bool Binder::BindTableFunctionParameters(TableFunctionCatalogEntry &table_function,
                                         vector<unique_ptr<ParsedExpression>> &expressions,
                                         vector<unique_ptr<Expression>> &positional_arguments,
                                         vector<pair<Identifier, unique_ptr<Expression>>> &named_arguments,
                                         BoundStatement &subquery, bool &table_in_out, ErrorData &error) {
	auto bind_type = GetTableFunctionBindType(table_function, expressions);
	if (bind_type == TableFunctionBindType::TABLE_IN_OUT_FUNCTION) {
		// bind table in-out function - its arguments are the columns of the subquery, so there is nothing to fold
		BindTableInTableOutFunction(expressions, subquery);
		table_in_out = true;
		return true;
	}
	bool seen_subquery = false;
	for (auto &child : expressions) {
		Identifier parameter_name;

		// hack to make named parameters work
		if (child->GetExpressionType() == ExpressionType::COMPARE_EQUAL) {
			// comparison, check if the LHS is a columnref
			auto &comp = child->Cast<ComparisonExpression>();
			if (comp.Left().GetExpressionType() == ExpressionType::COLUMN_REF) {
				auto &colref = comp.Left().Cast<ColumnRefExpression>();
				if (!colref.IsQualified()) {
					parameter_name = colref.GetColumnName();
					child = std::move(comp.RightMutable());
				}
			}
		} else if (!child->GetAlias().empty()) {
			// <name> => <expression> will set the alias of <expression> to <name>
			parameter_name = child->GetAlias();
		}
		if (bind_type == TableFunctionBindType::TABLE_PARAMETER_FUNCTION &&
		    child->GetExpressionType() == ExpressionType::SUBQUERY) {
			D_ASSERT(table_function.functions.Size() == 1);
			const auto &fun = *table_function.functions.GetFunctionByOffset(0);
			if (table_function.functions.Size() != 1 || fun.GetSignature().GetPositionalParameterCount() == 0) {
				throw BinderException(
				    "Only table-in-out functions can have subquery parameters - %s only accepts constant parameters",
				    fun.GetName());
			}
			if (seen_subquery) {
				error = ErrorData("Table function can have at most one subquery parameter");
				return false;
			}
			auto binder = Binder::CreateBinder(this->context, this);
			binder->SetCanContainNulls(true);
			auto &se = child->Cast<SubqueryExpression>();
			subquery = binder->BindNode(*se.Subquery()->node);
			MoveCorrelatedExpressions(*binder);
			seen_subquery = true;
			positional_arguments.push_back(make_uniq<BoundConstantExpression>(Value(LogicalType::TABLE)));
			continue;
		}

		TableFunctionBinder binder(*this, context, table_function.name.GetIdentifierName());
		auto expr = binder.Bind(child);
		if (expr->HasParameter()) {
			throw ParameterNotResolvedException();
		}
		if (!expr->IsScalar()) {
			// should have been eliminated before
			throw InternalException("Table function requires a constant parameter");
		}
		// The argument is folded to a constant only after the overload has been chosen, so that the bound
		// expression's STRING_LITERAL / INTEGER_LITERAL type reaches overload selection - which is what lets
		// "header = 1" match a BOOLEAN option, exactly as it already does for scalar functions
		if (parameter_name.empty()) {
			// unnamed parameter
			if (!named_arguments.empty()) {
				error = ErrorData("Unnamed parameters cannot come after named parameters");
				return false;
			}
			positional_arguments.push_back(std::move(expr));
		} else {
			named_arguments.emplace_back(parameter_name, std::move(expr));
		}
	}
	return true;
}

static string GetAlias(const TableFunctionRef &ref) {
	if (!ref.alias.empty()) {
		return ref.alias.GetIdentifierName();
	}
	if (ref.function && ref.function->GetExpressionType() == ExpressionType::FUNCTION) {
		auto &function_expr = ref.function->Cast<FunctionExpression>();
		return function_expr.FunctionName().GetIdentifierName();
	}
	return string();
}

//! Postgres names the single column of a set-returning function after the alias of the function
//! (e.g. "SELECT t FROM generate_series(1, 2) t" returns the values, not a struct)
//! returns the original column name, which is kept available as a column alias
static Identifier ApplyPostgresSetofAliasCompatibility(const BaseTableFunction &table_function,
                                                       const TableFunctionRef &ref, vector<Identifier> &return_names) {
	if (table_function.return_type != TableFunctionReturnType::SET_RETURNING_FUNCTION || ref.alias.empty() ||
	    !ref.column_name_alias.empty() || return_names.size() != 1) {
		return Identifier();
	}
	auto original_name = return_names[0];
	return_names[0] = ref.alias;
	return original_name;
}

//! Keep the original column name of a set-returning function bindable, e.g. "SELECT t.generate_series FROM
//! generate_series(1, 2) t" - the original name is hidden from * since the column is emitted under the alias
static void AddPostgresSetofColumnAlias(BindContext &bind_context, TableIndex bind_index,
                                        const Identifier &original_name) {
	if (original_name.empty()) {
		return;
	}
	bind_context.AddColumnAlias(bind_index, original_name, 0);
}

static bool PlanSupportsStatementCache(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op.Cast<LogicalGet>();
		if (get.bind_data && !get.bind_data->SupportStatementCache()) {
			return false;
		}
	}
	for (auto &child : op.children) {
		if (!PlanSupportsStatementCache(*child)) {
			return false;
		}
	}
	return true;
}

BoundStatement Binder::BindTableFunctionInternal(BoundTableFunction &table_function, const TableFunctionRef &ref,
                                                 vector<Value> parameters, named_argument_map_t named_parameters,
                                                 vector<LogicalType> input_table_types,
                                                 vector<Identifier> input_table_names,
                                                 optional_ptr<unique_ptr<LogicalOperator>> input_plan) {
	auto function_name = GetAlias(ref);
	auto &column_name_alias = ref.column_name_alias;
	auto bind_index = GenerateTableIndex();
	// perform the binding
	unique_ptr<FunctionData> bind_data;
	vector<LogicalType> return_types;
	vector<Identifier> return_names;
	auto constexpr ordinality_name = "ordinality";
	string ordinality_column_name = ordinality_name;
	optional_idx ordinality_column_id;
	// the function binder has placed them already, but not for a table in-out call or a direct bind
	table_function.GetSignature().FillNamedDefaults(context, named_parameters);
	table_function.SetCallArguments(parameters, named_parameters);
	if (table_function.bind || table_function.bind_replace || table_function.bind_operator) {
		TableFunctionBindInput bind_input(parameters, named_parameters, input_table_types, input_table_names,
		                                  table_function.function_info.get(), this, table_function, ref, input_plan);
		if (table_function.bind_operator) {
			auto new_plan = table_function.bind_operator(context, bind_input, bind_index, return_names);
			if (new_plan) {
				if (!PlanSupportsStatementCache(*new_plan)) {
					SetAlwaysRequireRebind();
				}
				new_plan->ResolveOperatorTypes();
				if (new_plan->types.size() != return_names.size()) {
					throw InternalException("Failed to bind %s: return_types/names must have same size",
					                        table_function.GetName());
				}
				for (auto &binding : new_plan->GetColumnBindings()) {
					if (binding.table_index != bind_index) {
						throw InternalException("Failed to bind %s: root bind index must be the passed in bind index",
						                        table_function.GetName());
					}
				}
				auto setof_column_name = ApplyPostgresSetofAliasCompatibility(table_function, ref, return_names);
				BoundStatement result;
				bind_context.AddGenericBinding(bind_index, Identifier(function_name), return_names, new_plan->types);
				AddPostgresSetofColumnAlias(bind_context, bind_index, setof_column_name);
				result.names = return_names;
				result.types = new_plan->types;
				result.plan = std::move(new_plan);
				return result;
			}
		}
		if (table_function.bind_replace) {
			auto new_plan = table_function.bind_replace(context, bind_input);
			if (new_plan) {
				if (!ref.alias.empty()) {
					new_plan->alias = ref.alias;
				}
				if (!ref.column_name_alias.empty()) {
					new_plan->column_name_alias = ref.column_name_alias;
				}
				return Bind(*new_plan);
			}
		}
		if (!table_function.bind) {
			throw BinderException("Failed to bind %s: nullptr returned from bind_replace without bind function",
			                      table_function.GetName());
		}
		bind_data = table_function.bind(context, bind_input, return_types, return_names);
		if (ref.with_ordinality == OrdinalityType::WITH_ORDINALITY) {
			// check if column name 'ordinality' already exists and if so, replace it iteratively until free name is
			// found
			identifier_set_t ci_return_names;
			idx_t ordinality_name_suffix = 0;
			for (auto &n : return_names) {
				ci_return_names.insert(n);
			}
			for (auto &n : column_name_alias) {
				ci_return_names.insert(n);
			}
			while (ci_return_names.find(Identifier(ordinality_column_name)) != ci_return_names.end()) {
				ordinality_column_name = ordinality_name + to_string(ordinality_name_suffix++);
			}
			if (!correlated_columns.empty()) {
				return_types.emplace_back(LogicalType::BIGINT);
				return_names.emplace_back(ordinality_column_name);
				D_ASSERT(return_names.size() == return_types.size());
				ordinality_column_id = return_types.size() - 1;
			}
		}
	} else {
		throw InvalidInputException("Cannot call function %s directly - it has no bind function",
		                            table_function.GetName());
	}
	if (bind_data && !bind_data->SupportStatementCache()) {
		SetAlwaysRequireRebind();
	}
	if (return_types.size() != return_names.size()) {
		throw InternalException("Failed to bind %s: return_types/names must have same size", table_function.GetName());
	}
	if (return_types.empty()) {
		throw InternalException("Failed to bind %s: Table function must return at least one column",
		                        table_function.GetName());
	}
	auto setof_column_name = ApplyPostgresSetofAliasCompatibility(table_function, ref, return_names);
	// overwrite the names with any supplied aliases
	for (idx_t i = 0; i < column_name_alias.size() && i < return_names.size(); i++) {
		return_names[i] = column_name_alias[i];
	}
	for (idx_t i = 0; i < return_names.size(); i++) {
		if (return_names[i].empty()) {
			return_names[i] = Identifier("C" + to_string(i));
		}
	}
	virtual_column_map_t virtual_columns;
	if (table_function.get_virtual_columns) {
		virtual_columns = table_function.get_virtual_columns(context, bind_data.get());
	}

	auto get = make_uniq<LogicalGet>(bind_index, table_function, std::move(bind_data), return_types, return_names,
	                                 virtual_columns);
	get->bind_info = ref.bind_info;
	get->parameters = parameters;
	get->named_parameters = named_parameters;
	get->source_ordinality = ref.with_ordinality;
	get->input_table_types = input_table_types;
	get->input_table_names = input_table_names;
	if (ref.with_ordinality == OrdinalityType::WITH_ORDINALITY && !correlated_columns.empty()) {
		get->ordinality_idx = ordinality_column_id;
	}
	if (table_function.in_out_function) {
		for (idx_t i = 0; i < return_types.size(); i++) {
			get->AddColumnId(i);
		}
	}

	if (ref.with_ordinality == OrdinalityType::WITH_ORDINALITY && correlated_columns.empty()) {
		bind_context.AddTableFunction(bind_index, Identifier(function_name), return_names, return_types,
		                              get->GetMutableColumnIds(), get->GetTable().get(), std::move(virtual_columns));
		AddPostgresSetofColumnAlias(bind_context, bind_index, setof_column_name);

		auto window_index = GenerateTableIndex();
		auto window = make_uniq<duckdb::LogicalWindow>(window_index);
		auto row_number = GetBuiltinWindowFunction(context, RowNumberFun::Name, {})->Bind(context);
		row_number->WindowStartMutable() = WindowBoundary::UNBOUNDED_PRECEDING;
		row_number->WindowEndMutable() = WindowBoundary::CURRENT_ROW_ROWS;
		Identifier ordinality_alias(ordinality_column_name);
		if (return_names.size() < column_name_alias.size()) {
			row_number->SetAlias(column_name_alias[return_names.size()]);
			ordinality_alias = column_name_alias[return_names.size()];
		} else {
			row_number->SetAlias(Identifier(ordinality_column_name));
		}
		return_names.push_back(ordinality_alias);
		return_types.push_back(LogicalType::BIGINT);
		window->expressions.push_back(std::move(row_number));
		window->types.push_back(LogicalType::BIGINT);
		window->children.push_back(std::move(get));
		bind_context.AddGenericBinding(window_index, Identifier(function_name), {ordinality_alias},
		                               {LogicalType::BIGINT});

		BoundStatement result;
		result.names = std::move(return_names);
		result.types = std::move(return_types);
		result.plan = std::move(window);
		return result;
	}
	// now add the table function to the bind context so its columns can be bound
	BoundStatement result;
	bind_context.AddTableFunction(bind_index, Identifier(function_name), return_names, return_types,
	                              get->GetMutableColumnIds(), get->GetTable().get(), std::move(virtual_columns));
	AddPostgresSetofColumnAlias(bind_context, bind_index, setof_column_name);
	result.names = std::move(return_names);
	result.types = std::move(return_types);
	result.plan = std::move(get);
	return result;
}

BoundStatement Binder::BindTableFunction(TableFunction &function, vector<Value> parameters) {
	named_argument_map_t named_parameters;
	vector<LogicalType> input_table_types;
	vector<Identifier> input_table_names;

	TableFunctionRef ref;
	ref.alias = function.name;
	D_ASSERT(!ref.alias.empty());
	BoundTableFunction bound_function(function);
	return BindTableFunctionInternal(bound_function, ref, std::move(parameters), std::move(named_parameters),
	                                 std::move(input_table_types), std::move(input_table_names), nullptr);
}

BoundStatement Binder::Bind(TableFunctionRef &ref) {
	QueryErrorContext error_context(ref.query_location);

	D_ASSERT(ref.function->GetExpressionType() == ExpressionType::FUNCTION);
	auto &fexpr = ref.function->Cast<FunctionExpression>();

	// fetch the function from the catalog. Resolve the qualification first: a leading component is the catalog when
	// it names an attached database, and otherwise the outermost schema of a (possibly nested) schema path.
	EntryLookupInfo table_function_lookup(CatalogType::TABLE_FUNCTION_ENTRY, QualifiedName(fexpr.FunctionName()),
	                                      error_context);
	auto bound_name = BindTableName(entry_retriever, fexpr.GetQualifiedName());
	auto &func_catalog =
	    *GetCatalogEntry(EntryLookupInfo(table_function_lookup, bound_name), OnEntryNotFound::THROW_EXCEPTION);

	if (func_catalog.type == CatalogType::TABLE_MACRO_ENTRY) {
		auto &macro_func = func_catalog.Cast<TableMacroCatalogEntry>();
		if (!func_catalog.ParentCatalog().IsSystemCatalog()) {
			GetStatementProperties().RegisterDBRead(func_catalog.ParentCatalog(), context);
		}
		auto query_node = BindTableMacro(fexpr, macro_func, 0);
		D_ASSERT(query_node);

		auto binder = Binder::CreateBinder(context, this);
		binder->SetCanContainNulls(true);
		binder->AddBoundTableMacro(macro_func);

		binder->alias = ref.alias.empty() ? "unnamed_query" : ref.alias;
		BoundStatement query;
		try {
			query = binder->BindNode(*query_node);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			error.AddQueryLocation(ref);
			error.Throw();
		}

		auto bind_index = query.plan->GetRootIndex();
		// string alias;
		string alias =
		    (ref.alias.empty() ? "unnamed_query" + to_string(bind_index.index) : ref.alias.GetIdentifierName());

		// remember ref here is TableFunctionRef and NOT base class
		bind_context.AddSubquery(bind_index, Identifier(alias), ref, query);
		MoveCorrelatedExpressions(*binder);
		return query;
	}
	D_ASSERT(func_catalog.type == CatalogType::TABLE_FUNCTION_ENTRY);
	auto &function = func_catalog.Cast<TableFunctionCatalogEntry>();

	// bind the input parameters to the function - they are folded to constants after the overload is chosen
	vector<unique_ptr<Expression>> positional_arguments;
	vector<pair<Identifier, unique_ptr<Expression>>> named_arguments;
	BoundStatement subquery;
	bool table_in_out = false;
	ErrorData error;

	vector<unique_ptr<ParsedExpression>> children;
	for (auto &child : fexpr.GetArgumentsMutable()) {
		children.push_back(std::move(child.GetExpressionMutable()));
	}

	if (!BindTableFunctionParameters(function, children, positional_arguments, named_arguments, subquery, table_in_out,
	                                 error)) {
		error.AddQueryLocation(ref);
		error.Throw();
	}

	// selection, named-argument checking, folding and casting all happen in the function binder
	FunctionBinder function_binder(*this);
	vector<Value> parameters;
	named_argument_map_t named_parameters;
	auto best_function_idx =
	    table_in_out ? function_binder.BindTableInOutFunction(function.name, function.functions, subquery.types, error)
	                 : function_binder.BindFunction(function.name, function.functions, positional_arguments,
	                                                named_arguments, parameters, named_parameters, error);
	if (!best_function_idx.IsValid()) {
		error.AddQueryLocation(ref);
		error.Throw();
	}
	// copied out of the set: BindTableFunctionInternal fills in the bound return types
	BoundTableFunction table_function(function.functions.GetFunctionByOffset(best_function_idx.GetIndex()));

	vector<LogicalType> input_table_types;
	vector<Identifier> input_table_names;

	if (subquery.plan) {
		input_table_types = subquery.types;
		input_table_names = subquery.names;
	} else if (table_function.in_out_function) {
		for (auto &param : parameters) {
			input_table_types.push_back(param.type());
			input_table_names.push_back(Identifier());
		}
	}
	if (parameters.empty() && subquery.plan) {
		// the arguments are the columns of a subquery, so the declared types apply to those instead - the function
		// binder casts the folded constants of every other call shape
		const auto &signature = table_function.GetSignature();
		auto args = signature.GetArgs();
		for (idx_t i = 0; i < input_table_types.size(); i++) {
			if (i >= signature.GetPositionalParameterCount() && !args) {
				break;
			}
			auto target_type =
			    i < signature.GetPositionalParameterCount() ? signature.GetParameter(i).GetType() : args->GetType();
			if (target_type != LogicalType::ANY && target_type != LogicalType::POINTER &&
			    target_type.id() != LogicalTypeId::LIST) {
				input_table_types[i] = target_type;
			}
		}
	}

	BoundStatement get;
	try {
		get = BindTableFunctionInternal(table_function, ref, std::move(parameters), std::move(named_parameters),
		                                std::move(input_table_types), std::move(input_table_names), &subquery.plan);
	} catch (std::exception &ex) {
		error = ErrorData(ex);
		// if the error does not already contain a query location, add one
		if (error.ExtraInfo().count("position") == 0) {
			error.AddQueryLocation(ref);
		}
		error.Throw();
	}

	if (subquery.plan) {
		auto child_node = std::move(subquery.plan);

		reference<LogicalOperator> node = *get.plan;

		while (!node.get().children.empty()) {
			D_ASSERT(node.get().children.size() == 1);
			if (node.get().children.size() != 1) {
				throw InternalException(
				    "Binder::CreatePlan<BoundTableFunction>: linear path expected, but found node with %d children",
				    node.get().children.size());
			}
			node = *node.get().children[0];
		}

		D_ASSERT(node.get().type == LogicalOperatorType::LOGICAL_GET);
		node.get().children.push_back(std::move(child_node));
	}
	BoundStatement result_statement;
	result_statement.names = get.names;
	result_statement.types = get.types;
	result_statement.plan = std::move(get.plan);
	return result_statement;
}

} // namespace duckdb
