#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"
#include "duckdb/common/enums/debug_verification_mode.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/relation.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

namespace {

// Stack-allocated structs need a no-op release.
void NoopReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void NoopReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

// STRUCT(i INTEGER, s VARCHAR) column with int8 indices; the dictionary holds three values and a NULL value, row 3
// points at the NULL value and row 4 has a NULL index.
struct StructDictionaryColumn {
	StructDictionaryColumn() {
		i_schema.format = "i";
		i_schema.name = "i";
		i_schema.flags = 2; // ARROW_FLAG_NULLABLE
		i_schema.release = NoopReleaseSchema;
		s_schema.format = "u";
		s_schema.name = "s";
		s_schema.flags = 2;
		s_schema.release = NoopReleaseSchema;
		value_schema_children[0] = &i_schema;
		value_schema_children[1] = &s_schema;
		value_schema.format = "+s";
		value_schema.flags = 2;
		value_schema.n_children = 2;
		value_schema.children = value_schema_children;
		value_schema.release = NoopReleaseSchema;
		schema.format = "c";
		schema.name = "d";
		schema.flags = 2;
		schema.dictionary = &value_schema;
		schema.release = NoopReleaseSchema;

		i_array.length = 4;
		i_array.n_buffers = 2;
		i_array.buffers = i_buffers;
		i_array.release = NoopReleaseArray;
		s_array.length = 4;
		s_array.n_buffers = 3;
		s_array.buffers = s_buffers;
		s_array.release = NoopReleaseArray;
		value_array_children[0] = &i_array;
		value_array_children[1] = &s_array;
		value_array.length = 4;
		value_array.null_count = 1;
		value_array.n_buffers = 1;
		value_array.buffers = value_buffers;
		value_array.n_children = 2;
		value_array.children = value_array_children;
		value_array.release = NoopReleaseArray;
		array.length = 7;
		array.null_count = 1;
		array.n_buffers = 2;
		array.buffers = buffers;
		array.dictionary = &value_array;
		array.release = NoopReleaseArray;
	}
	StructDictionaryColumn(const StructDictionaryColumn &) = delete;

	int32_t i_values[4] = {1, 2, 3, 0};
	int32_t s_offsets[5] = {0, 1, 2, 3, 3};
	uint8_t value_validity[1] = {0x07};
	int8_t indices[7] = {0, 1, 2, 3, 0, 1, 0};
	uint8_t validity[1] = {0x6F};
	const void *i_buffers[2] = {nullptr, i_values};
	const void *s_buffers[3] = {nullptr, s_offsets, "abc"};
	const void *value_buffers[1] = {value_validity};
	const void *buffers[2] = {validity, indices};
	ArrowSchema *value_schema_children[2];
	ArrowArray *value_array_children[2];
	ArrowSchema i_schema {};
	ArrowSchema s_schema {};
	ArrowSchema value_schema {};
	ArrowSchema schema {};
	ArrowArray i_array {};
	ArrowArray s_array {};
	ArrowArray value_array {};
	ArrowArray array {};
};

// debug_verification_mode is process wide, so it is restored for the tests that run later.
struct VerificationModeRestorer {
	~VerificationModeRestorer() {
		DBConfigOptions::global_verification_mode = previous;
	}
	DebugVerificationMode previous = DBConfigOptions::global_verification_mode;
};

// int8 indices {0, NULL, 1} into a dictionary of two values.
struct NullIndexColumn {
	NullIndexColumn(ArrowSchema &value_schema, ArrowArray &value_array) {
		schema.format = "c";
		schema.name = "d";
		schema.flags = 2; // ARROW_FLAG_NULLABLE
		schema.dictionary = &value_schema;
		schema.release = NoopReleaseSchema;
		array.length = 3;
		array.null_count = 1;
		array.n_buffers = 2;
		array.buffers = buffers;
		array.dictionary = &value_array;
		array.release = NoopReleaseArray;
	}
	NullIndexColumn(const NullIndexColumn &) = delete;

