#include "catch.hpp"

#if defined(__linux__)
#include "httplib.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

TEST_CASE("Writing a request to a closed connection fails without SIGPIPE", "[httplib]") {
	int server = socket(AF_INET, SOCK_STREAM, 0);
	REQUIRE(server >= 0);
	std::thread closer;
	sighandler_t previous_handler = SIG_ERR;
	duckdb_httplib::detail::scope_exit cleanup([&]() {
		if (previous_handler != SIG_ERR) {
			signal(SIGPIPE, previous_handler);
		}
		// unblocks the accept when the client never connected
		shutdown(server, SHUT_RDWR);
		if (closer.joinable()) {
			closer.join();
		}
		close(server);
	});
	sockaddr_in address {};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	REQUIRE(bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
	REQUIRE(listen(server, 1) == 0);
	socklen_t address_length = sizeof(address);
	REQUIRE(getsockname(server, reinterpret_cast<sockaddr *>(&address), &address_length) == 0);
	// a server that reads the start of the request and closes the connection without a response
	closer = std::thread([server]() {
		int connection = accept(server, nullptr, nullptr);
		if (connection >= 0) {
			char buffer[1024];
			recv(connection, buffer, sizeof(buffer), 0);
			close(connection);
		}
	});

	// the default action of SIGPIPE terminates the process
	previous_handler = signal(SIGPIPE, SIG_DFL);
	duckdb_httplib::Client client("127.0.0.1", ntohs(address.sin_port));
	std::string body(64 * 1024 * 1024, 'x');
	auto result = client.Post("/", body, "application/octet-stream");
	REQUIRE(!result);
}
#endif
