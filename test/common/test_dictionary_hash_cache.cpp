#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/join_hashtable.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/execution/operator/scan/physical_dummy_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include <thread>

namespace duckdb {

TEST_CASE("Dictionary hash cache publishes complete hashes to concurrent readers", "[dictionary_hash_cache]") {
	for (idx_t size : {idx_t(1), idx_t(257), idx_t(4099)}) {
		for (idx_t epoch = 0; epoch < 4; epoch++) {
			auto dictionary = DictionaryVector::CreateReusableGlobalDictionary(LogicalType::VARCHAR, size);
			{
				auto writer = FlatVector::Writer<string_t>(dictionary->data, size);
				for (idx_t i = 0; i < size; i++) {
					if (i % 17 == 0) {
						writer.WriteNull();
					} else {
						auto value = i % 17 == 1 ? string()
						                         : "dictionary value " + std::to_string(epoch) + std::to_string(i) +
						                               string(200, 'x');
						writer.WriteValue(string_t(value));
					}
				}
			}
			// Hashing the flat child gives an oracle without initializing the shared cache.
			Vector expected(LogicalType::HASH, size);
			VectorOperations::Hash(dictionary->data, expected);
			vector<hash_t> expected_hashes;
			for (auto hash : expected.ValidValues<hash_t>()) {
				expected_hashes.push_back(hash.GetValue());
			}
			constexpr idx_t THREAD_COUNT = 8;
			vector<unique_ptr<Vector>> inputs;
			for (idx_t t = 0; t < THREAD_COUNT; t++) {
				SelectionVector selection(STANDARD_VECTOR_SIZE);
				for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
					selection.set_index(i, (i + t) % size);
				}
				auto input = make_uniq<Vector>(LogicalType::VARCHAR);
				input->Dictionary(dictionary, selection, STANDARD_VECTOR_SIZE);
				inputs.push_back(std::move(input));
			}
			atomic<idx_t> ready {0};
			atomic<bool> start {false};
			atomic<bool> success {true};
			vector<std::thread> threads;
			for (idx_t t = 0; t < THREAD_COUNT; t++) {
				threads.emplace_back([&, t] {
					ready.fetch_add(1);
					while (!start.load()) {
						std::this_thread::yield();
					}
					try {
						for (idx_t repeat = 0; repeat < 32; repeat++) {
							const auto &cached = DictionaryVector::GetCachedHashes(*inputs[t]);
							if (cached.size() != size) {
								success = false;
								break;
							}
							idx_t i = 0;
							for (auto hash : cached.Values<hash_t>()) {
								if (!hash.IsValid() || hash.GetValue() != expected_hashes[i]) {
									success = false;
								}
								i++;
							}
						}
					} catch (...) {
						success = false;
					}
				});
			}
			while (ready.load() != THREAD_COUNT) {
				std::this_thread::yield();
			}
			start = true;
			for (auto &thread : threads) {
				thread.join();
			}
			REQUIRE(success.load());
		}
	}
}

TEST_CASE("Dictionary hash cache preserves selected and combined hashes after producer release",
          "[dictionary_hash_cache]") {
	auto dictionary = DictionaryVector::CreateReusableDictionary(LogicalType::VARCHAR, 3);
	{
		auto writer = FlatVector::Writer<string_t>(dictionary->data, 3);
		writer.WriteNull();
		writer.WriteValue(string_t("short"));
		writer.WriteValue(string_t("a dictionary string longer than the inline representation"));
	}
	SelectionVector selection(STANDARD_VECTOR_SIZE), reverse(STANDARD_VECTOR_SIZE);
	for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
		selection.set_index(i, i % 3);
		reverse.set_index(i, STANDARD_VECTOR_SIZE - i - 1);
	}
	Vector input(LogicalType::VARCHAR);
	input.Dictionary(dictionary, selection, STANDARD_VECTOR_SIZE);
	auto flat = Vector::Ref(input);
	flat.Flatten();
	dictionary.reset();

	Vector actual(LogicalType::HASH), expected(LogicalType::HASH);
	auto check = [&] {
		auto actual_hashes = actual.Values<hash_t>();
		auto expected_hashes = expected.Values<hash_t>();
		for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
			REQUIRE(actual_hashes[i].IsValid());
			REQUIRE(actual_hashes[i].GetValue() == expected_hashes[i].GetValue());
		}
	};
	VectorOperations::Hash(input, actual);
	VectorOperations::Hash(flat, expected);
	check();
	VectorOperations::CombineHash(actual, input);
	VectorOperations::CombineHash(expected, flat);
	check();
	VectorOperations::Hash(input, actual, reverse, STANDARD_VECTOR_SIZE);
	VectorOperations::Hash(flat, expected, reverse, STANDARD_VECTOR_SIZE);
	check();
	VectorOperations::CombineHash(actual, input, reverse, STANDARD_VECTOR_SIZE);
	VectorOperations::CombineHash(expected, flat, reverse, STANDARD_VECTOR_SIZE);
	check();
}

