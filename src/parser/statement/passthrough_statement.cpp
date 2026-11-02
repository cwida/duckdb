#include "duckdb/parser/statement/passthrough_statement.hpp"

namespace duckdb {

PassthroughStatement::PassthroughStatement() : SQLStatement(StatementType::PASSTHROUGH_STATEMENT) {
}

unique_ptr<SQLStatement> PassthroughStatement::Copy() const {
	return unique_ptr<PassthroughStatement>(new PassthroughStatement(*this));
}

string PassthroughStatement::ToString() const {
	return query;
}

} // namespace duckdb
