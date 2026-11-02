#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/main/retained_result_collection.hpp"

using namespace duckdb;

#ifndef DUCKDB_NO_THREADS

#include "test_result_format.hpp"

namespace {

//! BIGINT values start..start+count-1, VARCHAR values long enough to never be inlined
unique_ptr<DataChunk> MakeChunk(idx_t start, idx_t count) {
	auto chunk = make_uniq<DataChunk>();
	chunk->Initialize(Allocator::DefaultAllocator(), {LogicalType::BIGINT, LogicalType::VARCHAR},
	                  MaxValue<idx_t>(count, 1));
	for (idx_t i = 0; i < count; i++) {
		auto value = start + i;
		chunk->data[0].SetValue(i, Value::BIGINT(NumericCast<int64_t>(value)));
		chunk->data[1].SetValue(i, Value(StringUtil::Format("payload-%llu-not-inlined", value)));
	}
	chunk->SetChildCardinality(count);
	return chunk;
}

vector<int64_t> PayloadValues(const TestPayload &payload) {
	vector<int64_t> result;
	for (auto &value : UnitValues(payload, 0)) {
		result.push_back(value.GetValue<int64_t>());
	}
	return result;
}

vector<int64_t> Ascending(idx_t start, idx_t count) {
	vector<int64_t> result;
	for (idx_t i = 0; i < count; i++) {
		result.push_back(NumericCast<int64_t>(start + i));
	}
	return result;
}

ResultFormatContext MakeFormatContext(ClientProperties client_properties, ResultOrdering ordering) {
	return ResultFormatContext {{LogicalType::BIGINT, LogicalType::VARCHAR},
	                            {Identifier("a"), Identifier("b")},
	                            std::move(client_properties),
	                            ordering};
}

vector<int64_t> DrainBigints(ChunkRetainedCollection &collection) {
	vector<int64_t> result;
	while (auto chunk = collection.FetchRaw()) {
		for (idx_t i = 0; i < chunk->size(); i++) {
			result.push_back(chunk->GetValue(0, i).GetValue<int64_t>());
		}
	}
	return result;
}

} // namespace

TEST_CASE("Combine flushes the local's partial unit and keeps each producer's order", "[api][retained_collection]") {
	DuckDB db(nullptr);
	Connection con(db);
	TestFormat format(1000000);
	auto format_context = MakeFormatContext(con.context->GetClientProperties(), ResultOrdering::UNORDERED);
	auto gstate = format.InitGlobal(format_context);

	DefaultRetainedCollection<TestFormat> local_a(format, *gstate);
	for (idx_t offset = 0; offset < 1000; offset += 250) {
		auto chunk = MakeChunk(offset, 250);
		local_a.Append(*chunk, 0);
	}
	DefaultRetainedCollection<TestFormat> local_b(format, *gstate);
	for (idx_t offset = 1000; offset < 2000; offset += 250) {
		auto chunk = MakeChunk(offset, 250);
		local_b.Append(*chunk, 0);
	}
	// Neither local ever reached the cap, so each still holds its rows as one partial unit
	REQUIRE(local_a.Count() == 0);
	REQUIRE(local_b.Count() == 0);

	DefaultRetainedCollection<TestFormat> global(format, *gstate);
	global.Combine(local_b);
	global.Combine(local_a);
	global.Finalize();

	REQUIRE(global.Count() == 2000);
	// Only the producers built a local state: the global instance never receives a chunk
	REQUIRE(gstate->Cast<TestFormatGlobalState>().local_states == 2);
	vector<int64_t> a_values;
	vector<int64_t> b_values;
	for (auto &payload : global.Get()) {
		auto values = PayloadValues(*payload);
		for (auto value : values) {
			if (value < 1000) {
				a_values.push_back(value);
			} else {
				b_values.push_back(value);
			}
		}
	}
	REQUIRE(a_values == Ascending(0, 1000));
	REQUIRE(b_values == Ascending(1000, 1000));
}