TEST_CASE("Local dictionary owners preserve escaped values identity and hash caches", "[dictionary_hash_cache]") {
	Vector result(LogicalType::VARCHAR);
	weak_ptr<DictionaryEntry> original_owner;
	optional_ptr<const Vector> original_hashes;
	string dictionary_id;
	const string text = "a dictionary string longer than the inline representation";
	{
		auto entry = DictionaryVector::CreateReusableGlobalDictionary(LogicalType::VARCHAR, 3);
		{
			auto writer = FlatVector::Writer<string_t>(entry->data, 3);
			writer.WriteNull();
			writer.WriteValue(string_t(text));
			writer.WriteValue(string_t(""));
		}
		original_owner = entry;
		dictionary_id = entry->id;
		SelectionVector selection(3);
		selection.set_index(0, 1);
		selection.set_index(1, 0);
		selection.set_index(2, 2);
		Vector original(LogicalType::VARCHAR);
		original.Dictionary(entry, selection, 3);
		original_hashes = DictionaryVector::GetCachedHashes(original);

		DictionaryOwnershipCache first_cache(8), second_cache(1);
		const auto &first_slot = first_cache.GetEntry(0, entry);
		auto first = first_slot;
		auto second = second_cache.GetEntry(0, entry);
		REQUIRE(first.get() == entry.get());
		REQUIRE(second.get() == entry.get());
		const auto global_references = entry.use_count();
		const auto second_references = second.use_count();
		{
			auto retained = first_cache.GetEntry(0, entry);
			REQUIRE(entry.use_count() == global_references);
			REQUIRE(second.use_count() == second_references);
			result.Dictionary(retained, selection, 3);
			REQUIRE(entry.use_count() == global_references);
		}
		REQUIRE(&DictionaryVector::GetCachedHashes(result) == original_hashes.get());

		// Populating another column preserves slots and previously emitted vectors.
		REQUIRE(first_cache.GetEntry(7, entry).get() == entry.get());
		REQUIRE(&first_slot == &first_cache.GetEntry(0, entry));
		REQUIRE(first_slot.get() == entry.get());
		auto replacement = DictionaryVector::CreateReusableGlobalDictionary(LogicalType::VARCHAR, 1);
		REQUIRE(first_cache.GetEntry(0, replacement).get() == replacement.get());
		REQUIRE(first_slot.get() == replacement.get());
		first_cache.Reset();
		REQUIRE_FALSE(first_slot);
		REQUIRE(&first_slot == &first_cache.GetEntry(0, entry));
		REQUIRE(first_slot.get() == entry.get());
		first_cache.Reset();
		second_cache.Reset();
	}
	REQUIRE_FALSE(original_owner.expired());
	REQUIRE(DictionaryVector::IsGlobalDictionary(result));
	REQUIRE(DictionaryVector::DictionaryId(result) == dictionary_id);
	REQUIRE(&DictionaryVector::GetCachedHashes(result) == original_hashes.get());
	REQUIRE(result.GetValue(0) == Value(text));
	REQUIRE(result.GetValue(1).IsNull());
	REQUIRE(result.GetValue(2) == Value(""));
	Vector empty(LogicalType::VARCHAR);
	result.Reference(empty);
	REQUIRE(original_owner.expired());
}

TEST_CASE("Resetting a hash join scan releases its dictionary ownership", "[dictionary_hash_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	PhysicalPlan plan(Allocator::Get(*con.context));
	PhysicalDummyScan op(plan, {LogicalType::INTEGER}, 1);
	vector<JoinCondition> conditions;
	conditions.emplace_back(make_uniq<BoundReferenceExpression>(LogicalType::INTEGER, 0),
	                        make_uniq<BoundReferenceExpression>(LogicalType::INTEGER, 0),
	                        ExpressionType::COMPARE_EQUAL);
	const vector<idx_t> output_columns {1};
	JoinHashTable table(*con.context, op, conditions, {LogicalType::INTEGER}, JoinType::INNER, 0, output_columns,
	                    nullptr);
	TupleDataChunkState key_state;
	JoinHashTable::ScanStructure scan(table, key_state);
	weak_ptr<DictionaryEntry> original_owner;
	{
		auto entry = DictionaryVector::CreateReusableGlobalDictionary(LogicalType::INTEGER, 1);
		original_owner = entry;
		scan.dictionary_gather_state.GetEntry(0, entry);
	}
	REQUIRE_FALSE(original_owner.expired());
	scan.Reset();
	REQUIRE(original_owner.expired());
	scan.Reset();
	REQUIRE(original_owner.expired());
}

} // namespace duckdb
