#include "catch.hpp"

#include "duckdb/main/http/http_retry_budget.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/string_util.hpp"

using namespace duckdb;

TEST_CASE("HTTP retry back-off uses full jitter with a cap", "[http]") {
	// Retry 1 sleeps anywhere in [0, wait]
	REQUIRE(HTTPRetryBudget::BackoffMs(100, 4, 1, 0, 0) == 0);
	REQUIRE(HTTPRetryBudget::BackoffMs(100, 4, 1, 0, 100) == 100);
	REQUIRE(HTTPRetryBudget::BackoffMs(100, 4, 1, 0, 101) == 0);
	// Retry 3 grows the window to wait * backoff^2
	REQUIRE(HTTPRetryBudget::BackoffMs(100, 4, 3, 0, 1600) == 1600);
	REQUIRE(HTTPRetryBudget::BackoffMs(100, 4, 3, 0, 1601) == 0);
	// The window never exceeds the cap
	REQUIRE(HTTPRetryBudget::BackoffMs(1000, 2, 40, 0, HTTPRetryBudget::MAX_BACKOFF_MS) ==
	        HTTPRetryBudget::MAX_BACKOFF_MS);
	REQUIRE(HTTPRetryBudget::BackoffMs(1000, 2, 40, 0, HTTPRetryBudget::MAX_BACKOFF_MS + 1) == 0);
	// A Retry-After floor overrides a small jitter and is capped as well
	REQUIRE(HTTPRetryBudget::BackoffMs(100, 4, 1, 3000, 0) == 3000);
	REQUIRE(HTTPRetryBudget::BackoffMs(100, 4, 1, 60000, 0) == HTTPRetryBudget::MAX_BACKOFF_MS);
	// A zero wait never sleeps
	REQUIRE(HTTPRetryBudget::BackoffMs(0, 4, 5, 0, 12345) == 0);
}

TEST_CASE("Throttled HTTP retries keep at least half the back-off window", "[http]") {
	// Retry 1 against a throttling server sleeps in [wait / 2, wait]
	REQUIRE(HTTPRetryBudget::BackoffMs(100, 4, 1, 0, 0, true) == 50);
	REQUIRE(HTTPRetryBudget::BackoffMs(100, 4, 1, 0, 50, true) == 100);
	REQUIRE(HTTPRetryBudget::BackoffMs(100, 4, 1, 0, 51, true) == 50);
	// Retry 3 keeps the proportion
	REQUIRE(HTTPRetryBudget::BackoffMs(100, 4, 3, 0, 0, true) == 800);
	REQUIRE(HTTPRetryBudget::BackoffMs(100, 4, 3, 0, 800, true) == 1600);
	// The cap applies to the whole window
	REQUIRE(HTTPRetryBudget::BackoffMs(1000, 2, 40, 0, 0, true) == HTTPRetryBudget::MAX_BACKOFF_MS / 2);
	// Retry-After still raises the floor
	REQUIRE(HTTPRetryBudget::BackoffMs(100, 4, 1, 3000, 0, true) == 3000);
	// A zero wait never sleeps, throttled or not
	REQUIRE(HTTPRetryBudget::BackoffMs(0, 4, 5, 0, 12345, true) == 0);
}

TEST_CASE("Retry-After header values parse as seconds or HTTP-dates", "[http]") {
	uint64_t ms = 0;
	REQUIRE(HTTPRetryBudget::TryParseRetryAfterMs("3", ms));
	REQUIRE(ms == 3000);
	REQUIRE(HTTPRetryBudget::TryParseRetryAfterMs(" 7 ", ms));
	REQUIRE(ms == 7000);
	REQUIRE(HTTPRetryBudget::TryParseRetryAfterMs("3600", ms));
	REQUIRE(ms == HTTPRetryBudget::MAX_BACKOFF_MS);
	REQUIRE_FALSE(HTTPRetryBudget::TryParseRetryAfterMs("", ms));
	REQUIRE_FALSE(HTTPRetryBudget::TryParseRetryAfterMs("soon", ms));
	REQUIRE_FALSE(HTTPRetryBudget::TryParseRetryAfterMs("-5", ms));
	// A date in the past asks for no delay
	REQUIRE(HTTPRetryBudget::TryParseRetryAfterMs("Sun, 06 Nov 1994 08:49:37 GMT", ms));
	REQUIRE(ms == 0);
	// A date in the future asks for a positive delay
	REQUIRE(HTTPRetryBudget::TryParseRetryAfterMs("Fri, 31 Dec 2999 23:59:59 GMT", ms));
	REQUIRE(ms > 0);
}

TEST_CASE("Retryable HTTP statuses are the transient ones", "[http]") {
	REQUIRE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::RequestTimeout_408));
	REQUIRE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::TooManyRequests_429));
	REQUIRE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::InternalServerError_500));
	REQUIRE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::BadGateway_502));
	REQUIRE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::ServiceUnavailable_503));
	REQUIRE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::GatewayTimeout_504));
	REQUIRE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::InsufficientStorage_507));
	// Server errors about the request or the server's abilities never clear up on their own
	REQUIRE_FALSE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::NotImplemented_501));
	REQUIRE_FALSE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::HttpVersionNotSupported_505));
	REQUIRE_FALSE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::NetworkAuthenticationRequired_511));
	// Client errors other than the timeouts and throttles are final
	REQUIRE_FALSE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::Forbidden_403));
	REQUIRE_FALSE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::NotFound_404));
	REQUIRE_FALSE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::OK_200));
	REQUIRE_FALSE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::INVALID));
}

