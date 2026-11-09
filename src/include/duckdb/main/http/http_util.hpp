//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/http/http_util.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/enums/http_status_code.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/time_point.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/http/http_retry_budget.hpp"
#include <functional>

namespace duckdb {
class DatabaseInstance;
class Logger;
class HTTPUtil;
class HTTPRetryBudget;
class HTTPTransportManager;
class HTTPTransportManagerState;
class FileOpener;
struct FileOpenerInfo;

struct HTTPParams {
	explicit HTTPParams(HTTPUtil &http_util) : http_util(http_util) {
	}
	virtual ~HTTPParams();

	static constexpr uint64_t DEFAULT_TIMEOUT_SECONDS = 30; // 30 sec
	static constexpr uint64_t DEFAULT_RETRIES = 3;
	static constexpr uint64_t DEFAULT_RETRY_WAIT_MS = 100;
	static constexpr float DEFAULT_RETRY_BACKOFF = 4;
	static constexpr bool DEFAULT_KEEP_ALIVE = true;

	uint64_t timeout = DEFAULT_TIMEOUT_SECONDS; // seconds component of a timeout
	uint64_t timeout_usec = 0;                  // usec component of a timeout
	uint64_t retries = DEFAULT_RETRIES;
	uint64_t retry_wait_ms = DEFAULT_RETRY_WAIT_MS;
	float retry_backoff = DEFAULT_RETRY_BACKOFF;
	bool keep_alive = DEFAULT_KEEP_ALIVE;
	bool follow_location = true;
	bool override_verify_ssl = false;
	bool verify_ssl = true;
	//! When set, the caller has already checked the url against the file access settings, so the central check in
	//! HTTPUtil::Request is skipped (used by extension installation, which also allows the official repositories)
	bool skip_external_access_check = false;
	//! The path the request accesses, for file systems that map their own paths onto HTTP urls (e.g. s3://bucket/key).
	//! When set, the file access settings are checked against it instead of the url
	string access_path;

	string http_proxy;
	idx_t http_proxy_port = 0;
	string http_proxy_username;
	string http_proxy_password;
	unordered_map<string, string> extra_headers;
	HTTPUtil &http_util;
	shared_ptr<Logger> logger;

public:
	void Initialize(optional_ptr<FileOpener> opener);
	DUCKDB_API idx_t GetTransportReuseDomain() const;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}

protected:
	//! Adds a provider-specific partition to the manager's reuse key.
	DUCKDB_API void SetTransportReuseDomain(idx_t transport_reuse_domain);

private:
	friend class HTTPUtil;
	friend class HTTPTransportManager;

	bool ShouldRedactLogs() const;

	//! Borrowed database.
	optional_ptr<DatabaseInstance> db;
	//! Manager borrowed from the owning DatabaseInstance.
	optional_ptr<HTTPTransportManager> transport_manager;
	//! Provider publication captured by the creating manager session.
	optional_ptr<HTTPTransportManagerState> transport_state;
	//! Manager-issued identity used for session-local reuse.
	idx_t transport_session_id = DConstants::INVALID_INDEX;
	//! Provider-issued identity for transport-compatible parameters.
	idx_t transport_reuse_domain = 0;
};

struct SignatureV4Params {
	string canonical_request;
	string credential_scope;
	string region;
	string service;
	string secret_access_key;
	string date_now;
	string datetime_now;
};

enum class RequestType : uint8_t {
	GET_REQUEST,
	PUT_REQUEST,
	HEAD_REQUEST,
	DELETE_REQUEST,
	POST_REQUEST,
	OPTIONS_REQUEST
};

enum class HTTPClientCachePolicy : uint8_t { DEFAULT, BYPASS_CACHE };

struct HTTPClientInitializationOptions {
	HTTPClientCachePolicy cache_policy = HTTPClientCachePolicy::DEFAULT;
};

struct HTTPHeaders {
	using header_values_t = vector<string>;
	using header_map_t = case_insensitive_map_t<string>;

public:
	HTTPHeaders() = default;
	explicit HTTPHeaders(DatabaseInstance &db);
	// Out-of-line so the symbol is emitted/exported for loadable (WASM side-module) extensions.
	DUCKDB_API ~HTTPHeaders();

