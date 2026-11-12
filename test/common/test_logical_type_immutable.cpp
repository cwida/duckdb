#include "catch.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"

#include <thread>

using namespace duckdb;

TEST_CASE("Test that LogicalType::WithAlias does not modify shared type info", "[logical_type_immutable]") {
	SECTION("nested types share their extra type info") {
		auto base = LogicalType::LIST(LogicalType::INTEGER);
		auto shared = base;

		auto aliased = base.WithAlias("MY_LIST");
		REQUIRE(!base.HasAlias());
		REQUIRE(!shared.HasAlias());
		REQUIRE(aliased.GetAlias() == "MY_LIST");
		REQUIRE(aliased.InternalType() == base.InternalType());
		REQUIRE(ListType::GetChildType(aliased) == LogicalType::INTEGER);
	}

	SECTION("re-aliasing does not modify the previous alias") {
		auto first = LogicalType::LIST(LogicalType::INTEGER).WithAlias("FIRST");
		auto second = first.WithAlias("SECOND");
		REQUIRE(first.GetAlias() == "FIRST");
		REQUIRE(second.GetAlias() == "SECOND");
	}

	SECTION("enums are unshared and keep a working dictionary") {
		Vector values(LogicalType::VARCHAR, 2);
		auto data = FlatVector::GetDataMutable<string_t>(values);
		data[0] = StringVector::AddString(values, "a");
		data[1] = StringVector::AddString(values, "b");

		auto base = LogicalType::ENUM(values, 2);
		auto shared = base;
		auto aliased = base.WithAlias("MOOD");

		REQUIRE(!base.HasAlias());
		REQUIRE(!shared.HasAlias());
		REQUIRE(aliased.GetAlias() == "MOOD");
		REQUIRE(EnumType::GetSize(aliased) == 2);
		REQUIRE(EnumType::GetPos(aliased, string_t("b")) == 1);
		REQUIRE(EnumType::GetPos(aliased, string_t("c")) == -1);
	}

	SECTION("an empty alias does not allocate extra type info") {
		auto type = LogicalType(LogicalTypeId::INTEGER);
		REQUIRE(&type.WithAlias("").GetTypeInfo() == &type.GetTypeInfo());
		REQUIRE(type.WithAlias("") == type);
		// a generic type info with an empty alias compares equal to a type without type info
		REQUIRE(type.WithAlias("x").WithAlias("") == type);
	}
}

TEST_CASE("Test that LogicalType::WithExtensionInfo does not modify shared type info", "[logical_type_immutable]") {
	auto base = LogicalType::LIST(LogicalType::INTEGER);
	auto shared = base;

	auto info = make_uniq<ExtensionTypeInfo>();
	info->modifiers.emplace_back(Value::INTEGER(42));
	auto extended = base.WithExtensionInfo(std::move(info));

	REQUIRE(!base.HasExtensionInfo());
	REQUIRE(!shared.HasExtensionInfo());
	REQUIRE(extended.HasExtensionInfo());
	REQUIRE(extended.GetExtensionInfo()->modifiers[0].value.GetValue<int32_t>() == 42);
}

TEST_CASE("Test that types without parameters share one type info", "[logical_type_immutable]") {
	LogicalType a = LogicalType::INTEGER;
	LogicalType b(LogicalTypeId::INTEGER);
	REQUIRE(&a.GetTypeInfo() == &b.GetTypeInfo());
	REQUIRE(a.GetTypeInfo().type == LogicalTypeInfoType::INVALID_TYPE_INFO);
	REQUIRE(a.InternalType() == PhysicalType::INT32);
	REQUIRE(&LogicalType::JSON().GetTypeInfo() == &LogicalType::JSON().GetTypeInfo());
	REQUIRE(&LogicalType::VARIANT().GetTypeInfo() == &LogicalType::VARIANT().GetTypeInfo());
	// Plain decimals share metadata; nested types still have separate top-level infos.
	REQUIRE(&LogicalType::DECIMAL(18, 3).GetTypeInfo() == &LogicalType::DECIMAL(18, 3).GetTypeInfo());
	REQUIRE(&LogicalType::LIST(LogicalType::INTEGER).GetTypeInfo() !=
	        &LogicalType::LIST(LogicalType::INTEGER).GetTypeInfo());
	REQUIRE(LogicalType::DECIMAL(18, 3) == LogicalType::DECIMAL(18, 3));
}

TEST_CASE("Test that a moved-from type keeps its id", "[logical_type_immutable]") {
	auto list = LogicalType::LIST(LogicalType::VARCHAR);
	auto moved = std::move(list);
	REQUIRE(moved == LogicalType::LIST(LogicalType::VARCHAR));
	REQUIRE(list.id() == LogicalTypeId::LIST); // NOLINT: intentionally inspecting a moved-from type
	REQUIRE(!list.HasParameters());            // NOLINT
}

