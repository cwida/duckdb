//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/sync_point.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/assert.hpp"
#include "duckdb/common/winapi.hpp"

#include <memory>
#include <string>

namespace duckdb {

//! Sync points are named rendezvous points used by tests to force deterministic
//! thread interleavings. Business code calls SYNC_POINT(name); when the point is
//! enabled, the calling thread announces its arrival (making any WaitAndPause
//! return) and then suspends until the test calls Next. Disabling the point
//! releases every thread waiting on it.
//!
//! Hook naming convention: "<component>.<subsystem>.<meaning>", e.g.
//! "optimizer.partial_precompute.indices_captured". Add a hook only at a
//! boundary a test needs to hold the engine at; each hook is a deliberate, test-only change.
//!
//! ```
//! test thread                 business thread
//! EnableInScope("p")      → register rendezvous
//!                                      SYNC_POINT("p") → Sync("p")
//!                                        (a) arrival channel: signal arrived, block on release
//! WaitAndPause("p")       ← returns (a)
//!   ... test-controlled work ...
//! Next("p")                      ─────► (b) release channel: Sync("p") returns, thread continues
//! scope exit / Disable("p") → closes channels, wakes every waiter
//! ```
//!
//! Only compiled in when assertions are enabled (D_ASSERT_IS_ENABLED): in NDEBUG
//! builds the macro expands to nothing and all control methods are no-ops.
//! D_ASSERT_IS_ENABLED is set for debug, relassert (FORCE_ASSERT=1), and
//! FORCE_DEBUG reldebug builds - not for standard reldebug/release. Guard sync
//! point tests with the same macro or they will not be registered.

#ifdef D_ASSERT_IS_ENABLED
#define SYNC_POINT(name) SyncPointCtl::Sync(name)
#else
#define SYNC_POINT(name) ((void)0)
#endif

//! RAII scoped guard for a sync point: enables the point on construction and
//! disables it (releasing all waiters) on destruction.
class SyncPointRendezvous;

class DUCKDB_API SyncPointScopeGuard {
public:
	explicit SyncPointScopeGuard(const char *name);
	~SyncPointScopeGuard();
	SyncPointScopeGuard(SyncPointScopeGuard &&other) noexcept;
	SyncPointScopeGuard &operator=(SyncPointScopeGuard &&other) noexcept;
	SyncPointScopeGuard(const SyncPointScopeGuard &other) = delete;
	SyncPointScopeGuard &operator=(const SyncPointScopeGuard &other) = delete;

	//! Disable the point before destruction.
	void Disable();

	//! Wait until the business thread reaches the point.
	void WaitAndPause();
	//! Wait until the business thread reaches the point, or fail after timeout_ms.
	void WaitAndPause(uint64_t timeout_ms);

	//! Release the suspended business thread. Every Next() must correspond to one
	//! successful WaitAndPause() - extra calls accumulate release tokens and let a
	//! later Sync() pass without blocking.
	void Next();

private:
	std::string name;
	bool disabled = false;
};

class DUCKDB_API SyncPointCtl {
public:
	//! Enable a sync point. Calling this again for an enabled point is a no-op.
	static void Enable(const char *name);
	//! Disable a sync point and release every thread waiting on it.
	static void Disable(const char *name);

	//! Suspend until a business thread reaches the point. Throws if the point is
	//! not enabled or if it is disabled while waiting.
	static void WaitAndPause(const char *name);
	//! Same as WaitAndPause, but throws if the wait exceeds timeout_ms.
	static void WaitAndPause(const char *name, uint64_t timeout_ms);

	//! Continue execution after the point. Every Next() must correspond to one
	//! successful WaitAndPause() - extra calls accumulate release tokens and let a
	//! later Sync() pass without blocking.
	static void Next(const char *name);

	//! Business-side hook: suspend the calling thread until Next is called.
	//! No-op unless the point is enabled.
	static void Sync(const char *name);

	//! Enable the point in the current scope; the point is disabled when the
	//! returned guard goes out of scope.
	static SyncPointScopeGuard EnableInScope(const char *name);

private:
	static std::shared_ptr<SyncPointRendezvous> GetRendezvous(const char *name);
};

} // namespace duckdb
