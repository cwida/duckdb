#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/planner/operator/logical_pragma.hpp"
#include "duckdb/catalog/catalog_entry/pragma_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

unique_ptr<BoundPragmaInfo> Binder::BindPragma(PragmaInfo &info, QueryErrorContext error_context) {
	// the arguments are bound here and folded to constants only after the overload has been chosen, so that a
	// literal keeps its literal type for overload selection - as for every other function kind
	vector<unique_ptr<Expression>> positional_arguments;
	vector<pair<Identifier, unique_ptr<Expression>>> named_arguments;

	ConstantBinder pragma_binder(*this, context, "PRAGMA value");
	for (auto &param : info.parameters) {
		positional_arguments.push_back(pragma_binder.Bind(param));
	}
	for (auto &entry : info.named_parameters) {
		named_arguments.emplace_back(Identifier(entry.first), pragma_binder.Bind(entry.second));
	}

	// bind the pragma function
	auto entry = Catalog::GetEntry<PragmaFunctionCatalogEntry>(
	    context, QualifiedName(Identifier::InvalidCatalog(), Identifier::DefaultSchema(), info.name),
	    OnEntryNotFound::RETURN_NULL);
	if (!entry) {
		// try to find whether a table entry might exist
		auto table_entry = Catalog::GetEntry<TableFunctionCatalogEntry>(
		    context, QualifiedName(Identifier::InvalidCatalog(), Identifier::DefaultSchema(), info.name),
		    OnEntryNotFound::RETURN_NULL);
		if (table_entry) {
			// there is a table entry with the same name, now throw more explicit error message
			throw CatalogException("Pragma Function with name %s does not exist, but a table function with the same "
			                       "name exists, try `CALL %s(...)`",
			                       info.name, info.name);
		}
		// rebind to throw exception
		entry = Catalog::GetEntry<PragmaFunctionCatalogEntry>(
		    context, QualifiedName(Identifier::InvalidCatalog(), Identifier::DefaultSchema(), info.name),
		    OnEntryNotFound::THROW_EXCEPTION);
	}

	// selection, folding, casting and named-argument checking all happen in the function binder
	FunctionBinder function_binder(*this);
	ErrorData error;
	auto result = function_binder.BindPragmaFunction(entry->functions, positional_arguments, named_arguments, error);
	if (!result) {
		error.AddQueryLocation(error_context);
		error.Throw();
	}
	return result;
}

BoundStatement Binder::Bind(PragmaStatement &stmt) {
	// bind the pragma function
	QueryErrorContext error_context(stmt.stmt_location);
	auto bound_info = BindPragma(*stmt.info, error_context);
	if (!bound_info->function.function) {
		throw BinderException("PRAGMA function does not have a function specified");
	}

	BoundStatement result;
	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};
	result.plan = make_uniq<LogicalPragma>(std::move(bound_info));

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::QUERY_RESULT;
	return result;
}

} // namespace duckdb
