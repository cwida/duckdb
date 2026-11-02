#include "duckdb/function/table_function.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/planner/binder.hpp"

#include "dbgen/dbgen.hpp"
#include "tpch_extension.hpp"

#include <atomic>
#include <mutex>

namespace duckdb {

struct DBGenFunctionData : public TableFunctionData {
	DBGenFunctionData() {
	}

	double sf = 0;
	Identifier catalog = INVALID_CATALOG;
	Identifier schema = DEFAULT_SCHEMA;
	string suffix;
	bool overwrite = false;
	uint32_t children = 1;
	int step = -1;
};

struct DBGenGlobalState : public GlobalTableFunctionState {
	bool schema_created = false;
	atomic<bool> finished {false};
	mutable mutex generator_lock;
	unique_ptr<tpch::DBGenGenerator> generator;
};

class DBGenYieldTask : public AsyncTask {
public:
	void Execute() override {
	}
};

static AsyncResult DBGenYield() {
	vector<unique_ptr<AsyncTask>> tasks;
	tasks.push_back(make_uniq<DBGenYieldTask>());
	return AsyncResult(std::move(tasks));
}

static unique_ptr<FunctionData> DbgenBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<DBGenFunctionData>();

	// Set the current catalog and schema.
	const auto current_catalog = DatabaseManager::GetDefaultDatabase(context);
	const auto current_schema = ClientData::Get(context).catalog_search_path->GetDefault().GetSchema();
	result->catalog = current_catalog;
	result->schema = current_schema;

	for (auto &kv : input.named_parameters) {
		if (kv.second.IsNull()) {
			throw BinderException("Cannot use NULL as function argument");
		}
		if (kv.first == "sf") {
			result->sf = DoubleValue::Get(kv.second);
		} else if (kv.first == "catalog") {
			result->catalog = Identifier(StringValue::Get(kv.second));
		} else if (kv.first == "schema") {
			result->schema = Identifier(StringValue::Get(kv.second));
		} else if (kv.first == "suffix") {
			result->suffix = StringValue::Get(kv.second);
		} else if (kv.first == "overwrite") {
			result->overwrite = BooleanValue::Get(kv.second);
		} else if (kv.first == "children") {
			result->children = UIntegerValue::Get(kv.second);
		} else if (kv.first == "step") {
			result->step = UIntegerValue::Get(kv.second);
		}
	}
	if (result->children != 1 && result->step == -1) {
		throw InvalidInputException("Step must be defined when children are defined");
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

unique_ptr<GlobalTableFunctionState> DbgenInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<DBGenGlobalState>();
}

static void DbgenFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->Cast<DBGenFunctionData>();
	auto &state = data_p.global_state->Cast<DBGenGlobalState>();
	if (state.finished.load()) {
		data_p.async_result = AsyncResultType::FINISHED;
		return;
	}
	if (!state.schema_created) {
		tpch::DBGenWrapper::CreateTPCHSchema(context, data.catalog, data.schema, data.suffix);
		auto generator = tpch::CreateDBGenGenerator(context, data.sf, data.catalog, data.schema, data.suffix,
		                                            data.children, data.step);
		{
			lock_guard<mutex> guard(state.generator_lock);
			state.generator = std::move(generator);
		}
		state.schema_created = true;
	}

	while (true) {
		bool finished = false;
		{
			lock_guard<mutex> guard(state.generator_lock);
			finished = !state.generator || state.generator->GenerateNext();
		}
		if (finished) {
			state.finished.store(true);
			data_p.async_result = AsyncResultType::FINISHED;
			return;
		}
		if (data_p.results_execution_mode == AsyncResultsExecutionMode::TASK_EXECUTOR) {
			data_p.async_result = DBGenYield();
			return;
		}
	}
}

static double DbgenProgress(ClientContext &context, const FunctionData *bind_data,
                            const GlobalTableFunctionState *global_state) {
	if (!global_state) {
		return 0.0;
	}
	auto &state = global_state->Cast<DBGenGlobalState>();
	{
		lock_guard<mutex> guard(state.generator_lock);
		if (state.generator) {
			return state.generator->Progress();
		}
	}
	return state.finished.load() ? 100.0 : 0.0;
}

struct TPCHData : public GlobalTableFunctionState {
	TPCHData() : offset(0) {
	}
	idx_t offset;
};

unique_ptr<GlobalTableFunctionState> TPCHInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<TPCHData>();
	return std::move(result);
}

struct TPCHQueriesBindData : public TableFunctionData {
	//! Without a scale factor the stored queries are returned unchanged
	bool has_sf = false;
	double sf = 1;

	string GetQuery(int query) const {
		return has_sf ? tpch::DBGenWrapper::GetQuery(query, sf) : tpch::DBGenWrapper::GetQuery(query);
	}
};

//! The scale factor is either the positional argument or the named "sf" parameter
static void BindQueryScaleFactor(TableFunctionBindInput &input, const char *function_name,
                                 TPCHQueriesBindData &bind_data) {
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

static duckdb::unique_ptr<FunctionData> TPCHQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<TPCHQueriesBindData>();
	BindQueryScaleFactor(input, "tpch_queries", *result);

	names.emplace_back("query_nr");
	return_types.emplace_back(LogicalType::INTEGER);

	names.emplace_back("query");
	return_types.emplace_back(LogicalType::VARCHAR);

	return std::move(result);
}

