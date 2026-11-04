#include "catch.hpp"
#include "duckdb/main/result_unit.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

unique_ptr<DataChunk> MakeChunk(idx_t rows) {
	auto chunk = make_uniq<DataChunk>();
	chunk->Initialize(Allocator::DefaultAllocator(), {LogicalType::BIGINT, LogicalType::VARCHAR},
	                  MaxValue<idx_t>(rows, 1));
	for (idx_t i = 0; i < rows; i++) {
		chunk->data[0].SetValue(i, Value::BIGINT(NumericCast<int64_t>(i)));
		chunk->data[1].SetValue(i, Value(string(i, 'x')));
	}
	chunk->SetChildCardinality(rows);
	return chunk;
}

} // namespace

TEST_CASE("A chunk unit takes its rows and bytes from the chunk it holds", "[api][result_unit]") {
	auto chunk = MakeChunk(100);
	const auto bytes = chunk->GetDataSize();
	unique_ptr<ResultUnit> unit = make_uniq<ChunkUnit>(std::move(chunk));
	REQUIRE(unit->row_count == 100);
	REQUIRE(unit->byte_size == bytes);
	REQUIRE(unit->Cast<ChunkUnit>().chunk->size() == 100);
	const ResultUnit &const_unit = *unit;
	REQUIRE(const_unit.Cast<ChunkUnit>().chunk->size() == 100);

	unique_ptr<ResultUnit> empty = make_uniq<ChunkUnit>(MakeChunk(0));
	REQUIRE(empty->row_count == 0);
	REQUIRE(empty->Cast<ChunkUnit>().chunk->size() == 0);
}
