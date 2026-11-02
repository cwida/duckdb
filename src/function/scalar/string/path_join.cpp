#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar/string_functions.hpp"

namespace duckdb {

namespace {

struct PathJoinBindData : public FunctionData {
	explicit PathJoinBindData(idx_t path_count_p) : path_count(path_count_p) {
	}

	idx_t path_count;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<PathJoinBindData>(path_count);
	}

	bool Equals(const FunctionData &other) const override {
		auto &o = other.Cast<PathJoinBindData>();
		return path_count == o.path_count;
	}
};

// Process one output row; returns false if any input is NULL.
static bool ProcessRow(idx_t row_idx, const vector<VectorIterator<string_t>> &inputs, idx_t col_count,
                       string &out_result) {
	Path out_path;
	for (idx_t col_idx = 0; col_idx < col_count; col_idx++) {
		auto &vdata = inputs[col_idx];
		auto input_val = vdata[row_idx];
		if (!input_val.IsValid()) {
			return false;
		}
		auto input_value = input_val.GetValue().GetString();
		if (col_idx == 0) {
			out_path = Path::FromString(input_value);
		} else {
			out_path = out_path.Join(input_value);
		}
	}
	out_result = out_path.ToString();
	return true;
}

void PathJoinFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	auto col_count = args.ColumnCount();
	vector<VectorIterator<string_t>> inputs;
	for (idx_t col_idx = 0; col_idx < args.ColumnCount(); col_idx++) {
		inputs.emplace_back(args.data[col_idx].Values<string_t>());
	}

	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		string joined;
		if (!ProcessRow(row_idx, inputs, col_count, joined)) {
			result_data.WriteNull();
			continue;
		}
		result_data.WriteValue(string_t(joined));
	}
}

} // namespace

ScalarFunction PathJoinFun::GetFunction() {
	ScalarFunction path_join(PathJoinFun::Name, {}, LogicalType::VARCHAR, PathJoinFunction);
	path_join.GetSignature().AddParameter("path", LogicalType::VARCHAR);
	path_join.GetSignature().AddArgs("args", LogicalType::VARCHAR);
	path_join.SetNullHandling(FunctionNullHandling::DEFAULT_NULL_HANDLING);
	// throws if the paths that are joined are incompatible
	path_join.SetFallible();
	return path_join;
}

} // namespace duckdb