	int8_t indices[3] = {0, 0, 1};
	uint8_t validity[1] = {0x05};
	const void *buffers[2] = {validity, indices};
	ArrowSchema schema {};
	ArrowArray array {};
};

void InitNullTypeArray(ArrowSchema &schema, ArrowArray &array, const char *name, int64_t length) {
	schema.format = "n";
	schema.name = name;
	schema.flags = 2;
	schema.release = NoopReleaseSchema;
	array.length = length;
	array.null_count = length;
	array.release = NoopReleaseArray;
}

// The stream keeps pointers into the record batch, so both live in one object.
struct ColumnStream {
	ColumnStream(ArrowSchema &col_schema, ArrowArray &col_array) {
		schema_children[0] = &col_schema;
		record_schema.format = "+s";
		record_schema.n_children = 1;
		record_schema.children = schema_children;
		record_schema.release = NoopReleaseSchema;
		array_children[0] = &col_array;
		record_array.length = col_array.length;
		record_array.n_buffers = 1;
		record_array.buffers = buffers;
		record_array.n_children = 1;
		record_array.children = array_children;
		record_array.release = NoopReleaseArray;
		AdbcError err {};
		REQUIRE(duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &stream, &err) == ADBC_STATUS_OK);
	}
	ColumnStream(const ColumnStream &) = delete;

	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	const void *buffers[1] = {nullptr};
	ArrowSchema record_schema {};
	ArrowArray record_array {};
	ArrowArrayStream stream {};
};

bool ScanColumnMatches(ArrowSchema &col_schema, ArrowArray &col_array, const string &query,
                       bool verify_vectors = false) {
	DuckDB db(nullptr);
	Connection con(db);
	VerificationModeRestorer restore_verification_mode;
	if (verify_vectors) {
		REQUIRE_NO_FAIL(con.Query("SET debug_verification_mode='verify_vectors'"));
	}
	ColumnStream compared(col_schema, col_array);
	if (!ArrowTestHelper::RunArrowComparison(con, query, compared.stream)) {
		return false;
	}
	// RunArrowComparison only checks that every scanned row is expected, so a scan that drops rows passes it.
	ColumnStream counted(col_schema, col_array);
	auto scanned = con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(counted.stream))
	                   ->Aggregate("count(*)")
	                   ->Execute();
	auto expected = con.Query("SELECT count(*) FROM (" + query + ")");
	REQUIRE_NO_FAIL(*scanned);
	REQUIRE_NO_FAIL(*expected);
	return scanned->Fetch()->GetValue(0, 0) == expected->Fetch()->GetValue(0, 0);
}

} // namespace

