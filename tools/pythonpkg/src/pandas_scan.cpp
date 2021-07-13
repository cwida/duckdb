#include "duckdb_python/pandas_scan.hpp"
#include "duckdb_python/array_wrapper.hpp"
#include "duckdb/parallel/parallel_state.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb_python/vector_conversion.hpp"

#include "duckdb/common/atomic.hpp"

namespace duckdb {

struct PandasScanFunctionData : public TableFunctionData {
	PandasScanFunctionData(py::handle df, idx_t row_count, vector<PandasColumnBindData> pandas_bind_data,
	                       vector<LogicalType> sql_types)
	    : df(df), row_count(row_count), lines_read(0), pandas_bind_data(move(pandas_bind_data)),
	      sql_types(move(sql_types)) {
	}
	py::handle df;
	idx_t row_count;
	atomic<idx_t> lines_read;
	vector<PandasColumnBindData> pandas_bind_data;
	vector<LogicalType> sql_types;
};

struct PandasScanState : public FunctionOperatorData {
	PandasScanState(idx_t start, idx_t end) : start(start), end(end) {
	}

	idx_t start;
	idx_t end;
	vector<column_t> column_ids;
};

struct ParallelPandasScanState : public ParallelState {
	ParallelPandasScanState() : position(0) {
	}

	std::mutex lock;
	idx_t position;
};

PandasScanFunction::PandasScanFunction()
    : TableFunction("pandas_scan", {LogicalType::POINTER}, PandasScanFunc, PandasScanBind, PandasScanInit, nullptr,
                    nullptr, nullptr, PandasScanCardinality, nullptr, nullptr, PandasScanMaxThreads,
                    PandasScanInitParallelState, PandasScanFuncParallel, PandasScanParallelInit,
                    PandasScanParallelStateNext, true, false, PandasProgress) {
}

unique_ptr<FunctionData> PandasScanFunction::PandasScanBind(ClientContext &context, vector<Value> &inputs,
                                                            unordered_map<string, Value> &named_parameters,
                                                            vector<LogicalType> &input_table_types,
                                                            vector<string> &input_table_names,
                                                            vector<LogicalType> &return_types, vector<string> &names) {
	py::gil_scoped_acquire acquire;
	py::handle df((PyObject *)(inputs[0].GetPointer()));

	vector<PandasColumnBindData> pandas_bind_data;
	VectorConversion::BindPandas(df, pandas_bind_data, return_types, names);

	auto df_columns = py::list(df.attr("columns"));
	auto get_fun = df.attr("__getitem__");

	idx_t row_count = py::len(get_fun(df_columns[0]));
	return make_unique<PandasScanFunctionData>(df, row_count, move(pandas_bind_data), return_types);
}

unique_ptr<FunctionOperatorData> PandasScanFunction::PandasScanInit(ClientContext &context,
                                                                    const FunctionData *bind_data_p,
                                                                    const vector<column_t> &column_ids,
                                                                    TableFilterCollection *filters) {
	auto &bind_data = (const PandasScanFunctionData &)*bind_data_p;
	auto result = make_unique<PandasScanState>(0, bind_data.row_count);
	result->column_ids = column_ids;
	return move(result);
}

idx_t PandasScanFunction::PandasScanMaxThreads(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = (const PandasScanFunctionData &)*bind_data_p;
	return bind_data.row_count / PANDAS_PARTITION_COUNT + 1;
}

unique_ptr<ParallelState> PandasScanFunction::PandasScanInitParallelState(ClientContext &context,
                                                                          const FunctionData *bind_data_p) {
	return make_unique<ParallelPandasScanState>();
}

unique_ptr<FunctionOperatorData> PandasScanFunction::PandasScanParallelInit(ClientContext &context,
                                                                            const FunctionData *bind_data_p,
                                                                            ParallelState *state,
                                                                            const vector<column_t> &column_ids,
                                                                            TableFilterCollection *filters) {
	auto result = make_unique<PandasScanState>(0, 0);
	result->column_ids = column_ids;
	if (!PandasScanParallelStateNext(context, bind_data_p, result.get(), state)) {
		return nullptr;
	}
	return move(result);
}

bool PandasScanFunction::PandasScanParallelStateNext(ClientContext &context, const FunctionData *bind_data_p,
                                                     FunctionOperatorData *operator_state,
                                                     ParallelState *parallel_state_p) {
	auto &bind_data = (const PandasScanFunctionData &)*bind_data_p;
	auto &parallel_state = (ParallelPandasScanState &)*parallel_state_p;
	auto &state = (PandasScanState &)*operator_state;

	lock_guard<mutex> parallel_lock(parallel_state.lock);
	if (parallel_state.position >= bind_data.row_count) {
		return false;
	}
	state.start = parallel_state.position;
	parallel_state.position += PANDAS_PARTITION_COUNT;
	if (parallel_state.position > bind_data.row_count) {
		parallel_state.position = bind_data.row_count;
	}
	state.end = parallel_state.position;
	return true;
}

int PandasScanFunction::PandasProgress(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = (const PandasScanFunctionData &)*bind_data_p;
	if (bind_data.row_count == 0) {
		return 100;
	}
	auto percentage = bind_data.lines_read * 100 / bind_data.row_count;
	return percentage;
}

void PandasScanFunction::PandasScanFuncParallel(ClientContext &context, const FunctionData *bind_data,
                                                FunctionOperatorData *operator_state, DataChunk *input,
                                                DataChunk &output, ParallelState *parallel_state_p) {
	//! FIXME: Have specialized parallel function from pandas scan here
	PandasScanFunc(context, bind_data, operator_state, input, output);
}

//! The main pandas scan function: note that this can be called in parallel without the GIL
//! hence this needs to be GIL-safe, i.e. no methods that create Python objects are allowed
void PandasScanFunction::PandasScanFunc(ClientContext &context, const FunctionData *bind_data,
                                        FunctionOperatorData *operator_state, DataChunk *input, DataChunk &output) {
	auto &data = (PandasScanFunctionData &)*bind_data;
	auto &state = (PandasScanState &)*operator_state;

	if (state.start >= state.end) {
		return;
	}
	idx_t this_count = std::min((idx_t)STANDARD_VECTOR_SIZE, state.end - state.start);
	output.SetCardinality(this_count);
	for (idx_t idx = 0; idx < state.column_ids.size(); idx++) {
		auto col_idx = state.column_ids[idx];
		if (col_idx == COLUMN_IDENTIFIER_ROW_ID) {
			output.data[idx].Sequence(state.start, this_count);
		} else {
			VectorConversion::NumpyToDuckDB(data.pandas_bind_data[col_idx], data.pandas_bind_data[col_idx].numpy_col,
			                                this_count, state.start, output.data[idx]);
		}
	}
	state.start += this_count;
	data.lines_read += this_count;
}

unique_ptr<NodeStatistics> PandasScanFunction::PandasScanCardinality(ClientContext &context,
                                                                     const FunctionData *bind_data) {
	auto &data = (PandasScanFunctionData &)*bind_data;
	return make_unique<NodeStatistics>(data.row_count, data.row_count);
}

} // namespace duckdb
