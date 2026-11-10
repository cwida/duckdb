#include "catch.hpp"
#include "httplib.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace duckdb_httplib;

TEST_CASE("Cross-host redirects preserve the HTTP request timeout", "[httplib]") {
	std::atomic<bool> target_hit {false};
	Server target;
	target.Get("/target", [&](const Request &request, Response &response) {
		target_hit = true;
		for (duckdb::idx_t i = 0; i < 100 && !request.is_connection_closed(); i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		response.set_content("late", "text/plain");
	});
	auto target_port = target.bind_to_any_port("127.0.0.1");
	REQUIRE(target_port > 0);

	Server origin;
	origin.Get("/redirect", [&](const Request &, Response &response) {
		response.set_redirect("http://127.0.0.1:" + std::to_string(target_port) + "/target", 302);
	});
	auto origin_port = origin.bind_to_any_port("127.0.0.1");
	REQUIRE(origin_port > 0);
	std::thread target_listener([&]() { target.listen_after_bind(); });
	detail::scope_exit target_cleanup([&]() {
		target.stop();
		target_listener.join();
	});
	std::thread origin_listener([&]() { origin.listen_after_bind(); });
	detail::scope_exit origin_cleanup([&]() {
		origin.stop();
		origin_listener.join();
	});
	target.wait_until_ready();
	origin.wait_until_ready();

	Client client("127.0.0.1", origin_port);
	client.set_follow_location(true);
	client.set_max_timeout(std::chrono::milliseconds(200));
	auto result = client.Get("/redirect");
	CHECK(target_hit);
	REQUIRE(!result);
	CHECK(result.error() == Error::Read);
}
