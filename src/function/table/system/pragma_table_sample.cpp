#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/binder.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"

#include <algorithm>

namespace duckdb {

struct DuckDBTableSampleFunctionData : public TableFunctionData {
	explicit DuckDBTableSampleFunctionData(CatalogEntry &entry_p) : entry(entry_p) {
	}
	CatalogEntry &entry;
};

struct DuckDBTableSampleOperatorData : public GlobalTableFunctionState {
	DuckDBTableSampleOperatorData() : sample_offset(0) {
		sample = nullptr;
	}
	idx_t sample_offset;
	unique_ptr<BlockingSample> sample;
};

static unique_ptr<FunctionData> DuckDBTableSampleBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	// look up the table name in the catalog
	QualifiedName qname;
	string error;
	if (!QualifiedName::TryParse(input.inputs[0].GetValue<string>(), qname, error)) {
		throw ParserException(error);
	}
	CatalogEntryRetriever retriever(context);
	qname = Binder::BindTableName(retriever, qname);

	auto &entry = Catalog::GetEntry<TableCatalogEntry>(context, qname);
	if (entry.type != CatalogType::TABLE_ENTRY) {
		throw NotImplementedException("Invalid Catalog type passed to table_sample()");
	}
	auto &table_entry = entry.Cast<TableCatalogEntry>();
	auto types = table_entry.GetTypes();
	for (auto &type : types) {
		return_types.push_back(type);
	}
	for (idx_t i = 0; i < types.size(); i++) {
		auto logical_index = LogicalIndex(i);
		auto &col = table_entry.GetColumn(logical_index);
		names.push_back(col.GetName());
	}

	Binder::RegisterEntryRead(input.binder, context, entry);
	return make_uniq<DuckDBTableSampleFunctionData>(entry);
}

unique_ptr<GlobalTableFunctionState> DuckDBTableSampleInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<DuckDBTableSampleOperatorData>();
}

static void DuckDBTableSampleTable(ClientContext &context, DuckDBTableSampleOperatorData &data,
                                   TableCatalogEntry &table, DataChunk &output) {
	// if table has statistics.
	// copy the sample of statistics into the output chunk
	if (!data.sample) {
		data.sample = table.GetSample();
	}
	if (data.sample) {
		auto sample_chunk = data.sample->GetChunk();
		if (sample_chunk) {
			sample_chunk->Copy(output, 0);
			data.sample_offset += sample_chunk->size();
		}
	}
}

static void DuckDBTableSampleFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<DuckDBTableSampleFunctionData>();
	auto &state = data_p.global_state->Cast<DuckDBTableSampleOperatorData>();
	switch (bind_data.entry.type) {
	case CatalogType::TABLE_ENTRY:
		DuckDBTableSampleTable(context, state, bind_data.entry.Cast<TableCatalogEntry>(), output);
		break;
	default:
		throw NotImplementedException("Unimplemented catalog type for pragma_table_sample");
	}
}

void DuckDBTableSample::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(TableFunction("duckdb_table_sample",
	                              FunctionSignature().AddPositionalOnly("table_name", LogicalType::VARCHAR),
	                              DuckDBTableSampleFunction, DuckDBTableSampleBind, DuckDBTableSampleInit));
}

} // namespace duckdb
