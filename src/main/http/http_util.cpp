#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/hash_functions.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_file_opener.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/main/http/http_retry_budget.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

HTTPParams::~HTTPParams() {
}

bool HTTPParams::ShouldRedactLogs() const {
	return !db || Settings::Get<RedactHttpLogsSetting>(*db);
}

idx_t HTTPParams::GetTransportReuseDomain() const {
	return transport_reuse_domain;
}

void HTTPParams::SetTransportReuseDomain(idx_t transport_reuse_domain_p) {
	transport_reuse_domain = transport_reuse_domain_p;
}

HTTPHeaders::HTTPHeaders(DatabaseInstance &db) {
	headers.insert({"User-Agent", StringUtil::Format("%s %s", db.config.UserAgent(), DuckDB::SourceID())});
}

HTTPHeaders::~HTTPHeaders() = default;

void HTTPHeaders::Insert(string key, string value) {
	headers.insert(make_pair(std::move(key), std::move(value)));
}

void HTTPHeaders::Append(string key, string value) {
	auto entry = headers.find(key);
	if (entry == headers.end()) {
		headers.insert(make_pair(std::move(key), std::move(value)));
		return;
	}
	auto repeated_entry = repeated_headers.find(key);
	if (repeated_entry == repeated_headers.end()) {
		repeated_headers.insert(make_pair(std::move(key), header_values_t {entry->second, std::move(value)}));
	} else {
		repeated_entry->second.push_back(std::move(value));
	}
}

bool HTTPHeaders::HasHeader(const string &key) const {
	return headers.find(key) != headers.end();
}

string HTTPHeaders::GetHeaderValue(const string &key) const {
	auto entry = headers.find(key);
	if (entry == headers.end()) {
		throw InternalException("Header value not found");
	}
	return entry->second;
}

HTTPHeaders::header_values_t HTTPHeaders::GetHeaderValues(const string &key) const {
	auto repeated_entry = repeated_headers.find(key);
	if (repeated_entry != repeated_headers.end()) {
		return repeated_entry->second;
	}
	return {GetHeaderValue(key)};
}

HTTPResponse::HTTPResponse(HTTPStatusCode code) : status(code) {
}

HTTPResponse::~HTTPResponse() = default;

bool HTTPResponse::HasHeader(const string &key) const {
	return headers.HasHeader(key);
}

string HTTPResponse::GetHeaderValue(const string &key) const {
	return headers.GetHeaderValue(key);
}

bool HTTPResponse::Success() const {
	return success;
}

bool HTTPResponse::HasRequestError() const {
	return !request_error.empty();
}

const string &HTTPResponse::GetRequestError() const {
	return request_error;
}

const string &HTTPResponse::GetError() const {
	return request_error.empty() ? reason : request_error;
}

HTTPUtil::HTTPUtil() {
}

HTTPUtil &HTTPUtil::Get(DatabaseInstance &db) {
	return db.config.GetHTTPUtil();
}

string HTTPUtil::GetName() const {
	return "none";
}

HTTPTransportReusePolicy HTTPUtil::GetTransportReusePolicy() const {
	return HTTPTransportReusePolicy::EPHEMERAL;
}

bool HTTPResponse::ShouldRetry() const {
	if (HasRequestError()) {
		// request errors are eligible for retry
		return true;
	}
	// Core judges by status only; what a service says in the body is the provider's call (HTTPUtil::ShouldRetry).
	return HTTPUtil::IsRetryableStatus(status);
}

bool HTTPUtil::IsRetryableStatus(HTTPStatusCode status) {
	switch (status) {
	case HTTPStatusCode::RequestTimeout_408:
	case HTTPStatusCode::ImATeapot_418:
	case HTTPStatusCode::TooManyRequests_429:
		return true;
	// Server errors that describe the request or the server's abilities, not a passing condition.
	case HTTPStatusCode::NotImplemented_501:
	case HTTPStatusCode::HttpVersionNotSupported_505:
	case HTTPStatusCode::NetworkAuthenticationRequired_511:
		return false;
	default: {
		// Any other server error is eligible, as in the AWS SDKs.
		const auto code = static_cast<uint16_t>(status);
		return code >= 500 && code < 600;
	}
	}
}

bool HTTPUtil::IsIdempotent(RequestType type) {
	return type != RequestType::POST_REQUEST;
}

