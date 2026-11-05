#include "duckdb/function/table/summary.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/common/file_system.hpp"

// this function makes not that much sense on its own but is a demo for table-parameter table-producing functions

namespace duckdb {

static unique_ptr<FunctionData> SummaryFunctionBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("summary");

	for (idx_t i = 0; i < input.input_table_types.size(); i++) {
		return_types.push_back(input.input_table_types[i]);
		names.push_back(input.input_table_names[i]);
	}

	return make_uniq<TableFunctionData>();
}

static OperatorResultType SummaryFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                          DataChunk &output) {
	auto &summary_col = output.data[0];
	for (idx_t row_idx = 0; row_idx < input.size(); row_idx++) {
		string summary_val = "[";

		for (idx_t col_idx = 0; col_idx < input.ColumnCount(); col_idx++) {
			summary_val += input.GetValue(col_idx, row_idx).ToString();
			if (col_idx < input.ColumnCount() - 1) {
				summary_val += ", ";
			}
		}
		summary_val += "]";
		summary_col.Append(Value(summary_val));
	}
	for (idx_t col_idx = 0; col_idx < input.ColumnCount(); col_idx++) {
		output.data[col_idx + 1].Reference(input.data[col_idx]);
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

void SummaryTableFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunction summary_function("summary", FunctionSignature().AddPositionalOnly("input", LogicalType::TABLE),
	                               nullptr, SummaryFunctionBind);
	summary_function.in_out_function = SummaryFunction;
	set.AddFunction(summary_function);
}

} // namespace duckdb
