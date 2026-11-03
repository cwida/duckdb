#include "duckdb/logging/log_type.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/main/profiler/profiling_utils.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/logging/file_system_logger.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"

namespace duckdb {

constexpr LogLevel DefaultLogType::LEVEL;
constexpr LogLevel FileSystemLogType::LEVEL;
constexpr LogLevel QueryLogType::LEVEL;
constexpr LogLevel HTTPLogType::LEVEL;
constexpr LogLevel PhysicalOperatorLogType::LEVEL;
constexpr LogLevel MetricsLogType::LEVEL;
constexpr LogLevel CheckpointLogType::LEVEL;
constexpr LogLevel AdaptiveFilterLogType::LEVEL;
constexpr LogLevel ParquetPrefetchLogType::LEVEL;
constexpr LogLevel AsyncTaskScheduleLogType::LEVEL;
constexpr LogLevel ReadAheadBudgetLogType::LEVEL;
constexpr LogLevel ProgressVerificationLogType::LEVEL;

//===--------------------------------------------------------------------===//
// QueryLogType
//===--------------------------------------------------------------------===//
string QueryLogType::ConstructLogMessage(const string &str) {
	return str;
}

//===--------------------------------------------------------------------===//
// FileSystemLogType
//===--------------------------------------------------------------------===//
FileSystemLogType::FileSystemLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

// FIXME: Manual JSON strings are not winning any style points
string FileSystemLogType::ConstructLogMessage(const FileHandle &handle, const string &op, int64_t bytes, idx_t pos) {
	return StringUtil::Format("{\"fs\":\"%s\",\"path\":\"%s\",\"op\":\"%s\",\"bytes\":\"%lld\",\"pos\":\"%llu\"}",
	                          handle.file_system.GetName(), handle.path, op, bytes, pos);
}
string FileSystemLogType::ConstructLogMessage(const FileHandle &handle, const string &op) {
	return StringUtil::Format("{\"fs\":\"%s\",\"path\":\"%s\",\"op\":\"%s\"}", handle.file_system.GetName(),
	                          handle.path, op);
}

LogicalType FileSystemLogType::GetLogType() {
	LogicalType result;
	child_list_t<LogicalType> child_list = {
	    {"fs", LogicalType::VARCHAR},   {"path", LogicalType::VARCHAR}, {"op", LogicalType::VARCHAR},
	    {"bytes", LogicalType::BIGINT}, {"pos", LogicalType::BIGINT},
	};
	return LogicalType::STRUCT(child_list);
}

//===--------------------------------------------------------------------===//
// HTTPLogType
//===--------------------------------------------------------------------===//
static const case_insensitive_set_t &HTTPRequestHeaderAllowList() {
	//! For extra info, the advise from OpenTelemetry:
	//! https://opentelemetry.io/docs/specs/semconv/registry/attributes/http/#http-request-header
	static const case_insensitive_set_t allow_list = {
	    // HTTP semantics (https://www.rfc-editor.org/rfc/rfc9110.html).
	    "accept",              // Accepted media types and parameters.
	    "accept-charset",      // Accepted character sets.
	    "accept-encoding",     // Accepted content codings.
	    "accept-language",     // Preferred response languages.
	    "connection",          // Connection handling options.
	    "content-encoding",    // Applied content codings.
	    "content-language",    // Languages of the intended audience.
	    "content-length",      // Content size in bytes.
	    "content-type",        // Content media type and parameters.
	    "date",                // Message timestamp.
	    "expect",              // Expected server behavior before sending content.
	    "if-modified-since",   // Modification timestamp used for conditional requests.
	    "if-unmodified-since", // Modification timestamp used for conditional requests.
	    "max-forwards",        // Remaining forwarding limit.
	    "range",               // Requested ranges.
	    "te",                  // Accepted transfer codings and trailer support.
	    "trailer",             // Names of fields sent in trailers.
	    "upgrade",             // Proposed or selected protocols.
	    "user-agent",          // Client product and version information.
	    // "via",                 // Intermediary protocols, hosts and software comments.
	    // "authorization", // Contains credentials and signatures.
	    // "content-location", // URI can contain sensitive paths or query parameters.
	    // "from", // Contains the user's email address.
	    // "host", // Identifies the endpoint, including bucket or tenant names.
	    // "if-match", // Contains opaque resource validators.
	    // "if-none-match", // Contains opaque resource validators.
	    // "if-range", // Can contain an opaque resource validator instead of a date.
	    // "proxy-authorization", // Contains proxy credentials.
	    // "referer", // URI can contain sensitive paths or query parameters.

	    // Headers defined outside RFC 9110.
	    "cache-control",     // Caching directives.
	    "transfer-encoding", // Applied transfer codings.
	    // "cookie", // Can contain session credentials and user data.

	    // S3 common headers (https://docs.aws.amazon.com/AmazonS3/latest/developerguide/RESTCommonRequestHeaders.html).
	    "x-amz-date", // Request signing timestamp.
	    // "content-md5", // Fingerprints the request content.
	    // "x-amz-content-sha256", // Can fingerprint the payload, not just contain a fixed signing marker.
	    // "x-amz-security-token", // Contains session credentials.

	    // S3 PutObject (https://docs.aws.amazon.com/AmazonS3/latest/API/API_PutObject.html).
	    "x-amz-request-payer",          // Requester-pays billing mode.
	    "x-amz-server-side-encryption", // Server-side encryption algorithm.
	    // "x-amz-expected-bucket-owner", // Identifies an AWS account.
	    // "x-amz-server-side-encryption-aws-kms-key-id", // Identifies an encryption key and possibly its account.
	    // "x-amz-server-side-encryption-context", // Contains caller-provided encryption context.
	    // "x-amz-server-side-encryption-customer-key", // Contains the customer encryption key.
	    // "x-amz-server-side-encryption-customer-key-md5", // Fingerprints the customer encryption key.
	    // "x-amz-tagging", // Contains caller-provided object tags.
	    // "x-amz-website-redirect-location", // URI can contain sensitive paths or query parameters.

	    // S3 CopyObject (https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html).
	    // "x-amz-copy-source", // Contains a bucket, object path and possibly a version ID.
	    // "x-amz-copy-source-if-match", // Contains opaque resource validators.
	    // "x-amz-copy-source-if-none-match", // Contains opaque resource validators.
	    // "x-amz-copy-source-server-side-encryption-customer-key", // Contains the source encryption key.
	    // "x-amz-copy-source-server-side-encryption-customer-key-md5", // Fingerprints the source encryption key.
	    // "x-amz-source-expected-bucket-owner", // Identifies an AWS account.

	    // S3 session authorization (https://docs.aws.amazon.com/AmazonS3/latest/API/API_CreateSession.html).
	    // "x-amz-s3session-token", // Contains S3 Express session credentials.

	    // AWS JSON protocol (https://smithy.io/2.0/aws/protocols/aws-json-1_1-protocol.html), e.g. AWS Glue.
	    "x-amz-target", // Service and operation name, e.g. AWSGlue.GetTable.
	};
	return allow_list;
}

static const case_insensitive_set_t &HTTPResponseHeaderAllowList() {
	static const case_insensitive_set_t allow_list = {
	    // HTTP semantics (https://www.rfc-editor.org/rfc/rfc9110.html).
	    "accept",           // Accepted media types and parameters.
	    "accept-encoding",  // Accepted content codings.
	    "accept-ranges",    // Supported range units.
	    "allow",            // Supported HTTP methods.
	    "connection",       // Connection handling options.
	    "content-encoding", // Applied content codings.
	    "content-language", // Languages of the intended audience.
	    "content-length",   // Content size in bytes.
	    "content-range",    // Transferred range and total size.
	    "content-type",     // Content media type and parameters.
	    "date",             // Message timestamp.
	    "last-modified",    // Resource modification timestamp.
	    "retry-after",      // Retry delay or timestamp.
	    "server",           // Server product and version information.
	    "trailer",          // Names of fields sent in trailers.
	    "upgrade",          // Proposed or selected protocols.
	    "vary",             // Request field names used for response selection.
	    // "via",              // Intermediary protocols, hosts and software comments.
	    // "authentication-info", // Can contain authentication tokens and parameters.
	    // "content-location", // URI can contain sensitive paths or query parameters.
	    // "etag", // Contains an opaque resource validator.
	    // "location", // Redirect URI can contain credentials or signed query parameters.
	    // "proxy-authenticate", // Authentication challenges can contain tokens and realm information.
	    // "proxy-authentication-info", // Can contain authentication tokens and parameters.
	    // "www-authenticate", // Authentication challenges can contain tokens and realm information.

	    // HTTP caching (https://www.rfc-editor.org/rfc/rfc9111.html).
	    "age",           // Time spent in caches.
	    "cache-control", // Caching directives.
	    "expires",       // Cache expiration timestamp.

	    // Other HTTP headers.
	    "transfer-encoding", // Applied transfer codings.
	    // "set-cookie", // Can contain session credentials and user data.

	    // S3 (https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html).
	    "x-amz-bucket-region",          // Bucket region.
	    "x-amz-server-side-encryption", // Server-side encryption algorithm.
	    // "x-amz-abort-rule-id", // Identifies a lifecycle rule.
	    // "x-amz-copy-source-version-id", // Identifies a source object version.
	    // "x-amz-expiration", // Includes a lifecycle rule ID as well as a date.
	    // "x-amz-id-2", // Request correlation ID, enabling it exposes request identity.
	    // "x-amz-request-id", // Request correlation ID, enabling it exposes request identity.
	    // "x-amz-server-side-encryption-aws-kms-key-id", // Identifies an encryption key and possibly its account.
	    // "x-amz-server-side-encryption-customer-key-md5", // Fingerprints the customer encryption key.
	    // "x-amz-version-id", // Identifies an object version.
	};
	return allow_list;
}

HTTPLogType::HTTPLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

LogicalType HTTPLogType::GetLogType() {
	child_list_t<LogicalType> request_child_list = {
	    {"type", LogicalType::VARCHAR},
	    {"url", LogicalType::VARCHAR},
	    {"start_time", LogicalType::TIMESTAMP_TZ},
	    {"duration_ms", LogicalType::BIGINT},
	    {"duration_us", LogicalType::BIGINT},
	    {"request_body_length", LogicalType::UBIGINT},
	    {"headers", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)},
	};
	auto request_type = LogicalType::STRUCT(request_child_list);