// Regression for the off-by-one in ColumnArrowToDuckDBDictionary: the base
// vector is allocated with capacity dict_length+1 (for the NULL-sentinel slot),
// but the inner ColumnArrowToDuckDB call goes through DirectConversion ->
// FlatVector::SetData, which replaces the buffer with one whose capacity
// equals dict_length. The follow-up FlatVector::SetSize(dict_length + 1) then
// trips:
//   "Vector::SetSize out of range - trying to set size to 4 for vector with capacity 3"
//
// We hand-build a minimal dict-encoded int32 ArrowArray (which DuckDB itself
// never produces — its appender only dict-encodes ENUM/VARCHAR), wrap it in
// an arrow stream, and scan it through arrow_scan. That is the same path the
// failing duckdb-python tests in test_dictionary_arrow.py take.
TEST_CASE("Arrow scan of dict-encoded int32 column with NULL sentinel", "[arrow]") {
	// indices: int32 with a dictionary of int32 values
	ArrowSchema dict_schema {};
	dict_schema.format = "i";
	dict_schema.flags = 2; // ARROW_FLAG_NULLABLE
	dict_schema.release = NoopReleaseSchema;

	ArrowSchema col_schema {};
	col_schema.format = "i";
	col_schema.name = "a";
	col_schema.flags = 2;
	col_schema.dictionary = &dict_schema;
	col_schema.release = NoopReleaseSchema;

	ArrowSchema *col_children_schemas[1] = {&col_schema};
	ArrowSchema record_schema {};
	record_schema.format = "+s";
	record_schema.n_children = 1;
	record_schema.children = col_children_schemas;
	record_schema.release = NoopReleaseSchema;

	int32_t dict_values[] = {10, 20, 30};
	const void *dict_array_buffers[2] = {nullptr, dict_values};
	ArrowArray dict_array {};
	dict_array.length = 3;
	dict_array.n_buffers = 2;
	dict_array.buffers = dict_array_buffers;
	dict_array.release = NoopReleaseArray;

	int32_t indices[] = {0, 1, 2, 0, 1, 2, 0, 1};
	const void *col_array_buffers[2] = {nullptr, indices};
	ArrowArray col_array {};
	col_array.length = 8;
	col_array.n_buffers = 2;
	col_array.buffers = col_array_buffers;
	col_array.dictionary = &dict_array;
	col_array.release = NoopReleaseArray;

	ArrowArray *col_children_arrays[1] = {&col_array};
	const void *record_array_buffers[1] = {nullptr};
	ArrowArray record_array {};
	record_array.length = 8;
	record_array.n_buffers = 1;
	record_array.buffers = record_array_buffers;
	record_array.n_children = 1;
	record_array.children = col_children_arrays;
	record_array.release = NoopReleaseArray;

	// Wrap (schema, array) as an ArrowArrayStream and compare with a SQL
	// SELECT that produces the same materialized result.
	ArrowArrayStream stream {};
	AdbcError err {};
	REQUIRE(duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &stream, &err) == ADBC_STATUS_OK);

	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE(ArrowTestHelper::RunArrowComparison(
	    con, "SELECT * FROM (VALUES (10), (20), (30), (10), (20), (30), (10), (20)) t(a)", stream));
}

// A null_count of -1 means the number of nulls is unknown - the validity bitmap must still be used, and the
// (arbitrary) indices in null slots must not be used to read the dictionary.
TEST_CASE("Arrow scan of dict-encoded column with an unknown null count", "[arrow]") {
	ArrowSchema dict_schema {};
	dict_schema.format = "i";
	dict_schema.flags = 2; // ARROW_FLAG_NULLABLE
	dict_schema.release = NoopReleaseSchema;

	ArrowSchema col_schema {};
	col_schema.format = "i";
	col_schema.name = "a";
	col_schema.flags = 2;
	col_schema.dictionary = &dict_schema;
	col_schema.release = NoopReleaseSchema;

	ArrowSchema *col_children_schemas[1] = {&col_schema};
	ArrowSchema record_schema {};
	record_schema.format = "+s";
	record_schema.n_children = 1;
	record_schema.children = col_children_schemas;
	record_schema.release = NoopReleaseSchema;

	int32_t dict_values[] = {10, 20, 30};
	const void *dict_array_buffers[2] = {nullptr, dict_values};
	ArrowArray dict_array {};
	dict_array.length = 3;
	dict_array.n_buffers = 2;
	dict_array.buffers = dict_array_buffers;
	dict_array.release = NoopReleaseArray;

	// row 1 is NULL, and its index points outside of the dictionary
	int32_t indices[] = {0, 1000, 2, 1};
	uint8_t validity[] = {0x0D};
	const void *col_array_buffers[2] = {validity, indices};
	ArrowArray col_array {};
	col_array.length = 4;
	col_array.null_count = -1;
	col_array.n_buffers = 2;
	col_array.buffers = col_array_buffers;
	col_array.dictionary = &dict_array;
	col_array.release = NoopReleaseArray;

	ArrowArray *col_children_arrays[1] = {&col_array};
	const void *record_array_buffers[1] = {nullptr};
	ArrowArray record_array {};
	record_array.length = 4;
	record_array.n_buffers = 1;
	record_array.buffers = record_array_buffers;
	record_array.n_children = 1;
	record_array.children = col_children_arrays;
	record_array.release = NoopReleaseArray;

	ArrowArrayStream stream {};
	AdbcError err {};
	REQUIRE(duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &stream, &err) == ADBC_STATUS_OK);

	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE(ArrowTestHelper::RunArrowComparison(con, "SELECT * FROM (VALUES (10), (NULL), (30), (20)) t(a)", stream));
}

