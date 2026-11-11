#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

#include <atomic>
#include <thread>

using namespace duckdb;

TEST_CASE("Bloom sector indexing preserves concurrent inserts, folding and mapped probes", "[filter][bloom_filter]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;
	static constexpr idx_t LARGE_BUILD_COUNT = 1ULL << 19;
	static constexpr idx_t WORKER_COUNT = 4;
	static constexpr idx_t PROBE_COUNT = 2048;
	BloomFilter filter;

	for (const auto count : {idx_t(513), LARGE_BUILD_COUNT}) {
		CAPTURE(count);
		filter.Initialize(context, LARGE_BUILD_COUNT);
		vector<unique_ptr<Vector>> inputs;
		for (idx_t worker = 0; worker < WORKER_COUNT; worker++) {
			const auto begin = count * worker / WORKER_COUNT;
			const auto end = count * (worker + 1) / WORKER_COUNT;
			auto input = make_uniq<Vector>(LogicalType::HASH, end - begin);
			{
				auto writer = FlatVector::Writer<hash_t>(*input, count_t(end - begin));
				for (idx_t row = begin; row < end; row++) {
					writer.WriteValue(Hash(uint64_t(row * 2)));
				}
			}
			inputs.push_back(std::move(input));
		}
		std::atomic<idx_t> ready {0};
		std::atomic<bool> start {false};
		vector<std::thread> workers;
		for (idx_t worker = 0; worker < WORKER_COUNT; worker++) {
			workers.emplace_back([&, worker] {
				ready.fetch_add(1, std::memory_order_relaxed);
				while (!start.load(std::memory_order_acquire)) {
					std::this_thread::yield();
				}
				filter.InsertHashes(*inputs[worker]);
			});
		}
		while (ready.load(std::memory_order_relaxed) != WORKER_COUNT) {
			std::this_thread::yield();
		}
		start.store(true, std::memory_order_release);
		for (auto &worker : workers) {
			worker.join();
		}
		Vector boundary_hashes(LogicalType::HASH, 2);
		{
			auto writer = FlatVector::Writer<hash_t>(boundary_hashes, count_t(2));
			writer.WriteValue(0);
			writer.WriteValue(~uint64_t(0));
		}
		filter.InsertHashes(boundary_hashes);
		// The smaller build folds; the larger one keeps enough sectors to prefetch.
		filter.Seal(context);
		REQUIRE(filter.IsSealed());
		REQUIRE(filter.IsUseful());
		for (const auto &input : inputs) {
			for (const auto &hash : input->Values<hash_t>()) {
				REQUIRE(filter.LookupOne(hash.GetValue()));
			}
		}
		REQUIRE(filter.LookupOne(0));
		REQUIRE(filter.LookupOne(~uint64_t(0)));

		Vector probes(LogicalType::HASH, PROBE_COUNT);
		{
			auto writer = FlatVector::Writer<hash_t>(probes, count_t(PROBE_COUNT));
			for (idx_t row = 0; row < PROBE_COUNT; row++) {
				writer.WriteValue(Hash(uint64_t((row % count) * 2 + row % 2)));
			}
		}
		auto probe_hashes = probes.Values<hash_t>();
		SelectionVector input_sel(PROBE_COUNT), result_sel(PROBE_COUNT);
		for (idx_t row = 0; row < PROBE_COUNT; row++) {
			input_sel.set_index(row, (row * 73 + 19) % PROBE_COUNT);
		}
		for (bool mapped : {false, true}) {
			CAPTURE(mapped);
			vector<idx_t> expected;
			for (idx_t row = 0; row < PROBE_COUNT; row++) {
				const auto index = mapped ? input_sel.get_index(row) : row;
				if (filter.LookupOne(probe_hashes[index].GetValue())) {
					expected.push_back(row);
				}
			}
			REQUIRE(expected.size() >= PROBE_COUNT / 2);
			REQUIRE(expected.size() < PROBE_COUNT);
			const auto found = mapped ? filter.LookupHashes(probes, input_sel, result_sel, PROBE_COUNT)
			                          : filter.LookupHashes(probes, result_sel, PROBE_COUNT);
			REQUIRE(found == expected.size());
			for (idx_t row = 0; row < found; row++) {
				REQUIRE(result_sel.get_index(row) == expected[row]);
			}
		}
	}
}

