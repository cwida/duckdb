#include "catch.hpp"

#include "test_config.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/main/http/http_transport_manager.hpp"
#include "duckdb/main/extension/linked_extension_registry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/main/database_file_opener.hpp"
#include "duckdb/main/extension_helper.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace duckdb {

TEST_CASE("PUT requests retain their content type", "[http_transport_manager]") {
	HTTPUtil http;
	HTTPParams params(http);
	string content_type = "application/json";
	PutRequestInfo request("http://localhost/upload", HTTPHeaders(), params, nullptr, 0, content_type);
	content_type = "text/plain";
	CHECK(request.content_type == "application/json");
}

enum class MockResponseMode : uint8_t {
	SUCCESS,
	NULL_RESPONSE,
	REQUEST_ERROR,
	HTTP_ERROR,
	RETRYABLE_HTTP_ERROR,
	NOT_MODIFIED,
	THROW,
	CLIENT_THROW
};

struct HTTPTransportManagerTestHelper {
	static unique_ptr<HTTPTransportManager> Create(const shared_ptr<HTTPUtil> &provider, idx_t capacity) {
		auto result = HTTPTransportManager::Create(provider);
		HTTPClientPool clients(capacity);
		annotated_lock_guard<annotated_mutex> guard(result->lock);
		result->clients = std::move(clients);
		result->initialized = true;
		return result;
	}

	static idx_t CalculateCapacity(idx_t concurrency, optional_idx file_descriptor_limit) {
		return HTTPTransportManager::CalculateCapacity(concurrency, file_descriptor_limit);
	}

	static bool AdvanceConnectionEpoch(uint64_t &connection_epoch, bool &reuse_poisoned) {
		return HTTPTransportManager::AdvanceConnectionEpoch(connection_epoch, reuse_poisoned);
	}

	static idx_t OccupiedSlots(HTTPTransportManager &manager) {
		annotated_lock_guard<annotated_mutex> guard(manager.lock);
		return manager.clients.ReservedClients();
	}

	static idx_t IdleClients(HTTPTransportManager &manager) {
		annotated_lock_guard<annotated_mutex> guard(manager.lock);
		return manager.clients.IdleClients();
	}

	static idx_t IdleKeys(HTTPTransportManager &manager) {
		annotated_lock_guard<annotated_mutex> guard(manager.lock);
		return manager.clients.BucketCount();
	}

	static idx_t AdmissionWaiters(HTTPTransportManager &manager) {
		annotated_lock_guard<annotated_mutex> guard(manager.lock);
		return manager.clients.AdmissionWaiters();
	}

	static void Close(HTTPTransportManager &manager) {
		manager.Close();
	}

	static void SetHTTPUtil(HTTPTransportManager &manager, const shared_ptr<HTTPUtil> &provider) {
		manager.SetHTTPUtil(provider);
	}

	static HTTPUtil &GetHTTPUtil(HTTPTransportManager &manager) {
		return manager.GetHTTPUtil();
	}
};

struct MockTransportState {
	std::atomic<idx_t> created {0};
	std::atomic<idx_t> destroyed {0};
	std::atomic<idx_t> live {0};
	std::atomic<idx_t> high_water {0};
	std::atomic<idx_t> initialized {0};
	std::atomic<idx_t> compatibility_checks {0};
	std::atomic<idx_t> cleaned {0};
	std::atomic<idx_t> last_initialized_client {0};
	std::atomic<idx_t> creation_failures {0};
	std::atomic<idx_t> response_attempt {0};
	std::atomic<idx_t> bypass_initializations {0};
	MockResponseMode response_mode = MockResponseMode::SUCCESS;
	vector<MockResponseMode> response_sequence;
	string response_body;
	string observed_if_none_match;
	vector<string> initialized_paths;
	vector<bool> initialized_with_context;
	idx_t retries = 0;
	bool can_reuse = true;
	bool return_null = false;
	bool return_null_after_first = false;
	bool throw_on_create = false;
	bool throw_on_initialize = false;
	bool throw_on_can_reuse = false;
	bool throw_on_cleanup = false;
	std::function<void()> callback;
	std::function<void()> create_callback;
	std::function<void()> initialize_callback;
	std::function<void()> reuse_callback;
	std::function<void()> cleanup_callback;
	std::function<void()> request_callback;
	std::function<void()> destroy_callback;
	mutex request_lock;
	condition_variable request_cv;
	std::atomic<idx_t> requests_started {0};
	idx_t requests_allowed = NumericLimits<idx_t>::Maximum();
	vector<idx_t> request_client_ids;
	vector<string> request_urls;
};

static void UpdateHighWater(MockTransportState &state, idx_t live) {
	auto high_water = state.high_water.load();
	while (live > high_water && !state.high_water.compare_exchange_weak(high_water, live)) {
	}
}

class MockHTTPClient : public HTTPClient {
public:
	MockHTTPClient(const shared_ptr<MockTransportState> &state_p, const string &origin)
	    : HTTPClient(origin), state(state_p), client_id(++state->created) {
		auto live = ++state->live;
		UpdateHighWater(*state, live);
	}

	~MockHTTPClient() override {
		if (state->callback) {
			state->callback();
		}
		if (state->destroy_callback) {
			state->destroy_callback();
		}
		state->destroyed++;
		state->live--;
	}

	void Initialize(HTTPParams &) override {
		if (state->callback) {
			state->callback();
		}
		if (state->initialize_callback) {
			state->initialize_callback();
		}
		state->initialized++;
		state->last_initialized_client = client_id;
		if (state->throw_on_initialize) {
			throw IOException("mock initialize failure");
		}
	}

	bool CanReuse(const HTTPParams &) const override {
		if (state->callback) {
			state->callback();
		}
		if (state->reuse_callback) {
			state->reuse_callback();
		}
		state->compatibility_checks++;
		if (state->throw_on_can_reuse) {
			throw IOException("mock compatibility failure");
		}
		return state->can_reuse;
	}

	void Cleanup() override {
		if (state->callback) {
			state->callback();
		}
		if (state->cleanup_callback) {
			state->cleanup_callback();
		}
		state->cleaned++;
		if (state->throw_on_cleanup) {
			throw IOException("mock cleanup failure");
		}
	}

	unique_ptr<HTTPResponse> Get(GetRequestInfo &info) override {
		if (state->callback) {
			state->callback();
		}
		if (state->request_callback) {
			state->request_callback();
		}
		if (info.headers.HasHeader("If-None-Match")) {
			state->observed_if_none_match = info.headers.GetHeaderValue("If-None-Match");
		}
		{
			unique_lock<mutex> guard(state->request_lock);
			auto request_index = state->requests_started++;
			state->request_client_ids.push_back(client_id);
			state->request_urls.push_back(info.url);
			state->request_cv.notify_all();
			state->request_cv.wait(guard, [&]() { return request_index < state->requests_allowed; });
		}
		auto attempt = state->response_attempt++;
		auto mode = state->response_sequence.empty()
		                ? state->response_mode
		                : state->response_sequence[MinValue<idx_t>(attempt, state->response_sequence.size() - 1)];
		switch (mode) {
		case MockResponseMode::SUCCESS: {
			auto response = make_uniq<HTTPResponse>(HTTPStatusCode::OK_200);
			response->body = state->response_body;
			response->headers.Insert("ETag", "mock-etag");
			return response;
		}
		case MockResponseMode::REQUEST_ERROR: {
			auto response = make_uniq<HTTPResponse>(HTTPStatusCode::INVALID);
			response->request_error = "mock request error";
			return response;
		}
		case MockResponseMode::HTTP_ERROR:
			return make_uniq<HTTPResponse>(HTTPStatusCode::NotFound_404);
		case MockResponseMode::RETRYABLE_HTTP_ERROR:
			return make_uniq<HTTPResponse>(HTTPStatusCode::InternalServerError_500);
		case MockResponseMode::NOT_MODIFIED:
			return make_uniq<HTTPResponse>(HTTPStatusCode::NotModified_304);
		case MockResponseMode::CLIENT_THROW:
			throw IOException("mock client request failure");
		case MockResponseMode::NULL_RESPONSE:
		case MockResponseMode::THROW:
			return nullptr;
		}
		return nullptr;
	}
	unique_ptr<HTTPResponse> Put(PutRequestInfo &) override {
		return nullptr;
	}
	unique_ptr<HTTPResponse> Head(HeadRequestInfo &) override {
		return nullptr;
	}
	unique_ptr<HTTPResponse> Delete(DeleteRequestInfo &) override {
		return nullptr;
	}
	unique_ptr<HTTPResponse> Post(PostRequestInfo &) override {
		return nullptr;
	}
	unique_ptr<HTTPResponse> Options(OptionsRequestInfo &) override {
		return nullptr;
	}

private:
	shared_ptr<MockTransportState> state;
	idx_t client_id;
};

