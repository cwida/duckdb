// Arrow scans of time32 and time64 values: a count within a day reads as a TIME, a count outside of it is an error.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"

#include <string>
#include <vector>

using namespace duckdb;

namespace arrow_time_range_test {

void ReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void ReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

const int64_t HUNDRED_HOURS_US = int64_t(100) * 3600 * 1000000;

using rows = std::vector<string>;

std::vector<uint8_t> ValidityBits(idx_t count, const std::vector<idx_t> &null_rows) {
	std::vector<uint8_t> validity((count + 7) / 8, 0xFF);
	for (auto row : null_rows) {
		validity[row / 8] &= uint8_t(~(1u << (row % 8)));
	}
	return validity;
}

template <class T>
struct TimeColumn {
	TimeColumn(const char *format, const std::vector<T> &values_p, const std::vector<idx_t> &null_rows = {},
	           int64_t offset = 0)
	    : values(values_p), validity(ValidityBits(values_p.size(), null_rows)) {
		schema.format = format;
		schema.name = "t";
		schema.flags = ARROW_FLAG_NULLABLE;
		schema.release = ReleaseSchema;
		buffers[0] = null_rows.empty() ? nullptr : validity.data();
		buffers[1] = values.data();
		array.length = int64_t(values.size()) - offset;
		array.offset = offset;
		array.null_count = null_rows.empty() ? 0 : -1;
		array.n_buffers = 2;
		array.buffers = buffers;
		array.release = ReleaseArray;
	}
	TimeColumn(const TimeColumn &) = delete;

	std::vector<T> values;
	std::vector<uint8_t> validity;
	const void *buffers[2];
	ArrowSchema schema {};
	ArrowArray array {};
};

template <class T>
struct DictionaryColumn {
	DictionaryColumn(const char *format, const std::vector<T> &dictionary_values, const std::vector<int8_t> &indices_p,
	                 const std::vector<idx_t> &null_entries = {})
	    : values(format, dictionary_values, null_entries), indices(indices_p) {
		schema.format = "c";
		schema.name = "t";
		schema.flags = ARROW_FLAG_NULLABLE;
		schema.dictionary = &values.schema;
		schema.release = ReleaseSchema;
		buffers[0] = nullptr;
		buffers[1] = indices.data();
		array.length = int64_t(indices.size());
		array.n_buffers = 2;
		array.buffers = buffers;
		array.dictionary = &values.array;
		array.release = ReleaseArray;
	}
	DictionaryColumn(const DictionaryColumn &) = delete;

