#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/platform.hpp"

#include <cstdint>

namespace duckdb {

struct PragmaVersionData : public GlobalTableFunctionState {
	PragmaVersionData() : finished(false) {
	}

	bool finished;
};

static unique_ptr<FunctionData> PragmaVersionBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("library_version");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("source_id");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("codename");
	return_types.emplace_back(LogicalType::VARCHAR);
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> PragmaVersionInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<PragmaVersionData>();
}

static void PragmaVersionFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<PragmaVersionData>();
	if (data.finished) {
		// finished returning values
		return;
	}
	output.data[0].Append(Value(DuckDB::LibraryVersion()));
	output.data[1].Append(Value(DuckDB::SourceID()));
	output.data[2].Append(Value(DuckDB::ReleaseCodename()));

	data.finished = true;
}

void PragmaVersion::RegisterFunction(BuiltinFunctions &set) {
	TableFunction pragma_version("pragma_version", {}, PragmaVersionFunction);
	pragma_version.bind = PragmaVersionBind;
	pragma_version.init_global = PragmaVersionInit;
	set.AddFunction(pragma_version);
}

idx_t DuckDB::StandardVectorSize() {
	return STANDARD_VECTOR_SIZE;
}

const char *DuckDB::SourceID() {
	return DUCKDB_SOURCE_ID;
}

const char *DuckDB::LibraryVersion() {
	return DUCKDB_VERSION;
}

const char *DuckDB::ReleaseCodename() {
	// dev releases have no name
	if (!VersioningUtils::IsReleaseVersion(DUCKDB_VERSION)) {
		return "Development Version";
	}
	if (StringUtil::StartsWith(DUCKDB_VERSION, "v1.2.")) {
		return "Histrionicus";
	}
	if (StringUtil::StartsWith(DUCKDB_VERSION, "v1.3.")) {
		return "Ossivalis";
	}
	if (StringUtil::StartsWith(DUCKDB_VERSION, "v1.4.")) {
		return "Andium";
	}
	if (StringUtil::StartsWith(DUCKDB_VERSION, "v1.5.")) {
		return "Variegata";
	}
	if (StringUtil::StartsWith(DUCKDB_VERSION, "v2.0.")) {
		return "Cyanoptera";
	}
	if (StringUtil::StartsWith(DUCKDB_VERSION, "v2.1.")) {
		return "Islandica";
	}
	// add new version names here

	// we should not get here, but let's not fail because of it because tags on forks can be whatever
	return "Unknown Version";
}

string DuckDB::Platform() {
	return DuckDBPlatform();
}

struct PragmaPlatformData : public GlobalTableFunctionState {
	PragmaPlatformData() : finished(false) {
	}

	bool finished;
};

static unique_ptr<FunctionData> PragmaPlatformBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("platform");
	return_types.emplace_back(LogicalType::VARCHAR);
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> PragmaPlatformInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<PragmaPlatformData>();
}

static void PragmaPlatformFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<PragmaPlatformData>();
	if (data.finished) {
		// finished returning values
		return;
	}
	output.data[0].Append(Value(DuckDB::Platform()));
	data.finished = true;
}

void PragmaPlatform::RegisterFunction(BuiltinFunctions &set) {
	TableFunction pragma_platform("pragma_platform", {}, PragmaPlatformFunction);
	pragma_platform.bind = PragmaPlatformBind;
	pragma_platform.init_global = PragmaPlatformInit;
	set.AddFunction(pragma_platform);
}

} // namespace duckdb
