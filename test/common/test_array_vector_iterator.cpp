#include "catch.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"

namespace duckdb {

static Value IntArray(const vector<Value> &values) {
	return Value::ARRAY(LogicalType::INTEGER, values);
}

static vector<int32_t> ReadArray(const VectorIterator<VectorArrayType<int32_t>>::ValueEntry &entry) {
	vector<int32_t> result;
	for (auto child : entry.GetChildValues()) {
		result.push_back(child.IsValid() ? child.GetValue() : -1);
	}
	return result;
}

TEST_CASE("Array vector iterator", "[vector_iterator]") {
	const auto type = LogicalType::ARRAY(LogicalType::INTEGER, 3);
	Vector arrays(type, 4);
	arrays.SetValue(0, IntArray({Value::INTEGER(1), Value::INTEGER(2), Value::INTEGER(3)}));
	arrays.SetValue(1, Value(type));
	arrays.SetValue(2, IntArray({Value::INTEGER(4), Value(LogicalType::INTEGER), Value::INTEGER(6)}));
	arrays.SetValue(3, IntArray({Value::INTEGER(7), Value::INTEGER(8), Value::INTEGER(9)}));
	FlatVector::SetSize(arrays, count_t(4));

	SECTION("Flat arrays") {
		auto values = arrays.Values<VectorArrayType<int32_t>>();
		REQUIRE(values.size() == 4);
		REQUIRE(values.CanHaveNull());

		idx_t row = 0;
		for (auto entry : values) {
			REQUIRE(entry.GetIndex() == row++);
			REQUIRE(entry.GetArraySize() == 3);
		}
		REQUIRE(row == 4);

		REQUIRE(values[0].IsValid());
		REQUIRE(ReadArray(values[0]) == vector<int32_t> {1, 2, 3});
		auto data = values[0].TryGetData();
		REQUIRE(data);
		REQUIRE(data[0] == 1);
		REQUIRE(data[2] == 3);

		REQUIRE(!values[1].IsValid());

		// a NULL element prevents reading the array in place
		REQUIRE(values[2].IsValid());
		REQUIRE(!values[2].TryGetData());
		REQUIRE(ReadArray(values[2]) == vector<int32_t> {4, -1, 6});
		REQUIRE(!values[2].GetChildValue(1).IsValid());
		REQUIRE(values[2].GetChildValue(2).GetValue() == 6);

		REQUIRE(values[3].TryGetData()[1] == 8);
		REQUIRE(!values.CanReadAllInPlace());
	}
	SECTION("Dictionary arrays") {
		SelectionVector sel(3);
		sel.set_index(0, 3);
		sel.set_index(1, 2);
		sel.set_index(2, 3);
		auto dictionary = DictionaryVector::CreateReusableGlobalDictionary(type, 4);
		dictionary->data.Reference(arrays);
		Vector sliced(type);
		sliced.Dictionary(dictionary, sel, 3);
		REQUIRE(sliced.GetVectorType() == VectorType::DICTIONARY_VECTOR);

		auto values = sliced.Values<VectorArrayType<int32_t>>();
		REQUIRE(values.size() == 3);
		REQUIRE(ReadArray(values[0]) == vector<int32_t> {7, 8, 9});
		REQUIRE(ReadArray(values[1]) == vector<int32_t> {4, -1, 6});
		REQUIRE(values[0].TryGetData()[0] == 7);
		REQUIRE(!values[1].TryGetData());
		REQUIRE(values[2].TryGetData() == values[0].TryGetData());
	}
	SECTION("Constant arrays") {
		Vector constant(IntArray({Value::INTEGER(5), Value::INTEGER(6), Value::INTEGER(7)}), count_t(2));
		auto values = constant.Values<VectorArrayType<int32_t>>();
		REQUIRE(values.size() == 2);
		REQUIRE(ReadArray(values[1]) == vector<int32_t> {5, 6, 7});
		REQUIRE(values[1].TryGetData()[2] == 7);

		REQUIRE(values.CanReadAllInPlace());
		REQUIRE(values[1].GetDataUnsafe() == values[0].GetDataUnsafe());
		REQUIRE(values[1].GetDataUnsafe()[2] == 7);
	}
	SECTION("Arrays without NULLs") {
		Vector valid_arrays(type, 2);
		valid_arrays.SetValue(0, IntArray({Value::INTEGER(1), Value::INTEGER(2), Value::INTEGER(3)}));
		valid_arrays.SetValue(1, IntArray({Value::INTEGER(4), Value::INTEGER(5), Value::INTEGER(6)}));
		FlatVector::SetSize(valid_arrays, count_t(2));

		auto values = valid_arrays.Values<VectorArrayType<int32_t>>();
		REQUIRE(values.CanReadAllInPlace());
		REQUIRE(values[0].GetDataUnsafe() == values[0].TryGetData());
		REQUIRE(values[1].GetDataUnsafe() == values[0].GetDataUnsafe() + 3);
		REQUIRE(values[1].GetDataUnsafe()[2] == 6);
	}
}

} // namespace duckdb
