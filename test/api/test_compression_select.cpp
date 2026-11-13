#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/scan_state.hpp"

using namespace duckdb;

static unique_ptr<ColumnSegment> MakeRLETestSegment(DuckDB &db, const CompressionFunction &function,
                                                    const vector<uint16_t> &counts, idx_t row_count) {
	auto &buffers = BufferManager::GetBufferManager(*db.instance);
	auto &blocks = buffers.GetTemporaryBlockManager();
	auto segment = ColumnSegment::CreateTransientSegment(*db.instance, function, LogicalType::INTEGER,
	                                                     blocks.GetBlockSize(), blocks);
	const auto count_offset = sizeof(uint64_t) + counts.size() * sizeof(int32_t);
	const auto byte_size = count_offset + counts.size() * sizeof(uint16_t);
	segment->count = row_count;
	// Bound the reader to the encoded bytes, excluding allocator padding.
	segment->SetByteSize(NumericCast<uint32_t>(byte_size));
	auto handle = buffers.Pin(segment->GetBlockHandle());
	auto data = handle.GetDataMutable();
	Store<uint64_t>(count_offset, data);
	for (idx_t i = 0; i < counts.size(); i++) {
		Store<int32_t>(NumericCast<int32_t>(i + 1), data + sizeof(uint64_t) + i * sizeof(int32_t));
		Store<uint16_t>(counts[i], data + count_offset + i * sizeof(uint16_t));
	}
	return segment;
}

TEST_CASE("RLE validation covers batch boundaries before decoding", "[storage][compression][rle]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto function = RLEFun::GetFunction(PhysicalType::INT32);
	const auto entries = GENERATE(31, 32, 33, 64, 65);
	const auto zeros = GENERATE(false, true);
	vector<uint16_t> counts(entries, 1);
	if (zeros) {
		counts[0] = 0;
		counts[entries - 1] = 0;
		if (entries > 32) {
			counts[31] = counts[32] = 0;
		}
	}
	vector<int32_t> expected;
	for (idx_t i = 0; i < counts.size(); i++) {
		if (counts[i]) {
			expected.push_back(NumericCast<int32_t>(i + 1));
		}
	}
	// Bytes after the last logical row are not another run to validate.
	counts.push_back(0);
	counts.push_back(65535);
	const auto row_count = expected.size();
	auto segment = MakeRLETestSegment(db, function, counts, row_count);
	ColumnScanState state(nullptr);
	segment->InitializeScan(state);
	Vector result(LogicalType::INTEGER, row_count + 3);
	auto check = [&](idx_t offset, idx_t count, idx_t expected_offset) {
		auto values = result.Values<int32_t>();
		for (idx_t i = 0; i < count; i++) {
			REQUIRE(values[offset + i].GetValue() == expected[expected_offset + i]);
		}
	};

	SECTION("scan") {
		segment->Scan(state, row_count, result, 0, ScanVectorType::SCAN_ENTIRE_VECTOR);
		check(0, row_count, 0);
	}
	SECTION("partial scans reuse the validation frontier") {
		const auto first = MinValue<idx_t>(31, row_count - 1);
		segment->Scan(state, first, result, 0, ScanVectorType::SCAN_FLAT_VECTOR);
		state.offset_in_column = first;
		segment->Scan(state, row_count - first, result, first, ScanVectorType::SCAN_FLAT_VECTOR);
		check(0, row_count, 0);
	}
	SECTION("selection advances over the unselected tail") {
		SelectionVector selection(1);
		selection.set_index(0, 0);
		segment->Select(state, row_count - 1, result, selection, 1, 0, ScanVectorType::SCAN_FLAT_VECTOR);
		check(0, 1, 0);
		state.offset_in_column = row_count - 1;
		segment->Scan(state, 1, result, 1, ScanVectorType::SCAN_FLAT_VECTOR);
		check(1, 1, row_count - 1);
	}
	SECTION("skip then scan") {
		state.offset_in_column = row_count - 1;
		segment->Skip(state);
		segment->Scan(state, 1, result, 0, ScanVectorType::SCAN_ENTIRE_VECTOR);
		check(0, 1, row_count - 1);
	}
	SECTION("filter validates the complete dictionary") {
		auto filter = ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHAN, Value::INTEGER(0));
		auto filter_state = TableFilterState::Initialize(*con.context, *filter);
		FlatVector::SetSize(result, count_t(row_count));
		SelectionVector selection;
		idx_t selected_count = row_count;
		segment->Filter(state, row_count, result, selection, selected_count, *filter, *filter_state);
		REQUIRE(selected_count == row_count);
		check(0, row_count, 0);
	}
}

TEST_CASE("Pure RLE filtering advances past a partially selected batch", "[storage][compression][rle]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto function = RLEFun::GetFunction(PhysicalType::INT32);
	vector<uint16_t> counts {0, 1, 9, 0, 1, 9, 0};
	auto segment = MakeRLETestSegment(db, function, counts, 20);
	ColumnScanState state(nullptr);
	segment->InitializeScan(state);
	auto filter =
	    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value::INTEGER(3));
	auto filter_state = TableFilterState::Initialize(*con.context, *filter);
	REQUIRE(filter_state->can_cache);
	REQUIRE(filter_state->Cast<ExpressionFilterState>().can_cache_result);
	const idx_t prefix = GENERATE(0, 1, 5, 10);
	const auto incoming_selection = GENERATE(false, true);
	Vector result(LogicalType::INTEGER, 10);
	SelectionVector selection;
	if (incoming_selection) {
		selection.Initialize(10);
		for (idx_t row = 0; row < prefix; row++) {
			selection.set_index(row, row);
		}
	}
	idx_t selected_count = prefix;
	segment->Filter(state, 10, result, selection, selected_count, *filter, *filter_state);
	REQUIRE(selected_count == (prefix == 0 ? 0 : prefix - 1));
	{
		auto values = result.Values<int32_t>();
		for (idx_t row = 0; row < selected_count; row++) {
			REQUIRE(selection.get_index(row) == row + 1);
			REQUIRE(values[selection.get_index(row)].GetValue() == 3);
		}
	}
	state.offset_in_column = 10;
	segment->Scan(state, 10, result, 0, ScanVectorType::SCAN_FLAT_VECTOR);
	auto values = result.Values<int32_t>();
	for (idx_t row = 0; row < 10; row++) {
		REQUIRE(values[row].GetValue() == (row == 0 ? 5 : 6));
	}
}