TEST_CASE("HTTP retry quota charges retries and refunds successes", "[http]") {
	HTTPRetryQuota quota;
	REQUIRE(quota.Available() == HTTPRetryQuota::INITIAL_CAPACITY);
	REQUIRE(quota.TryAcquire(HTTPRetryQuota::RETRY_COST));
	REQUIRE(quota.Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);
	// Refunds never grow the quota beyond its initial capacity
	quota.Release(HTTPRetryQuota::INITIAL_CAPACITY);
	REQUIRE(quota.Available() == HTTPRetryQuota::INITIAL_CAPACITY);
	REQUIRE_FALSE(quota.TryAcquire(HTTPRetryQuota::INITIAL_CAPACITY + 1));
	REQUIRE(quota.Available() == HTTPRetryQuota::INITIAL_CAPACITY);
	REQUIRE(quota.TryAcquire(HTTPRetryQuota::INITIAL_CAPACITY));
	REQUIRE(quota.Available() == 0);
	REQUIRE_FALSE(quota.TryAcquire(1));
}

TEST_CASE("HTTP retry quota is full again after the host was left alone", "[http]") {
	HTTPRetryQuota quota;
	const auto now_ms = TimePoint::GetTickMs();
	REQUIRE(quota.TryAcquire(HTTPRetryQuota::INITIAL_CAPACITY, now_ms));
	REQUIRE(quota.Available() == 0);
	// Use shortly after keeps the quota empty
	REQUIRE_FALSE(quota.TryAcquire(1, now_ms + HTTPRetryQuota::IDLE_RESET_MS - 1));
	REQUIRE(quota.IdleMs(now_ms + HTTPRetryQuota::IDLE_RESET_MS - 1) == 0);
	// Once nothing has touched it for the idle period, the quota starts over
	REQUIRE(quota.TryAcquire(HTTPRetryQuota::RETRY_COST, now_ms + 2 * HTTPRetryQuota::IDLE_RESET_MS));
	REQUIRE(quota.Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);
}

TEST_CASE("HTTP retry budget stops when the host quota is exhausted", "[http]") {
	HTTPUtil http_util;
	HTTPParams params(http_util);
	params.retries = 100;
	params.retry_wait_ms = 0;
	auto quota = make_shared_ptr<HTTPRetryQuota>();
	// Leave room for exactly two server-error retries
	REQUIRE(quota->TryAcquire(HTTPRetryQuota::INITIAL_CAPACITY - 2 * HTTPRetryQuota::RETRY_COST));

	HTTPRetryBudget budget(params, HTTPRetryOutcomeOwner::COORDINATOR);
	budget.BindQuota(quota);
	idx_t attempts = 0;
	budget.Run([&]() {
		attempts++;
		return HTTPRetryDecision::Retry();
	});
	REQUIRE(attempts == 3);
	REQUIRE(budget.RetriesUsed() == 2);
	REQUIRE(budget.QuotaExhausted());
	REQUIRE(quota->Available() == 0);

	// The refused retry means the earlier ones did not get the request through: a later success in the same
	// budget refunds only the single token of a request that never retried
	budget.ReportSuccess();
	REQUIRE(quota->Available() == HTTPRetryQuota::NO_RETRY_REFUND);
	quota->Release(HTTPRetryQuota::RETRY_COST - HTTPRetryQuota::NO_RETRY_REFUND);

	// A success right after an admitted retry refunds that retry's cost
	HTTPRetryBudget refunded_budget(params, HTTPRetryOutcomeOwner::COORDINATOR);
	refunded_budget.BindQuota(quota);
	attempts = 0;
	refunded_budget.Run([&]() {
		attempts++;
		return attempts == 1 ? HTTPRetryDecision::Retry() : HTTPRetryDecision::Finish();
	});
	REQUIRE(attempts == 2);
	REQUIRE(quota->Available() == 0);
	refunded_budget.ReportSuccess();
	REQUIRE(quota->Available() == HTTPRetryQuota::RETRY_COST);

	// A run that starts afresh is not marked by an earlier refusal
	HTTPRetryBudget reused_budget(params);
	reused_budget.BindQuota(quota);
	reused_budget.Run([&]() { return HTTPRetryDecision::Retry(); });
	REQUIRE(reused_budget.QuotaExhausted());
	reused_budget.Run([&]() { return HTTPRetryDecision::Finish(); });
	REQUIRE_FALSE(reused_budget.QuotaExhausted());
	REQUIRE(quota->Available() == 0);

	// Transport failures cost more than server errors
	quota->Release(HTTPRetryQuota::RETRY_COST);
	HTTPRetryBudget transient_budget(params);
	transient_budget.BindQuota(quota);
	attempts = 0;
	transient_budget.Run([&]() {
		attempts++;
		return HTTPRetryDecision::Transient();
	});
	REQUIRE(attempts == 1);
	REQUIRE(transient_budget.QuotaExhausted());
	quota->Release(HTTPRetryQuota::TRANSIENT_RETRY_COST);
	HTTPRetryBudget transient_budget_with_room(params);
	transient_budget_with_room.BindQuota(quota);
	attempts = 0;
	transient_budget_with_room.Run([&]() {
		attempts++;
		return HTTPRetryDecision::Transient();
	});
	REQUIRE(attempts == 2);
	REQUIRE(quota->Available() == HTTPRetryQuota::RETRY_COST);

	// A request that succeeded without retrying refunds a single token
	HTTPRetryBudget clean_budget(params, HTTPRetryOutcomeOwner::COORDINATOR);
	clean_budget.BindQuota(quota);
	clean_budget.ReportSuccess();
	REQUIRE(quota->Available() == HTTPRetryQuota::RETRY_COST + HTTPRetryQuota::NO_RETRY_REFUND);

	// Throttled retries beyond http_retries are still charged
	params.retries = 1;
	HTTPRetryBudget throttled_budget(params);
	throttled_budget.BindQuota(quota);
	attempts = 0;
	throttled_budget.Run([&]() {
		attempts++;
		return HTTPRetryDecision::Throttled("0");
	});
	REQUIRE(attempts == 2);
	REQUIRE(throttled_budget.ThrottledRetries() == 1);
	REQUIRE(quota->Available() == HTTPRetryQuota::NO_RETRY_REFUND);
}