bool HTTPUtil::ShouldRetry(const BaseRequest &request, const HTTPResponse &response) {
	return response.ShouldRetry();
}

void HTTPUtil::PrepareAttempt(BaseRequest &request, idx_t attempt, idx_t max_attempts) {
	// default: requests are sent as-is
}

shared_ptr<HTTPRetryQuota> HTTPUtil::GetRetryQuota(const string &proto_host_port) {
	lock_guard<mutex> guard(retry_quota_lock);
	auto entry = retry_quotas.find(proto_host_port);
	if (entry != retry_quotas.end()) {
		return entry->second;
	}
	if (retry_quotas.size() >= MAX_RETRY_QUOTAS) {
		// Forget the host that has been quiet for longest; an idle quota would have been reset anyway.
		const auto now_ms = TimePoint::GetTickMs();
		auto victim = retry_quotas.begin();
		for (auto it = retry_quotas.begin(); it != retry_quotas.end(); ++it) {
			if (it->second->IdleMs(now_ms) > victim->second->IdleMs(now_ms)) {
				victim = it;
			}
		}
		retry_quotas.erase(victim);
	}
	auto quota = make_shared_ptr<HTTPRetryQuota>();
	retry_quotas.emplace(proto_host_port, quota);
	return quota;
}

unique_ptr<HTTPResponse> HTTPUtil::Request(BaseRequest &request) {
	unique_ptr<HTTPClient> client;
	return Request(request, client);
}

unique_ptr<HTTPResponse> HTTPUtil::Request(BaseRequest &request, unique_ptr<HTTPClient> &client) {
	if (request.params.db && !request.params.skip_external_access_check) {
		// HTTP requests access an external resource - check this is allowed by the file access settings. This is the
		// central check for requests that do not go through the file system (e.g. the REST catalog calls of iceberg).
		auto &config = request.params.db->config;
		auto &access_path = request.params.access_path.empty() ? request.url : request.params.access_path;
		if (!config.CanAccessFile(access_path, FileType::FILE_TYPE_REGULAR)) {
			throw PermissionException("Cannot access \"%s\" - file system operations are disabled by configuration",
			                          access_path);
		}
	}
	auto response = SendRequest(request, client);
	if (!response) {
		auto method = EnumUtil::ToString(request.type);
		throw IOException("HTTP provider returned no response for HTTP %s to '%s'", method, request.url);
	}
	return response;
}

BaseRequest::BaseRequest(RequestType type, const string &url, const HTTPHeaders &headers, HTTPParams &params)
    : type(type), url(url), headers(MergeHeaders(headers, params)), params(params) {
	HTTPUtil::DecomposeURL(url, path, proto_host_port);
}

// Out-of-line destructors: force the symbols to be emitted and exported from the main WASM module so
// loadable side-module extensions (e.g. the aws/httpfs extensions) can link against them at load time.
BaseRequest::~BaseRequest() = default;
GetRequestInfo::~GetRequestInfo() = default;
PutRequestInfo::~PutRequestInfo() = default;
HeadRequestInfo::~HeadRequestInfo() = default;
DeleteRequestInfo::~DeleteRequestInfo() = default;
PostRequestInfo::~PostRequestInfo() = default;

unique_ptr<HTTPClient> HTTPUtil::InitializeClient(HTTPParams &http_params, const string &proto_host_port) {
	return nullptr;
}

unique_ptr<HTTPClient> HTTPUtil::InitializeClientExtended(HTTPParams &http_params, const string &proto_host_port,
                                                          const HTTPClientInitializationOptions &) {
	return InitializeClient(http_params, proto_host_port);
}

void HTTPUtil::CloseClient(unique_ptr<HTTPClient> &&) {
	// default: no-op, client is destroyed
}

unique_ptr<HTTPClient> HTTPUtil::InitializeClientWithPolicy(BaseRequest &request, HTTPClientCachePolicy cache_policy) {
	HTTPClientInitializationOptions options;
	options.cache_policy = cache_policy;
	return InitializeClientExtended(request.params, request.proto_host_port, options);
}

