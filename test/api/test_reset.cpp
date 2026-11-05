#include "catch.hpp"
#include "duckdb/common/enums/lambda_syntax.hpp"
#include "duckdb/common/enums/allow_parser_override.hpp"
#include "duckdb/common/enums/dialect_compatibility_mode.hpp"
#include "duckdb/common/enums/table_function_identifier_conversion.hpp"
#include "duckdb/common/enums/show_behavior.hpp"
#include "duckdb/parser/peg/dialect_extension.hpp"
#include "test_helpers.hpp"

#include <iostream>
#include <map>
#include <set>
#include <cstring>

using namespace duckdb;

struct OptionValuePair {
	OptionValuePair() {
	}
	OptionValuePair(Value val) : input(val), output(val) {
	}
	OptionValuePair(Value input, Value output) : input(std::move(input)), output(std::move(output)) {
	}

	Value input;
	Value output;
};

struct OptionValueSet {
	OptionValueSet() {
	}
	OptionValueSet(Value val) {
		pairs.emplace_back(std::move(val));
	}
	OptionValueSet(Value input, Value output) {
		pairs.emplace_back(std::move(input), std::move(output));
	}
	OptionValueSet(duckdb::vector<std::string> pairs_p) {
		for (auto &pair : pairs_p) {
			pairs.emplace_back(pair);
		}
	}
	OptionValueSet(duckdb::vector<OptionValuePair> pairs_p) : pairs(std::move(pairs_p)) {
	}

	duckdb::vector<OptionValuePair> pairs;
};

void RequireValueEqual(const string &option, const Value &left, const Value &right, int line);
#define REQUIRE_VALUE_EQUAL(option, lhs, rhs) RequireValueEqual(option, lhs, rhs, __LINE__)

