#include "test_capi_v2.hpp"

#include <cstring>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// V2 column data collection tests — append, scan, combine, reset, and the
// data_chunk create/copy variants that pair with the zero-copy scan.
//
// Scans hand back borrowed data: a scanned chunk's vectors reference buffers
// pinned by the worker scan state, valid until that worker's next scan or the
// state's destruction. data_chunk_copy is the escape hatch
// and is pinned as such below.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

// Build a single-column INTEGER chunk holding the given values.
duckdb_v2_data_chunk_handle MakeIntChunk(duckdb_v2_factory_handle factory, const std::vector<int32_t> &vals) {
	auto int_type = MakeType(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};
	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(factory, types, 1, &chunk, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, vals.size(), nullptr) == DUCKDB_V2_ERROR_NONE);
	if (!vals.empty()) {
		void *raw = nullptr;
		REQUIRE(duckdb_v2_vector_get_data_mutable(vec, &raw, nullptr) == DUCKDB_V2_ERROR_NONE);
		std::memcpy(raw, vals.data(), vals.size() * sizeof(int32_t));
	}
	return chunk;
}

// A single-column INTEGER collection.
duckdb_v2_column_data_collection_handle MakeIntCollection(duckdb_v2_factory_handle factory) {
	auto int_type = MakeType(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};
	duckdb_v2_column_data_collection_handle cdc = nullptr;
	auto rc = duckdb_v2_column_data_collection_create(factory, types, 1, &cdc, nullptr);
	duckdb_v2_logical_type_destroy(&int_type);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(cdc != nullptr);
	return cdc;
}

// Append the values through a fresh append state.
void AppendInts(duckdb_v2_factory_handle factory, duckdb_v2_column_data_collection_handle cdc,
                const std::vector<int32_t> &vals) {
	auto chunk = MakeIntChunk(factory, vals);
	duckdb_v2_column_data_collection_append_state_handle st = nullptr;
	auto create_rc = duckdb_v2_column_data_collection_append_state_create(cdc, &st, nullptr);
	auto append_rc = create_rc == DUCKDB_V2_ERROR_NONE
	                     ? duckdb_v2_column_data_collection_append(cdc, st, chunk, nullptr)
	                     : create_rc;
	duckdb_v2_column_data_collection_append_state_destroy(&st);
	duckdb_v2_data_chunk_destroy(&chunk);
	REQUIRE(create_rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(append_rc == DUCKDB_V2_ERROR_NONE);
}

idx_t RowCount(duckdb_v2_column_data_collection_handle cdc) {
	idx_t count = 99;
	REQUIRE(duckdb_v2_column_data_collection_row_count(cdc, &count, nullptr) == DUCKDB_V2_ERROR_NONE);
	return count;
}

// Drain a single-column INTEGER collection through shared + worker scan
// states, returning all values in scan order.
std::vector<int32_t> ScanInts(duckdb_v2_factory_handle factory, duckdb_v2_column_data_collection_handle cdc) {
	duckdb_v2_column_data_collection_shared_scan_state_handle shared = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_shared_scan_state_create(cdc, &shared, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_column_data_collection_worker_scan_state_handle worker = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_worker_scan_state_create(cdc, &worker, nullptr) == DUCKDB_V2_ERROR_NONE);

	auto chunk = MakeIntChunk(factory, {});
	std::vector<int32_t> out;
	auto scan_rc = DUCKDB_V2_ERROR_NONE;
	while (true) {
		bool did_produce = false;
		scan_rc = duckdb_v2_column_data_collection_scan(cdc, shared, worker, chunk, &did_produce, nullptr);
		if (scan_rc != DUCKDB_V2_ERROR_NONE || !did_produce) {
			break;
		}
		idx_t size = 0;
		duckdb_v2_data_chunk_get_size(chunk, &size, nullptr);
		duckdb_v2_vector_handle vec = nullptr;
		duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
		duckdb_v2_vector_view view {};
		duckdb_v2_vector_get_view(vec, &view, nullptr);
		for (idx_t i = 0; i < size; i++) {
			out.push_back(static_cast<const int32_t *>(view.data)[SelAt(view.sel, i)]);
		}
	}
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_column_data_collection_worker_scan_state_destroy(&worker);
	duckdb_v2_column_data_collection_shared_scan_state_destroy(&shared);
	REQUIRE(scan_rc == DUCKDB_V2_ERROR_NONE);
	return out;
}

} // namespace

