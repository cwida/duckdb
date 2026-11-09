#include "catch.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

static void CheckCombineIntoConstantHashes(const Vector &input, idx_t hashes_size, const vector<idx_t> &positions) {
	constexpr hash_t SEED = 123;
	SelectionVector sel(positions.size());
	for (idx_t i = 0; i < positions.size(); i++) {
		sel.set_index(i, positions[i]);
	}
	Vector expected(LogicalType::HASH);
	{
		auto writer = FlatVector::Writer<hash_t>(expected, hashes_size + 1);
		for (idx_t i = 0; i <= hashes_size; i++) {
			writer.WriteValue(SEED);
		}
	}
	VectorOperations::CombineHash(expected, input, sel, positions.size());

	Vector actual(Value::HASH(SEED), count_t(hashes_size));
	VectorOperations::CombineHash(actual, input, sel, positions.size());
	REQUIRE(actual.GetVectorType() == VectorType::FLAT_VECTOR);
	auto actual_hashes = FlatVector::GetData<hash_t>(actual);
	auto expected_hashes = FlatVector::GetData<hash_t>(expected);
	for (auto position : positions) {
		REQUIRE(actual_hashes[position] == expected_hashes[position]);
	}
}

TEST_CASE("Combining a nested key into constant hashes covers every selected row", "[vector_hash]") {
	if (STANDARD_VECTOR_SIZE < 4) {
		// the shapes below need vectors of at least four rows
		return;
	}
	const auto list = Value::LIST(LogicalType::INTEGER, {Value::INTEGER(5), Value(LogicalType::INTEGER)});
	const auto array = Value::ARRAY(LogicalType::INTEGER, {Value::INTEGER(5), Value::INTEGER(6)});
	child_list_t<Value> struct_values;
	struct_values.emplace_back("k", list);
	const auto struct_value = Value::STRUCT(std::move(struct_values));
	for (auto &key : {list, array, struct_value}) {
		Vector constant_input(key, count_t(3));
		Vector flat_input(key, count_t(3));
		flat_input.Flatten();
		for (auto input : {&constant_input, &flat_input}) {
			// a join drops the NULL key rows first, so the selection is not a prefix of the chunk
			CheckCombineIntoConstantHashes(*input, 2, {1, 2});
			// a constant hash vector may hold one physical slot for all of its rows
			CheckCombineIntoConstantHashes(*input, 3, {0, 2});
		}
	}
}

} // namespace duckdb
