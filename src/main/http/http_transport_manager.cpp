#include "duckdb/main/http/http_transport_manager.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

static constexpr idx_t HTTP_TRANSPORT_MAX_CAPACITY = 256;

class HTTPTransportManagerState {
public:
	HTTPTransportManagerState(shared_ptr<HTTPUtil> http_util_p, HTTPTransportReusePolicy reuse_policy_p,
	                          idx_t provider_epoch_p)
	    : http_util(std::move(http_util_p)), reuse_policy(reuse_policy_p), provider_epoch(provider_epoch_p) {
	}

	//! Provider retained for this publication.
	const shared_ptr<HTTPUtil> http_util;
	//! Client reuse contract reported by the provider.
	const HTTPTransportReusePolicy reuse_policy;
	//! Stable manager-issued provider identity.
	const idx_t provider_epoch;
	//! Current connection generation for explicit invalidation.
	uint64_t connection_epoch = 0;
	//! Whether epoch exhaustion permanently disabled reuse.
	bool reuse_poisoned = false;
};

class HTTPTransportCapacityGuard;

class HTTPTransportCapacityGuard {
public:
	explicit HTTPTransportCapacityGuard(HTTPTransportManager &manager_p)
	    : manager(manager_p), previous(CurrentThreadGuard()) {
		CurrentThreadGuard() = this;
	}

	~HTTPTransportCapacityGuard() {
		D_ASSERT(CurrentThreadGuard().get() == this);
		CurrentThreadGuard() = previous;
	}

	void MarkCapacityOwned() {
		D_ASSERT(CurrentThreadGuard().get() == this);
		owns_capacity = true;
	}

	static bool CurrentThreadOwnsCapacity(HTTPTransportManager &manager) {
		for (auto entry = CurrentThreadGuard(); entry; entry = entry->previous) {
			if (entry->manager.get() == &manager && entry->owns_capacity) {
				return true;
			}
		}
		return false;
	}

	static void MarkCurrentCapacityOwned(HTTPTransportManager &manager) {
		D_ASSERT(CurrentThreadGuard());
		D_ASSERT(CurrentThreadGuard()->manager.get() == &manager);
		CurrentThreadGuard()->MarkCapacityOwned();
	}

private:
	static optional_ptr<HTTPTransportCapacityGuard> &CurrentThreadGuard() {
		static thread_local optional_ptr<HTTPTransportCapacityGuard> current_guard;
		return current_guard;
	}

	//! Manager whose capacity can be held by this call stack.
	optional_ptr<HTTPTransportManager> manager;
	//! Previous guard on the current thread.
	optional_ptr<HTTPTransportCapacityGuard> previous;
	//! Whether this scope currently owns counted capacity.
	bool owns_capacity = false;
};

static void ValidateReusePolicy(HTTPTransportReusePolicy policy) {
	switch (policy) {
	case HTTPTransportReusePolicy::CLIENT_FREE:
	case HTTPTransportReusePolicy::EPHEMERAL:
	case HTTPTransportReusePolicy::SESSION_LOCAL:
	case HTTPTransportReusePolicy::SHARED:
		return;
	default:
		throw InvalidInputException("HTTP provider returned an invalid transport reuse policy");
	}
}

HTTPTransportManager::Session::Session(HTTPTransportManager &manager_p, HTTPTransportManagerState &state_p,
                                       idx_t session_id_p, unique_ptr<HTTPParams> params_p)
    : manager(manager_p), state(state_p), session_id(session_id_p), params(std::move(params_p)) {
}

HTTPTransportManager::Session::Session(Session &&other) noexcept
    : manager(other.manager), state(other.state), session_id(other.session_id), params(std::move(other.params)) {
	other.manager = nullptr;
	other.state = nullptr;
	other.session_id = DConstants::INVALID_INDEX;
}

HTTPTransportManager::Session &HTTPTransportManager::Session::operator=(Session &&other) noexcept {
	if (this == &other) {
		return *this;
	}
	Reset();
	manager = other.manager;
	state = other.state;
	session_id = other.session_id;
	params = std::move(other.params);
	other.manager = nullptr;
	other.state = nullptr;
	other.session_id = DConstants::INVALID_INDEX;
	return *this;
}

HTTPTransportManager::Session::~Session() {
	Reset();
}

void HTTPTransportManager::Session::Reset() noexcept {
	if (manager) {
		manager->DestroySession(session_id);
	}
	manager = nullptr;
	state = nullptr;
	session_id = DConstants::INVALID_INDEX;
	params.reset();
}

