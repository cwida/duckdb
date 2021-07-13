#include "catch.hpp"
#include "test_helpers.hpp"

#include <set>
#include <map>

using namespace duckdb;
using namespace std;

TEST_CASE("Test DB config configuration", "[api]") {
	DBConfig config;

	auto options = config.GetOptions();

	map<ConfigurationOptionType, vector<string>> test_options;
	test_options[ConfigurationOptionType::ACCESS_MODE] = {"automatic", "read_only", "read_write"};
	test_options[ConfigurationOptionType::DEFAULT_ORDER_TYPE] = {"asc", "desc"};
	test_options[ConfigurationOptionType::DEFAULT_NULL_ORDER] = {"nulls_first", "nulls_last"};
	test_options[ConfigurationOptionType::ENABLE_EXTERNAL_ACCESS] = {"true", "false"};
	test_options[ConfigurationOptionType::ENABLE_OBJECT_CACHE] = {"true", "false"};
	test_options[ConfigurationOptionType::MAXIMUM_MEMORY] = {"-1", "16GB"};
	test_options[ConfigurationOptionType::THREADS] = {"-1", "4"};

	set<ConfigurationOptionType> skip_invalid;

	REQUIRE(config.GetOptionByName("unknownoption") == nullptr);

	for (auto &option : options) {
		auto op = config.GetOptionByName(option.name);
		REQUIRE(op);

		auto entry = test_options.find(option.type);
		if (entry != test_options.end()) {
			for (auto &str_val : entry->second) {
				Value val(str_val);
				REQUIRE_NOTHROW(config.SetOption(option, val));
			}
		}
		if (skip_invalid.find(option.type) != skip_invalid.end()) {
			continue;
		}
		Value invalid_val("___this_is_probably_invalid");
		REQUIRE_THROWS(config.SetOption(option, invalid_val));
	}
}
