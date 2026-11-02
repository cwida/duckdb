#include "core_functions/scalar/string_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unicode_bar.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"

namespace duckdb {

static string_t BarScalarFunction(double x, double min, double max, double max_width, string &result) {
	static const char *FULL_BLOCK = UnicodeBar::FullBlock();
	static const char *const *PARTIAL_BLOCKS = UnicodeBar::PartialBlocks();
	static const idx_t PARTIAL_BLOCKS_COUNT = UnicodeBar::PartialBlocksCount();

	if (!Value::IsFinite(max_width)) {
		throw OutOfRangeException("Max bar width must not be NaN or infinity");
	}
	if (max_width < 1) {
		throw OutOfRangeException("Max bar width must be >= 1");
	}
	if (max_width > 1000) {
		throw OutOfRangeException("Max bar width must be <= 1000");
	}

	double width;

	if (Value::IsNan(x) || Value::IsNan(min) || Value::IsNan(max) || x <= min) {
		width = 0;
	} else if (x >= max) {
		width = max_width;
	} else {
		width = max_width * (x - min) / (max - min);
	}

	if (!Value::IsFinite(width)) {
		throw OutOfRangeException("Bar width must not be NaN or infinity");
	}

	result.clear();
	idx_t used_blocks = 0;

	auto width_as_int = LossyNumericCast<uint32_t>(width * PARTIAL_BLOCKS_COUNT);
	idx_t full_blocks_count = (width_as_int / PARTIAL_BLOCKS_COUNT);
	for (idx_t i = 0; i < full_blocks_count; i++) {
		used_blocks++;
		result += FULL_BLOCK;
	}

	idx_t remaining = width_as_int % PARTIAL_BLOCKS_COUNT;

	if (remaining) {
		used_blocks++;
		result += PARTIAL_BLOCKS[remaining];
	}

	const idx_t integer_max_width = (idx_t)max_width;
	if (used_blocks < integer_max_width) {
		result += std::string(integer_max_width - used_blocks, ' ');
	}
	return string_t(result);
}

static void BarFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 3 || args.ColumnCount() == 4);
	const auto &x_arg = args.data[0];
	const auto &min_arg = args.data[1];
	const auto &max_arg = args.data[2];
	string buffer;

	auto &heap = StringVector::GetStringHeap(result);
	if (args.ColumnCount() == 3) {
		GenericExecutor::ExecuteTernary<PrimitiveType<double>, PrimitiveType<double>, PrimitiveType<double>,
		                                PrimitiveType<string_t>>(
		    x_arg, min_arg, max_arg, result,
		    [&](PrimitiveType<double> x, PrimitiveType<double> min, PrimitiveType<double> max) {
			    return heap.AddString(BarScalarFunction(x.val, min.val, max.val, 80, buffer));
		    });
	} else {
		const auto &width_arg = args.data[3];
		GenericExecutor::ExecuteQuaternary<PrimitiveType<double>, PrimitiveType<double>, PrimitiveType<double>,
		                                   PrimitiveType<double>, PrimitiveType<string_t>>(
		    x_arg, min_arg, max_arg, width_arg, result,
		    [&](PrimitiveType<double> x, PrimitiveType<double> min, PrimitiveType<double> max,
		        PrimitiveType<double> width) {
			    return heap.AddString(BarScalarFunction(x.val, min.val, max.val, width.val, buffer));
		    });
	}
}

ScalarFunctionSet BarFun::GetFunctions() {
	ScalarFunctionSet bar;
	ScalarFunction bar_width({}, LogicalType::VARCHAR, BarFunction);
	bar_width.GetSignature()
	    .AddParameter("x", LogicalType::DOUBLE)
	    .AddParameter("min", LogicalType::DOUBLE)
	    .AddParameter("max", LogicalType::DOUBLE)
	    .AddParameter("width", LogicalType::DOUBLE);
	bar.AddFunction(bar_width);

	ScalarFunction bar_default_width({}, LogicalType::VARCHAR, BarFunction);
	bar_default_width.GetSignature()
	    .AddParameter("x", LogicalType::DOUBLE)
	    .AddParameter("min", LogicalType::DOUBLE)
	    .AddParameter("max", LogicalType::DOUBLE);
	bar.AddFunction(bar_default_width);
	// throws if the bar width is out of range
	bar.SetFallible();
	return bar;
}

} // namespace duckdb