OptionValueSet GetValueForOption(const string &name, const LogicalType &type) {
	static unordered_map<string, OptionValueSet> value_map = {
	    {"threads", {Value::BIGINT(42), Value::BIGINT(42)}},
	    {"checkpoint_threshold", {"4.0 GiB"}},
	    {"debug_checkpoint_abort", {{"none", "before_truncate", "before_header", "after_free_list_write"}}},
	    {"default_collation", {"nocase"}},
	    {"default_order", {"DESC"}},
	    {"default_null_order", {"NULLS_FIRST"}},
	    {"disabled_compression_methods", {"RLE"}},
	    {"disabled_optimizers", {"extension"}},
	    {"debug_force_external", {Value(true)}},
	    {"debug_order_verification", {"create_sort_key"}},
	    {"old_implicit_casting", {Value(true)}},
	    {"prefer_range_joins", {Value(true)}},
	    {"variant_minimum_shredding_size", {Value::INTEGER(-1)}},
	    {"allow_persistent_secrets", {Value(false)}},
	    {"secret_directory", {"/tmp/some/path"}},
	    {"default_secret_storage", {"custom_storage"}},
	    {"custom_extension_repository", {"duckdb.org/no-extensions-here", "duckdb.org/no-extensions-here"}},
	    {"autoinstall_extension_repository", {"duckdb.org/no-extensions-here", "duckdb.org/no-extensions-here"}},
	    {"lambda_syntax", {EnumUtil::ToString(LambdaSyntax::DISABLE_SINGLE_ARROW)}},
	    {"table_function_identifier_conversion",
	     {EnumUtil::ToString(TableFunctionIdentifierConversion::DISABLE_IMPLICIT_STRING)}},
	    {"dialect_compatibility_mode", {EnumUtil::ToString(DialectCompatibilityMode::SPARK)}},
	    {"allow_parser_override_extension", {EnumUtil::ToString(AllowParserOverride::FALLBACK_OVERRIDE)}},
	    {"profiling_coverage", {EnumUtil::ToString(ProfilingCoverage::ALL)}},
	    {"show_behavior", {EnumUtil::ToString(ShowBehaviorType::TABLE)}},
	    {"autoload_known_extensions", {false}},
	    {"autoinstall_known_extensions", {false}},
	    {"enable_block_allocator", {!DBConfigOptions().enable_block_allocator}},
	    {"enable_profiling", {"json"}},
	    {"explain_output", {{"all", "optimized_only", "physical_only"}}},
	    {"file_search_path", {"test"}},
	    {"force_compression", {"uncompressed", "uncompressed"}},
	    {"fsync_mode", {"NONE"}},
	    {"home_directory", {"test"}},
	    {"allow_extensions_metadata_mismatch", {"true"}},
	    {"extension_directory", {"test"}},
	    {"extension_repository_directory", {"test"}},
	    {"extension_directories", {"[test]"}},
	    {"max_expression_depth", {50}},
	    {"write_buffer_row_group_memory_limit", {"4.0 GiB"}},
	    {"max_memory", {"4.0 GiB"}},
	    {"max_temp_directory_size", {"10.0 GiB"}},
	    {"merge_join_threshold", {73}},
	    {"nested_loop_join_threshold", {73}},
	    {"memory_limit", {"4.0 GiB"}},
	    {"storage_compatibility_version", {"v0.10.0"}},
	    {"ordered_aggregate_threshold", {Value::UBIGINT(idx_t(1) << 12)}},
	    {"null_order", {"NULLS_FIRST"}},
	    {"debug_verify_vector", {"dictionary_expression"}},
	    {"debug_physical_table_scan_execution_strategy", {"default"}},
	    {"perfect_ht_threshold", {0}},
	    {"pivot_filter_threshold", {999}},
	    {"pivot_limit", {999}},
	    {"partitioned_write_flush_threshold", {123}},
	    {"preserve_identifier_case", {"lowercase"}},
	    {"preserve_insertion_order", {false}},
	    {"profile_output", {"output.txt"}},
	    {"profiling_mode", {"standard"}},
	    {"disabled_log_types", {"blabla"}},
	    {"enabled_log_types", {"blabla"}},
	    {"enabled_log_types", {"blabla"}},
	    {"enable_logging", {true}},
	    {"logging_mode", {"ENABLE_SELECTED"}},
	    {"logging_level", {"FATAL"}},
	    {"logging_storage", {"stdout"}},
	    {"redact_http_logs", {false}},
	    {"enable_progress_bar_print", {false}},
	    {"scalar_subquery_error_on_multiple_rows", {false}},
	    {"ieee_floating_point_ops", {false}},
	    {"error_on_division_by_zero", {false}},
	    {"progress_bar_time", {0}},
	    {"regex_match_operator_semantics", {"full"}},
	    {"temp_directory", {"tmp"}},
	    {"wal_autocheckpoint", {"4.0 GiB"}},
	    {"force_bitpacking_mode", {"constant"}},
	    {"http_proxy", {"localhost:80"}},
	    {"http_proxy_username", {"john"}},
	    {"http_proxy_password", {"doe"}},
	    {"allocator_flush_threshold", {"4.0 GiB"}},
	    {"allocator_bulk_deallocation_flush_threshold", {"4.0 GiB"}},
	    {"arrow_output_version", {"1.5"}},
	    {"enable_external_file_cache", {false}},
	    {"external_file_cache_local_max_block_size", {Value::UBIGINT(4096)}},
	    {"external_file_cache_remote_max_block_size", {Value::UBIGINT(4096)}},
	    {"external_file_cache_remote_min_block_size", {Value::UBIGINT(8192)}},
	    {"external_file_cache_request_sizing", {"GRID"}},
	    {"validate_external_file_cache", {"NO_VALIDATION"}},
	    {"experimental_metadata_reuse", {false}},
	    {"storage_block_prefetch", {"always_prefetch"}},
	    {"operator_memory_limit", {"4.0 GiB"}},
	    {"pin_threads", {"off"}},
	    {"current_transaction_invalidation_policy", {"SYNTACTIC_ERRORS_DO_NOT_INVALIDATE"}},
	    {"default_transaction_invalidation_policy", {"SYNTACTIC_ERRORS_DO_NOT_INVALIDATE"}},
	    {"checkpoint_on_detach", {"ENABLED"}},
	    {"debug_verify_statement", {"copy_statement"}},
	    {"enable_caching_operators", {false}},
	    {"enable_optimistic_write", {false}},
	    {"enable_optimizer", {false}},
	    {"initial_column_segment_size", {4096}},
	    {"delim_join_as_cte", {false}}};
	// Every option that's not excluded has to be part of this map
	if (!value_map.count(name)) {
		switch (type.id()) {
		case LogicalTypeId::BOOLEAN:
			return OptionValueSet(Value::BOOLEAN(true));
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
		case LogicalTypeId::UTINYINT:
		case LogicalTypeId::USMALLINT:
		case LogicalTypeId::UINTEGER:
		case LogicalTypeId::UBIGINT:
			return OptionValueSet(Value::Numeric(type, 42));
		default:
			break;
		}
		REQUIRE(name == "MISSING_FROM_MAP");
	}
	return value_map[name];
}