	child_list_t<LogicalType> response_child_list = {
	    {"status", LogicalType::VARCHAR},
	    {"reason", LogicalType::VARCHAR},
	    {"headers", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)},
	};
	auto response_type = LogicalType::STRUCT(response_child_list);

	LogicalType result_type;
	child_list_t<LogicalType> child_list = {{"request", request_type}, {"response", response_type}};
	return LogicalType::STRUCT(child_list);
}

static Value CreateHTTPHeadersValue(const HTTPHeaders &headers, const case_insensitive_set_t &allow_list,
                                    bool redact_http_logs) {
	vector<Value> keys;
	vector<Value> values;
	for (const auto &header : headers) {
		keys.emplace_back(header.first);
		const bool allow_value = !redact_http_logs || allow_list.find(header.first) != allow_list.end();
		values.emplace_back(allow_value ? header.second : HTTPLogType::REDACTED_VALUE);
	}
	return Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, keys, values);
}

static string BuildLogURL(const BaseRequest &request, bool redact_http_logs) {
	string url;
	url.reserve(request.proto_host_port.size() + request.path.size());

	if (!redact_http_logs) {
		url.append(request.proto_host_port);
		url.append(request.path);
		return url;
	}

	// Redact user info, e.g. https://user:pass@duckdb.org -> https://redacted@duckdb.org
	const auto pos_at = request.proto_host_port.rfind('@');
	if (pos_at != string::npos) {
		auto pos_proto_end = request.proto_host_port.find("://");
		if (pos_proto_end == string::npos) {
			// Without a scheme separator, redact from the start.
			pos_proto_end = 0;
		} else {
			pos_proto_end += 3;
		}
		url.append(request.proto_host_port, 0, pos_proto_end);
		url.append(HTTPLogType::REDACTED_VALUE);
		url.append(request.proto_host_port, pos_at, string::npos);
	} else {
		url += request.proto_host_port;
	}

	// Redact the query parameters or fragment
	const auto pos_end_of_path = request.path.find_first_of("?#");
	if (pos_end_of_path == string::npos) {
		url += request.path;
	} else {
		url.append(request.path, 0, pos_end_of_path + 1);
		url.append(HTTPLogType::REDACTED_VALUE);
	}
	return url;
}

