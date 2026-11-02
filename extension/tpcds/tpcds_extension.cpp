#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "dsdgen.hpp"
#include "tpcds_extension.hpp"

#include <atomic>
#include <mutex>

namespace duckdb {

struct DSDGenFunctionData : public TableFunctionData {
	DSDGenFunctionData() {
	}

	double sf = 0;
	Identifier catalog = INVALID_CATALOG;
	Identifier schema = DEFAULT_SCHEMA;
	string suffix;
	bool overwrite = false;
	bool keys = false;
};

struct DSDGenGlobalState : public GlobalTableFunctionState {
	bool schema_created = false;
	atomic<bool> finished {false};
	// Progress polling can run while dsdgen is being resumed by the table function.
	mutable mutex generator_lock;
	unique_ptr<tpcds::DSDGenGenerator> generator;
};

class DSDGenYieldTask : public AsyncTask {
public:
	void Execute() override {
	}
};

static AsyncResult DSDGenYield() {
	vector<unique_ptr<AsyncTask>> tasks;
	tasks.push_back(make_uniq<DSDGenYieldTask>());
	return AsyncResult(std::move(tasks));
}

static unique_ptr<FunctionData> DsdgenBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<DSDGenFunctionData>();

	const auto current_catalog = DatabaseManager::GetDefaultDatabase(context);
	const auto current_schema = ClientData::Get(context).catalog_search_path->GetDefault().GetSchema();
	result->catalog = current_catalog;
	result->schema = current_schema;

	for (auto &kv : input.named_parameters) {
		if (kv.second.IsNull()) {
			throw BinderException("Cannot use NULL as function argument");
		}
		if (kv.first == "sf") {
			result->sf = kv.second.GetValue<double>();
		} else if (kv.first == "catalog") {
			result->catalog = Identifier(StringValue::Get(kv.second));
		} else if (kv.first == "schema") {
			result->schema = Identifier(StringValue::Get(kv.second));
		} else if (kv.first == "suffix") {
			result->suffix = StringValue::Get(kv.second);
		} else if (kv.first == "overwrite") {
			result->overwrite = kv.second.GetValue<bool>();
		} else if (kv.first == "keys") {
			result->keys = kv.second.GetValue<bool>();
		}
	}
	if (input.binder) {
		auto &catalog = Catalog::GetCatalog(context, result->catalog);
		auto &properties = input.binder->GetStatementProperties();
		DatabaseModificationType modification;
		modification |= DatabaseModificationType::CREATE_CATALOG_ENTRY;
		modification |= DatabaseModificationType::INSERT_DATA;
		properties.RegisterDBModify(catalog, context, modification);
	}
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> DsdgenInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<DSDGenGlobalState>();
}

static void DsdgenFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<DSDGenFunctionData>();
	auto &state = data_p.global_state->Cast<DSDGenGlobalState>();
	if (state.finished.load()) {
		data_p.async_result = AsyncResultType::FINISHED;
		return;
	}
	if (!state.schema_created) {
		tpcds::DSDGenWrapper::CreateTPCDSSchema(context, data.catalog, data.schema, data.suffix, data.keys,
		                                        data.overwrite);
		auto generator = tpcds::CreateDSDGenGenerator(context, data.sf, data.catalog, data.schema, data.suffix);
		{
			lock_guard<mutex> guard(state.generator_lock);
			state.generator = std::move(generator);
		}
		state.schema_created = true;
	}

	while (true) {
		bool finished = false;
		bool can_yield = false;
		{
			lock_guard<mutex> guard(state.generator_lock);
			finished = !state.generator || state.generator->GenerateNext();
			can_yield = state.generator && state.generator->CanYield();
		}
		if (finished) {
			state.finished.store(true);
			data_p.async_result = AsyncResultType::FINISHED;
			return;
		}
		if (data_p.results_execution_mode == AsyncResultsExecutionMode::TASK_EXECUTOR && can_yield) {
			data_p.async_result = DSDGenYield();
			return;
		}
	}
}

static double DsdgenProgress(ClientContext &context, const FunctionData *bind_data,
                             const GlobalTableFunctionState *global_state) {
	if (!global_state) {
		return 0.0;
	}
	auto &state = global_state->Cast<DSDGenGlobalState>();
	{
		lock_guard<mutex> guard(state.generator_lock);
		if (state.generator) {
			return state.generator->Progress();
		}
	}
	return state.finished.load() ? 100.0 : 0.0;
}

