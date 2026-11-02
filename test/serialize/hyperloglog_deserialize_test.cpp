#include "catch.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/types/hyperloglog.hpp"

namespace duckdb {

static unique_ptr<HyperLogLog> RoundTripHyperLogLog(const HyperLogLog &hll) {
	Allocator allocator;
	MemoryStream stream(allocator);
	BinarySerializer::Serialize(hll, stream);
	stream.Rewind();
	return BinaryDeserializer::Deserialize<HyperLogLog>(stream);
}

TEST_CASE("Deserialize HyperLogLog sketches with out-of-range registers", "[serialization]") {
	HyperLogLog hll;
	for (hash_t h = 0; h < 1000; h++) {
		hll.InsertElement(Hash(h));
	}
	auto valid = RoundTripHyperLogLog(hll);
	REQUIRE(valid->Count() == hll.Count());

	// a register can hold at most Q + 1
	HyperLogLog max_register;
	max_register.Update(0, HyperLogLog::Q + 1);
	REQUIRE_NOTHROW(RoundTripHyperLogLog(max_register));

	for (uint8_t value : {uint8_t(HyperLogLog::Q + 2), uint8_t(200), uint8_t(255)}) {
		HyperLogLog invalid;
		invalid.Update(HyperLogLog::M - 1, value);
		REQUIRE_THROWS_AS(RoundTripHyperLogLog(invalid), SerializationException);
	}
}

} // namespace duckdb
