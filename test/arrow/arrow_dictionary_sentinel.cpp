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

// Hand built Arrow arrays: each node keeps its schema, array and pointer lists at one address while a stream refers to
// them.
class ArrowNodes {
public:
	struct Node {
		ArrowSchema schema {};
		ArrowArray array {};
		vector<const void *> buffers;
		vector<ArrowSchema *> schema_children;
		vector<ArrowArray *> array_children;
	};

	Node &Add(const char *format, const char *name, int64_t length, vector<const void *> buffers,
	          vector<reference<Node>> children = {}, int64_t null_count = 0, int64_t offset = 0) {
		nodes.push_back(make_uniq<Node>());
		auto &node = *nodes.back();
		node.buffers = std::move(buffers);
		for (auto &child : children) {
			node.schema_children.push_back(&child.get().schema);
			node.array_children.push_back(&child.get().array);
		}
		node.schema.format = format;
		node.schema.name = name;
		node.schema.flags = 2; // ARROW_FLAG_NULLABLE
		node.schema.n_children = NumericCast<int64_t>(children.size());
		node.schema.children = node.schema_children.data();
		node.schema.release = NoopReleaseSchema;
		node.array.length = length;
		node.array.null_count = null_count;
		node.array.offset = offset;
		node.array.n_buffers = NumericCast<int64_t>(node.buffers.size());
		node.array.buffers = node.buffers.data();
		node.array.n_children = NumericCast<int64_t>(children.size());
		node.array.children = node.array_children.data();
		node.array.release = NoopReleaseArray;
		return node;
	}

	Node &Dictionary(const char *name, Node &values, int64_t length, const uint8_t *validity, const void *indices,
	                 int64_t null_count, int64_t offset = 0, const char *index_format = "c") {
		auto &node = Add(index_format, name, length, {validity, indices}, {}, null_count, offset);
		node.schema.dictionary = &values.schema;
		node.array.dictionary = &values.array;
		return node;
	}

private:
	vector<unique_ptr<Node>> nodes;
};

// Three values make dict_length + 1 a power of two, so the parent's own capacity already holds the sentinel slot
// while a zero copy child holds the three values only. Row 0 is only read through an offset.
const int32_t INT_VALUES[] = {9, 1, 2, 3, 4, 5, 6};
const int32_t STRING_OFFSETS[] = {0, 1, 2, 3, 4};
const char STRING_DATA[] = "zabc";

// STRUCT(i INTEGER, s VARCHAR) values {1, 'a'}, {2, 'b'}, {3, 'c'}.
ArrowNodes::Node &StructValues(ArrowNodes &nodes, int64_t offset = 0) {
	auto length = 3 + offset;
	auto &i = nodes.Add("i", "i", length, {nullptr, INT_VALUES + 1 - offset});
	auto &s = nodes.Add("u", "s", length, {nullptr, STRING_OFFSETS + 1 - offset, STRING_DATA});
	return nodes.Add("+s", nullptr, 3, {nullptr}, {i, s}, 0, offset);
}

// int8 indices {0, NULL, 2, 1, 0}; the offset variant reads the same rows after a valid index of 1.
const int8_t NULL_INDICES[] = {0, 0, 2, 1, 0};
const uint8_t NULL_INDICES_VALIDITY[] = {0x1D};
const int8_t OFFSET_NULL_INDICES[] = {1, 0, 0, 2, 1, 0};
const uint8_t OFFSET_NULL_INDICES_VALIDITY[] = {0x3B};

ArrowNodes::Node &NullIndexDictionary(ArrowNodes &nodes, ArrowNodes::Node &values) {
	return nodes.Dictionary("d", values, 5, NULL_INDICES_VALIDITY, NULL_INDICES, 1);
}

const char *const STRUCT_ROWS = "SELECT * FROM (VALUES ({'i': 1, 's': 'a'}), (NULL), ({'i': 3, 's': 'c'}), "
                                "({'i': 2, 's': 'b'}), ({'i': 1, 's': 'a'})) t(d)";

vector<Value> FirstColumn(QueryResult &result) {
	vector<Value> values;
	while (auto chunk = result.Fetch()) {
		for (idx_t row = 0; row < chunk->size(); row++) {
			values.push_back(chunk->GetValue(0, row));
		}
	}
	return values;
}

