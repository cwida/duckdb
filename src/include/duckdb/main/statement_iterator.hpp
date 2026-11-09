//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/statement_iterator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/profiler.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/main/parse_iterator.hpp"

namespace duckdb {
class ClientContext;
class SQLStatement;

//! Iterator over the user statements of a script, as parsed. The submission of each statement expands it.
//!
//! Usage:
//!   StatementIterator it(ParseIterator(context, sql));
//!   while (it.Peek()) {
//!       auto stmt = it.GetStatement();
//!   }
//!
class StatementIterator {
public:
	//! Wrap a lazy parse-facing stream (consumed by move). The context is inherited from the
	//! wrapped ParseIterator and must outlive this iterator.
	DUCKDB_API explicit StatementIterator(ParseIterator &&parse_iterator);
	DUCKDB_API ~StatementIterator();

	StatementIterator(const StatementIterator &) = delete;
	StatementIterator &operator=(const StatementIterator &) = delete;
	DUCKDB_API StatementIterator(StatementIterator &&) noexcept;
	// Not move-assignable: holds a ClientContext reference, which cannot be rebound.
	StatementIterator &operator=(StatementIterator &&) = delete;

	//! Returns true while another statement remains. Parses ahead, so it throws a parser error.
	DUCKDB_API bool Peek();

	//! Grammar-free predicate: is there another statement after the current one? Never parses and
	//! never throws (see ParseIterator::HasMore), so it is safe to consult before the current
	//! statement has executed, for example to decide whether the current statement is the last.
	DUCKDB_API bool HasMore();

	//! Pull the next statement. Returns nullptr once the input is exhausted (Peek would return false).
	DUCKDB_API unique_ptr<SQLStatement> GetStatement();
	//! Pull the next statement and attach its parser timing to the per-statement query profiler.
	DUCKDB_API unique_ptr<SQLStatement> GetStatementForExecution();

private:
	unique_ptr<SQLStatement> GetStatementInternal(bool profile_statement);

private:
	//! The single parse-facing source this iterator pulls from.
	ParseIterator source;
	//! The bound context, inherited from `source`, whose profiler receives the parser timing.
	ClientContext &context;
	//! The statement parsed ahead by Peek.
	unique_ptr<SQLStatement> pending_statement;
	//! Parser timing for the statement parsed ahead.
	Profiler parser_timer;
	timestamp_t parser_start_time = timestamp_t::epoch();
	timestamp_t parser_end_time = timestamp_t::epoch();
};

} // namespace duckdb