HTTPParams &HTTPTransportManager::Session::Parameters() {
	if (!manager || !params) {
		throw InvalidInputException("Cannot access parameters from an invalid HTTP transport session");
	}
	return *params;
}

unique_ptr<HTTPResponse> HTTPTransportManager::Session::Request(BaseRequest &request) {
	if (!manager) {
		throw InvalidInputException("Cannot perform a request with an invalid HTTP transport session");
	}
	return manager->PerformRequest(*this, request);
}

void HTTPTransportManager::Session::Invalidate() noexcept {
	if (manager) {
		manager->Invalidate(state);
	}
}

HTTPTransportManager::Lease::Lease(HTTPTransportManager &manager_p, HTTPTransportManagerState &state_p,
                                   BucketHandle bucket_p, unique_ptr<HTTPClient> client_p)
    : manager(manager_p), state(state_p), bucket(bucket_p), client(std::move(client_p)) {
}

HTTPTransportManager::Lease::Lease(Lease &&other) noexcept
    : manager(other.manager), state(other.state), bucket(other.bucket), client(std::move(other.client)),
      reusable(other.reusable) {
	other.manager = nullptr;
	other.state = nullptr;
	other.bucket = BucketHandle();
	other.reusable = false;
}

HTTPTransportManager::Lease::~Lease() {
	Reset();
}

void HTTPTransportManager::Lease::Reset() noexcept {
	if (manager) {
		manager->Release(*this);
	}
	manager = nullptr;
	state = nullptr;
	bucket = BucketHandle();
	client.reset();
	reusable = false;
}

unique_ptr<HTTPClient> &HTTPTransportManager::Lease::Client() {
	if (!manager) {
		throw InvalidInputException("Cannot use an invalid HTTP transport lease");
	}
	return client;
}

void HTTPTransportManager::Lease::InvalidateClient() noexcept {
	reusable = false;
}

unique_ptr<HTTPTransportManager> HTTPTransportManager::Create(const shared_ptr<HTTPUtil> &initial_http_util) {
	return unique_ptr<HTTPTransportManager>(new HTTPTransportManager(initial_http_util));
}

HTTPTransportManager::HTTPTransportManager(const shared_ptr<HTTPUtil> &initial_http_util) {
	if (!initial_http_util) {
		throw InvalidInputException("HTTP provider cannot be null");
	}
	auto reuse_policy = initial_http_util->GetTransportReusePolicy();
	ValidateReusePolicy(reuse_policy);
	providers.push_back(make_uniq<HTTPTransportManagerState>(initial_http_util, reuse_policy, idx_t(0)));
}

HTTPTransportManager::~HTTPTransportManager() {
	Close();
	annotated_lock_guard<annotated_mutex> guard(lock);
	D_ASSERT(clients.IsEmpty());
}

idx_t HTTPTransportManager::CalculateCapacity(idx_t system_concurrency, optional_idx file_descriptor_limit,
                                              idx_t io_concurrency) {
	idx_t cpu_target;
	if (system_concurrency >= 128) {
		cpu_target = HTTP_TRANSPORT_MAX_CAPACITY;
	} else {
		cpu_target = system_concurrency * 2;
		cpu_target = MaxValue<idx_t>(cpu_target, 16);
	}
	// every async I/O thread can hold a client at once, so do not let the pool be the smaller of the two
	cpu_target = MaxValue<idx_t>(cpu_target, io_concurrency);
	if (!file_descriptor_limit.IsValid()) {
		return cpu_target;
	}
	auto fd_target = FileSystem::GetFileDescriptorShare(file_descriptor_limit.GetIndex());
	return MinValue<idx_t>(cpu_target, fd_target);
}

bool HTTPTransportManager::AdvanceConnectionEpoch(uint64_t &connection_epoch, bool &reuse_poisoned) noexcept {
	if (reuse_poisoned) {
		return true;
	}
	if (connection_epoch == NumericLimits<uint64_t>::Maximum()) {
		reuse_poisoned = true;
		return true;
	}
	connection_epoch++;
	return false;
}

