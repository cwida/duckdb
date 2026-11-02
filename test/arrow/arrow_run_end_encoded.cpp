// Arrow scans of run-end encoded columns, at the top level and as children of LIST and FIXED_SIZE_LIST.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "compare_result.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"

#include <string>
#include <vector>

using namespace duckdb;

namespace arrow_run_end_encoded_test {

void ReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void ReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

// Run-end encoded int32 column: run i ends at run_ends[i] and holds values[i], NULL for the given runs.
struct RunEndColumn {
	RunEndColumn(const std::vector<int32_t> &run_ends_p, const std::vector<int32_t> &values_p,
	             const std::vector<idx_t> &null_runs)
	    : run_ends(run_ends_p), values(values_p), validity((values_p.size() + 7) / 8 + 2, 0xFF) {
		for (auto run : null_runs) {
			validity[run / 8] &= uint8_t(~(1u << (run % 8)));
		}
		auto run_count = int64_t(run_ends.size());

		run_ends_schema.format = "i";
		run_ends_schema.name = "run_ends";
		run_ends_schema.release = ReleaseSchema;
		values_schema.format = "i";
		values_schema.name = "values";
		values_schema.flags = 2; // ARROW_FLAG_NULLABLE
		values_schema.release = ReleaseSchema;
		schema_children[0] = &run_ends_schema;
		schema_children[1] = &values_schema;
		schema.format = "+r";
		schema.name = "item";
		schema.flags = 2;
		schema.n_children = 2;
		schema.children = schema_children;
		schema.release = ReleaseSchema;

		run_ends_buffers[0] = nullptr;
		run_ends_buffers[1] = run_ends.data();
		run_ends_array.length = run_count;
		run_ends_array.n_buffers = 2;
		run_ends_array.buffers = run_ends_buffers;
		run_ends_array.release = ReleaseArray;
		values_buffers[0] = validity.data();
		values_buffers[1] = values.data();
		values_array.length = run_count;
		values_array.null_count = int64_t(null_runs.size());
		values_array.n_buffers = 2;
		values_array.buffers = values_buffers;
		values_array.release = ReleaseArray;
		array_children[0] = &run_ends_array;
		array_children[1] = &values_array;
		array.length = run_ends.empty() ? 0 : run_ends.back();
		array.n_buffers = 1;
		array.buffers = buffers;
		array.n_children = 2;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	RunEndColumn(const RunEndColumn &) = delete;

	std::vector<int32_t> run_ends;
	std::vector<int32_t> values;
	std::vector<uint8_t> validity;
	const void *run_ends_buffers[2];
	const void *values_buffers[2];
	const void *buffers[1] = {nullptr};
	ArrowSchema *schema_children[2];
	ArrowArray *array_children[2];
	ArrowSchema run_ends_schema {};
	ArrowSchema values_schema {};
	ArrowSchema schema {};
	ArrowArray run_ends_array {};
	ArrowArray values_array {};
	ArrowArray array {};
};

// LIST column over a run-end encoded or fixed-size list child with the given offsets.
struct ListColumn {
	template <class CHILD>
	ListColumn(CHILD &child, const std::vector<int32_t> &offsets_p) : offsets(offsets_p) {
		schema_children[0] = &child.schema;
		schema.format = "+l";
		schema.name = "a";
		schema.flags = 2;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		buffers[0] = nullptr;
		buffers[1] = offsets.data();
		array_children[0] = &child.array;
		array.length = int64_t(offsets.size() - 1);
		array.n_buffers = 2;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	ListColumn(const ListColumn &) = delete;

	std::vector<int32_t> offsets;
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	const void *buffers[2];
	ArrowSchema schema {};
	ArrowArray array {};
};

// FIXED_SIZE_LIST column of list_size elements over a run-end encoded child, starting at row offset, NULL for the
// given rows.
struct FixedSizeListColumn {
	FixedSizeListColumn(RunEndColumn &child, idx_t list_size, int64_t length, int64_t offset,
	                    const std::vector<idx_t> &null_rows = {})
	    : format("+w:" + std::to_string(list_size)), validity((length + offset + 7) / 8 + 1, 0xFF) {
		for (auto row : null_rows) {
			auto bit = row + idx_t(offset);
			validity[bit / 8] &= uint8_t(~(1u << (bit % 8)));
		}
		buffers[0] = null_rows.empty() ? nullptr : validity.data();
		schema_children[0] = &child.schema;
		schema.format = format.c_str();
		schema.name = "a";
		schema.flags = 2;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		array_children[0] = &child.array;
		array.length = length;
		array.offset = offset;
		array.null_count = int64_t(null_rows.size());
		array.n_buffers = 1;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	FixedSizeListColumn(const FixedSizeListColumn &) = delete;

	std::string format;
	std::vector<uint8_t> validity;
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	const void *buffers[1];
	ArrowSchema schema {};
	ArrowArray array {};
};

// Calls scan with a stream of one record batch that holds the column.
template <class SCAN>
bool WithStream(ArrowSchema &col_schema, ArrowArray &col_array, SCAN scan) {
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
	return scan(stream);
}

bool ScanMatches(ArrowSchema &col_schema, ArrowArray &col_array, const string &query) {
	return WithStream(col_schema, col_array, [&](ArrowArrayStream &stream) {
		DuckDB db(nullptr);
		Connection con(db);
		return ArrowTestHelper::RunArrowComparison(con, query, stream);
	});
}

// Runs the query over the scan of the column, as the view arrow_view, and checks that it returns expected.
bool QueryMatches(ArrowSchema &col_schema, ArrowArray &col_array, const string &query, const string &expected) {
	return WithStream(col_schema, col_array, [&](ArrowArrayStream &stream) {
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
	});
}

// Run-end encoded utf8 column, whose run i ends at run_ends[i] and holds values[i].
struct RunEndStringColumn {
	RunEndStringColumn(const char *name, const std::vector<int32_t> &run_ends_p, const std::vector<string> &values_p)
	    : run_ends(run_ends_p) {
		offsets.push_back(0);
		for (auto &value : values_p) {
			data += value;
			offsets.push_back(int32_t(data.size()));
		}
		auto run_count = int64_t(run_ends.size());

		run_ends_schema.format = "i";
		run_ends_schema.name = "run_ends";
		run_ends_schema.release = ReleaseSchema;
		values_schema.format = "u";
		values_schema.name = "values";
		values_schema.flags = 2; // ARROW_FLAG_NULLABLE
		values_schema.release = ReleaseSchema;
		schema_children[0] = &run_ends_schema;
		schema_children[1] = &values_schema;
		schema.format = "+r";
		schema.name = name;
		schema.flags = 2;
		schema.n_children = 2;
		schema.children = schema_children;
		schema.release = ReleaseSchema;

		run_ends_buffers[0] = nullptr;
		run_ends_buffers[1] = run_ends.data();
		run_ends_array.length = run_count;
		run_ends_array.n_buffers = 2;
		run_ends_array.buffers = run_ends_buffers;
		run_ends_array.release = ReleaseArray;
		values_buffers[0] = nullptr;
		values_buffers[1] = offsets.data();
		values_buffers[2] = data.data();
		values_array.length = run_count;
		values_array.n_buffers = 3;
		values_array.buffers = values_buffers;
		values_array.release = ReleaseArray;
		array_children[0] = &run_ends_array;
		array_children[1] = &values_array;
		array.length = run_ends.empty() ? 0 : run_ends.back();
		array.n_children = 2;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	RunEndStringColumn(const RunEndStringColumn &) = delete;

	std::vector<int32_t> run_ends;
	std::vector<int32_t> offsets;
	string data;
	const void *run_ends_buffers[2];
	const void *values_buffers[3];
	ArrowSchema *schema_children[2];
	ArrowArray *array_children[2];
	ArrowSchema run_ends_schema {};
	ArrowSchema values_schema {};
	ArrowSchema schema {};
	ArrowArray run_ends_array {};
	ArrowArray values_array {};
	ArrowArray array {};
};

// int32 column without NULLs.
struct Int32Column {
	Int32Column(const char *name, const std::vector<int32_t> &values_p) : values(values_p) {
		schema.format = "i";
		schema.name = name;
		schema.flags = 2;
		schema.release = ReleaseSchema;
		buffers[0] = nullptr;
		buffers[1] = values.data();
		array.length = int64_t(values.size());
		array.n_buffers = 2;
		array.buffers = buffers;
		array.release = ReleaseArray;
	}
	Int32Column(const Int32Column &) = delete;