// ===========================================================================
// Round-trip: append two batches, scan them back in order.
// ===========================================================================

TEST_CASE("V2: column_data_collection append + scan round-trip", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto cdc = MakeIntCollection(fx.factory);

	REQUIRE(RowCount(cdc) == 0);
	AppendInts(fx.factory, cdc, {1, 2, 3});
	AppendInts(fx.factory, cdc, {4, 5});
	REQUIRE(RowCount(cdc) == 5);

	auto values = ScanInts(fx.factory, cdc);
	REQUIRE(values == std::vector<int32_t> {1, 2, 3, 4, 5});

	REQUIRE(duckdb_v2_column_data_collection_destroy(&cdc) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(cdc == nullptr);
}

// A completed scan reports did_produce_chunk = false and resets the chunk.
TEST_CASE("V2: column_data_collection scan completion resets the chunk", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto cdc = MakeIntCollection(fx.factory);
	AppendInts(fx.factory, cdc, {7});

	duckdb_v2_column_data_collection_shared_scan_state_handle shared = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_shared_scan_state_create(cdc, &shared, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_column_data_collection_worker_scan_state_handle worker = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_worker_scan_state_create(cdc, &worker, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto chunk = MakeIntChunk(fx.factory, {});

	bool did_produce = false;
	REQUIRE(duckdb_v2_column_data_collection_scan(cdc, shared, worker, chunk, &did_produce, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(did_produce);

	did_produce = true;
	REQUIRE(duckdb_v2_column_data_collection_scan(cdc, shared, worker, chunk, &did_produce, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(did_produce);
	idx_t size = 99;
	REQUIRE(duckdb_v2_data_chunk_get_size(chunk, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 0);

	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_column_data_collection_worker_scan_state_destroy(&worker);
	duckdb_v2_column_data_collection_shared_scan_state_destroy(&shared);
	duckdb_v2_column_data_collection_destroy(&cdc);
}

// More rows than one chunk holds: values survive the chunk boundary in order.
TEST_CASE("V2: column_data_collection multi-chunk scan", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto cdc = MakeIntCollection(fx.factory);

	std::vector<int32_t> full(STANDARD_VECTOR_SIZE);
	for (idx_t i = 0; i < full.size(); i++) {
		full[i] = static_cast<int32_t>(i);
	}
	AppendInts(fx.factory, cdc, full);
	AppendInts(fx.factory, cdc, {-1, -2, -3});
	REQUIRE(RowCount(cdc) == STANDARD_VECTOR_SIZE + 3);

	auto values = ScanInts(fx.factory, cdc);
	auto expected = full;
	expected.insert(expected.end(), {-1, -2, -3});
	REQUIRE(values == expected);

	duckdb_v2_column_data_collection_destroy(&cdc);
}

// ===========================================================================
// VARCHAR round-trip — strings (inlined and heap-backed) survive the copy in
// and the scan out.
// ===========================================================================
#if (STANDARD_VECTOR_SIZE > 2)
TEST_CASE("V2: column_data_collection VARCHAR round-trip", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto varchar_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_logical_type_handle types[1] = {varchar_type};
	duckdb_v2_column_data_collection_handle cdc = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_create(fx.factory, types, 1, &cdc, nullptr) == DUCKDB_V2_ERROR_NONE);

	const std::vector<std::string> strings = {"a", "a string too long for the inline representation", ""};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	REQUIRE(duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&varchar_type);
	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_vector_set_size(vec, strings.size(), nullptr) == DUCKDB_V2_ERROR_NONE);
	for (idx_t i = 0; i < strings.size(); i++) {
		REQUIRE(V2VectorAssignString(vec, i, strings[i].data(), strings[i].size(), nullptr) == DUCKDB_V2_ERROR_NONE);
	}

	duckdb_v2_column_data_collection_append_state_handle st = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_append_state_create(cdc, &st, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_column_data_collection_append(cdc, st, chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_column_data_collection_append_state_destroy(&st);
	duckdb_v2_data_chunk_destroy(&chunk);

	// Scan back into a fresh chunk and compare.
	duckdb_v2_column_data_collection_shared_scan_state_handle shared = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_shared_scan_state_create(cdc, &shared, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_column_data_collection_worker_scan_state_handle worker = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_worker_scan_state_create(cdc, &worker, nullptr) == DUCKDB_V2_ERROR_NONE);

	auto scan_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_logical_type_handle scan_types[1] = {scan_type};
	duckdb_v2_data_chunk_handle scan_chunk = nullptr;
	REQUIRE(duckdb_v2_data_chunk_create(fx.factory, scan_types, 1, &scan_chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&scan_type);

	bool did_produce = false;
	REQUIRE(duckdb_v2_column_data_collection_scan(cdc, shared, worker, scan_chunk, &did_produce, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(did_produce);
	idx_t size = 0;
	REQUIRE(duckdb_v2_data_chunk_get_size(scan_chunk, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == strings.size());

	duckdb_v2_vector_handle scan_vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(scan_chunk, 0, &scan_vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(scan_vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	const auto *data = static_cast<const duckdb_v2_bytes *>(view.data);
	for (idx_t i = 0; i < strings.size(); i++) {
		REQUIRE(Convert(Convert(data[SelAt(view.sel, i)])) == strings[i]);
	}

	duckdb_v2_data_chunk_destroy(&scan_chunk);
	duckdb_v2_column_data_collection_worker_scan_state_destroy(&worker);
	duckdb_v2_column_data_collection_shared_scan_state_destroy(&shared);
	duckdb_v2_column_data_collection_destroy(&cdc);
}
#endif
// ===========================================================================
// Combine
// ===========================================================================

TEST_CASE("V2: column_data_collection combine consumes the source", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto target = MakeIntCollection(fx.factory);
	auto source = MakeIntCollection(fx.factory);
	AppendInts(fx.factory, target, {1, 2, 3});
	AppendInts(fx.factory, source, {4, 5});

	REQUIRE(duckdb_v2_column_data_collection_combine(target, &source, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(source == nullptr);
	REQUIRE(RowCount(target) == 5);
	REQUIRE(ScanInts(fx.factory, target) == std::vector<int32_t> {1, 2, 3, 4, 5});

	duckdb_v2_column_data_collection_destroy(&target);
}

TEST_CASE("V2: column_data_collection combine refusals", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto target = MakeIntCollection(fx.factory);

	// Mismatching types: the source survives the refusal.
	auto bigint_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	duckdb_v2_logical_type_handle types[1] = {bigint_type};
	duckdb_v2_column_data_collection_handle source = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_create(fx.factory, types, 1, &source, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&bigint_type);

	REQUIRE(duckdb_v2_column_data_collection_combine(target, &source, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(source != nullptr);
	duckdb_v2_column_data_collection_destroy(&source);

	// Self-combine is refused rather than corrupting the collection.
	auto self = target;
	REQUIRE(duckdb_v2_column_data_collection_combine(target, &self, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(self != nullptr);

	// Null arguments.
	REQUIRE(duckdb_v2_column_data_collection_combine(nullptr, &self, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_column_data_collection_combine(target, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_column_data_collection_handle null_source = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_combine(target, &null_source, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_column_data_collection_destroy(&target);
}

// ===========================================================================
// Reset
// ===========================================================================

TEST_CASE("V2: column_data_collection reset keeps types, drops rows", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto cdc = MakeIntCollection(fx.factory);
	AppendInts(fx.factory, cdc, {1, 2, 3});
	REQUIRE(RowCount(cdc) == 3);

	REQUIRE(duckdb_v2_column_data_collection_reset(cdc, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RowCount(cdc) == 0);

	// The collection is appendable again through a fresh state.
	AppendInts(fx.factory, cdc, {9, 8});
	REQUIRE(RowCount(cdc) == 2);
	REQUIRE(ScanInts(fx.factory, cdc) == std::vector<int32_t> {9, 8});

	duckdb_v2_column_data_collection_destroy(&cdc);
}

TEST_CASE("V2: column_data_collection clear keeps types, drops rows", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto cdc = MakeIntCollection(fx.factory);
	AppendInts(fx.factory, cdc, {1, 2, 3});
	REQUIRE(RowCount(cdc) == 3);

	// Same observable effect as reset; the difference is that the buffers are retained for the next appends.
	REQUIRE(duckdb_v2_column_data_collection_clear(cdc, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RowCount(cdc) == 0);
	REQUIRE(ScanInts(fx.factory, cdc).empty());

	AppendInts(fx.factory, cdc, {9, 8});
	REQUIRE(RowCount(cdc) == 2);
	REQUIRE(ScanInts(fx.factory, cdc) == std::vector<int32_t> {9, 8});

	// Repeated fill/clear cycles are the point of it.
	for (int round = 0; round < 3; round++) {
		REQUIRE(duckdb_v2_column_data_collection_clear(cdc, nullptr) == DUCKDB_V2_ERROR_NONE);
		AppendInts(fx.factory, cdc, {round});
		REQUIRE(ScanInts(fx.factory, cdc) == std::vector<int32_t> {round});
	}

	duckdb_v2_column_data_collection_destroy(&cdc);
}

TEST_CASE("V2: column_data_collection clear null arg", "[capi_v2][column_data_collection]") {
	REQUIRE(duckdb_v2_column_data_collection_clear(nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
}

// ===========================================================================
// Refusals: create / append / scan
// ===========================================================================

TEST_CASE("V2: column_data_collection create refusals", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[2] = {int_type, nullptr};
	duckdb_v2_column_data_collection_handle cdc = nullptr;

	// Null connection / types / out slot.
	REQUIRE(duckdb_v2_column_data_collection_create(nullptr, types, 1, &cdc, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(cdc == nullptr);
	REQUIRE(duckdb_v2_column_data_collection_create(fx.factory, nullptr, 1, &cdc, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(cdc == nullptr);
	REQUIRE(duckdb_v2_column_data_collection_create(fx.factory, types, 1, nullptr, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	// A collection must have at least one column.
	REQUIRE(duckdb_v2_column_data_collection_create(fx.factory, types, 0, &cdc, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(cdc == nullptr);

	// A null element in the types array.
	REQUIRE(duckdb_v2_column_data_collection_create(fx.factory, types, 2, &cdc, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(cdc == nullptr);

	duckdb_v2_logical_type_destroy(&int_type);
}

TEST_CASE("V2: column_data_collection append refuses mismatching chunks", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto cdc = MakeIntCollection(fx.factory);
	duckdb_v2_column_data_collection_append_state_handle st = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_append_state_create(cdc, &st, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Wrong column count.
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle two_types[2] = {int_type, int_type};
	duckdb_v2_data_chunk_handle two_cols = nullptr;
	REQUIRE(duckdb_v2_data_chunk_create(fx.factory, two_types, 2, &two_cols, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_column_data_collection_append(cdc, st, two_cols, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_data_chunk_destroy(&two_cols);
	duckdb_v2_logical_type_destroy(&int_type);

	// Wrong column type.
	auto double_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE);
	duckdb_v2_logical_type_handle double_types[1] = {double_type};
	duckdb_v2_data_chunk_handle double_chunk = nullptr;
	REQUIRE(duckdb_v2_data_chunk_create(fx.factory, double_types, 1, &double_chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_column_data_collection_append(cdc, st, double_chunk, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_logical_type_destroy(&double_type);

	// Nothing was appended by the refusals.
	REQUIRE(RowCount(cdc) == 0);

	// Null arguments.
	REQUIRE(duckdb_v2_column_data_collection_append(nullptr, st, double_chunk, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_column_data_collection_append(cdc, nullptr, double_chunk, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_column_data_collection_append(cdc, st, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_data_chunk_destroy(&double_chunk);

	duckdb_v2_column_data_collection_append_state_destroy(&st);
	duckdb_v2_column_data_collection_destroy(&cdc);
}

TEST_CASE("V2: column_data_collection scan refuses mismatching chunks", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto cdc = MakeIntCollection(fx.factory);
	AppendInts(fx.factory, cdc, {1});

	duckdb_v2_column_data_collection_shared_scan_state_handle shared = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_shared_scan_state_create(cdc, &shared, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_column_data_collection_worker_scan_state_handle worker = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_worker_scan_state_create(cdc, &worker, nullptr) == DUCKDB_V2_ERROR_NONE);

	auto double_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_DOUBLE);
	duckdb_v2_logical_type_handle types[1] = {double_type};
	duckdb_v2_data_chunk_handle wrong_chunk = nullptr;
	REQUIRE(duckdb_v2_data_chunk_create(fx.factory, types, 1, &wrong_chunk, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&double_type);

	bool did_produce = false;
	REQUIRE(duckdb_v2_column_data_collection_scan(cdc, shared, worker, wrong_chunk, &did_produce, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	// Null arguments.
	REQUIRE(duckdb_v2_column_data_collection_scan(nullptr, shared, worker, wrong_chunk, &did_produce, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_column_data_collection_scan(cdc, nullptr, worker, wrong_chunk, &did_produce, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_column_data_collection_scan(cdc, shared, nullptr, wrong_chunk, &did_produce, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_column_data_collection_scan(cdc, shared, worker, nullptr, &did_produce, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_column_data_collection_scan(cdc, shared, worker, wrong_chunk, nullptr, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_data_chunk_destroy(&wrong_chunk);
	duckdb_v2_column_data_collection_worker_scan_state_destroy(&worker);
	duckdb_v2_column_data_collection_shared_scan_state_destroy(&shared);
	duckdb_v2_column_data_collection_destroy(&cdc);
}

// Destroy is null-safe for the collection and all three state kinds.
TEST_CASE("V2: column_data_collection destroy null-safety", "[capi_v2][column_data_collection]") {
	REQUIRE(duckdb_v2_column_data_collection_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_column_data_collection_handle cdc = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_destroy(&cdc) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_column_data_collection_append_state_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_column_data_collection_append_state_handle append_state = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_append_state_destroy(&append_state) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_column_data_collection_shared_scan_state_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_column_data_collection_shared_scan_state_handle shared = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_shared_scan_state_destroy(&shared) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_column_data_collection_worker_scan_state_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_column_data_collection_worker_scan_state_handle worker = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_worker_scan_state_destroy(&worker) == DUCKDB_V2_ERROR_NONE);
}

// ===========================================================================
// Parallel scan: one shared state, one worker state per thread. Threads
// record into their own slots; all assertions happen after the join.
// ===========================================================================

TEST_CASE("V2: column_data_collection parallel scan", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto cdc = MakeIntCollection(fx.factory);

	constexpr idx_t CHUNKS = 8;
	int64_t expected_sum = 0;
	for (idx_t c = 0; c < CHUNKS; c++) {
		std::vector<int32_t> vals(STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < vals.size(); i++) {
			vals[i] = static_cast<int32_t>(c * STANDARD_VECTOR_SIZE + i);
			expected_sum += vals[i];
		}
		AppendInts(fx.factory, cdc, vals);
	}
	const idx_t total_rows = CHUNKS * STANDARD_VECTOR_SIZE;
	REQUIRE(RowCount(cdc) == total_rows);

	duckdb_v2_column_data_collection_shared_scan_state_handle shared = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_shared_scan_state_create(cdc, &shared, nullptr) == DUCKDB_V2_ERROR_NONE);

	// Per-thread resources are created on the main thread: Catch2 assertions
	// (and the REQUIRE-based helpers) are not thread-safe.
	constexpr idx_t THREADS = 4;
	std::vector<duckdb_v2_column_data_collection_worker_scan_state_handle> states(THREADS, nullptr);
	std::vector<duckdb_v2_data_chunk_handle> chunks(THREADS, nullptr);
	for (idx_t t = 0; t < THREADS; t++) {
		REQUIRE(duckdb_v2_column_data_collection_worker_scan_state_create(cdc, &states[t], nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
		chunks[t] = MakeIntChunk(fx.factory, {});
	}

	std::vector<idx_t> rows_seen(THREADS, 0);
	std::vector<int64_t> sums(THREADS, 0);
	std::vector<DUCKDB_V2_ERROR> codes(THREADS, DUCKDB_V2_ERROR_NONE);

	std::vector<std::thread> workers;
	for (idx_t t = 0; t < THREADS; t++) {
		workers.emplace_back([&, t]() {
			auto rc = DUCKDB_V2_ERROR_NONE;
			while (rc == DUCKDB_V2_ERROR_NONE) {
				bool did_produce = false;
				rc = duckdb_v2_column_data_collection_scan(cdc, shared, states[t], chunks[t], &did_produce, nullptr);
				if (rc != DUCKDB_V2_ERROR_NONE || !did_produce) {
					break;
				}
				idx_t size = 0;
				duckdb_v2_data_chunk_get_size(chunks[t], &size, nullptr);
				duckdb_v2_vector_handle vec = nullptr;
				duckdb_v2_data_chunk_get_vector(chunks[t], 0, &vec, nullptr);
				duckdb_v2_vector_view view {};
				duckdb_v2_vector_get_view(vec, &view, nullptr);
				for (idx_t i = 0; i < size; i++) {
					sums[t] += static_cast<const int32_t *>(view.data)[SelAt(view.sel, i)];
				}
				rows_seen[t] += size;
			}
			codes[t] = rc;
		});
	}
	for (auto &w : workers) {
		w.join();
	}
	for (idx_t t = 0; t < THREADS; t++) {
		duckdb_v2_data_chunk_destroy(&chunks[t]);
		duckdb_v2_column_data_collection_worker_scan_state_destroy(&states[t]);
	}
	duckdb_v2_column_data_collection_shared_scan_state_destroy(&shared);

	idx_t total_seen = 0;
	int64_t total_sum = 0;
	for (idx_t t = 0; t < THREADS; t++) {
		REQUIRE(codes[t] == DUCKDB_V2_ERROR_NONE);
		total_seen += rows_seen[t];
		total_sum += sums[t];
	}
	REQUIRE(total_seen == total_rows);
	REQUIRE(total_sum == expected_sum);

	duckdb_v2_column_data_collection_destroy(&cdc);
}

// ===========================================================================
// data_chunk create/copy variants
// ===========================================================================

TEST_CASE("V2: data_chunk_create through a connection's context", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto int_type = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_logical_type_handle types[1] = {int_type};

	duckdb_v2_data_chunk_handle chunk = nullptr;
	auto rc = duckdb_v2_data_chunk_create(fx.factory, types, 1, &chunk, nullptr);
	REQUIRE(rc == DUCKDB_V2_ERROR_NONE);
	REQUIRE(chunk != nullptr);
	idx_t vec_count = 0;
	REQUIRE(duckdb_v2_data_chunk_get_vector_count(chunk, &vec_count, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(vec_count == 1);
	duckdb_v2_data_chunk_destroy(&chunk);

	// Null context.
	REQUIRE(duckdb_v2_data_chunk_create(nullptr, types, 1, &chunk, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(chunk == nullptr);

	duckdb_v2_logical_type_destroy(&int_type);
}

// The copy is deep: it keeps its values after everything that produced it —
// scan states, the collection, and the source chunk — is destroyed. This
// pins the documented escape hatch for the zero-copy scan.
#if (STANDARD_VECTOR_SIZE > 2)
TEST_CASE("V2: data_chunk_copy outlives the scan", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto cdc = MakeIntCollection(fx.factory);
	AppendInts(fx.factory, cdc, {10, 20, 30});

	duckdb_v2_column_data_collection_shared_scan_state_handle shared = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_shared_scan_state_create(cdc, &shared, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_column_data_collection_worker_scan_state_handle worker = nullptr;
	REQUIRE(duckdb_v2_column_data_collection_worker_scan_state_create(cdc, &worker, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto chunk = MakeIntChunk(fx.factory, {});

	bool did_produce = false;
	REQUIRE(duckdb_v2_column_data_collection_scan(cdc, shared, worker, chunk, &did_produce, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(did_produce);

	duckdb_v2_data_chunk_handle copy = nullptr;
	REQUIRE(duckdb_v2_data_chunk_copy(fx.factory, chunk, &copy, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(copy != nullptr);

	// Tear down everything the scanned chunk borrowed from.
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_column_data_collection_worker_scan_state_destroy(&worker);
	duckdb_v2_column_data_collection_shared_scan_state_destroy(&shared);
	duckdb_v2_column_data_collection_destroy(&cdc);

	idx_t size = 0;
	REQUIRE(duckdb_v2_data_chunk_get_size(copy, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 3);
	duckdb_v2_vector_handle vec = nullptr;
	REQUIRE(duckdb_v2_data_chunk_get_vector(copy, 0, &vec, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_vector_view view {};
	REQUIRE(duckdb_v2_vector_get_view(vec, &view, nullptr) == DUCKDB_V2_ERROR_NONE);
	const auto *data = static_cast<const int32_t *>(view.data);
	REQUIRE(data[SelAt(view.sel, 0)] == 10);
	REQUIRE(data[SelAt(view.sel, 1)] == 20);
	REQUIRE(data[SelAt(view.sel, 2)] == 30);

	duckdb_v2_data_chunk_destroy(&copy);
}
#endif

TEST_CASE("V2: data_chunk_copy refusals and empty copy", "[capi_v2][column_data_collection]") {
	EnvFixture fx;
	auto chunk = MakeIntChunk(fx.factory, {});

	// An empty chunk copies to an empty chunk.
	duckdb_v2_data_chunk_handle copy = nullptr;
	REQUIRE(duckdb_v2_data_chunk_copy(fx.factory, chunk, &copy, nullptr) == DUCKDB_V2_ERROR_NONE);
	idx_t size = 99;
	REQUIRE(duckdb_v2_data_chunk_get_size(copy, &size, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(size == 0);
	duckdb_v2_data_chunk_destroy(&copy);

	// Null arguments.
	REQUIRE(duckdb_v2_data_chunk_copy(nullptr, chunk, &copy, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(copy == nullptr);
	REQUIRE(duckdb_v2_data_chunk_copy(fx.factory, nullptr, &copy, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(copy == nullptr);
	REQUIRE(duckdb_v2_data_chunk_copy(fx.factory, chunk, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_data_chunk_destroy(&chunk);
}

} // namespace test_capi_v2
