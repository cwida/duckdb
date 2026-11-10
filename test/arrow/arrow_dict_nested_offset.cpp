// Arrow scans of dictionary-encoded children of LIST/ARRAY/STRUCT with nonzero child offsets,
// as produced by e.g. pyarrow slicing.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"

#include <string>
#include <vector>

using namespace duckdb;

namespace {

// Stack-allocated Arrow structs need a no-op release.
void ReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void ReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

const int32_t DICT_VALUES[3] = {10, 20, 30};

// Dict-encoded int32 child: slot j holds 10 * (j % 3 + 1), NULL at the given slots.
struct DictChild {
	DictChild(idx_t len, const std::vector<idx_t> &null_slots)
	    // padded so GetValidityMask's realignment branch (reads one extra byte) stays in bounds
	    : indices(len), validity((len + 7) / 8 + 2, 0xFF) {
		for (idx_t j = 0; j < len; j++) {
			indices[j] = int32_t(j % 3);
		}
		for (auto slot : null_slots) {
			validity[slot / 8] &= uint8_t(~(1u << (slot % 8)));
		}
		dict_schema.format = "i";
		dict_schema.release = ReleaseSchema;
		schema.format = "i";
		schema.name = "item";
		schema.flags = 2; // ARROW_FLAG_NULLABLE
		schema.dictionary = &dict_schema;
		schema.release = ReleaseSchema;
		dict_buffers[0] = nullptr;
		dict_buffers[1] = DICT_VALUES;
		dict_array.length = 3;
		dict_array.n_buffers = 2;
		dict_array.buffers = dict_buffers;
		dict_array.release = ReleaseArray;
		buffers[0] = validity.data();
		buffers[1] = indices.data();
		array.length = int64_t(len);
		array.null_count = int64_t(null_slots.size());
		array.n_buffers = 2;
		array.buffers = buffers;
		array.dictionary = &dict_array;
		array.release = ReleaseArray;
	}
	DictChild(const DictChild &) = delete;