static unique_ptr<NodeStatistics> DsdgenCardinality(ClientContext &, const FunctionData *) {
	// The result is a single status row, but the side-effecting scan does the real work.
	return make_uniq<NodeStatistics>(1000);
}

struct TPCDSData : public GlobalTableFunctionState {
	TPCDSData() : offset(0) {
	}
	idx_t offset;
};

unique_ptr<GlobalTableFunctionState> TPCDSInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<TPCDSData>();
	return std::move(result);
}

struct TPCDSQueriesBindData : public TableFunctionData {
	//! Without a scale factor the stored queries are returned unchanged
	bool has_sf = false;
	double sf = 1;

	string GetQuery(int query) const {
		return has_sf ? tpcds::DSDGenWrapper::GetQuery(query, sf) : tpcds::DSDGenWrapper::GetQuery(query);
	}
};

//! The scale factor is either the positional argument or the named "sf" parameter
static void BindQueryScaleFactor(TableFunctionBindInput &input, const char *function_name,
                                 TPCDSQueriesBindData &bind_data) {
	Value sf;
	if (!input.inputs.empty()) {
		sf = input.inputs[0];
		bind_data.has_sf = true;
	}
	auto entry = input.named_parameters.find("sf");
	if (entry != input.named_parameters.end()) {
		if (bind_data.has_sf) {
			throw BinderException("%s: the scale factor can be given as a positional argument or as \"sf\", not both",
			                      function_name);
		}
		sf = entry->second;
		bind_data.has_sf = true;
	}
	if (!bind_data.has_sf) {
		return;
	}
	if (sf.IsNull()) {
		throw BinderException("%s: cannot use NULL as scale factor", function_name);
	}
	bind_data.sf = sf.GetValue<double>();
}

static duckdb::unique_ptr<FunctionData> TPCDSQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<TPCDSQueriesBindData>();
	BindQueryScaleFactor(input, "tpcds_queries", *result);

	names.emplace_back("query_nr");
	return_types.emplace_back(LogicalType::INTEGER);

	names.emplace_back("query");
	return_types.emplace_back(LogicalType::VARCHAR);

	return std::move(result);
}

static void TPCDSQueryFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<TPCDSQueriesBindData>();
	auto &data = data_p.global_state->Cast<TPCDSData>();
	idx_t tpcds_queries = tpcds::DSDGenWrapper::QueriesCount();
	if (data.offset >= tpcds_queries) {
		// finished returning values
		return;
	}
	idx_t chunk_count = 0;

	// query_nr, INTEGER
	auto &query_nr = output.data[0];
	// query, VARCHAR
	auto &query_col = output.data[1];

	while (data.offset < tpcds_queries && chunk_count < STANDARD_VECTOR_SIZE) {
		auto query = bind_data.GetQuery(data.offset + 1);
		query_nr.Append(Value::INTEGER((int32_t)data.offset + 1));
		query_col.Append(Value(query));
		data.offset++;
		chunk_count++;
	}
}

static duckdb::unique_ptr<FunctionData> TPCDSQueryAnswerBind(ClientContext &context, TableFunctionBindInput &input,
                                                             vector<LogicalType> &return_types,
                                                             vector<Identifier> &names) {
	names.emplace_back("query_nr");
	return_types.emplace_back(LogicalType::INTEGER);

	names.emplace_back("scale_factor");
	return_types.emplace_back(LogicalType::DOUBLE);

	names.emplace_back("answer");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

static void TPCDSQueryAnswerFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<TPCDSData>();
	idx_t tpcds_queries = tpcds::DSDGenWrapper::QueriesCount();
	vector<double> scale_factors {1, 10};
	idx_t total_answers = tpcds_queries * scale_factors.size();
	if (data.offset >= total_answers) {
		// finished returning values
		return;
	}
	idx_t chunk_count = 0;

	// query_nr, INTEGER
	auto &query_nr = output.data[0];
	// scale_factor, DOUBLE
	auto &scale_factor = output.data[1];
	// answer, VARCHAR
	auto &answer_col = output.data[2];

	while (data.offset < total_answers && chunk_count < STANDARD_VECTOR_SIZE) {
		idx_t cur_query = data.offset % tpcds_queries;
		idx_t cur_sf = data.offset / tpcds_queries;
		auto answer = TpcdsExtension::GetAnswer(scale_factors[cur_sf], cur_query + 1);
		query_nr.Append(Value::INTEGER((int32_t)cur_query + 1));
		scale_factor.Append(Value::DOUBLE(scale_factors[cur_sf]));
		answer_col.Append(Value(answer));
		data.offset++;
		chunk_count++;
	}
}