unique_ptr<HTTPResponse> HTTPUtil::SendRequestOnce(BaseRequest &request, HTTPClient &client,
                                                   HTTPClientCachePolicy initial_cache_policy,
                                                   HTTPClientCachePolicy &retry_cache_policy) {
	retry_cache_policy = initial_cache_policy;

	// When logging is enabled, we collect request timings
	if (request.params.logger) {
		request.have_request_timing = request.params.logger->ShouldLog(HTTPLogType::NAME, HTTPLogType::LEVEL);
	}

	unique_ptr<HTTPResponse> response;
	try {
		request.request_system_start = Timestamp::GetCurrentTimestamp();
		request.request_monotonic_start = TimePoint::Tick();
		response = client.Request(request);
	} catch (...) {
		retry_cache_policy = HTTPClientCachePolicy::BYPASS_CACHE;
		request.request_monotonic_end = TimePoint::Tick();
		LogRequest(request, nullptr);
		throw;
	}
	if (!response || response->HasRequestError()) {
		retry_cache_policy = HTTPClientCachePolicy::BYPASS_CACHE;
	}
	request.request_monotonic_end = TimePoint::Tick();
	LogRequest(request, response ? response.get() : nullptr);
	return response;
}

unique_ptr<HTTPResponse> HTTPUtil::SendRequest(BaseRequest &request, unique_ptr<HTTPClient> &client) {
	const auto initial_cache_policy =
	    request.params.transport_manager ? HTTPClientCachePolicy::BYPASS_CACHE : HTTPClientCachePolicy::DEFAULT;
	if (!client) {
		if (initial_cache_policy == HTTPClientCachePolicy::BYPASS_CACHE) {
			client = InitializeClientWithPolicy(request, initial_cache_policy);
		} else {
			client = InitializeClient(request.params, request.proto_host_port);
		}
		if (!client) {
			throw InvalidConfigurationException(
			    "HTTP provider '%s' has no HTTP client for '%s', no HTTP request can be performed. Load an extension "
			    "that provides one (such as httpfs), or build with ENABLE_BUILTIN_HTTPLIB=ON",
			    GetName(), request.url);
		}
	}

	auto retry_cache_policy = initial_cache_policy;
	return RunRequestWithRetry(
	    [&]() { return SendRequestOnce(request, *client, initial_cache_policy, retry_cache_policy); }, request,
	    [&]() {
		    client.reset();
		    client = InitializeClientWithPolicy(request, retry_cache_policy);
		    if (!client) {
			    throw InvalidConfigurationException("HTTP provider returned a null client during retry");
		    }
	    });
}

void HTTPUtil::LogRequest(BaseRequest &request, optional_ptr<HTTPResponse> response) {
	if (!request.params.logger || !request.params.logger->ShouldLog(HTTPLogType::NAME, HTTPLogType::LEVEL)) {
		return;
	}
	auto log_string = HTTPLogType::ConstructLogMessage(request, response, request.params.ShouldRedactLogs());
	request.params.logger->WriteLog(HTTPLogType::NAME, HTTPLogType::LEVEL, log_string);
}

void HTTPUtil::ParseHTTPProxyHost(string &proxy_value, string &hostname_out, idx_t &port_out, idx_t default_port) {
	auto sanitized_proxy_value = proxy_value;
	if (StringUtil::StartsWith(proxy_value, "http://")) {
		sanitized_proxy_value = proxy_value.substr(7);
	}
	auto proxy_split = StringUtil::Split(sanitized_proxy_value, ":");
	if (proxy_split.size() == 1) {
		hostname_out = proxy_split[0];
		port_out = default_port;
	} else if (proxy_split.size() == 2) {
		idx_t port;
		if (!TryCast::Operation<string_t, idx_t>(proxy_split[1], port, false)) {
			throw InvalidInputException("Failed to parse port from http_proxy '%s'", proxy_value);
		}
		hostname_out = proxy_split[0];
		port_out = port;
	} else {
		throw InvalidInputException("Failed to parse http_proxy '%s' into a host and port", proxy_value);
	}
}