TEST_CASE("HTTP retry quota is not drained by requests that get through after being throttled", "[http]") {
	HTTPUtil http_util;
	HTTPParams params(http_util);
	params.retry_wait_ms = 0;
	auto quota = make_shared_ptr<HTTPRetryQuota>();
	// start below the capacity, so that a refund beyond what was charged would show
	REQUIRE(quota->TryAcquire(100));
	const auto available = HTTPRetryQuota::INITIAL_CAPACITY - 100;

	// as for a glob over many objects of a host that throttles each object twice (duckdb/duckdb#6153)
	for (idx_t object = 0; object < 1000; object++) {
		HTTPRetryBudget budget(params, HTTPRetryOutcomeOwner::COORDINATOR);
		budget.BindQuota(quota);
		idx_t attempts = 0;
		budget.Run([&]() {
			attempts++;
			return attempts <= 2 ? HTTPRetryDecision::Throttled("0") : HTTPRetryDecision::Finish();
		});
		REQUIRE(attempts == 3);
		REQUIRE_FALSE(budget.QuotaExhausted());
		budget.ReportSuccess();
	}
	REQUIRE(quota->Available() == available);

	// a server error before the throttle is not refunded, as for an operation that retried server errors only
	HTTPRetryBudget error_then_throttle(params, HTTPRetryOutcomeOwner::COORDINATOR);
	error_then_throttle.BindQuota(quota);
	idx_t attempts = 0;
	error_then_throttle.Run([&]() {
		attempts++;
		if (attempts == 1) {
			return HTTPRetryDecision::Retry();
		}
		return attempts == 2 ? HTTPRetryDecision::Throttled("0") : HTTPRetryDecision::Finish();
	});
	error_then_throttle.ReportSuccess();
	REQUIRE(quota->Available() == available - HTTPRetryQuota::RETRY_COST);

	// throttles before a server error are refunded with it
	HTTPRetryBudget throttle_then_error(params, HTTPRetryOutcomeOwner::COORDINATOR);
	throttle_then_error.BindQuota(quota);
	attempts = 0;
	throttle_then_error.Run([&]() {
		attempts++;
		if (attempts <= 2) {
			return HTTPRetryDecision::Throttled("0");
		}
		return attempts == 3 ? HTTPRetryDecision::Retry() : HTTPRetryDecision::Finish();
	});
	throttle_then_error.ReportSuccess();
	REQUIRE(quota->Available() == available - HTTPRetryQuota::RETRY_COST);

	// a request that keeps being throttled is not refunded, so the quota still stops a host that never lets one through
	HTTPRetryBudget throttled(params, HTTPRetryOutcomeOwner::COORDINATOR);
	throttled.BindQuota(quota);
	throttled.Run([&]() { return HTTPRetryDecision::Throttled("0"); });
	throttled.ReportFailure();
	REQUIRE(quota->Available() == available - HTTPRetryQuota::RETRY_COST -
	                                  static_cast<int64_t>(throttled.RetriesUsed()) * HTTPRetryQuota::RETRY_COST);

	// a failure after throttles refunds nothing, and a later clean run of the same budget refunds a single token
	const auto before_failure = quota->Available();
	HTTPRetryBudget failed(params, HTTPRetryOutcomeOwner::COORDINATOR);
	failed.BindQuota(quota);
	attempts = 0;
	failed.Run([&]() {
		attempts++;
		return attempts <= 2 ? HTTPRetryDecision::Throttled("0") : HTTPRetryDecision::Failure();
	});
	REQUIRE(quota->Available() == before_failure - 2 * HTTPRetryQuota::RETRY_COST);
	failed.Run([&]() { return HTTPRetryDecision::Success(); });
	REQUIRE(quota->Available() == before_failure - 2 * HTTPRetryQuota::RETRY_COST + HTTPRetryQuota::NO_RETRY_REFUND);

	// throttles charged to another host are refunded to that host
	auto other_quota = make_shared_ptr<HTTPRetryQuota>();
	HTTPRetryBudget redirected(params, HTTPRetryOutcomeOwner::COORDINATOR);
	redirected.BindQuota(other_quota);
	attempts = 0;
	redirected.Run([&]() {
		attempts++;
		if (attempts == 2) {
			redirected.BindQuota(quota);
		}
		return attempts <= 2 ? HTTPRetryDecision::Throttled("0") : HTTPRetryDecision::Finish();
	});
	redirected.ReportSuccess();
	REQUIRE(other_quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY);
}