// The NULL index selects the sentinel slot, whose struct children must be NULL too: vector verification checks that
// directly, the plain scan otherwise does not compare the row equal to a NULL struct, and with
// DUCKDB_JE_MALLOC_CONF=junk:true it crashes on a garbage string.
TEST_CASE("Arrow scan of dict-encoded STRUCT column with NULL sentinel", "[arrow]") {
	const string expected = "SELECT * FROM (VALUES ({'i': 1, 's': 'a'}), ({'i': 2, 's': 'b'}), ({'i': 3, 's': 'c'}), "
	                        "(NULL), (NULL), ({'i': 2, 's': 'b'}), ({'i': 1, 's': 'a'})) t(d)";
	StructDictionaryColumn verified_col;
	REQUIRE(ScanColumnMatches(verified_col.schema, verified_col.array, expected, /*verify_vectors=*/true));
	StructDictionaryColumn col;
	REQUIRE(ScanColumnMatches(col.schema, col.array, expected));
}

TEST_CASE("Arrow scan of dict-encoded LIST column with NULL sentinel", "[arrow]") {
	ArrowSchema element_schema {};
	element_schema.format = "u";
	element_schema.name = "item";
	element_schema.flags = 2; // ARROW_FLAG_NULLABLE
	element_schema.release = NoopReleaseSchema;
	int32_t element_offsets[] = {0, 1, 2, 3};
	const void *element_buffers[3] = {nullptr, element_offsets, "abc"};
	ArrowArray element_array {};
	element_array.length = 3;
	element_array.n_buffers = 3;
	element_array.buffers = element_buffers;
	element_array.release = NoopReleaseArray;

	// ['a'], ['b', 'c'], [] and a NULL list
	ArrowSchema *value_schema_children[1] = {&element_schema};
	ArrowSchema value_schema {};
	value_schema.format = "+l";
	value_schema.flags = 2;
	value_schema.n_children = 1;
	value_schema.children = value_schema_children;
	value_schema.release = NoopReleaseSchema;
	uint8_t value_validity[] = {0x07};
	int32_t value_offsets[] = {0, 1, 3, 3, 3};
	const void *value_buffers[2] = {value_validity, value_offsets};
	ArrowArray *value_array_children[1] = {&element_array};
	ArrowArray value_array {};
	value_array.length = 4;
	value_array.null_count = 1;
	value_array.n_buffers = 2;
	value_array.buffers = value_buffers;
	value_array.n_children = 1;
	value_array.children = value_array_children;
	value_array.release = NoopReleaseArray;

	// row 3 points at the NULL list and row 4 has a NULL index
	ArrowSchema schema {};
	schema.format = "c";
	schema.name = "d";
	schema.flags = 2;
	schema.dictionary = &value_schema;
	schema.release = NoopReleaseSchema;
	int8_t indices[] = {0, 1, 2, 3, 0, 1, 0};
	uint8_t validity[] = {0x6F};
	const void *buffers[2] = {validity, indices};
	ArrowArray array {};
	array.length = 7;
	array.null_count = 1;
	array.n_buffers = 2;
	array.buffers = buffers;
	array.dictionary = &value_array;
	array.release = NoopReleaseArray;

	REQUIRE(
	    ScanColumnMatches(schema, array,
	                      "SELECT * FROM (VALUES (['a']), (['b', 'c']), ([]::VARCHAR[]), (NULL), (NULL), (['b', 'c']), "
	                      "(['a'])) t(d)"));
}