TEST_CASE("Test LogicalType::HasParameters", "[logical_type_immutable]") {
	REQUIRE(!LogicalType(LogicalTypeId::LIST).HasParameters());
	REQUIRE(!LogicalType(LogicalTypeId::DECIMAL).HasParameters());
	REQUIRE(!LogicalType(LogicalTypeId::STRUCT).HasParameters());
	REQUIRE(LogicalType::LIST(LogicalType::ANY).HasParameters());
	REQUIRE(LogicalType::DECIMAL(4, 1).HasParameters());
	REQUIRE(!LogicalType(LogicalType::INTEGER).HasParameters());
	// an alias does not parameterize a type
	auto aliased_list = LogicalType(LogicalTypeId::LIST).WithAlias("L");
	REQUIRE(!aliased_list.HasParameters());
	REQUIRE(aliased_list.ToString() == "L");
	REQUIRE(LogicalType(LogicalTypeId::DECIMAL).WithAlias("D").InternalType() == PhysicalType::INVALID);
}

TEST_CASE("Test that type equality is symmetric", "[logical_type_immutable]") {
	auto bare_list = LogicalType(LogicalTypeId::LIST);
	auto generic_list = bare_list.WithExtensionInfo(nullptr);
	auto int_list = LogicalType::LIST(LogicalType::INTEGER);
	REQUIRE(bare_list == generic_list);
	REQUIRE(generic_list == bare_list);
	REQUIRE(generic_list != int_list);
	REQUIRE(int_list != generic_list);
	// collations do not affect equality
	REQUIRE(LogicalType::VARCHAR_COLLATION("nocase") == LogicalType::VARCHAR);
	REQUIRE(LogicalType(LogicalType::VARCHAR) == LogicalType::VARCHAR_COLLATION("nocase"));
}

static LogicalType RoundtripCachedDecimalType(const LogicalType &type) {
	Allocator allocator;
	MemoryStream stream(allocator);
	BinarySerializer::Serialize(type, stream);
	stream.Rewind();
	BinaryDeserializer deserializer(stream);
	deserializer.Begin();
	auto result = LogicalType::Deserialize(deserializer);
	deserializer.End();
	return result;
}

TEST_CASE("Plain decimal types share metadata across construction and deserialization", "[logical_type_immutable]") {
	for (uint32_t width = 1; width <= Decimal::MAX_WIDTH_DECIMAL; width++) {
		for (uint32_t scale = 0; scale <= width; scale++) {
			auto type = LogicalType::DECIMAL(width, scale);
			auto same = LogicalType::DECIMAL(width, scale);
			auto deserialized = RoundtripCachedDecimalType(type);
			REQUIRE(&type.GetTypeInfo() == &same.GetTypeInfo());
			REQUIRE(&type.GetTypeInfo() == &deserialized.GetTypeInfo());
			REQUIRE(DecimalType::GetWidth(type) == width);
			REQUIRE(DecimalType::GetScale(type) == scale);
			auto expected = width <= 4    ? PhysicalType::INT16
			                : width <= 9  ? PhysicalType::INT32
			                : width <= 18 ? PhysicalType::INT64
			                              : PhysicalType::INT128;
			REQUIRE(type.InternalType() == expected);
		}
	}
}

TEST_CASE("Cached decimal metadata preserves aliases extensions and explicit copies", "[logical_type_immutable]") {
	auto plain = LogicalType::DECIMAL(18, 3);
	auto aliased = plain.WithAlias("MONEY");
	auto aliased_roundtrip = RoundtripCachedDecimalType(aliased);
	REQUIRE(&plain.GetTypeInfo() != &aliased.GetTypeInfo());
	REQUIRE(aliased_roundtrip.GetAlias() == "MONEY");
	REQUIRE(DecimalType::GetWidth(aliased_roundtrip) == 18);
	REQUIRE(DecimalType::GetScale(aliased_roundtrip) == 3);
	REQUIRE_FALSE(plain.HasAlias());

	auto info = make_uniq<ExtensionTypeInfo>();
	info->modifiers.emplace_back(Value::INTEGER(42));
	auto extended = plain.WithExtensionInfo(std::move(info));
	auto extended_roundtrip = RoundtripCachedDecimalType(extended);
	REQUIRE_FALSE(plain.HasExtensionInfo());
	REQUIRE(extended_roundtrip.HasExtensionInfo());
	REQUIRE(extended_roundtrip.GetExtensionInfo()->modifiers[0].value.GetValue<int32_t>() == 42);
	REQUIRE(&plain.GetTypeInfo() != &extended.GetTypeInfo());

	auto copied = plain.Copy();
	REQUIRE(copied == plain);
	REQUIRE(&copied.GetTypeInfo() == &plain.GetTypeInfo());
	auto aliased_copy = aliased.Copy();
	REQUIRE(aliased_copy == aliased);
	REQUIRE(&aliased_copy.GetTypeInfo() != &aliased.GetTypeInfo());
	auto extended_copy = extended.Copy();
	REQUIRE(extended_copy == extended);
	REQUIRE(&extended_copy.GetTypeInfo() != &extended.GetTypeInfo());
	REQUIRE(&plain.GetTypeInfo() == &LogicalType::DECIMAL(18, 3).GetTypeInfo());
}