TEST_CASE("HTTP retry budget charges the host of the request being retried", "[http]") {
	HTTPUtil http_util;
	HTTPParams params(http_util);
	params.retries = 10;
	params.retry_wait_ms = 0;
	auto first_host = make_shared_ptr<HTTPRetryQuota>();
	auto second_host = make_shared_ptr<HTTPRetryQuota>();

	// One operation: a request to the first host is retried once, then a redirect sends it to the second host,
	// where it is retried once more and succeeds
	HTTPRetryBudget budget(params, HTTPRetryOutcomeOwner::COORDINATOR);
	budget.BindQuota(first_host);
	idx_t attempts = 0;
	budget.Run([&]() {
		attempts++;
		return attempts == 1 ? HTTPRetryDecision::Retry() : HTTPRetryDecision::Finish();
	});
	REQUIRE(first_host->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);
	REQUIRE(second_host->Available() == HTTPRetryQuota::INITIAL_CAPACITY);

	budget.BindQuota(second_host);
	attempts = 0;
	budget.Run([&]() {
		attempts++;
		return attempts == 1 ? HTTPRetryDecision::Retry() : HTTPRetryDecision::Finish();
	});
	REQUIRE(first_host->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);
	REQUIRE(second_host->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);

	// The success refunds the host that was charged for the last retry
	budget.ReportSuccess();
	REQUIRE(first_host->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);
	REQUIRE(second_host->Available() == HTTPRetryQuota::INITIAL_CAPACITY);
}

TEST_CASE("HTTP retry budget grants extra throttled retries unless retries are off", "[http]") {
	HTTPUtil http_util;
	HTTPParams params(http_util);
	params.retry_wait_ms = 0;

	params.retries = 1;
	HTTPRetryBudget budget(params);
	idx_t attempts = 0;
	budget.Run([&]() {
		attempts++;
		return HTTPRetryDecision::Throttled();
	});
	REQUIRE(attempts == 1 + 1 + HTTPRetryBudget::THROTTLE_EXTRA_RETRIES);
	REQUIRE(budget.ThrottledRetries() == 1 + HTTPRetryBudget::THROTTLE_EXTRA_RETRIES);
	// Every attempt stayed within what the budget announces as its maximum
	REQUIRE(attempts == budget.MaxAttempts());

	params.retries = 0;
	HTTPRetryBudget no_retries(params);
	attempts = 0;
	no_retries.Run([&]() {
		attempts++;
		return HTTPRetryDecision::Throttled();
	});
	REQUIRE(attempts == 1);
	REQUIRE(no_retries.MaxAttempts() == 1);
}

TEST_CASE("HTTP retry budget numbers every send of the operation", "[http]") {
	HTTPUtil http_util;
	HTTPParams params(http_util);
	params.retries = 3;
	params.retry_wait_ms = 0;
	HTTPRetryBudget budget(params);
	REQUIRE(budget.MaxAttempts() == 1 + 3 + HTTPRetryBudget::THROTTLE_EXTRA_RETRIES);
	// Two sends in one inner loop, a corrected re-send without a retry in between, then one more
	REQUIRE(budget.BeginAttempt() == 1);
	REQUIRE(budget.Corrections() == 0);
	REQUIRE(budget.BeginAttempt() == 2);
	REQUIRE(budget.BeginAttempt() == 3);
	REQUIRE(budget.RetriesUsed() == 0);
	// Sends that consumed no retry are corrections and widen the advertised bound, so attempt <= max always holds
	REQUIRE(budget.Corrections() == 2);
	REQUIRE(budget.MaxAttempts() == 1 + 3 + HTTPRetryBudget::THROTTLE_EXTRA_RETRIES + 2);
	budget.Run([&]() { return HTTPRetryDecision::Finish(); });
	REQUIRE(budget.BeginAttempt() == 4);
	REQUIRE(budget.Corrections() == 3);
}

namespace {

//! Records the attempt headers a provider would tag every send with.
class AttemptRecordingHTTPUtil : public HTTPUtil {
public:
	void PrepareAttempt(BaseRequest &request, idx_t attempt, idx_t max_attempts) override {
		attempts.emplace_back(attempt, max_attempts);
	}

	vector<std::pair<idx_t, idx_t>> attempts;
};

//! A provider that reads its service's error documents: a RequestTimeout code makes a final-looking 400 retryable.
//! The classification is the provider's, in whatever XML form its parser accepts; core never looks at the body.
class ErrorDocumentHTTPUtil : public HTTPUtil {
public:
	bool ShouldRetry(const BaseRequest &request, const HTTPResponse &response) override {
		if (HTTPUtil::ShouldRetry(request, response)) {
			return true;
		}
		return response.status == HTTPStatusCode::BadRequest_400 && ErrorCode(response.body) == "RequestTimeout";
	}

	//! Error code of an S3-style error document, with or without a namespace prefix or CDATA section.
	static string ErrorCode(const string &body) {
		auto code_begin = body.find("Code>");
		if (code_begin == string::npos) {
			return string();
		}
		code_begin += 5;
		auto code_end = body.find("</", code_begin);
		if (code_end == string::npos) {
			return string();
		}
		auto code = body.substr(code_begin, code_end - code_begin);
		if (StringUtil::StartsWith(code, "<![CDATA[") && StringUtil::EndsWith(code, "]]>")) {
			code = code.substr(9, code.size() - 12);
		}
		return code;
	}
};

unique_ptr<HTTPResponse> MakeResponse(HTTPStatusCode status, const string &body = string()) {
	auto response = make_uniq<HTTPResponse>(status);
	response->body = body;
	return response;
}

const char *S3_REQUEST_TIMEOUT_BODY = "<Error><Code>RequestTimeout</Code><Message>Your socket connection to the "
                                      "server was not read from or written to within the timeout period.</Message>"
                                      "</Error>";
const char *S3_NAMESPACED_REQUEST_TIMEOUT_BODY =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><s3:Error xmlns:s3=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
    "<s3:Code><![CDATA[RequestTimeout]]></s3:Code><s3:Message>timed out</s3:Message></s3:Error>";
const char *S3_INVALID_PART_BODY = "<Error><Code>InvalidPart</Code><Message>One or more of the specified parts could "
                                   "not be found.</Message></Error>";
const char *S3_INTERNAL_ERROR_BODY = "<Error><Code>InternalError</Code><Message>We encountered an internal error. "
                                     "Please try again.</Message></Error>";

} // namespace

