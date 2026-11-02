#include "catch.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/storage/data_pointer.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"

namespace duckdb {

struct Bar {
	uint32_t b;
	void Serialize(Serializer &serializer) const {
		serializer.WriteProperty<uint32_t>(1, "b", b);
	}

	static unique_ptr<Bar> Deserialize(Deserializer &deserializer) {
		auto result = make_uniq<Bar>();
		deserializer.ReadProperty<uint32_t>(1, "b", result->b);
		return result;
	}
};

struct Foo {
	int32_t a;
	unique_ptr<Bar> bar;
	int32_t c;

	void Serialize(Serializer &serializer) const {
		serializer.WriteProperty<int32_t>(1, "a", a);
		serializer.WritePropertyWithDefault<unique_ptr<Bar>>(2, "bar", bar, unique_ptr<Bar>());
		serializer.WriteProperty<int32_t>(3, "c", c);
	}

	static unique_ptr<Foo> Deserialize(Deserializer &deserializer) {
		auto result = make_uniq<Foo>();
		deserializer.ReadProperty<int32_t>(1, "a", result->a);
		deserializer.ReadPropertyWithExplicitDefault<unique_ptr<Bar>>(2, "bar", result->bar, unique_ptr<Bar>());
		deserializer.ReadProperty<int32_t>(3, "c", result->c);
		return result;
	}
};

TEST_CASE("Test default values", "[serialization]") {
	Foo foo_in;
	foo_in.a = 42;
	foo_in.bar = make_uniq<Bar>();
	foo_in.bar->b = 43;
	foo_in.c = 44;

	Allocator allocator;
	MemoryStream stream(allocator);
	SerializationOptions options;
	options.serialize_default_values = false;
	BinarySerializer::Serialize(foo_in, stream, options);
	auto pos1 = stream.GetPosition();
	stream.Rewind();
	auto foo_out_ptr = BinaryDeserializer::Deserialize<Foo>(stream);
	auto &foo_out = *foo_out_ptr.get();

	REQUIRE(foo_in.a == foo_out.a);
	REQUIRE(foo_in.bar->b == foo_out.bar->b);
	REQUIRE(foo_in.c == foo_out.c);

	// Now try with a default value
	foo_in.bar = nullptr;

	stream.Rewind();

	options.serialize_default_values = false;
	BinarySerializer::Serialize(foo_in, stream, options);
	auto pos2 = stream.GetPosition();
	stream.Rewind();

	foo_out_ptr = BinaryDeserializer::Deserialize<Foo>(stream);
	auto &foo_out2 = *foo_out_ptr.get();

	REQUIRE(foo_in.a == foo_out2.a);
	REQUIRE(foo_out2.bar == nullptr);
	REQUIRE(foo_in.c == foo_out2.c);

	// We should not have written the default value
	REQUIRE(pos1 > pos2);
}

//------------------------------------------------------
// Test deleted properties
//------------------------------------------------------

struct Complex {
	int c1;
	string c2;
	Complex(int c1, string c2) : c1(c1), c2(c2) {
	}
	Complex() : c1(0), c2("") {
	}

	void Serialize(Serializer &serializer) const {
		serializer.WriteProperty<int>(1, "c1", c1);
		serializer.WriteProperty<string>(2, "c2", c2);
	}

	static unique_ptr<Complex> Deserialize(Deserializer &deserializer) {
		auto result = make_uniq<Complex>();
		deserializer.ReadProperty<int>(1, "c1", result->c1);
		deserializer.ReadProperty<string>(2, "c2", result->c2);
		return result;
	}
};

struct FooV1 {
	int p1;
	vector<unique_ptr<Complex>> p2;
	int p3;
	unique_ptr<Complex> p4;

	void Serialize(Serializer &serializer) const {
		serializer.WriteProperty<int>(1, "p1", p1);
		serializer.WritePropertyWithDefault<vector<unique_ptr<Complex>>>(2, "p2", p2);
		serializer.WriteProperty<int>(3, "p3", p3);
		serializer.WriteProperty<unique_ptr<Complex>>(4, "p4", p4);
	}

	static unique_ptr<FooV1> Deserialize(Deserializer &deserializer) {
		auto result = make_uniq<FooV1>();
		deserializer.ReadProperty<int>(1, "p1", result->p1);
		deserializer.ReadPropertyWithDefault<vector<unique_ptr<Complex>>>(2, "p2", result->p2);
		deserializer.ReadProperty<int>(3, "p3", result->p3);
		deserializer.ReadProperty<unique_ptr<Complex>>(4, "p4", result->p4);
		return result;
	}
};

struct FooV2 {
	int p1;
	/*vector<unique_ptr<Complex>> p2;*/ // In v2, this is deleted
	int p3;
	unique_ptr<Complex> p4;
	unique_ptr<Complex> p5; // In v2, this is added

