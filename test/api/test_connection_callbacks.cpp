#include "catch.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <condition_variable>
#include <thread>

using namespace duckdb;

namespace {

// SQL cannot register connection callbacks or synchronize them with host-side connection destruction.
class DrainingCloseCallback : public ExtensionCallback {
public:
	explicit DrainingCloseCallback(ClientContext &subject_p) : subject(&subject_p) {
	}

	~DrainingCloseCallback() override {
		Join();
	}

	void OnConnectionClosed(ClientContext &context) override {
		if (&context == subject) {
			old_count = ConnectionManager::Get(context).GetConnectionCount();
		}
	}

	void OnConnectionUnregistered(ClientContext &context) override {
		if (&context != subject) {
			return;
		}
		called = true;
		auto &manager = ConnectionManager::Get(context);
		new_count = manager.GetConnectionCount();
		helper = std::thread([this, db = context.db]() {
			try {
				{
					Connection user(*db);
					user_opened = true;
					auto internal = Connection::CreateInternal(*db);
					auto result = internal.Query("SELECT i FROM attached.t");
					if (result->HasError()) {
						helper_failed = true;
					} else {
						auto chunk = result->Fetch();
						helper_failed = !chunk || chunk->size() != 1 || chunk->GetValue(0, 0) != Value::BIGINT(42);
					}
				}
			} catch (...) {
				helper_failed = true;
			}
			std::lock_guard<std::mutex> guard(lock);
			done = true;
			cv.notify_all();
		});
		{
			std::unique_lock<std::mutex> guard(lock);
			drained = cv.wait_for(guard, std::chrono::seconds(5), [&]() { return done; });
		}
		// Join outside the callback. If the manager lock is held, the wait fails without hanging the test.
		if (drained && !helper_failed) {
			for (auto &connection : manager.GetConnectionList()) {
				visible = visible || RefersToSameObject(*connection, context);
			}
		}
		completed = true;
	}

	void Join() {
		if (helper.joinable()) {
			helper.join();
		}
	}

	ClientContext *subject;
	bool called = false;
	bool drained = false;
	bool visible = false;
	bool user_opened = false;
	bool helper_failed = false;
	std::atomic<bool> completed {false};
	idx_t old_count = 0;
	idx_t new_count = 0;

private:
	std::thread helper;
	std::mutex lock;
	std::condition_variable cv;
	bool done = false;
};

class OrderedCallback : public ExtensionCallback {
public:
	OrderedCallback(vector<string> &events_p, string name_p) : events(events_p), name(std::move(name_p)) {
	}

	void OnConnectionOpened(ClientContext &context) override {
		Record(context, "open");
	}

	void OnConnectionClosed(ClientContext &context) override {
		Record(context, "close");
		if (register_on_close) {
			ExtensionCallback::Register(DBConfig::GetConfig(context), std::move(register_on_close));
		}
	}

	void OnConnectionUnregistered(ClientContext &context) override {
		Record(context, "unregistered");
	}

	void Record(ClientContext &context, const string &event) {
		events.push_back(name + ":" + event + ":" +
		                 std::to_string(ConnectionManager::Get(context).GetConnectionCount()));
	}

	vector<string> &events;
	string name;
	shared_ptr<ExtensionCallback> register_on_close;
};

class ThrowingConnectionCallback : public ExtensionCallback {
public:
	void OnConnectionOpened(ClientContext &context) override {
		if (context.GetConnectionType() == ConnectionType::USER && !called) {
			called = true;
			throw InvalidInputException("connection callback rejected open");
		}
	}

	bool called = false;
};

} // namespace

TEST_CASE("Post-unregistration callbacks can drain connection work", "[api][connection_callbacks]") {
	DBConfig config;
	config.options.load_extensions = false;
	DuckDB db(nullptr, &config);
	Connection anchor(db);
	REQUIRE_NO_FAIL(anchor.Query("ATTACH ':memory:' AS attached"));
	REQUIRE_NO_FAIL(anchor.Query("CREATE TABLE attached.t AS SELECT 42::BIGINT AS i"));
	auto connection = make_uniq<Connection>(db);
	auto retained_context = connection->context;
	auto callback = make_shared_ptr<DrainingCloseCallback>(*retained_context);
	ExtensionCallback::Register(DBConfig::GetConfig(*db.instance), callback);

	connection.reset();
	auto completed_before_join = callback->completed.load();
	callback->Join();

	REQUIRE(callback->called);
	CHECK(completed_before_join);
	CHECK(callback->drained);
	CHECK(callback->user_opened);
	CHECK_FALSE(callback->helper_failed);
	CHECK(callback->old_count == 2);
	CHECK(callback->new_count == 1);
	CHECK_FALSE(callback->visible);
	CHECK(ConnectionManager::Get(*db.instance).GetConnectionCount() == 1);
	// The callback has drained even though its context is retained past Connection destruction.
	REQUIRE_NO_FAIL(retained_context->Query("SELECT * FROM attached.t", QueryParameters()));
}

TEST_CASE("Both close phases use the same callback snapshot", "[api][connection_callbacks]") {
	vector<string> events;
	DBConfig config;
	config.options.load_extensions = false;
	DuckDB db(nullptr, &config);
	Connection anchor(db);
	auto first = make_shared_ptr<OrderedCallback>(events, "a");
	auto second = make_shared_ptr<OrderedCallback>(events, "b");
	first->register_on_close = make_shared_ptr<OrderedCallback>(events, "c");
	ExtensionCallback::Register(DBConfig::GetConfig(*db.instance), first);
	ExtensionCallback::Register(DBConfig::GetConfig(*db.instance), second);
	{ Connection connection(db); }
	vector<string> expected {"a:open:1", "b:open:1", "a:close:2", "b:close:2", "a:unregistered:1", "b:unregistered:1"};
	CHECK(events == expected);
	events.clear();
	{ Connection connection(db); }
	expected = {"a:open:1",  "b:open:1",         "c:open:1",         "a:close:2",       "b:close:2",
	            "c:close:2", "a:unregistered:1", "b:unregistered:1", "c:unregistered:1"};
	CHECK(events == expected);
}

TEST_CASE("An opening callback exception does not register the connection", "[api][connection_callbacks]") {
	DBConfig config;
	config.options.load_extensions = false;
	DuckDB db(nullptr, &config);
	Connection anchor(db);
	auto callback = make_shared_ptr<ThrowingConnectionCallback>();
	ExtensionCallback::Register(DBConfig::GetConfig(*db.instance), callback);
	REQUIRE_THROWS_AS(make_uniq<Connection>(db), InvalidInputException);
	CHECK(ConnectionManager::Get(*db.instance).GetConnectionCount() == 1);
	CHECK(ConnectionManager::Get(*db.instance).GetConnectionList().size() == 1);
	Connection next(db);
	CHECK(ConnectionManager::Get(*db.instance).GetConnectionCount() == 2);
	REQUIRE_NO_FAIL(next.Query("SELECT 42"));
}
