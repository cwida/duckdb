#include "catch.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"

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
	// parameterized types are not shared
	REQUIRE(&LogicalType::DECIMAL(18, 3).GetTypeInfo() != &LogicalType::DECIMAL(18, 3).GetTypeInfo());
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
