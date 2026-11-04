//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/http/http_retry_budget.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/time_point.hpp"
#include <functional>

namespace duckdb {

struct HTTPParams;

//! An attempt's decision; constructing one does not consume retry budget.
//!
//! The loop that runs an operation is its coordinator: after inspecting a response (status, headers and body) it
//! decides explicitly whether the operation succeeded, failed for good, is retried, or is re-sent corrected. The budget
//! only admits retries, waits between them and keeps the host quota's books, refunding once after a reported success.
//! Finish() is the decision of a loop that does not judge the operation itself (core's request loop, or a provider
//! that has not adopted the explicit protocol): with a status-owned budget the quota is then settled from the HTTP
//! status core recorded; a coordinator-owned budget waits for the coordinator's report.
class HTTPRetryDecision {
	friend class HTTPRetryBudget;
	friend class HTTPUtil;

public:
	//! Stop without judging the operation: a nested loop leaves the decision to the coordinator. The outermost loop of
	//! a status-owned budget settles the quota from the HTTP status core recorded (the pre-protocol behaviour); a
	//! coordinator-owned budget stays unsettled until ReportSuccess() or ReportFailure().
	DUCKDB_API static HTTPRetryDecision Finish();
	//! The operation succeeded: the coordinator validated the response, the quota is refunded once. Only the owner
	//! of a coordinator-owned budget decides this.
	DUCKDB_API static HTTPRetryDecision Success();
	//! The operation failed for good (e.g. an error document behind HTTP 200): nothing is refunded. Only the owner of
	//! a coordinator-owned budget decides this.
	DUCKDB_API static HTTPRetryDecision Failure();
	//! Re-send after correcting the request (credentials, region, endpoint): no retry is consumed and no back-off is
	//! waited, bounded by HTTPRetryBudget::MAX_CORRECTIONS per operation.
	DUCKDB_API static HTTPRetryDecision Correction();
	//! Retry after a received server error (e.g. HTTP 500).
	DUCKDB_API static HTTPRetryDecision Retry();
	//! Retry after a transport failure (connection error, timeout, truncated body): no response was received.
	DUCKDB_API static HTTPRetryDecision Transient();
	//! Retry after a throttling response (HTTP 429/503), honoring an optional Retry-After header value.
	DUCKDB_API static HTTPRetryDecision Throttled(const string &retry_after = string());

private:
	enum class Type : uint8_t { FINISH, SUCCESS, FAILURE, CORRECTION, RETRY, TRANSIENT, THROTTLED };
	explicit HTTPRetryDecision(Type type_p, string retry_after_p = {})
	    : type(type_p), retry_after(std::move(retry_after_p)) {
	}

private:
	Type type;
	string retry_after;
};

//! Retry tokens shared by every operation against one host, after the AWS SDK retry quota.
//! Retries spend tokens and successes refund them, so a host that keeps failing stops being retried.
//! A host nobody has talked to for a while starts over with a full quota, like the CRT drops an idle host's bucket.
class HTTPRetryQuota {
public:
	static constexpr int64_t INITIAL_CAPACITY = 500;
	//! Cost of retrying a received server error or throttle: the server answered, it is just unhappy.
	static constexpr int64_t RETRY_COST = 5;
	//! Cost of retrying a transport failure: nothing is known about the server, so it costs more.
	static constexpr int64_t TRANSIENT_RETRY_COST = 10;
	//! Refund for a request that succeeded without retrying.
	static constexpr int64_t NO_RETRY_REFUND = 1;
	//! A quota untouched for this long is full again when it is next used.
	static constexpr int64_t IDLE_RESET_MS = int64_t(60) * 1000;

public:
	//! Take `cost` tokens; false (and nothing taken) when fewer are available.
	DUCKDB_API bool TryAcquire(int64_t cost, int64_t now_ms = TimePoint::GetTickMs());
	//! Return tokens, never above the initial capacity.
	DUCKDB_API void Release(int64_t amount, int64_t now_ms = TimePoint::GetTickMs());
	DUCKDB_API int64_t Available() const;
	//! Milliseconds since the quota was last charged or refunded.
	DUCKDB_API int64_t IdleMs(int64_t now_ms = TimePoint::GetTickMs()) const;

private:
	//! Refill an idle quota and record the use; the lock must be held.
	void Touch(int64_t now_ms);

private:
	mutable mutex lock;
	int64_t capacity = INITIAL_CAPACITY;
	int64_t last_used_ms = TimePoint::GetTickMs();
};

//! Who settles an operation's outcome with the host quota, fixed when the operation starts.
enum class HTTPRetryOutcomeOwner : uint8_t {
	//! Nobody validates beyond the HTTP status: the status core recorded settles the quota once the outermost loop
	//! stops. Core's own request loop and loops that have not adopted the explicit protocol.
	STATUS,
	//! The coordinator that runs the operation validates the response and settles the quota itself, with a
	//! Success/Failure decision or ReportSuccess/ReportFailure after its loop; core's status is never final for it.
	//! An operation whose coordinator never reports stays charged, like one whose outcome is unknown.
	COORDINATOR
};

//! A throttled retry charged to the quota of a host
struct HTTPThrottledRetryCharge {
	shared_ptr<HTTPRetryQuota> quota;
	int64_t cost;
};

//! Operation-local budget shared by synchronous nested retry loops, never concurrent loops. One budget is one
//! logical operation (one `amz-sdk-invocation-id`): every send it makes, including corrected re-sends, is one of
//! its attempts, as in the AWS SDK's attempt loop. The host quota is settled once per operation, when its outcome is
//! known, by the owner fixed at construction: a coordinator reports it (Success/Failure decisions or
//! ReportSuccess/ReportFailure, whenever it has validated the response, inside or after its loop); for a status-owned
//! budget it is taken from the HTTP status core recorded (RecordStatusOutcome). A status recorded inside an outer Run
//! waits for that loop to stop, since an outer loop may still retry.
class HTTPRetryBudget {
	friend class HTTPUtil;

public:
	//! Longest single back-off sleep, as in the AWS SDKs.
	static constexpr uint64_t MAX_BACKOFF_MS = 20000;
	//! Retries granted beyond `http_retries` for throttled responses: the server asked to slow down, not to stop.
	static constexpr uint64_t THROTTLE_EXTRA_RETRIES = 5;
	//! Corrected re-sends admitted per operation: a credential refresh and a region redirect, with room for a
	//! provider that needs both again after the other; past this the operation ends with the last response.
	static constexpr uint64_t MAX_CORRECTIONS = 4;

public:
	DUCKDB_API explicit HTTPRetryBudget(const HTTPParams &params,
	                                    HTTPRetryOutcomeOwner owner = HTTPRetryOutcomeOwner::STATUS);
	HTTPRetryBudget(const HTTPRetryBudget &) = delete;
	HTTPRetryBudget &operator=(const HTTPRetryBudget &) = delete;

public:
	//! Run once, then consume budget and wait before each requested retry. Exhaustion returns normally.
	//! Callbacks establish replay safety; exceptions propagate without further retries.
	DUCKDB_API void Run(const std::function<HTTPRetryDecision()> &attempt);
	//! Retries admitted for HTTP 429 or 503 responses.
	DUCKDB_API uint64_t ThrottledRetries() const;
	//! Retries admitted across all participating requests.
	DUCKDB_API uint64_t RetriesUsed() const;
	//! Number the next send of the operation: the first call returns 1.
	DUCKDB_API uint64_t BeginAttempt();
	//! Sends so far that were neither the first nor an admitted retry: re-sends after the coordinator corrected the
	//! request (credentials, region), which do not consume retries.
	DUCKDB_API uint64_t Corrections() const;
	//! Most attempts the operation can make: the first one, `http_retries`, the throttle extras and the corrected
	//! re-sends made so far, so that every numbered attempt stays within the bound advertised with it.
	DUCKDB_API uint64_t MaxAttempts() const;
	//! Whether the current Run refused a retry because the host's quota ran out.
	DUCKDB_API bool QuotaExhausted() const;
	//! Whether the current Run refused a corrected re-send because MAX_CORRECTIONS were made already.
	DUCKDB_API bool CorrectionsExhausted() const;
	//! Charge the next admitted retries to this host's quota; a request to another host rebinds.
	DUCKDB_API void BindQuota(shared_ptr<HTTPRetryQuota> quota);
	//! Who settles this operation's outcome.
	DUCKDB_API HTTPRetryOutcomeOwner Owner() const;
	//! The coordinator's final word, for a coordinator that validates the response after its Run returned: the
	//! operation succeeded, refund the last admitted retry and the throttled retries before it to the quotas they were
	//! charged to, or NO_RETRY_REFUND to the bound quota. Settles once; a later report of the same operation changes
	//! nothing. Coordinator-owned budgets only.
	DUCKDB_API void ReportSuccess();
	//! The coordinator's final word: the operation failed, the last admitted retry is not refunded. Coordinator-owned
	//! budgets only.
	DUCKDB_API void ReportFailure();
	//! What the HTTP status of the last response says, recorded by core's request loop. For a status-owned budget it
	//! settles the quota, at once outside a Run or when the outermost Run stops; a coordinator-owned budget ignores
	//! it, its coordinator judges the response.
	DUCKDB_API void RecordStatusOutcome(bool success);
	//! Whether a Run of this budget is in progress, i.e. a nested loop is running inside an outer one.
	DUCKDB_API bool InRun() const;