TEST_CASE("Core retries a final-looking status only when the provider says so", "[http]") {
	HTTPUtil core_util;
	ErrorDocumentHTTPUtil provider_util;
	HTTPHeaders headers;
	const string url = "http://bucket.s3.amazonaws.com/key";

	// Core alone judges by status: a 400 is final whatever its body says
	HTTPResponse timeout(HTTPStatusCode::BadRequest_400);
	timeout.body = S3_REQUEST_TIMEOUT_BODY;
	REQUIRE_FALSE(timeout.ShouldRetry());
	REQUIRE_FALSE(HTTPUtil::IsRetryableStatus(HTTPStatusCode::BadRequest_400));

	auto run_sends = [&](HTTPUtil &http_util, const string &body, bool thrown) {
		HTTPParams params(http_util);
		params.retries = 2;
		params.retry_wait_ms = 0;
		GetRequestInfo request(
		    url, headers, params, [](const HTTPResponse &) { return true; },
		    [](const_data_ptr_t, idx_t) { return true; });
		request.try_request = true;
		idx_t sends = 0;
		auto response = HTTPUtil::RunRequestWithRetry(
		    [&]() -> unique_ptr<HTTPResponse> {
			    sends++;
			    auto failed = MakeResponse(HTTPStatusCode::BadRequest_400, body);
			    if (thrown) {
				    // Handlers turn error statuses into exceptions before the loop sees the response
				    throw HTTPException(*failed, "HTTP GET error reading '%s' (HTTP 400 Bad Request)", url);
			    }
			    return failed;
		    },
		    request, {});
		REQUIRE_FALSE(response->Success());
		return sends;
	};

	// Without a provider classification the request fails after one send, received or thrown
	REQUIRE(run_sends(core_util, S3_REQUEST_TIMEOUT_BODY, false) == 1);
	REQUIRE(run_sends(core_util, S3_REQUEST_TIMEOUT_BODY, true) == 1);
	// The provider's ShouldRetry reads the error document, in the forms its own parser accepts
	REQUIRE(run_sends(provider_util, S3_REQUEST_TIMEOUT_BODY, false) == 3);
	REQUIRE(run_sends(provider_util, S3_REQUEST_TIMEOUT_BODY, true) == 3);
	REQUIRE(run_sends(provider_util, S3_NAMESPACED_REQUEST_TIMEOUT_BODY, true) == 3);
	// A different error code behind the same status stays final
	REQUIRE(run_sends(provider_util, S3_INVALID_PART_BODY, false) == 1);
	REQUIRE(run_sends(provider_util, S3_INVALID_PART_BODY, true) == 1);
}

