#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/hyperloglog.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"

namespace duckdb {

TEST_CASE("Aggregate HLL observes every distinct hash across lookup hits and reuse", "[aggregate_hll]") {
	DuckDB db(nullptr);
	Connection con(db);
	vector<LogicalType> types {LogicalType::BIGINT, LogicalType::VARCHAR};
	GroupedAggregateHashTable ht(*con.context, Allocator::DefaultAllocator(), types);
	const auto &adaptivity = ht.GetAdaptivityState();
	ht.Resize(16384);
	ht.EnableHLL(true);
	HyperLogLogP<8> expected;
	DataChunk groups;
	groups.Initialize(Allocator::DefaultAllocator(), types);
	Vector hashes(LogicalType::HASH);
	Vector addresses(LogicalType::POINTER);
	SelectionVector new_groups(STANDARD_VECTOR_SIZE);
	auto check = [&]() {
		groups.Hash(hashes);
		expected.Update(hashes);
		ht.FindOrCreateGroups(groups, hashes, addresses, new_groups);
		const auto upper_bound = LossyNumericCast<idx_t>((1 + HyperLogLogP<8>::GetErrorRate()) * expected.Count());
		REQUIRE(adaptivity.GetHLLUpperBound() == upper_bound);
	};

	for (idx_t phase = 0; phase < 3; phase++) {
		if (phase == 1) {
			ht.Abandon();
			ht.Resize(32768);
		} else if (phase == 2) {
			ht.ResetForNewIteration(0);
			ht.EnableHLL(true);
			expected = HyperLogLogP<8>();
		}
		groups.Reset();
		{
			auto keys = FlatVector::Writer<int64_t>(groups.data[0], STANDARD_VECTOR_SIZE);
			for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
				keys.WriteValue(-NumericCast<int64_t>(i) - 1);
			}
		}
		groups.data[1].Reference(Value("unique_group"), count_t(STANDARD_VECTOR_SIZE));
		groups.CheckCardinality(STANDARD_VECTOR_SIZE);
		check();
		for (idx_t batch = 0; batch < 8; batch++) {
			groups.Reset();
			{
				auto keys = FlatVector::Writer<int64_t>(groups.data[0], STANDARD_VECTOR_SIZE);
				auto names = FlatVector::Writer<string_t>(groups.data[1], STANDARD_VECTOR_SIZE);
				for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
					const auto key = (i / 2 + batch * 127) % 2048;
					if (key % 17 == 0) {
						keys.WriteNull();
					} else {
						keys.WriteValue(NumericCast<int64_t>(key));
					}
					if (key % 23 == 0) {
						names.WriteNull();
					} else {
						const auto name = "group_with_retained_string_" + std::to_string(key);
						names.WriteValue(string_t(name));
					}
				}
			}
			groups.CheckCardinality(STANDARD_VECTOR_SIZE);
			check();
			SelectionVector duplicates(STANDARD_VECTOR_SIZE);
			for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
				duplicates.set_index(i, i % 127);
			}
			groups.Slice(duplicates, STANDARD_VECTOR_SIZE);
			check();
		}
		groups.data[0].Reference(Value::BIGINT(42), count_t(STANDARD_VECTOR_SIZE));
		groups.data[1].Reference(Value("constant_group"), count_t(STANDARD_VECTOR_SIZE));
		groups.CheckCardinality(STANDARD_VECTOR_SIZE);
		check();
		check();
	}

	ht.SkipLookups();
	groups.data[0].Reference(Value::BIGINT(100000), count_t(STANDARD_VECTOR_SIZE));
	check();
}

TEST_CASE("Aggregate lookup recovery resets dictionary caches and preserves observation across reuse",
          "[aggregate_hll]") {
	DuckDB db(nullptr);
	Connection con(db);
	vector<LogicalType> types {LogicalType::BIGINT};
	GroupedAggregateHashTable ht(*con.context, Allocator::DefaultAllocator(), types);
	const auto &adaptivity = ht.GetAdaptivityState();
	ht.Resize(262144);
	const auto distinct = MinValue<idx_t>(7, STANDARD_VECTOR_SIZE);
	auto dictionary = DictionaryVector::CreateReusableDictionary(types[0], distinct);
	{
		auto writer = FlatVector::Writer<int64_t>(dictionary->data, distinct);
		for (idx_t i = 0; i < distinct; i++) {
			writer.WriteValue(NumericCast<int64_t>(i));
		}
	}
	SelectionVector selection(STANDARD_VECTOR_SIZE);
	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
		selection.set_index(i, i % distinct);
	}
	DataChunk groups;
	groups.Initialize(Allocator::DefaultAllocator(), types);
	groups.data[0].Dictionary(dictionary, selection, STANDARD_VECTOR_SIZE);
	groups.CheckCardinality(STANDARD_VECTOR_SIZE);
	DataChunk payload;
	unsafe_vector<idx_t> filter;
	ht.AddChunk(groups, payload, filter);
	ht.SkipLookups();
	ht.ResetForNewIteration(0);
	REQUIRE_FALSE(adaptivity.LookupsSkipped());
	REQUIRE(adaptivity.GetCycleInputCount() == 0);
	REQUIRE(adaptivity.GetSkippedInputCount() == 0);
	REQUIRE(ht.Capacity() < 262144);
	REQUIRE(ht.GetSizeInBytes() >= 262144 * sizeof(ht_entry_t));

	ht.EnableHLL(true);
	ht.AddChunk(groups, payload, filter);
	const auto estimate = adaptivity.GetHLLUpperBound();
	ht.SkipLookups();
	ht.Abandon();
	ht.AddChunk(groups, payload, filter);
	ht.AddChunk(groups, payload, filter);
	REQUIRE(adaptivity.GetSkippedInputCount() == 2 * STANDARD_VECTOR_SIZE);
	REQUIRE(adaptivity.GetCycleInputCount() == 2 * STANDARD_VECTOR_SIZE);
	REQUIRE(ht.Count() == distinct);
	REQUIRE(ht.GetMaterializedCount() == 2 * distinct);

	ht.Abandon();
	REQUIRE(adaptivity.GetSkippedInputCount() == 2 * STANDARD_VECTOR_SIZE);
	REQUIRE(adaptivity.GetCycleInputCount() == 0);
	const auto retained_bytes = ht.GetSizeInBytes();
	ht.ResumeLookups();
	REQUIRE(ht.GetSizeInBytes() == retained_bytes);
	REQUIRE(adaptivity.GetSkippedInputCount() == 0);
	ht.AddChunk(groups, payload, filter);
	ht.AddChunk(groups, payload, filter);
	REQUIRE(ht.Count() == distinct);
	REQUIRE(ht.GetMaterializedCount() == 3 * distinct);
	REQUIRE(adaptivity.GetHLLUpperBound() == estimate);

	GroupedAggregateHashTable combined(*con.context, Allocator::DefaultAllocator(), types);
	ht.Abandon();
	auto data = ht.AcquirePartitionedData();
	for (auto &partition : data->GetPartitions()) {
		combined.Combine(*partition);
	}
	REQUIRE(combined.Count() == distinct);
}

} // namespace duckdb