void HTTPTransportManager::Initialize(const DBConfig &config) {
	auto new_capacity = config.options.http_client_pool_capacity == DConstants::INVALID_INDEX
	                        ? AutomaticCapacity(config)
	                        : config.options.http_client_pool_capacity;
	if (new_capacity == 0) {
		throw InvalidInputException("The HTTP client pool capacity must be at least 1");
	}
	HTTPClientPool new_clients(new_capacity);

	annotated_lock_guard<annotated_mutex> guard(lock);
	if (initialized) {
		throw InternalException("HTTP transport manager was initialized more than once");
	}
	if (clients.IsClosed()) {
		throw InvalidInputException("HTTP transport manager is closed");
	}
	clients = std::move(new_clients);
	initialized = true;
}

idx_t HTTPTransportManager::AutomaticCapacity(const DBConfig &config) {
	auto io_concurrency = config.options.async_threads == DConstants::INVALID_INDEX
	                          ? DBConfig::GetSystemMaxAsyncThreads()
	                          : config.options.async_threads;
	return CalculateCapacity(DBConfig::GetSystemMaxThreads(), FileSystem::GetFileDescriptorLimit(), io_concurrency);
}

idx_t HTTPTransportManager::GetCapacity() const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	return clients.GetCapacity();
}

void HTTPTransportManager::SetCapacity(idx_t capacity) {
	if (capacity == 0) {
		throw InvalidInputException("The HTTP client pool capacity must be at least 1");
	}
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (!initialized) {
			throw InternalException("HTTP transport manager capacity was set before initialization");
		}
		if (clients.IsClosed()) {
			return;
		}
		clients.SetCapacity(capacity);
	}
	DisposeIdle(IdleFilter::EXCESS);
}

HTTPTransportManager::SessionReservation HTTPTransportManager::ReserveSession() {
	annotated_lock_guard<annotated_mutex> guard(lock);
	if (!initialized) {
		throw InternalException("HTTP transport manager is not initialized");
	}
	if (clients.IsClosed()) {
		throw InvalidInputException("HTTP transport manager is closed");
	}
	if (next_session_id == DConstants::INVALID_INDEX) {
		throw InternalException("HTTP transport session identifiers exhausted");
	}
	SessionReservation result;
	result.session_id = next_session_id++;
	result.state = providers[current_provider_epoch].get();
	return result;
}

HTTPTransportManager::Session HTTPTransportManager::FinishSession(SessionReservation reservation,
                                                                  unique_ptr<HTTPParams> params) {
	if (!params) {
		throw InvalidConfigurationException("HTTP provider returned null parameters");
	}
	if (&params->http_util != reservation.state->http_util.get()) {
		throw InvalidConfigurationException("HTTP provider returned parameters owned by a different provider");
	}
	params->transport_manager = this;
	params->transport_state = reservation.state;
	params->transport_session_id = reservation.session_id;
	return Session(*this, *reservation.state, reservation.session_id, std::move(params));
}

void HTTPTransportManager::ValidateDatabase(DatabaseInstance &db) const {
	if (&db.config.GetHTTPTransportManager() != this) {
		throw InvalidInputException("HTTP transport session belongs to a different database");
	}
}

HTTPTransportManager::Session HTTPTransportManager::CreateSession(DatabaseInstance &db, const string &path) {
	ValidateDatabase(db);
	auto reservation = ReserveSession();
	return FinishSession(reservation, reservation.state->http_util->InitializeParameters(db, path));
}

HTTPTransportManager::Session HTTPTransportManager::CreateSession(ClientContext &context, const string &path) {
	ValidateDatabase(DatabaseInstance::GetDatabase(context));
	auto reservation = ReserveSession();
	return FinishSession(reservation, reservation.state->http_util->InitializeParameters(context, path));
}

HTTPTransportManager::Session HTTPTransportManager::CreateSession(optional_ptr<FileOpener> opener,
                                                                  optional_ptr<FileOpenerInfo> info) {
	auto opener_db = FileOpener::TryGetDatabase(opener);
	if (opener_db) {
		ValidateDatabase(*opener_db);
	}
	auto reservation = ReserveSession();
	return FinishSession(reservation, reservation.state->http_util->InitializeParameters(opener, info));
}

