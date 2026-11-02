#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"

// whatever
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>

#ifdef __MVS__
#define _XOPEN_SOURCE_EXTENDED 1
#include <strings.h>
#endif

using namespace duckdb;

TEST_CASE("Test using a remote optimizer pass in case thats important to someone", "[extension]") {
	pid_t pid = fork();

	int port = 4242;

	if (pid == 0) { // child process
		// sockets, man, how do they work?!
		struct sockaddr_in servaddr, cli;

		auto sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sockfd == -1) {
			printf("Failed to set up socket in child process: %s", strerror(errno));
			exit(1);
		}
		bzero(&servaddr, sizeof(servaddr));

		servaddr.sin_family = AF_INET;
		servaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
		servaddr.sin_port = htons(port);
		auto res = ::bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
		if (res != 0) {
			printf("Failed to bind socket in child process: %s", strerror(errno));
			exit(1);
		}
		res = listen(sockfd, 5);
		if (res != 0) {
			printf("Failed to listen to socked in child process: %s", strerror(errno));
			exit(1);
		}

		socklen_t len = sizeof(cli);
		auto connfd = accept(sockfd, (struct sockaddr *)&cli, &len);
		if (connfd < 0) {
			printf("Failed to set up socket in child process: %s", strerror(errno));
			exit(1);
		}

		DBConfig config;
		config.SetOptionByName("allow_unsigned_extensions", true);
		DuckDB db2(nullptr, &config);
		Connection con2(db2);
		auto load_parquet = con2.Query("LOAD parquet");
		if (load_parquet->HasError()) {
			printf("Failed to load Parquet in child process: %s", load_parquet->GetError().c_str());
			exit(1);
		}

		while (true) {
			idx_t bytes;
			REQUIRE(read(connfd, &bytes, sizeof(idx_t)) == sizeof(idx_t));

			if (bytes == 0) {
				break;
			}

			auto buffer = malloc(bytes);
			REQUIRE(buffer);
			REQUIRE(read(connfd, buffer, bytes) == ssize_t(bytes));

			// Non-owning stream
			MemoryStream stream(data_ptr_cast(buffer), bytes);
			con2.BeginTransaction();

			BinaryDeserializer deserializer(stream);
			deserializer.Set<ClientContext &>(*con2.context);
			deserializer.Begin();
			auto plan = LogicalOperator::Deserialize(deserializer);
			deserializer.End();

			plan->ResolveOperatorTypes();
			con2.Commit();

			auto statement = make_uniq<LogicalPlanStatement>(std::move(plan));
			auto result = con2.Query(std::move(statement));
			auto &collection = result->Collection();
			idx_t num_chunks = collection.ChunkCount();
			REQUIRE(write(connfd, &num_chunks, sizeof(idx_t)) == sizeof(idx_t));
			for (auto &chunk : collection.Chunks()) {
				Allocator allocator;
				MemoryStream target(allocator);
				BinarySerializer serializer(target);
				serializer.Begin();
				chunk.Serialize(serializer);
				serializer.End();
				auto data = target.GetData();
				idx_t len = target.GetPosition();
				REQUIRE(write(connfd, &len, sizeof(idx_t)) == sizeof(idx_t));
				REQUIRE(write(connfd, data, len) == ssize_t(len));
			}
		}
		exit(0);
	} else if (pid > 0) { // parent process
		DBConfig config;
		config.SetOptionByName("allow_unsigned_extensions", true);
		DuckDB db1(nullptr, &config);
		Connection con1(db1);
		auto load_parquet = con1.Query("LOAD 'parquet'");
		if (load_parquet->HasError()) {
			// Do not execute the test.
			if (kill(pid, SIGKILL) != 0) {
				FAIL();
			}
			return;
		}

		REQUIRE_NO_FAIL(con1.Query("LOAD '" + TestBuildDirectory() +
		                           "/test/extension/loadable_extension_optimizer_demo.duckdb_extension'"));
		REQUIRE_NO_FAIL(con1.Query("SET waggle_location_host='127.0.0.1'"));
		REQUIRE_NO_FAIL(con1.Query("SET waggle_location_port=4242"));
		usleep(10000); // need to wait a bit till socket is up

		// check if the child PID is still there
		if (kill(pid, 0) != 0) {
			// child is gone!
			printf("Failed to execute remote optimizer test - child exited unexpectedly");
			FAIL();
		}

		REQUIRE_NO_FAIL(con1.Query(
		    "SELECT first_name FROM PARQUET_SCAN('data/parquet-testing/userdata1.parquet') GROUP BY first_name"));

		if (kill(pid, SIGKILL) != 0) {
			FAIL();
		}

	} else {
		FAIL();
	}
}