class MockHTTPParams : public HTTPParams {
public:
	explicit MockHTTPParams(HTTPUtil &http_util) : HTTPParams(http_util) {
	}

public:
	void SetReuseDomain(idx_t reuse_domain) {
		SetTransportReuseDomain(reuse_domain);
	}
};

class MockHTTPUtil : public HTTPUtil {
public:
	MockHTTPUtil(HTTPTransportReusePolicy policy_p, string name_p = "mock")
	    : policy(policy_p), name(std::move(name_p)), state(make_shared_ptr<MockTransportState>()) {
	}

	string GetName() const override {
		return name;
	}

	HTTPTransportReusePolicy GetTransportReusePolicy() const override {
		return policy;
	}

	unique_ptr<HTTPParams> InitializeParameters(optional_ptr<FileOpener> opener,
	                                            optional_ptr<FileOpenerInfo> info) override {
		if (state->callback) {
			state->callback();
		}
		state->initialized_paths.push_back(info ? info->file_path : string());
		state->initialized_with_context.push_back(bool(FileOpener::TryGetClientContext(opener)));
		auto result = make_uniq<MockHTTPParams>(*this);
		result->Initialize(opener);
		result->retries = state->retries;
		return unique_ptr<HTTPParams>(std::move(result));
	}

	unique_ptr<HTTPClient> InitializeClientExtended(HTTPParams &, const string &origin,
	                                                const HTTPClientInitializationOptions &options) override {
		if (state->callback) {
			state->callback();
		}
		if (state->create_callback) {
			state->create_callback();
		}
		if (options.cache_policy == HTTPClientCachePolicy::BYPASS_CACHE) {
			state->bypass_initializations++;
		}
		if (state->throw_on_create) {
			throw IOException("mock create failure");
		}
		auto failures = state->creation_failures.load();
		while (failures > 0 && !state->creation_failures.compare_exchange_weak(failures, failures - 1)) {
		}
		if (failures > 0) {
			throw IOException("mock one-shot create failure");
		}
		if (state->return_null || (state->return_null_after_first && state->created > 0)) {
			return nullptr;
		}
		return make_uniq<MockHTTPClient>(state, origin);
	}

	unique_ptr<HTTPResponse> SendRequest(BaseRequest &request, unique_ptr<HTTPClient> &client) override {
		if (policy == HTTPTransportReusePolicy::CLIENT_FREE) {
			return make_uniq<HTTPResponse>(HTTPStatusCode::OK_200);
		}
		if (state->response_mode == MockResponseMode::NULL_RESPONSE) {
			return nullptr;
		}
		if (state->response_mode == MockResponseMode::THROW) {
			throw IOException("mock send failure");
		}
		if (policy == HTTPTransportReusePolicy::EPHEMERAL && !client) {
			return make_uniq<HTTPResponse>(HTTPStatusCode::OK_200);
		}
		return HTTPUtil::SendRequest(request, client);
	}

	HTTPTransportReusePolicy policy;
	string name;
	shared_ptr<MockTransportState> state;
};