// Unlike ScanColumnMatches, compares the type and every row in order.
bool ScanReadsRows(ArrowSchema &col_schema, ArrowArray &col_array, const string &expected_query,
                   bool verify_vectors = false) {
	DuckDB db(nullptr);
	Connection con(db);
	VerificationModeRestorer restore_verification_mode;
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	if (verify_vectors) {
		REQUIRE_NO_FAIL(con.Query("SET debug_verification_mode='verify_vectors'"));
	}
	ColumnStream stream(col_schema, col_array);
	auto scanned =
	    con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream.stream))->Execute();
	auto expected = con.Query(expected_query);
	REQUIRE_NO_FAIL(*scanned);
	REQUIRE_NO_FAIL(*expected);
	if (scanned->GetTypes() != expected->GetTypes()) {
		UNSCOPED_INFO("scanned type " << scanned->GetTypes()[0].ToString() << ", expected "
		                              << expected->GetTypes()[0].ToString());
		return false;
	}
	auto scanned_values = FirstColumn(*scanned);
	auto expected_values = FirstColumn(*expected);
	if (scanned_values.size() != expected_values.size()) {
		UNSCOPED_INFO("scanned " << scanned_values.size() << " rows, expected " << expected_values.size());
		return false;
	}
	for (idx_t row = 0; row < scanned_values.size(); row++) {
		if (!Value::NotDistinctFrom(scanned_values[row], expected_values[row])) {
			UNSCOPED_INFO("row " << row << ": scanned " << scanned_values[row].ToString() << ", expected "
			                     << expected_values[row].ToString());
			return false;
		}
	}
	return true;
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

TEST_CASE("Arrow scan of dict-encoded STRUCT column whose parent capacity covers the NULL sentinel", "[arrow]") {
	ArrowNodes nodes;
	auto &values = StructValues(nodes);
	auto &with_null_index = NullIndexDictionary(nodes, values);
	REQUIRE(ScanReadsRows(with_null_index.schema, with_null_index.array, STRUCT_ROWS, /*verify_vectors=*/true));
	REQUIRE(ScanReadsRows(with_null_index.schema, with_null_index.array, STRUCT_ROWS));

	const int8_t indices[] = {2, 0, 1};
	auto &without_null_index = nodes.Dictionary("d", values, 3, nullptr, indices, 0);
	REQUIRE(
	    ScanReadsRows(without_null_index.schema, without_null_index.array,
	                  "SELECT * FROM (VALUES ({'i': 3, 's': 'c'}), ({'i': 1, 's': 'a'}), ({'i': 2, 's': 'b'})) t(d)",
	                  /*verify_vectors=*/true));
}

TEST_CASE("Arrow scan of dict-encoded STRUCT column with offset indices and offset values", "[arrow]") {
	ArrowNodes nodes;
	auto &values = StructValues(nodes);
	auto &offset_indices =
	    nodes.Dictionary("d", values, 5, OFFSET_NULL_INDICES_VALIDITY, OFFSET_NULL_INDICES, 1, /*offset=*/1);
	REQUIRE(ScanReadsRows(offset_indices.schema, offset_indices.array, STRUCT_ROWS, /*verify_vectors=*/true));

	auto &offset_values = StructValues(nodes, /*offset=*/1);
	auto &column = NullIndexDictionary(nodes, offset_values);
	REQUIRE(ScanReadsRows(column.schema, column.array, STRUCT_ROWS, /*verify_vectors=*/true));
}

TEST_CASE("Arrow scan of LIST of dict-encoded STRUCT whose parent capacity covers the NULL sentinel", "[arrow]") {
	ArrowNodes nodes;
	auto &elements = NullIndexDictionary(nodes, StructValues(nodes));
	const char *expected = "SELECT * FROM (VALUES ([{'i': 1, 's': 'a'}, NULL]), ([]), "
	                       "([{'i': 3, 's': 'c'}, {'i': 2, 's': 'b'}, {'i': 1, 's': 'a'}])) t(d)";
	const int32_t offsets[] = {0, 2, 2, 5};
	auto &list = nodes.Add("+l", "d", 3, {nullptr, offsets}, {elements});
	REQUIRE(ScanReadsRows(list.schema, list.array, expected, /*verify_vectors=*/true));

	const int32_t offset_offsets[] = {0, 0, 2, 2, 5};
	auto &offset_list = nodes.Add("+l", "d", 3, {nullptr, offset_offsets}, {elements}, 0, /*offset=*/1);
	REQUIRE(ScanReadsRows(offset_list.schema, offset_list.array, expected, /*verify_vectors=*/true));
}