	void Insert(string key, string value);
	//! Append another field line without combining its value.
	void Append(string key, string value);
	bool HasHeader(const string &key) const;
	//! Return the first field value.
	string GetHeaderValue(const string &key) const;
	header_values_t GetHeaderValues(const string &key) const;

	header_map_t::iterator begin() { // NOLINT: match stl API
		return headers.begin();
	}
	header_map_t::iterator end() { // NOLINT: match stl API
		return headers.end();
	}
	header_map_t::const_iterator begin() const { // NOLINT: match stl API
		return headers.begin();
	}
	header_map_t::const_iterator end() const { // NOLINT: match stl API
		return headers.end();
	}
	header_map_t::const_iterator cbegin() const { // NOLINT: match stl API
		return headers.begin();
	}
	header_map_t::const_iterator cend() const { // NOLINT: match stl API
		return headers.end();
	}
	header_map_t::mapped_type &operator[](const header_map_t::key_type &key) {
		return headers[key];
	}

private:
	header_map_t headers;
	//! HTTP responses can contain multiple field lines with the same name (i.e., Cache-Control).
	case_insensitive_map_t<header_values_t> repeated_headers;
};

struct HTTPResponse {
	explicit HTTPResponse(HTTPStatusCode code);
	// Out-of-line so the symbol is emitted/exported for loadable (WASM side-module) extensions.
	DUCKDB_API ~HTTPResponse();

	HTTPStatusCode status;
	string url;
	string body;
	string request_error;
	string reason;
	HTTPHeaders headers;
	bool success = true;

public:
	bool HasHeader(const string &key) const;
	string GetHeaderValue(const string &key) const;

	bool Success() const;

	bool HasRequestError() const;
	const string &GetRequestError() const;
	const string &GetError() const;

	bool ShouldRetry() const;
};

struct BaseRequest {
	BaseRequest(RequestType type, const string &url, const HTTPHeaders &headers, HTTPParams &params);
	// Out-of-line so the symbol is emitted/exported for loadable (WASM side-module) extensions.
	// Non-virtual on purpose: these types carry no vtable and are only destroyed as their concrete
	// stack type, so keeping the dtor non-virtual preserves object layout / ABI.
	DUCKDB_API ~BaseRequest();

	RequestType type;
	string url;
	string path;
	string proto_host_port;
	HTTPHeaders headers;
	HTTPParams &params;
	//! Whether or not to return failed requests (instead of throwing)
	bool try_request = false;
	//! Borrowed operation-local budget; it must outlive all participating requests.
	optional_ptr<HTTPRetryBudget> retry_budget;

	//! Requests will optionally contain their timings
	bool have_request_timing = false;
	// System clock start timestamp
	timestamp_t request_system_start;
	// Monotonic clock start and end timestamp
	TimePoint request_monotonic_start;
	TimePoint request_monotonic_end;
	//! Request body size in bytes (the Content-Length we send). Only set for PUT/POST.
	idx_t request_body_length = 0;

	//! Optional per-request network measurements, populated by clients that measure them.
	bool have_time_to_fst_byte = false;
	double time_to_fst_byte_sec = 0;
	idx_t bytes_received = 0;

	template <class TARGET>
	TARGET &Cast() {
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		return reinterpret_cast<const TARGET &>(*this);
	}

	static HTTPHeaders MergeHeaders(const HTTPHeaders &headers, HTTPParams &params) {
		HTTPHeaders result = headers;
		for (const auto &header : params.extra_headers) {
			result.Insert(header.first, header.second);
		}
		return result;
	}
};

struct GetRequestInfo : public BaseRequest {
	GetRequestInfo(const string &url, const HTTPHeaders &headers, HTTPParams &params,
	               std::function<bool(const HTTPResponse &response)> response_handler_p,
	               std::function<bool(const_data_ptr_t data, idx_t data_length)> content_handler_p)
	    : BaseRequest(RequestType::GET_REQUEST, url, headers, params), content_handler(std::move(content_handler_p)),
	      response_handler(std::move(response_handler_p)) {
	}

	DUCKDB_API ~GetRequestInfo();

	std::function<bool(const_data_ptr_t data, idx_t data_length)> content_handler;
	std::function<bool(const HTTPResponse &response)> response_handler;
};

