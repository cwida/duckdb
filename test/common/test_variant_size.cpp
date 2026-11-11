#include "catch.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/variant_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/cast/variant/json_to_variant.hpp"

using namespace duckdb;

namespace {

data_ptr_t AllocateSmallVariantBuffer(PrivateAllocatorData *data, idx_t size) {
	if (size > 65536) {
		throw InternalException("Unexpected large allocation in VARIANT size test");
	}
	return Allocator::DefaultAllocate(data, size);
}

data_ptr_t ReallocateSmallVariantBuffer(PrivateAllocatorData *data, data_ptr_t pointer, idx_t old_size, idx_t size) {
	if (size > 65536) {
		throw InternalException("Unexpected large allocation in VARIANT size test");
	}
	return Allocator::DefaultReallocate(data, pointer, old_size, size);
}

struct VariantSizeData {
	VariantSizeData()
	    : output(LogicalType::VARIANT(), 1), dictionary(Allocator::DefaultAllocator()), variant_data(output),
	      result(variant_data, offsets, dictionary, keys_selvec, Allocator::DefaultAllocator()) {
		offsets.Initialize(Allocator::DefaultAllocator(),
		                   {LogicalType::UINTEGER, LogicalType::UINTEGER, LogicalType::UINTEGER, LogicalType::UINTEGER},
		                   1);
		offsets.SetChildCardinality(1);
		auto writer = FlatVector::Writer<string_t>(VariantVector::GetData(output), 1);
		writer.WriteValue(string_t(""));
		Reset();
	}

	void Reset() {
		duckdb::variant::InitializeOffsets(offsets, 1);
	}

	uint32_t &BlobOffset() {
		return duckdb::variant::OffsetData::GetBlob(offsets)[0];
	}

	Vector output;
	DataChunk offsets;
	SelectionVector keys_selvec;
	OrderedOwningStringMap<uint32_t> dictionary;
	VariantVectorData variant_data;
	duckdb::variant::ToVariantGlobalResultData result;
};

} // namespace

TEST_CASE("VARIANT rejects overflowing tuple string sizes before reading payloads", "[variant][variant_size]") {
	Allocator allocator(AllocateSmallVariantBuffer, Allocator::DefaultFree, ReallocateSmallVariantBuffer,
	                    make_uniq<PrivateAllocatorData>());
	Vector source(
	    LogicalType::TUPLE({{"a", LogicalType::VARCHAR}, {"b", LogicalType::VARCHAR}, {"c", LogicalType::VARCHAR}}), 1);
	auto &children = StructVector::GetEntries(source);
	for (auto lengths : {duckdb::vector<uint32_t> {2000000000, 2000000000, 295067278},
	                     duckdb::vector<uint32_t> {1600000000, 1600000000, 1600000000}}) {
		for (idx_t i = 0; i < children.size(); i++) {
			auto writer = FlatVector::Writer<string_t>(children[i], 1);
			// Rejection in the size pass must precede allocation or payload access.
			writer.WriteStringRef(string_t(lengths[i]));
		}
		FlatVector::SetSize(source, 1);
		Vector output(LogicalType::VARIANT(), 1);
		auto &blob = VariantVector::GetData(output);
		blob.SetBuffer(make_buffer<VectorStringBuffer>(allocator, capacity_t(1)));
		REQUIRE_THROWS_WITH(VectorOperations::DefaultCast(source, output, 1),
		                    Catch::Matchers::Contains("encoded row size exceeds the maximum"));
	}
}