TEST_CASE("Malformed RLE batches fail before unchecked run access", "[storage][compression][rle]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto function = RLEFun::GetFunction(PhysicalType::INT32);
	const auto entries = GENERATE(31, 32, 33, 64, 65);
	const auto corruption = GENERATE(0, 1, 2);
	vector<uint16_t> counts(entries, 1);
	idx_t row_count = entries + 1;
	if (corruption == 1) {
		// A nonnegative batch cannot hide a run exceeding the logical remainder.
		counts[MinValue<idx_t>(31, entries - 1)] = 65535;
	} else if (corruption == 2) {
		std::fill(counts.begin(), counts.end(), 0);
	}
	auto segment = MakeRLETestSegment(db, function, counts, row_count);
	ColumnScanState state(nullptr);
	segment->InitializeScan(state);
	Vector result(LogicalType::INTEGER, row_count);

	SECTION("scan") {
		REQUIRE_THROWS_AS(segment->Scan(state, row_count, result, 0, ScanVectorType::SCAN_ENTIRE_VECTOR),
		                  DataCorruptionException);
	}
	SECTION("partial scan") {
		REQUIRE_THROWS_AS(segment->Scan(state, row_count, result, 0, ScanVectorType::SCAN_FLAT_VECTOR),
		                  DataCorruptionException);
	}
	SECTION("selection validates its unselected tail") {
		SelectionVector selection(1);
		selection.set_index(0, 0);
		REQUIRE_THROWS_AS(segment->Select(state, row_count, result, selection, 1, 0, ScanVectorType::SCAN_FLAT_VECTOR),
		                  DataCorruptionException);
	}
	SECTION("skip") {
		state.offset_in_column = row_count;
		REQUIRE_THROWS_AS(segment->Skip(state), DataCorruptionException);
	}
	SECTION("filter validates beyond the requested vector") {
		auto filter = ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHAN, Value::INTEGER(0));
		auto filter_state = TableFilterState::Initialize(*con.context, *filter);
		SelectionVector selection;
		idx_t selected_count = 1;
		REQUIRE_THROWS_AS(segment->Filter(state, 1, result, selection, selected_count, *filter, *filter_state),
		                  DataCorruptionException);
	}
}

TEST_CASE("Constant selection preserves the surrounding flat result", "[storage][compression]") {
	DuckDB db(nullptr);
	Vector result(LogicalType::INTEGER, 8);
	{
		auto writer = FlatVector::Writer<int32_t>(result, 8);
		for (int32_t i = 0; i < 8; i++) {
			writer.WriteValue(i);
		}
	}
	SelectionVector selection(2);
	selection.set_index(0, 1);
	selection.set_index(1, 6);
	ColumnScanState state(nullptr);

	SECTION("values") {
		auto function = ConstantFun::GetFunction(PhysicalType::INT32);
		auto stats = NumericStats::CreateEmpty(LogicalType::INTEGER);
		NumericStats::SetMin(stats, Value::INTEGER(42));
		NumericStats::SetMax(stats, Value::INTEGER(42));
		stats.SetHasNoNull();
		ColumnSegment segment(*db.instance, nullptr, ColumnSegmentType::PERSISTENT, 8, function, std::move(stats),
		                      INVALID_BLOCK, 0, 0);
		segment.Select(state, 8, result, selection, 2, 3, ScanVectorType::SCAN_FLAT_VECTOR);
		REQUIRE(result.GetVectorType() == VectorType::FLAT_VECTOR);
		auto values = result.Values<int32_t>();
		REQUIRE(values.size() == 8);
		for (idx_t i = 0; i < values.size(); i++) {
			REQUIRE(values[i].IsValid());
			REQUIRE(values[i].GetValue() == (i == 3 || i == 4 ? 42 : int32_t(i)));
		}
	}

	SECTION("validity") {
		auto function = ConstantFun::GetFunction(PhysicalType::BIT);
		auto stats = BaseStatistics::CreateEmpty(LogicalTypeId::VALIDITY);
		stats.SetHasNull();
		ColumnSegment segment(*db.instance, nullptr, ColumnSegmentType::PERSISTENT, 8, function, std::move(stats),
		                      INVALID_BLOCK, 0, 0);
		segment.Select(state, 8, result, selection, 2, 3, ScanVectorType::SCAN_FLAT_VECTOR);
		REQUIRE(result.GetVectorType() == VectorType::FLAT_VECTOR);
		auto values = result.Values<int32_t>();
		REQUIRE(values.size() == 8);
		for (idx_t i = 0; i < values.size(); i++) {
			REQUIRE(values[i].IsValid() == (i != 3 && i != 4));
			if (values[i].IsValid()) {
				REQUIRE(values[i].GetValue() == int32_t(i));
			}
		}
	}
}