static string HTTPStatusToLogString(HTTPStatusCode status) {
	try {
		return EnumUtil::ToString(status);
	} catch (const NotImplementedException &) {
		return to_string(static_cast<uint16_t>(status));
	}
}

string HTTPLogType::ConstructLogMessage(BaseRequest &request, optional_ptr<HTTPResponse> response,
                                        bool redact_http_logs) {
	child_list_t<Value> request_child_list = {
	    {"type", Value(EnumUtil::ToString(request.type))},
	    {"url", Value(BuildLogURL(request, redact_http_logs))},
	    {"headers", CreateHTTPHeadersValue(request.headers, HTTPRequestHeaderAllowList(), redact_http_logs)},
	    {"start_time", request.have_request_timing ? Value::TIMESTAMP(request.request_system_start) : Value()},
	    {"duration_ms",
	     request.have_request_timing
	         ? Value::BIGINT(TimePoint::ElapsedMillis(request.request_monotonic_start, request.request_monotonic_end))
	         : Value()},
	    {"duration_us",
	     request.have_request_timing
	         ? Value::BIGINT(TimePoint::ElapsedMicros(request.request_monotonic_start, request.request_monotonic_end))
	         : Value()},
	    {"request_body_length", request.request_body_length ? Value::UBIGINT(request.request_body_length) : Value()}};
	auto request_value = Value::STRUCT(request_child_list);
	Value response_value;
	if (response) {
		child_list_t<Value> response_child_list = {
		    {"status", Value(HTTPStatusToLogString(response->status))},
		    {"reason", Value(response->reason.empty() ? response->GetRequestError() : response->reason)},
		    {"headers", CreateHTTPHeadersValue(response->headers, HTTPResponseHeaderAllowList(), redact_http_logs)},
		};
		response_value = Value::STRUCT(response_child_list);
	}

	child_list_t<Value> child_list = {{"request", request_value}, {"response", response_value}};

	return Value::STRUCT(child_list).ToString();
}