	void Serialize(Serializer &serializer) const {
		serializer.WriteProperty<int>(1, "p1", p1);
		// This field is deleted!
		/* serializer.WriteDeletedProperty<vector<unique_ptr<Complex>>>(2, "p2"); */
		serializer.WriteProperty<int>(3, "p3", p3);
		serializer.WriteProperty<unique_ptr<Complex>>(4, "p4", p4);

		// Because this is a new field, we have to provide a default value
		// to try to preserve backwards compatibility (in best case)
		serializer.WritePropertyWithDefault<unique_ptr<Complex>>(5, "p5", p5);
	}

	static unique_ptr<FooV2> Deserialize(Deserializer &deserializer) {
		auto result = make_uniq<FooV2>();
		deserializer.ReadProperty(1, "p1", result->p1);
		deserializer.ReadDeletedProperty<vector<unique_ptr<Complex>>>(2, "p2");
		deserializer.ReadProperty(3, "p3", result->p3);
		deserializer.ReadProperty(4, "p4", result->p4);
		deserializer.ReadPropertyWithDefault<unique_ptr<Complex>>(5, "p5", result->p5);
		return result;
	}
};

TEST_CASE("Test deleted values", "[serialization]") {
	FooV1 v1_in = {1, {}, 6, make_uniq<Complex>(1, "foo")};
	v1_in.p2.push_back(make_uniq<Complex>(2, "3"));
	v1_in.p2.push_back(make_uniq<Complex>(4, "5"));

	FooV2 v2_in = {1, 3, make_uniq<Complex>(1, "foo"), nullptr};

	Allocator allocator;
	MemoryStream stream(allocator);
	SerializationOptions options;
	options.serialize_default_values = false;
	// First of, sanity check that foov1 <-> foov1 works
	BinarySerializer::Serialize(v1_in, stream, options);
	{
		stream.Rewind();
		auto v1_out_ptr = BinaryDeserializer::Deserialize<FooV1>(stream);
		auto &v1_out = *v1_out_ptr.get();
		REQUIRE(v1_in.p1 == v1_out.p1);
		REQUIRE(v1_in.p2.size() == v1_out.p2.size());
		REQUIRE(v1_in.p2[0]->c1 == v1_out.p2[0]->c1);
		REQUIRE(v1_in.p2[0]->c2 == v1_out.p2[0]->c2);
		REQUIRE(v1_in.p2[1]->c1 == v1_out.p2[1]->c1);
		REQUIRE(v1_in.p2[1]->c2 == v1_out.p2[1]->c2);
		REQUIRE(v1_in.p3 == v1_out.p3);
		REQUIRE(v1_in.p4->c1 == v1_out.p4->c1);
		REQUIRE(v1_in.p4->c2 == v1_out.p4->c2);
	}

	stream.Rewind();

	// Also check that foov2 <-> foov2 works
	options.serialize_default_values = false;
	BinarySerializer::Serialize(v2_in, stream, options);
	{
		stream.Rewind();
		auto v2_out_ptr = BinaryDeserializer::Deserialize<FooV2>(stream);
		auto &v2_out = *v2_out_ptr.get();
		REQUIRE(v2_in.p1 == v2_out.p1);
		REQUIRE(v2_in.p3 == v2_out.p3);
		REQUIRE(v2_in.p4->c1 == v2_out.p4->c1);
		REQUIRE(v2_in.p4->c2 == v2_out.p4->c2);
		REQUIRE(v2_in.p5 == v2_out.p5);
	}

	// Check that foov1 -> foov2 works (backwards compatible)
	stream.Rewind();
	options.serialize_default_values = false;
	BinarySerializer::Serialize(v1_in, stream, options);
	{
		stream.Rewind();
		auto v2_out_ptr = BinaryDeserializer::Deserialize<FooV2>(stream);
		auto &v2_out = *v2_out_ptr.get();
		REQUIRE(v1_in.p1 == v2_out.p1);
		REQUIRE(v1_in.p3 == v2_out.p3);
		REQUIRE(v1_in.p4->c1 == v2_out.p4->c1);
		REQUIRE(v1_in.p4->c2 == v2_out.p4->c2);
		REQUIRE(v2_out.p5 == nullptr);
	}

	// Check that foov2 -> foov1 works (forwards compatible)
	// This should be ok, since the property we deleted was optional (had a default value)
	stream.Rewind();
	options.serialize_default_values = false;
	BinarySerializer::Serialize(v2_in, stream, options);
	{
		stream.Rewind();
		auto v1_out_ptr = BinaryDeserializer::Deserialize<FooV1>(stream);
		auto &v1_out = *v1_out_ptr.get();
		REQUIRE(v2_in.p1 == v1_out.p1);
		REQUIRE(v2_in.p3 == v1_out.p3);
		REQUIRE(v2_in.p4->c1 == v1_out.p4->c1);
		REQUIRE(v2_in.p4->c2 == v1_out.p4->c2);
		REQUIRE(v1_out.p2.empty());
	}

	// If we change the new value in foov2 to something thats not the default, we break forwards compatibility.
	// But thats life. Tough shit.
	stream.Rewind();
	v2_in.p5 = make_uniq<Complex>(2, "foo");
	options.serialize_default_values = false;
	BinarySerializer::Serialize(v2_in, stream, options);
	{
		stream.Rewind();
		REQUIRE_THROWS(BinaryDeserializer::Deserialize<FooV1>(stream));
	}

	// However, the new value should be read correctly!
	stream.Rewind();
	options.serialize_default_values = false;
	BinarySerializer::Serialize(v2_in, stream, options);
	{
		stream.Rewind();
		auto v2_out_ptr = BinaryDeserializer::Deserialize<FooV2>(stream);
		auto &v2_out = *v2_out_ptr.get();
		REQUIRE(v2_in.p1 == v2_out.p1);
		REQUIRE(v2_in.p3 == v2_out.p3);
		REQUIRE(v2_in.p4->c1 == v2_out.p4->c1);
		REQUIRE(v2_in.p4->c2 == v2_out.p4->c2);
		REQUIRE(v2_out.p5->c1 == 2);
		REQUIRE(v2_out.p5->c2 == "foo");
	}
}

static DataPointer DeserializeDataPointer(MemoryStream &stream, const LogicalType &type) {
	BinaryDeserializer deserializer(stream);
	deserializer.Set<const LogicalType &>(type);
	deserializer.Begin();
	auto result = DataPointer::Deserialize(deserializer);
	deserializer.End();
	return result;
}

TEST_CASE("Test DataPointer byte size storage version compatibility", "[serialization]") {
	Allocator allocator;
	MemoryStream stream(allocator);
	auto type = LogicalType::INTEGER;
	DataPointer pointer(BaseStatistics::CreateUnknown(type));
	pointer.tuple_count = 42;
	pointer.block_pointer = BlockPointer(7, 11);
	pointer.compression_type = CompressionType::COMPRESSION_UNCOMPRESSED;
	pointer.byte_size = 1234;

	SerializationOptions options;
	options.storage_compatibility.storage_version = StorageVersion::V2_0_0;
	BinarySerializer::Serialize(pointer, stream, options);
	stream.Rewind();
	auto current = DeserializeDataPointer(stream, type);
	REQUIRE(current.byte_size == pointer.byte_size);

	stream.Rewind();
	options.storage_compatibility.storage_version = StorageVersion::V1_5_0;
	BinarySerializer::Serialize(pointer, stream, options);
	stream.Rewind();
	auto legacy = DeserializeDataPointer(stream, type);
	REQUIRE(!legacy.byte_size);
}

TEST_CASE("Constraint check mode survives serialization", "[serialization][deferred]") {
	for (auto check_mode :
	     {ConstraintCheckMode::DEFAULT, ConstraintCheckMode::IMMEDIATE, ConstraintCheckMode::DEFERRED}) {
		for (auto primary_key : {false, true}) {
			UniqueConstraint constraint(vector<Identifier> {Identifier("i")}, primary_key, check_mode);

			Allocator allocator;
			MemoryStream stream(allocator);
			SerializationOptions options;
			options.storage_compatibility = StorageCompatibility::Latest();
			BinarySerializer::Serialize(constraint, stream, options);
			stream.Rewind();
			auto restored = BinaryDeserializer::Deserialize<Constraint>(stream);
			REQUIRE(restored->Cast<UniqueConstraint>().check_mode == check_mode);
			REQUIRE(restored->ToString() == constraint.ToString());
		}
	}
}

TEST_CASE("Test window expression type survives serialization", "[serialization]") {
	auto parser = Parser::GetBuiltinParser();
	auto expressions = parser.ParseExpressionList(
	    "row_number() OVER (), lag(x, 1) OVER (ORDER BY x), sum(x) OVER (PARTITION BY y), first_value(x) OVER ()");
	for (auto version : {StorageVersion::V1_5_0, StorageVersion::V2_0_0}) {
		for (auto &expr : expressions) {
			INFO(expr->ToString());
			Allocator allocator;
			MemoryStream stream(allocator);
			SerializationOptions options;
			options.storage_compatibility.storage_version = version;
			BinarySerializer::Serialize(*expr, stream, options);
			stream.Rewind();
			auto copy = BinaryDeserializer::Deserialize<ParsedExpression>(stream);
			REQUIRE(copy->GetExpressionType() == expr->GetExpressionType());
		}
	}
}

} // namespace duckdb