TEST_CASE("The coordinator's explicit outcome settles the host quota", "[http]") {
	ErrorDocumentHTTPUtil http_util;
	HTTPParams params(http_util);
	params.retries = 3;
	params.retry_wait_ms = 0;
	HTTPHeaders headers;
	const string url = "http://bucket.s3.amazonaws.com/key?uploadId=1";
	auto quota = http_util.GetRetryQuota("http://bucket.s3.amazonaws.com");
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY);

	// The provider's loop is the operation's coordinator, as the S3 multipart completion is: core's request loop
	// shares its budget, judges statuses and returns Finish(); the coordinator inspects the body and decides.
	// `script` lists the responses of one operation: {status, body}; the last entry repeats.
	using Script = vector<std::pair<HTTPStatusCode, const char *>>;
	auto run_operation = [&](const Script &script, bool explicit_protocol) {
		// A coordinator owns the outcome from the start; a loop without the protocol leaves it to the status
		HTTPRetryBudget budget(params,
		                       explicit_protocol ? HTTPRetryOutcomeOwner::COORDINATOR : HTTPRetryOutcomeOwner::STATUS);
		idx_t sends = 0;
		unique_ptr<HTTPResponse> last;
		budget.Run([&]() {
			PostRequestInfo request(url, headers, params, nullptr, 0);
			request.retry_budget = budget;
			request.try_request = true;
			last = HTTPUtil::RunRequestWithRetry(
			    [&]() {
				    auto &scripted = script[MinValue<idx_t>(sends, script.size() - 1)];
				    sends++;
				    return MakeResponse(scripted.first, scripted.second);
			    },
			    request, {});
			if (!explicit_protocol) {
				// A loop that has not adopted the protocol: stop or retry, never judge
				return last->Success() ? HTTPRetryDecision::Finish() : HTTPRetryDecision::Retry();
			}
			// The coordinator owns the POST's replay safety, which core cannot assume: it retries the server errors
			// core left alone and the retryable error codes it reads from the body
			auto code = ErrorDocumentHTTPUtil::ErrorCode(last->body);
			if ((!last->Success() && HTTPUtil::IsRetryableStatus(last->status)) || code == "InternalError" ||
			    code == "RequestTimeout") {
				return HTTPRetryDecision::Retry();
			}
			if (!last->Success() || !code.empty()) {
				// A final status, or an error document behind HTTP 200: the operation failed for good
				return HTTPRetryDecision::Failure();
			}
			return HTTPRetryDecision::Success();
		});
		return sends;
	};
	const Script server_error_then_invalid_part {{HTTPStatusCode::InternalServerError_500, ""},
	                                             {HTTPStatusCode::OK_200, S3_INVALID_PART_BODY}};
	const Script timeout_then_ok {{HTTPStatusCode::BadRequest_400, S3_NAMESPACED_REQUEST_TIMEOUT_BODY},
	                              {HTTPStatusCode::OK_200, "<CompleteMultipartUploadResult/>"}};
	const Script ok {{HTTPStatusCode::OK_200, "<CompleteMultipartUploadResult/>"}};
	const Script internal_errors {{HTTPStatusCode::OK_200, S3_INTERNAL_ERROR_BODY}};

	// HTTP 500, then HTTP 200 with an InvalidPart error document: the coordinator retried the 500, core's loop saw
	// a 2xx and recorded a success, the coordinator rejects the body. The retry stays charged.
	REQUIRE(run_operation(server_error_then_invalid_part, true) == 2);
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);
	// A host that keeps doing this runs out of quota: 1000 such operations make 100 retries, then one send each
	for (idx_t i = 1; i < 1000; i++) {
		run_operation(server_error_then_invalid_part, true);
	}
	REQUIRE(quota->Available() == 0);
	REQUIRE(run_operation(server_error_then_invalid_part, true) == 1);

	// A 400 RequestTimeout (namespaced, CDATA) is the provider's call: its coordinator reads the code and retries
	// (core's loop would, through the provider's ShouldRetry, for a replay-safe method); the success refunds it
	quota->Release(HTTPRetryQuota::INITIAL_CAPACITY);
	REQUIRE(run_operation(timeout_then_ok, true) == 2);
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY);
	// A retryable error document behind HTTP 200 (InternalError) is the coordinator's own retry: core's loop saw a
	// 2xx each time, the coordinator keeps retrying until the budget is spent, and nothing is refunded
	quota->Release(HTTPRetryQuota::INITIAL_CAPACITY);
	REQUIRE(run_operation(internal_errors, true) == 1 + 3);
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY - 3 * HTTPRetryQuota::RETRY_COST);

	// A first send that succeeds refunds the single token of a request that never retried
	quota->Release(HTTPRetryQuota::INITIAL_CAPACITY);
	REQUIRE(quota->TryAcquire(HTTPRetryQuota::NO_RETRY_REFUND));
	REQUIRE(run_operation(ok, true) == 1);
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY);

	// Compatibility: a status-owned budget whose loop only returns Finish() leaves the judgement to the status core
	// recorded, so the 2xx that carried the error document refunds the retry. That is the pre-protocol behaviour a
	// provider keeps until it owns the outcome.
	REQUIRE(quota->TryAcquire(HTTPRetryQuota::RETRY_COST));
	REQUIRE(run_operation(server_error_then_invalid_part, false) == 2);
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);
}

TEST_CASE("An explicit outcome is settled once", "[http]") {
	HTTPUtil http_util;
	HTTPParams params(http_util);
	params.retries = 3;
	params.retry_wait_ms = 0;
	auto quota = make_shared_ptr<HTTPRetryQuota>();

	// A coordinator that validates after its Run reports the outcome itself
	HTTPRetryBudget budget(params, HTTPRetryOutcomeOwner::COORDINATOR);
	budget.BindQuota(quota);
	idx_t attempts = 0;
	budget.Run([&]() {
		attempts++;
		return attempts == 1 ? HTTPRetryDecision::Retry() : HTTPRetryDecision::Finish();
	});
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);
	budget.ReportSuccess();
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY);
	// Reporting again, either way, changes nothing
	budget.ReportSuccess();
	budget.ReportFailure();
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY);

	// A failure reported after the loop keeps the retry charged, and a later success does not refund it
	HTTPRetryBudget failed(params, HTTPRetryOutcomeOwner::COORDINATOR);
	failed.BindQuota(quota);
	attempts = 0;
	failed.Run([&]() {
		attempts++;
		return attempts == 1 ? HTTPRetryDecision::Retry() : HTTPRetryDecision::Finish();
	});
	failed.ReportFailure();
	failed.ReportSuccess();
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);

	// A Success decision inside the loop is the same report
	quota->Release(HTTPRetryQuota::INITIAL_CAPACITY);
	HTTPRetryBudget decided(params, HTTPRetryOutcomeOwner::COORDINATOR);
	decided.BindQuota(quota);
	attempts = 0;
	decided.Run([&]() {
		attempts++;
		return attempts == 1 ? HTTPRetryDecision::Retry() : HTTPRetryDecision::Success();
	});
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY);
	decided.ReportFailure();
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY);

	// Deciding or reporting an outcome is the coordinator's: a status-owned budget refuses it
	HTTPRetryBudget status_owned(params);
	status_owned.BindQuota(quota);