	std::vector<int32_t> values;
	const void *buffers[2];
	ArrowSchema schema {};
	ArrowArray array {};
};

// Runs the query on a view t that scans a record batch of the given columns with arrow_scan.
unique_ptr<QueryResult> QueryArrowTable(Connection &con, const std::vector<ArrowSchema *> &column_schemas,
                                        const std::vector<ArrowArray *> &column_arrays, const string &query) {
	std::vector<ArrowSchema *> schema_children(column_schemas);
	ArrowSchema record_schema {};
	record_schema.format = "+s";
	record_schema.n_children = int64_t(schema_children.size());
	record_schema.children = schema_children.data();
	record_schema.release = ReleaseSchema;

	std::vector<ArrowArray *> array_children(column_arrays);
	const void *buffers[1] = {nullptr};
	ArrowArray record_array {};
	record_array.length = array_children[0]->length;
	record_array.n_buffers = 1;
	record_array.buffers = buffers;
	record_array.n_children = int64_t(array_children.size());
	record_array.children = array_children.data();
	record_array.release = ReleaseArray;

	ArrowArrayStream stream {};
	AdbcError err {};
	REQUIRE(duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &stream, &err) == ADBC_STATUS_OK);
	con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream))->CreateView("t", true, true);
	return con.Query(query);
}

} // namespace arrow_run_end_encoded_test

TEST_CASE("Arrow scan of a run-end encoded column with NULL runs across chunks", "[arrow]") {
	arrow_run_end_encoded_test::RunEndColumn col({1000, 3000, 5000}, {1, 2, 3}, {1});
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(
	    col.schema, col.array,
	    "SELECT CASE WHEN r < 1000 THEN 1 WHEN r < 3000 THEN NULL ELSE 3 END::INT FROM range(5000) t(r)"));
}