TEST_CASE("HTTP transport manager capacity and provider contracts", "[http_transport_manager]") {
	SECTION("database configuration sets the initial capacity") {
		DBConfig config;
		config.SetOptionByName("http_client_pool_capacity", Value::BIGINT(2));
		DuckDB db(nullptr, &config);
		CHECK(db.instance->config.GetHTTPTransportManager().GetCapacity() == 2);
	}

	SECTION("capacity calculation is bounded and descriptor aware") {
		CHECK(HTTPTransportManagerTestHelper::CalculateCapacity(0, optional_idx()) == 16);
		CHECK(HTTPTransportManagerTestHelper::CalculateCapacity(1, optional_idx()) == 16);
		CHECK(HTTPTransportManagerTestHelper::CalculateCapacity(12, optional_idx()) == 24);
		CHECK(HTTPTransportManagerTestHelper::CalculateCapacity(128, optional_idx()) == 256);
		CHECK(HTTPTransportManagerTestHelper::CalculateCapacity(NumericLimits<idx_t>::Maximum(), optional_idx()) ==
		      256);
		CHECK(HTTPTransportManagerTestHelper::CalculateCapacity(64, optional_idx(0)) == 1);
		CHECK(HTTPTransportManagerTestHelper::CalculateCapacity(64, optional_idx(7)) == 1);
		CHECK(HTTPTransportManagerTestHelper::CalculateCapacity(64, optional_idx(80)) == 10);
		CHECK(HTTPTransportManagerTestHelper::CalculateCapacity(64, optional_idx(4096)) == 128);
	}

	SECTION("connection epoch exhaustion permanently poisons reuse") {
		auto connection_epoch = NumericLimits<uint64_t>::Maximum() - 1;
		bool reuse_poisoned = false;
		CHECK_FALSE(HTTPTransportManagerTestHelper::AdvanceConnectionEpoch(connection_epoch, reuse_poisoned));
		CHECK(connection_epoch == NumericLimits<uint64_t>::Maximum());
		CHECK_FALSE(reuse_poisoned);
		CHECK(HTTPTransportManagerTestHelper::AdvanceConnectionEpoch(connection_epoch, reuse_poisoned));
		CHECK(connection_epoch == NumericLimits<uint64_t>::Maximum());
		CHECK(reuse_poisoned);
		CHECK(HTTPTransportManagerTestHelper::AdvanceConnectionEpoch(connection_epoch, reuse_poisoned));
	}

	SECTION("pool coalesces prepared buckets and separates colliding origins and configurations") {
		HTTPClientPool pool(2);
		HTTPClientPool::ClientKey key;
		key.provider_epoch = 0;
		key.origin_hash = 42;
		key.transport_config_hash = 42;
		const string first_origin = "https://first.example.com";
		string second_origin = first_origin;
		const HTTPTransportConfig first_config;
		HTTPTransportConfig second_config;
		SECTION("origin hash collision") {
			second_origin = "https://second.example.com";
		}
		SECTION("transport configuration hash collision") {
			HTTPUtil provider;
			HTTPParams params(provider);
			params.http_proxy = "proxy.example.com";
			params.http_proxy_port = 8080;
			second_config = HTTPTransportConfig(params);
		}
		auto state = make_shared_ptr<MockTransportState>();

		auto first = pool.Reserve(key, first_origin, first_config, true);
		auto second = pool.Reserve(key, first_origin, first_config, true);
		REQUIRE(first.PrepareBucket(first_origin, first_config));
		REQUIRE(second.PrepareBucket(first_origin, first_config));
		pool.AdoptPreparedBucket(first);
		pool.AdoptPreparedBucket(second);
		CHECK(pool.BucketCount() == 1);
		CHECK(pool.ReservedClients() == 2);
		CHECK_FALSE(pool.HasAdmissionResource());
		pool.Return(first.bucket, make_uniq<MockHTTPClient>(state, first_origin));
		CHECK(pool.HasAdmissionResource());
		pool.SetCapacity(1);
		CHECK_FALSE(pool.HasAdmissionResource());
		pool.SetCapacity(2);
		CHECK(pool.HasAdmissionResource());

		// Hash collisions must not reuse clients with a different origin or configuration.
		auto collision = pool.Reserve(key, second_origin, second_config, true);
		CHECK(collision.kind == HTTPClientPool::ReservationKind::NEW_CLIENT);
		REQUIRE(collision.client);
		CHECK(collision.client->GetBaseUrl() == first_origin);
		collision.client.reset();
		CHECK(pool.ReservedClients() == 2);
		REQUIRE(collision.PrepareBucket(second_origin, second_config));
		pool.AdoptPreparedBucket(collision);
		CHECK(pool.BucketCount() == 2);
		pool.Return(collision.bucket, make_uniq<MockHTTPClient>(state, second_origin));
		auto removed_first = pool.FinishDestruction(second.bucket);
		CHECK(pool.ReservedClients() == 1);
		CHECK(pool.BucketCount() == 1);

		auto reused = pool.Reserve(key, second_origin, second_config, true);
		CHECK(reused.kind == HTTPClientPool::ReservationKind::REUSE);
		REQUIRE(reused.client);
		CHECK(reused.client->GetBaseUrl() == second_origin);
		reused.client.reset();
		CHECK(pool.ReservedClients() == 1);
		auto removed_second = pool.FinishDestruction(reused.bucket);
		CHECK(pool.IsEmpty());
		CHECK(state->live == 0);
	}

	SECTION("default provider contracts preserve direct callers") {
		HTTPUtil provider;
		CHECK(provider.GetTransportReusePolicy() == HTTPTransportReusePolicy::EPHEMERAL);
		auto params = provider.InitializeParameters(nullptr, nullptr);
		CHECK(&params->http_util == &provider);
	}

	SECTION("publication retains old direct parameters and republishes the same object") {
		auto first = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::EPHEMERAL, "first");
		auto second = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::EPHEMERAL, "second");
		auto manager = HTTPTransportManagerTestHelper::Create(first, 1);
		auto direct_params = first->InitializeParameters(nullptr, nullptr);

		HTTPTransportManagerTestHelper::SetHTTPUtil(*manager, second);
		CHECK(HTTPTransportManagerTestHelper::GetHTTPUtil(*manager).GetName() == "second");
		first.reset();
		CHECK(direct_params->http_util.GetName() == "first");

		HTTPTransportManagerTestHelper::SetHTTPUtil(*manager, second);
		CHECK(HTTPTransportManagerTestHelper::GetHTTPUtil(*manager).GetName() == "second");
	}

	SECTION("session construction rejects another database") {
		DuckDB first(nullptr);
		DuckDB second(nullptr);
		Connection second_connection(second);
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::EPHEMERAL);
		auto &manager = first.instance->config.GetHTTPTransportManager();
		first.instance->config.SetHTTPUtil(provider);

		CHECK_THROWS_AS(manager.CreateSession(*second.instance, "https://example.com/"), InvalidInputException);
		CHECK_THROWS_AS(manager.CreateSession(*second_connection.context, "https://example.com/"),
		                InvalidInputException);
		DatabaseFileOpener second_opener(*second.instance);
		FileOpenerInfo info {"https://example.com/"};
		CHECK_THROWS_AS(manager.CreateSession(second_opener, info), InvalidInputException);

		auto session = manager.CreateSession(*first.instance, "https://example.com/");
		CHECK(&session.Parameters().http_util == provider.get());
		DatabaseFileOpener first_opener(*first.instance);
		auto opener_session = manager.CreateSession(first_opener, info);
		CHECK(&opener_session.Parameters().http_util == provider.get());
	}
}

static unique_ptr<HTTPResponse> RunManagedRequest(HTTPTransportManager::Session &session, HTTPParams &params,
                                                  const string &url) {
	HTTPHeaders headers;
	GetRequestInfo request(url, headers, params, nullptr, nullptr);
	request.try_request = true;
	return session.Request(request);
}

static void BlockMockRequests(MockTransportState &state) {
	lock_guard<mutex> guard(state.request_lock);
	state.requests_allowed = 0;
}

static bool WaitForMockRequests(MockTransportState &state, idx_t count) {
	unique_lock<mutex> guard(state.request_lock);
	return state.request_cv.wait_for(guard, std::chrono::seconds(5), [&]() { return state.requests_started >= count; });
}

static void AllowMockRequests(MockTransportState &state, idx_t count) {
	lock_guard<mutex> guard(state.request_lock);
	state.requests_allowed = count;
	state.request_cv.notify_all();
}

static bool WaitForAdmissionWaiters(HTTPTransportManager &manager, idx_t count) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (std::chrono::steady_clock::now() < deadline) {
		if (HTTPTransportManagerTestHelper::AdmissionWaiters(manager) == count) {
			return true;
		}
		std::this_thread::yield();
	}
	return false;
}