namespace {

enum class URISchemeType { HTTP, HTTPS, NONE, OTHER };

struct URISchemeDetectionResult {
	string lower_scheme;
	URISchemeType scheme_type = URISchemeType::NONE;
};

bool IsValidSchemeChar(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '.' || c == '-';
}

//! See https://datatracker.ietf.org/doc/html/rfc3986#section-3.1
URISchemeDetectionResult DetectURIScheme(const string &uri) {
	URISchemeDetectionResult result;
	auto colon_pos = uri.find(':');

	// No colon or it's before any non-scheme content
	if (colon_pos == string::npos || colon_pos == 0) {
		result.lower_scheme = "";
		result.scheme_type = URISchemeType::NONE;
		return result;
	}

	if (!std::isalpha(static_cast<unsigned char>(uri[0]))) {
		//! Scheme names consist of a sequence of characters beginning with a letter
		result.lower_scheme = "";
		result.scheme_type = URISchemeType::NONE;
		return result;
	}

	// Validate scheme characters
	for (size_t i = 1; i < colon_pos; ++i) {
		if (!IsValidSchemeChar(uri[i])) {
			//! Scheme can't contain this character, assume the URI has no scheme
			result.lower_scheme = "";
			result.scheme_type = URISchemeType::NONE;
			return result;
		}
	}

	string scheme = uri.substr(0, colon_pos);
	result.lower_scheme = StringUtil::Lower(scheme);

	if (result.lower_scheme == "http") {
		result.scheme_type = URISchemeType::HTTP;
		return result;
	}
	if (result.lower_scheme == "https") {
		result.scheme_type = URISchemeType::HTTPS;
		return result;
	}
	result.scheme_type = URISchemeType::OTHER;
	return result;
}

} // namespace

void HTTPUtil::DecomposeURL(const string &input, string &path_out, string &proto_host_port_out) {
	auto detection_result = DetectURIScheme(input);
	auto url = input;
	if (detection_result.scheme_type == URISchemeType::NONE) {
		//! Assume it's HTTP
		url = "http://" + url;
	}

	auto slash_pos = url.find('/', 8);
	if (slash_pos == string::npos) {
		throw IOException("URL needs to contain a '/' after the host");
	}
	proto_host_port_out = url.substr(0, slash_pos);

	path_out = url.substr(slash_pos);

	if (path_out.empty()) {
		throw IOException("URL needs to contain a path");
	}
}

// Handlers turn error statuses into exceptions; rebuild the response they saw so it can be judged like a received one.
static unique_ptr<HTTPResponse> ResponseFromError(const ErrorData &error) {
	auto &extra_info = error.ExtraInfo();
	auto status_entry = extra_info.find("status_code");
	int32_t status_code = 0;
	if (status_entry == extra_info.end() ||
	    !TryCast::Operation<string_t, int32_t>(string_t(status_entry->second), status_code)) {
		return nullptr;
	}
	auto response = make_uniq<HTTPResponse>(HTTPUtil::ToStatusCode(status_code));
	for (auto &entry : extra_info) {
		if (entry.first == "response_body") {
			response->body = entry.second;
		} else if (entry.first == "reason") {
			response->reason = entry.second;
		} else if (StringUtil::StartsWith(entry.first, "header_")) {
			response->headers.Insert(entry.first.substr(7), entry.second);
		}
	}
	return response;
}

