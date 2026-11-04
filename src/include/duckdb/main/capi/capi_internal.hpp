//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/capi/capi_internal.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.h"
#include "duckdb.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"
#include "duckdb/main/db_instance_cache.hpp"

#include <cstring>
#include <cassert>

#ifdef _WIN32
#ifndef strdup
#define strdup _strdup
#endif
#endif

namespace duckdb {

//! Allocate payloads before exposing a flat vector through the v1 C API.
void PrepareCAPIVector(Vector &result);

struct DBInstanceCacheWrapper {
	unique_ptr<DBInstanceCache> instance_cache;
};

struct DatabaseWrapper {
	shared_ptr<DuckDB> database;
};

struct CClientContextWrapper {
	explicit CClientContextWrapper(ClientContext &context) : context(context) {};
	ClientContext &context;
};

struct CClientArrowOptionsWrapper {
	explicit CClientArrowOptionsWrapper(ClientProperties &properties) : properties(properties) {};
	ClientProperties properties;
};

struct PreparedStatementWrapper {
	//! Map of name -> values
	identifier_map_t<BoundParameterData> values;
	unique_ptr<PreparedStatement> statement;
	bool success = true;
	ErrorData error_data;
	unordered_map<idx_t, string> param_index_to_name;
};

struct ExtractStatementsWrapper {
	vector<unique_ptr<SQLStatement>> statements;
	string error;
};

struct PendingStatementWrapper {
	unique_ptr<QueryResult> statement;
	bool allow_streaming;
};

struct ArrowResultWrapper {
	unique_ptr<QueryResult> result;
};

struct AppenderWrapper {
	unique_ptr<BaseAppender> appender;
	ErrorData error_data;
	bool flush_failed = false;
};

struct TableDescriptionWrapper {
	unique_ptr<TableDescription> description;
	string error;
};

struct ErrorDataWrapper {
	ErrorData error_data;
};

struct ExpressionWrapper {
	unique_ptr<Expression> expr;
};

enum class CAPIResultSetType : uint8_t {
	CAPI_RESULT_TYPE_NONE = 0,
	CAPI_RESULT_TYPE_MATERIALIZED,
	CAPI_RESULT_TYPE_STREAMING,
	CAPI_RESULT_TYPE_DEPRECATED
};

//! Either a query handle or a stream opened from one. Only the streaming entry points care which;
//! everything else reads the metadata and the error, which both carry
struct DuckDBResultData {
	//! The query handle, or null once a stream was opened from it
	unique_ptr<QueryResult> result;
	//! The stream opened from the handle (may be null)
	unique_ptr<QueryResultStream<>> stream;
	// Results can only use either the new API or the old API, not a mix of the two
	// They start off as "none" and switch to one or the other when an API method is used
	CAPIResultSetType result_set_type;

	bool IsStreaming() const {
		return stream != nullptr;
	}
	//! The retained result. Only valid when the result is not streaming
	QueryResult &Retained() const {
		D_ASSERT(result);
		return *result;
	}
	bool HasError() const {
		return stream ? stream->HasError() : result->HasError();
	}
	const string &GetError() const {
		return stream ? stream->GetError() : result->GetError();
	}
	const ExceptionType &GetErrorType() const {
		return stream ? stream->GetErrorType() : result->GetErrorType();
	}
	idx_t ColumnCount() const {
		return stream ? stream->ColumnCount() : result->ColumnCount();
	}
	const vector<LogicalType> &GetTypes() const {
		return stream ? stream->GetTypes() : result->GetTypes();
	}
	const Identifier &ColumnName(idx_t index) const {
		return stream ? stream->ColumnName(index) : result->ColumnName(index);
	}
	StatementType GetStatementType() const {
		return stream ? stream->GetStatementType() : result->GetStatementType();
	}
	const StatementProperties &GetStatementProperties() const {
		return stream ? stream->GetStatementProperties() : result->GetStatementProperties();
	}
	ClientProperties &GetClientProperties() {
		return stream ? stream->GetClientProperties() : result->client_properties;
	}
	unique_ptr<DataChunk> Fetch() {
		return stream ? stream->Fetch() : result->Fetch();
	}
};

duckdb_type LogicalTypeIdToC(const LogicalTypeId type);
LogicalTypeId LogicalTypeIdFromC(const duckdb_type type);
idx_t GetCTypeSize(const duckdb_type type);
duckdb_statement_type StatementTypeToC(const StatementType type);
duckdb_error_type ErrorTypeToC(const ExceptionType type);
ExceptionType ErrorTypeFromC(const duckdb_error_type type);

duckdb_state DuckDBTranslateResult(unique_ptr<QueryResult> result, duckdb_result *out);
duckdb_state DuckDBTranslateStreamResult(unique_ptr<QueryResultStream<>> stream, duckdb_result *out);
bool DeprecatedMaterializeResult(duckdb_result *result);

} // namespace duckdb