TEST_CASE("HTTP transport manager synchronous session API", "[http_transport_manager]") {
	SECTION("resizing wakes waiters and retires excess clients") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 1);
		auto session = manager->CreateSession(nullptr, nullptr);
		auto params = make_uniq<HTTPParams>(session.Parameters());
		BlockMockRequests(*provider->state);
		auto first = std::async(std::launch::async, [&]() {
			return RunManagedRequest(session, session.Parameters(), "https://example.com/");
		});
		REQUIRE(WaitForMockRequests(*provider->state, 1));
		auto second = std::async(std::launch::async,
		                         [&]() { return RunManagedRequest(session, *params, "https://example.com/"); });
		REQUIRE(WaitForAdmissionWaiters(*manager, 1));
		manager->SetCapacity(2);
		REQUIRE(WaitForMockRequests(*provider->state, 2));
		manager->SetCapacity(1);
		AllowMockRequests(*provider->state, 1);
		REQUIRE(first.get());
		CHECK(provider->state->live == 1);
		AllowMockRequests(*provider->state, NumericLimits<idx_t>::Maximum());
		REQUIRE(second.get());
		CHECK(HTTPTransportManagerTestHelper::IdleClients(*manager) == 1);
		manager->SetCapacity(2);
		REQUIRE(RunManagedRequest(session, session.Parameters(), "https://other.example.com/"));
		CHECK(provider->state->live == 2);
		manager->SetCapacity(1);
		CHECK(provider->state->live == 1);
		CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*manager) == 1);
	}

	SECTION("reuse policies have distinct capacity and lifetime behavior") {
		for (auto policy : {HTTPTransportReusePolicy::CLIENT_FREE, HTTPTransportReusePolicy::EPHEMERAL,
		                    HTTPTransportReusePolicy::SESSION_LOCAL, HTTPTransportReusePolicy::SHARED}) {
			auto provider = make_shared_ptr<MockHTTPUtil>(policy);
			auto manager = HTTPTransportManagerTestHelper::Create(provider, 1);
			{
				auto session = manager->CreateSession(nullptr, nullptr);
				REQUIRE(RunManagedRequest(session, session.Parameters(), "https://example.com/"));
				if (policy == HTTPTransportReusePolicy::SESSION_LOCAL || policy == HTTPTransportReusePolicy::SHARED) {
					CHECK(provider->state->live == 1);
					CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*manager) == 1);
				} else {
					CHECK(provider->state->live == 0);
					CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*manager) == 0);
				}
				CHECK(provider->state->bypass_initializations ==
				      (policy == HTTPTransportReusePolicy::CLIENT_FREE ? 0 : 1));
			}
			CHECK(provider->state->live == (policy == HTTPTransportReusePolicy::SHARED ? 1 : 0));
			manager.reset();
			CHECK(provider->state->live == 0);
		}
	}

	SECTION("cloned parameters reuse and explicit invalidation advances the connection epoch") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SESSION_LOCAL);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 1);
		auto session = manager->CreateSession(nullptr, nullptr);
		auto clone = make_uniq<HTTPParams>(session.Parameters());
		REQUIRE(RunManagedRequest(session, session.Parameters(), "https://example.com/"));
		REQUIRE(RunManagedRequest(session, *clone, "https://example.com/"));
		CHECK(provider->state->created == 1);
		CHECK(provider->state->compatibility_checks == 1);
		session.Invalidate();
		CHECK(provider->state->live == 0);
		REQUIRE(RunManagedRequest(session, *clone, "https://example.com/"));
		CHECK(provider->state->created == 2);
	}

	SECTION("transport reuse domains separate otherwise identical clients") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 1);
		auto session = manager->CreateSession(nullptr, nullptr);
		auto &params = session.Parameters().Cast<MockHTTPParams>();
		params.SetReuseDomain(42);
		auto clone = make_uniq<HTTPParams>(params);
		CHECK(clone->GetTransportReuseDomain() == 42);

		REQUIRE(RunManagedRequest(session, params, "https://example.com/"));
		REQUIRE(RunManagedRequest(session, *clone, "https://example.com/"));
		CHECK(provider->state->created == 1);

		params.SetReuseDomain(43);
		REQUIRE(RunManagedRequest(session, params, "https://example.com/"));
		CHECK(provider->state->created == 2);
		CHECK(provider->state->high_water == 1);
	}

	SECTION("core isolates transport settings even when the provider accepts every client") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 2);
		auto first = manager->CreateSession(nullptr, nullptr);
		auto second = manager->CreateSession(nullptr, nullptr);
		auto &first_params = first.Parameters();
		auto &second_params = second.Parameters();
		first_params.http_proxy = second_params.http_proxy = "proxy.example.com";
		first_params.http_proxy_port = second_params.http_proxy_port = 8080;
		first_params.http_proxy_username = second_params.http_proxy_username = "user";
		first_params.http_proxy_password = second_params.http_proxy_password = "password";
		first_params.override_verify_ssl = second_params.override_verify_ssl = true;
		first_params.verify_ssl = second_params.verify_ssl = true;
		REQUIRE(first_params.GetTransportReuseDomain() == second_params.GetTransportReuseDomain());
		REQUIRE(provider->state->can_reuse);
		const string url = "https://example.com/";
		REQUIRE(RunManagedRequest(first, first_params, url));
		REQUIRE(RunManagedRequest(second, second_params, url));
		CHECK(provider->state->created == 1);

		SECTION("proxy host") {
			second_params.http_proxy = "other-proxy.example.com";
		}
		SECTION("proxy port") {
			second_params.http_proxy_port++;
		}
		SECTION("proxy username") {
			second_params.http_proxy_username = "other-user";
		}
		SECTION("proxy password") {
			second_params.http_proxy_password = "other-password";
		}
		SECTION("proxy removed") {
			second_params.http_proxy.clear();
		}
		SECTION("TLS verification") {
			second_params.verify_ssl = false;
		}
		SECTION("TLS override removed") {
			second_params.override_verify_ssl = false;
		}
		REQUIRE(RunManagedRequest(second, second_params, url));
		CHECK(provider->state->created == 2);
		// The cached configuration must not follow mutations of the session's parameters.
		REQUIRE(RunManagedRequest(first, first_params, url));
		REQUIRE(RunManagedRequest(second, second_params, url));
		CHECK(provider->state->created == 2);
		CHECK(HTTPTransportManagerTestHelper::IdleKeys(*manager) == 2);
	}

	SECTION("inactive transport fields and request-local settings do not split clients") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 1);
		auto first = manager->CreateSession(nullptr, nullptr);
		auto second = manager->CreateSession(nullptr, nullptr);
		auto &params = second.Parameters();
		params.http_proxy_port = 8080;
		params.http_proxy_username = "unused-user";
		params.http_proxy_password = "unused-password";
		params.verify_ssl = false;
		params.timeout++;
		params.keep_alive = false;
		params.follow_location = false;
		params.extra_headers["Authorization"] = "request-local";
		const HTTPTransportConfig first_config(first.Parameters());
		const HTTPTransportConfig second_config(params);
		CHECK(first_config == second_config);
		CHECK(first_config.Hash() == second_config.Hash());
		CHECK(first_config.Matches(params));
		REQUIRE(RunManagedRequest(first, first.Parameters(), "https://example.com/"));
		REQUIRE(RunManagedRequest(second, params, "https://example.com/"));
		CHECK(provider->state->created == 1);
	}

	SECTION("generic failure discards only its client") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 2);
		auto session = manager->CreateSession(nullptr, nullptr);
		REQUIRE(RunManagedRequest(session, session.Parameters(), "https://healthy.example.com/"));
		provider->state->response_mode = MockResponseMode::REQUEST_ERROR;
		auto response = RunManagedRequest(session, session.Parameters(), "https://failing.example.com/");
		REQUIRE(response);
		CHECK(response->HasRequestError());
		CHECK(provider->state->live == 1);
		CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*manager) == 1);
		session.Invalidate();
		CHECK(provider->state->live == 0);
	}

	SECTION("capacity waiters sleep and reuse a client across threads") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 1);
		auto session = manager->CreateSession(nullptr, nullptr);
		auto second_params = make_uniq<HTTPParams>(session.Parameters());
		BlockMockRequests(*provider->state);

		auto first = std::async(std::launch::async, [&]() {
			return RunManagedRequest(session, session.Parameters(), "https://example.com/");
		});
		REQUIRE(WaitForMockRequests(*provider->state, 1));
		std::promise<void> second_started;
		auto second = std::async(std::launch::async, [&]() {
			second_started.set_value();
			return RunManagedRequest(session, *second_params, "https://example.com/");
		});
		second_started.get_future().wait();
		CHECK(second.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
		CHECK(provider->state->requests_started == 1);

		AllowMockRequests(*provider->state, 1);
		REQUIRE(first.get());
		REQUIRE(WaitForMockRequests(*provider->state, 2));
		AllowMockRequests(*provider->state, 2);
		REQUIRE(second.get());
		CHECK(provider->state->created == 1);
		CHECK(provider->state->high_water == 1);
		CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*manager) == 1);
	}

	SECTION("FIFO admission prevents a later exact-key request from barging") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 1);
		auto busy_session = manager->CreateSession(nullptr, nullptr);
		auto waiting_session = manager->CreateSession(nullptr, nullptr);
		auto busy_params = make_uniq<HTTPParams>(busy_session.Parameters());
		BlockMockRequests(*provider->state);

		auto holder = std::async(std::launch::async, [&]() {
			return RunManagedRequest(busy_session, busy_session.Parameters(), "https://busy.example.com/holder");
		});
		REQUIRE(WaitForMockRequests(*provider->state, 1));

		auto first_waiter = std::async(std::launch::async, [&]() {
			return RunManagedRequest(waiting_session, waiting_session.Parameters(), "https://waiting.example.com/");
		});
		REQUIRE(WaitForAdmissionWaiters(*manager, 1));

		auto later_exact_request = std::async(std::launch::async, [&]() {
			return RunManagedRequest(busy_session, *busy_params, "https://busy.example.com/later");
		});
		REQUIRE(WaitForAdmissionWaiters(*manager, 2));

		AllowMockRequests(*provider->state, 1);
		REQUIRE(holder.get());
		REQUIRE(WaitForMockRequests(*provider->state, 2));
		{
			lock_guard<mutex> guard(provider->state->request_lock);
			REQUIRE(provider->state->request_urls.size() == 2);
			CHECK(provider->state->request_urls[1] == "https://waiting.example.com/");
		}

		AllowMockRequests(*provider->state, 2);
		REQUIRE(first_waiter.get());
		REQUIRE(WaitForMockRequests(*provider->state, 3));
		{
			lock_guard<mutex> guard(provider->state->request_lock);
			REQUIRE(provider->state->request_urls.size() == 3);
			CHECK(provider->state->request_urls[2] == "https://busy.example.com/later");
		}
		AllowMockRequests(*provider->state, 3);
		REQUIRE(later_exact_request.get());
		CHECK(provider->state->high_water == 1);
	}

	SECTION("FIFO admission hands off after client creation fails") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 1);
		auto holding_session = manager->CreateSession(nullptr, nullptr);
		auto failing_session = manager->CreateSession(nullptr, nullptr);
		auto succeeding_session = manager->CreateSession(nullptr, nullptr);
		BlockMockRequests(*provider->state);

		auto holder = std::async(std::launch::async, [&]() {
			return RunManagedRequest(holding_session, holding_session.Parameters(), "https://holder.example.com/");
		});
		REQUIRE(WaitForMockRequests(*provider->state, 1));
		auto failing_waiter = std::async(std::launch::async, [&]() {
			return RunManagedRequest(failing_session, failing_session.Parameters(), "https://failing.example.com/");
		});
		REQUIRE(WaitForAdmissionWaiters(*manager, 1));
		auto succeeding_waiter = std::async(std::launch::async, [&]() {
			return RunManagedRequest(succeeding_session, succeeding_session.Parameters(),
			                         "https://succeeding.example.com/");
		});
		REQUIRE(WaitForAdmissionWaiters(*manager, 2));

		provider->state->creation_failures = 1;
		AllowMockRequests(*provider->state, 1);
		REQUIRE(holder.get());
		CHECK_THROWS_AS(failing_waiter.get(), IOException);
		REQUIRE(WaitForMockRequests(*provider->state, 2));
		{
			lock_guard<mutex> guard(provider->state->request_lock);
			REQUIRE(provider->state->request_urls.size() == 2);
			CHECK(provider->state->request_urls[1] == "https://succeeding.example.com/");
		}
		AllowMockRequests(*provider->state, 2);
		REQUIRE(succeeding_waiter.get());
		CHECK(HTTPTransportManagerTestHelper::AdmissionWaiters(*manager) == 0);
		CHECK(provider->state->high_water == 1);
	}

	SECTION("exact keys reuse clients and local sessions remain separate") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SESSION_LOCAL);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 2);
		auto first_session = manager->CreateSession(nullptr, nullptr);
		auto first_clone = make_uniq<HTTPParams>(first_session.Parameters());
		BlockMockRequests(*provider->state);

		auto first = std::async(std::launch::async, [&]() {
			return RunManagedRequest(first_session, first_session.Parameters(), "https://example.com/");
		});
		REQUIRE(WaitForMockRequests(*provider->state, 1));
		auto second = std::async(std::launch::async, [&]() {
			return RunManagedRequest(first_session, *first_clone, "https://example.com/");
		});
		REQUIRE(WaitForMockRequests(*provider->state, 2));
		AllowMockRequests(*provider->state, 1);
		REQUIRE(first.get());
		AllowMockRequests(*provider->state, 2);
		REQUIRE(second.get());
		AllowMockRequests(*provider->state, NumericLimits<idx_t>::Maximum());
		CHECK(HTTPTransportManagerTestHelper::IdleClients(*manager) == 2);
		CHECK(HTTPTransportManagerTestHelper::IdleKeys(*manager) == 1);

		REQUIRE(RunManagedRequest(first_session, first_session.Parameters(), "https://example.com/"));
		CHECK(provider->state->last_initialized_client >= 1);
		CHECK(provider->state->last_initialized_client <= 2);
		CHECK(provider->state->created == 2);
		auto second_session = manager->CreateSession(nullptr, nullptr);
		REQUIRE(RunManagedRequest(second_session, second_session.Parameters(), "https://example.com/"));
		CHECK(provider->state->created == 3);
		CHECK(provider->state->high_water == 2);
	}

	SECTION("shared clients cross sessions and idle keys remain bounded during origin churn") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 2);
		auto first_session = manager->CreateSession(nullptr, nullptr);
		auto second_session = manager->CreateSession(nullptr, nullptr);
		REQUIRE(RunManagedRequest(first_session, first_session.Parameters(), "https://shared.example.com/"));
		REQUIRE(RunManagedRequest(second_session, second_session.Parameters(), "https://shared.example.com/"));
		CHECK(provider->state->created == 1);

		for (idx_t index = 0; index < 20; index++) {
			REQUIRE(RunManagedRequest(first_session, first_session.Parameters(),
			                          "https://origin" + to_string(index) + ".example.com/"));
			CHECK(HTTPTransportManagerTestHelper::IdleKeys(*manager) <= 2);
			CHECK(HTTPTransportManagerTestHelper::IdleKeys(*manager) ==
			      HTTPTransportManagerTestHelper::IdleClients(*manager));
			CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*manager) == 2);
		}
		CHECK(provider->state->high_water == 2);
		CHECK(provider->state->created == 21);
		CHECK(provider->state->destroyed == 19);
	}

	SECTION("creation compatibility initialization and cleanup failures release capacity") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 1);
		auto session = manager->CreateSession(nullptr, nullptr);

		provider->state->throw_on_create = true;
		CHECK_THROWS_AS(RunManagedRequest(session, session.Parameters(), "https://example.com/"), IOException);
		CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*manager) == 0);
		provider->state->throw_on_create = false;
		REQUIRE(RunManagedRequest(session, session.Parameters(), "https://example.com/"));

		provider->state->can_reuse = false;
		REQUIRE(RunManagedRequest(session, session.Parameters(), "https://example.com/"));
		CHECK(provider->state->created == 2);
		CHECK(provider->state->high_water == 1);
		provider->state->can_reuse = true;
		provider->state->throw_on_can_reuse = true;
		CHECK_THROWS_AS(RunManagedRequest(session, session.Parameters(), "https://example.com/"), IOException);
		CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*manager) == 0);
		provider->state->throw_on_can_reuse = false;
		REQUIRE(RunManagedRequest(session, session.Parameters(), "https://example.com/"));

		provider->state->throw_on_initialize = true;
		CHECK_THROWS_AS(RunManagedRequest(session, session.Parameters(), "https://example.com/"), IOException);
		CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*manager) == 0);
		provider->state->throw_on_initialize = false;
		provider->state->throw_on_cleanup = true;
		REQUIRE(RunManagedRequest(session, session.Parameters(), "https://example.com/"));
		CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*manager) == 0);
		CHECK(provider->state->high_water == 1);
	}

	SECTION("null-client handling follows the provider policy") {
		auto reusable_provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		reusable_provider->state->return_null = true;
		auto reusable_manager = HTTPTransportManagerTestHelper::Create(reusable_provider, 1);
		auto reusable_session = reusable_manager->CreateSession(nullptr, nullptr);
		CHECK_THROWS_AS(RunManagedRequest(reusable_session, reusable_session.Parameters(), "https://example.com/"),
		                InvalidConfigurationException);
		CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*reusable_manager) == 0);

		auto ephemeral_provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::EPHEMERAL);
		ephemeral_provider->state->return_null = true;
		auto ephemeral_manager = HTTPTransportManagerTestHelper::Create(ephemeral_provider, 1);
		auto ephemeral_session = ephemeral_manager->CreateSession(nullptr, nullptr);
		REQUIRE(RunManagedRequest(ephemeral_session, ephemeral_session.Parameters(), "https://example.com/"));
		CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*ephemeral_manager) == 0);
	}

	SECTION("provider publication and invalidation reject active stale returns") {
		auto first_provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED, "first");
		auto second_provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED, "second");
		auto manager = HTTPTransportManagerTestHelper::Create(first_provider, 2);
		auto first_session = manager->CreateSession(nullptr, nullptr);
		BlockMockRequests(*first_provider->state);
		auto request = std::async(std::launch::async, [&]() {
			return RunManagedRequest(first_session, first_session.Parameters(), "https://example.com/");
		});
		REQUIRE(WaitForMockRequests(*first_provider->state, 1));
		HTTPTransportManagerTestHelper::SetHTTPUtil(*manager, second_provider);
		AllowMockRequests(*first_provider->state, 1);
		REQUIRE(request.get());
		CHECK(first_provider->state->live == 0);

		auto second_session = manager->CreateSession(nullptr, nullptr);
		BlockMockRequests(*second_provider->state);
		auto invalidated_request = std::async(std::launch::async, [&]() {
			return RunManagedRequest(second_session, second_session.Parameters(), "https://example.com/");
		});
		REQUIRE(WaitForMockRequests(*second_provider->state, 1));
		second_session.Invalidate();
		AllowMockRequests(*second_provider->state, 1);
		REQUIRE(invalidated_request.get());
		CHECK(second_provider->state->live == 0);
		CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*manager) == 0);
	}

	SECTION("stale invalidation does not discard clients from a newer connection epoch") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 2);
		auto session = manager->CreateSession(nullptr, nullptr);
		REQUIRE(RunManagedRequest(session, session.Parameters(), "https://example.com/"));

		bool nested_request_succeeded = false;
		std::atomic<bool> invalidated {false};
		provider->state->destroy_callback = [&]() {
			if (!invalidated.exchange(true)) {
				session.Invalidate();
				nested_request_succeeded =
				    bool(RunManagedRequest(session, session.Parameters(), "https://example.com/"));
			}
		};
		session.Invalidate();
		provider->state->destroy_callback = nullptr;

		CHECK(nested_request_succeeded);
		CHECK(provider->state->created == 2);
		CHECK(provider->state->destroyed == 1);
		CHECK(provider->state->live == 1);
		CHECK(HTTPTransportManagerTestHelper::IdleClients(*manager) == 1);
		CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*manager) == 1);
	}

	SECTION("capacity-owning callbacks fail nested waits without deadlocking") {
		for (idx_t callback_index = 0; callback_index < 5; callback_index++) {
			auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
			auto manager = HTTPTransportManagerTestHelper::Create(provider, 1);
			auto session = manager->CreateSession(nullptr, nullptr);
			if (callback_index != 0) {
				REQUIRE(RunManagedRequest(session, session.Parameters(), "https://example.com/"));
			}

			std::atomic<bool> invoked {false};
			auto callback = [&]() {
				if (!invoked.exchange(true)) {
					CHECK_THROWS_AS(RunManagedRequest(session, session.Parameters(), "https://nested.example.com/"),
					                InvalidInputException);
				}
			};
			switch (callback_index) {
			case 0:
				provider->state->create_callback = callback;
				break;
			case 1:
				provider->state->reuse_callback = callback;
				break;
			case 2:
				provider->state->initialize_callback = callback;
				break;
			case 3:
				provider->state->cleanup_callback = callback;
				break;
			case 4:
				provider->state->destroy_callback = callback;
				break;
			default:
				FAIL("unreachable callback kind");
			}

			if (callback_index == 4) {
				session.Invalidate();
			} else {
				REQUIRE(RunManagedRequest(session, session.Parameters(), "https://example.com/"));
			}
			CHECK(invoked);
		}
	}

	SECTION("shutdown wakes capacity waiters") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 1);
		auto session = manager->CreateSession(nullptr, nullptr);
		auto second_params = make_uniq<HTTPParams>(session.Parameters());
		auto third_params = make_uniq<HTTPParams>(session.Parameters());
		BlockMockRequests(*provider->state);
		auto first = std::async(std::launch::async, [&]() {
			return RunManagedRequest(session, session.Parameters(), "https://example.com/");
		});
		REQUIRE(WaitForMockRequests(*provider->state, 1));
		auto first_waiter = std::async(
		    std::launch::async, [&]() { return RunManagedRequest(session, *second_params, "https://example.com/"); });
		REQUIRE(WaitForAdmissionWaiters(*manager, 1));
		auto second_waiter = std::async(
		    std::launch::async, [&]() { return RunManagedRequest(session, *third_params, "https://example.com/"); });
		REQUIRE(WaitForAdmissionWaiters(*manager, 2));
		HTTPTransportManagerTestHelper::Close(*manager);
		CHECK_THROWS_AS(manager->CreateSession(nullptr, nullptr), InvalidInputException);
		CHECK_THROWS_AS(HTTPTransportManagerTestHelper::SetHTTPUtil(*manager, provider), InvalidInputException);
		CHECK_THROWS_AS(first_waiter.get(), InvalidInputException);
		CHECK_THROWS_AS(second_waiter.get(), InvalidInputException);
		CHECK(HTTPTransportManagerTestHelper::AdmissionWaiters(*manager) == 0);
		AllowMockRequests(*provider->state, 1);
		REQUIRE(first.get());
		CHECK(provider->state->live == 0);
		CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(*manager) == 0);
	}

	SECTION("parameters cannot cross manager identity") {
		auto first_provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::EPHEMERAL);
		auto second_provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::EPHEMERAL);
		auto first_manager = HTTPTransportManagerTestHelper::Create(first_provider, 1);
		auto second_manager = HTTPTransportManagerTestHelper::Create(second_provider, 1);
		auto first_session = first_manager->CreateSession(nullptr, nullptr);
		auto second_session = second_manager->CreateSession(nullptr, nullptr);
		CHECK_THROWS_AS(RunManagedRequest(first_session, second_session.Parameters(), "https://example.com/"),
		                InvalidInputException);
	}

	SECTION("client-free requests still validate shutdown admission") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::CLIENT_FREE);
		auto manager = HTTPTransportManagerTestHelper::Create(provider, 1);
		auto session = manager->CreateSession(nullptr, nullptr);
		HTTPTransportManagerTestHelper::Close(*manager);
		CHECK_THROWS_AS(RunManagedRequest(session, session.Parameters(), "https://example.com/"),
		                InvalidInputException);
	}
}