TEST_CASE("A batch index change flushes the partial unit under the old batch", "[api][retained_collection]") {
	DuckDB db(nullptr);
	Connection con(db);
	TestFormat format(1000000);
	auto format_context = MakeFormatContext(con.context->GetClientProperties(), ResultOrdering::BATCH_INDEX_ORDERED);
	auto gstate = format.InitGlobal(format_context);

	DefaultRetainedCollection<TestFormat> local(format, *gstate);
	for (auto batch : {0, 1, 2}) {
		auto base = NumericCast<idx_t>(batch) * 1000;
		auto chunk1 = MakeChunk(base, 100);
		local.Append(*chunk1, NumericCast<idx_t>(batch));
		auto chunk2 = MakeChunk(base + 100, 100);
		local.Append(*chunk2, NumericCast<idx_t>(batch));
	}

	DefaultRetainedCollection<TestFormat> global(format, *gstate);
	global.Combine(local);
	global.Finalize();

	REQUIRE(global.Count() == 600);
	auto &payloads = global.Get();
	REQUIRE(payloads.size() == 3);
	for (idx_t batch = 0; batch < 3; batch++) {
		auto base = batch * 1000;
		REQUIRE(payloads[batch]->row_count == 200);
		REQUIRE(PayloadValues(*payloads[batch]) == Ascending(base, 200));
	}
}

TEST_CASE("Finalize sorts stably: batches ascend, production order survives within a batch",
          "[api][retained_collection]") {
	DuckDB db(nullptr);
	Connection con(db);
	TestFormat format(1000000);
	auto format_context = MakeFormatContext(con.context->GetClientProperties(), ResultOrdering::BATCH_INDEX_ORDERED);
	auto gstate = format.InitGlobal(format_context);

	DefaultRetainedCollection<TestFormat> local_a(format, *gstate);
	{
		auto v1 = MakeChunk(100, 5);
		local_a.Append(*v1, 0);
		auto v2 = MakeChunk(200, 5);
		local_a.Append(*v2, 2); // flushes v1 under batch 0
		auto v3 = MakeChunk(105, 5);
		local_a.Append(*v3, 0); // flushes v2 under batch 2, v3 stays partial under batch 0
	}
	DefaultRetainedCollection<TestFormat> local_b(format, *gstate);
	{
		auto w1 = MakeChunk(300, 5);
		local_b.Append(*w1, 1); // stays partial under batch 1
	}

	DefaultRetainedCollection<TestFormat> global(format, *gstate);
	// Combined out of batch order: b (batch 1) before a's batch 0/2 entries
	global.Combine(local_b);
	global.Combine(local_a);
	global.Finalize();

	auto &payloads = global.Get();
	REQUIRE(payloads.size() == 4);
	REQUIRE(PayloadValues(*payloads[0]) == Ascending(100, 5)); // batch 0, v1 (first produced)
	REQUIRE(PayloadValues(*payloads[1]) == Ascending(105, 5)); // batch 0, v3 (produced after v1)
	REQUIRE(PayloadValues(*payloads[2]) == Ascending(300, 5)); // batch 1, w1
	REQUIRE(PayloadValues(*payloads[3]) == Ascending(200, 5)); // batch 2, v2
}

TEST_CASE("Count includes an empty local combined in", "[api][retained_collection]") {
	DuckDB db(nullptr);
	Connection con(db);
	TestFormat format(1000000);
	auto format_context = MakeFormatContext(con.context->GetClientProperties(), ResultOrdering::UNORDERED);
	auto gstate = format.InitGlobal(format_context);

	DefaultRetainedCollection<TestFormat> local(format, *gstate);
	auto chunk = MakeChunk(0, 50);
	local.Append(*chunk, 0);
	// This local never receives a single append, mirroring a producer whose partition held no rows
	DefaultRetainedCollection<TestFormat> empty_local(format, *gstate);

	DefaultRetainedCollection<TestFormat> global(format, *gstate);
	global.Combine(local);
	global.Combine(empty_local);
	global.Finalize();

	REQUIRE(global.Count() == 50);
	REQUIRE(global.Get().size() == 1);
	REQUIRE(PayloadValues(*global.Get()[0]) == Ascending(0, 50));
}

