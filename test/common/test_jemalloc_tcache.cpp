#include "catch.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception.hpp"

#ifdef DUCKDB_ENABLE_JEMALLOC
#include "jemalloc/jemalloc.h"

#include <thread>

using namespace duckdb;

TEST_CASE("Flushing a thread without a jemalloc tcache", "[api]") {
	// jemalloc runs a thread without a tcache when it has no memory for one
	string error;
	std::thread worker([&]() {
		try {
			bool enabled = false;
			if (duckdb_je_mallctl("thread.tcache.enabled", nullptr, nullptr, &enabled, sizeof(enabled)) != 0) {
				error = "failed to disable the tcache";
				return;
			}
			auto &allocator = Allocator::DefaultAllocator();
			auto data = allocator.AllocateData(1 << 16);
			allocator.FreeData(data, 1 << 16);
			Allocator::ThreadFlush(false, 0, 1);
			Allocator::FlushAll();
			Allocator::ThreadIdle();
		} catch (std::exception &ex) {
			error = ex.what();
		}
	});
	worker.join();
	REQUIRE(error.empty());
}
#endif