static string ReadExtensionBinary(const string &path) {
	LocalFileSystem fs;
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto size = NumericCast<idx_t>(handle->GetFileSize());
	string result(size, '\0');
	handle->Read(data_ptr_cast(result.data()), size, 0);
	return result;
}

static string CaptureExceptionMessage(const std::function<void()> &action) {
	try {
		action();
	} catch (std::exception &exception) {
		return exception.what();
	}
	return string();
}

TEST_CASE("Core extension downloads use managed HTTP transports", "[http_transport_manager]") {
	auto extension_directory = TestCreatePath("http_transport_manager_extensions");
	TestDeleteDirectory(extension_directory);
	DBConfig config;
	config.SetOptionByName("allow_unsigned_extensions", true);
	config.SetOptionByName("extension_directory", extension_directory);
	DuckDB db(nullptr, &config);
	Connection connection(db);
	auto build_directory = TestConfiguration::Get().GetTestEnv("BUILD_DIR", "build/reldebug");
	const string extension_filename = "loadable_extension_demo.duckdb_extension";
	const string extension_url = "http://mock.test/" + extension_filename;
	auto extension_body = ReadExtensionBinary(build_directory + "/test/extension/" + extension_filename);

	SECTION("client-context and database openers preserve provider capture") {
		auto first = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED, "first");
		auto second = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED, "second");
		first->state->response_body = extension_body;
		second->state->response_body = extension_body;
		db.instance->config.SetHTTPUtil(first);
		std::atomic<bool> republished {false};
		first->state->callback = [&]() {
			if (!republished.exchange(true)) {
				db.instance->config.SetHTTPUtil(second);
			}
		};

		ExtensionInstallOptions options;
		options.force_install = true;
		options.use_etags = true;
		auto context_info = ExtensionHelper::InstallExtension(*connection.context, extension_url, options);
		first->state->callback = nullptr;
		REQUIRE(context_info);
		CHECK(context_info->etag == "mock-etag");
		CHECK(first->state->initialized_paths == vector<string> {extension_url});
		CHECK(first->state->initialized_with_context == vector<bool> {true});
		CHECK(first->state->created == 1);
		CHECK(first->state->live == 0);
		CHECK(second->state->created == 0);

		second->state->response_mode = MockResponseMode::NOT_MODIFIED;
		auto &local_fs = FileSystem::GetLocal(*db.instance);
		auto database_info = ExtensionHelper::InstallExtension(*db.instance, local_fs, extension_url, options);
		REQUIRE(database_info);
		CHECK(database_info->etag == "mock-etag");
		CHECK(second->state->initialized_paths == vector<string> {extension_url});
		CHECK(second->state->initialized_with_context == vector<bool> {false});
		CHECK(second->state->observed_if_none_match == "mock-etag");
		CHECK(second->state->created == 1);
		CHECK(second->state->live == 1);
	}

	SECTION("retry replaces the client before a successful attempt") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		provider->state->response_body = extension_body;
		provider->state->retries = 1;
		db.instance->config.SetHTTPUtil(provider);
		ExtensionInstallOptions options;
		options.force_install = true;

		provider->state->response_sequence = {MockResponseMode::REQUEST_ERROR, MockResponseMode::SUCCESS};
		REQUIRE(ExtensionHelper::InstallExtension(*connection.context, extension_url, options));
		CHECK(provider->state->response_attempt == 2);
		CHECK(provider->state->created == 2);
		CHECK(provider->state->destroyed == 1);
		CHECK(provider->state->high_water == 1);

		provider->state->response_attempt = 0;
		provider->state->response_sequence = {MockResponseMode::CLIENT_THROW, MockResponseMode::SUCCESS};
		REQUIRE(ExtensionHelper::InstallExtension(*connection.context, extension_url, options));
		CHECK(provider->state->response_attempt == 2);
		CHECK(provider->state->created == 3);
		CHECK(provider->state->destroyed == 2);
		CHECK(provider->state->high_water == 1);

		provider->state->response_attempt = 0;
		provider->state->response_sequence = {MockResponseMode::RETRYABLE_HTTP_ERROR, MockResponseMode::SUCCESS};
		REQUIRE(ExtensionHelper::InstallExtension(*connection.context, extension_url, options));
		CHECK(provider->state->response_attempt == 2);
		CHECK(provider->state->created == 4);
		CHECK(provider->state->destroyed == 3);
		CHECK(provider->state->bypass_initializations == 4);
		CHECK(provider->state->high_water == 1);

		provider->state->response_attempt = 0;
		provider->state->response_sequence = {MockResponseMode::REQUEST_ERROR, MockResponseMode::SUCCESS};
		provider->state->return_null_after_first = true;
		auto null_retry_error = CaptureExceptionMessage(
		    [&]() { ExtensionHelper::InstallExtension(*connection.context, extension_url, options); });
		CHECK(StringUtil::Contains(null_retry_error, "HTTP provider returned a null client during retry"));
		CHECK(provider->state->response_attempt == 1);
		CHECK(provider->state->created == 4);
		CHECK(provider->state->destroyed == 4);
		CHECK(provider->state->live == 0);

		provider->state->response_attempt = 0;
		provider->state->response_sequence = {MockResponseMode::SUCCESS};
		provider->state->return_null_after_first = false;
		REQUIRE(ExtensionHelper::InstallExtension(*connection.context, extension_url, options));
		CHECK(provider->state->created == 5);
		CHECK(provider->state->high_water == 1);
	}

	SECTION("transport failures invalidate shared clients") {
		auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::SHARED);
		provider->state->response_body = extension_body;
		db.instance->config.SetHTTPUtil(provider);
		ExtensionInstallOptions options;
		options.force_install = true;
		REQUIRE(ExtensionHelper::InstallExtension(*connection.context, extension_url, options));
		CHECK(provider->state->live == 1);

		provider->state->response_mode = MockResponseMode::NULL_RESPONSE;
		auto null_error = CaptureExceptionMessage([&]() {
			ExtensionHelper::InstallExtension(*connection.context, "http://mock.test/null.duckdb_extension", options);
		});
		CHECK(StringUtil::Contains(null_error, "HTTP provider returned no response"));
		CHECK(StringUtil::Contains(null_error, "null.duckdb_extension"));
		CHECK(provider->state->live == 0);
		CHECK(HTTPTransportManagerTestHelper::OccupiedSlots(db.instance->config.GetHTTPTransportManager()) == 0);

		provider->state->response_mode = MockResponseMode::REQUEST_ERROR;
		auto request_error = CaptureExceptionMessage([&]() {
			ExtensionHelper::InstallExtension(*connection.context, "http://mock.test/request_error.duckdb_extension",
			                                  options);
		});
		CHECK(StringUtil::Contains(request_error, "\"exception_type\":\"IO\""));
		CHECK(StringUtil::Contains(request_error, "request_error.duckdb_extension"));
		CHECK(StringUtil::Contains(request_error, "mock request error"));
		CHECK(provider->state->live == 0);

		provider->state->response_mode = MockResponseMode::THROW;
		auto throw_error = CaptureExceptionMessage([&]() {
			ExtensionHelper::InstallExtension(*connection.context, "http://mock.test/throw.duckdb_extension", options);
		});
		CHECK(StringUtil::Contains(throw_error, "mock send failure"));
		CHECK(provider->state->live == 0);

		provider->state->response_mode = MockResponseMode::HTTP_ERROR;
		auto http_error = CaptureExceptionMessage([&]() {
			ExtensionHelper::InstallExtension(*connection.context, "http://mock.test/not_found.duckdb_extension",
			                                  options);
		});
		CHECK(StringUtil::Contains(http_error, "\"exception_type\":\"HTTP\""));
		CHECK(StringUtil::Contains(http_error, "not_found.duckdb_extension"));
		CHECK(StringUtil::Contains(http_error, "\"status_code\":\"404\""));
		CHECK(provider->state->live == 1);
	}

	TestDeleteDirectory(extension_directory);
}

