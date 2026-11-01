#include "catch.hpp"
#include "test_helpers.hpp"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace duckdb;

#ifndef _WIN32
static const char *RANDOM_VALUES =
    "SELECT uuid()::VARCHAR || ' ' || gen_random_uuid()::VARCHAR || ' ' || "
    "random()::VARCHAR || ' ' || (SELECT sum(i) FROM (SELECT * FROM range(1000) t(i) USING SAMPLE 5))";

static string QueryValue(Connection &con, const string &sql, const string &setup = string()) {
	if (!setup.empty()) {
		auto setup_result = con.Query(setup);
		if (setup_result->HasError()) {
			return "error: " + setup_result->GetError();
		}
	}
	auto result = con.Query(sql);
	if (result->HasError()) {
		return "error: " + result->GetError();
	}
	return result->Collection().GetValue(0, 0).ToString();
}

//! Runs 'sql' in a process forked from this one, which shares the connection's state as it is now
static string QueryInForkedProcess(Connection &con, const string &sql, const string &setup = string()) {
	int fds[2];
	REQUIRE(pipe(fds) == 0);
	auto pid = fork();
	REQUIRE(pid >= 0);
	if (pid == 0) {
		close(fds[0]);
		auto value = QueryValue(con, sql, setup);
		auto written = write(fds[1], value.c_str(), value.size());
		_exit(written == static_cast<ssize_t>(value.size()) ? 0 : 1);
	}
	close(fds[1]);
	string value;
	char buffer[256];
	ssize_t read_bytes;
	while ((read_bytes = read(fds[0], buffer, sizeof(buffer))) > 0) {
		value.append(buffer, static_cast<size_t>(read_bytes));
	}
	close(fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	REQUIRE(WIFEXITED(status));
	REQUIRE(WEXITSTATUS(status) == 0);
	return value;
}

TEST_CASE("Random values differ between processes cloned from one connection", "[api]") {
	DBConfig config;
	config.SetOptionByName("threads", Value::BIGINT(1));
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));

	// a clone of the process (fork, a VM snapshot) must not repeat the sequence of its source
	auto in_child = QueryInForkedProcess(con, RANDOM_VALUES);
	auto in_parent = QueryValue(con, RANDOM_VALUES);
	REQUIRE(in_child.rfind("error", 0) != 0);
	REQUIRE(in_parent.rfind("error", 0) != 0);
	REQUIRE(in_child != in_parent);

	// once seeded explicitly, the sequence repeats, in a clone as well
	auto seeded_child = QueryInForkedProcess(con, RANDOM_VALUES, "SELECT setseed(0.42)");
	auto seeded_parent = QueryValue(con, RANDOM_VALUES, "SELECT setseed(0.42)");
	REQUIRE(seeded_child.rfind("error", 0) != 0);
	REQUIRE(seeded_child == seeded_parent);
}
#endif