TEST_CASE("Arrow scan of STRUCT of dict-encoded STRUCT whose parent capacity covers the NULL sentinel", "[arrow]") {
	ArrowNodes nodes;
	auto &field = nodes.Dictionary("x", StructValues(nodes), 5, NULL_INDICES_VALIDITY, NULL_INDICES, 1);
	// row 3 is NULL in the parent while its index is valid
	const uint8_t validity[] = {0x17};
	auto &column = nodes.Add("+s", "d", 5, {validity}, {field}, 1);
	REQUIRE(ScanReadsRows(column.schema, column.array,
	                      "SELECT * FROM (VALUES ({'x': {'i': 1, 's': 'a'}}), ({'x': NULL}), ({'x': {'i': 3, 's': "
	                      "'c'}}), (NULL), ({'x': {'i': 1, 's': 'a'}})) t(d)",
	                      /*verify_vectors=*/true));
}

TEST_CASE("Arrow scan of dict-encoded STRUCT of STRUCT column with NULL sentinel", "[arrow]") {
	ArrowNodes nodes;
	auto &b = nodes.Add("i", "b", 3, {nullptr, INT_VALUES + 1});
	auto &a = nodes.Add("+s", "a", 3, {nullptr}, {b});
	auto &column = NullIndexDictionary(nodes, nodes.Add("+s", nullptr, 3, {nullptr}, {a}));
	REQUIRE(ScanReadsRows(column.schema, column.array,
	                      "SELECT * FROM (VALUES ({'a': {'b': 1}}), (NULL), ({'a': {'b': 3}}), ({'a': {'b': 2}}), "
	                      "({'a': {'b': 1}})) t(d)",
	                      /*verify_vectors=*/true));
}

TEST_CASE("Arrow scan of dict-encoded fixed-size ARRAY column with NULL sentinel", "[arrow]") {
	ArrowNodes nodes;
	auto &element = nodes.Add("i", "item", 6, {nullptr, INT_VALUES + 1});
	auto &column = NullIndexDictionary(nodes, nodes.Add("+w:2", nullptr, 3, {nullptr}, {element}));
	REQUIRE(ScanReadsRows(column.schema, column.array,
	                      "SELECT * FROM (VALUES ([1, 2]::INTEGER[2]), (NULL), ([5, 6]::INTEGER[2]), "
	                      "([3, 4]::INTEGER[2]), ([1, 2]::INTEGER[2])) t(d)",
	                      /*verify_vectors=*/true));
}

TEST_CASE("Arrow scan of dict-encoded STRUCT column with a dict-encoded field and NULL sentinel", "[arrow]") {
	ArrowNodes nodes;
	const int32_t string_offsets[] = {0, 1, 2};
	auto &strings = nodes.Add("u", nullptr, 2, {nullptr, string_offsets, "pq"});
	SECTION("three values") {
		const int8_t field_indices[] = {1, 0, 1};
		auto &field = nodes.Dictionary("x", strings, 3, nullptr, field_indices, 0);
		auto &column = NullIndexDictionary(nodes, nodes.Add("+s", nullptr, 3, {nullptr}, {field}));
		REQUIRE(ScanReadsRows(column.schema, column.array,
		                      "SELECT * FROM (VALUES ({'x': 'q'}), (NULL), ({'x': 'q'}), ({'x': 'p'}), ({'x': 'q'})) "
		                      "t(d)",
		                      /*verify_vectors=*/true));
	}
	SECTION("two values") {
		const int8_t field_indices[] = {1, 0};
		auto &field = nodes.Dictionary("x", strings, 2, nullptr, field_indices, 0);
		const int8_t indices[] = {1, 0, 0};
		const uint8_t validity[] = {0x05};
		auto &column = nodes.Dictionary("d", nodes.Add("+s", nullptr, 2, {nullptr}, {field}), 3, validity, indices, 1);
		REQUIRE(ScanReadsRows(column.schema, column.array,
		                      "SELECT * FROM (VALUES ({'x': 'p'}), (NULL), ({'x': 'q'})) t(d)",
		                      /*verify_vectors=*/true));
	}
}

TEST_CASE("Arrow scan of dict-encoded column whose values are dict-encoded, with NULL sentinel", "[arrow]") {
	ArrowNodes nodes;
	auto &strings = nodes.Add("u", nullptr, 3, {nullptr, STRING_OFFSETS + 1, STRING_DATA});
	const int8_t value_indices[] = {2, 1, 0};
	auto &values = nodes.Dictionary(nullptr, strings, 3, nullptr, value_indices, 0);
	auto &column = NullIndexDictionary(nodes, values);
	REQUIRE(ScanReadsRows(column.schema, column.array, "SELECT * FROM (VALUES ('c'), (NULL), ('a'), ('b'), ('c')) t(d)",
	                      /*verify_vectors=*/true));
}