// Retry eligible requests using an operation-local budget, the host's retry quota and the transport retry hook.
// This loop is not the operation's coordinator: it judges statuses (with the provider's ShouldRetry), returns Finish()
// once it stops and records what the status says. A coordinator-owned budget shared by a provider loop
// (request.retry_budget) ignores that record and settles the quota from the provider's validated outcome; the local
// status-owned budget settles it from the status.
duckdb::unique_ptr<HTTPResponse>
HTTPUtil::RunRequestWithRetry(const std::function<unique_ptr<HTTPResponse>(void)> &on_request, BaseRequest &request,
                              const std::function<void(void)> &retry_cb) {
	auto &params = request.params;
	HTTPRetryBudget local_retry_budget(params);
	auto retry_budget = request.retry_budget;
	if (!retry_budget) {
		retry_budget = local_retry_budget;
	}
	// Charge this request's host, even when a shared budget was bound to another host before.
	retry_budget->BindQuota(params.http_util.GetRetryQuota(request.proto_host_port));

	unique_ptr<HTTPResponse> response;
	std::exception_ptr caught_e;
	ErrorData caught_error;
	string exception_error;
	bool should_retry = false;
	retry_budget->Run(
	    [&]() {
		    response.reset();
		    caught_e = nullptr;
		    caught_error = ErrorData();
		    exception_error.clear();
		    unique_ptr<HTTPResponse> caught_response;
		    // Attempts are numbered per operation, so a re-send after a correction counts as the attempt it is; the
		    // bound grows with such re-sends, which do not consume retries.
		    const auto attempt = retry_budget->BeginAttempt();
		    params.http_util.PrepareAttempt(request, attempt, retry_budget->MaxAttempts());
		    try {
			    response = on_request();
			    if (response) {
				    response->url = request.url;
			    }
		    } catch (IOException &e) {
			    exception_error = e.what();
			    caught_e = std::current_exception();
			    caught_error = ErrorData(e);
		    } catch (HTTPException &e) {
			    exception_error = e.what();
			    caught_e = std::current_exception();
			    caught_error = ErrorData(e);
			    caught_response = ResponseFromError(caught_error);
		    }

		    if (response) {
			    should_retry = params.http_util.ShouldRetry(request, *response);
		    } else if (caught_response) {
			    // A thrown status is judged like a received one, provider rules included.
			    should_retry = params.http_util.ShouldRetry(request, *caught_response);
		    } else {
			    // Without a status the transport failed and nothing is known about the server.
			    should_retry = true;
		    }
		    if (!should_retry) {
			    if (response) {
				    auto response_code = static_cast<uint16_t>(response->status);
				    response->success = (response_code >= 200 && response_code < 300) ||
				                        response->status == HTTPStatusCode::NotModified_304;
			    }
			    return HTTPRetryDecision::Finish();
		    }
		    if (!HTTPUtil::IsIdempotent(request.type)) {
			    return HTTPRetryDecision::Finish();
		    }
		    if (!response && !caught_response) {
			    // The transport failed before any status arrived: nothing is known about the server.
			    return HTTPRetryDecision::Transient();
		    }
		    auto &judged = response ? *response : *caught_response;
		    if (judged.HasRequestError()) {
			    return HTTPRetryDecision::Transient();
		    }
		    if (judged.status == HTTPStatusCode::TooManyRequests_429 ||
		        judged.status == HTTPStatusCode::ServiceUnavailable_503) {
			    return HTTPRetryDecision::Throttled(
			        judged.headers.HasHeader("Retry-After") ? judged.headers.GetHeaderValue("Retry-After") : string());
		    }
		    return HTTPRetryDecision::Retry();
	    },
	    retry_cb);

	// With a coordinator-owned budget the outcome is settled by the coordinator once it has validated the response:
	// an error document behind HTTP 200 is a failure or a retry there and must not refund the retry it consumed.
	if (!should_retry && response) {
		retry_budget->RecordStatusOutcome(response->success);
		return response;
	}
	// The request failed: it threw, could not be retried any further, or is not replay-safe.
	retry_budget->RecordStatusOutcome(false);
	string quota_note;
	if (retry_budget->QuotaExhausted()) {
		quota_note = StringUtil::Format(" (retry quota for '%s' exhausted: requests to this host keep failing)",
		                                request.proto_host_port);
	}
	if (request.try_request) {
		if (!response) {
			response = make_uniq<HTTPResponse>(HTTPStatusCode::INVALID);
			response->request_error =
			    (exception_error.empty() ? "Unknown error" : std::move(exception_error)) + quota_note;
		} else if (!quota_note.empty()) {
			// A received error keeps its status; the note goes to the field GetError() reports.
			if (response->HasRequestError()) {
				response->request_error += quota_note;
			} else {
				response->reason += quota_note;
			}
		}
		response->success = false;
		return response;
	}
	auto method = EnumUtil::ToString(request.type);
	if (caught_e) {
		if (quota_note.empty()) {
			std::rethrow_exception(caught_e);
		}
		// Same type and extra info as the caught exception, with the quota note appended.
		if (caught_error.Type() == ExceptionType::HTTP) {
			throw HTTPException(caught_error.ExtraInfo(), caught_error.RawMessage() + quota_note);
		}
		throw IOException(caught_error.ExtraInfo(), caught_error.RawMessage() + quota_note);
	}
	if (response && !response->HasRequestError()) {
		throw HTTPException(*response, "Request returned HTTP %d for HTTP %s to '%s'%s",
		                    static_cast<int>(response->status), method, request.url, quota_note);
	}
	string error = response ? response->GetError() : "Unknown error";
	throw IOException("%s error for HTTP %s to '%s'%s", error, method, request.url, quota_note);
}