TEST_CASE("Arrow scan of dict-encoded STRUCT column inside a STRUCT with NULL sentinel", "[arrow]") {
	StructDictionaryColumn dict;
	ArrowSchema *schema_children[1] = {&dict.schema};
	ArrowSchema schema {};
	schema.format = "+s";
	schema.name = "o";
	schema.flags = 2;
	schema.n_children = 1;
	schema.children = schema_children;
	schema.release = NoopReleaseSchema;

	// row 2 is NULL in the parent while its index is valid
	uint8_t validity[] = {0x7B};
	const void *buffers[1] = {validity};
	ArrowArray *array_children[1] = {&dict.array};
	ArrowArray array {};
	array.length = 7;
	array.null_count = 1;
	array.n_buffers = 1;
	array.buffers = buffers;
	array.n_children = 1;
	array.children = array_children;
	array.release = NoopReleaseArray;

	REQUIRE(ScanColumnMatches(
	    schema, array,
	    "SELECT * FROM (VALUES ({'d': {'i': 1, 's': 'a'}}), ({'d': {'i': 2, 's': 'b'}}), (NULL), "
	    "({'d': NULL}), ({'d': NULL}), ({'d': {'i': 2, 's': 'b'}}), ({'d': {'i': 1, 's': 'a'}})) t(o)"));
}

TEST_CASE("Arrow scan of dict-encoded null type column with NULL sentinel", "[arrow]") {
	ArrowSchema value_schema {};
	ArrowArray value_array {};
	InitNullTypeArray(value_schema, value_array, nullptr, 2);
	NullIndexColumn col(value_schema, value_array);
	REQUIRE(ScanColumnMatches(col.schema, col.array, "SELECT * FROM (VALUES (NULL), (NULL), (NULL)) t(d)"));
}

TEST_CASE("Arrow scan of dict-encoded STRUCT column with a null type field and NULL sentinel", "[arrow]") {
	ArrowSchema n_schema {};
	ArrowArray n_array {};
	InitNullTypeArray(n_schema, n_array, "n", 2);
	ArrowSchema s_schema {};
	s_schema.format = "u";
	s_schema.name = "s";
	s_schema.flags = 2;
	s_schema.release = NoopReleaseSchema;
	int32_t s_offsets[] = {0, 1, 2};
	const void *s_buffers[3] = {nullptr, s_offsets, "ab"};
	ArrowArray s_array {};
	s_array.length = 2;
	s_array.n_buffers = 3;
	s_array.buffers = s_buffers;
	s_array.release = NoopReleaseArray;

	ArrowSchema *value_schema_children[2] = {&n_schema, &s_schema};
	ArrowSchema value_schema {};
	value_schema.format = "+s";
	value_schema.flags = 2;
	value_schema.n_children = 2;
	value_schema.children = value_schema_children;
	value_schema.release = NoopReleaseSchema;
	ArrowArray *value_array_children[2] = {&n_array, &s_array};
	const void *value_buffers[1] = {nullptr};
	ArrowArray value_array {};
	value_array.length = 2;
	value_array.n_buffers = 1;
	value_array.buffers = value_buffers;
	value_array.n_children = 2;
	value_array.children = value_array_children;
	value_array.release = NoopReleaseArray;

	NullIndexColumn col(value_schema, value_array);
	REQUIRE(ScanColumnMatches(col.schema, col.array,
	                          "SELECT * FROM (VALUES ({'n': NULL, 's': 'a'}), (NULL), ({'n': NULL, 's': 'b'})) t(d)"));
}

TEST_CASE("Arrow scan of dict-encoded ARRAY of null type column with NULL sentinel", "[arrow]") {
	ArrowSchema element_schema {};
	ArrowArray element_array {};
	InitNullTypeArray(element_schema, element_array, "item", 4);

	ArrowSchema *value_schema_children[1] = {&element_schema};
	ArrowSchema value_schema {};
	value_schema.format = "+w:2";
	value_schema.flags = 2;
	value_schema.n_children = 1;
	value_schema.children = value_schema_children;
	value_schema.release = NoopReleaseSchema;
	ArrowArray *value_array_children[1] = {&element_array};
	const void *value_buffers[1] = {nullptr};
	ArrowArray value_array {};
	value_array.length = 2;
	value_array.n_buffers = 1;
	value_array.buffers = value_buffers;
	value_array.n_children = 1;
	value_array.children = value_array_children;
	value_array.release = NoopReleaseArray;

	NullIndexColumn col(value_schema, value_array);
	REQUIRE(
	    ScanColumnMatches(col.schema, col.array, "SELECT * FROM (VALUES ([NULL, NULL]), (NULL), ([NULL, NULL])) t(d)"));
}