TEST_CASE("Arrow scan of dict-encoded flat columns whose capacity covers the NULL sentinel", "[arrow]") {
	ArrowNodes nodes;
	SECTION("INTEGER") {
		auto &column = NullIndexDictionary(nodes, nodes.Add("i", nullptr, 3, {nullptr, INT_VALUES + 1}));
		REQUIRE(ScanReadsRows(column.schema, column.array, "SELECT * FROM (VALUES (1), (NULL), (3), (2), (1)) t(d)",
		                      /*verify_vectors=*/true));
	}
	SECTION("VARCHAR") {
		auto &column =
		    NullIndexDictionary(nodes, nodes.Add("u", nullptr, 3, {nullptr, STRING_OFFSETS + 1, STRING_DATA}));
		REQUIRE(ScanReadsRows(column.schema, column.array,
		                      "SELECT * FROM (VALUES ('a'), (NULL), ('c'), ('b'), ('a')) t(d)",
		                      /*verify_vectors=*/true));
	}
	SECTION("LIST") {
		auto &element = nodes.Add("i", "item", 3, {nullptr, INT_VALUES + 1});
		const int32_t offsets[] = {0, 1, 3, 3};
		auto &column = NullIndexDictionary(nodes, nodes.Add("+l", nullptr, 3, {nullptr, offsets}, {element}));
		REQUIRE(ScanReadsRows(column.schema, column.array,
		                      "SELECT * FROM (VALUES ([1]), (NULL), ([]), ([2, 3]), ([1])) t(d)",
		                      /*verify_vectors=*/true));
	}
}

TEST_CASE("Arrow scan of fixed-size ARRAY of dict-encoded VARCHAR reads the dictionary values", "[arrow]") {
	ArrowNodes nodes;
	auto &strings = nodes.Add("u", nullptr, 3, {nullptr, STRING_OFFSETS + 1, STRING_DATA});
	// the first array is only read when the offset is ignored
	const int8_t indices[] = {1, 1, 0, 2, 2, 1};
	const char *expected = "SELECT * FROM (VALUES (['a', 'c']::VARCHAR[2]), (['c', 'b']::VARCHAR[2])) t(d)";

	auto &elements = nodes.Dictionary("item", strings, 4, nullptr, indices + 2, 0);
	auto &column = nodes.Add("+w:2", "d", 2, {nullptr}, {elements});
	REQUIRE(ScanReadsRows(column.schema, column.array, expected, /*verify_vectors=*/true));

	const uint8_t validity[] = {0x0D};
	auto &null_elements = nodes.Dictionary("item", strings, 4, validity, indices + 2, 1);
	auto &null_column = nodes.Add("+w:2", "d", 2, {nullptr}, {null_elements});
	REQUIRE(ScanReadsRows(null_column.schema, null_column.array,
	                      "SELECT * FROM (VALUES (['a', NULL]::VARCHAR[2]), (['c', 'b']::VARCHAR[2])) t(d)",
	                      /*verify_vectors=*/true));

	auto &offset_elements = nodes.Dictionary("item", strings, 6, nullptr, indices, 0);
	auto &offset_column = nodes.Add("+w:2", "d", 2, {nullptr}, {offset_elements}, 0, /*offset=*/1);
	REQUIRE(ScanReadsRows(offset_column.schema, offset_column.array, expected, /*verify_vectors=*/true));
}

TEST_CASE("Arrow scan of dict-encoded LIST of dict-encoded VARCHAR with NULL sentinel", "[arrow]") {
	ArrowNodes nodes;
	auto &strings = nodes.Add("u", nullptr, 3, {nullptr, STRING_OFFSETS + 1, STRING_DATA});
	const int8_t element_indices[] = {2, 0, 1};
	auto &elements = nodes.Dictionary("item", strings, 3, nullptr, element_indices, 0);
	const int32_t offsets[] = {0, 1, 3, 3};
	auto &column = NullIndexDictionary(nodes, nodes.Add("+l", nullptr, 3, {nullptr, offsets}, {elements}));
	REQUIRE(ScanReadsRows(column.schema, column.array,
	                      "SELECT * FROM (VALUES (['c']), (NULL), ([]::VARCHAR[]), (['a', 'b']), (['c'])) t(d)",
	                      /*verify_vectors=*/true));
}

