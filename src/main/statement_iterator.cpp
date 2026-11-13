#include "duckdb/main/statement_iterator.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"

namespace duckdb {

static bool StatementIteratorIsExplainAnalyze(SQLStatement *statement) {
	if (!statement || statement->type != StatementType::EXPLAIN_STATEMENT) {
		return false;
	}
	return statement->Cast<ExplainStatement>().explain_type == ExplainType::EXPLAIN_ANALYZE;
}

StatementIterator::StatementIterator(ParseIterator &&parse_iterator)
    : source(std::move(parse_iterator)), context(source.GetClientContext()) {
}

StatementIterator::~StatementIterator() = default;

StatementIterator::StatementIterator(StatementIterator &&) noexcept = default;

bool StatementIterator::Peek() {
	if (pending_statement) {
		return true;
	}
	parser_start_time = Timestamp::GetCurrentTimestamp();
	parser_timer.Start();
	if (!source.Peek()) {
		parser_timer.Reset();
		return false;
	}
	parser_timer.End();
	parser_end_time = Timestamp::GetCurrentTimestamp();
	pending_statement = source.GetStatement();
	if (!pending_statement) {
		parser_timer.Reset();
		return false;
	}
	return true;
}

bool StatementIterator::HasMore() {
	if (pending_statement) {
		return true;
	}
	return source.HasMore();
}

unique_ptr<SQLStatement> StatementIterator::GetStatementInternal(bool profile_statement) {
	if (!pending_statement && !Peek()) {
		return nullptr;
	}
	auto statement = std::move(pending_statement);
	if (profile_statement) {
		auto &profiler = QueryProfiler::Get(context);
		profiler.StartQuery(statement->query, StatementIteratorIsExplainAnalyze(statement.get()));
		profiler.AddParserTime(parser_timer, parser_start_time, parser_end_time);
	}
	parser_timer.Reset();
	return statement;
}

unique_ptr<SQLStatement> StatementIterator::GetStatement() {
	return GetStatementInternal(false);
}

unique_ptr<SQLStatement> StatementIterator::GetStatementForExecution() {
	return GetStatementInternal(true);
}

} // namespace duckdb
