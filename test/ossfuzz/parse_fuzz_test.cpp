#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include <string>
#include <unordered_set>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	std::string input(reinterpret_cast<const char *>(data), size);
	duckdb::DBConfig config;
	config.SetOptionByName("threads", duckdb::Value::BIGINT(2));
	// Fuzzed SQL must not override the harness's resource limits.
	config.SetOptionByName("lock_configuration", duckdb::Value::BOOLEAN(true));
	duckdb::DuckDB db(nullptr, &config);
	duckdb::Connection con(db);

	std::unordered_set<std::string> internal_error_messages = {"Unoptimized Result differs from original result!",
	                                                           "INTERNAL"};
	try {
		auto result = con.Query(input);
		if (result->HasError()) {
			for (auto &internal_error : internal_error_messages) {
				if (duckdb::StringUtil::Contains(result->GetError(), internal_error)) {
					return 1;
				}
			}
		}
	} catch (std::exception &e) {
	}
	return 0;
}