#ifndef DUCKDB_CRASH_ON_ASSERT
	// an InternalException aborts the process when DUCKDB_CRASH_ON_ASSERT is set
	REQUIRE_THROWS_AS(status_owned.ReportSuccess(), InternalException);
	REQUIRE_THROWS_AS(status_owned.ReportFailure(), InternalException);
	REQUIRE_THROWS_AS(status_owned.Run([&]() { return HTTPRetryDecision::Success(); }), InternalException);
	REQUIRE_THROWS_AS(status_owned.Run([&]() { return HTTPRetryDecision::Failure(); }), InternalException);
#endif
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY);
}

TEST_CASE("A coordinator that validates after its loop owns the settlement", "[http]") {
	HTTPUtil http_util;
	HTTPParams params(http_util);
	params.retries = 3;
	params.retry_wait_ms = 0;
	HTTPHeaders headers;
	const string url = "http://bucket.s3.amazonaws.com/key?uploadId=1";
	auto quota = http_util.GetRetryQuota("http://bucket.s3.amazonaws.com");
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY);

	// The coordinator retries server errors inside its loop and returns Finish() for everything else: the operation's
	// handler validates the response after the loop, as the multipart completion parses its document, and reports.
	// Core's nested request loop records what each status says along the way; that record must not settle anything.
	using Script = vector<std::pair<HTTPStatusCode, const char *>>;
	const Script server_error_then_invalid_part {{HTTPStatusCode::InternalServerError_500, ""},
	                                             {HTTPStatusCode::OK_200, S3_INVALID_PART_BODY}};
	unique_ptr<HTTPResponse> last;
	auto run_loop = [&](HTTPRetryBudget &budget, const Script &script) {
		idx_t sends = 0;
		budget.Run([&]() {
			PostRequestInfo request(url, headers, params, nullptr, 0);
			request.retry_budget = budget;
			request.try_request = true;
			last = HTTPUtil::RunRequestWithRetry(
			    [&]() {
				    auto &scripted = script[MinValue<idx_t>(sends, script.size() - 1)];
				    sends++;
				    return MakeResponse(scripted.first, scripted.second);
			    },
			    request, {});
			if (!last->Success() && HTTPUtil::IsRetryableStatus(last->status)) {
				return HTTPRetryDecision::Retry();
			}
			return HTTPRetryDecision::Finish();
		});
		return sends;
	};

	// HTTP 500, then HTTP 200 carrying InvalidPart: core recorded a success for the 2xx and the loop stopped with
	// Finish(); the handler rejects the body and reports the failure, so the retry stays charged
	HTTPRetryBudget rejected(params, HTTPRetryOutcomeOwner::COORDINATOR);
	REQUIRE(run_loop(rejected, server_error_then_invalid_part) == 2);
	REQUIRE(last->Success());
	REQUIRE(ErrorDocumentHTTPUtil::ErrorCode(last->body) == "InvalidPart");
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);
	rejected.ReportFailure();
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);
	// ... and a success reported after that changes nothing
	rejected.ReportSuccess();
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);
	// A host that keeps doing this runs out of quota: 100 such operations make 100 retries, then one send each
	for (idx_t i = 1; i < HTTPRetryQuota::INITIAL_CAPACITY / HTTPRetryQuota::RETRY_COST; i++) {
		HTTPRetryBudget budget(params, HTTPRetryOutcomeOwner::COORDINATOR);
		REQUIRE(run_loop(budget, server_error_then_invalid_part) == 2);
		budget.ReportFailure();
	}
	REQUIRE(quota->Available() == 0);
	HTTPRetryBudget refused(params, HTTPRetryOutcomeOwner::COORDINATOR);
	REQUIRE(run_loop(refused, server_error_then_invalid_part) == 1);
	REQUIRE(refused.QuotaExhausted());
	refused.ReportFailure();
	REQUIRE(quota->Available() == 0);

	// Only the handler's validated success refunds: the same responses, accepted by the handler
	quota->Release(HTTPRetryQuota::INITIAL_CAPACITY);
	HTTPRetryBudget accepted(params, HTTPRetryOutcomeOwner::COORDINATOR);
	REQUIRE(run_loop(accepted, server_error_then_invalid_part) == 2);
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);
	accepted.ReportSuccess();
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY);

	// An operation whose handler could not read the response (an unknown outcome) never reports: it stays charged
	{
		HTTPRetryBudget unknown(params, HTTPRetryOutcomeOwner::COORDINATOR);
		REQUIRE(run_loop(unknown, server_error_then_invalid_part) == 2);
	}
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);

	// The status-owned budget of the same loop is settled by the 2xx the moment the loop stops, and nobody can
	// report on it afterwards: that is what a coordinator must not leave to core
	quota->Release(HTTPRetryQuota::INITIAL_CAPACITY);
	HTTPRetryBudget status_owned(params);
	REQUIRE(run_loop(status_owned, server_error_then_invalid_part) == 2);
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY);
#ifndef DUCKDB_CRASH_ON_ASSERT
	// an InternalException aborts the process when DUCKDB_CRASH_ON_ASSERT is set
	REQUIRE_THROWS_AS(status_owned.ReportFailure(), InternalException);
#endif
}