static void TPCHQueryFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<TPCHQueriesBindData>();
	auto &data = data_p.global_state->Cast<TPCHData>();
	idx_t tpch_queries = 22;
	if (data.offset >= tpch_queries) {
		// finished returning values
		return;
	}
	idx_t chunk_count = 0;

	// query_nr, INTEGER
	auto &query_nr = output.data[0];
	// query, VARCHAR
	auto &query_col = output.data[1];

	while (data.offset < tpch_queries && chunk_count < STANDARD_VECTOR_SIZE) {
		auto query = bind_data.GetQuery(data.offset + 1);
		query_nr.Append(Value::INTEGER((int32_t)data.offset + 1));
		query_col.Append(Value(query));
		data.offset++;
		chunk_count++;
	}
}

static duckdb::unique_ptr<FunctionData> TPCHQueryAnswerBind(ClientContext &context, TableFunctionBindInput &input,
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

static void TPCHQueryAnswerFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<TPCHData>();
	idx_t tpch_queries = 22;
	vector<double> scale_factors {0.01, 0.1, 1};
	idx_t total_answers = tpch_queries * scale_factors.size();
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
		idx_t cur_query = data.offset % tpch_queries;
		idx_t cur_sf = data.offset / tpch_queries;
		auto answer = tpch::DBGenWrapper::GetAnswer(scale_factors[cur_sf], cur_query + 1);
		query_nr.Append(Value::INTEGER((int32_t)cur_query + 1));
		scale_factor.Append(Value::DOUBLE(scale_factors[cur_sf]));
		answer_col.Append(Value(answer));
		data.offset++;
		chunk_count++;
	}
}

static string PragmaTpchQuery(ClientContext &context, const FunctionParameters &parameters) {
	if (parameters.values[0].IsNull()) {
		throw InvalidInputException("Cannot use NULL as argument for the TPC-H query number");
	}
	auto index = parameters.values[0].GetValue<int32_t>();
	auto sf_entry = parameters.named_parameters.find("sf");
	if (sf_entry == parameters.named_parameters.end()) {
		return tpch::DBGenWrapper::GetQuery(index);
	}
	if (sf_entry->second.IsNull()) {
		throw InvalidInputException("Cannot use NULL as scale factor for the TPC-H query");
	}
	return tpch::DBGenWrapper::GetQuery(index, sf_entry->second.GetValue<double>());
}

static void LoadInternal(ExtensionLoader &loader) {
	TableFunction dbgen_func("dbgen", {}, DbgenFunction, DbgenBind, DbgenInit);
	dbgen_func.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("sf", LogicalType::DOUBLE)
		    .Add("overwrite", LogicalType::BOOLEAN)
		    .Add("catalog", LogicalType::VARCHAR)
		    .Add("schema", LogicalType::VARCHAR)
		    .Add("suffix", LogicalType::VARCHAR)
		    .Add("children", LogicalType::UINTEGER)
		    .Add("step", LogicalType::UINTEGER);
	});
	dbgen_func.call_return_type = StatementReturnType::NOTHING;
	dbgen_func.table_scan_progress = DbgenProgress;
	loader.RegisterFunction(dbgen_func);

	// create the TPCH pragma that allows us to run the query
	auto tpch_func = PragmaFunction::PragmaCall("tpch", PragmaTpchQuery,
	                                            FunctionSignature().AddPositionalOnly("query_nr", LogicalType::BIGINT));
	tpch_func.GetSignature().WithTypedKwargs("options",
	                                         [](TypedKwargs &options) { options.Add("sf", LogicalType::DOUBLE); });
	loader.RegisterFunction(tpch_func);

	// create the TPCH_QUERIES function that returns the queries, optionally parameterized for a scale factor
	TableFunctionSet tpch_queries_set("tpch_queries");
	for (auto &arguments : vector<vector<LogicalType>> {{}, {LogicalType::DOUBLE}}) {
		TableFunction tpch_query_func(arguments, TPCHQueryFunction, TPCHQueryBind, TPCHInit);
		tpch_query_func.GetSignature().WithTypedKwargs(
		    "options", [](TypedKwargs &options) { options.Add("sf", LogicalType::DOUBLE); });
		tpch_queries_set.AddFunction(tpch_query_func);
	}
	loader.RegisterFunction(tpch_queries_set);

	// create the TPCH_ANSWERS that returns the query result
	TableFunction tpch_query_answer_func("tpch_answers", {}, TPCHQueryAnswerFunction, TPCHQueryAnswerBind, TPCHInit);
	loader.RegisterFunction(tpch_query_answer_func);
}

// LCOV_EXCL_START
void TpchExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
// LCOV_EXCL_STOP

std::string TpchExtension::GetQuery(int query) {
	return tpch::DBGenWrapper::GetQuery(query);
}

std::string TpchExtension::GetQuery(int query, double sf) {
	return tpch::DBGenWrapper::GetQuery(query, sf);
}

std::string TpchExtension::GetAnswer(double sf, int query) {
	return tpch::DBGenWrapper::GetAnswer(sf, query);
}

// LCOV_EXCL_START
std::string TpchExtension::Name() {
	return "tpch";
}

std::string TpchExtension::Version() const {
#ifdef EXT_VERSION_TPCH
	return EXT_VERSION_TPCH;
#else
	return "";
#endif
}
// LCOV_EXCL_STOP

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(tpch, loader) {
	duckdb::LoadInternal(loader);
}
}
