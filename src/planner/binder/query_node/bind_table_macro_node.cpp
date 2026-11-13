#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/function/table_macro_function.hpp"

namespace duckdb {

static void MarkExpandedTableMacros(QueryNode &node, const reference_set_t<CatalogEntry> &expanded_table_macros);

static void MarkExpandedTableMacros(unique_ptr<ParsedExpression> &expr,
                                    const reference_set_t<CatalogEntry> &expanded_table_macros) {
	if (expr->GetExpressionClass() == ExpressionClass::SUBQUERY) {
		MarkExpandedTableMacros(*expr->Cast<SubqueryExpression>().Subquery()->node, expanded_table_macros);
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { MarkExpandedTableMacros(child, expanded_table_macros); });
}

//! Records on every table function reference in a macro body which table macros were expanded to produce it
static void MarkExpandedTableMacros(QueryNode &node, const reference_set_t<CatalogEntry> &expanded_table_macros) {
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    node, [&](unique_ptr<ParsedExpression> &child) { MarkExpandedTableMacros(child, expanded_table_macros); },
	    [&](TableRef &ref) {
		    if (ref.type == TableReferenceType::TABLE_FUNCTION) {
			    ref.Cast<TableFunctionRef>().expanded_table_macros = expanded_table_macros;
		    }
	    });
}

unique_ptr<QueryNode> Binder::BindTableMacro(FunctionExpression &function, TableMacroCatalogEntry &macro_func,
                                             const reference_set_t<CatalogEntry> &expanded_table_macros, idx_t depth) {
	// expanding nested macros can take a long time, allow the query to be interrupted
	context.InterruptCheck();
	// validate the arguments and separate positional and default arguments
	vector<unique_ptr<ParsedExpression>> positional_arguments;
	InsertionOrderPreservingMap<unique_ptr<ParsedExpression>, Identifier, identifier_map_t<idx_t>> named_arguments;

	auto bind_result = MacroFunction::BindMacroFunction(*this, macro_func.macros, macro_func.name, function,
	                                                    positional_arguments, named_arguments, depth);
	if (!bind_result.error.empty()) {
		throw BinderException(function, bind_result.error);
	}
	auto &macro_def = *macro_func.macros[bind_result.function_idx.GetIndex()];

	auto new_macro_binding =
	    MacroFunction::CreateDummyBinding(macro_def, macro_func.name, positional_arguments, named_arguments);
	new_macro_binding->arguments = &positional_arguments;

	// We need an ExpressionBinder so that we can call ExpressionBinder::ReplaceMacroParametersRecursive()
	auto eb = ExpressionBinder(*this, this->context);

	eb.macro_binding = new_macro_binding.get();
	vector<identifier_set_t> lambda_params;

	auto node = macro_def.Cast<TableMacroFunction>().query_node->Copy();
	// mark the body before the parameters are replaced, so references inside the arguments are not marked
	MarkExpandedTableMacros(*node, expanded_table_macros);
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    *node, [&](unique_ptr<ParsedExpression> &child) { eb.ReplaceMacroParameters(child, lambda_params); });

	return node;
}

} // namespace duckdb
