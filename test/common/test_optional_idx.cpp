#include "catch.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

using namespace duckdb; // NOLINT

namespace {
optional_idx RoundTrip(optional_idx value) {
	MemoryStream stream;
	BinarySerializer serializer(stream);
	serializer.Begin();
	serializer.WriteProperty(100, "value", value);
	serializer.End();

	stream.Rewind();
	BinaryDeserializer deserializer(stream);
	deserializer.Begin();
	auto result = deserializer.ReadProperty<optional_idx>(100, "value");
	deserializer.End();
	return result;
}
} // namespace

TEST_CASE("optional_idx validity", "[optional_idx]") {
	optional_idx unset;
	REQUIRE(!unset.IsValid());
	REQUIRE(!optional_idx::Invalid().IsValid());
#ifndef DUCKDB_CRASH_ON_ASSERT
	REQUIRE_THROWS_AS(unset.GetIndex(), InternalException);
#endif

	optional_idx zero(0);
	REQUIRE(zero.IsValid());
	REQUIRE(zero.GetIndex() == 0);

	zero.SetInvalid();
	REQUIRE(!zero.IsValid());
}

TEST_CASE("optional_idx can store INVALID_INDEX", "[optional_idx]") {
	optional_idx value(DConstants::INVALID_INDEX);
	REQUIRE(value.IsValid());
	REQUIRE(value.GetIndex() == DConstants::INVALID_INDEX);
	REQUIRE(value != optional_idx());
	REQUIRE(value == optional_idx(DConstants::INVALID_INDEX));
}

TEST_CASE("optional_idx comparison", "[optional_idx]") {
	REQUIRE(optional_idx() == optional_idx::Invalid());
	REQUIRE(optional_idx(1) == optional_idx(1));
	REQUIRE(optional_idx(1) != optional_idx(2));
	REQUIRE(optional_idx(0) != optional_idx());
}

TEST_CASE("optional_idx serialization", "[optional_idx]") {
	REQUIRE(!RoundTrip(optional_idx()).IsValid());
	REQUIRE(RoundTrip(optional_idx(0)) == optional_idx(0));
	REQUIRE(RoundTrip(optional_idx(42)) == optional_idx(42));
	REQUIRE(RoundTrip(optional_idx(DConstants::INVALID_INDEX - 1)) == optional_idx(DConstants::INVALID_INDEX - 1));
	// INVALID_INDEX is the serialized representation of an unset optional_idx
#ifndef DUCKDB_CRASH_ON_ASSERT
	REQUIRE_THROWS_AS(RoundTrip(optional_idx(DConstants::INVALID_INDEX)), InternalException);
#endif
}
