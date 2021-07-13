#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parser/sql_statement.hpp"

namespace duckdb {

PreparedStatementData::PreparedStatementData(StatementType type)
    : statement_type(type), read_only(true), requires_valid_transaction(true), allow_stream_result(false) {
}

PreparedStatementData::~PreparedStatementData() {
}

void PreparedStatementData::Bind(vector<Value> values) {
	// set parameters
	if (values.size() != value_map.size()) {
		throw BinderException("Parameter/argument count mismatch for prepared statement. Expected %llu, got %llu",
		                      value_map.size(), values.size());
	}
	// bind the values
	for (idx_t i = 0; i < values.size(); i++) {
		auto it = value_map.find(i + 1);
		if (it == value_map.end()) {
			throw BinderException("Could not find parameter with index %llu", i + 1);
		}
		D_ASSERT(!it->second.empty());
		if (!values[i].TryCastAs(it->second[0]->type())) {
			throw BinderException(
			    "Type mismatch for binding parameter with index %llu, expected type %s but got type %s", i + 1,
			    it->second[0]->type().ToString().c_str(), values[i].type().ToString().c_str());
		}
		for (auto &target : it->second) {
			*target = values[i];
		}
	}
}

LogicalType PreparedStatementData::GetType(idx_t param_idx) {
	auto it = value_map.find(param_idx);
	if (it == value_map.end()) {
		throw BinderException("Could not find parameter with index %llu", param_idx);
	}
	D_ASSERT(!it->second.empty());
	return it->second[0]->type();
}

} // namespace duckdb