	//! Back-off window for `retry` is [0, min(wait * backoff^(retry - 1), MAX_BACKOFF_MS)]: full jitter, or its upper
	//! half for throttled retries so they always keep some distance. The result is raised to `floor_ms`.
	DUCKDB_API static uint64_t BackoffMs(uint64_t retry_wait_ms, float retry_backoff, uint64_t retry, uint64_t floor_ms,
	                                     uint64_t random_value, bool throttled = false);
	//! Parse a Retry-After header (delay in seconds or an HTTP-date) into a delay in milliseconds.
	DUCKDB_API static bool TryParseRetryAfterMs(const string &retry_after, uint64_t &delay_ms);

private:
	//! Core's hook runs after admission/backoff, outside the attempt's transport-error handling.
	void Run(const std::function<HTTPRetryDecision()> &attempt, const std::function<void()> &before_retry);
	bool ConsumeAndWait(const HTTPRetryDecision &decision);
	//! Admit a corrected re-send within MAX_CORRECTIONS.
	bool AdmitCorrection();
	uint64_t ThrottleExtraRetries() const;
	//! Settle the operation's outcome with the quota once: refund on success, then forget the last admitted retry.
	void Settle(bool success);
	//! Explicit decisions and reports belong to a coordinator-owned budget; anything else is a programming error.
	void RequireCoordinator(const char *what) const;
	//! Forget the last admitted retry and the throttled retries before it, so that nothing is refunded for them.
	void ForgetLastRetry();

private:
	//! Retry settings captured for the operation.
	const uint64_t retries;
	const uint64_t retry_wait_ms;
	const float retry_backoff;
	//! Who settles the outcome with the quota.
	const HTTPRetryOutcomeOwner owner;