void HTTPParams::Initialize(optional_ptr<FileOpener> opener) {
	db = FileOpener::TryGetDatabase(opener);
	if (db) {
		auto &http_proxy_setting = db->config.options.http_proxy;
		if (!http_proxy_setting.empty()) {
			idx_t port;
			string host;
			HTTPUtil::ParseHTTPProxyHost(http_proxy_setting, host, port);
			http_proxy = host;
			http_proxy_port = port;
		}
		http_proxy_username = Settings::Get<HTTPProxyUsernameSetting>(*db);
		http_proxy_password = Settings::Get<HTTPProxyPasswordSetting>(*db);
	}

	auto client_context = FileOpener::TryGetClientContext(opener);
	if (client_context) {
		logger = client_context->logger;
	}
}

unique_ptr<HTTPParams> HTTPUtil::InitializeParameters(DatabaseInstance &db, const string &url) {
	DatabaseFileOpener opener(db);
	FileOpenerInfo info;
	info.file_path = url;
	return InitializeParameters(&opener, &info);
}

unique_ptr<HTTPParams> HTTPUtil::InitializeParameters(ClientContext &context, const string &url) {
	ClientContextFileOpener opener(context);
	FileOpenerInfo info;
	info.file_path = url;
	return InitializeParameters(&opener, &info);
}

unique_ptr<HTTPParams> HTTPUtil::InitializeParameters(optional_ptr<FileOpener> opener,
                                                      optional_ptr<FileOpenerInfo> info) {
	auto result = make_uniq<HTTPParams>(*this);
	result->Initialize(opener);
	return result;
}

unique_ptr<HTTPResponse> HTTPClient::Request(BaseRequest &request) {
	switch (request.type) {
	case RequestType::GET_REQUEST:
		return Get(request.Cast<GetRequestInfo>());
	case RequestType::PUT_REQUEST:
		return Put(request.Cast<PutRequestInfo>());
	case RequestType::HEAD_REQUEST:
		return Head(request.Cast<HeadRequestInfo>());
	case RequestType::DELETE_REQUEST:
		return Delete(request.Cast<DeleteRequestInfo>());
	case RequestType::POST_REQUEST:
		return Post(request.Cast<PostRequestInfo>());
	case RequestType::OPTIONS_REQUEST:
		return Options(request.Cast<OptionsRequestInfo>());
	default:
		throw InternalException("Unsupported request type");
	}
}

bool HTTPClient::CanReuse(const HTTPParams &) const {
	return false;
}

bool HTTPUtil::IsHTTPProtocol(const string &url) {
	return StringUtil::StartsWith(url, "http://");
}
void HTTPUtil::BumpToSecureProtocol(string &url) {
	if (IsHTTPProtocol(url)) {
		url = "https://" + url.substr(7);
	}
}

string HTTPUtil::CreateSignatureV4(EncryptionUtil &encryption_util, const SignatureV4Params &sig_params) {
	hash_bytes canonical_request_hash;
	hash_str canonical_request_hash_str;
	sha256(encryption_util, const_data_ptr_cast(sig_params.canonical_request.data()),
	       sig_params.canonical_request.length(), canonical_request_hash);
	hex256(canonical_request_hash, canonical_request_hash_str);

	auto string_to_sign = "AWS4-HMAC-SHA256\n" + sig_params.datetime_now + "\n" + sig_params.credential_scope + "\n" +
	                      string(const_char_ptr_cast(canonical_request_hash_str), sizeof(hash_str));
	hash_bytes k_date, k_region, k_service, signing_key, signature;
	auto sign_key = "AWS4" + sig_params.secret_access_key;
	hmac256(encryption_util, sig_params.date_now, const_data_ptr_cast(sign_key.data()), sign_key.length(), k_date);
	hmac256(encryption_util, sig_params.region, k_date, k_region);
	hmac256(encryption_util, sig_params.service, k_region, k_service);
	hmac256(encryption_util, "aws4_request", k_service, signing_key);
	hmac256(encryption_util, string_to_sign, signing_key, signature);

	hash_str signature_str;
	hex256(signature, signature_str);
	return string(const_char_ptr_cast(signature_str), sizeof(hash_str));
}

} // namespace duckdb
