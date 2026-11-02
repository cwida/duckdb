//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/client_config.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/enums/output_type.hpp"
#include "duckdb/common/progress_bar/progress_bar.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/enums/profiling_coverage.hpp"
#include "duckdb/main/user_settings.hpp"

namespace duckdb {

class ClientContext;
class PhysicalResultCollector;
class PreparedStatementData;
struct CompiledGrammar;
struct ReplacementScan;

typedef std::function<unique_ptr<PhysicalOperator>(ClientContext &context, PreparedStatementData &data)>
    get_result_collector_t;

struct ClientConfig {
	//! If the query profiler is enabled or not.
	bool enable_profiler = false;
	//! The format to print query profiling information in (default: query_tree), if enabled.
	//! This is the profiler format name passed to QueryProfiler::CreateProfiler.
	string profiler_print_format = "query_tree";
	//! The file to save query profiling information to, instead of printing it to the console
	//! (empty = print to console)
	string profiler_save_location;
	//! Glob patterns for tracked_metrics (controls which metrics are gathered and displayed).
	//! Default "*" means all metrics are tracked.
	vector<string> tracked_metrics = {"*"};
	//! Settings that are passed to the renderer of the profiler output (e.g. 'maximum_render_width' for the text
	//! renderer). Settings that are not recognized by the active renderer are ignored.
	unordered_map<string, Value> profiling_renderer_settings;

	//! system-wide progress bar disable.
	const char *system_progress_bar_disable_reason = nullptr;
	//! If the progress bar is enabled or not.
	bool enable_progress_bar = false;
	//! If the print of the progress bar is enabled
	bool print_progress_bar = true;
	//! The wait time before showing the progress bar
	int wait_time = 2000;

	//! Force parallelism of small tables, used for testing
	bool verify_parallelism = false;
	//! If this context should also try to use the available replacement scans
	//! True by default
	bool use_replacement_scans = true;
	//! Replacement scans visible only to this connection, consulted before the database-wide ones in DBConfig.
	//! Held by pointer so that copying a ClientConfig stays possible; a ReplacementScan owns its data uniquely.
	vector<shared_ptr<ReplacementScan>> replacement_scans;

	//! The maximum amount of memory to keep buffered in a streaming query result. Default: 10mb.
	idx_t max_streaming_buffer_size = idx_t(10) * 1024 * 1024;

	//! The maximum memory for query intermediates (sorts, hash tables) per connection (in bytes). Default: Global
	//! memory limit.
	optional_idx operator_memory_limit;

	//! Callback to create a progress bar display
	progress_bar_display_create_func_t display_create_func = nullptr;

	//! The profiling coverage. SELECT is the default behavior, and ALL emits profiling information for all operator
	//! types.
	ProfilingCoverage profiling_coverage = ProfilingCoverage::SELECT;

	//! Generic options
	LocalUserSettings user_settings;

	//! Variables set by the user
	identifier_map_t<Value> user_variables;

	//! Function that is used to create the result collector for a materialized result.
	get_result_collector_t get_result_collector = nullptr;

	optional<string> current_dialect;
	//! The (ordered) list of grammar extensions currently used by the parser
	vector<string> active_grammar_extensions;
	//! The compiled grammar active for the connection
	shared_ptr<CompiledGrammar> cached_grammar;
	//! The grammar of the database this client is CONNECT-ed to; unset when not connected
	shared_ptr<CompiledGrammar> connected_grammar;

public:
	static ClientConfig &GetConfig(ClientContext &context);
	static const ClientConfig &GetConfig(const ClientContext &context);

	void SetUserVariable(const String &name, Value value);
	bool GetUserVariable(const Identifier &name, Value &result);
	bool GetUserVariable(const string &name, Value &result);
	void ResetUserVariable(const String &name);

public:
	void SetDefaultStreamingBufferSize();
};

struct ScopedConfigSetting {
public:
	using config_modify_func_t = std::function<void(ClientConfig &config)>;

public:
	explicit ScopedConfigSetting(ClientConfig &config, config_modify_func_t set_f = nullptr,
	                             config_modify_func_t unset_f = nullptr)
	    : config(config), set(std::move(set_f)), unset(std::move(unset_f)) {
		if (set) {
			set(config);
		}
	}
	~ScopedConfigSetting() {
		if (unset) {
			unset(config);
		}
	}

public:
	ClientConfig &config;
	config_modify_func_t set;
	config_modify_func_t unset;
};

} // namespace duckdb
