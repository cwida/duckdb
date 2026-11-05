// Arrow scans of a NULL FIXED_SIZE_LIST row over a LIST or FIXED_SIZE_LIST child that has its own NULL bitmap.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"

#include <string>
#include <vector>

using namespace duckdb;

namespace arrow_fixed_size_list_null_children_test {

void ReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void ReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

// Sets the bits of the given rows to NULL in a validity bitmap that starts all valid.
std::vector<uint8_t> NullBitmap(idx_t length, const std::vector<idx_t> &null_rows) {
	std::vector<uint8_t> validity((length + 7) / 8 + 1, 0xFF);
	for (auto row : null_rows) {
		validity[row / 8] &= uint8_t(~(1u << (row % 8)));
	}
	return validity;
}

// Non-NULL utf8 column with the given values.
struct StringColumn {
	explicit StringColumn(const std::vector<string> &values) {
		offsets.push_back(0);
		for (auto &value : values) {
			data += value;
			offsets.push_back(int32_t(data.size()));
		}
		schema.format = "u";
		schema.name = "item";
		schema.flags = 2;
		schema.release = ReleaseSchema;
		buffers[0] = nullptr;
		buffers[1] = offsets.data();
		buffers[2] = data.data();
		array.length = int64_t(values.size());
		array.n_buffers = 3;
		array.buffers = buffers;
		array.release = ReleaseArray;
	}
	StringColumn(const StringColumn &) = delete;

	std::vector<int32_t> offsets;
	string data;
	const void *buffers[3];
	ArrowSchema schema {};
	ArrowArray array {};
};

// LIST (format "+l", given offsets) or FIXED_SIZE_LIST (format "+w:<n>") column over child, starting at row offset,
// NULL for the given rows (counted from the start of the buffers).
struct NestedColumn {
	template <class CHILD>
	NestedColumn(CHILD &child, const string &format_p, int64_t length, const std::vector<idx_t> &null_rows,
	             const std::vector<int32_t> &offsets_p = {}, int64_t offset = 0)
	    : format(format_p), validity(NullBitmap(idx_t(length + offset), null_rows)), offsets(offsets_p) {
		schema_children[0] = &child.schema;
		schema.format = format.c_str();
		schema.name = "a";
		schema.flags = 2;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		buffers[0] = null_rows.empty() ? nullptr : validity.data();
		buffers[1] = offsets.data();
		array_children[0] = &child.array;
		array.length = length;
		array.offset = offset;
		array.null_count = int64_t(null_rows.size());
		array.n_buffers = offsets.empty() ? 1 : 2;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	NestedColumn(const NestedColumn &) = delete;

	string format;
	std::vector<uint8_t> validity;
	std::vector<int32_t> offsets;
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	const void *buffers[2];
	ArrowSchema schema {};
	ArrowArray array {};
};

// Runs the query over the scan of the column, as the view arrow_view, and checks that it returns expected.
bool QueryMatches(ArrowSchema &col_schema, ArrowArray &col_array, const string &query, const string &expected) {
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
	con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream))->CreateView("arrow_view");
	auto result = con.Query(query);
	auto expected_result = con.Query(expected);
	stream.release = nullptr;
	if (result->HasError() || expected_result->HasError()) {
		return false;
	}
	return result->Equals(*expected_result, false);
}

// Row 0 is NULL over the inner rows ['1000', '1000'] and ['5', '5'], row 1 is [['5', '5'], NULL].
const std::vector<string> STRING_VALUES = {"1000", "1000", "5", "5", "5", "5", "5", "5"};

} // namespace arrow_fixed_size_list_null_children_test

TEST_CASE("Arrow scan of a NULL FIXED_SIZE_LIST row over FIXED_SIZE_LIST children with NULLs", "[arrow]") {
	arrow_fixed_size_list_null_children_test::StringColumn strings(
	    arrow_fixed_size_list_null_children_test::STRING_VALUES);
	arrow_fixed_size_list_null_children_test::NestedColumn inner(strings, "+w:2", 4, {3});
	arrow_fixed_size_list_null_children_test::NestedColumn outer(inner, "+w:2", 2, {0});
	REQUIRE(arrow_fixed_size_list_null_children_test::QueryMatches(
	    outer.schema, outer.array, "SELECT a::TINYINT[2][2]::VARCHAR FROM arrow_view",
	    "SELECT * FROM (VALUES (NULL), ('[[5, 5], NULL]')) t(a)"));
	REQUIRE(arrow_fixed_size_list_null_children_test::QueryMatches(
	    outer.schema, outer.array, "SELECT count(*) FROM arrow_view WHERE a IS NULL", "SELECT 1::BIGINT"));
}

TEST_CASE("Arrow scan of a NULL FIXED_SIZE_LIST row over LIST children with NULLs", "[arrow]") {
	arrow_fixed_size_list_null_children_test::StringColumn strings(
	    arrow_fixed_size_list_null_children_test::STRING_VALUES);
	arrow_fixed_size_list_null_children_test::NestedColumn inner(strings, "+l", 4, {3}, {0, 2, 4, 6, 8});
	arrow_fixed_size_list_null_children_test::NestedColumn outer(inner, "+w:2", 2, {0});
	REQUIRE(arrow_fixed_size_list_null_children_test::QueryMatches(
	    outer.schema, outer.array, "SELECT a::TINYINT[][2]::VARCHAR FROM arrow_view",
	    "SELECT * FROM (VALUES (NULL), ('[[5, 5], NULL]')) t(a)"));
}

TEST_CASE("Arrow scan of a NULL FIXED_SIZE_LIST row three levels above the values", "[arrow]") {
	// the middle level has its own NULL, so the NULL of row 0 has to pass through it to the inner level
	arrow_fixed_size_list_null_children_test::StringColumn strings(
	    {"1000", "1000", "1000", "1000", "5", "5", "5", "5"});
	arrow_fixed_size_list_null_children_test::NestedColumn innermost(strings, "+w:1", 8, {});
	arrow_fixed_size_list_null_children_test::NestedColumn middle(innermost, "+w:2", 4, {3});
	arrow_fixed_size_list_null_children_test::NestedColumn outer(middle, "+w:2", 2, {0});
	REQUIRE(arrow_fixed_size_list_null_children_test::QueryMatches(
	    outer.schema, outer.array, "SELECT a::TINYINT[1][2][2]::VARCHAR FROM arrow_view",
	    "SELECT * FROM (VALUES (NULL), ('[[[5], [5]], NULL]')) t(a)"));
}

TEST_CASE("Arrow scan of a NULL FIXED_SIZE_LIST row at an array offset", "[arrow]") {
	// the scan starts at row 1, which is NULL over ['1000', '1000'] and ['5', '5']
	arrow_fixed_size_list_null_children_test::StringColumn strings(
	    {"5", "5", "5", "5", "1000", "1000", "5", "5", "5", "5", "5", "5"});
	arrow_fixed_size_list_null_children_test::NestedColumn inner(strings, "+w:2", 6, {5});
	arrow_fixed_size_list_null_children_test::NestedColumn outer(inner, "+w:2", 2, {1}, {}, 1);
	REQUIRE(arrow_fixed_size_list_null_children_test::QueryMatches(
	    outer.schema, outer.array, "SELECT a::TINYINT[2][2]::VARCHAR FROM arrow_view",
	    "SELECT * FROM (VALUES (NULL), ('[[5, 5], NULL]')) t(a)"));
}