	TimeColumn<T> values;
	std::vector<int8_t> indices;
	const void *buffers[2];
	ArrowSchema schema {};
	ArrowArray array {};
};

template <class T>
struct RunEndColumn {
	RunEndColumn(const char *format, const std::vector<int32_t> &run_ends_p, const std::vector<T> &run_values,
	             const std::vector<idx_t> &null_runs = {}, int64_t offset = 0)
	    : run_ends(run_ends_p), values(format, run_values, null_runs) {
		run_ends_schema.format = "i";
		run_ends_schema.name = "run_ends";
		run_ends_schema.release = ReleaseSchema;
		values.schema.name = "values";
		schema_children[0] = &run_ends_schema;
		schema_children[1] = &values.schema;
		schema.format = "+r";
		schema.name = "t";
		schema.flags = ARROW_FLAG_NULLABLE;
		schema.n_children = 2;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		run_ends_buffers[0] = nullptr;
		run_ends_buffers[1] = run_ends.data();
		run_ends_array.length = int64_t(run_ends.size());
		run_ends_array.n_buffers = 2;
		run_ends_array.buffers = run_ends_buffers;
		run_ends_array.release = ReleaseArray;
		array_children[0] = &run_ends_array;
		array_children[1] = &values.array;
		array.length = run_ends.back() - offset;
		array.offset = offset;
		array.n_children = 2;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	RunEndColumn(const RunEndColumn &) = delete;

	std::vector<int32_t> run_ends;
	TimeColumn<T> values;
	const void *run_ends_buffers[2];
	ArrowSchema *schema_children[2];
	ArrowArray *array_children[2];
	ArrowSchema run_ends_schema {};
	ArrowSchema schema {};
	ArrowArray run_ends_array {};
	ArrowArray array {};
};

template <class T>
struct ListColumn {
	ListColumn(const char *format, const std::vector<T> &child_values, const std::vector<int32_t> &offsets_p,
	           const std::vector<idx_t> &null_rows = {})
	    : child(format, child_values), offsets(offsets_p), validity(ValidityBits(offsets_p.size() - 1, null_rows)) {
		child.schema.name = "item";
		schema_children[0] = &child.schema;
		schema.format = "+l";
		schema.name = "t";
		schema.flags = ARROW_FLAG_NULLABLE;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		buffers[0] = null_rows.empty() ? nullptr : validity.data();
		buffers[1] = offsets.data();
		array_children[0] = &child.array;
		array.length = int64_t(offsets.size()) - 1;
		array.null_count = null_rows.empty() ? 0 : -1;
		array.n_buffers = 2;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	ListColumn(const ListColumn &) = delete;

	TimeColumn<T> child;
	std::vector<int32_t> offsets;
	std::vector<uint8_t> validity;
	const void *buffers[2];
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	ArrowSchema schema {};
	ArrowArray array {};
};

template <class T>
struct StructColumn {
	StructColumn(const char *format, const std::vector<T> &child_values, const std::vector<idx_t> &null_rows)
	    : child(format, child_values), validity(ValidityBits(child_values.size(), null_rows)) {
		child.schema.name = "x";
		schema_children[0] = &child.schema;
		schema.format = "+s";
		schema.name = "t";
		schema.flags = ARROW_FLAG_NULLABLE;
		schema.n_children = 1;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		buffers[0] = validity.data();
		array_children[0] = &child.array;
		array.length = int64_t(child_values.size());
		array.null_count = -1;
		array.n_buffers = 1;
		array.buffers = buffers;
		array.n_children = 1;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	StructColumn(const StructColumn &) = delete;

	TimeColumn<T> child;
	std::vector<uint8_t> validity;
	const void *buffers[1];
	ArrowSchema *schema_children[1];
	ArrowArray *array_children[1];
	ArrowSchema schema {};
	ArrowArray array {};
};

// Sparse UNION(t TIME, i INTEGER) where slot j selects member type_ids[j].
struct SparseUnionColumn {
	SparseUnionColumn(const std::vector<int8_t> &type_ids_p, const std::vector<int64_t> &time_values,
	                  const std::vector<int32_t> &int_values)
	    : type_ids(type_ids_p), times("ttu", time_values), ints("i", int_values) {
		ints.schema.name = "i";
		schema_children[0] = &times.schema;
		schema_children[1] = &ints.schema;
		schema.format = "+us:0,1";
		schema.name = "t";
		schema.n_children = 2;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		buffers[0] = type_ids.data();
		array_children[0] = &times.array;
		array_children[1] = &ints.array;
		array.length = int64_t(type_ids.size());
		array.n_buffers = 1;
		array.buffers = buffers;
		array.n_children = 2;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	SparseUnionColumn(const SparseUnionColumn &) = delete;

	std::vector<int8_t> type_ids;
	TimeColumn<int64_t> times;
	TimeColumn<int32_t> ints;
	const void *buffers[1];
	ArrowSchema *schema_children[2];
	ArrowArray *array_children[2];
	ArrowSchema schema {};
	ArrowArray array {};
};

unique_ptr<QueryResult> Scan(Connection &con, ArrowSchema &col_schema, ArrowArray &col_array,
                             const string &projection) {
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
	REQUIRE(duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &stream, &err) == ADBC_STATUS_OK);
	auto result = con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream))
	                  ->Project(projection)
	                  ->Execute();
	stream.release = nullptr;
	return result;
}

template <class COLUMN>
rows ScanText(COLUMN &col) {
	DuckDB db(nullptr);
	Connection con(db);
	auto result = Scan(con, col.schema, col.array, "t::VARCHAR");
	REQUIRE_NO_FAIL(*result);
	rows text;
	for (idx_t row = 0; row < result->RowCount(); row++) {
		text.push_back(result->Collection().GetValue(0, row).ToString());
	}
	return text;
}

template <class COLUMN>
void RequireOutOfRange(COLUMN &col, const string &projection = "t::VARCHAR") {
	DuckDB db(nullptr);
	Connection con(db);
	auto result = Scan(con, col.schema, col.array, projection);
	REQUIRE(result->HasError());
	REQUIRE(result->GetErrorType() == ExceptionType::CONVERSION);
	REQUIRE_THAT(result->GetError(), Catch::Matchers::Contains("TIME values must be within a day"));
}

// Counts of exactly a day are outside Arrow's own range [0, a day), but a DuckDB TIME holds 24:00:00.
TEST_CASE("Arrow scan of times at the bounds of a day", "[arrow]") {
	TimeColumn<int32_t> seconds("tts", {0, 86399, 86400});
	REQUIRE(ScanText(seconds) == rows({"00:00:00", "23:59:59", "24:00:00"}));
	TimeColumn<int32_t> millis("ttm", {0, 86399999, 86400000});
	REQUIRE(ScanText(millis) == rows({"00:00:00", "23:59:59.999", "24:00:00"}));
	TimeColumn<int64_t> micros("ttu", {0, 86399999999, 86400000000});
	REQUIRE(ScanText(micros) == rows({"00:00:00", "23:59:59.999999", "24:00:00"}));
	TimeColumn<int64_t> nanos("ttn", {0, 86399999999999, 86400000000000});
	REQUIRE(ScanText(nanos) == rows({"00:00:00", "23:59:59.999999999", "24:00:00"}));
}

TEST_CASE("Arrow scan of a time of a hundred hours", "[arrow]") {
	TimeColumn<int32_t> seconds("tts", {0, 360000});
	RequireOutOfRange(seconds);
	{
		DuckDB db(nullptr);
		Connection con(db);
		auto result = Scan(con, seconds.schema, seconds.array, "t");
		REQUIRE_THAT(
		    result->GetError(),
		    Catch::Matchers::Contains("Invalid TIME value 360000 in Arrow array - TIME values must be within a day"));
	}
	TimeColumn<int32_t> millis("ttm", {0, 360000000});
	RequireOutOfRange(millis);
	TimeColumn<int64_t> micros("ttu", {0, HUNDRED_HOURS_US});
	RequireOutOfRange(micros);
	// the error comes from the scan, before any formatting
	RequireOutOfRange(micros, "hour(t)");
	TimeColumn<int64_t> nanos("ttn", {0, HUNDRED_HOURS_US * 1000});
	RequireOutOfRange(nanos);
}

TEST_CASE("Arrow scan of a time one unit past a day or before midnight", "[arrow]") {
	TimeColumn<int32_t> seconds_past("tts", {86401});
	RequireOutOfRange(seconds_past);
	TimeColumn<int32_t> millis_past("ttm", {86400001});
	RequireOutOfRange(millis_past);
	TimeColumn<int64_t> micros_past("ttu", {86400000001});
	RequireOutOfRange(micros_past);
	TimeColumn<int64_t> nanos_past("ttn", {86400000000001});
	RequireOutOfRange(nanos_past);
	TimeColumn<int32_t> seconds_negative("tts", {-1});
	RequireOutOfRange(seconds_negative);
	TimeColumn<int32_t> millis_negative("ttm", {-1});
	RequireOutOfRange(millis_negative);
	TimeColumn<int64_t> micros_negative("ttu", {-1});
	RequireOutOfRange(micros_negative);
	TimeColumn<int64_t> nanos_negative("ttn", {-1});
	RequireOutOfRange(nanos_negative);
}

TEST_CASE("Arrow scan of times at the limits of their integer type", "[arrow]") {
	TimeColumn<int64_t> micros("ttu", {NumericLimits<int64_t>::Maximum()});
	RequireOutOfRange(micros);
	TimeColumn<int64_t> nanos("ttn", {NumericLimits<int64_t>::Minimum()});
	RequireOutOfRange(nanos);
	TimeColumn<int32_t> seconds("tts", {NumericLimits<int32_t>::Maximum()});
	RequireOutOfRange(seconds);
}

TEST_CASE("Arrow scan of an out of range time behind a NULL or an offset", "[arrow]") {
	TimeColumn<int64_t> null_row("ttu", {HUNDRED_HOURS_US, 3600000000, -1}, {0, 2});
	REQUIRE(ScanText(null_row) == rows({"NULL", "01:00:00", "NULL"}));
	TimeColumn<int64_t> null_nanos("ttn", {HUNDRED_HOURS_US * 1000, 1000}, {0});
	REQUIRE(ScanText(null_nanos) == rows({"NULL", "00:00:00.000001"}));
	TimeColumn<int32_t> skipped("tts", {360000, 3600, 7200}, {}, 1);
	REQUIRE(ScanText(skipped) == rows({"01:00:00", "02:00:00"}));
	TimeColumn<int32_t> after_offset("tts", {3600, 7200, 360000}, {}, 1);
	RequireOutOfRange(after_offset);
}

TEST_CASE("Arrow scan of an out of range time beside a NULL", "[arrow]") {
	TimeColumn<int64_t> micros("ttu", {3600000000, HUNDRED_HOURS_US, 86400000000}, {1});
	REQUIRE(ScanText(micros) == rows({"01:00:00", "NULL", "24:00:00"}));
	TimeColumn<int64_t> micros_past("ttu", {3600000000, 0, HUNDRED_HOURS_US}, {1});
	RequireOutOfRange(micros_past);
	TimeColumn<int64_t> micros_negative("ttu", {3600000000, 0, -1}, {1});
	RequireOutOfRange(micros_negative);
	TimeColumn<int32_t> seconds_past("tts", {3600, 0, 360000}, {1});
	RequireOutOfRange(seconds_past);
	TimeColumn<int64_t> nanos("ttn", {1000, HUNDRED_HOURS_US * 1000, 86400000000000}, {1});
	REQUIRE(ScanText(nanos) == rows({"00:00:00.000001", "NULL", "24:00:00"}));
	TimeColumn<int64_t> nanos_past("ttn", {1000, 0, HUNDRED_HOURS_US * 1000}, {1});
	RequireOutOfRange(nanos_past);
	TimeColumn<int64_t> nanos_negative("ttn", {1000, 0, -1}, {1});
	RequireOutOfRange(nanos_negative);
}

TEST_CASE("Arrow scan of an out of range time past the first chunk", "[arrow]") {
	std::vector<int32_t> values(3000, 3600);
	values[2500] = 360000;
	TimeColumn<int32_t> out_of_range("tts", values);
	RequireOutOfRange(out_of_range);
	TimeColumn<int32_t> null_row("tts", values, {2500});
	auto text = ScanText(null_row);
	REQUIRE(text.size() == 3000);
	REQUIRE(text[2499] == "01:00:00");
	REQUIRE(text[2500] == "NULL");
	REQUIRE(text[2501] == "01:00:00");
}

TEST_CASE("Arrow scan of dictionary encoded times", "[arrow]") {
	DictionaryColumn<int64_t> valid("ttu", {0, 86400000000}, {1, 0, 1});
	REQUIRE(ScanText(valid) == rows({"24:00:00", "00:00:00", "24:00:00"}));
	DictionaryColumn<int64_t> referenced("ttu", {0, HUNDRED_HOURS_US}, {0, 1});
	RequireOutOfRange(referenced);
	// the dictionary is converted as a whole, so an entry no index points at is still checked
	DictionaryColumn<int64_t> unreferenced("ttu", {0, HUNDRED_HOURS_US}, {0, 0});
	RequireOutOfRange(unreferenced);
	DictionaryColumn<int64_t> null_entry("ttu", {0, HUNDRED_HOURS_US}, {1, 0}, {1});
	REQUIRE(ScanText(null_entry) == rows({"NULL", "00:00:00"}));
}

TEST_CASE("Arrow scan of run-end encoded times", "[arrow]") {
	RunEndColumn<int32_t> valid("tts", {2, 3}, {86400, 0});
	REQUIRE(ScanText(valid) == rows({"24:00:00", "24:00:00", "00:00:00"}));
	RunEndColumn<int32_t> out_of_range("tts", {2, 3}, {0, 360000});
	RequireOutOfRange(out_of_range);
	RunEndColumn<int32_t> null_run("tts", {2, 3}, {360000, 60}, {0});
	REQUIRE(ScanText(null_run) == rows({"NULL", "NULL", "00:01:00"}));
	// the run values are converted as a whole, so a run outside the offset window is still checked
	RunEndColumn<int32_t> sliced("tts", {2, 4}, {360000, 3600}, {}, 2);
	RequireOutOfRange(sliced);
}

TEST_CASE("Arrow scan of times nested in lists and structs and unions", "[arrow]") {
	ListColumn<int64_t> valid_list("ttu", {0, 86400000000, 3600000000}, {0, 2, 3});
	REQUIRE(ScanText(valid_list) == rows({"['00:00:00', '24:00:00']", "['01:00:00']"}));
	ListColumn<int64_t> out_of_range_list("ttu", {0, HUNDRED_HOURS_US, 3600000000}, {0, 2, 3});
	RequireOutOfRange(out_of_range_list);
	// the child of a list is converted as a whole, so a segment under a NULL list entry is still checked
	ListColumn<int64_t> null_list("ttu", {0, HUNDRED_HOURS_US, 3600000000}, {0, 2, 3}, {0});
	RequireOutOfRange(null_list);
	StructColumn<int64_t> null_struct("ttu", {HUNDRED_HOURS_US, 0}, {0});
	REQUIRE(ScanText(null_struct) == rows({"NULL", "{'x': '00:00:00'}"}));
	SparseUnionColumn selected({0, 1}, {HUNDRED_HOURS_US, 0}, {5, 6});
	RequireOutOfRange(selected);
	// every member of a sparse union is converted over all slots, so a slot no row selects is still checked
	SparseUnionColumn unselected({1, 0}, {HUNDRED_HOURS_US, 3600000000}, {5, 6});
	RequireOutOfRange(unselected);
	SparseUnionColumn valid_union({1, 0}, {0, 3600000000}, {5, 6});
	REQUIRE(ScanText(valid_union) == rows({"5", "01:00:00"}));
}

} // namespace arrow_time_range_test
