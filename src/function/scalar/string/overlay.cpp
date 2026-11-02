#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/string_functions.hpp"

namespace duckdb {

namespace {

// Returns the byte offset of the start of the `char_pos`th character (1-based).
// Returns input_size if char_pos is past the end of the string.
// Clamps to 0 if char_pos <= 1.
static idx_t CharPosToByte(const char *data, idx_t input_size, int64_t char_pos) {
	if (char_pos <= 1) {
		return 0;
	}
	int64_t current = 1;
	for (idx_t i = 0; i < input_size; i++) {
		if (IsCharacter(data[i])) {
			if (current == char_pos) {
				return i;
			}
			current++;
		}
	}
	return input_size;
}

static int64_t CharLength(const char *data, idx_t size) {
	int64_t length = 0;
	for (idx_t i = 0; i < size; i++) {
		length += IsCharacter(data[i]);
	}
	return length;
}

void OverlayFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	auto input_values = args.data[0].Values<string_t>();
	auto repl_values = args.data[1].Values<string_t>();
	auto start_values = args.data[2].Values<int64_t>();
	optional<VectorIterator<int64_t>> count_values;
	if (args.ColumnCount() == 4) {
		count_values = args.data[3].Values<int64_t>();
	}

	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto input_entry = input_values[i];
		auto repl_entry = repl_values[i];
		auto start_entry = start_values[i];

		if (!input_entry.IsValid() || !repl_entry.IsValid() || !start_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}
		auto input = input_entry.GetValue();
		auto repl = repl_entry.GetValue();
		int64_t char_count = CharLength(repl.GetData(), repl.GetSize());
		if (count_values.has_value()) {
			auto count_entry = count_values.value()[i];
			if (!count_entry.IsValid()) {
				result_data.WriteNull();
				continue;
			}
			if (count_entry.GetValue() >= 0) {
				char_count = count_entry.GetValue();
			}
		}

		auto input_ptr = input.GetData();
		auto input_size = input.GetSize();
		auto repl_ptr = repl.GetData();
		auto repl_size = repl.GetSize();
		auto start = start_entry.GetValue();

		idx_t byte_start = CharPosToByte(input_ptr, input_size, start);
		int64_t end_pos = (char_count > 0 && start > NumericLimits<int64_t>::Maximum() - char_count)
		                      ? NumericLimits<int64_t>::Maximum()
		                      : start + char_count;
		idx_t byte_end = CharPosToByte(input_ptr, input_size, end_pos);
		idx_t after_size = input_size - byte_end;
		idx_t blob_size = byte_start + repl_size + after_size;

		auto &blob = result_data.WriteEmptyString(blob_size);
		auto blob_ptr = blob.GetDataWriteable();

		memcpy(blob_ptr, input_ptr, byte_start);
		memcpy(blob_ptr + byte_start, repl_ptr, repl_size);
		memcpy(blob_ptr + byte_start + repl_size, input_ptr + byte_end, after_size);

		blob.Finalize();
	}
}

} // namespace

ScalarFunctionSet OverlayFun::GetFunctions() {
	ScalarFunctionSet overlay_set("overlay");

	ScalarFunction three_arg({}, LogicalType::VARCHAR, OverlayFunction);
	three_arg.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("replacement", LogicalType::VARCHAR)
	    .AddParameter("start", LogicalType::BIGINT);
	overlay_set.AddFunction(three_arg);

	ScalarFunction four_arg({}, LogicalType::VARCHAR, OverlayFunction);
	four_arg.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("replacement", LogicalType::VARCHAR)
	    .AddParameter("start", LogicalType::BIGINT)
	    .AddParameter("count", LogicalType::BIGINT);
	overlay_set.AddFunction(four_arg);

	return overlay_set;
}

} // namespace duckdb
