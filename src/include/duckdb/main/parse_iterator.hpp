//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/parse_iterator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {
class ClientContext;
class ClientContextLock;
class ErrorData;
class SQLStatement;
class TokenIterator;

//! Iterator over the parse-facing statements of a multi-statement query.
//!
//! Usage:
//!   ParseIterator it(context, sql);
//!   while (it.Peek()) {
//!       auto stmt = it.GetStatement();
//!       // raw, just-parsed statement
//!   }
//!
//! Separator-only stretches (e.g. ";;;") are transparently skipped inside Peek — the caller
//! always sees either a real statement or a clean exhaustion.
class ParseIterator {
public:
	//! Peel parse-facing statements out of a SQL string (PEG / parser_override). The context is
	//! bound for the lifetime of the iterator and must outlive it. With apply_invalidation_policy,
	//! a parse error invalidates the active transaction according to the transaction invalidation
	//! policy. Without it, a parse error leaves the transaction untouched, for callers that only
	//! inspect the statements or that handle the error through ErrorResult themselves. A caller that
	//! holds the context lock while using the iterator passes it as held_lock, otherwise the iterator
	//! takes the lock itself to apply the policy.
	DUCKDB_API ParseIterator(ClientContext &context, const string &sql, bool apply_invalidation_policy = true,
	                         optional_ptr<ClientContextLock> held_lock = nullptr);
	DUCKDB_API ~ParseIterator();

	ParseIterator(const ParseIterator &) = delete;
	ParseIterator &operator=(const ParseIterator &) = delete;
	DUCKDB_API ParseIterator(ParseIterator &&) noexcept;
	// Not move-assignable: holds a ClientContext reference, which cannot be rebound.
	ParseIterator &operator=(ParseIterator &&) = delete;

	//! Returns true if a statement is currently available (after parsing as needed). Returns
	//! false when the iterator is exhausted. Non-const: parses on demand and buffers the result.
	//! A parse error ends the iteration: after it is thrown, Peek returns false.
	DUCKDB_API bool Peek();

	//! Returns the next buffered statement and clears the buffer. Returns nullptr if no
	//! statement is buffered — the caller should call Peek() first.
	DUCKDB_API unique_ptr<SQLStatement> GetStatement();

	//! Grammar-free predicate: does another statement remain in the input? Unlike Peek() this never
	//! invokes the parser (only tokenizes / walks the token cursor, skipping separators), so it never
	//! throws a parser error and is safe as a look-ahead before the current statement has executed.
	//! Assumes the input has already been resolved by a prior Peek() (true for all current callers).
	DUCKDB_API bool HasMore();

	//! The context this iterator is bound to (used by StatementIterator to inherit it).
	DUCKDB_API ClientContext &GetClientContext();

private:
	bool PeekInternal();
	void ApplyInvalidationPolicy(const ErrorData &error);
	//! Tokenize the full input once (grammar-free); no-op if already tokenized.
	void EnsureTokenized();

private:
	//! The bound context, used for parser options / metrics / override extensions.
	ClientContext &context;
	string sql;
	bool apply_invalidation_policy;
	optional_ptr<ClientContextLock> held_lock;
	//! Tokenized view of `sql` and its current position. Populated once on the first Peek.
	unique_ptr<TokenIterator> token_iterator;
	//! Single-statement buffer holding the result of the most recent Peek. Cleared by
	//! GetStatement.
	unique_ptr<SQLStatement> current_statement;
	//! Once Peek determines there are no more statements (cursor past end of tokens), we stay
	//! exhausted; subsequent Peek calls return false without re-invoking the parser.
	bool exhausted = false;
	//! Statements produced by a successful `parser_override` extension; if non-empty the
	//! iterator yields these in order instead of running the PEG parser at all. Populated on
	//! the first Peek when an extension claims the query.
	unique_ptr<vector<unique_ptr<SQLStatement>>> overridden_statements;
	//! Cursor into `overridden_statements`.
	idx_t override_cursor = 0;
	//! True once we've consulted parser_override extensions for this query.
	bool override_resolved = false;
};

} // namespace duckdb
