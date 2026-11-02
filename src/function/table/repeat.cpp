#include "duckdb/function/table/range.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/atomic.hpp"

namespace duckdb {

struct RepeatFunctionData : public TableFunctionData {
	RepeatFunctionData(Value value, idx_t target_count) : value(std::move(value)), target_count(target_count) {
	}

	Value value;
	idx_t target_count;
};

struct RepeatOperatorData : public GlobalTableFunctionState {
	RepeatOperatorData() : current_count(0) {
	}
	atomic<idx_t> current_count;
};

static unique_ptr<FunctionData> RepeatBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<Identifier> &names) {
	// the repeat function returns the type of the first argument
	auto &inputs = input.inputs;
	return_types.push_back(inputs[0].type());
	names.emplace_back(inputs[0].ToString());
	if (inputs[1].IsNull()) {
		throw BinderException("Repeat second parameter cannot be NULL");
	}
	auto repeat_count = inputs[1].GetValue<int64_t>();
	if (repeat_count < 0) {
		throw BinderException("Repeat second parameter cannot be be less than 0");
	}
	return make_uniq<RepeatFunctionData>(inputs[0], NumericCast<idx_t>(repeat_count));
}

static unique_ptr<GlobalTableFunctionState> RepeatInit(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<RepeatOperatorData>();
}

static void RepeatFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<RepeatFunctionData>();
	auto &state = data_p.global_state->Cast<RepeatOperatorData>();

	idx_t remaining = MinValue<idx_t>(bind_data.target_count - state.current_count, STANDARD_VECTOR_SIZE);
	output.data[0].Reference(bind_data.value, count_t(remaining));
	output.SetChildCardinality(remaining);
	state.current_count += remaining;
}

static double RepeatProgress(ClientContext &, const FunctionData *bind_data_p,
                             const GlobalTableFunctionState *state_p) {
	if (!state_p) {
		return -1;
	}
	auto &bind_data = bind_data_p->Cast<RepeatFunctionData>();
	auto &state = state_p->Cast<RepeatOperatorData>();
	return bind_data.target_count == 0 ? 100
	                                   : 100.0 * double(state.current_count.load()) / double(bind_data.target_count);
}

static unique_ptr<NodeStatistics> RepeatCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<RepeatFunctionData>();
	return make_uniq<NodeStatistics>(bind_data.target_count, bind_data.target_count);
}

void RepeatTableFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunction repeat("repeat",
	                     FunctionSignature()
	                         .AddPositionalOnly("value", LogicalType::ANY)
	                         .AddPositionalOnly("count", LogicalType::BIGINT),
	                     RepeatFunction, RepeatBind, RepeatInit);
	repeat.cardinality = RepeatCardinality;
	repeat.table_scan_progress = RepeatProgress;
	set.AddFunction(repeat);
}

} // namespace duckdb