TEST_CASE("Corrected re-sends are bounded and stay within the advertised attempt bound", "[http]") {
	AttemptRecordingHTTPUtil http_util;
	HTTPParams params(http_util);
	params.retry_wait_ms = 0;
	HTTPHeaders headers;
	const string url = "http://bucket.s3.amazonaws.com/key";
	auto quota = http_util.GetRetryQuota("http://bucket.s3.amazonaws.com");

	// A credential correction (a re-send that consumes no retry) followed by a throttling server
	auto run_operation = [&](idx_t corrections) {
		http_util.attempts.clear();
		HTTPRetryBudget budget(params, HTTPRetryOutcomeOwner::COORDINATOR);
		idx_t corrected = 0;
		budget.Run([&]() {
			HeadRequestInfo request(url, headers, params);
			request.retry_budget = budget;
			request.try_request = true;
			auto response = HTTPUtil::RunRequestWithRetry(
			    [&]() {
				    return MakeResponse(corrected < corrections ? HTTPStatusCode::Forbidden_403
				                                                : HTTPStatusCode::ServiceUnavailable_503);
			    },
			    request, {});
			if (response->status == HTTPStatusCode::Forbidden_403) {
				corrected++;
				return HTTPRetryDecision::Correction();
			}
			return HTTPRetryDecision::Failure();
		});
		return budget.CorrectionsExhausted();
	};

	params.retries = 3;
	REQUIRE_FALSE(run_operation(1));
	const auto base_max = 1 + 3 + HTTPRetryBudget::THROTTLE_EXTRA_RETRIES;
	// The correction, then the first throttled send and every throttled retry: attempt=10; max=10 at the end
	REQUIRE(http_util.attempts.size() == base_max + 1);
	REQUIRE(http_util.attempts.front() == std::pair<idx_t, idx_t>(1, base_max));
	for (auto &entry : http_util.attempts) {
		REQUIRE(entry.first <= entry.second);
	}
	REQUIRE(http_util.attempts.back() == std::pair<idx_t, idx_t>(base_max + 1, base_max + 1));
	// The throttled retries were charged and the failure refunded nothing
	REQUIRE(quota->Available() ==
	        HTTPRetryQuota::INITIAL_CAPACITY - static_cast<int64_t>(base_max - 1) * HTTPRetryQuota::RETRY_COST);

	// With retries switched off the corrected re-send is the second and last attempt
	params.retries = 0;
	REQUIRE_FALSE(run_operation(1));
	REQUIRE(http_util.attempts.size() == 2);
	REQUIRE(http_util.attempts[0] == std::pair<idx_t, idx_t>(1, 1));
	REQUIRE(http_util.attempts[1] == std::pair<idx_t, idx_t>(2, 2));

	// A coordinator that keeps correcting is stopped after MAX_CORRECTIONS re-sends, with the last response
	quota->Release(HTTPRetryQuota::INITIAL_CAPACITY);
	REQUIRE(run_operation(1000));
	REQUIRE(http_util.attempts.size() == 1 + HTTPRetryBudget::MAX_CORRECTIONS);
	for (auto &entry : http_util.attempts) {
		REQUIRE(entry.first <= entry.second);
	}
	REQUIRE(quota->Available() == HTTPRetryQuota::INITIAL_CAPACITY);
}

TEST_CASE("A returned transport error carries the quota exhaustion note where GetError reads it", "[http]") {
	HTTPUtil http_util;
	HTTPParams params(http_util);
	params.retries = 10;
	params.retry_wait_ms = 0;
	HTTPHeaders headers;
	HeadRequestInfo request("http://flaky.example.com/file", headers, params);
	request.try_request = true;
	auto quota = http_util.GetRetryQuota("http://flaky.example.com");
	// Room for exactly one transport-failure retry
	REQUIRE(quota->TryAcquire(HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::TRANSIENT_RETRY_COST));

	idx_t sends = 0;
	auto response = HTTPUtil::RunRequestWithRetry(
	    [&]() {
		    sends++;
		    auto failed = make_uniq<HTTPResponse>(HTTPStatusCode::INVALID);
		    failed->request_error = "Connection reset by peer";
		    return failed;
	    },
	    request, {});
	REQUIRE(sends == 2);
	REQUIRE_FALSE(response->Success());
	REQUIRE(response->HasRequestError());
	REQUIRE(StringUtil::Contains(response->GetError(), "Connection reset by peer"));
	REQUIRE(StringUtil::Contains(response->GetError(), "retry quota for 'http://flaky.example.com' exhausted"));
	REQUIRE(quota->Available() == 0);
}

TEST_CASE("HTTPUtil hands out one retry quota per host and forgets the quietest", "[http]") {
	HTTPUtil http_util;
	auto first = http_util.GetRetryQuota("https://a.example.com");
	auto second = http_util.GetRetryQuota("https://b.example.com");
	REQUIRE(first != second);
	REQUIRE(first == http_util.GetRetryQuota("https://a.example.com"));

	// Make the first host the one that was used least recently
	REQUIRE(first->TryAcquire(HTTPRetryQuota::RETRY_COST, TimePoint::GetTickMs() - 1000));
	for (idx_t i = 0; i < HTTPUtil::MAX_RETRY_QUOTAS - 2; i++) {
		http_util.GetRetryQuota(StringUtil::Format("https://host%llu.example.com", i));
	}
	// The map is full: the next new host replaces the first one, which gets a fresh quota on its return
	auto newcomer = http_util.GetRetryQuota("https://c.example.com");
	REQUIRE(newcomer != first);
	REQUIRE(second == http_util.GetRetryQuota("https://b.example.com"));
	auto returned = http_util.GetRetryQuota("https://a.example.com");
	REQUIRE(returned != first);
	REQUIRE(returned->Available() == HTTPRetryQuota::INITIAL_CAPACITY);
	// The budget that still held the evicted quota keeps a valid object
	REQUIRE(first->Available() == HTTPRetryQuota::INITIAL_CAPACITY - HTTPRetryQuota::RETRY_COST);
}
