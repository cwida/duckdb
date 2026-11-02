#include "duckdb/parser/parser.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/table/range.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

namespace duckdb {

static unique_ptr<SubqueryRef> ParseSubquery(const string &query, ClientContext &context, const string &err_msg) {
	Parser parser(context);
	parser.ParseQuery(query);
	if (parser.statements.size() != 1) {
		throw ParserException(err_msg);
	}

	auto &stmt = parser.statements[0];

	if (stmt->type == StatementType::SELECT_STATEMENT) {
		// Regular SELECT statement
		auto select_stmt = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(stmt));
		return duckdb::make_uniq<SubqueryRef>(std::move(select_stmt));
	} else if (stmt->type == StatementType::MULTI_STATEMENT) {
		// MultiStatement (e.g., from PIVOT statements that create enum types)
		throw ParserException(
		    "PIVOT statements without explicit IN clauses are not supported in query() function. "
		    "Please specify the pivot values explicitly, e.g.: PIVOT ... ON col IN (val1, val2, ...)");
	} else {
		throw ParserException(err_msg);
	}
}

//! Whether the argument is a file path or URL rather than a (qualified) table name: a path separator outside of quotes
static bool IsFilePath(const string &input) {
	bool quoted = false;
	for (auto c : input) {
		if (c == '"') {
			quoted = !quoted;
		} else if (!quoted && (c == '/' || c == '\\')) {
			return true;
		}
	}
	return false;
}

//! Renders the argument as a table reference: file paths are passed as string literals, like FROM 'file.parquet'
static string TableReference(const string &input) {
	if (IsFilePath(input)) {
		return SQLString::ToString(input);
	}
	QualifiedName qname;
	string error;
	if (!QualifiedName::TryParse(input, qname, error)) {
		throw ParserException(error);
	}
	return qname.ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
}

static string UnionTablesQuery(TableFunctionBindInput &input) {
	for (auto &input_val : input.inputs) {
		if (input_val.IsNull()) {
			throw BinderException("Cannot use NULL as function argument");
		}
	}
	string result;
	string by_name = (input.inputs.size() == 2 &&
	                  (input.inputs[1].type().id() == LogicalTypeId::BOOLEAN && input.inputs[1].GetValue<bool>()))
	                     ? "BY NAME "
	                     : ""; // 'by_name' variable defaults to false
	if (input.inputs[0].type().id() == LogicalTypeId::VARCHAR) {
		result += "FROM " + TableReference(input.inputs[0].ToString());
	} else if (input.inputs[0].type() == LogicalType::LIST(LogicalType::VARCHAR)) {
		string union_all_clause = " UNION ALL " + by_name + "FROM ";
		const auto &children = ListValue::GetChildren(input.inputs[0]);

		if (children.empty()) {
			throw InvalidInputException("Input list is empty");
		}
		for (auto &child : children) {
			if (child.IsNull()) {
				throw BinderException("Cannot use NULL as function argument");
			}
		}
		result += "FROM " + TableReference(children[0].ToString());
		for (size_t i = 1; i < children.size(); ++i) {
			result += union_all_clause + TableReference(children[i].ToString());
		}
	} else {
		throw InvalidInputException("Expected a table or a list with tables as input");
	}
	return result;
}

static unique_ptr<TableRef> QueryBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("Cannot use NULL as function argument");
	}
	auto query = input.inputs[0].ToString();
	auto subquery_ref = ParseSubquery(query, context, "Expected a single SELECT statement");
	return std::move(subquery_ref);
}

static unique_ptr<TableRef> TableBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto query = UnionTablesQuery(input);
	auto subquery_ref = ParseSubquery(query, context, "Expected a table or a list with tables as input");
	return std::move(subquery_ref);
}

void QueryTableFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunction query("query", FunctionSignature().AddPositionalOnly("query", LogicalType::VARCHAR), nullptr,
	                    nullptr);
	query.bind_replace = QueryBindReplace;
	set.AddFunction(query);

	TableFunctionSet query_table("query_table");
	TableFunction query_table_function(FunctionSignature().AddPositionalOnly("table_name", LogicalType::VARCHAR),
	                                   nullptr, nullptr);
	query_table_function.bind_replace = TableBindReplace;
	query_table.AddFunction(query_table_function);

	query_table_function.GetSignature() =
	    FunctionSignature().AddPositionalOnly("table_names", LogicalType::LIST(LogicalType::VARCHAR));
	query_table.AddFunction(query_table_function);
	// add by_name option, for a single table name as well as for a list of them
	query_table_function.GetSignature().AddPositionalOnly("by_name", LogicalType::BOOLEAN);
	query_table.AddFunction(query_table_function);
	query_table_function.GetSignature() = FunctionSignature()
	                                          .AddPositionalOnly("table_name", LogicalType::VARCHAR)
	                                          .AddPositionalOnly("by_name", LogicalType::BOOLEAN);
	query_table.AddFunction(query_table_function);
	set.AddFunction(query_table);
}

} // namespace duckdb