//===--------------------------------------------------------------------===//
// PhysicalOperatorLogType
//===--------------------------------------------------------------------===//
PhysicalOperatorLogType::PhysicalOperatorLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

LogicalType PhysicalOperatorLogType::GetLogType() {
	child_list_t<LogicalType> child_list = {
	    {"operator_type", LogicalType::VARCHAR},
	    {"parameters", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)},
	    {"class", LogicalType::VARCHAR},
	    {"event", LogicalType::VARCHAR},
	    {"info", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)},
	};
	return LogicalType::STRUCT(child_list);
}

template <class ITERABLE>
static Value StringPairIterableToMap(const ITERABLE &iterable) {
	vector<Value> keys;
	vector<Value> values;
	for (const auto &kv : iterable) {
		keys.emplace_back(kv.first);
		values.emplace_back(kv.second);
	}
	return Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(keys), std::move(values));
}

template <class PARAMETERS>
static string ConstructPhysicalOperatorLogMessage(PhysicalOperatorType operator_type, const PARAMETERS &parameters,
                                                  const string &class_p, const string &event,
                                                  const vector<pair<string, string>> &info) {
	child_list_t<Value> child_list = {
	    {"operator_type", EnumUtil::ToString(operator_type)},
	    {"parameters", StringPairIterableToMap(parameters)},
	    {"class", class_p},
	    {"event", event},
	    {"info", StringPairIterableToMap(info)},
	};

	return Value::STRUCT(std::move(child_list)).ToString();
}

string PhysicalOperatorLogType::ConstructLogMessage(const PhysicalOperator &physical_operator, const string &class_p,
                                                    const string &event, const vector<pair<string, string>> &info) {
	return ConstructPhysicalOperatorLogMessage(physical_operator.type, physical_operator.ParamsToString(), class_p,
	                                           event, info);
}