TEST_CASE("Arrow scan of LIST(run-end encoded) with a nonzero leading child offset", "[arrow]") {
	// child: [1, 1, 1, NULL, NULL, 3, 3, 3], the lists start at child slot 2
	arrow_run_end_encoded_test::RunEndColumn child({3, 5, 8}, {1, 2, 3}, {1});
	arrow_run_end_encoded_test::ListColumn col(child, {2, 4, 6, 8});
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(
	    col.schema, col.array, "SELECT * FROM (VALUES ([1, NULL]::INT[]), ([NULL, 3]), ([3, 3]))"));
}

TEST_CASE("Arrow scan of LIST(run-end encoded) with leading empty lists", "[arrow]") {
	// child: [7, 7, NULL, 9]
	arrow_run_end_encoded_test::RunEndColumn child({2, 3, 4}, {7, 8, 9}, {1});
	arrow_run_end_encoded_test::ListColumn col(child, {0, 0, 0, 2, 4});
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(col.schema, col.array,
	                                                "SELECT * FROM (VALUES ([]::INT[]), ([]), ([7, 7]), ([NULL, 9]))"));
}

TEST_CASE("Arrow scan of LIST(run-end encoded) across chunks with a leading child offset", "[arrow]") {
	// child slot s holds s / 1000 + 1, NULL for slots [1000, 2000); row r covers child slots 3 + 2r and 4 + 2r
	arrow_run_end_encoded_test::RunEndColumn child({1000, 2000, 3000, 4000, 5000, 6003}, {1, 2, 3, 4, 5, 6}, {1});
	std::vector<int32_t> offsets;
	for (int32_t r = 0; r <= 3000; r++) {
		offsets.push_back(3 + 2 * r);
	}
	arrow_run_end_encoded_test::ListColumn col(child, offsets);
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(
	    col.schema, col.array,
	    "SELECT [CASE WHEN s // 1000 = 1 THEN NULL ELSE LEAST(s // 1000, 5) + 1 END::INT FOR s IN [3 + 2 * r, 4 + 2 * "
	    "r]] FROM range(3000) t(r)"));
}

TEST_CASE("Arrow scan of LIST(run-end encoded) whose first chunk holds no list elements", "[arrow]") {
	// child: [7, 7], the first 2048 lists are empty
	arrow_run_end_encoded_test::RunEndColumn child({2}, {7}, {});
	std::vector<int32_t> offsets(2049, 0);
	offsets.push_back(2);
	arrow_run_end_encoded_test::ListColumn col(child, offsets);
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(
	    col.schema, col.array, "SELECT CASE WHEN r < 2048 THEN [] ELSE [7, 7] END::INT[] FROM range(2049) t(r)"));
}

TEST_CASE("Arrow scan of FIXED_SIZE_LIST(run-end encoded)", "[arrow]") {
	// the child holds 10, 10, 10 and 20
	arrow_run_end_encoded_test::RunEndColumn child({3, 4}, {10, 20}, {});
	arrow_run_end_encoded_test::FixedSizeListColumn col(child, 2, 2, 0);
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(col.schema, col.array,
	                                                "SELECT * FROM (VALUES ([10, 10]::INT[2]), ([10, 20]))"));
}

TEST_CASE("Arrow scan of FIXED_SIZE_LIST(run-end encoded) across chunks with an offset", "[arrow]") {
	// child slot s holds s / 1000 + 1, and NULL for slots 1000 to 1999, and row r covers child slots 2 + 2r and 3 + 2r
	arrow_run_end_encoded_test::RunEndColumn child({1000, 2000, 3000, 4000, 5000, 6002}, {1, 2, 3, 4, 5, 6}, {1});
	arrow_run_end_encoded_test::FixedSizeListColumn col(child, 2, 3000, 1);
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(
	    col.schema, col.array,
	    "SELECT [CASE WHEN s // 1000 = 1 THEN NULL ELSE LEAST(s // 1000, 5) + 1 END::INT FOR s IN [2 + 2 * r, 3 + 2 * "
	    "r]]::INT[2] FROM range(3000) t(r)"));
}