bool OptionIsExcludedFromTest(const string &name) {
	static unordered_set<string> excluded_options = {
	    "access_mode",
	    "active_grammar_extensions",
	    "allow_community_extensions",   // cant change this while db is running
	    "allow_extension_repositories", // can only be tightened at runtime, cannot be freely reset
	    "allow_unredacted_secrets",     // cant change this while db is running
	    "allow_unsigned_extensions",    // cant change this while db is running
	    "allowed_configs",
	    "allowed_directories",
	    "allowed_paths",
	    "current_dialect",
	    "custom_user_agent",
	    "debug_delta_only_variant_encoding_enabled",
	    "debug_verification_mode",
	    "debug_window_mode",
	    "default_block_size",
	    "disable_database_invalidation", // cant change this while db is running
	    "disabled_filesystems",          // cant change this while db is running
	    "duckdb_api",
	    "enable_external_access", // cant change this while db is running
	    "enable_object_cache",
	    "enable_profiling",
	    "enable_progress_bar",
	    "enable_progress_bar_print",
	    "experimental_parallel_csv",
	    "extension_directories",
	    "extension_repository_directory", // trust anchor, cant change while db is running (unless unsigned allowed)
	    "external_threads",               // tested in test_threads.cpp
	    "force_variant_shredding",
	    "index_scan_max_count",
	    "index_scan_percentage",
	    "lock_configuration", // cant change this while db is running
	    "log_query_path",
	    "max_execution_time",
	    "max_streaming_buffer_size",
	    "password",
	    "profiling_mode",
	    "profiling_output", // just an alias
	    "profiling_renderer_settings",
	    "progress_bar_time",
	    "scheduler_process_partial",
	    "schema",
	    "search_path",
	    "standard_vector_size",
	    "streaming_buffer_size", // alias of max_streaming_buffer_size
	    "temp_file_encryption",
	    "tracked_metrics",
	    "user",
	    "username",
	    "vacuum_rebuild_indexes", // cant change this while db is running
	    "warnings_as_errors",     // requires logging to be enabled
	    "worker_threads",
	};
	return excluded_options.count(name) == 1;
}

bool ValueEqual(const Value &left, const Value &right) {
	return Value::NotDistinctFrom(left, right);
}

void RequireValueEqual(const string &option_name, const Value &left, const Value &right, int line) {
	if (ValueEqual(left, right)) {
		return;
	}
	auto error = StringUtil::Format("\nLINE[%d] (Option:%s) | Expected left:'%s' and right:'%s' to be equal", line,
	                                option_name, left.ToString(), right.ToString());
	std::cerr << error << std::endl;
	REQUIRE(false);
}

Value GetValueForSetting(Connection &con, const string &name, const LogicalType &type) {
	string new_value;
	auto result = con.Query(StringUtil::Format("SELECT value FROM duckdb_settings() WHERE name = %s", SQLString(name)));
	for (auto &row : *result) {
		new_value = row.GetValue<string>(0);
	}
	return Value(new_value).CastAs(*con.context, type);
}

//! New options should be added to the value_map in GetValueForOption
//! Or added to the 'excluded_options' in OptionIsExcludedFromTest
TEST_CASE("Test RESET statement for ClientConfig options", "[api]") {
	// Create a connection
	DBConfig config;
	config.options.load_extensions = false;
	DuckDB db(nullptr, &config);
	Connection con(db);
	con.Query("BEGIN TRANSACTION");
	con.Query("PRAGMA disable_profiling");

	struct ResetSettingOption {
		string name;
		Value value;
		LogicalType type;
	};
	duckdb::vector<ResetSettingOption> options;

	auto result = con.Query("SELECT name, value, input_type FROM duckdb_settings()");
	for (auto row : *result) {
		ResetSettingOption option;
		option.name = row.GetValue<string>(0);
		option.type = DBConfig::ParseLogicalType(row.GetValue<string>(2));
		if (row.IsNull(1)) {
			option.value = Value(option.type);
		} else {
			Value str_val = Value(row.GetValue<string>(1));
			option.value = str_val.CastAs(*con.context, option.type);
		}

		if (OptionIsExcludedFromTest(option.name)) {
			continue;
		}
		options.push_back(std::move(option));
	}
	for (auto &option : options) {
		auto value_set = GetValueForOption(option.name, option.type);
		// verify that at least one value is different
		bool any_different = false;
		string options;
		for (auto &value_pair : value_set.pairs) {
			if (!ValueEqual(option.value, value_pair.output)) {
				any_different = true;
			} else {
				if (!options.empty()) {
					options += ", ";
				}
				options += value_pair.output.ToString();
			}
		}
		if (!any_different) {
			auto error = StringUtil::Format(
			    "\n(Option:%s) | Expected original value '%s' and provided option '%s' to be different", option.name,
			    option.value.ToString(), options);
			std::cerr << error << std::endl;
			REQUIRE(false);
		}
		auto original_value = GetValueForSetting(con, option.name, option.type);
		for (auto &value_pair : value_set.pairs) {
			// Get the new value for the option
			auto input = value_pair.input.CastAs(*con.context, option.type);
			// Set the new option
			REQUIRE_NO_FAIL(con.Query(StringUtil::Format("SET %s = %s", option.name, input.ToSQLString())));

			auto changed_value = GetValueForSetting(con, option.name, option.type);

			// Get the value of the option again
			REQUIRE_VALUE_EQUAL(option.name, changed_value, value_pair.output);

			// reset the option again
			REQUIRE_NO_FAIL(con.Query(StringUtil::Format("RESET %s", option.name)));

			auto reset_value = GetValueForSetting(con, option.name, option.type);

			// Get the reset value of the option
			REQUIRE_VALUE_EQUAL(option.name, reset_value, original_value);
		}
	}
}
