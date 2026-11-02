#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: ColumnDataCollection, and the connection-scoped
// DataChunk constructor and Copy that pair with its zero-copy scan.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

// Build a single-column INTEGER chunk holding the given values.
DataChunk MakeIntChunk(Connection &conn, const std::vector<int32_t> &vals) {
	auto &factory = conn.GetFactory();
	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));
	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);
	vec.SetSize(vals.size());
	auto *data = vec.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < vals.size(); i++) {
		data[i] = vals[i];
	}
	return chunk;
}

// Drain a single-column INTEGER collection through one shared + worker state.
std::vector<int32_t> ScanInts(Connection &conn, const ColumnDataCollection &collection) {
	auto shared = collection.CreateSharedScanState();
	auto worker = collection.CreateWorkerScanState();
	auto chunk = MakeIntChunk(conn, {});

	std::vector<int32_t> out;
	while (collection.Scan(shared, worker, chunk)) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			REQUIRE(view.IsValid(i));
			out.push_back(view.Data<int32_t>()[view.SelAt(i)]);
		}
	}
	return out;
}

} // namespace

TEST_CASE("Stable C++API: ColumnDataCollection round-trip", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));
	ColumnDataCollection collection(factory, types);
	REQUIRE(collection.GetRowCount() == 0);

	// Append twice through one state, then once through the one-shot form.
	auto state = collection.CreateAppendState();
	collection.Append(state, MakeIntChunk(conn, {1, 2, 3}));
	collection.Append(state, MakeIntChunk(conn, {4}));
	collection.Append(MakeIntChunk(conn, {5, 6}));
	REQUIRE(collection.GetRowCount() == 6);

	REQUIRE(ScanInts(conn, collection) == std::vector<int32_t> {1, 2, 3, 4, 5, 6});
}

TEST_CASE("Stable C++API: ColumnDataCollection combine consumes the source", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));
	ColumnDataCollection target(factory, types);
	ColumnDataCollection source(factory, types);
	target.Append(MakeIntChunk(conn, {1, 2}));
	source.Append(MakeIntChunk(conn, {3}));

	target.Combine(std::move(source));
	REQUIRE_FALSE(source); // NOLINT: pinning the moved-from state
	REQUIRE(target.GetRowCount() == 3);
	REQUIRE(ScanInts(conn, target) == std::vector<int32_t> {1, 2, 3});

	// A refused merge throws and leaves the source alive.
	std::vector<LogicalType> other_types;
	other_types.push_back(factory.ParseType("BIGINT"));
	ColumnDataCollection mismatched(factory, other_types);
	REQUIRE_THROWS_MATCHES(target.Combine(std::move(mismatched)), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE(mismatched); // NOLINT: pinning the not-consumed state
}

TEST_CASE("Stable C++API: ColumnDataCollection reset", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));
	ColumnDataCollection collection(factory, types);
	collection.Append(MakeIntChunk(conn, {1, 2, 3}));
	REQUIRE(collection.GetRowCount() == 3);

	collection.Reset();
	REQUIRE(collection.GetRowCount() == 0);

	// Appendable again through a fresh state.
	collection.Append(MakeIntChunk(conn, {9}));
	REQUIRE(ScanInts(conn, collection) == std::vector<int32_t> {9});
}

TEST_CASE("Stable C++API: ColumnDataCollection clear", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));
	ColumnDataCollection collection(factory, types);

	// Same observable effect as Reset; the difference is that the buffers survive for the next appends.
	for (int32_t round = 0; round < 3; round++) {
		collection.Append(MakeIntChunk(conn, {round, round}));
		REQUIRE(collection.GetRowCount() == 2);
		collection.Clear();
		REQUIRE(collection.GetRowCount() == 0);
		REQUIRE(ScanInts(conn, collection).empty());
	}

	collection.Append(MakeIntChunk(conn, {7}));
	REQUIRE(ScanInts(conn, collection) == std::vector<int32_t> {7});
}

TEST_CASE("Stable C++API: ColumnDataCollection scan refuses a mismatching chunk", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));
	ColumnDataCollection collection(factory, types);
	collection.Append(MakeIntChunk(conn, {1}));

	auto shared = collection.CreateSharedScanState();
	auto worker = collection.CreateWorkerScanState();
	std::vector<LogicalType> wrong_types;
	wrong_types.push_back(factory.ParseType("DOUBLE"));
	DataChunk wrong_chunk(factory, wrong_types);
	REQUIRE_THROWS_MATCHES(collection.Scan(shared, worker, wrong_chunk), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}
TEST_CASE("Stable C++API: DataChunk::Copy outlives the scan", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("INTEGER"));
	ColumnDataCollection collection(factory, types);
	collection.Append(MakeIntChunk(conn, {10}));

	// Scan one chunk, copy it, then tear down everything it borrowed from.
	auto copy = [&]() {
		auto shared = collection.CreateSharedScanState();
		auto worker = collection.CreateWorkerScanState();
		auto chunk = MakeIntChunk(conn, {});
		REQUIRE(collection.Scan(shared, worker, chunk));
		return chunk.Copy(factory);
	}();
	collection.Reset();

	REQUIRE(copy.GetRowCount() == 1);
	auto view = copy.GetVector(0).GetView();
	REQUIRE(view.Data<int32_t>()[view.SelAt(0)] == 10);
}