struct PutRequestInfo : public BaseRequest {
	PutRequestInfo(const string &path, const HTTPHeaders &headers, HTTPParams &params, const_data_ptr_t buffer_in,
	               idx_t buffer_in_len, const string &content_type)
	    : BaseRequest(RequestType::PUT_REQUEST, path, headers, params), buffer_in(buffer_in),
	      buffer_in_len(buffer_in_len), content_type(content_type) {
		request_body_length = buffer_in_len;
	}
	DUCKDB_API ~PutRequestInfo();

	const_data_ptr_t buffer_in;
	idx_t buffer_in_len;
	string content_type;
};

struct HeadRequestInfo : public BaseRequest {
	HeadRequestInfo(const string &path, const HTTPHeaders &headers, HTTPParams &params)
	    : BaseRequest(RequestType::HEAD_REQUEST, path, headers, params) {
	}
	DUCKDB_API ~HeadRequestInfo();
};

struct DeleteRequestInfo : public BaseRequest {
	DeleteRequestInfo(const string &path, const HTTPHeaders &headers, HTTPParams &params)
	    : BaseRequest(RequestType::DELETE_REQUEST, path, headers, params) {
	}
	DUCKDB_API ~DeleteRequestInfo();
};

struct OptionsRequestInfo : public BaseRequest {
	OptionsRequestInfo(const string &path, const HTTPHeaders &headers, HTTPParams &params)
	    : BaseRequest(RequestType::OPTIONS_REQUEST, path, headers, params) {
	}
};

struct PostRequestInfo : public BaseRequest {
	PostRequestInfo(const string &path, const HTTPHeaders &headers, HTTPParams &params, const_data_ptr_t buffer_in,
	                idx_t buffer_in_len)
	    : BaseRequest(RequestType::POST_REQUEST, path, headers, params), buffer_in(buffer_in),
	      buffer_in_len(buffer_in_len) {
		request_body_length = buffer_in_len;
	}

	DUCKDB_API ~PostRequestInfo();

	const_data_ptr_t buffer_in;
	idx_t buffer_in_len;
	string buffer_out;
	//! Used to send a GET request with a body (non-standard but supported by some servers)
	bool send_post_as_get_request = false;
};

class HTTPClient {
public:
	HTTPClient() = default;
	explicit HTTPClient(const string &proto_host_port) : base_url(proto_host_port) {
	}
	virtual ~HTTPClient() = default;
	//! Apply all request-local settings before use, including clearing absent credentials.
	virtual void Initialize(HTTPParams &http_params) = 0;
	//! Additional provider-specific compatibility check for managed reuse; defaults to false.
	//! Must account for connection-affecting state not represented by HTTPTransportConfig.
	DUCKDB_API virtual bool CanReuse(const HTTPParams &http_params) const;

	virtual unique_ptr<HTTPResponse> Get(GetRequestInfo &info) = 0;
	virtual unique_ptr<HTTPResponse> Put(PutRequestInfo &info) = 0;
	virtual unique_ptr<HTTPResponse> Head(HeadRequestInfo &info) = 0;
	virtual unique_ptr<HTTPResponse> Delete(DeleteRequestInfo &info) = 0;
	virtual unique_ptr<HTTPResponse> Post(PostRequestInfo &info) = 0;
	virtual unique_ptr<HTTPResponse> Options(OptionsRequestInfo &info) = 0;
	//! Remove request-local credentials, headers, callbacks and borrowed state before pooling.
	//! Throw if cleanup cannot make the client reusable; the manager will destroy it.
	virtual void Cleanup() {};

	unique_ptr<HTTPResponse> Request(BaseRequest &request);

	const string &GetBaseUrl() const {
		return base_url;
	}

private:
	//! The base URL (scheme + host + port) this client was created for
	const string base_url;
};

enum class HTTPTransportReusePolicy : uint8_t {
	//! Requests do not use an HTTPClient and do not consume manager capacity.
	CLIENT_FREE,
	//! Each request uses a bounded client that is destroyed after the request.
	EPHEMERAL,
	//! Compatible clients may be reused only by their creating session.
	SESSION_LOCAL,
	//! Compatible clients may be reused across sessions in one DatabaseInstance.
	SHARED
};

class HTTPUtil {
public:
	HTTPUtil();
	virtual ~HTTPUtil() = default;
	// disable copy constructors
	HTTPUtil(const HTTPUtil &other) = delete;
	HTTPUtil &operator=(const HTTPUtil &) = delete;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}

