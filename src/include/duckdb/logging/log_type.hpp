//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/logging/log_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/logging/logging.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

namespace duckdb {

struct FileHandle;
struct BaseRequest;
struct HTTPResponse;
struct MetricTiming;
class PhysicalOperator;
enum class PhysicalOperatorType : uint8_t;
class AttachedDatabase;
class RowGroup;
struct DataTableInfo;
//! Log types provide some structure to the formats that the different log messages can have
//! For now, this holds a type that the VARCHAR value will be auto-cast into.
class LogType {
public:
	//! Construct an unstructured type
	LogType(const string &name_p, const LogLevel &level_p)
	    : name(name_p), level(level_p), is_structured(false), type(LogicalType::VARCHAR) {
	}
	//! Construct a structured type
	LogType(const string &name_p, const LogLevel &level_p, LogicalType structured_type)
	    : name(name_p), level(level_p), is_structured(true), type(std::move(structured_type)) {
		if (!type.IsNested()) {
			throw InternalException("LogType must be nested if the type is explicitly set");
		}
	}

	string name;
	LogLevel level;

	bool is_structured;
	LogicalType type;
};

class DefaultLogType : public LogType {
public:
	static constexpr const char *NAME = "";
	static constexpr LogLevel LEVEL = LogLevel::LOG_INFO;

	DefaultLogType() : LogType(NAME, LEVEL) {
	}
};

class QueryLogType : public LogType {
public:
	static constexpr const char *NAME = "QueryLog";
	static constexpr LogLevel LEVEL = LogLevel::LOG_INFO;

	QueryLogType() : LogType(NAME, LEVEL) {};

	static string ConstructLogMessage(const string &str);
};

class FileSystemLogType : public LogType {
public:
	static constexpr const char *NAME = "FileSystem";
	static constexpr LogLevel LEVEL = LogLevel::LOG_TRACE;

	//! Construct the log type
	FileSystemLogType();

	static LogicalType GetLogType();

	static string ConstructLogMessage(const FileHandle &handle, const string &op, int64_t bytes, idx_t pos);
	static string ConstructLogMessage(const FileHandle &handle, const string &op);
};

class HTTPLogType : public LogType {
public:
	static constexpr const char *NAME = "HTTP";
	static constexpr LogLevel LEVEL = LogLevel::LOG_DEBUG;
	static constexpr auto REDACTED_VALUE = "redacted";

	//! Construct the log types
	HTTPLogType();

	static LogicalType GetLogType();

	static string ConstructLogMessage(BaseRequest &request, optional_ptr<HTTPResponse> response,
	                                  bool redact_http_logs = true);

	// FIXME: HTTPLogType should be structured probably
	static string ConstructLogMessage(const string &str) {
		return str;
	}
};

class PhysicalOperatorLogType : public LogType {
public:
	static constexpr const char *NAME = "PhysicalOperator";
	static constexpr LogLevel LEVEL = LogLevel::LOG_DEBUG;

	//! Construct the log type
	PhysicalOperatorLogType();

	static LogicalType GetLogType();

	static string ConstructLogMessage(const PhysicalOperator &op, const string &class_p, const string &event,
	                                  const vector<pair<string, string>> &info);
	static string ConstructLogMessage(PhysicalOperatorType operator_type,
	                                  const vector<pair<string, string>> &parameters, const string &class_p,
	                                  const string &event, const vector<pair<string, string>> &info);
};

class MetricsLogType : public LogType {
public:
	static constexpr const char *NAME = "Metrics";
	static constexpr LogLevel LEVEL = LogLevel::LOG_INFO;

	//! Construct the log type
	MetricsLogType();

	static LogicalType GetLogType();

	static string ConstructLogMessage(const string &metric, const Value &value,
	                                  optional_ptr<const MetricTiming> timing = nullptr);
};

class CheckpointLogType : public LogType {
public:
	static constexpr const char *NAME = "Checkpoint";
	static constexpr LogLevel LEVEL = LogLevel::LOG_DEBUG;

