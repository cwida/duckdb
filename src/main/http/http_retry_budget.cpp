#include "duckdb/main/http/http_retry_budget.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/main/http/http_util.hpp"

#include <cmath>
#ifndef DUCKDB_NO_THREADS
#include <chrono>
#include <thread>
#endif

namespace duckdb {

HTTPRetryDecision HTTPRetryDecision::Finish() {
	return HTTPRetryDecision(Type::FINISH);
}

HTTPRetryDecision HTTPRetryDecision::Success() {
	return HTTPRetryDecision(Type::SUCCESS);
}

HTTPRetryDecision HTTPRetryDecision::Failure() {
	return HTTPRetryDecision(Type::FAILURE);
}

HTTPRetryDecision HTTPRetryDecision::Correction() {
	return HTTPRetryDecision(Type::CORRECTION);
}

HTTPRetryDecision HTTPRetryDecision::Retry() {
	return HTTPRetryDecision(Type::RETRY);
}

HTTPRetryDecision HTTPRetryDecision::Transient() {
	return HTTPRetryDecision(Type::TRANSIENT);
}

HTTPRetryDecision HTTPRetryDecision::Throttled(const string &retry_after) {
	return HTTPRetryDecision(Type::THROTTLED, retry_after);
}

void HTTPRetryQuota::Touch(int64_t now_ms) {
	if (now_ms - last_used_ms >= IDLE_RESET_MS) {
		capacity = INITIAL_CAPACITY;
	}
	last_used_ms = now_ms;
}

bool HTTPRetryQuota::TryAcquire(int64_t cost, int64_t now_ms) {
	lock_guard<mutex> guard(lock);
	Touch(now_ms);
	if (cost > capacity) {
		return false;
	}
	capacity -= cost;
	return true;
}

void HTTPRetryQuota::Release(int64_t amount, int64_t now_ms) {
	lock_guard<mutex> guard(lock);
	Touch(now_ms);
	capacity = MinValue<int64_t>(capacity + amount, INITIAL_CAPACITY);
}

int64_t HTTPRetryQuota::Available() const {
	lock_guard<mutex> guard(lock);
	return capacity;
}

int64_t HTTPRetryQuota::IdleMs(int64_t now_ms) const {
	lock_guard<mutex> guard(lock);
	return now_ms - last_used_ms;
}

HTTPRetryBudget::HTTPRetryBudget(const HTTPParams &params, HTTPRetryOutcomeOwner owner_p)
    : retries(params.retries), retry_wait_ms(params.retry_wait_ms), retry_backoff(params.retry_backoff),
      owner(owner_p) {
#ifdef DUCKDB_NO_THREADS
	// Without threads there is no back-off sleep, so the schedule settings go unused.
	(void)retry_wait_ms;
	(void)retry_backoff;
#endif
}

uint64_t HTTPRetryBudget::ThrottledRetries() const {
	return throttled_retries;
}

uint64_t HTTPRetryBudget::RetriesUsed() const {
	return retries_used;
}

uint64_t HTTPRetryBudget::BeginAttempt() {
	return ++attempts;
}

uint64_t HTTPRetryBudget::ThrottleExtraRetries() const {
#ifndef DUCKDB_NO_THREADS
	// http_retries=0 switches retries off entirely, throttled or not.
	return retries > 0 ? THROTTLE_EXTRA_RETRIES : 0;
#else
	// Without threads we cannot sleep between retries, so do not add zero-delay retries.
	return 0;
#endif
}

uint64_t HTTPRetryBudget::Corrections() const {
	// The first send and every admitted retry are accounted for; any other send was a corrected re-send.
	const auto accounted = 1 + retries_used;
	return attempts > accounted ? attempts - accounted : 0;
}

uint64_t HTTPRetryBudget::MaxAttempts() const {
	return 1 + retries + ThrottleExtraRetries() + Corrections();
}

bool HTTPRetryBudget::InRun() const {
	return run_depth > 0;
}

bool HTTPRetryBudget::QuotaExhausted() const {
	return quota_exhausted;
}

bool HTTPRetryBudget::CorrectionsExhausted() const {
	return corrections_exhausted;
}

void HTTPRetryBudget::BindQuota(shared_ptr<HTTPRetryQuota> quota_p) {
	quota = std::move(quota_p);
}

HTTPRetryOutcomeOwner HTTPRetryBudget::Owner() const {
	return owner;
}

void HTTPRetryBudget::RequireCoordinator(const char *what) const {
	if (owner != HTTPRetryOutcomeOwner::COORDINATOR) {
		throw InternalException(
		    "HTTP retry budget: %s is the coordinator's decision, but this operation's outcome is "
		    "owned by the HTTP status; construct the budget with HTTPRetryOutcomeOwner::COORDINATOR",
		    what);
	}
}

void HTTPRetryBudget::ReportSuccess() {
	RequireCoordinator("ReportSuccess");
	Settle(true);
}

void HTTPRetryBudget::ReportFailure() {
	RequireCoordinator("ReportFailure");
	Settle(false);
}

void HTTPRetryBudget::RecordStatusOutcome(bool success) {
	if (owner != HTTPRetryOutcomeOwner::STATUS) {
		// The coordinator validates the response and reports; what the status says is never final here.
		return;
	}
	status_outcome = success ? RecordedOutcome::SUCCESS : RecordedOutcome::FAILURE;
	if (!InRun()) {
		// Nobody else is going to judge this response.
		Settle(success);
	}
}

void HTTPRetryBudget::Settle(bool success) {
	status_outcome = RecordedOutcome::NONE;
	if (outcome_settled) {
		// Refunded or charged already: the operation has one outcome.
		return;
	}
	outcome_settled = true;
	if (success) {
		if (last_retry_quota) {
			last_retry_quota->Release(last_retry_cost);
		} else if (quota) {
			quota->Release(HTTPRetryQuota::NO_RETRY_REFUND);
		}
		for (auto &charge : earlier_throttled_retries) {
			charge.quota->Release(charge.cost);
		}
	}
	ForgetLastRetry();
}

void HTTPRetryBudget::ForgetLastRetry() {
	last_retry_quota.reset();
	last_retry_cost = 0;
	last_retry_throttled = false;
	earlier_throttled_retries.clear();
}

uint64_t HTTPRetryBudget::BackoffMs(uint64_t retry_wait_ms, float retry_backoff, uint64_t retry, uint64_t floor_ms,
                                    uint64_t random_value, bool throttled) {
	D_ASSERT(retry > 0);
	const auto exponent = static_cast<double>(retry - 1);
	// Cap in the double domain so the exponential cannot overflow the cast.
	const auto base_ms =
	    MinValue<double>(static_cast<double>(retry_wait_ms) * pow(static_cast<double>(retry_backoff), exponent),
	                     static_cast<double>(MAX_BACKOFF_MS));
	const auto base = static_cast<uint64_t>(base_ms);
	// Throttled retries never collapse onto the moment the server asked us to slow down.
	const auto lower = throttled ? base / 2 : 0;
	const auto jittered = lower + random_value % (base - lower + 1);
	return MaxValue<uint64_t>(jittered, MinValue<uint64_t>(floor_ms, MAX_BACKOFF_MS));
}

static const StrpTimeFormat &RetryAfterDateFormat() {
	// RFC 7231 IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
	static const StrpTimeFormat format = []() {
		StrpTimeFormat result;
		auto error = StrTimeFormat::ParseFormatSpecifier("%a, %d %b %Y %H:%M:%S GMT", result);
		D_ASSERT(error.empty());
		(void)error;
		return result;
	}();
	return format;
}

bool HTTPRetryBudget::TryParseRetryAfterMs(const string &retry_after, uint64_t &delay_ms) {
	string value = retry_after;
	StringUtil::Trim(value);
	if (value.empty()) {
		return false;
	}
	uint64_t seconds = 0;
	if (TryCast::Operation<string_t, uint64_t>(string_t(value), seconds)) {
		delay_ms = seconds > MAX_BACKOFF_MS / 1000 ? MAX_BACKOFF_MS : seconds * 1000;
		return true;
	}
	timestamp_t target;
	if (!RetryAfterDateFormat().TryParseTimestamp(value.c_str(), value.size(), target)) {
		return false;
	}
	const auto now_ms = Timestamp::GetEpochMs(Timestamp::GetCurrentTimestamp());
	const auto target_ms = Timestamp::GetEpochMs(target);
	delay_ms = target_ms > now_ms ? static_cast<uint64_t>(target_ms - now_ms) : 0;
	return true;
}

void HTTPRetryBudget::Run(const std::function<HTTPRetryDecision()> &attempt) {
	Run(attempt, {});
}

void HTTPRetryBudget::Run(const std::function<HTTPRetryDecision()> &attempt,
                          const std::function<void()> &before_retry) {
	if (!attempt) {
		throw InternalException("HTTP retry loop requires an attempt callback");
	}
	// A refusal belongs to the run that hit it, not to later requests sharing the budget.
	quota_exhausted = false;
	corrections_exhausted = false;
	const bool outermost = !InRun();
	if (outermost) {
		// A new operation leg: whatever an earlier leg settled is history.
		status_outcome = RecordedOutcome::NONE;
		outcome_settled = false;
	}
	struct RunDepthGuard {
		explicit RunDepthGuard(uint64_t &depth_p) : depth(depth_p) {
			depth++;
		}
		~RunDepthGuard() {
			depth--;
		}
		uint64_t &depth;
	} depth_guard(run_depth);
	for (;;) {
		auto decision = attempt();
		switch (decision.type) {
		case HTTPRetryDecision::Type::SUCCESS:
			// The coordinator validated the response: the explicit outcome is final wherever it is decided.
			RequireCoordinator("a Success decision");
			Settle(true);
			return;
		case HTTPRetryDecision::Type::FAILURE:
			RequireCoordinator("a Failure decision");
			Settle(false);
			return;
		case HTTPRetryDecision::Type::FINISH:
			if (outermost && status_outcome != RecordedOutcome::NONE) {
				// A status-owned operation is over and nobody judges it: the outcome core recorded from the status
				// is settled now. Without one the caller records the outcome itself after the run. A
				// coordinator-owned budget never records a status and waits for the coordinator's report.
				Settle(status_outcome == RecordedOutcome::SUCCESS);
			}
			return;
		case HTTPRetryDecision::Type::CORRECTION:
			if (!AdmitCorrection()) {
				return;
			}
			continue;
		default:
			if (!ConsumeAndWait(decision)) {
				return;
			}
			if (before_retry) {
				before_retry();
			}
		}
	}
}

bool HTTPRetryBudget::AdmitCorrection() {
	// The corrected request is a new send of the same operation: the last response does not count.
	status_outcome = RecordedOutcome::NONE;
	if (corrections_admitted >= MAX_CORRECTIONS) {
		// The operation ends with the response it has, and its retries did not get it through.
		corrections_exhausted = true;
		ForgetLastRetry();
		return false;
	}
	corrections_admitted++;
	return true;
}

bool HTTPRetryBudget::ConsumeAndWait(const HTTPRetryDecision &decision) {
	D_ASSERT(decision.type != HTTPRetryDecision::Type::FINISH);
	const bool throttled = decision.type == HTTPRetryDecision::Type::THROTTLED;
	const auto extra_retries = throttled ? ThrottleExtraRetries() : 0;
	// Asking for a retry means the last attempt did not get the operation through, whatever a nested loop recorded.
	status_outcome = RecordedOutcome::NONE;
	if (retries_used >= retries && retries_used - retries >= extra_retries) {
		// Nothing left to refund: the retries already spent did not get this request through.
		ForgetLastRetry();
		return false;
	}
	const auto cost = decision.type == HTTPRetryDecision::Type::TRANSIENT ? HTTPRetryQuota::TRANSIENT_RETRY_COST
	                                                                      : HTTPRetryQuota::RETRY_COST;
	if (quota && !quota->TryAcquire(cost)) {
		quota_exhausted = true;
		ForgetLastRetry();
		return false;
	}
	// The admitted retry is the one a later success refunds; its outcome is still open.
	if (last_retry_quota && last_retry_throttled) {
		earlier_throttled_retries.push_back(HTTPThrottledRetryCharge {std::move(last_retry_quota), last_retry_cost});
	}
	last_retry_quota = quota;
	last_retry_cost = cost;
	last_retry_throttled = throttled;
	outcome_settled = false;
	retries_used++;
	if (throttled) {
		throttled_retries++;
	}
#ifndef DUCKDB_NO_THREADS
	uint64_t floor_ms = 0;
	if (throttled && !decision.retry_after.empty()) {
		TryParseRetryAfterMs(decision.retry_after, floor_ms);
	}
	RandomEngine random;
	const auto sleep_ms =
	    BackoffMs(retry_wait_ms, retry_backoff, retries_used, floor_ms, random.NextRandomInteger64(), throttled);
	if (sleep_ms > 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
	}
#endif
	return true;
}

} // namespace duckdb