void HTTPTransportManager::SetHTTPUtil(const shared_ptr<HTTPUtil> &new_http_util) {
	if (!new_http_util) {
		throw InvalidInputException("HTTP provider cannot be null");
	}
	auto reuse_policy = new_http_util->GetTransportReusePolicy();
	ValidateReusePolicy(reuse_policy);

	idx_t old_provider_epoch;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (clients.IsClosed()) {
			throw InvalidInputException("HTTP transport manager is closed");
		}
		if (providers.size() == providers.max_size()) {
			throw InternalException("HTTP provider identifiers exhausted");
		}
		old_provider_epoch = current_provider_epoch;
		auto provider_epoch = providers.size();
		providers.push_back(make_uniq<HTTPTransportManagerState>(new_http_util, reuse_policy, provider_epoch));
		current_provider_epoch = provider_epoch;
	}
	DisposeIdle(IdleFilter::PROVIDER, old_provider_epoch);
}

HTTPUtil &HTTPTransportManager::GetHTTPUtil() const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	return *providers[current_provider_epoch]->http_util;
}

bool HTTPTransportManager::IsStateValidLocked(const HTTPTransportManagerState &state) const {
	return state.provider_epoch < providers.size() && providers[state.provider_epoch].get() == &state;
}

void HTTPTransportManager::ValidateStateLocked(const HTTPTransportManagerState &state) const {
	if (clients.IsClosed()) {
		throw InvalidInputException("HTTP transport manager is closed");
	}
	if (!IsStateValidLocked(state)) {
		throw InvalidInputException("HTTP parameters contain an invalid provider snapshot");
	}
}

HTTPTransportManager::Reservation
HTTPTransportManager::ReserveClientLocked(HTTPTransportManagerState &state, ClientKey key, const string &origin,
                                          const HTTPTransportConfig &transport_config,
                                          annotated_unique_lock<annotated_mutex> &guard) {
	D_ASSERT(state.reuse_policy != HTTPTransportReusePolicy::CLIENT_FREE);
	ValidateStateLocked(state);
	clients.WaitForAdmission(guard, HTTPTransportCapacityGuard::CurrentThreadOwnsCapacity(*this));
	ValidateStateLocked(state);

	key.connection_epoch = state.connection_epoch;
	const bool cacheable = state.provider_epoch == current_provider_epoch && !state.reuse_poisoned &&
	                       (state.reuse_policy == HTTPTransportReusePolicy::SESSION_LOCAL ||
	                        state.reuse_policy == HTTPTransportReusePolicy::SHARED);
	auto result = clients.Reserve(key, origin, transport_config, cacheable);
	HTTPTransportCapacityGuard::MarkCurrentCapacityOwned(*this);
	return result;
}

void HTTPTransportManager::PrepareClient(Reservation &reservation, HTTPTransportManagerState &state, HTTPParams &params,
                                         const string &origin) {
	if (reservation.kind == HTTPClientPool::ReservationKind::REUSE && reservation.client) {
		if (reservation.client->CanReuse(params)) {
			reservation.client->Initialize(params);
			return;
		}
	}
	reservation.client.reset();

	HTTPClientInitializationOptions options;
	options.cache_policy = HTTPClientCachePolicy::BYPASS_CACHE;
	reservation.client = state.http_util->InitializeClientExtended(params, origin, options);
	if (!reservation.client && state.reuse_policy != HTTPTransportReusePolicy::EPHEMERAL) {
		throw InvalidConfigurationException("Reusable HTTP provider returned a null client");
	}
}

HTTPTransportManager::Lease HTTPTransportManager::Acquire(Session &session, HTTPParams &params, const string &origin) {
	auto &state = *session.state;
	D_ASSERT(state.reuse_policy != HTTPTransportReusePolicy::CLIENT_FREE);
	const HTTPTransportConfig transport_config(params);
	ClientKey key;
	key.provider_epoch = state.provider_epoch;
	key.session_id =
	    state.reuse_policy == HTTPTransportReusePolicy::SESSION_LOCAL ? session.session_id : DConstants::INVALID_INDEX;
	key.reuse_domain = params.GetTransportReuseDomain();
	key.origin_hash = std::hash<string> {}(origin);
	key.transport_config_hash = transport_config.Hash();

	Reservation reservation;
	{
		annotated_unique_lock<annotated_mutex> guard(lock);
		reservation = ReserveClientLocked(state, key, origin, transport_config, guard);
	}
	try {
		if (reservation.PrepareBucket(origin, transport_config)) {
			annotated_lock_guard<annotated_mutex> guard(lock);
			clients.AdoptPreparedBucket(reservation);
		}
		PrepareClient(reservation, state, params, origin);
	} catch (...) {
		DropReservation(std::move(reservation.client), reservation.bucket);
		throw;
	}
	return Lease(*this, state, reservation.bucket, std::move(reservation.client));
}

