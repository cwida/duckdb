#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"

using namespace duckdb;

namespace {

bool VariantCustomVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &) {
	auto writer = FlatVector::Writer<string_t>(result, count);
	auto validity = source.Validity();
	for (idx_t i = 0; i < count; i++) {
		if (validity.IsValid(i)) {
			writer.WriteValue(string_t("custom"));
		} else {
			writer.WriteNull();
		}
	}
	return true;
}

} // namespace

TEST_CASE("VARIANT serialization respects registered scalar casts", "[variant][variant_cast]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &casts = CastFunctionSet::Get(*con.context);
	casts.RegisterCastFunction(LogicalType::INTEGER, LogicalType::VARCHAR, BoundCastInfo(VariantCustomVarcharCast));

	duckdb::vector<pair<string, string>> cases {
	    {"SELECT 42::INTEGER::VARIANT::VARCHAR", "custom"},
	    {"SELECT {'n': 42::INTEGER}::VARIANT::VARCHAR", "{'n': custom}"},
	    {"SELECT [42::INTEGER]::VARIANT::VARCHAR", "[custom]"},
	    {"SELECT {'n': 42::INTEGER, 'b': 'a'::BLOB}::VARIANT::VARCHAR", "{'b': a, 'n': custom}"}};
	for (auto &entry : cases) {
		auto result = con.Query(entry.first);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(CHECK_COLUMN(result, 0, {entry.second}));
	}

	auto result = con.Query("SELECT count(*) FROM "
	                        "(SELECT i::INTEGER::VARIANT AS v FROM range(10000) t(i) WHERE i % 2 = 1) "
	                        "WHERE v::VARCHAR = 'custom'");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {5000}));
}

TEST_CASE("Prepared VARIANT serialization sees later cast registrations", "[variant][variant_cast]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE variants AS SELECT 42::INTEGER::VARIANT v"));
	auto prepared = con.Prepare("SELECT v::VARCHAR FROM variants");
	REQUIRE_FALSE(prepared->HasError());
	auto before = prepared->Execute();
	REQUIRE_NO_FAIL(*before);
	REQUIRE(CHECK_COLUMN(before, 0, {"42"}));

	auto &casts = CastFunctionSet::Get(*con.context);
	casts.RegisterCastFunction(LogicalType::INTEGER, LogicalType::VARCHAR, BoundCastInfo(VariantCustomVarcharCast));
	auto after = prepared->Execute();
	REQUIRE_NO_FAIL(*after);
	REQUIRE(CHECK_COLUMN(after, 0, {"custom"}));
}

TEST_CASE("VARIANT serialization respects parameterized cast registrations", "[variant][variant_cast]") {
	struct CastCase {
		LogicalType type;
		string expression;
	};
	for (auto &entry : {CastCase {LogicalType::STRUCT({{"n", LogicalType::INTEGER}}), "{'n': 42::INTEGER}"},
	                    CastCase {LogicalType::STRUCT({{"any", LogicalType::ANY}}), "{'n': 42::INTEGER}"},
	                    CastCase {LogicalType::LIST(LogicalType::INTEGER), "[42::INTEGER]"},
	                    CastCase {LogicalType::LIST(LogicalType::ANY), "[42::INTEGER]"},
	                    CastCase {LogicalType::DECIMAL(9, 2), "42::DECIMAL(9, 2)"},
	                    CastCase {LogicalType(LogicalTypeId::DECIMAL), "42::DECIMAL(9, 2)"}}) {
		INFO(entry.type.ToString());
		DuckDB db(nullptr);
		Connection con(db);
		auto &casts = CastFunctionSet::Get(*con.context);
		casts.RegisterCastFunction(entry.type, LogicalType::VARCHAR, BoundCastInfo(VariantCustomVarcharCast));
		auto result = con.Query("SELECT (" + entry.expression + ")::VARIANT::VARCHAR");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(CHECK_COLUMN(result, 0, {"custom"}));
		result = con.Query("SELECT {'outer': " + entry.expression + "}::VARIANT::VARCHAR");
		REQUIRE_NO_FAIL(*result);
		auto expected = entry.type == LogicalType::STRUCT({{"any", LogicalType::ANY}}) ? "custom" : "{'outer': custom}";
		REQUIRE(CHECK_COLUMN(result, 0, {expected}));
	}
}

TEST_CASE("VARIANT serialization preserves registered cast failures", "[variant][variant_cast]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &casts = CastFunctionSet::Get(*con.context);
	casts.RegisterCastFunction(LogicalType::INTEGER, LogicalType::VARCHAR,
	                           BoundCastInfo(DefaultCasts::TryVectorNullCast));
	auto result = con.Query("SELECT 42::INTEGER::VARIANT::VARCHAR");
	REQUIRE(result->HasError());
	result = con.Query("SELECT TRY_CAST(42::INTEGER::VARIANT AS VARCHAR)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {Value()}));
}

TEST_CASE("VARIANT serialization respects casts to aliased VARCHAR", "[variant][variant_cast]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto target = LogicalType(LogicalType::VARCHAR).WithAlias("custom_text");
	auto &casts = CastFunctionSet::Get(*con.context);
	casts.RegisterCastFunction(LogicalType::INTEGER, target, BoundCastInfo(VariantCustomVarcharCast));
	auto variant = Value::INTEGER(42).DefaultCastAs(LogicalType::VARIANT());
	auto result = variant.TryCastAs(*con.context, target);
	REQUIRE(result.has_value());
	REQUIRE(StringValue::Get(*result) == "custom");
}

TEST_CASE("Registered cast source lookup distinguishes aliases and includes wildcards", "[variant][variant_cast]") {
	CastFunctionSet casts;
	casts.RegisterCastFunction(LogicalType::INTEGER, LogicalType::JSON(), BoundCastInfo(VariantCustomVarcharCast));
	REQUIRE(casts.GetRegisteredCastSourceTypes(LogicalType::VARCHAR).empty());
	REQUIRE(casts.GetRegisteredCastSourceTypes(LogicalType::JSON()).size() == 1);
	auto source = LogicalType::STRUCT({{"n", LogicalType::INTEGER}});
	casts.RegisterCastFunction(source, LogicalType::VARCHAR, BoundCastInfo(VariantCustomVarcharCast));
	auto sources = casts.GetRegisteredCastSourceTypes(LogicalType::VARCHAR);
	REQUIRE(sources.size() == 1);
	REQUIRE(sources[0] == source);
	casts.RegisterCastFunction(LogicalType::BIGINT, LogicalType::ANY, BoundCastInfo(VariantCustomVarcharCast));
	REQUIRE(casts.GetRegisteredCastSourceTypes(LogicalType::VARCHAR).size() == 2);
	casts.RegisterCastFunction(LogicalType::ANY, LogicalType::VARCHAR, BoundCastInfo(VariantCustomVarcharCast));
	REQUIRE(casts.GetRegisteredCastSourceTypes(LogicalType::VARCHAR).size() == 3);
}

TEST_CASE("String results reject oversized lengths before reading the buffer", "[variant][variant_cast]") {
	Vector result(LogicalType::VARCHAR, 1);
	// Serialized text keeps its idx_t length until the string heap checks it.
	REQUIRE_THROWS_WITH(StringVector::GetStringHeap(result).AddBlob("", string_t::MAX_STRING_SIZE + 1),
	                    Catch::Matchers::Contains("maximum supported string size"));
}
