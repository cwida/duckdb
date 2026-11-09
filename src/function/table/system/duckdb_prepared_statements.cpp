#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/prepared_statement_data.hpp"

namespace duckdb {

struct DuckDBPreparedStatementsData : public GlobalTableFunctionState {
	DuckDBPreparedStatementsData() : offset(0) {
	}

	vector<std::pair<string, shared_ptr<PreparedStatementData>>> entries;
	idx_t offset;
};

static unique_ptr<FunctionData> DuckDBPreparedStatementsBind(ClientContext &context, TableFunctionBindInput &input,
                                                             vector<LogicalType> &return_types,
                                                             vector<Identifier> &names) {
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("statement");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("parameter_types");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("result_types");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
	return nullptr;
}

unique_ptr<GlobalTableFunctionState> DuckDBPreparedStatementsInit(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBPreparedStatementsData>();
	auto &prepared_statements = context.client_data->prepared_statements;
	for (auto &it : prepared_statements) {
		result->entries.emplace_back(it.first, it.second);
	}
	return std::move(result);
}

void DuckDBPreparedStatementsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBPreparedStatementsData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;

	// name, VARCHAR
	auto &name_vec = output.data[0];
	// statement, VARCHAR
	auto &statement_vec = output.data[1];
	// parameter_types, VARCHAR[]
	auto &parameter_types_vec = output.data[2];
	// result_types, VARCHAR[]
	auto &result_types_vec = output.data[3];

	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset++];
		auto &name = entry.first;
		auto &prepared_statement = *entry.second;

		name_vec.Append(Value(name));
		statement_vec.Append(Value(prepared_statement.unbound_statement->ToString()));
		auto &named_parameter_map = prepared_statement.unbound_statement->named_param_map;
		if (named_parameter_map.empty()) {
			parameter_types_vec.Append(Value(LogicalType::LIST(LogicalType::VARCHAR)));
		} else {
			vector<Value> parameter_types;
			for (idx_t i = 0; i < prepared_statement.properties.parameter_count; i++) {
				parameter_types.push_back(LogicalType(LogicalTypeId::UNKNOWN).ToString());
			}
			parameter_types_vec.Append(Value::LIST(std::move(parameter_types)));
		}
		switch (prepared_statement.properties.return_type) {
		case StatementReturnType::QUERY_RESULT: {
			if (prepared_statement.physical_plan) {
				auto plan_types = prepared_statement.physical_plan->Root().GetTypes();
				vector<Value> return_types;
				for (auto &type : plan_types) {
					return_types.push_back(type.ToString());
				}
				result_types_vec.Append(Value::LIST(return_types));
			} else {
				result_types_vec.Append(Value(LogicalType::LIST(LogicalType::VARCHAR)));
			}
			break;
		}
		case StatementReturnType::CHANGED_ROWS: {
			result_types_vec.Append(Value::LIST({"BIGINT"}));
			break;
		}
		case StatementReturnType::NOTHING:
		default: {
			result_types_vec.Append(Value(LogicalType::LIST(LogicalType::VARCHAR)));
			break;
		}
		}
		count++;
	}
}

void DuckDBPreparedStatementsFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction prepared_fun("duckdb_prepared_statements", {}, DuckDBPreparedStatementsFunction,
	                           DuckDBPreparedStatementsBind, DuckDBPreparedStatementsInit);
	// reads the live prepared statement registry
	prepared_fun.stability = FunctionStability::VOLATILE;
	set.AddFunction(std::move(prepared_fun));
}

} // namespace duckdb
