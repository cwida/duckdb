#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: Arrow. The chunk converters need a Context, which only a
// callback has, so what is reachable from a test is the schema exporter driven
// from inside a scalar function.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

} // namespace

TEST_CASE("Stable C++API: ArrowExporter and ArrowImporter round-trip", "[cpp_api][arrow]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	// Both handles need a Context, which only a callback has, so the round-trip runs inside a scalar function. A
	// callback must not use Catch assertions, so what it observes is latched and checked after the query.
	static idx_t observed_count = 0;
	static std::string observed_names;
	static std::string observed_first_type;
	static int64_t observed_value = 0;
	static int64_t observed_arrays = 0;
	observed_count = 0;
	observed_names.clear();
	observed_first_type.clear();
	observed_value = 0;
	observed_arrays = 0;

	auto function = ScalarFunction::Create(conn.GetFactory());
	function.SetName("cpp_arrow_probe");
	function.SetExecCallback([](ScalarFunction::ExecInput &input) {
		auto &context = input.GetContext();

		std::vector<LogicalType> types;
		types.push_back(context.GetFactory().ParseType("BIGINT"));
		ArrowExporter exporter(context, types, {"a"});

		// The exporter's schema is what an importer resolves, so the two agree by construction.
		ArrowSchema schema {};
		exporter.GetSchema(schema);
		ArrowImporter importer(context, schema);
		schema.release(&schema);

		auto resolved = importer.GetSchema();
		observed_count = resolved.GetFieldCount();
		for (idx_t i = 0; i < observed_count; i++) {
			observed_names += std::string(resolved.GetFieldName(i));
		}
		if (observed_count > 0) {
			observed_first_type = resolved.GetFieldType(0).ToText();
		}

		// Build a one-row chunk, push it out to Arrow and read it straight back.
		DataChunk chunk(context.GetFactory(), types);
		auto column = chunk.GetVector(0);
		column.GetDataMutable<int64_t>()[0] = 4242;
		column.SetSize(1);

		exporter.Append(chunk, /*flush=*/true);
		ArrowArray array {};
		while (exporter.NextArray(array)) {
			observed_arrays++;
			importer.Append(array, /*consume=*/true, /*flush=*/true);
			auto imported = importer.NextChunk();
			if (imported) {
				observed_value = imported.GetVector(0).GetView().Data<int64_t>()[0];
			}
		}

		auto result = input.GetResult();
		auto *out = result.GetDataMutable<int32_t>();
		for (idx_t i = 0; i < input.GetRowCount(); i++) {
			out[i] = 1;
		}
	});
	const auto integer = factory.ParseType("INTEGER");
	function.GetSignature().AddParameter("x", integer).SetReturnType(integer);
	conn.Register(function);

	conn.Execute("SELECT cpp_arrow_probe(1)").Drain();
	REQUIRE(observed_count == 1);
	REQUIRE(observed_names == "a");
	REQUIRE(observed_first_type == "BIGINT");
	REQUIRE(observed_arrays == 1);
	REQUIRE(observed_value == 4242);
}