	//! Construct the log type
	CheckpointLogType();

	static LogicalType GetLogType();

	//! Vacuum
	static string ConstructLogMessage(const AttachedDatabase &db, DataTableInfo &table, idx_t segment_idx,
	                                  idx_t merge_count, idx_t target_count, idx_t merge_rows, idx_t row_start);
	//! Checkpoint
	static string ConstructLogMessage(const AttachedDatabase &db, DataTableInfo &table, idx_t segment_idx,
	                                  RowGroup &row_group, idx_t row_group_start);

private:
	static string CreateLog(const AttachedDatabase &db, DataTableInfo &table, const char *op, vector<Value> map_keys,
	                        vector<Value> map_values);
};

class TransactionLogType : public LogType {
public:
	static constexpr const char *NAME = "Transaction";
	static constexpr LogLevel LEVEL = LogLevel::LOG_DEBUG;

	//! Construct the log type
	TransactionLogType();

	static LogicalType GetLogType();

	static string ConstructLogMessage(const AttachedDatabase &db, const char *log_type,
	                                  transaction_t transaction_id = MAX_TRANSACTION_ID);
};

class AdaptiveFilterLogType : public LogType {
public:
	static constexpr const char *NAME = "AdaptiveFilter";
	static constexpr LogLevel LEVEL = LogLevel::LOG_DEBUG;

	AdaptiveFilterLogType();

	static LogicalType GetLogType();

	static string ConstructLogMessage(const char *event, const string &file_path, const vector<idx_t> &permutation,
	                                  const vector<pair<string, string>> &info);
};

class ParquetPrefetchLogType : public LogType {
public:
	static constexpr const char *NAME = "ParquetPrefetch";
	static constexpr LogLevel LEVEL = LogLevel::LOG_DEBUG;

	ParquetPrefetchLogType();

	static LogicalType GetLogType();

	static string ConstructLogMessage(const string &file_path, idx_t row_group_id, bool fully_filtered,
	                                  const char *strategy, const vector<vector<string>> &prefetch_groups,
	                                  const vector<string> &minimal_filters, uint64_t accepted_column_gap);
};

class AsyncTaskScheduleLogType : public LogType {
public:
	static constexpr const char *NAME = "AsyncTaskSchedule";
	static constexpr LogLevel LEVEL = LogLevel::LOG_DEBUG;

	AsyncTaskScheduleLogType();

	static LogicalType GetLogType();

	static string ConstructLogMessage(const string &pool, idx_t task_count);
};

class ReadAheadBudgetLogType : public LogType {
public:
	static constexpr const char *NAME = "ReadAheadBudget";
	static constexpr LogLevel LEVEL = LogLevel::LOG_DEBUG;

	ReadAheadBudgetLogType();

	static LogicalType GetLogType();

	//! One entry per pushed read-ahead job with the backlog charge, its budget and the jobs in flight
	static string ConstructLogMessage(idx_t pending_bytes, idx_t budget_bytes, idx_t active_jobs);
};

class ProgressVerificationLogType : public LogType {
public:
	static constexpr const char *NAME = "ProgressVerification";
	static constexpr LogLevel LEVEL = LogLevel::LOG_INFO;

	ProgressVerificationLogType();

	static LogicalType GetLogType();

	static string ConstructLogMessage(const string &invariant, const string &operator_name, const string &pipeline,
	                                  const string &detail);
};

class ExternalResourceLogType : public LogType {
public:
	static constexpr const char *NAME = "ExternalResource";
	static constexpr LogLevel LEVEL = LogLevel::LOG_INFO;

	ExternalResourceLogType();

	static LogicalType GetLogType();

	//! One recipe callback invocation (create/status/destroy), logged on response. `error` is empty on
	//! success (rendered NULL); `resource_name` is empty for an anonymous resource (rendered NULL).
	static string ConstructLogMessage(const string &resource_type, const string &resource_name, const string &operation,
	                                  const string &error, const Value &extra_info);
};

} // namespace duckdb