string PhysicalOperatorLogType::ConstructLogMessage(PhysicalOperatorType operator_type,
                                                    const vector<pair<string, string>> &parameters,
                                                    const string &class_p, const string &event,
                                                    const vector<pair<string, string>> &info) {
	return ConstructPhysicalOperatorLogMessage(operator_type, parameters, class_p, event, info);
}

//===--------------------------------------------------------------------===//
// MetricsLogType
//===--------------------------------------------------------------------===//
MetricsLogType::MetricsLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

LogicalType MetricsLogType::GetLogType() {
	child_list_t<LogicalType> child_list = {
	    {"metric", LogicalType::VARCHAR},
	    {"value", LogicalType::VARCHAR},
	    {"start_time", LogicalType::TIMESTAMP_TZ},
	    {"end_time", LogicalType::TIMESTAMP_TZ},
	};
	return LogicalType::STRUCT(child_list);
}

string MetricsLogType::ConstructLogMessage(const string &metric, const Value &value,
                                           optional_ptr<const MetricTiming> timing) {
	child_list_t<Value> child_list = {
	    {"metric", metric},
	    {"value", value.ToString()},
	    {"start_time", timing && timing->has_bounds ? Value::TIMESTAMP(timing->start_time) : Value()},
	    {"end_time", timing && timing->has_bounds ? Value::TIMESTAMP(timing->end_time) : Value()},
	};
	return Value::STRUCT(std::move(child_list)).ToString();
}

//===--------------------------------------------------------------------===//
// CheckpointLogType
//===--------------------------------------------------------------------===//
CheckpointLogType::CheckpointLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

LogicalType CheckpointLogType::GetLogType() {
	child_list_t<LogicalType> child_list = {
	    {"database", LogicalType::VARCHAR},
	    {"schema", LogicalType::VARCHAR},
	    {"table", LogicalType::VARCHAR},
	    {"type", LogicalType::VARCHAR},
	    {"info", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)},
	};
	return LogicalType::STRUCT(child_list);
}

string CheckpointLogType::CreateLog(const AttachedDatabase &db, DataTableInfo &table, const char *op_name,
                                    vector<Value> map_keys, vector<Value> map_values) {
	child_list_t<Value> child_list = {
	    {"database", db.name.GetIdentifierName()},
	    {"schema", table.GetSchemaName().GetIdentifierName()},
	    {"table", table.GetTableName().GetIdentifierName()},
	    {"type", op_name},
	    {"info", Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(map_keys), std::move(map_values))},
	};

	return Value::STRUCT(std::move(child_list)).ToString();
}

string CheckpointLogType::ConstructLogMessage(const AttachedDatabase &db, DataTableInfo &table, idx_t segment_idx,
                                              idx_t merge_count, idx_t target_count, idx_t merge_rows,
                                              idx_t row_start) {
	vector<Value> map_keys = {"segment_idx", "merge_count", "target_count", "merge_rows", "row_start"};
	vector<Value> map_values = {to_string(segment_idx), to_string(merge_count), to_string(target_count),
	                            to_string(merge_rows), to_string(row_start)};
	return CreateLog(db, table, "vacuum", std::move(map_keys), std::move(map_values));
}

string CheckpointLogType::ConstructLogMessage(const AttachedDatabase &db, DataTableInfo &table, idx_t segment_idx,
                                              RowGroup &row_group, idx_t row_group_start) {
	vector<Value> map_keys = {"segment_idx", "start", "count"};
	vector<Value> map_values = {to_string(segment_idx), to_string(row_group_start), to_string(row_group.count.load())};
	return CreateLog(db, table, "checkpoint", std::move(map_keys), std::move(map_values));
}

//===--------------------------------------------------------------------===//
// TransactionLogType
//===--------------------------------------------------------------------===//
constexpr LogLevel TransactionLogType::LEVEL;

TransactionLogType::TransactionLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

LogicalType TransactionLogType::GetLogType() {
	child_list_t<LogicalType> child_list = {
	    {"database", LogicalType::VARCHAR},
	    {"type", LogicalType::VARCHAR},
	    {"transaction_id", LogicalType::UBIGINT},
	};
	return LogicalType::STRUCT(child_list);
}

