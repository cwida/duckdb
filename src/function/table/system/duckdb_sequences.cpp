#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/sequence_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"

namespace duckdb {

struct DuckDBSequencesData : public GlobalTableFunctionState {
	DuckDBSequencesData() : offset(0) {
	}

	vector<reference<SequenceCatalogEntry>> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBSequencesBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("database_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("schema_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("sequence_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("sequence_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("comment");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));

	names.emplace_back("temporary");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("start_value");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("min_value");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("max_value");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("increment_by");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("cycle");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("last_value");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("sql");
	return_types.emplace_back(LogicalType::VARCHAR);

	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBSequencesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBSequencesData>();

	// scan all the schemas for tables and collect themand collect them
	auto schemas = Catalog::GetAllSchemas(context);
	for (auto &schema : schemas) {
		schema.get().Scan(context, CatalogType::SEQUENCE_ENTRY,
		                  [&](CatalogEntry &entry) { result->entries.push_back(entry.Cast<SequenceCatalogEntry>()); });
	};
	return std::move(result);
}

void DuckDBSequencesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBSequencesData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;

	// database_name, VARCHAR
	auto &database_name = output.data[0];
	// database_oid, BIGINT
	auto &database_oid = output.data[1];
	// schema_name, VARCHAR
	auto &schema_name = output.data[2];
	// schema_oid, BIGINT
	auto &schema_oid = output.data[3];
	// sequence_name, VARCHAR
	auto &sequence_name = output.data[4];
	// sequence_oid, BIGINT
	auto &sequence_oid = output.data[5];
	// comment, VARCHAR
	auto &comment = output.data[6];
	// tags, MAP(VARCHAR, VARCHAR)
	auto &tags = output.data[7];
	// temporary, BOOLEAN
	auto &temporary = output.data[8];
	// start_value, BIGINT
	auto &start_value = output.data[9];
	// min_value, BIGINT
	auto &min_value = output.data[10];
	// max_value, BIGINT
	auto &max_value = output.data[11];
	// increment_by, BIGINT
	auto &increment_by = output.data[12];
	// cycle, BOOLEAN
	auto &cycle = output.data[13];
	// last_value, BIGINT
	auto &last_value = output.data[14];
	// sql, VARCHAR
	auto &sql = output.data[15];

	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &seq = data.entries[data.offset++].get();
		auto seq_data = seq.GetData();

		database_name.Append(Value(seq.catalog.GetName()));
		database_oid.Append(Value::BIGINT(NumericCast<int64_t>(seq.catalog.GetOid())));
		schema_name.Append(Value(seq.schema.name));
		schema_oid.Append(Value::BIGINT(NumericCast<int64_t>(seq.schema.oid)));
		sequence_name.Append(Value(seq.name));
		sequence_oid.Append(Value::BIGINT(NumericCast<int64_t>(seq.oid)));
		comment.Append(Value(seq.comment));
		tags.Append(Value::MAP(seq.tags));
		temporary.Append(Value::BOOLEAN(seq.temporary));
		start_value.Append(Value::BIGINT(seq_data.start_value));
		min_value.Append(Value::BIGINT(seq_data.min_value));
		max_value.Append(Value::BIGINT(seq_data.max_value));
		increment_by.Append(Value::BIGINT(seq_data.increment));
		cycle.Append(Value::BOOLEAN(seq_data.cycle));
		if (seq_data.usage_count == 0 || !seq_data.last_value) {
			last_value.Append(Value());
		} else {
			last_value.Append(Value::BIGINT(seq_data.last_value.value()));
		}
		sql.Append(Value(seq.ToSQL()));

		count++;
	}
}

void DuckDBSequencesFun::RegisterFunction(BuiltinFunctions &set) {
	auto function =
	    TableFunction("duckdb_sequences", {}, DuckDBSequencesFunction, DuckDBSequencesBind, DuckDBSequencesInit);
	set.AddFunction(std::move(function));
}

} // namespace duckdb
