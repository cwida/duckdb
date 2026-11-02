#include "duckdb/common/sync_point.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace duckdb {

#ifdef D_ASSERT_IS_ENABLED

//! A two-channel rendezvous: the business thread signals its arrival, then blocks
//! until the test thread sends a release token. Closing the rendezvous wakes
//! everyone and marks the handshake as aborted.
class SyncPointRendezvous {
public:
	SyncPointRendezvous() = default;
	SyncPointRendezvous(const SyncPointRendezvous &) = delete;
	SyncPointRendezvous &operator=(const SyncPointRendezvous &) = delete;

	void SignalArrival() {
		std::unique_lock lock(mutex);
		arrival_count++;
		cv.notify_all();
	}

	//! Returns false when the rendezvous was closed (disabled) while waiting.
	bool WaitForArrival(uint64_t timeout_ms) {
		std::unique_lock lock(mutex);
		auto wait_done = [this] {
			return closed || arrival_count > 0;
		};
		if (timeout_ms == 0) {
			cv.wait(lock, wait_done);
		} else if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), wait_done)) {
			throw InternalException("Timed out after %llu ms waiting for a sync point arrival", timeout_ms);
		}
		if (closed) {
			return false;
		}
		arrival_count--;
		return true;
	}

	void SignalRelease() {
		std::unique_lock lock(mutex);
		release_count++;
		cv.notify_all();
	}

	//! Returns false when the rendezvous was closed (disabled) while waiting.
	bool WaitForRelease() {
		std::unique_lock lock(mutex);
		cv.wait(lock, [this] { return closed || release_count > 0; });
		if (closed) {
			return false;
		}
		release_count--;
		return true;
	}

	void Close() {
		std::unique_lock lock(mutex);
		closed = true;
		cv.notify_all();
	}

private:
	std::mutex mutex;
	std::condition_variable cv;
	idx_t arrival_count = 0;
	idx_t release_count = 0;
	bool closed = false;
};

struct SyncPointRegistry {
	static std::mutex mutex;
	static std::unordered_map<std::string, std::shared_ptr<SyncPointRendezvous>> entries;
	static std::atomic<bool> any_enabled;
};
std::mutex SyncPointRegistry::mutex;
std::unordered_map<std::string, std::shared_ptr<SyncPointRendezvous>> SyncPointRegistry::entries;
std::atomic<bool> SyncPointRegistry::any_enabled(false);

std::shared_ptr<SyncPointRendezvous> SyncPointCtl::GetRendezvous(const char *name) {
	std::unique_lock lock(SyncPointRegistry::mutex);
	auto &entries = SyncPointRegistry::entries;
	auto entry = entries.find(name);
	if (entry == entries.end()) {
		return nullptr;
	}
	return entry->second;
}

void SyncPointCtl::Enable(const char *name) {
	if (name == nullptr || name[0] == '\0') {
		return;
	}
	std::unique_lock lock(SyncPointRegistry::mutex);
	auto &entries = SyncPointRegistry::entries;
	if (entries.find(name) == entries.end()) {
		entries.emplace(name, std::make_shared<SyncPointRendezvous>());
	}
	SyncPointRegistry::any_enabled.store(true, std::memory_order_release);
}

void SyncPointCtl::Disable(const char *name) {
	std::shared_ptr<SyncPointRendezvous> rendezvous;
	{
		std::unique_lock lock(SyncPointRegistry::mutex);
		auto &entries = SyncPointRegistry::entries;
		auto entry = entries.find(name);
		if (entry != entries.end()) {
			rendezvous = std::move(entry->second);
			entries.erase(entry);
			if (entries.empty()) {
				SyncPointRegistry::any_enabled.store(false, std::memory_order_release);
			}
		}
	}
	if (rendezvous) {
		rendezvous->Close();
	}
}

void SyncPointCtl::WaitAndPause(const char *name) {
	WaitAndPause(name, 0);
}

void SyncPointCtl::WaitAndPause(const char *name, uint64_t timeout_ms) {
	auto rendezvous = GetRendezvous(name);
	if (!rendezvous) {
		throw InternalException("Sync point \"%s\" is not enabled", name);
	}
	if (!rendezvous->WaitForArrival(timeout_ms)) {
		throw InternalException("Sync point \"%s\" was disabled while waiting for arrival", name);
	}
}

void SyncPointCtl::Next(const char *name) {
	auto rendezvous = GetRendezvous(name);
	if (!rendezvous) {
		throw InternalException("Sync point \"%s\" is not enabled", name);
	}
	rendezvous->SignalRelease();
}

void SyncPointCtl::Sync(const char *name) {
	if (!SyncPointRegistry::any_enabled.load(std::memory_order_acquire)) {
		return;
	}
	auto rendezvous = GetRendezvous(name);
	if (!rendezvous) {
		// the point is not enabled - no-op
		return;
	}
	rendezvous->SignalArrival();
	// ignoring the result is intentional: a disabled point aborts the handshake and
	// the caller continues instead of blocking
	rendezvous->WaitForRelease();
}

SyncPointScopeGuard SyncPointCtl::EnableInScope(const char *name) {
	return SyncPointScopeGuard(name);
}

#else

void SyncPointCtl::Enable(const char *) {
}
void SyncPointCtl::Disable(const char *) {
}
void SyncPointCtl::WaitAndPause(const char *) {
}
void SyncPointCtl::WaitAndPause(const char *, uint64_t) {
}
void SyncPointCtl::Next(const char *) {
}
void SyncPointCtl::Sync(const char *) {
}
SyncPointScopeGuard SyncPointCtl::EnableInScope(const char *name) {
	return SyncPointScopeGuard(name);
}

#endif

SyncPointScopeGuard::SyncPointScopeGuard(const char *name) : name(name ? name : "") {
	SyncPointCtl::Enable(this->name.c_str());
}

SyncPointScopeGuard::~SyncPointScopeGuard() {
	Disable();
}

SyncPointScopeGuard::SyncPointScopeGuard(SyncPointScopeGuard &&other) noexcept
    : name(std::move(other.name)), disabled(other.disabled) {
	other.disabled = true;
}

SyncPointScopeGuard &SyncPointScopeGuard::operator=(SyncPointScopeGuard &&other) noexcept {
	Disable();
	name = std::move(other.name);
	disabled = other.disabled;
	other.disabled = true;
	return *this;
}

void SyncPointScopeGuard::Disable() {
	if (disabled) {
		return;
	}
	disabled = true;
	SyncPointCtl::Disable(name.c_str());
}

void SyncPointScopeGuard::WaitAndPause() {
	SyncPointCtl::WaitAndPause(name.c_str());
}

void SyncPointScopeGuard::WaitAndPause(uint64_t timeout_ms) {
	SyncPointCtl::WaitAndPause(name.c_str(), timeout_ms);
}

void SyncPointScopeGuard::Next() {
	SyncPointCtl::Next(name.c_str());
}

} // namespace duckdb