TEST_CASE("Fetch copies payloads that outlive the collection; Get is unaffected by Fetch",
          "[api][retained_collection]") {
	DuckDB db(nullptr);
	Connection con(db);
	TestFormat format(1000000);
	auto format_context = MakeFormatContext(con.context->GetClientProperties(), ResultOrdering::BATCH_INDEX_ORDERED);
	auto gstate = format.InitGlobal(format_context);

	DefaultRetainedCollection<TestFormat> local(format, *gstate);
	for (auto batch : {0, 1, 2}) {
		auto chunk = MakeChunk(NumericCast<idx_t>(batch) * 100, 10);
		local.Append(*chunk, NumericCast<idx_t>(batch));
	}

	auto global = make_uniq<DefaultRetainedCollection<TestFormat>>(format, *gstate);
	global->Combine(local);
	global->Finalize();
	REQUIRE(global->Get().size() == 3);

	auto fetched_0 = global->Fetch();
	auto fetched_1 = global->Fetch();
	auto fetched_2 = global->Fetch();
	REQUIRE(global->Fetch() == nullptr);
	REQUIRE(global->Fetch() == nullptr); // forever after, not just once

	REQUIRE(PayloadValues(*fetched_0) == Ascending(0, 10));
	REQUIRE(PayloadValues(*fetched_1) == Ascending(100, 10));
	REQUIRE(PayloadValues(*fetched_2) == Ascending(200, 10));
	// Fetch copies: the stored payloads are untouched
	REQUIRE(global->Get().size() == 3);
	REQUIRE(PayloadValues(*global->Get()[0]) == Ascending(0, 10));

	global.reset();
	// The copy has no dependency on the collection's storage, including its string heap
	REQUIRE(fetched_1->chunks[0]->GetValue(1, 0).ToString() == "payload-100-not-inlined");
}

TEST_CASE("ChunkRetainedCollection materializes rows for every memory type and ordering",
          "[api][retained_collection]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	vector<LogicalType> types {LogicalType::BIGINT, LogicalType::VARCHAR};

	bool batch_ordered = false;
	QueryResultMemoryType memory_type = QueryResultMemoryType::IN_MEMORY;
	SECTION("unordered, in memory") {
		batch_ordered = false;
		memory_type = QueryResultMemoryType::IN_MEMORY;
	}
	SECTION("unordered, buffer managed") {
		batch_ordered = false;
		memory_type = QueryResultMemoryType::BUFFER_MANAGED;
	}
	SECTION("batch ordered, in memory") {
		batch_ordered = true;
		memory_type = QueryResultMemoryType::IN_MEMORY;
	}
	SECTION("batch ordered, buffer managed") {
		batch_ordered = true;
		memory_type = QueryResultMemoryType::BUFFER_MANAGED;
	}

	ChunkRetainedCollection local1(context, types, memory_type, batch_ordered);
	ChunkRetainedCollection local2(context, types, memory_type, batch_ordered);
	ChunkRetainedCollection global(context, types, memory_type, batch_ordered);

	vector<int64_t> expected;
	if (batch_ordered) {
		// Batch indexes out of order across the two locals, and out of order between the locals
		auto c1 = MakeChunk(500, 5);
		local1.Append(*c1, 5);
		auto c2 = MakeChunk(200, 2);
		local1.Append(*c2, 2);
		auto c3 = MakeChunk(800, 3);
		local2.Append(*c3, 8);
		auto c4 = MakeChunk(100, 4);
		local2.Append(*c4, 1);
		expected = Ascending(100, 4);
		auto batch2 = Ascending(200, 2);
		auto batch5 = Ascending(500, 5);
		auto batch8 = Ascending(800, 3);
		expected.insert(expected.end(), batch2.begin(), batch2.end());
		expected.insert(expected.end(), batch5.begin(), batch5.end());
		expected.insert(expected.end(), batch8.begin(), batch8.end());
	} else {
		auto c1 = MakeChunk(10, 5);
		local1.Append(*c1, 0);
		auto c2 = MakeChunk(20, 6);
		local2.Append(*c2, 0);
		expected = Ascending(10, 5);
		auto rest = Ascending(20, 6);
		expected.insert(expected.end(), rest.begin(), rest.end());
	}

	global.Combine(local1);
	global.Combine(local2);
	global.Finalize();

	REQUIRE(global.Get().Count() == expected.size());
	auto rows = DrainBigints(global);
	REQUIRE(global.FetchRaw() == nullptr); // forever after
	if (batch_ordered) {
		REQUIRE(rows == expected);
	} else {
		std::sort(rows.begin(), rows.end());
		REQUIRE(rows == expected);
	}

	auto taken = global.Take();
	REQUIRE(taken);
	REQUIRE(taken->Count() == expected.size());
}

#endif