TEST_CASE("VARIANT container headers reject overflowing blob offsets", "[variant][variant_size]") {
	VariantSizeData data;
	const auto maximum = NumericLimits<uint32_t>::Maximum();
	struct Header {
		uint32_t length;
		uint32_t children_offset;
		uint32_t bytes;
	};
	for (auto header : {Header {1, 0, 2}, {128, 128, 4}, {maximum, maximum, 10}, {0, maximum, 1}}) {
		INFO(header.length);
		data.Reset();
		data.BlobOffset() = maximum - header.bytes;
		duckdb::variant::WriteContainerData<false>(data.variant_data, 0, data.BlobOffset(), header.length,
		                                           header.children_offset);
		REQUIRE(data.BlobOffset() == maximum);

		data.BlobOffset() = maximum - header.bytes + 1;
		auto original_offset = data.BlobOffset();
		REQUIRE_THROWS_WITH(duckdb::variant::WriteContainerData<false>(data.variant_data, 0, data.BlobOffset(),
		                                                               header.length, header.children_offset),
		                    Catch::Matchers::Contains("encoded row size exceeds the maximum"));
		REQUIRE(data.BlobOffset() == original_offset);
	}
}

TEST_CASE("VARIANT JSON size pass rejects overflowing blob offsets", "[variant][variant_size]") {
	VariantSizeData data;
	const auto maximum = NumericLimits<uint32_t>::Maximum();
	duckdb::vector<pair<string, uint32_t>> cases {{"\"\"", 1},
	                                              {"\"abc\"", 4},
	                                              {"\"\\u00e9\"", 3},
	                                              {"1", 8},
	                                              {"18446744073709551615", 8},
	                                              {"-1", 8},
	                                              {"-9223372036854775808", 8},
	                                              {"1.5", 8},
	                                              {"123456789012345678901234567890", 8},
	                                              {"[]", 1},
	                                              {"{}", 1},
	                                              {"[1]", 10},
	                                              {"{\"a\":1}", 10},
	                                              {"[[],{}]", 4},
	                                              {"[true,false,null]", 2},
	                                              {"null", 0},
	                                              {"true", 0},
	                                              {"false", 0}};
	for (auto length : {127U, 128U, 16383U, 16384U}) {
		cases.emplace_back("\"" + string(length, 'a') + "\"", length + GetVarintSize(length));
	}
	for (auto &entry : cases) {
		INFO(entry.first);
		Vector source(LogicalType::JSON(), 1);
		{
			auto writer = FlatVector::Writer<string_t>(source, 1);
			writer.WriteValue(string_t(entry.first));
		}
		duckdb::variant::ToVariantSourceData source_data(source, 1);
		data.Reset();
		data.BlobOffset() = maximum - entry.second;
		REQUIRE(
		    (duckdb::variant::ConvertJSONToVariant<false, false>(source_data, data.result, 1, nullptr, nullptr, true)));
		REQUIRE(data.BlobOffset() == maximum);

		if (entry.second) {
			data.Reset();
			data.BlobOffset() = maximum - entry.second + 1;
			REQUIRE_THROWS_WITH((duckdb::variant::ConvertJSONToVariant<false, false>(source_data, data.result, 1,
			                                                                         nullptr, nullptr, true)),
			                    Catch::Matchers::Contains("encoded row size exceeds the maximum"));
		}
	}
}

TEST_CASE("VARIANT JSON string sizes include the length prefix without wrapping", "[variant][variant_size]") {
	VariantSizeData data;
	const auto maximum = NumericLimits<uint32_t>::Maximum();
	duckdb_yyjson::yyjson_val value {};
	// The size pass only reads this descriptor's length.
	REQUIRE(duckdb_yyjson::yyjson_set_strn(&value, "", maximum - 5));
	REQUIRE(duckdb::variant::ConvertJSONPrimitive<false>(&value, data.result, 0, true));
	REQUIRE(data.BlobOffset() == maximum);

	for (auto length : {maximum - 4, maximum}) {
		data.Reset();
		REQUIRE(duckdb_yyjson::yyjson_set_strn(&value, "", length));
		REQUIRE_THROWS_WITH(duckdb::variant::ConvertJSONPrimitive<false>(&value, data.result, 0, true),
		                    Catch::Matchers::Contains("encoded row size exceeds the maximum"));
	}
}
