//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/http/http_transport_manager.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/http/http_client_pool.hpp"
#include "duckdb/common/mutex.hpp"

namespace duckdb {

struct DBConfig;
struct HTTPTransportManagerTestHelper;

//! Owns provider publication and bounded HTTP clients for one DatabaseInstance.
//! Sessions and managed parameters borrow the owning DatabaseInstance lifetime.
//! Managed reuse always requires an exact origin and HTTPTransportConfig match.
//! Provider reuse domains and CanReuse may restrict this further, never bypass it.
class HTTPTransportManager {
public:
	class Session {
	public:
		DUCKDB_API Session(Session &&other) noexcept;
		DUCKDB_API Session &operator=(Session &&other) noexcept;
		Session(const Session &) = delete;
		Session &operator=(const Session &) = delete;
		DUCKDB_API ~Session();

	private:
		friend class HTTPTransportManager;

		Session(HTTPTransportManager &manager, HTTPTransportManagerState &state, idx_t session_id,
		        unique_ptr<HTTPParams> params);

	public:
		DUCKDB_API HTTPParams &Parameters();
		DUCKDB_API unique_ptr<HTTPResponse> Request(BaseRequest &request);
		DUCKDB_API void Invalidate() noexcept;

	private:
		void Reset() noexcept;

	private:
		//! Manager borrowed from the owning DatabaseInstance.
		optional_ptr<HTTPTransportManager> manager;
		//! Provider publication captured when the session was initialized.
		optional_ptr<HTTPTransportManagerState> state;
		//! Unique non-recycled identity issued by the manager.
		idx_t session_id = DConstants::INVALID_INDEX;
		//! Parameters owned for the lifetime of this session.
		unique_ptr<HTTPParams> params;
	};

public:
	DUCKDB_API ~HTTPTransportManager();

	//! Retire excess idle clients and discard excess active clients on release
	DUCKDB_API void SetCapacity(idx_t capacity) DUCKDB_EXCLUDES(lock);
	DUCKDB_API idx_t GetCapacity() const DUCKDB_EXCLUDES(lock);
	DUCKDB_API static idx_t AutomaticCapacity(const DBConfig &config);

private:
	explicit HTTPTransportManager(const shared_ptr<HTTPUtil> &initial_http_util);

public:
	DUCKDB_API Session CreateSession(DatabaseInstance &db, const string &path) DUCKDB_EXCLUDES(lock);
	DUCKDB_API Session CreateSession(ClientContext &context, const string &path) DUCKDB_EXCLUDES(lock);
	DUCKDB_API Session CreateSession(optional_ptr<FileOpener> opener, optional_ptr<FileOpenerInfo> info)
	    DUCKDB_EXCLUDES(lock);

private:
	using ClientKey = HTTPClientPool::ClientKey;
	using BucketHandle = HTTPClientPool::BucketHandle;
	using Reservation = HTTPClientPool::Reservation;
	using IdleFilter = HTTPClientPool::IdleFilter;

	class Lease {
	public:
		Lease(Lease &&other) noexcept;
		Lease(const Lease &) = delete;
		Lease &operator=(const Lease &) = delete;
		Lease &operator=(Lease &&other) = delete;
		~Lease();

	private:
		friend class HTTPTransportManager;

		Lease(HTTPTransportManager &manager, HTTPTransportManagerState &state, BucketHandle bucket,
		      unique_ptr<HTTPClient> client);

	public:
		unique_ptr<HTTPClient> &Client();
		void InvalidateClient() noexcept;

	private:
		void Reset() noexcept;

	private:
		//! Manager borrowed from the owning DatabaseInstance.
		optional_ptr<HTTPTransportManager> manager;
		//! Provider publication captured by the creating session.
		optional_ptr<HTTPTransportManagerState> state;
		//! Stable bucket accounting for this reusable reservation.
		BucketHandle bucket;
		//! Client held exclusively by the synchronous request.
		unique_ptr<HTTPClient> client;
		//! Whether this request may return its client to the idle pool.
		bool reusable = true;
	};

