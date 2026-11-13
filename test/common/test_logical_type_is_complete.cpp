#include "catch.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/limits.hpp"

using namespace duckdb;

TEST_CASE("Test LogicalType::IsComplete for DECIMAL", "[is_complete]") {
	SECTION("well-formed decimals are complete") {
		// Regression: every well-formed DECIMAL was wrongly reported as incomplete.
		REQUIRE(LogicalType::DECIMAL(18, 3).IsComplete());
		REQUIRE(LogicalType::DECIMAL(1, 0).IsComplete());
		REQUIRE(LogicalType::DECIMAL(10, 0).IsComplete());
		REQUIRE(LogicalType::DECIMAL(10, 10).IsComplete()); // scale == width
		REQUIRE(LogicalType::DECIMAL(1000, 0).IsComplete());
		REQUIRE(LogicalType::DECIMAL(1000, 1000).IsComplete());
		REQUIRE(LogicalType::DECIMAL(NumericLimits<uint32_t>::Maximum(), 0).IsComplete());
	}

	SECTION("ill-formed decimals are incomplete") {
		// width 0 is not a valid decimal width
		REQUIRE(!LogicalType::DECIMAL(0, 0).IsComplete());
	}

	SECTION("types containing a decimal are complete") {
		// TypeVisitor recurses into children; a well-formed decimal node must not report the whole type incomplete.
		const auto dec = LogicalType::DECIMAL(18, 3);
		REQUIRE(LogicalType::LIST(dec).IsComplete());
		REQUIRE(LogicalType::ARRAY(dec, 4).IsComplete());

		child_list_t<LogicalType> struct_children;
		struct_children.emplace_back("a", dec);
		REQUIRE(LogicalType::STRUCT(struct_children).IsComplete());

		REQUIRE(LogicalType::MAP(LogicalType::VARCHAR, dec).IsComplete());

		child_list_t<LogicalType> nested_children;
		nested_children.emplace_back("a", LogicalType::LIST(dec));
		REQUIRE(LogicalType::STRUCT(nested_children).IsComplete());
	}

	SECTION("unrelated types are unaffected") {
		REQUIRE(LogicalType(LogicalType::INTEGER).IsComplete());
		REQUIRE(LogicalType(LogicalType::VARCHAR).IsComplete());

		child_list_t<LogicalType> plain_children;
		plain_children.emplace_back("a", LogicalType::INTEGER);
		plain_children.emplace_back("b", LogicalType::VARCHAR);
		REQUIRE(LogicalType::STRUCT(plain_children).IsComplete());

		REQUIRE(!LogicalType(LogicalTypeId::INVALID).IsComplete());
		REQUIRE(!LogicalType(LogicalTypeId::UNKNOWN).IsComplete());
		REQUIRE(!LogicalType(LogicalTypeId::ANY).IsComplete());
	}
}
