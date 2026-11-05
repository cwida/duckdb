#include "catch.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/vector_cache.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"

using namespace duckdb;

TEST_CASE("A constant reference to a string keeps the heap that holds it", "[api]") {
	const string long_string(100000, 'x');
	auto make_source = [&]() {
		Vector source(LogicalType::VARCHAR, 4);
		{
			auto writer = FlatVector::Writer<string_t>(source, 4);
			for (idx_t i = 0; i < 4; i++) {
				writer.WriteValue(i == 2 ? string_t(long_string) : string_t("short"));
			}
		}
		return source;
	};

	// the constant shares the bytes of the string instead of copying them
	Vector reference(LogicalType::VARCHAR);
	{
		auto source = make_source();
		ConstantVector::Reference(reference, count_t(3000), source, 2, 4);
		REQUIRE(ConstantVector::GetData<string_t>(reference)->GetData() ==
		        FlatVector::GetData<string_t>(source)[2].GetData());
	}

	// the source goes away before the reference is read
	{
		auto source = make_source();
		ConstantVector::Reference(reference, count_t(3000), source, 2, 4);
	}
	REQUIRE(reference.GetVectorType() == VectorType::CONSTANT_VECTOR);
	REQUIRE(reference.size() == 3000);
	REQUIRE(reference.GetValue(0) == Value(long_string));
	REQUIRE(reference.GetValue(2999) == Value(long_string));

	// the vector is the source itself
	auto vector = make_source();
	ConstantVector::Reference(vector, count_t(3000), vector, 2, 4);
	REQUIRE(vector.GetVectorType() == VectorType::CONSTANT_VECTOR);
	// the constant holds the heap of the string, which nothing else holds now
	REQUIRE(vector.GetBufferRef()->GetAuxiliaryData());
	REQUIRE(vector.GetValue(1) == Value(long_string));

	// a NULL gives a NULL constant
	auto with_null = make_source();
	FlatVector::SetNull(with_null, 1, true);
	ConstantVector::Reference(reference, count_t(10), with_null, 1, 4);
	REQUIRE(ConstantVector::IsNull(reference));

	// an inlined string is held by the constant itself
	auto with_inlined = make_source();
	ConstantVector::Reference(reference, count_t(10), with_inlined, 1, 4);
	REQUIRE(reference.GetValue(9) == Value("short"));

	// a dictionary references the string of its child, and a constant the string at its only entry
	auto dictionary = make_source();
	SelectionVector sel(2);
	sel.set_index(0, 3);
	sel.set_index(1, 2);
	dictionary.Slice(sel, 2);
	REQUIRE(dictionary.GetVectorType() == VectorType::DICTIONARY_VECTOR);
	ConstantVector::Reference(reference, count_t(3000), dictionary, 1, 2);
	REQUIRE(reference.GetVectorType() == VectorType::CONSTANT_VECTOR);
	REQUIRE(ConstantVector::GetData<string_t>(reference)->GetData() ==
	        FlatVector::GetData<string_t>(DictionaryVector::Child(dictionary))[2].GetData());
	Vector constant(LogicalType::VARCHAR);
	ConstantVector::Reference(constant, count_t(3000), reference, 2999, 3000);
	REQUIRE(ConstantVector::GetData<string_t>(constant)->GetData() ==
	        ConstantVector::GetData<string_t>(reference)->GetData());
	REQUIRE(constant.GetValue(1) == Value(long_string));
}

TEST_CASE("A constant reference to a string outlives the reset of its cached source", "[api]") {
	const string long_string(100000, 'x');
	VectorCache cache(Allocator::DefaultAllocator(), LogicalType::VARCHAR);
	Vector source(cache);
	{
		auto writer = FlatVector::Writer<string_t>(source, 2);
		writer.WriteValue(string_t("short"));
		writer.WriteValue(string_t(long_string));
	}
	Vector reference(LogicalType::VARCHAR);
	ConstantVector::Reference(reference, count_t(3000), source, 1, 2);

	// the cache gives the source its buffer again, and new strings overwrite its entries and heap
	source.ResetFromCache(cache);
	{
		const string other_string(100000, 'y');
		auto writer = FlatVector::Writer<string_t>(source, 2);
		writer.WriteValue(string_t(other_string));
		writer.WriteValue(string_t(other_string));
	}
	REQUIRE(reference.GetValue(2999) == Value(long_string));
}

TEST_CASE("A constant reference holds the entry of a sequence", "[api]") {
	Vector sequence(LogicalType::BIGINT);
	sequence.Sequence(10, 3, 100);
	REQUIRE(sequence.GetVectorType() == VectorType::SEQUENCE_VECTOR);
	Vector reference(LogicalType::BIGINT);
	ConstantVector::Reference(reference, count_t(3000), sequence, 7, 100);
	REQUIRE(reference.GetVectorType() == VectorType::CONSTANT_VECTOR);
	REQUIRE(reference.size() == 3000);
	REQUIRE(reference.GetValue(2999) == Value::BIGINT(31));
}