TEST_CASE("A linked httplib client is the default HTTP provider", "[http_transport_manager]") {
	// statically linked extensions, such as httpfs, may replace the provider when they load
	DBConfig config;
	config.options.load_extensions = false;
	DuckDB db(nullptr, &config);
	Connection connection(db);
	auto &http_util = HTTPUtil::Get(*db.instance);
	bool linked = false;
	for (auto &entry : db.instance->config.linked_extensions) {
		linked = linked || entry.name == "httplib";
	}
	CHECK(http_util.GetName() == (linked ? "Built-In" : "none"));
	if (!linked) {
		return;
	}
	auto result = connection.Query("SELECT count(*) FROM duckdb_extensions() WHERE extension_name = 'httplib'");
	REQUIRE_NO_FAIL(*result);
	CHECK(CHECK_COLUMN(result, 0, {0}));
}

class NoClientHTTPUtil : public HTTPUtil {
public:
	string GetName() const override {
		return "none";
	}
	unique_ptr<HTTPClient> InitializeClient(HTTPParams &, const string &) override {
		return nullptr;
	}
};

TEST_CASE("Extension downloads without an HTTP client", "[http_transport_manager]") {
	auto extension_directory = TestCreatePath("http_transport_manager_no_client");
	TestDeleteDirectory(extension_directory);
	DBConfig config;
	config.SetOptionByName("extension_directory", extension_directory);
	DuckDB db(nullptr, &config);
	Connection connection(db);
	db.instance->config.SetHTTPUtil(make_shared_ptr<NoClientHTTPUtil>());

	ExtensionInstallOptions options;
	options.force_install = true;
	const string extension_url = "http://mock.test/no_client.duckdb_extension";
	auto error = CaptureExceptionMessage(
	    [&]() { ExtensionHelper::InstallExtension(*connection.context, extension_url, options); });
	CHECK(StringUtil::Contains(error, "HTTP provider 'none' has no HTTP client"));
	CHECK(StringUtil::Contains(error, extension_url));
	CHECK(StringUtil::Contains(error, "ENABLE_BUILTIN_HTTPLIB=ON"));
	TestDeleteDirectory(extension_directory);
}