public:
	static HTTPUtil &Get(DatabaseInstance &db);

	virtual string GetName() const;
	//! Defaults to EPHEMERAL. Reuse requires the CanReuse, Initialize and Cleanup contracts.
	DUCKDB_API virtual HTTPTransportReusePolicy GetTransportReusePolicy() const;

	virtual unique_ptr<HTTPParams> InitializeParameters(DatabaseInstance &db, const string &path);
	virtual unique_ptr<HTTPParams> InitializeParameters(ClientContext &context, const string &path);
	virtual unique_ptr<HTTPParams> InitializeParameters(optional_ptr<FileOpener> opener,
	                                                    optional_ptr<FileOpenerInfo> info);

	virtual unique_ptr<HTTPClient> InitializeClient(HTTPParams &http_params, const string &proto_host_port);
	DUCKDB_API virtual unique_ptr<HTTPClient> InitializeClientExtended(HTTPParams &http_params,
	                                                                   const string &proto_host_port,
	                                                                   const HTTPClientInitializationOptions &options);

	//! Close a client — implementations may cache it for reuse
	virtual void CloseClient(unique_ptr<HTTPClient> &&client);

	//! Perform a checked request that throws when the provider returns no response.
	unique_ptr<HTTPResponse> Request(BaseRequest &request);
	unique_ptr<HTTPResponse> Request(BaseRequest &request, unique_ptr<HTTPClient> &client);

	//! Advanced provider hook used by the checked Request wrappers.
	virtual unique_ptr<HTTPResponse> SendRequest(BaseRequest &request, unique_ptr<HTTPClient> &client);
	virtual void LogRequest(BaseRequest &request, optional_ptr<HTTPResponse> response);

	//! Whether a failed request should be retried. Judges received responses and statuses a handler turned into an
	//! exception alike; core knows only the generic statuses (IsRetryableStatus), a provider adds what its service
	//! says in the body (e.g. an S3 error document whose code is retryable behind a final-looking status).
	DUCKDB_API virtual bool ShouldRetry(const BaseRequest &request, const HTTPResponse &response);
	//! Runs before every attempt with the 1-based attempt number and the configured maximum, e.g. to tag headers.
	DUCKDB_API virtual void PrepareAttempt(BaseRequest &request, idx_t attempt, idx_t max_attempts);
	//! Retry quota shared by all requests of this provider against one host.
	DUCKDB_API shared_ptr<HTTPRetryQuota> GetRetryQuota(const string &proto_host_port);
	//! Whether a received status is worth retrying: 408, 418, 429 and the server errors that can clear up.
	DUCKDB_API static bool IsRetryableStatus(HTTPStatusCode status);
	//! Hosts remembered for retry quotas; past this the host unused for longest makes room.
	static constexpr idx_t MAX_RETRY_QUOTAS = 1024;

	//! Whether replaying this request is safe. POST is the only method we cannot assume is idempotent.
	DUCKDB_API static bool IsIdempotent(RequestType type);

	static void ParseHTTPProxyHost(string &proxy_value, string &hostname_out, idx_t &port_out, idx_t default_port = 80);
	static void DecomposeURL(const string &url, string &path_out, string &proto_host_port_out);
	static HTTPStatusCode ToStatusCode(int32_t status_code);
	static string GetStatusMessage(HTTPStatusCode status);
	static bool IsHTTPProtocol(const string &url);
	static void BumpToSecureProtocol(string &url);
	static string CreateSignatureV4(EncryptionUtil &encryption_util, const SignatureV4Params &sig_params);

private:
	unique_ptr<HTTPClient> InitializeClientWithPolicy(BaseRequest &request, HTTPClientCachePolicy cache_policy);
	unique_ptr<HTTPResponse> SendRequestOnce(BaseRequest &request, HTTPClient &client,
	                                         HTTPClientCachePolicy initial_cache_policy,
	                                         HTTPClientCachePolicy &retry_cache_policy);

private:
	//! Protects retry_quotas.
	mutex retry_quota_lock;
	//! Per-host retry quotas, created on first use; budgets keep theirs alive across an eviction.
	unordered_map<string, shared_ptr<HTTPRetryQuota>> retry_quotas;

public:
	static duckdb::unique_ptr<HTTPResponse>
	RunRequestWithRetry(const std::function<unique_ptr<HTTPResponse>(void)> &on_request, BaseRequest &request,
	                    const std::function<void(void)> &retry_cb);
};
} // namespace duckdb