	//! Retries admitted across all participating requests.
	uint64_t retries_used = 0;
	uint64_t throttled_retries = 0;
	//! Sends numbered so far.
	uint64_t attempts = 0;
	//! Corrected re-sends admitted by Correction() decisions.
	uint64_t corrections_admitted = 0;
	//! Nesting depth of Run; status outcomes recorded while this is positive wait for the outermost loop's decision.
	uint64_t run_depth = 0;
	//! Outcome core recorded from the HTTP status, pending the outermost loop's stop (status-owned budgets only).
	enum class RecordedOutcome : uint8_t { NONE, SUCCESS, FAILURE };
	RecordedOutcome status_outcome = RecordedOutcome::NONE;
	//! Whether the operation's outcome has been settled with the quota; a new admitted retry starts a new settlement.
	bool outcome_settled = false;
	//! Quota of the host the current request goes to, if any.
	shared_ptr<HTTPRetryQuota> quota;
	//! Quota and tokens of the most recent admitted retry, refunded on success.
	shared_ptr<HTTPRetryQuota> last_retry_quota;
	int64_t last_retry_cost = 0;
	bool last_retry_throttled = false;
	//! Quotas and tokens of the throttled retries before the most recent one, refunded on success as well, as a host
	//! that throttles requests which then get through is not failing
	vector<HTTPThrottledRetryCharge> earlier_throttled_retries;
	bool quota_exhausted = false;
	bool corrections_exhausted = false;
};

} // namespace duckdb
