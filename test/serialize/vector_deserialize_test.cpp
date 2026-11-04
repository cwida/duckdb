#include "catch.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

//! Deserializes a vector of the given type from data written by write, and returns it
template <class FUNC>
static unique_ptr<Vector> DeserializeVectorReturn(const LogicalType &type, idx_t count, FUNC write) {
	Allocator allocator;
	MemoryStream stream(allocator);
	BinarySerializer serializer(stream);
	serializer.Begin();
	write(serializer);
	serializer.End();
	stream.Rewind();

	BinaryDeserializer deserializer(stream);
	deserializer.Begin();
	auto result = make_uniq<Vector>(type, MaxValue<idx_t>(count, STANDARD_VECTOR_SIZE));
	result->Deserialize(deserializer, count);
	deserializer.End();
	return result;
}

//! Deserializes a vector of the given type from data written by write
template <class FUNC>
static void DeserializeVector(const LogicalType &type, idx_t count, FUNC write) {
	DeserializeVectorReturn(type, count, write);
}

static void WriteFlatHeader(Serializer &serializer, VectorType vector_type = VectorType::FLAT_VECTOR) {
	serializer.WriteProperty(90, "vector_type", vector_type);
	serializer.WriteProperty(100, "has_validity_mask", false);
}

static void WriteIntegerChild(Serializer &serializer, field_id_t field_id, idx_t count) {
	serializer.WriteObject(field_id, "child", [&](Serializer &obj) {
		WriteFlatHeader(obj);
		vector<int32_t> data(count, 42);
		obj.WriteProperty(102, "data", const_data_ptr_cast(data.data()), count * sizeof(int32_t));
	});
}

static void WriteListVector(Serializer &serializer, idx_t list_size, const vector<list_entry_t> &entries) {
	WriteFlatHeader(serializer);
	serializer.WriteProperty<uint64_t>(104, "list_size", list_size);
	serializer.WriteList(105, "entries", entries.size(), [&](Serializer::List &list, idx_t i) {
		list.WriteObject([&](Serializer &obj) {
			obj.WriteProperty<uint64_t>(100, "offset", entries[i].offset);
			obj.WriteProperty<uint64_t>(101, "length", entries[i].length);
		});
	});
	WriteIntegerChild(serializer, 106, list_size);
}

TEST_CASE("Deserialize LIST vectors with out-of-range entries", "[serialization]") {
	auto type = LogicalType::LIST(LogicalType::INTEGER);
	REQUIRE_NOTHROW(DeserializeVector(type, 2, [&](Serializer &s) { WriteListVector(s, 3, {{0, 1}, {1, 2}}); }));
	REQUIRE_THROWS_AS(DeserializeVector(type, 1,
	                                    [&](Serializer &s) {
		                                    WriteListVector(s, 3, {{2, 2}});
	                                    }),
	                  SerializationException);
	REQUIRE_THROWS_AS(DeserializeVector(type, 1,
	                                    [&](Serializer &s) {
		                                    WriteListVector(s, 3, {{5, 0}});
	                                    }),
	                  SerializationException);
	REQUIRE_THROWS_AS(DeserializeVector(type, 1,
	                                    [&](Serializer &s) {
		                                    WriteListVector(s, 3, {{1, NumericLimits<uint64_t>::Maximum()}});
	                                    }),
	                  SerializationException);
	// more entries than rows in the vector
	REQUIRE_THROWS_AS(DeserializeVector(type, 1,
	                                    [&](Serializer &s) {
		                                    WriteListVector(s, 3, {{0, 1}, {1, 1}});
	                                    }),
	                  SerializationException);
}

TEST_CASE("Deserialize ARRAY vectors with a mismatching array size", "[serialization]") {
	auto type = LogicalType::ARRAY(LogicalType::INTEGER, 2);
	auto write_array = [&](idx_t array_size, idx_t count) {
		return [=](Serializer &s) {
			WriteFlatHeader(s);
			s.WriteProperty<uint64_t>(103, "array_size", array_size);
			WriteIntegerChild(s, 104, array_size * count);
		};
	};
	REQUIRE_NOTHROW(DeserializeVector(type, 3, write_array(2, 3)));
	REQUIRE_THROWS_AS(DeserializeVector(type, 3, write_array(1, 3)), SerializationException);
	REQUIRE_THROWS_AS(DeserializeVector(type, 3, write_array(3, 3)), SerializationException);
}

TEST_CASE("Deserialize dictionary vectors with out-of-range selection indices", "[serialization]") {
	auto write_dictionary = [&](sel_t index) {
		return [=](Serializer &s) {
			s.WriteProperty(90, "vector_type", VectorType::DICTIONARY_VECTOR);
			vector<sel_t> sel {0, index};
			s.WriteProperty(91, "sel_vector", const_data_ptr_cast(sel.data()), sel.size() * sizeof(sel_t));
			s.WriteProperty<idx_t>(92, "dict_count", 1);
			s.WriteProperty(100, "has_validity_mask", false);
			int32_t value = 42;
			s.WriteProperty(102, "data", const_data_ptr_cast(&value), sizeof(int32_t));
		};
	};
	REQUIRE_NOTHROW(DeserializeVector(LogicalType::INTEGER, 2, write_dictionary(0)));
	REQUIRE_THROWS_AS(DeserializeVector(LogicalType::INTEGER, 2, write_dictionary(1000)), SerializationException);
}