TEST_CASE("HTTP transport manager enforces enable_external_access", "[http_transport_manager]") {
	auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::EPHEMERAL);

	// with external access enabled (the default), the request goes through to the provider
	{
		DuckDB db(nullptr);
		db.instance->config.SetHTTPUtil(provider);
		auto &manager = db.instance->config.GetHTTPTransportManager();
		auto session = manager.CreateSession(*db.instance, "https://example.com/");
		REQUIRE(RunManagedRequest(session, session.Parameters(), "https://example.com/"));
	}

	// with external access disabled, the request is rejected before it is sent, even though it does not go through
	// the file system
	{
		DuckDB db(nullptr);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET enable_external_access=false"));
		db.instance->config.SetHTTPUtil(provider);
		auto &manager = db.instance->config.GetHTTPTransportManager();
		auto session = manager.CreateSession(*db.instance, "https://example.com/");
		CHECK_THROWS_AS(RunManagedRequest(session, session.Parameters(), "https://example.com/"), PermissionException);
	}

	// a caller that performs its own access control (e.g. extension installation) can skip the check
	{
		DuckDB db(nullptr);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("SET enable_external_access=false"));
		db.instance->config.SetHTTPUtil(provider);
		auto &manager = db.instance->config.GetHTTPTransportManager();
		auto session = manager.CreateSession(*db.instance, "https://example.com/");
		auto &params = session.Parameters();
		params.skip_external_access_check = true;
		REQUIRE(RunManagedRequest(session, params, "https://example.com/"));
	}
}

