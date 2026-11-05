#include "duckdb/planner/statement_preprocessor.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/parser.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/pragma_function_catalog_entry.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/parser/parsed_data/bound_pragma_info.hpp"
#include "duckdb/function/function.hpp"

#include "duckdb/main/client_context.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/function_binder.hpp"

namespace duckdb {

StatementPreprocessor::StatementPreprocessor(ClientContext &context) : context(context) {
}

vector<unique_ptr<SQLStatement>> StatementPreprocessor::TryReparsePragma(unique_ptr<SQLStatement> statement) const {
	// Try reparsing
	const auto info = statement->Cast<PragmaStatement>().info->Copy();
	QueryErrorContext error_context(statement->stmt_location);
	const auto binder = Binder::CreateBinder(context);
	const auto bound_info = binder->BindPragma(*info, error_context);
	if (bound_info->function.query) {
		// Needs reparsing
		FunctionParameters parameters {bound_info->parameters, bound_info->named_parameters, &bound_info->function};
		const auto query_to_reparse = bound_info->function.query(context, parameters);
		Parser parser(context);
		parser.ParseQuery(query_to_reparse);
		return std::move(parser.statements);
	}
	vector<unique_ptr<SQLStatement>> res;
	res.push_back(std::move(statement));
	return res;
}

PreprocessedStatement StatementPreprocessor::ExpandStatement(unique_ptr<SQLStatement> statement) const {
	PreprocessedStatement result;
	switch (statement->type) {
	case StatementType::MULTI_STATEMENT: {
		auto &multi_statement = statement->Cast<MultiStatement>();
		result.statements = std::move(multi_statement.statements);
		break;
	}
	case StatementType::PRAGMA_STATEMENT:
		result.statements = TryReparsePragma(std::move(statement));
		break;
	default:
		result.statements.push_back(std::move(statement));
		return result;
	}
	if (result.statements.size() > 1) {
		for (auto &expanded : result.statements) {
			// The statements run as one query in one transaction, which a transaction statement would end or take over
			if (expanded->type == StatementType::TRANSACTION_STATEMENT) {
				throw InvalidInputException(
				    "A statement that expands into several cannot contain a transaction statement: %s",
				    expanded->query);
			}
		}
	}
	return result;
}

PreprocessedStatement StatementPreprocessor::PreprocessStatement(ClientContextLock &lock,
                                                                 unique_ptr<SQLStatement> statement) {
	if (statement->type != StatementType::PRAGMA_STATEMENT && statement->type != StatementType::MULTI_STATEMENT) {
		return ExpandStatement(std::move(statement));
	}
	// Only PRAGMA and multi-statements touch the catalog and need a transaction
	PreprocessedStatement result;
	context.RunFunctionInTransactionInternal(lock, [&] { result = ExpandStatement(std::move(statement)); });
	return result;
}

} // namespace duckdb
