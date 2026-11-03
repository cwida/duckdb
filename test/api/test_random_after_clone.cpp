#include "catch.hpp"
#include "test_helpers.hpp"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace duckdb;

// a child forked from a process with threads hangs under the thread sanitizer
#if !defined(_WIN32) && !defined(DUCKDB_THREAD_SANITIZER)
static const vector<string> RANDOM_QUERIES = {
    "SELECT uuid()::VARCHAR", "SELECT gen_random_uuid()::VARCHAR", "SELECT uuidv7()::VARCHAR",
    "SELECT random()::VARCHAR",
    "SELECT list(i ORDER BY i)::VARCHAR FROM (SELECT * FROM range(100000) t(i) USING SAMPLE 50 ROWS)"};

static vector<string> QueryValues(Connection &con, const string &setup) {
	vector<string> values;
	if (!setup.empty()) {
		auto setup_result = con.Query(setup);
		if (setup_result->HasError()) {
			values.push_back("error: " + setup_result->GetError());
			return values;
		}
	}
	for (auto &query : RANDOM_QUERIES) {
		auto result = con.Query(query);
		if (result->HasError()) {
			values.push_back("error: " + result->GetError());
		} else if (result->Collection().Count() != 1) {
			values.push_back("error: no row");
		} else {
			values.push_back(result->Collection().GetValue(0, 0).ToString());
		}
	}
	return values;
}

//! Runs the queries in a process forked from this one, which shares the connection's state as it is now
static vector<string> QueryValuesInForkedProcess(Connection &con, const string &setup) {
	int fds[2];
	REQUIRE(pipe(fds) == 0);
	auto pid = fork();
	REQUIRE(pid >= 0);
	if (pid == 0) {
		// the child must never return into the test framework
		alarm(30);
		close(fds[0]);
		try {
			string output;
			for (auto &value : QueryValues(con, setup)) {
				output += value + "\n";
			}
			auto written = write(fds[1], output.c_str(), output.size());
			_exit(written == static_cast<ssize_t>(output.size()) ? 0 : 1);
		} catch (...) {
			_exit(2);
		}
	}
	close(fds[1]);
	string output;
	char buffer[256];
	ssize_t read_bytes;
	while ((read_bytes = read(fds[0], buffer, sizeof(buffer))) > 0) {
		output.append(buffer, static_cast<size_t>(read_bytes));
	}
	close(fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	REQUIRE(WIFEXITED(status));
	REQUIRE(WEXITSTATUS(status) == 0);
	auto values = StringUtil::Split(output, '\n');
	REQUIRE(values.size() == RANDOM_QUERIES.size());
	return values;
}

TEST_CASE("Random values differ between processes cloned from one connection", "[api]") {
	DBConfig config;
	config.SetOptionByName("threads", Value::BIGINT(1));
	DuckDB db(nullptr, &config);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));

	// a clone of the process (fork, a VM snapshot) must not repeat the sequence of its source
	auto in_child = QueryValuesInForkedProcess(con, string());
	auto in_parent = QueryValues(con, string());
	for (idx_t i = 0; i < RANDOM_QUERIES.size(); i++) {
		INFO(RANDOM_QUERIES[i]);
		REQUIRE(in_child[i].rfind("error", 0) != 0);
		REQUIRE(in_parent[i].rfind("error", 0) != 0);
		REQUIRE(in_child[i] != in_parent[i]);
	}

	// once seeded explicitly, the sequence repeats, in a clone as well
	auto seeded_child = QueryValuesInForkedProcess(con, "SELECT setseed(0.42)");
	auto seeded_parent = QueryValues(con, "SELECT setseed(0.42)");
	for (idx_t i = 0; i < RANDOM_QUERIES.size(); i++) {
		INFO(RANDOM_QUERIES[i]);
		REQUIRE(seeded_child[i].rfind("error", 0) != 0);
		// uuidv7 embeds the current time
		if (RANDOM_QUERIES[i].find("uuidv7") == string::npos) {
			REQUIRE(seeded_child[i] == seeded_parent[i]);
		}
	}
}
#endif
