#include "duckdb/function/table/range.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/atomic.hpp"

namespace duckdb {

struct RepeatRowFunctionData : public TableFunctionData {
	RepeatRowFunctionData(vector<Value> values, idx_t target_count)
	    : values(std::move(values)), target_count(target_count) {
	}

	const vector<Value> values;
	idx_t target_count;
};

struct RepeatRowOperatorData : public GlobalTableFunctionState {
	RepeatRowOperatorData() : current_count(0) {
	}
	atomic<idx_t> current_count;
};

static unique_ptr<FunctionData> RepeatRowBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &inputs = input.inputs;
	for (idx_t input_idx = 0; input_idx < inputs.size(); input_idx++) {
		return_types.push_back(inputs[input_idx].type());
		names.emplace_back("column" + std::to_string(input_idx));
	}
	auto &num_rows = input.named_parameters.at("num_rows");
	if (inputs.empty()) {
		throw BinderException("repeat_rows requires at least one column to be specified");
	}
	if (num_rows.IsNull()) {
		throw BinderException("num_rows should be an integer value >= 0");
	}
	auto repeat_rows = num_rows.GetValue<int64_t>();
	if (repeat_rows < 0) {
		throw BinderException("num_rows cannot be less than zero");
	}
	return make_uniq<RepeatRowFunctionData>(inputs, NumericCast<idx_t>(repeat_rows));
}

static unique_ptr<GlobalTableFunctionState> RepeatRowInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<RepeatRowOperatorData>();
}

static void RepeatRowFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<RepeatRowFunctionData>();
	auto &state = data_p.global_state->Cast<RepeatRowOperatorData>();

	idx_t remaining = MinValue<idx_t>(bind_data.target_count - state.current_count, STANDARD_VECTOR_SIZE);
	for (idx_t val_idx = 0; val_idx < bind_data.values.size(); val_idx++) {
		output.data[val_idx].Reference(bind_data.values[val_idx], count_t(remaining));
	}
	output.SetChildCardinality(remaining);
	state.current_count += remaining;
}

static double RepeatRowProgress(ClientContext &, const FunctionData *bind_data_p,
                                const GlobalTableFunctionState *state_p) {
	if (!state_p) {
		return -1;
	}
	auto &bind_data = bind_data_p->Cast<RepeatRowFunctionData>();
	auto &state = state_p->Cast<RepeatRowOperatorData>();
	return bind_data.target_count == 0 ? 100
	                                   : 100.0 * double(state.current_count.load()) / double(bind_data.target_count);
}

static unique_ptr<NodeStatistics> RepeatRowCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<RepeatRowFunctionData>();
	return make_uniq<NodeStatistics>(bind_data.target_count, bind_data.target_count);
}

void RepeatRowTableFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunction repeat_row("repeat_row", {}, RepeatRowFunction, RepeatRowBind, RepeatRowInit);
	// a positional list, so it declares "*args" and no "**kwargs" - an argument named after no parameter is an error
	repeat_row.GetSignature().AddArgs("args", LogicalType::ANY);
	repeat_row.GetSignature().AddKeywordOnly("num_rows", LogicalType::BIGINT);
	repeat_row.cardinality = RepeatRowCardinality;
	repeat_row.table_scan_progress = RepeatRowProgress;
	set.AddFunction(repeat_row);
}

} // namespace duckdb