TEST_CASE("Decimal cache preserves validation and unparameterized types", "[logical_type_immutable]") {
	LogicalType bare(LogicalTypeId::DECIMAL);
	REQUIRE_FALSE(bare.HasParameters());
	REQUIRE(bare.InternalType() == PhysicalType::INVALID);
	auto invalid = LogicalType::DECIMAL(0, 0);
	REQUIRE(DecimalType::GetWidth(invalid) == 0);
	REQUIRE_THROWS(RoundtripCachedDecimalType(invalid));
	REQUIRE_FALSE(Decimal::IsValidWidthScale(39, 0));
#ifndef DUCKDB_CRASH_ON_ASSERT
	// InternalException aborts instead of throwing in crash-on-assert builds.
	REQUIRE_THROWS(LogicalType::DECIMAL(39, 0));
#endif
}

namespace {

struct ReleaseTypeInfo : LogicalTypeInfo {
	ReleaseTypeInfo(array<idx_t, 8> &writes_p, idx_t &destroyed_p, bool &visible_p)
	    : LogicalTypeInfo(LogicalTypeInfoType::GENERIC_TYPE_INFO), writes(writes_p), destroyed(destroyed_p),
	      visible(visible_p) {
	}

	~ReleaseTypeInfo() override {
		visible = true;
		for (idx_t i = 0; i < writes.size(); i++) {
			visible = visible && writes[i] == i + 1;
		}
		destroyed++;
	}

	array<idx_t, 8> &writes;
	idx_t &destroyed;
	bool &visible;
};

} // namespace

TEST_CASE("Final LogicalType release observes prior owners", "[logical_type_immutable]") {
	array<idx_t, 8> writes {};
	idx_t destroyed = 0;
	bool visible = false;
	atomic<bool> start {false};
	LogicalType type(LogicalTypeId::INTEGER, make_uniq<ReleaseTypeInfo>(writes, destroyed, visible));
	duckdb::vector<std::thread> workers;
	for (idx_t i = 0; i < writes.size(); i++) {
		workers.emplace_back([type, &start, &writes, i]() mutable {
			while (!start.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
			writes[i] = i + 1;
			type = LogicalType::INTEGER;
		});
	}
	type = LogicalType::INTEGER;
	start.store(true, std::memory_order_release);
	for (auto &worker : workers) {
		worker.join();
	}
	REQUIRE(destroyed == 1);
	REQUIRE(visible);
}

TEST_CASE("DataChunk preserves immortal metadata and detaches dynamic metadata", "[logical_type_immutable]") {
	auto decimal = LogicalType::DECIMAL(18, 3);
	auto aliased = decimal.WithAlias("MONEY");
	auto nested = LogicalType::LIST(decimal);
	Allocator allocator;
	DataChunk chunk;
	chunk.Initialize(allocator, {decimal, aliased, nested, LogicalType::JSON(), LogicalType::VARIANT()});
	REQUIRE(&chunk.data[0].GetType().GetTypeInfo() == &decimal.GetTypeInfo());
	REQUIRE(chunk.data[1].GetType() == aliased);
	REQUIRE(&chunk.data[1].GetType().GetTypeInfo() != &aliased.GetTypeInfo());
	REQUIRE(chunk.data[2].GetType() == nested);
	REQUIRE(&chunk.data[2].GetType().GetTypeInfo() != &nested.GetTypeInfo());
	REQUIRE(&ListType::GetChildType(chunk.data[2].GetType()).GetTypeInfo() == &decimal.GetTypeInfo());
	REQUIRE(&chunk.data[3].GetType().GetTypeInfo() == &LogicalType::JSON().GetTypeInfo());
	REQUIRE(&chunk.data[4].GetType().GetTypeInfo() == &LogicalType::VARIANT().GetTypeInfo());

	auto re_aliased = chunk.data[0].GetType().WithAlias("OTHER");
	REQUIRE(re_aliased.GetAlias() == "OTHER");
	REQUIRE_FALSE(decimal.HasAlias());
	REQUIRE_FALSE(chunk.data[0].GetType().HasAlias());
}
