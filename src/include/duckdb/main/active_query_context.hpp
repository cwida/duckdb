//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/active_query_context.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/result_lifetime.hpp"
#include "duckdb/common/progress_bar/progress_bar.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/main/query_parameters.hpp"
#include "duckdb/planner/statement_preprocessor.hpp"

namespace duckdb {
class BaseQueryResult;
class QueryResult;

//! The user statement a connection runs, from its submission until a call ends it. A statement that expands into
//! several runs them one after another, each with its own executor, as one query
struct ActiveQueryContext {
public:
	explicit ActiveQueryContext(PreprocessedStatement statements_p) : statements(std::move(statements_p.statements)) {
	}

	//! The text of the statement that is running
	string query;
	//! Prepared statement data of the statement that is running
	shared_ptr<PreparedStatementData> prepared;
	//! The executor of the statement that is running
	unique_ptr<Executor> executor;
	//! The progress bar of the statement that is running
	unique_ptr<ProgressBar> progress_bar;
	//! Whether a collector built the open result, which has no way to end the query
	bool collector_built_result = false;
	//! The format every statement of the query is submitted in; null is the chunk format
	shared_ptr<ResultFormat> format;
	//! The consumer's retention decision, applied to the result statement's buffer when it is created
	ResultLifetime retention = ResultLifetime::UNDECIDED;
	//! Whether the statement whose rows the result carries has started
	bool result_statement_started = false;
	//! The handle of a statement that expands into several, which the statement producing the result fills in
	optional_ptr<QueryResult> handle;
	//! Whether the statements run inside the submission, where a result collector may build its own result object
	bool inside_submission = false;
	//! Whether the statement runs on the CONNECT-ed database, outside the local transaction
	bool outside_local_transaction = false;

public:
	void SetOpenResult(BaseQueryResult &result) {
		open_result = &result;
	}
	bool IsOpenResult(BaseQueryResult &result) {
		return open_result == &result;
	}
	bool HasOpenResult() const {
		return open_result != nullptr;
	}
	//! Whether the user's statement expanded into several
	bool ExpandsIntoSeveral() const {
		// Started statements stay in the list, moved out
		return statements.size() > 1;
	}
	bool HasNextStatement() const {
		return next_statement < statements.size();
	}
	unique_ptr<SQLStatement> TakeNextStatement() {
		D_ASSERT(HasNextStatement());
		return std::move(statements[next_statement++]);
	}
	//! Records the databases a statement of the query read and modified
	void RecordDatabases(const StatementProperties &properties) {
		MergeDatabases(properties, databases);
	}
	//! Whether a statement of the query that started modifies a database
	bool Wrote() const {
		return !databases.IsReadOnly();
	}
	//! The properties of the statement that produces the result, describing the databases of the whole query
	StatementProperties QueryProperties(StatementProperties properties) const {
		MergeDatabases(databases, properties);
		return properties;
	}

private:
	//! The currently open result
	BaseQueryResult *open_result = nullptr;
	//! The statements the user's statement preprocessed into
	vector<unique_ptr<SQLStatement>> statements;
	//! The index of the statement that starts next
	idx_t next_statement = 0;
	//! The databases the statements that already ran read and modified
	StatementProperties databases;

private:
	static void MergeDatabases(const StatementProperties &source, StatementProperties &target) {
		for (auto &entry : source.read_databases) {
			target.read_databases.insert(entry);
		}
		for (auto &entry : source.modified_databases) {
			auto inserted = target.modified_databases.insert(entry);
			if (!inserted.second) {
				inserted.first->second.modifications |= entry.second.modifications;
			}
		}
	}
};

} // namespace duckdb