	struct SessionReservation {
		//! Provider publication captured for the new session.
		optional_ptr<HTTPTransportManagerState> state;
		//! Unique non-recycled identity reserved for the new session.
		idx_t session_id = DConstants::INVALID_INDEX;
	};

private:
	friend struct DBConfig;
	friend class DatabaseInstance;
	friend struct HTTPTransportManagerTestHelper;

	//! Database lifecycle helpers.
	static unique_ptr<HTTPTransportManager> Create(const shared_ptr<HTTPUtil> &initial_http_util);
	void Initialize(const DBConfig &config) DUCKDB_EXCLUDES(lock);
	void Close() noexcept DUCKDB_EXCLUDES(lock);
	static idx_t CalculateCapacity(idx_t system_concurrency, optional_idx file_descriptor_limit,
	                               idx_t io_concurrency = 0);
	static bool AdvanceConnectionEpoch(uint64_t &connection_epoch, bool &reuse_poisoned) noexcept;
	void SetHTTPUtil(const shared_ptr<HTTPUtil> &new_http_util) DUCKDB_EXCLUDES(lock);
	HTTPUtil &GetHTTPUtil() const DUCKDB_EXCLUDES(lock);

	//! Session lifecycle helpers.
	void ValidateDatabase(DatabaseInstance &db) const;
	SessionReservation ReserveSession() DUCKDB_EXCLUDES(lock);
	Session FinishSession(SessionReservation reservation, unique_ptr<HTTPParams> params) DUCKDB_EXCLUDES(lock);
	void DestroySession(idx_t session_id) noexcept DUCKDB_EXCLUDES(lock);
	void Invalidate(optional_ptr<HTTPTransportManagerState> state) noexcept DUCKDB_EXCLUDES(lock);

	//! Request lifecycle helpers.
	unique_ptr<HTTPResponse> PerformRequest(Session &session, BaseRequest &request) DUCKDB_EXCLUDES(lock);
	Lease Acquire(Session &session, HTTPParams &params, const string &origin) DUCKDB_EXCLUDES(lock);
	Reservation ReserveClientLocked(HTTPTransportManagerState &state, ClientKey key, const string &origin,
	                                const HTTPTransportConfig &transport_config,
	                                annotated_unique_lock<annotated_mutex> &guard) DUCKDB_REQUIRES(lock);
	void PrepareClient(Reservation &reservation, HTTPTransportManagerState &state, HTTPParams &params,
	                   const string &origin) DUCKDB_EXCLUDES(lock);
	void Release(Lease &lease) noexcept DUCKDB_EXCLUDES(lock);
	void DropReservation(unique_ptr<HTTPClient> client, BucketHandle bucket) noexcept DUCKDB_EXCLUDES(lock);

	//! Lock-held state helpers.
	void ValidateStateLocked(const HTTPTransportManagerState &state) const DUCKDB_REQUIRES(lock);
	bool IsStateValidLocked(const HTTPTransportManagerState &state) const DUCKDB_REQUIRES(lock);
	bool CanReturnClientLocked(const Lease &lease, bool cleanup_succeeded) const DUCKDB_REQUIRES(lock);

	//! Idle disposal helpers.
	void DisposeIdle(IdleFilter filter, idx_t first = DConstants::INVALID_INDEX, uint64_t second = 0) noexcept
	    DUCKDB_EXCLUDES(lock);

private:
	//! Protects all mutable manager state below.
	mutable annotated_mutex lock;
	//! Retained provider publications indexed by provider epoch.
	vector<unique_ptr<HTTPTransportManagerState>> providers DUCKDB_GUARDED_BY(lock);
	//! Client storage, admission and reservation accounting under the manager lock.
	HTTPClientPool clients DUCKDB_GUARDED_BY(lock);
	//! Provider epoch selected for new sessions.
	idx_t current_provider_epoch DUCKDB_GUARDED_BY(lock) = 0;
	//! Next non-recycled manager session identity.
	idx_t next_session_id DUCKDB_GUARDED_BY(lock) = 1;
	//! Whether Configure installed the capacity and bucket indexes.
	bool initialized DUCKDB_GUARDED_BY(lock) = false;
};

} // namespace duckdb