TEST_CASE("Extension downloads respect enable_external_access", "[http_transport_manager]") {
	auto extension_directory = TestCreatePath("http_transport_manager_external_access");
	TestDeleteDirectory(extension_directory);
	TestCreateDirectory(extension_directory);
	DBConfig config;
	config.SetOptionByName("extension_directory", extension_directory);
	DuckDB db(nullptr, &config);
	Connection connection(db);
	auto provider = make_shared_ptr<MockHTTPUtil>(HTTPTransportReusePolicy::EPHEMERAL);
	db.instance->config.SetHTTPUtil(provider);
	REQUIRE_NO_FAIL(
	    connection.Query("SET allowed_directories=['" + extension_directory + "', 'http://mock.test/allowed/']"));
	REQUIRE_NO_FAIL(connection.Query("SET enable_external_access=false"));

	// returns whether the install was denied by the access check
	auto install_denied = [&](const string &url) {
		ExtensionInstallOptions options;
		options.force_install = true;
		try {
			ExtensionHelper::InstallExtension(*connection.context, url, options);
		} catch (PermissionException &ex) {
			// the extension directory is allowed, so only the url can be denied
			REQUIRE(StringUtil::Contains(ex.what(), "Cannot download extension"));
			return true;
		} catch (std::exception &) {
			return false;
		}
		return false;
	};
	auto requested = [&](const string &url) {
		auto &paths = provider->state->initialized_paths;
		return std::find(paths.begin(), paths.end(), url) != paths.end();
	};

	SECTION("urls outside of allowed_directories are rejected before any request is made") {
		const vector<string> rejected_urls {"http://mock.test/ext.duckdb_extension",
		                                    "http://mock.test/allowed_not/ext.duckdb_extension",
		                                    "http://extensions.duckdb.org.mock.test/ext.duckdb_extension",
		                                    "http://mock.test/extensions.duckdb.org/ext.duckdb_extension",
		                                    "http://extensions.duckdb.org@mock.test/ext.duckdb_extension",
		                                    "http://mock.test/allowed/../ext.duckdb_extension",
		                                    "http://mock.test/allowed/%2E%2E/ext.duckdb_extension"};
		for (auto &url : rejected_urls) {
			CHECK(install_denied(url));
		}
		CHECK(provider->state->initialized_paths.empty());
	}
	SECTION("a custom repository outside of allowed_directories is rejected") {
		REQUIRE_NO_FAIL(connection.Query("SET custom_extension_repository='http://mock.test/repository'"));
		auto result = connection.Query("INSTALL ext");
		REQUIRE(result->HasError());
		CHECK(result->GetErrorType() == ExceptionType::PERMISSION);
		result = connection.Query("INSTALL ext FROM 'http://mock.test/repository'");
		REQUIRE(result->HasError());
		CHECK(result->GetErrorType() == ExceptionType::PERMISSION);
		CHECK(provider->state->initialized_paths.empty());
	}
	SECTION("urls inside allowed_directories are requested") {
		const string url = "http://mock.test/allowed/ext.duckdb_extension";
		CHECK(!install_denied(url));
		CHECK(requested(url));
	}
	SECTION("the official repositories are requested") {
		const vector<string> official_urls {"http://extensions.duckdb.org/v/osx/ext.duckdb_extension",
		                                    "http://community-extensions.duckdb.org/v/osx/ext.duckdb_extension",
		                                    "http://extensions.duckdb-backup.org/v/osx/ext.duckdb_extension"};
		for (auto &url : official_urls) {
			CHECK(!install_denied(url));
			CHECK(requested(url));
		}
	}
	TestDeleteDirectory(extension_directory);
}

} // namespace duckdb