TEST_CASE("Deserialize VARCHAR vectors with string lengths exceeding the string data", "[serialization]") {
	auto write_strings = [&](uint32_t second_length) {
		return [=](Serializer &s) {
			WriteFlatHeader(s);
			s.WriteProperty<optional_idx>(107, "byte_data_length", optional_idx(4));
			vector<uint32_t> lengths {2, second_length};
			s.WriteProperty(108, "length_data", const_data_ptr_cast(lengths.data()), lengths.size() * sizeof(uint32_t));
			s.WriteProperty(109, "byte_data", const_data_ptr_cast("abcd"), 4);
		};
	};
	REQUIRE_NOTHROW(DeserializeVector(LogicalType::VARCHAR, 2, write_strings(2)));
	REQUIRE_THROWS_AS(DeserializeVector(LogicalType::VARCHAR, 2, write_strings(3)), SerializationException);
	REQUIRE_THROWS_AS(DeserializeVector(LogicalType::VARCHAR, 2, write_strings(1000000)), SerializationException);
}

TEST_CASE("Deserialize shredded VARIANT vectors with a mismatched unshredded layout", "[serialization]") {
	auto write_shredded = [&](const LogicalType &child0_type, const LogicalType &child1_type) {
		return [=](Serializer &s) {
			auto shredded_type = LogicalType::STRUCT({{"unshredded", child0_type}, {"shredded", child1_type}});
			s.WriteProperty(90, "vector_type", VectorType::SHREDDED_VECTOR);
			s.WriteProperty(91, "shredded_type", shredded_type);
			// matches the declared child count, so this doesn't trip the (unrelated) child-count check
			s.WriteObject(92, "shredded_data", [&](Serializer &obj) {
				WriteFlatHeader(obj);
				obj.WriteList(103, "children", 2, [&](Serializer::List &list, idx_t i) {
					list.WriteObject([&](Serializer &child_obj) {
						WriteFlatHeader(child_obj);
						int32_t value = 42;
						child_obj.WriteProperty(102, "data", const_data_ptr_cast(&value), sizeof(int32_t));
					});
				});
			});
		};
	};
	// a shredded_type whose first child isn't the canonical VARIANT struct layout is rejected
	REQUIRE_THROWS_AS(
	    DeserializeVector(LogicalType::VARIANT(), 1, write_shredded(LogicalType::INTEGER, LogicalType::INTEGER)),
	    SerializationException);
}

TEST_CASE("Deserialize STRUCT vectors with too many children", "[serialization]") {
	auto type = LogicalType::STRUCT({{"a", LogicalType::INTEGER}});
	auto write_struct = [&](idx_t child_count) {
		return [=](Serializer &s) {
			WriteFlatHeader(s);
			s.WriteList(103, "children", child_count, [&](Serializer::List &list, idx_t i) {
				list.WriteObject([&](Serializer &obj) {
					WriteFlatHeader(obj);
					int32_t value = 42;
					obj.WriteProperty(102, "data", const_data_ptr_cast(&value), sizeof(int32_t));
				});
			});
		};
	};
	REQUIRE_NOTHROW(DeserializeVector(type, 1, write_struct(1)));
	REQUIRE_THROWS_AS(DeserializeVector(type, 1, write_struct(2)), SerializationException);
	REQUIRE_THROWS_AS(DeserializeVector(type, 1, write_struct(0)), SerializationException);
}

TEST_CASE("GetValue on a UNION vector with an out-of-range stored tag", "[serialization]") {
	auto type = LogicalType::UNION({{"a", LogicalType::INTEGER}, {"b", LogicalType::INTEGER}});
	auto write_union = [&](uint8_t tag) {
		return [=](Serializer &s) {
			WriteFlatHeader(s);
			s.WriteList(103, "children", 3, [&](Serializer::List &list, idx_t i) {
				list.WriteObject([&](Serializer &obj) {
					WriteFlatHeader(obj);
					if (i == 0) {
						obj.WriteProperty(102, "data", const_data_ptr_cast(&tag), sizeof(uint8_t));
					} else {
						int32_t value = 42;
						obj.WriteProperty(102, "data", const_data_ptr_cast(&value), sizeof(int32_t));
					}
				});
			});
		};
	};
	// a valid tag (0 or 1, since there are 2 members) materializes without issue
	auto valid = DeserializeVectorReturn(type, 1, write_union(0));
	REQUIRE_NOTHROW(valid->GetValue(0));
	// a stored tag beyond the member count must be rejected, not index past the children array
	auto invalid = DeserializeVectorReturn(type, 1, write_union(5));
	REQUIRE_THROWS_AS(invalid->GetValue(0), DataCorruptionException);
}

} // namespace duckdb