static string PragmaTpcdsQuery(ClientContext &context, const FunctionParameters &parameters) {
	if (parameters.values[0].IsNull()) {
		throw InvalidInputException("Cannot use NULL as argument for the TPC-DS query number");
	}
	auto index = parameters.values[0].GetValue<int32_t>();
	auto sf_entry = parameters.named_parameters.find("sf");
	if (sf_entry == parameters.named_parameters.end()) {
		return tpcds::DSDGenWrapper::GetQuery(index);
	}
	if (sf_entry->second.IsNull()) {
		throw InvalidInputException("Cannot use NULL as scale factor for the TPC-DS query");
	}
	return tpcds::DSDGenWrapper::GetQuery(index, sf_entry->second.GetValue<double>());
}

static void LoadInternal(ExtensionLoader &loader) {
	TableFunction dsdgen_func("dsdgen", {}, DsdgenFunction, DsdgenBind, DsdgenInit);
	dsdgen_func.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("sf", LogicalType::DOUBLE)
		    .Add("overwrite", LogicalType::BOOLEAN)
		    .Add("keys", LogicalType::BOOLEAN)
		    .Add("catalog", LogicalType::VARCHAR)
		    .Add("schema", LogicalType::VARCHAR)
		    .Add("suffix", LogicalType::VARCHAR);
	});
	dsdgen_func.call_return_type = StatementReturnType::NOTHING;
	dsdgen_func.table_scan_progress = DsdgenProgress;
	dsdgen_func.cardinality = DsdgenCardinality;

	loader.RegisterFunction(dsdgen_func);

	// create the TPCDS pragma that allows us to run the query
	auto tpcds_func = PragmaFunction::PragmaCall(
	    "tpcds", PragmaTpcdsQuery, FunctionSignature().AddPositionalOnly("query_nr", LogicalType::BIGINT));
	tpcds_func.GetSignature().WithTypedKwargs("options",
	                                          [](TypedKwargs &options) { options.Add("sf", LogicalType::DOUBLE); });
	loader.RegisterFunction(tpcds_func);

	// create the TPCDS_QUERIES function that returns the queries, optionally parameterized for a scale factor
	TableFunctionSet tpcds_queries_set("tpcds_queries");
	for (auto &arguments : vector<vector<LogicalType>> {{}, {LogicalType::DOUBLE}}) {
		TableFunction tpcds_query_func(arguments, TPCDSQueryFunction, TPCDSQueryBind, TPCDSInit);
		tpcds_query_func.GetSignature().WithTypedKwargs(
		    "options", [](TypedKwargs &options) { options.Add("sf", LogicalType::DOUBLE); });
		tpcds_queries_set.AddFunction(tpcds_query_func);
	}
	loader.RegisterFunction(tpcds_queries_set);

	// create the TPCDS_ANSWERS that returns the query result
	TableFunction tpcds_query_answer_func("tpcds_answers", {}, TPCDSQueryAnswerFunction, TPCDSQueryAnswerBind,
	                                      TPCDSInit);

	loader.RegisterFunction(tpcds_query_answer_func);
}

// LCOV_EXCL_START
void TpcdsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
// LCOV_EXCL_STOP

std::string TpcdsExtension::GetQuery(int query) {
	return tpcds::DSDGenWrapper::GetQuery(query);
}

std::string TpcdsExtension::GetQuery(int query, double sf) {
	return tpcds::DSDGenWrapper::GetQuery(query, sf);
}

std::string TpcdsExtension::GetAnswer(double sf, int query) {
	return tpcds::DSDGenWrapper::GetAnswer(sf, query);
}

// LCOV_EXCL_START
std::string TpcdsExtension::Name() {
	return "tpcds";
}

std::string TpcdsExtension::Version() const {
#ifdef EXT_VERSION_TPCDS
	return EXT_VERSION_TPCDS;
#else
	return "";
#endif
}
// LCOV_EXCL_STOP

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(tpcds, loader) {
	duckdb::LoadInternal(loader);
}
}