TEST_CASE("Arrow scan of an empty dict-encoded STRUCT dictionary with only NULL indices", "[arrow]") {
	ArrowNodes nodes;
	auto &i = nodes.Add("i", "i", 0, {nullptr, INT_VALUES});
	auto &s = nodes.Add("u", "s", 0, {nullptr, STRING_OFFSETS, STRING_DATA});
	auto &values = nodes.Add("+s", nullptr, 0, {nullptr}, {i, s});
	const int8_t indices[] = {0, 0};
	const uint8_t validity[] = {0x00};
	auto &column = nodes.Dictionary("d", values, 2, validity, indices, 2);
	REQUIRE(ScanReadsRows(column.schema, column.array,
	                      "SELECT * FROM (VALUES (NULL::STRUCT(i INTEGER, s VARCHAR)), (NULL)) t(d)",
	                      /*verify_vectors=*/true));
}

TEST_CASE("Arrow scan of dict-encoded MAP column with dict-encoded keys and NULL sentinel", "[arrow]") {
	ArrowNodes nodes;
	const int32_t key_offsets[] = {0, 1, 2};
	auto &key_strings = nodes.Add("u", nullptr, 2, {nullptr, key_offsets, "kl"});
	const int8_t key_indices[] = {0, 1, 0};
	auto &keys = nodes.Dictionary("key", key_strings, 3, nullptr, key_indices, 0);
	keys.schema.flags = 0;
	auto &map_values = nodes.Add("i", "value", 3, {nullptr, INT_VALUES + 1});
	auto &entries = nodes.Add("+s", "entries", 3, {nullptr}, {keys, map_values});
	entries.schema.flags = 0;
	const int32_t offsets[] = {0, 1, 3, 3};
	auto &column = NullIndexDictionary(nodes, nodes.Add("+m", nullptr, 3, {nullptr, offsets}, {entries}));
	REQUIRE(ScanReadsRows(column.schema, column.array,
	                      "SELECT * FROM (VALUES (MAP {'k': 1}), (NULL), (MAP {}::MAP(VARCHAR, INTEGER)), "
	                      "(MAP {'l': 2, 'k': 3}), (MAP {'k': 1})) t(d)",
	                      /*verify_vectors=*/true));
}

TEST_CASE("Arrow scan of dict-encoded fixed-size ARRAY of STRUCT column with NULL sentinel", "[arrow]") {
	ArrowNodes nodes;
	auto &i = nodes.Add("i", "i", 6, {nullptr, INT_VALUES + 1});
	auto &element = nodes.Add("+s", "item", 6, {nullptr}, {i});
	auto &column = NullIndexDictionary(nodes, nodes.Add("+w:2", nullptr, 3, {nullptr}, {element}));
	REQUIRE(ScanReadsRows(column.schema, column.array,
	                      "SELECT * FROM (VALUES ([{'i': 1}, {'i': 2}]::STRUCT(i INTEGER)[2]), (NULL), "
	                      "([{'i': 5}, {'i': 6}]::STRUCT(i INTEGER)[2]), ([{'i': 3}, {'i': 4}]::STRUCT(i INTEGER)[2]), "
	                      "([{'i': 1}, {'i': 2}]::STRUCT(i INTEGER)[2])) t(d)",
	                      /*verify_vectors=*/true));
}

TEST_CASE("Arrow scan of dict-encoded STRUCT column whose field is a dict-encoded STRUCT", "[arrow]") {
	ArrowNodes nodes;
	const int8_t field_indices[] = {0, 0, 2};
	const uint8_t field_validity[] = {0x05};
	auto &field = nodes.Dictionary("x", StructValues(nodes), 3, field_validity, field_indices, 1);
	auto &column = NullIndexDictionary(nodes, nodes.Add("+s", nullptr, 3, {nullptr}, {field}));
	REQUIRE(ScanReadsRows(column.schema, column.array,
	                      "SELECT * FROM (VALUES ({'x': {'i': 1, 's': 'a'}}), (NULL), ({'x': {'i': 3, 's': 'c'}}), "
	                      "({'x': NULL}), ({'x': {'i': 1, 's': 'a'}})) t(d)",
	                      /*verify_vectors=*/true));
}

