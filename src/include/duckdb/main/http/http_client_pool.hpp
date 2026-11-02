//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/http/http_client_pool.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/condition_variable.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/main/http/http_transport_config.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {

//! Client storage, FIFO admission and reservation accounting under the transport manager's mutex.
class HTTPClientPool {
public:
	struct ClientKey {
		bool operator==(const ClientKey &other) const;

		idx_t provider_epoch = DConstants::INVALID_INDEX;
		uint64_t connection_epoch = 0;
		idx_t session_id = DConstants::INVALID_INDEX;
		idx_t reuse_domain = 0;
		hash_t origin_hash = 0;
		hash_t transport_config_hash = 0;
	};

private:
	struct AdmissionWaiter {
		condition_variable availability;
	};

	struct ClientKeyHash {
		hash_t operator()(const ClientKey &key) const;
	};

	struct ClientBucket {
		ClientKey key;
		string origin;
		HTTPTransportConfig transport_config;
		vector<unique_ptr<HTTPClient>> idle_clients;
		//! Includes creating, leased, destroying, and idle clients.
		idx_t reserved_clients = 0;
		optional_idx non_empty_index;
	};

	using ClientBucketMap = std::unordered_multimap<ClientKey, ClientBucket, ClientKeyHash>;

public:
	class BucketHandle {
	public:
		BucketHandle() = default;
		bool IsValid() const;
		const ClientKey &GetKey() const;

	private:
		friend class HTTPClientPool;
		explicit BucketHandle(ClientBucket &bucket);
		optional_ptr<ClientBucket> bucket;
	};

	struct Reservation;

	//! Removed storage whose destruction must occur outside the manager lock.
	struct DetachedBucket {
	private:
		friend class HTTPClientPool;
		friend struct Reservation;
		ClientBucketMap::node_type node;
	};

	enum class ReservationKind : uint8_t { REUSE, NEW_CLIENT };
	enum class IdleFilter : uint8_t { PROVIDER, CONNECTION, SESSION, EXCESS, ALL };

	struct Reservation {
	public:
		//! Prepares exclusively owned storage outside the lock; returns whether adoption is needed.
		bool PrepareBucket(const string &origin, const HTTPTransportConfig &transport_config);

		BucketHandle bucket;
		unique_ptr<HTTPClient> client;
		ReservationKind kind = ReservationKind::NEW_CLIENT;

	private:
		friend class HTTPClientPool;
		ClientKey key;
		DetachedBucket bucket_node;
		bool cacheable = false;
		idx_t bucket_capacity = 0;
	};

public:
	HTTPClientPool() = default;
	explicit HTTPClientPool(idx_t capacity);

	//! May release the manager lock while waiting; reserve with refreshed parameters before unlocking.
	void WaitForAdmission(annotated_unique_lock<annotated_mutex> &guard, bool owns_capacity);
	void Close() noexcept;
	bool IsClosed() const;
	bool HasAdmissionResource() const;
	//! Reserve storage before increasing the admission limit
	void SetCapacity(idx_t new_capacity);
	idx_t GetCapacity() const;
	Reservation Reserve(const ClientKey &key, const string &origin, const HTTPTransportConfig &transport_config,
	                    bool cacheable);
	void AdoptPreparedBucket(Reservation &reservation);
	void Return(BucketHandle bucket, unique_ptr<HTTPClient> client);
	Reservation TakeIdleForDisposal(IdleFilter filter, idx_t first = DConstants::INVALID_INDEX, uint64_t second = 0);
	DetachedBucket FinishDestruction(BucketHandle bucket);

	idx_t ReservedClients() const;
	idx_t IdleClients() const;
	idx_t BucketCount() const;
	idx_t AdmissionWaiters() const;
	bool IsEmpty() const;

private:
	void WakeNextAdmission();
	void RemoveAdmissionWaiter(AdmissionWaiter &waiter);
	ClientBucketMap::iterator FindBucket(const ClientKey &key, const string &origin,
	                                     const HTTPTransportConfig &transport_config);
	ClientBucketMap::node_type ExtractBucket(ClientBucket &bucket);
	unique_ptr<HTTPClient> TakeIdleClient(ClientBucket &bucket);
	void AddNonEmptyBucket(ClientBucket &bucket);
	void RemoveNonEmptyBucket(ClientBucket &bucket);
	static bool MatchesFilter(const ClientKey &key, IdleFilter filter, idx_t first, uint64_t second);

private:
	idx_t capacity = 0;
	//! Includes creating, leased, destroying, and idle clients.
	idx_t reserved_clients = 0;
	ClientBucketMap client_buckets;
	//! Nonempty buckets permit constant-time arbitrary eviction.
	vector<optional_ptr<ClientBucket>> non_empty_buckets;
	deque<optional_ptr<AdmissionWaiter>> admission_waiters;
	bool closed = false;
};

} // namespace duckdb