	std::vector<int32_t> indices;
	std::vector<uint8_t> validity;
	const void *dict_buffers[2];
	const void *buffers[2];
	ArrowSchema dict_schema {};
	ArrowSchema schema {};
	ArrowArray dict_array {};
	ArrowArray array {};
};

// LIST column with offsets[0] == gap: row r covers child slots gap + r * list_len onward.
struct ListColumn {
	ListColumn(DictChild &child, idx_t rows, idx_t gap, idx_t list_len = 1, bool wrap_struct = false)
	    : offsets(rows + 1) {
		for (idx_t i = 0; i <= rows; i++) {
			offsets[i] = int32_t(gap + i * list_len);
		}
		if (wrap_struct) {
			struct_schema_children[0] = &child.schema;
			struct_schema.format = "+s";
			struct_schema.name = "s";
			struct_schema.flags = 2;
			struct_schema.n_children = 1;
			struct_schema.children = struct_schema_children;
			struct_schema.release = ReleaseSchema;
			struct_array_children[0] = &child.array;
			struct_array.length = child.array.length;
			struct_array.n_buffers = 1;
			struct_array.buffers = struct_buffers;
			struct_array.n_children = 1;
			struct_array.children = struct_array_children;
			struct_array.release = ReleaseArray;
		}
		schema_children[0] = wrap_struct ? &struct_schema : &child.schema;
		schema.format = "+l";
		schema.name = "a";
		schema.flags = 2;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		buffers[0] = nullptr;
		buffers[1] = offsets.data();
		array_children[0] = wrap_struct ? &struct_array : &child.array;
		array.length = int64_t(rows);
		array.n_buffers = 2;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	ListColumn(const ListColumn &) = delete;

	std::vector<int32_t> offsets;
	ArrowSchema *struct_schema_children[1];
	ArrowArray *struct_array_children[1];
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	const void *struct_buffers[1] = {nullptr};
	const void *buffers[2];
	ArrowSchema struct_schema {};
	ArrowArray struct_array {};
	ArrowSchema schema {};
	ArrowArray array {};
};

// ARRAY(k) column: row r element e covers child slot (array_offset + r) * k + e.
struct FixedArrayColumn {
	FixedArrayColumn(DictChild &child, idx_t k, idx_t array_offset, idx_t rows) : fmt("+w:" + std::to_string(k)) {
		schema_children[0] = &child.schema;
		schema.format = fmt.c_str();
		schema.name = "a";
		schema.flags = 2;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		buffers[0] = nullptr;
		array_children[0] = &child.array;
		array.length = int64_t(rows);
		array.offset = int64_t(array_offset);
		array.n_buffers = 1;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	FixedArrayColumn(const FixedArrayColumn &) = delete;

	std::string fmt;
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	const void *buffers[1];
	ArrowSchema schema {};
	ArrowArray array {};
};

// Scans the column through arrow_scan as a one-column batch and compares against the query.
bool ScanMatches(ArrowSchema &col_schema, ArrowArray &col_array, const string &query) {
	ArrowSchema *schema_children[1] = {&col_schema};
	ArrowSchema record_schema {};
	record_schema.format = "+s";
	record_schema.n_children = 1;
	record_schema.children = schema_children;
	record_schema.release = ReleaseSchema;

	ArrowArray *array_children[1] = {&col_array};
	const void *buffers[1] = {nullptr};
	ArrowArray record_array {};
	record_array.length = col_array.length;
	record_array.n_buffers = 1;
	record_array.buffers = buffers;
	record_array.n_children = 1;
	record_array.children = array_children;
	record_array.release = ReleaseArray;

	ArrowArrayStream stream {};
	AdbcError err {};
	if (duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &stream, &err) != ADBC_STATUS_OK) {
		return false;
	}
	DuckDB db(nullptr);
	Connection con(db);
	return ArrowTestHelper::RunArrowComparison(con, query, stream);
}

} // namespace

TEST_CASE("Arrow scan of LIST(dict int32) with nonzero leading child offset", "[arrow]") {
	// offsets[0] = 5: row r reads child slot 5 + r, slots 0-4 are unused
	DictChild child(13, {0, 6, 10});
	ListColumn col(child, 8, 5);
	REQUIRE(ScanMatches(col.schema, col.array,
	                    "SELECT [CASE WHEN r + 5 IN (6, 10) THEN NULL ELSE (10 * ((r + 5) % 3 + 1))::INT END] "
	                    "FROM range(8) t(r)"));
}

TEST_CASE("Arrow scan of LIST(dict int32) across chunks without leading offset - control", "[arrow]") {
	// later chunks start at child slot == chunk_offset: no divergence, passes even with the bug
	DictChild child(4096, {3000});
	ListColumn col(child, 4096, 0);
	REQUIRE(ScanMatches(col.schema, col.array,
	                    "SELECT [CASE WHEN r = 3000 THEN NULL ELSE (10 * (r % 3 + 1))::INT END] "
	                    "FROM range(4096) t(r)"));
}

TEST_CASE("Arrow scan of LIST(dict int32) across chunks with leading offset", "[arrow]") {
	// the second chunk reads child slots from 2055 while chunk_offset is 2048; slot 2055 is row 2048
	DictChild child(7 + 4096, {2055});
	ListColumn col(child, 4096, 7);
	REQUIRE(ScanMatches(col.schema, col.array,
	                    "SELECT [CASE WHEN r = 2048 THEN NULL ELSE (10 * ((r + 7) % 3 + 1))::INT END] "
	                    "FROM range(4096) t(r)"));
}

TEST_CASE("Arrow scan of LIST(dict int32) with more child slots than rows per chunk", "[arrow]") {
	// 6144 child slots in one chunk; slot 2054 is row 684 element 2, slot 6000 is row 2000 element 0
	DictChild child(6144, {2054, 6000});
	ListColumn col(child, 2048, 0, 3);
	REQUIRE(ScanMatches(col.schema, col.array,
	                    "SELECT (CASE WHEN r = 684 THEN [10, 20, NULL] WHEN r = 2000 THEN [NULL, 20, 30] "
	                    "ELSE [10, 20, 30] END)::INT[] FROM range(2048) t(r)"));
}

TEST_CASE("Arrow scan of LIST(STRUCT(dict int32)) with nonzero leading child offset", "[arrow]") {
	DictChild child(13, {0, 6, 10});
	ListColumn col(child, 8, 5, 1, /*wrap_struct=*/true);
	REQUIRE(ScanMatches(col.schema, col.array,
	                    "SELECT [{'item': CASE WHEN r + 5 IN (6, 10) THEN NULL "
	                    "ELSE (10 * ((r + 5) % 3 + 1))::INT END}] FROM range(8) t(r)"));
}

TEST_CASE("Arrow scan of fixed-size ARRAY(dict int32) with nonzero array offset", "[arrow]") {
	// array.offset = 2 shifts the child window by 2 * 3 slots; slot 6 is row 0 element 0
	DictChild child(18, {0, 6});
	FixedArrayColumn col(child, 3, 2, 4);
	REQUIRE(ScanMatches(col.schema, col.array,
	                    "SELECT (CASE WHEN r = 0 THEN [NULL, 20, 30] ELSE [10, 20, 30] END)::INT[3] "
	                    "FROM range(4) t(r)"));
}

TEST_CASE("Arrow scan of LIST(dict string) whose first chunk holds no list elements", "[arrow]") {
	// dictionary ['a'], child [0], and lists [] x 2048 followed by ['a']
	const int32_t dict_offsets[2] = {0, 1};
	const char dict_data[] = "a";
	const void *dict_buffers[3] = {nullptr, dict_offsets, dict_data};
	ArrowSchema dict_schema {};
	dict_schema.format = "u";
	dict_schema.release = ReleaseSchema;
	ArrowArray dict_array {};
	dict_array.length = 1;
	dict_array.n_buffers = 3;
	dict_array.buffers = dict_buffers;
	dict_array.release = ReleaseArray;

	const int32_t indices[1] = {0};
	const void *child_buffers[2] = {nullptr, indices};
	ArrowSchema child_schema {};
	child_schema.format = "i";
	child_schema.name = "item";
	child_schema.flags = 2; // ARROW_FLAG_NULLABLE
	child_schema.dictionary = &dict_schema;
	child_schema.release = ReleaseSchema;
	ArrowArray child_array {};
	child_array.length = 1;
	child_array.n_buffers = 2;
	child_array.buffers = child_buffers;
	child_array.dictionary = &dict_array;
	child_array.release = ReleaseArray;

	std::vector<int32_t> offsets(2049, 0);
	offsets.push_back(1);
	const void *list_buffers[2] = {nullptr, offsets.data()};
	ArrowSchema *list_schema_children[1] = {&child_schema};
	ArrowSchema list_schema {};
	list_schema.format = "+l";
	list_schema.name = "l";
	list_schema.flags = 2;
	list_schema.n_children = 1;
	list_schema.children = list_schema_children;
	list_schema.release = ReleaseSchema;
	ArrowArray *list_array_children[1] = {&child_array};
	ArrowArray list_array {};
	list_array.length = 2049;
	list_array.n_buffers = 2;
	list_array.buffers = list_buffers;
	list_array.n_children = 1;
	list_array.children = list_array_children;
	list_array.release = ReleaseArray;

	REQUIRE(ScanMatches(list_schema, list_array,
	                    "SELECT CASE WHEN r < 2048 THEN [] ELSE ['a'] END::VARCHAR[] FROM range(2049) t(r)"));
}