string TransactionLogType::ConstructLogMessage(const AttachedDatabase &db, const char *log_type,
                                               transaction_t transaction_id) {
	child_list_t<Value> child_list = {
	    {"database", db.name.GetIdentifierName()},
	    {"type", log_type},
	    {"transaction_id", transaction_id == MAX_TRANSACTION_ID ? Value() : Value::UBIGINT(transaction_id)},
	};

	return Value::STRUCT(std::move(child_list)).ToString();
}

//===--------------------------------------------------------------------===//
// AdaptiveFilterLogType
//===--------------------------------------------------------------------===//
AdaptiveFilterLogType::AdaptiveFilterLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

LogicalType AdaptiveFilterLogType::GetLogType() {
	child_list_t<LogicalType> child_list = {
	    {"event", LogicalType::VARCHAR},
	    {"file_path", LogicalType::VARCHAR},
	    {"permutation", LogicalType::VARCHAR},
	    {"info", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)},
	};
	return LogicalType::STRUCT(child_list);
}

string AdaptiveFilterLogType::ConstructLogMessage(const char *event, const string &file_path,
                                                  const vector<idx_t> &permutation,
                                                  const vector<pair<string, string>> &info) {
	auto permutation_str =
	    "[" + StringUtil::Join(permutation, permutation.size(), ", ", [](idx_t v) { return to_string(v); }) + "]";
	child_list_t<Value> child_list = {
	    {"event", Value(event)},
	    {"file_path", Value(file_path)},
	    {"permutation", Value(std::move(permutation_str))},
	    {"info", StringPairIterableToMap(info)},
	};
	return Value::STRUCT(std::move(child_list)).ToString();
}

//===--------------------------------------------------------------------===//
// ParquetPrefetchLogType
//===--------------------------------------------------------------------===//
ParquetPrefetchLogType::ParquetPrefetchLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

LogicalType ParquetPrefetchLogType::GetLogType() {
	child_list_t<LogicalType> child_list = {
	    {"file_path", LogicalType::VARCHAR},
	    {"row_group_id", LogicalType::BIGINT},
	    {"fully_filtered", LogicalType::BOOLEAN},
	    {"strategy", LogicalType::VARCHAR},
	    {"prefetch_groups", LogicalType::LIST(LogicalType::LIST(LogicalType::VARCHAR))},
	    {"minimal_filters", LogicalType::LIST(LogicalType::VARCHAR)},
	    {"accepted_column_gap", LogicalType::UBIGINT},
	};
	return LogicalType::STRUCT(child_list);
}

string ParquetPrefetchLogType::ConstructLogMessage(const string &file_path, idx_t row_group_id, bool fully_filtered,
                                                   const char *strategy, const vector<vector<string>> &prefetch_groups,
                                                   const vector<string> &minimal_filters,
                                                   uint64_t accepted_column_gap) {
	vector<Value> outer;
	outer.reserve(prefetch_groups.size());
	for (auto &group : prefetch_groups) {
		vector<Value> inner;
		inner.reserve(group.size());
		for (auto &name : group) {
			inner.emplace_back(name);
		}
		outer.push_back(Value::LIST(LogicalType::VARCHAR, std::move(inner)));
	}
	vector<Value> minimal;
	minimal.reserve(minimal_filters.size());
	for (auto &name : minimal_filters) {
		minimal.emplace_back(name);
	}
	child_list_t<Value> child_list = {
	    {"file_path", Value(file_path)},
	    {"row_group_id", Value::BIGINT(static_cast<int64_t>(row_group_id))},
	    {"fully_filtered", Value::BOOLEAN(fully_filtered)},
	    {"strategy", strategy ? Value(strategy) : Value(LogicalType::VARCHAR)},
	    {"prefetch_groups", Value::LIST(LogicalType::LIST(LogicalType::VARCHAR), std::move(outer))},
	    {"minimal_filters", Value::LIST(LogicalType::VARCHAR, std::move(minimal))},
	    {"accepted_column_gap", Value::UBIGINT(accepted_column_gap)},
	};
	return Value::STRUCT(std::move(child_list)).ToString();
}