TEST_CASE("Arrow scan of a dict-encoded STRUCT column over more rows and values than a vector", "[arrow]") {
	constexpr int32_t VALUE_COUNT = 4095;
	constexpr int64_t ROW_COUNT = 5000;
	vector<int32_t> i_values(VALUE_COUNT);
	for (int32_t value = 0; value < VALUE_COUNT; value++) {
		i_values[NumericCast<idx_t>(value)] = value;
	}
	vector<int16_t> indices(ROW_COUNT);
	vector<uint8_t> validity((ROW_COUNT + 7) / 8, 0);
	for (int64_t row = 0; row < ROW_COUNT; row++) {
		indices[NumericCast<idx_t>(row)] = NumericCast<int16_t>(VALUE_COUNT - 1 - row % VALUE_COUNT);
		if (row % 7 != 0) {
			validity[NumericCast<idx_t>(row / 8)] |= NumericCast<uint8_t>(1 << (row % 8));
		}
	}
	ArrowNodes nodes;
	auto &i = nodes.Add("i", "i", VALUE_COUNT, {nullptr, i_values.data()});
	auto &values = nodes.Add("+s", nullptr, VALUE_COUNT, {nullptr}, {i});
	auto &column =
	    nodes.Dictionary("d", values, ROW_COUNT, validity.data(), indices.data(), (ROW_COUNT + 6) / 7, 0, "s");
	REQUIRE(ScanReadsRows(column.schema, column.array,
	                      "SELECT CASE WHEN r % 7 = 0 THEN NULL ELSE {'i': (4094 - r % 4095)::INTEGER} END AS d "
	                      "FROM range(5000) t(r) ORDER BY r",
	                      /*verify_vectors=*/true));
}

// The type info of a run-end encoded type describes its run ends and values, not its value type.
TEST_CASE("Arrow scan binds run-end encoded nested columns", "[arrow]") {
	ArrowNodes nodes;
	const int32_t run_ends[] = {1, 2};
	auto &array_ends = nodes.Add("i", "run_ends", 2, {nullptr, run_ends});
	auto &element = nodes.Add("i", "item", 4, {nullptr, INT_VALUES + 1});
	auto &array_values = nodes.Add("+w:2", "values", 2, {nullptr}, {element});
	auto &ree_array = nodes.Add("+r", "d", 2, {}, {array_ends, array_values});
	ree_array.schema.flags = 0;

	auto &struct_ends = nodes.Add("i", "run_ends", 2, {nullptr, run_ends});
	auto &a = nodes.Add("i", "a", 2, {nullptr, INT_VALUES + 1});
	auto &struct_values = nodes.Add("+s", "values", 2, {nullptr}, {a});
	auto &ree_struct = nodes.Add("+r", nullptr, 2, {}, {struct_ends, struct_values});
	ree_struct.schema.flags = 0;
	const int8_t indices[] = {1, 0};
	auto &dictionary_of_ree = nodes.Dictionary("d", ree_struct, 2, nullptr, indices, 0);

	DuckDB db(nullptr);
	Connection con(db);
	ColumnStream array_stream(ree_array.schema, ree_array.array);
	auto array_scan = con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(array_stream.stream));
	REQUIRE(array_scan->Columns()[0].Type() == LogicalType::ARRAY(LogicalType::INTEGER, 2));
	ColumnStream dictionary_stream(dictionary_of_ree.schema, dictionary_of_ree.array);
	auto dictionary_scan =
	    con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(dictionary_stream.stream));
	REQUIRE(dictionary_scan->Columns()[0].Type() == LogicalType::STRUCT({{"a", LogicalType::INTEGER}}));
	// binding only reads the schema, so no scan releases the streams
	array_stream.stream.release(&array_stream.stream);
	dictionary_stream.stream.release(&dictionary_stream.stream);
}

TEST_CASE("Arrow scan of an empty dict-encoded fixed-size ARRAY dictionary of dict-encoded VARCHAR", "[arrow]") {
	ArrowNodes nodes;
	auto &strings = nodes.Add("u", nullptr, 1, {nullptr, STRING_OFFSETS + 1, STRING_DATA});
	const int8_t indices[] = {0};
	auto &elements = nodes.Dictionary("item", strings, 0, nullptr, indices, 0);
	auto &values = nodes.Add("+w:2", nullptr, 0, {nullptr}, {elements});
	const uint8_t validity[] = {0x00};
	auto &column = nodes.Dictionary("d", values, 1, validity, indices, 1);
	REQUIRE(ScanReadsRows(column.schema, column.array, "SELECT NULL::VARCHAR[2] AS d", /*verify_vectors=*/true));
}
