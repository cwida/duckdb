#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/planner/collation_binding.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

struct PragmaCollateData : public GlobalTableFunctionState {
	PragmaCollateData() : offset(0) {
	}

	vector<string> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> PragmaCollateBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("collname");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> PragmaCollateInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<PragmaCollateData>();

	result->entries = CollationFunctions::GetCollations(context);
	return std::move(result);
}

static void PragmaCollateFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<PragmaCollateData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}
	idx_t next = MinValue<idx_t>(data.offset + STANDARD_VECTOR_SIZE, data.entries.size());

	// collname, VARCHAR
	auto &collname = output.data[0];

	for (idx_t i = data.offset; i < next; i++) {
		collname.Append(Value(data.entries[i]));
	}
	data.offset = next;
}

void PragmaCollations::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(
	    TableFunction("pragma_collations", {}, PragmaCollateFunction, PragmaCollateBind, PragmaCollateInit));
}

} // namespace duckdb
