#include "duckdb_cpp.hpp"

#include <cstdio>

// Minimal consumer of the stable C++ API: open an in-memory database, run a
// query, print the result.
int main() {
	try {
		duckdb::cxx::Instance db(":memory:");
		auto conn = db.Connect();
		auto result = conn.Execute("SELECT 21 * 2 AS answer");
		std::fputs(result.RenderBox().c_str(), stdout);
		return 0;
	} catch (const duckdb::cxx::Exception &ex) {
		std::fprintf(stderr, "error: %s\n", ex.what());
		return 1;
	}
}