unique_ptr<HTTPResponse> HTTPTransportManager::PerformRequest(Session &session, BaseRequest &request) {
	if (request.params.transport_manager.get() != this || request.params.transport_state != session.state ||
	    request.params.transport_session_id != session.session_id) {
		throw InvalidInputException("HTTP parameters do not belong to this transport session");
	}

	auto &state = *session.state;
	if (state.reuse_policy == HTTPTransportReusePolicy::CLIENT_FREE) {
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			ValidateStateLocked(state);
		}
		unique_ptr<HTTPClient> client;
		return state.http_util->Request(request, client);
	}

	HTTPTransportCapacityGuard capacity_guard(*this);
	auto lease = Acquire(session, request.params, request.proto_host_port);
	unique_ptr<HTTPResponse> response;
	try {
		response = state.http_util->Request(request, lease.Client());
	} catch (...) {
		lease.InvalidateClient();
		throw;
	}
	if (response->HasRequestError()) {
		lease.InvalidateClient();
	}
	return response;
}

bool HTTPTransportManager::CanReturnClientLocked(const Lease &lease, bool cleanup_succeeded) const {
	if (!cleanup_succeeded || !lease.reusable || !lease.client || clients.IsClosed() ||
	    clients.ReservedClients() > clients.GetCapacity() || !lease.state || !lease.bucket.IsValid() ||
	    !IsStateValidLocked(*lease.state)) {
		return false;
	}
	auto &state = *lease.state;
	auto &key = lease.bucket.GetKey();
	return !state.reuse_poisoned && state.provider_epoch == current_provider_epoch &&
	       state.connection_epoch == key.connection_epoch &&
	       (state.reuse_policy == HTTPTransportReusePolicy::SESSION_LOCAL ||
	        state.reuse_policy == HTTPTransportReusePolicy::SHARED);
}

void HTTPTransportManager::Release(Lease &lease) noexcept {
	D_ASSERT(HTTPTransportCapacityGuard::CurrentThreadOwnsCapacity(*this));

	bool cleanup_succeeded = true;
	if (lease.client) {
		try {
			lease.client->Cleanup();
		} catch (...) {
			cleanup_succeeded = false;
		}
	}

	bool returned = false;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (CanReturnClientLocked(lease, cleanup_succeeded)) {
			clients.Return(lease.bucket, std::move(lease.client));
			returned = true;
		}
	}
	if (returned) {
		return;
	}

	DropReservation(std::move(lease.client), lease.bucket);
}

void HTTPTransportManager::DropReservation(unique_ptr<HTTPClient> client, BucketHandle bucket) noexcept {
	HTTPClientPool::DetachedBucket bucket_node;
	client.reset();
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		bucket_node = clients.FinishDestruction(bucket);
	}
}

void HTTPTransportManager::DisposeIdle(IdleFilter filter, idx_t first, uint64_t second) noexcept {
	HTTPTransportCapacityGuard capacity_guard(*this);
	while (true) {
		Reservation reservation;
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			reservation = clients.TakeIdleForDisposal(filter, first, second);
			if (!reservation.client) {
				return;
			}
			HTTPTransportCapacityGuard::MarkCurrentCapacityOwned(*this);
		}
		DropReservation(std::move(reservation.client), reservation.bucket);
	}
}

void HTTPTransportManager::DestroySession(idx_t session_id) noexcept {
	if (session_id != DConstants::INVALID_INDEX) {
		DisposeIdle(IdleFilter::SESSION, session_id);
	}
}

void HTTPTransportManager::Invalidate(optional_ptr<HTTPTransportManagerState> state) noexcept {
	if (!state) {
		return;
	}
	uint64_t connection_epoch = 0;
	bool poisoned = false;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (!IsStateValidLocked(*state) || state->reuse_poisoned) {
			return;
		}
		poisoned = AdvanceConnectionEpoch(state->connection_epoch, state->reuse_poisoned);
		if (!poisoned) {
			connection_epoch = state->connection_epoch;
		}
	}
	if (poisoned) {
		DisposeIdle(IdleFilter::PROVIDER, state->provider_epoch);
	} else {
		DisposeIdle(IdleFilter::CONNECTION, state->provider_epoch, connection_epoch);
	}
}

void HTTPTransportManager::Close() noexcept {
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		clients.Close();
	}
	DisposeIdle(IdleFilter::ALL);
}

} // namespace duckdb