TEST_CASE("Arrow scan of FIXED_SIZE_LIST(run-end encoded) with NULL rows over values", "[arrow]") {
	// the child holds 1000, 1000, NULL, NULL, 5 and 5, and the first row, over the two values 1000, is NULL
	arrow_run_end_encoded_test::RunEndColumn child({2, 4, 6}, {1000, 0, 5}, {1});
	arrow_run_end_encoded_test::FixedSizeListColumn col(child, 2, 3, 0, {0});
	REQUIRE(arrow_run_end_encoded_test::QueryMatches(
	    col.schema, col.array, "SELECT a::TINYINT[2] FROM arrow_view",
	    "SELECT * FROM (VALUES (NULL::TINYINT[2]), ([NULL, NULL]::TINYINT[2]), ([5, 5]::TINYINT[2]))"));
}

TEST_CASE("Arrow scan of LIST(run-end encoded) with only empty lists", "[arrow]") {
	arrow_run_end_encoded_test::RunEndColumn child({2}, {7}, {});
	arrow_run_end_encoded_test::ListColumn col(child, {0, 0, 0});
	REQUIRE(arrow_run_end_encoded_test::ScanMatches(col.schema, col.array, "SELECT * FROM (VALUES ([]::INT[]), ([]))"));
}

TEST_CASE("Arrow scan of LIST(FIXED_SIZE_LIST(run-end encoded)) with only empty lists", "[arrow]") {
	arrow_run_end_encoded_test::RunEndColumn child({2}, {7}, {});
	arrow_run_end_encoded_test::FixedSizeListColumn fixed_size_list(child, 2, 1, 0);
	arrow_run_end_encoded_test::ListColumn col(fixed_size_list, {0, 0, 0});
	REQUIRE(
	    arrow_run_end_encoded_test::ScanMatches(col.schema, col.array, "SELECT * FROM (VALUES ([]::INT[2][]), ([]))"));
}

TEST_CASE("Arrow scan of a table with a run-end encoded column and a filter", "[arrow]") {
	DuckDB db(nullptr);
	Connection con(db);
	// PyArrow cannot filter a record batch with a run-end encoded column, so the filters stay above the scan
	std::vector<string> queries {"SELECT i FROM t WHERE i > 1 ORDER BY i", "SELECT i FROM t WHERE r = 'a' ORDER BY i",
	                             "SELECT i FROM t WHERE i > 1 AND r = 'b' ORDER BY i"};
	vector<vector<Value>> expected {{2, 3}, {1, 2}, {3}};
	for (idx_t q = 0; q < queries.size(); q++) {
		arrow_run_end_encoded_test::Int32Column i("i", {1, 2, 3});
		arrow_run_end_encoded_test::RunEndStringColumn r("r", {2, 3}, {"a", "b"});
		auto result =
		    arrow_run_end_encoded_test::QueryArrowTable(con, {&i.schema, &r.schema}, {&i.array, &r.array}, queries[q]);
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == expected[q].size());
		REQUIRE(CHECK_COLUMN(result, 0, expected[q]));
	}
	arrow_run_end_encoded_test::Int32Column i("i", {1, 2, 3});
	arrow_run_end_encoded_test::RunEndColumn r({2, 3}, {10, 20}, {});
	auto result = arrow_run_end_encoded_test::QueryArrowTable(con, {&i.schema, &r.schema}, {&i.array, &r.array},
	                                                          "SELECT i, item FROM t WHERE i > 1 ORDER BY i");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 2);
	REQUIRE(CHECK_COLUMN(result, 0, {2, 3}));
	REQUIRE(CHECK_COLUMN(result, 1, {10, 20}));

	// a run-end encoded child of a list counts as well, child [7, 7, 8, 9]
	arrow_run_end_encoded_test::Int32Column j("i", {1, 2, 3});
	arrow_run_end_encoded_test::RunEndColumn child({2, 3, 4}, {7, 8, 9}, {});
	arrow_run_end_encoded_test::ListColumn list(child, {0, 2, 3, 4});
	result = arrow_run_end_encoded_test::QueryArrowTable(con, {&j.schema, &list.schema}, {&j.array, &list.array},
	                                                     "SELECT i, a FROM t WHERE i > 1 ORDER BY i");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == 2);
	REQUIRE(CHECK_COLUMN(result, 0, {2, 3}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::LIST({Value::INTEGER(8)}), Value::LIST({Value::INTEGER(9)})}));
}
