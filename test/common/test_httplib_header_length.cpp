#include "catch.hpp"
#include "httplib.hpp"

#include <thread>

using namespace duckdb_httplib;

TEST_CASE("A response header line longer than 8 KiB is read", "[httplib]") {
	// github.com answers with a content-security-policy header of over 9 KiB, which httplib's default
	// header line limit rejected as a read error
	const std::string long_value(12 * 1024, 'x');
	Server server;
	server.Get("/", [&](const Request &, Response &response) {
		response.set_header("X-Long", long_value);
		response.set_content("ok", "text/plain");
	});
	auto port = server.bind_to_any_port("127.0.0.1");
	REQUIRE(port > 0);
	std::thread listener([&]() { server.listen_after_bind(); });
	detail::scope_exit cleanup([&]() {
		server.stop();
		listener.join();
	});
	server.wait_until_ready();

	Client client("127.0.0.1", port);
	auto result = client.Get("/");
	REQUIRE(result);
	REQUIRE(result->status == 200);
	REQUIRE(result->get_header_value("X-Long") == long_value);
	REQUIRE(result->body == "ok");
}