//===--------------------------------------------------------------------===//
// AsyncTaskScheduleLogType
//===--------------------------------------------------------------------===//
AsyncTaskScheduleLogType::AsyncTaskScheduleLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

LogicalType AsyncTaskScheduleLogType::GetLogType() {
	child_list_t<LogicalType> child_list = {
	    {"pool", LogicalType::VARCHAR},
	    {"task_count", LogicalType::BIGINT},
	};
	return LogicalType::STRUCT(child_list);
}

string AsyncTaskScheduleLogType::ConstructLogMessage(const string &pool, idx_t task_count) {
	child_list_t<Value> child_list = {
	    {"pool", Value(pool)},
	    {"task_count", Value::BIGINT(static_cast<int64_t>(task_count))},
	};
	return Value::STRUCT(std::move(child_list)).ToString();
}

//===--------------------------------------------------------------------===//
// ReadAheadBudgetLogType
//===--------------------------------------------------------------------===//
ReadAheadBudgetLogType::ReadAheadBudgetLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

LogicalType ReadAheadBudgetLogType::GetLogType() {
	child_list_t<LogicalType> child_list = {
	    {"pending_bytes", LogicalType::UBIGINT},
	    {"budget_bytes", LogicalType::UBIGINT},
	    {"active_jobs", LogicalType::UBIGINT},
	};
	return LogicalType::STRUCT(child_list);
}

string ReadAheadBudgetLogType::ConstructLogMessage(idx_t pending_bytes, idx_t budget_bytes, idx_t active_jobs) {
	child_list_t<Value> child_list = {
	    {"pending_bytes", Value::UBIGINT(pending_bytes)},
	    {"budget_bytes", Value::UBIGINT(budget_bytes)},
	    {"active_jobs", Value::UBIGINT(active_jobs)},
	};
	return Value::STRUCT(std::move(child_list)).ToString();
}

//===--------------------------------------------------------------------===//
// ProgressVerificationLogType
//===--------------------------------------------------------------------===//
ProgressVerificationLogType::ProgressVerificationLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

LogicalType ProgressVerificationLogType::GetLogType() {
	child_list_t<LogicalType> child_list = {
	    {"invariant", LogicalType::VARCHAR},
	    {"operator", LogicalType::VARCHAR},
	    {"pipeline", LogicalType::VARCHAR},
	    {"detail", LogicalType::VARCHAR},
	};
	return LogicalType::STRUCT(child_list);
}

string ProgressVerificationLogType::ConstructLogMessage(const string &invariant, const string &operator_name,
                                                        const string &pipeline, const string &detail) {
	child_list_t<Value> child_list = {
	    {"invariant", Value(invariant)},
	    {"operator", Value(operator_name)},
	    {"pipeline", Value(pipeline)},
	    {"detail", Value(detail)},
	};
	return Value::STRUCT(std::move(child_list)).ToString();
}

//===--------------------------------------------------------------------===//
// ExternalResourceLogType
//===--------------------------------------------------------------------===//
ExternalResourceLogType::ExternalResourceLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

LogicalType ExternalResourceLogType::GetLogType() {
	child_list_t<LogicalType> child_list = {
	    {"resource_type", LogicalType::VARCHAR},
	    {"resource_name", LogicalType::VARCHAR},
	    {"operation", LogicalType::VARCHAR},
	    {"outcome", LogicalType::VARCHAR},
	    {"error", LogicalType::VARCHAR},
	    {"extra_info", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)},
	};
	return LogicalType::STRUCT(child_list);
}

string ExternalResourceLogType::ConstructLogMessage(const string &resource_type, const string &resource_name,
                                                    const string &operation, const string &error,
                                                    const Value &extra_info) {
	auto nullable = [](const string &s) {
		return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
	};
	child_list_t<Value> child_list = {
	    {"resource_type", nullable(resource_type)},
	    {"resource_name", nullable(resource_name)},
	    {"operation", Value(operation)},
	    {"outcome", Value(error.empty() ? "ok" : "error")},
	    {"error", nullable(error)},
	    {"extra_info", extra_info},
	};
	return Value::STRUCT(std::move(child_list)).ToString();
}

} // namespace duckdb
