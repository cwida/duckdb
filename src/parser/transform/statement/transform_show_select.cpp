#include "duckdb/parser/statement/show_statement.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

using namespace duckdb_libpgquery;

unique_ptr<ShowStatement> Transformer::TransformShowSelect(PGNode *node) {
	// we capture the select statement of SHOW
	auto stmt = reinterpret_cast<PGVariableShowSelectStmt *>(node);
	auto select_stmt = reinterpret_cast<PGSelectStmt *>(stmt->stmt);

	auto result = make_unique<ShowStatement>();
	auto &info = *result->info;

	info.query = TransformSelectNode(select_stmt);

	return result;
}

} // namespace duckdb
