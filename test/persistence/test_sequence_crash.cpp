#include "catch.hpp"
#include "duckdb.hpp"
#include "test_helpers.hpp"

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>

using namespace duckdb;

TEST_CASE("Test that sequence never returns the same value twice even with aborts", "[persistence][.]") {
	// disabled test for now
	return;

	string dbdir = TestCreatePath("defaultseq");
	DeleteDatabase(dbdir);
	// create a database
	{
		DuckDB db(dbdir);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE SEQUENCE seq"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE a (i INTEGER DEFAULT nextval('seq'), j INTEGER)"));
	}

	// now fork the process a bunch of times
	for (int i = 0; i < 100; i++) {
		// fork the process
		pid_t pid = fork();
		if (pid == 0) {
			// child process, connect to the database and start inserting values
			DuckDB db(dbdir);
			Connection con(db);
			while (true) {
				con.Query("INSERT INTO a (j) VALUES(1)");
			}
		} else if (pid > 0) {
			// parent process, sleep a bit
			usleep(100000);
			// send SIGKILL to the child
			if (kill(pid, SIGKILL) != 0) {
				FAIL();
			}
			// sleep a bit before continuing the loop, to make sure the lock on the database was released
			usleep(100000);
		} else {
			FAIL();
		}
	}
	// now connect to the database
	{
		DuckDB db(dbdir);
		Connection con(db);
		// verify that "i" only has unique values from the sequence
		// i.e. COUNT = COUNT(DISTINCT)
		auto result = con.Query("SELECT COUNT(i), COUNT(DISTINCT i) FROM a");
		REQUIRE(CHECK_COLUMN(result, 1, {result->Collection().GetValue(0, 0)}));
	}
	DeleteDatabase(dbdir);
}

static void write_entries_to_table(DuckDB *db, int i) {
	Connection con(*db);
	if (i % 2 == 0) {
		// i % 2 = 0, insert values
		while (true) {
			con.Query("INSERT INTO a (j) VALUES(1)");
		}
	} else {
		// use nextval in select clause
		while (true) {
			con.Query("SELECT nextval('seq')");
		}
	}
}

TEST_CASE("Test that sequence never returns the same value twice even with aborts and concurrent usage",
          "[persistence][.]") {
	// disabled test for now
	return;

	string dbdir = TestCreatePath("defaultseqconcurrent");
	DeleteDatabase(dbdir);
	// create a database
	{
		DuckDB db(dbdir);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE SEQUENCE seq"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE a (i INTEGER DEFAULT nextval('seq'), j INTEGER)"));
	}
	// now fork the process a bunch of times
	for (int i = 0; i < 100; i++) {
		// fork the process
		pid_t pid = fork();
		if (pid == 0) {
			// child process, connect to the database and start inserting values in separate threads
			DuckDB db(dbdir);
			std::thread write_threads[8];
			for (size_t i = 0; i < 8; i++) {
				write_threads[i] = std::thread(write_entries_to_table, &db, i);
			}
			while (true)
				;
		} else if (pid > 0) {
			// parent process, sleep a bit
			usleep(100000);
			// send SIGKILL to the child
			if (kill(pid, SIGKILL) != 0) {
				FAIL();
			}
			// sleep a bit before continuing the loop, to make sure the lock on the database was released
			usleep(100000);
		} else {
			FAIL();
		}
	}
	// now connect to the database
	{
		DuckDB db(dbdir);
		Connection con(db);
		// verify that "i" only has unique values from the sequence
		// i.e. COUNT = COUNT(DISTINCT)
		auto result = con.Query("SELECT COUNT(i), COUNT(DISTINCT i) FROM a");
		REQUIRE(CHECK_COLUMN(result, 1, {result->Collection().GetValue(0, 0)}));
	}
	DeleteDatabase(dbdir);
}