TEST_CASE("Bloom filter selection preserves NULL policy and mapped rows", "[filter][bloom_filter]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;

	Vector build_keys(LogicalType::BIGINT, 2);
	build_keys.SetValue(0, Value(LogicalType::BIGINT));
	build_keys.SetValue(1, Value::BIGINT(11));
	FlatVector::SetSize(build_keys, count_t(2));
	Vector hashes(LogicalType::HASH, 2);
	VectorOperations::Hash(build_keys, hashes, 2);
	const vector<Value> values = {Value::BIGINT(11), Value(LogicalType::BIGINT), Value::BIGINT(22),
	                              Value::BIGINT(11), Value(LogicalType::BIGINT), Value::BIGINT(22),
	                              Value::BIGINT(11), Value::BIGINT(22)};
	const auto count = values.size();
	Vector keys(LogicalType::BIGINT, count);
	for (idx_t i = 0; i < count; i++) {
		keys.SetValue(i, values[i]);
	}
	FlatVector::SetSize(keys, count_t(count));
	SelectionVector dictionary_sel(count);
	SelectionVector input_sel(count);
	for (idx_t i = 0; i < count; i++) {
		dictionary_sel.set_index(i, (3 * i + 1) % count);
		input_sel.set_index(i, count - i - 1);
	}

	for (bool contains_null_hash : {false, true}) {
		auto filter = make_shared_ptr<BloomFilter>();
		filter->Initialize(context, 2);
		SelectionVector build_sel(2);
		build_sel.set_index(0, 1);
		build_sel.set_index(1, 0);
		// With the NULL hash present, only the explicit NULL policy can reject it.
		filter->InsertHashes(hashes, build_sel, contains_null_hash ? 2 : 1);
		filter->Seal(context);
		REQUIRE(filter->LookupOne(hashes.GetValue(0).GetValue<hash_t>()) == contains_null_hash);
		REQUIRE_FALSE(filter->LookupOne(Hash(int64_t(22))));

		for (bool filters_null_values : {false, true}) {
			vector<unique_ptr<Expression>> children;
			children.push_back(make_uniq<BoundReferenceExpression>(LogicalType::BIGINT, 0ULL));
			BoundFunctionExpression expr(
			    BoundScalarFunction(BloomFilterScalarFun::GetFunction(LogicalType::BIGINT)), std::move(children),
			    make_uniq<BloomFilterFunctionData>(filter, filters_null_values, "k", LogicalType::BIGINT, 0.0f, 0ULL));
			ExpressionExecutor executor(context, expr);

			for (bool dictionary : {false, true}) {
				DataChunk input;
				input.Initialize(context, {LogicalType::BIGINT}, count);
				if (dictionary) {
					input.data[0].Slice(keys, dictionary_sel, count);
				} else {
					input.data[0].Reference(keys);
				}
				input.CheckCardinality(count);

				for (bool mapped : {false, true}) {
					CAPTURE(contains_null_hash, filters_null_values, dictionary, mapped);
					vector<idx_t> expected_true, expected_false;
					for (idx_t i = 0; i < count; i++) {
						const auto row = mapped ? input_sel.get_index(i) : i;
						const auto value = input.GetValue(0, row);
						const auto passed = value.IsNull() ? !filters_null_values : value.GetValue<int64_t>() == 11;
						(passed ? expected_true : expected_false).push_back(row);
					}
					SelectionVector true_sel(count), false_sel(count);
					const auto true_count =
					    executor.SelectExpression(input, &true_sel, &false_sel, mapped ? &input_sel : nullptr, count);
					REQUIRE(true_count == expected_true.size());
					for (idx_t i = 0; i < true_count; i++) {
						REQUIRE(true_sel.get_index(i) == expected_true[i]);
					}
					for (idx_t i = 0; i < count - true_count; i++) {
						REQUIRE(false_sel.get_index(i) == expected_false[i]);
					}
				}
			}
		}
	}
}
