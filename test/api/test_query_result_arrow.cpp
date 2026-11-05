#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/main/query_result_stream.hpp"

using namespace duckdb;

#ifndef DUCKDB_NO_THREADS

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/arrow/arrow_format.hpp"
#include "duckdb/main/buffered_data/batched_buffered_data.hpp"
#include "duckdb/main/buffered_data/simple_buffered_data.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "result_wait_helpers.hpp"

namespace {

//! A submitted handle in the Arrow format, ready for a stream
unique_ptr<QueryResult> SubmitArrow(Connection &con, const string &query, idx_t batch_size) {
	auto handle = con.Submit(query, make_shared_ptr<ArrowFormat>(batch_size));
	REQUIRE(!handle->HasError());
	return handle;
}

idx_t Rows(const ArrowArrayWrapper &array) {
	return NumericCast<idx_t>(array.arrow_array.length);
}

vector<unique_ptr<ArrowArrayWrapper>> DrainArrays(QueryResultStream<ArrowFormat> &stream) {
	vector<unique_ptr<ArrowArrayWrapper>> arrays;
	while (auto array = stream.Fetch()) {
		REQUIRE(array->arrow_array.release != nullptr);
		REQUIRE(Rows(*array) > 0);
		arrays.push_back(std::move(array));
	}
	REQUIRE(!stream.HasError());
	return arrays;
}

template <class ARRAYS>
idx_t TotalRows(const ARRAYS &arrays) {
	idx_t rows = 0;
	for (auto &array : arrays) {
		rows += Rows(*array);
	}
	return rows;
}

//! Runs the query in chunks and drives the Arrow format over them on this thread, so a test can read
//! the units the buffer would count before they are unpacked
vector<unique_ptr<ResultUnit>> FormatUnits(Connection &con, const string &query, idx_t batch_size) {
	auto result = con.Query(query);
	REQUIRE_NO_FAIL(*result);
	ArrowFormat format(batch_size);
	ResultFormatContext context {result->GetTypes(), result->GetNames(), con.context->GetClientProperties(),
	                             ResultOrdering::UNORDERED};
	auto gstate = format.InitGlobal(context);
	auto lstate = format.InitLocal(*gstate);
	vector<unique_ptr<ResultUnit>> units;
	while (auto chunk = result->Fetch()) {
		format.AppendToUnit(*gstate, *lstate, *chunk);
		while (format.IsUnitFinished(*lstate)) {
			units.push_back(format.FinishUnit(*gstate, *lstate));
		}
	}
	while (auto unit = format.FinishUnit(*gstate, *lstate)) {
		units.push_back(std::move(unit));
	}
	return units;
}

//! Serves Arrow arrays that are already in hand as an ArrowArrayStream, so arrow_scan can read them
//! back the way test/arrow does
class ServedArrays {
public:
	ServedArrays(vector<LogicalType> types_p, vector<string> names_p, ClientProperties properties_p,
	             vector<unique_ptr<ArrowArrayWrapper>> arrays_p)
	    : types(std::move(types_p)), names(std::move(names_p)), properties(std::move(properties_p)),
	      arrays(std::move(arrays_p)) {
		stream.private_data = this;
		stream.get_schema = GetSchema;
		stream.get_next = GetNext;
		stream.get_last_error = GetLastError;
		stream.release = Release;
	}

public:
	//! The rows of these arrays, scanned back through arrow_scan
	unique_ptr<QueryResult> Scan(Connection &con) {
		auto params = ArrowTestHelper::ConstructArrowScan(stream);
		return ArrowTestHelper::ScanArrowObject(con, params);
	}

private:
	static ServedArrays &Self(ArrowArrayStream *stream) {
		return *static_cast<ServedArrays *>(stream->private_data);
	}
	static int GetSchema(ArrowArrayStream *stream, ArrowSchema *out) {
		auto &self = Self(stream);
		ArrowConverter::ToArrowSchema(out, self.types, self.names, self.properties);
		return 0;
	}
	static int GetNext(ArrowArrayStream *stream, ArrowArray *out) {
		auto &self = Self(stream);
		out->release = nullptr;
		if (self.next == self.arrays.size()) {
			return 0;
		}
		self.arrays[self.next++]->MoveTo(*out);
		return 0;
	}
	static const char *GetLastError(ArrowArrayStream *stream) {
		return "";
	}
	// arrow_scan releases the arrays it took, and the ones never served go with this object
	static void Release(ArrowArrayStream *stream) {
		stream->release = nullptr;
	}

private:
	vector<LogicalType> types;
	vector<string> names;
	ClientProperties properties;
	vector<unique_ptr<ArrowArrayWrapper>> arrays;
	idx_t next = 0;
	ArrowArrayStream stream {};
};

//! The arrays of a stream, scanned back into a DuckDB result
unique_ptr<QueryResult> ScanBack(Connection &con, QueryResultStream<ArrowFormat> &stream,
                                 vector<unique_ptr<ArrowArrayWrapper>> arrays) {
	ServedArrays served(stream.GetTypes(), IdentifiersToStrings(stream.GetNames()), stream.GetClientProperties(),
	                    std::move(arrays));
	return served.Scan(con);
}

void RequireAscendingRows(QueryResult &result, idx_t expected_count) {
	REQUIRE(!result.HasError());
	auto &collection = result.Collection();
	REQUIRE(collection.Count() == expected_count);
	idx_t row = 0;
	for (auto &chunk_row : collection.Rows()) {
		auto value = chunk_row.GetValue(0);
		if (value.IsNull() || value.GetValue<int64_t>() != NumericCast<int64_t>(row)) {
			FAIL(StringUtil::Format("Out-of-order row %llu: %s", row, value.ToString()));
		}
		row++;
	}
	REQUIRE(row == expected_count);
}

} // namespace

TEST_CASE("A formatted Arrow stream drains an ordered plan in row order", "[api][query_result_arrow]") {
	constexpr idx_t ROWS = 50000;
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(50000)"));

	SECTION("through the simple store") {
		auto handle = SubmitArrow(con, "SELECT i FROM range(50000) t(i)", 4096);
		DrainWatchdog watchdog(con);
		QueryResultStream<ArrowFormat> stream(std::move(handle));
		REQUIRE_NOTHROW(stream.GetBufferedData().Cast<SimpleBufferedData>());
		auto arrays = DrainArrays(stream);
		REQUIRE(TotalRows(arrays) == ROWS);
		auto scanned = ScanBack(con, stream, std::move(arrays));
		RequireAscendingRows(*scanned, ROWS);
	}
	SECTION("through the batched store") {
		auto handle = SubmitArrow(con, "SELECT i FROM t", 4096);
		DrainWatchdog watchdog(con);
		QueryResultStream<ArrowFormat> stream(std::move(handle));
		REQUIRE_NOTHROW(stream.GetBufferedData().Cast<BatchedBufferedData>());
		auto arrays = DrainArrays(stream);
		REQUIRE(TotalRows(arrays) == ROWS);
		auto scanned = ScanBack(con, stream, std::move(arrays));
		RequireAscendingRows(*scanned, ROWS);
	}
}

TEST_CASE("An Arrow batch size smaller than a chunk fills every array but the last", "[api][query_result_arrow]") {
	constexpr idx_t ROWS = 20000;
	constexpr idx_t BATCH = 500;
	DuckDB db(nullptr);
	Connection con(db);
	// One producer and no batch boundaries, so the only array the format may cut short is the last
	REQUIRE_NO_FAIL(con.Query("SET threads=1"));
	REQUIRE_NO_FAIL(con.Query("SET preserve_insertion_order=false"));

	SECTION("under the default cap") {
		auto handle = SubmitArrow(con, "SELECT i FROM range(20000) t(i)", BATCH);
		DrainWatchdog watchdog(con);
		QueryResultStream<ArrowFormat> stream(std::move(handle));
		auto arrays = DrainArrays(stream);
		REQUIRE(TotalRows(arrays) == ROWS);
		REQUIRE(arrays.size() == ROWS / BATCH);
		for (auto &array : arrays) {
			REQUIRE(Rows(*array) == BATCH);
		}
	}
	SECTION("with a cap smaller than one array") {
		// Every unit exceeds the cap, so each one parks its producer and is deposited on the pop
		REQUIRE_NO_FAIL(con.Query("SET max_streaming_buffer_size='1b'"));
		auto handle = SubmitArrow(con, "SELECT i FROM range(20000) t(i)", BATCH);
		DrainWatchdog watchdog(con);
		QueryResultStream<ArrowFormat> stream(std::move(handle));
		auto arrays = DrainArrays(stream);
		REQUIRE(TotalRows(arrays) == ROWS);
		REQUIRE(stream.Poll() == QueryResultState::FINISHED);
	}
}

TEST_CASE("An Arrow batch larger than a row group gives one array per row group", "[api][query_result_arrow]") {
	constexpr idx_t GROUPS = 3;
	const idx_t rows = GROUPS * DEFAULT_ROW_GROUP_SIZE;
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format("CREATE TABLE t AS SELECT range i FROM range(%llu)", rows)));

	auto handle = SubmitArrow(con, "SELECT i FROM t", rows);
	DrainWatchdog watchdog(con);
	QueryResultStream<ArrowFormat> stream(std::move(handle));
	REQUIRE_NOTHROW(stream.GetBufferedData().Cast<BatchedBufferedData>());
	auto arrays = DrainArrays(stream);
	REQUIRE(TotalRows(arrays) == rows);
	// NextBatch finishes the array before the producer moves on, so none spans two row groups
	for (auto &array : arrays) {
		REQUIRE(Rows(*array) <= DEFAULT_ROW_GROUP_SIZE);
	}
	REQUIRE(arrays.size() == GROUPS);
}

TEST_CASE("Arrow extension types round-trip through the format", "[api][query_result_arrow]") {
	constexpr idx_t ROWS = 3000;
	DBConfig config;
	DuckDB db(nullptr, &config);
	Connection con(db);

	SECTION("UUID") {
		auto handle = SubmitArrow(con, "SELECT '4ac7a9e9-607c-4c8a-84f3-843f0191e3fd'::UUID u FROM range(3000)", 1024);
		DrainWatchdog watchdog(con);
		QueryResultStream<ArrowFormat> stream(std::move(handle));
		auto arrays = DrainArrays(stream);
		REQUIRE(TotalRows(arrays) == ROWS);
		auto scanned = ScanBack(con, stream, std::move(arrays));
		REQUIRE(!scanned->HasError());
		REQUIRE(scanned->Collection().Count() == ROWS);
		REQUIRE(scanned->Collection().GetValue(0, 0).ToString() == "4ac7a9e9-607c-4c8a-84f3-843f0191e3fd");
	}
	SECTION("JSON") {
		if (!db.ExtensionIsLoaded("json")) {
			return;
		}
		auto handle = SubmitArrow(con, "SELECT '{\"a\":1}'::JSON j FROM range(3000)", 1024);
		DrainWatchdog watchdog(con);
		QueryResultStream<ArrowFormat> stream(std::move(handle));
		auto arrays = DrainArrays(stream);
		REQUIRE(TotalRows(arrays) == ROWS);
		auto scanned = ScanBack(con, stream, std::move(arrays));
		REQUIRE(!scanned->HasError());
		REQUIRE(scanned->Collection().Count() == ROWS);
		REQUIRE(scanned->Collection().GetValue(0, 0).ToString() == "{\"a\":1}");
	}
}

TEST_CASE("An Arrow unit reports the bytes its buffers hold", "[api][query_result_arrow]") {
	constexpr idx_t ROWS = 4096;
	DuckDB db(nullptr);
	Connection con(db);

	auto numbers = FormatUnits(con, "SELECT i FROM range(4096) t(i)", ROWS);
	REQUIRE(numbers.size() == 1);
	REQUIRE(numbers[0]->row_count == ROWS);
	// A BIGINT column is eight bytes a row, plus whatever the validity mask costs
	REQUIRE(numbers[0]->byte_size >= ROWS * sizeof(int64_t));

	auto strings = FormatUnits(con, "SELECT 'a reasonably long string ' || i AS s FROM range(4096) t(i)", ROWS);
	REQUIRE(strings.size() == 1);
	REQUIRE(strings[0]->row_count == ROWS);
	// The same rows as strings: offsets alone match the numbers, and the characters come on top
	REQUIRE(strings[0]->byte_size > numbers[0]->byte_size);

	auto nested = FormatUnits(con,
	                          "SELECT CASE WHEN i % 3 = 0 THEN NULL ELSE i END AS n, "
	                          "[i, NULL, i + 1] AS l, {'a': i, 'b': 'x' || i} AS s FROM range(4096) t(i)",
	                          ROWS);
	REQUIRE(nested.size() == 1);
	// Five BIGINTs a row live in n, the list's child and the struct's child, so children must count
	REQUIRE(nested[0]->byte_size >= 5 * ROWS * sizeof(int64_t));
}

TEST_CASE("Arrow units over NULL-heavy and nested columns keep their counts", "[api][query_result_arrow]") {
	constexpr idx_t ROWS = 5000;
	constexpr idx_t BATCH = 1024;
	DuckDB db(nullptr);
	Connection con(db);

	auto handle = SubmitArrow(con,
	                          "SELECT CASE WHEN i % 3 = 0 THEN NULL ELSE i END AS n, "
	                          "CASE WHEN i % 5 = 0 THEN NULL ELSE [i, NULL, i + 1] END AS l, "
	                          "{'a': i, 'b': CASE WHEN i % 2 = 0 THEN NULL ELSE 'x' || i END} AS s "
	                          "FROM range(5000) t(i)",
	                          BATCH);
	DrainWatchdog watchdog(con);
	QueryResultStream<ArrowFormat> stream(std::move(handle));
	auto arrays = DrainArrays(stream);
	REQUIRE(TotalRows(arrays) == ROWS);
	REQUIRE(arrays.size() == ROWS / BATCH + 1);
	for (auto &wrapper : arrays) {
		auto &array = wrapper->arrow_array;
		REQUIRE(array.n_children == 3);
		REQUIRE(array.children[0]->length == array.length);
		REQUIRE(array.children[1]->length == array.length);
		REQUIRE(array.children[2]->length == array.length);
		REQUIRE(array.children[0]->null_count > 0);
		REQUIRE(array.children[1]->null_count > 0);
		// The struct itself has no NULLs; its second child does
		REQUIRE(array.children[2]->null_count == 0);
		REQUIRE(array.children[2]->children[1]->null_count > 0);
	}

	auto scanned = ScanBack(con, stream, std::move(arrays));
	REQUIRE(!scanned->HasError());
	auto &collection = scanned->Collection();
	REQUIRE(collection.Count() == ROWS);
	auto rows = collection.GetRows();
	REQUIRE(rows.GetValue(0, 0).IsNull());
	REQUIRE(rows.GetValue(0, 1) == Value::BIGINT(1));
	REQUIRE(rows.GetValue(1, 0).IsNull());
	REQUIRE(rows.GetValue(1, 1) ==
	        Value::LIST(LogicalType::BIGINT, {Value::BIGINT(1), Value(LogicalType::BIGINT), Value::BIGINT(2)}));
	REQUIRE(rows.GetValue(2, 2) == Value::STRUCT({{"a", Value::BIGINT(2)}, {"b", Value(LogicalType::VARCHAR)}}));
	REQUIRE(rows.GetValue(2, 3) == Value::STRUCT({{"a", Value::BIGINT(3)}, {"b", Value("x3")}}));
	REQUIRE(rows.GetValue(0, ROWS - 1).IsNull() == ((ROWS - 1) % 3 == 0));
}

TEST_CASE("An empty result in the Arrow format has no units", "[api][query_result_arrow]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT range i FROM range(10000)"));

	SECTION("streamed from a source without rows") {
		auto handle = SubmitArrow(con, "SELECT i FROM range(0) t(i)", 1024);
		DrainWatchdog watchdog(con);
		QueryResultStream<ArrowFormat> stream(std::move(handle));
		auto arrays = DrainArrays(stream);
		REQUIRE(arrays.empty());
		REQUIRE(stream.Poll() == QueryResultState::FINISHED);
		REQUIRE(stream.FormatState().Schema().n_children == 1);
	}
	SECTION("streamed from a table whose rows are all filtered") {
		auto handle = SubmitArrow(con, "SELECT i FROM t WHERE i < 0", 1024);
		DrainWatchdog watchdog(con);
		QueryResultStream<ArrowFormat> stream(std::move(handle));
		REQUIRE_NOTHROW(stream.GetBufferedData().Cast<BatchedBufferedData>());
		auto arrays = DrainArrays(stream);
		REQUIRE(arrays.empty());
		REQUIRE(stream.Poll() == QueryResultState::FINISHED);
	}
	SECTION("retained") {
		auto result = con.Query("SELECT i FROM t WHERE i < 0", make_shared_ptr<ArrowFormat>(1024));
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 0);
		REQUIRE(result->Collection<ArrowFormat>().empty());
		REQUIRE(result->Fetch<ArrowFormat>() == nullptr);
		REQUIRE(result->FormatState<ArrowFormat>().Schema().n_children == 1);
		auto taken = result->TakeCollection<ArrowFormat>();
		REQUIRE(taken);
		REQUIRE(taken->empty());
	}
}

TEST_CASE("Query with an Arrow format returns its arrays and their schema", "[api][query_result_arrow]") {
	constexpr idx_t ROWS = 20000;
	DuckDB db(nullptr);
	Connection con(db);

	auto result = con.Query("SELECT i, 'r' || i AS s FROM range(20000) t(i)", make_shared_ptr<ArrowFormat>(4096));
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->RowCount() == ROWS);

	auto &state = result->FormatState<ArrowFormat>();
	auto &schema = state.Schema();
	REQUIRE(schema.n_children == 2);
	REQUIRE(string(schema.children[0]->name) == "i");
	REQUIRE(string(schema.children[1]->name) == "s");
	REQUIRE(string(schema.children[0]->format) == "l");
	REQUIRE(string(schema.children[1]->format) == "u");

	auto &collection = result->Collection<ArrowFormat>();
	REQUIRE(collection.size() == ROWS / 4096 + 1);
	REQUIRE(TotalRows(collection) == ROWS);
}

namespace {

const int64_t *Int64Values(const ArrowArray &node) {
	return reinterpret_cast<const int64_t *>(node.buffers[1]) + node.offset;
}

bool IsValidRow(const ArrowArray &node, idx_t row) {
	auto validity = reinterpret_cast<const uint8_t *>(node.buffers[0]);
	if (!validity) {
		return true;
	}
	auto bit = NumericCast<idx_t>(node.offset) + row;
	return (validity[bit / 8] >> (bit % 8)) & 1;
}

string StringAt(const ArrowArray &node, idx_t row) {
	auto offsets = reinterpret_cast<const int32_t *>(node.buffers[1]) + node.offset;
	auto data = reinterpret_cast<const char *>(node.buffers[2]);
	return string(data + offsets[row], NumericCast<idx_t>(offsets[row + 1] - offsets[row]));
}

//! An export has a descriptor of its own at every node, and the source's metadata and buffers
void RequireSharedExport(const ArrowArray &exported, const ArrowArray &source) {
	REQUIRE(exported.release != nullptr);
	REQUIRE(exported.release != source.release);
	REQUIRE(exported.private_data != source.private_data);
	REQUIRE(exported.length == source.length);
	REQUIRE(exported.null_count == source.null_count);
	REQUIRE(exported.offset == source.offset);
	REQUIRE(exported.n_buffers == source.n_buffers);
	REQUIRE(exported.buffers == source.buffers);
	REQUIRE(exported.n_children == source.n_children);
	if (source.n_children > 0) {
		REQUIRE(exported.children != source.children);
	}
	for (int64_t i = 0; i < source.n_children; i++) {
		REQUIRE(exported.children[i] != source.children[i]);
		RequireSharedExport(*exported.children[i], *source.children[i]);
	}
	REQUIRE((exported.dictionary == nullptr) == (source.dictionary == nullptr));
	if (source.dictionary) {
		REQUIRE(exported.dictionary != source.dictionary);
		RequireSharedExport(*exported.dictionary, *source.dictionary);
	}
}

//! An original built by hand, {a: dictionary encoded, s: {x}}, whose release only counts, so a test sees
//! exactly when its last holder lets go
class HandBuiltArray {
public:
	HandBuiltArray() {
		Node(root, root_buffers, 1);
		root.n_children = 2;
		root.children = root_children;
		root.private_data = &releases;
		root.release = CountRelease;
		Node(a, a_buffers, 2);
		a.dictionary = &dictionary;
		Node(dictionary, dictionary_buffers, 2);
		Node(s, s_buffers, 1);
		s.n_children = 1;
		s.children = s_children;
		Node(x, x_buffers, 2);
		x.offset = 1;
	}

public:
	ArrowArrayOwner Own() {
		auto wrapper = make_uniq<ArrowArrayWrapper>();
		wrapper->arrow_array = root;
		return ArrowArrayOwner(std::move(wrapper));
	}

public:
	idx_t releases = 0;

private:
	static void Node(ArrowArray &node, const void **buffers, int64_t n_buffers) {
		node = ArrowArray {};
		node.length = 3;
		node.n_buffers = n_buffers;
		node.buffers = buffers;
		node.release = ReleaseChild;
	}
	static void CountRelease(ArrowArray *array) {
		(*static_cast<idx_t *>(array->private_data))++;
		array->release = nullptr;
	}
	static void ReleaseChild(ArrowArray *array) {
		array->release = nullptr;
	}

private:
	int8_t indexes[3] = {2, 0, 1};
	int64_t words[3] = {10, 20, 30};
	int64_t values[4] = {1, 2, 3, 4};
	const void *root_buffers[1] = {nullptr};
	const void *a_buffers[2] = {nullptr, indexes};
	const void *dictionary_buffers[2] = {nullptr, words};
	const void *s_buffers[1] = {nullptr};
	const void *x_buffers[2] = {nullptr, values};
	ArrowArray root;
	ArrowArray a;
	ArrowArray dictionary;
	ArrowArray s;
	ArrowArray x;
	ArrowArray *root_children[2] = {&a, &s};
	ArrowArray *s_children[1] = {&x};
};

//! Integers with NULLs, strings, a struct over a list, and an ENUM, which Arrow receives as a dictionary
const char *const MIXED_QUERY = "SELECT CASE WHEN i % 3 = 0 THEN NULL ELSE i END AS n, 'v' || i AS s, "
                                "{'a': i, 'l': [i, i + 1]} AS st, "
                                "(['x', 'y', 'z'])[i % 3 + 1]::ENUM('x', 'y', 'z') AS e FROM range(3000) t(i)";

//! Samples the rows of one array of MIXED_QUERY, whose first row is first_row
void RequireMixedRows(const ArrowArray &array, idx_t first_row) {
	REQUIRE(array.n_children == 4);
	auto &n = *array.children[0];
	auto &s = *array.children[1];
	auto &st = *array.children[2];
	auto &e = *array.children[3];
	REQUIRE(e.dictionary != nullptr);
	REQUIRE(StringAt(*e.dictionary, 2) == "z");
	auto &list = *st.children[1];
	auto list_offsets = reinterpret_cast<const int32_t *>(list.buffers[1]) + list.offset;
	for (idx_t row = 0; row < NumericCast<idx_t>(array.length); row += 100) {
		auto i = NumericCast<int64_t>(first_row + row);
		REQUIRE(IsValidRow(n, row) == (i % 3 != 0));
		if (i % 3 != 0) {
			REQUIRE(Int64Values(n)[row] == i);
		}
		REQUIRE(StringAt(s, row) == "v" + to_string(i));
		REQUIRE(Int64Values(*st.children[0])[row] == i);
		REQUIRE(Int64Values(*list.children[0])[list_offsets[row] + 1] == i + 1);
	}
}

} // namespace

TEST_CASE("Exports of one Arrow array are independent descriptor trees over its buffers", "[api][query_result_arrow]") {
	HandBuiltArray source;
	auto owner = source.Own();
	auto first = ArrowFormat::ShareArray(owner);
	auto second = ArrowFormat::ShareArray(owner);
	RequireSharedExport(first->arrow_array, owner->arrow_array);
	RequireSharedExport(second->arrow_array, owner->arrow_array);
	REQUIRE(first->arrow_array.children != second->arrow_array.children);
	REQUIRE(first->arrow_array.children[0]->dictionary != second->arrow_array.children[0]->dictionary);
	REQUIRE(first->arrow_array.children[1]->children[0] != second->arrow_array.children[1]->children[0]);
	REQUIRE(first->arrow_array.children[1]->children[0]->children == nullptr);
	REQUIRE(source.releases == 0);

	SECTION("the original goes with the last export, after the owner") {
		owner.reset();
		first.reset();
		REQUIRE(source.releases == 0);
		REQUIRE(Int64Values(*second->arrow_array.children[1]->children[0])[2] == 4);
		second.reset();
		REQUIRE(source.releases == 1);
	}
	SECTION("the original goes with the owner, after the exports") {
		second.reset();
		first.reset();
		REQUIRE(source.releases == 0);
		owner.reset();
		REQUIRE(source.releases == 1);
	}
	SECTION("a root moved to a consumer holds the original on its own") {
		ArrowArray exported;
		first->MoveTo(exported);
		first.reset();
		second.reset();
		owner.reset();
		REQUIRE(source.releases == 0);
		REQUIRE(Int64Values(*exported.children[1]->children[0])[0] == 2);
		exported.release(&exported);
		REQUIRE(exported.release == nullptr);
		REQUIRE(source.releases == 1);
	}
	SECTION("a child moved out outlives its parent, the other export and the owner") {
		// The C interface lets a consumer take a child by copying its descriptor and clearing its release
		auto &child = *first->arrow_array.children[1];
		ArrowArray moved = child;
		child.release = nullptr;
		first.reset();
		second.reset();
		owner.reset();
		REQUIRE(source.releases == 0);
		REQUIRE(Int64Values(*moved.children[0])[1] == 3);
		moved.release(&moved);
		REQUIRE(moved.release == nullptr);
		REQUIRE(source.releases == 1);
	}
	SECTION("a dictionary moved out outlives its parent, the other export and the owner") {
		auto &dictionary = *first->arrow_array.children[0]->dictionary;
		ArrowArray moved = dictionary;
		dictionary.release = nullptr;
		first.reset();
		second.reset();
		owner.reset();
		REQUIRE(source.releases == 0);
		REQUIRE(Int64Values(moved)[2] == 30);
		moved.release(&moved);
		REQUIRE(moved.release == nullptr);
		REQUIRE(source.releases == 1);
	}
}

TEST_CASE("A streamed Arrow array is the one the appender built and outlives its stream", "[api][query_result_arrow]") {
	DuckDB db(nullptr);
	auto con = make_uniq<Connection>(db);
	// Stored arrays are the appender's own, so their release tells an original from an export
	auto retained = con->Query("SELECT i FROM range(10) t(i)", make_shared_ptr<ArrowFormat>(1024));
	REQUIRE_NO_FAIL(*retained);
	auto appender_release = retained->Collection<ArrowFormat>()[0]->arrow_array.release;
	REQUIRE(retained->Fetch<ArrowFormat>()->arrow_array.release != appender_release);
	retained.reset();

	auto stream = make_uniq<QueryResultStream<ArrowFormat>>(SubmitArrow(*con, MIXED_QUERY, 1024));
	auto first = stream->Fetch();
	REQUIRE(first);
	REQUIRE(first->arrow_array.release == appender_release);
	stream.reset();
	con.reset();
	RequireMixedRows(first->arrow_array, 0);
}

TEST_CASE("Fetching a retained Arrow result exports its arrays and leaves the collection whole",
          "[api][query_result_arrow]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto result = con.Query(MIXED_QUERY, make_shared_ptr<ArrowFormat>(1024));
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection<ArrowFormat>().size() == 3);
	weak_ptr<const ArrowArrayWrapper> original = result->Collection<ArrowFormat>()[0];

	SECTION("the collection outlives every fetched array") {
		vector<unique_ptr<ArrowArrayWrapper>> fetched;
		while (auto array = result->Fetch<ArrowFormat>()) {
			fetched.push_back(std::move(array));
		}
		REQUIRE(result->Fetch<ArrowFormat>() == nullptr);
		auto &collection = result->Collection<ArrowFormat>();
		REQUIRE(fetched.size() == collection.size());
		for (idx_t i = 0; i < fetched.size(); i++) {
			RequireSharedExport(fetched[i]->arrow_array, collection[i]->arrow_array);
			RequireMixedRows(fetched[i]->arrow_array, i * 1024);
		}
		fetched.clear();
		REQUIRE(!original.expired());
		REQUIRE(TotalRows(collection) == 3000);
		for (idx_t i = 0; i < collection.size(); i++) {
			RequireMixedRows(collection[i]->arrow_array, i * 1024);
		}
	}
	SECTION("a fetched array outlives the result") {
		auto first = result->Fetch<ArrowFormat>();
		result.reset();
		REQUIRE(!original.expired());
		RequireMixedRows(first->arrow_array, 0);
		first.reset();
		REQUIRE(original.expired());
	}
	SECTION("two exports of one array are released in either order") {
		auto first = result->Fetch<ArrowFormat>();
		auto second = ArrowFormat::ShareArray(result->Collection<ArrowFormat>()[0]);
		REQUIRE(first->arrow_array.children != second->arrow_array.children);
		REQUIRE(first->arrow_array.children[3]->dictionary != second->arrow_array.children[3]->dictionary);
		REQUIRE(first->arrow_array.children[1]->buffers[2] == second->arrow_array.children[1]->buffers[2]);
		result.reset();
		SECTION("the fetched one first") {
			first.reset();
			REQUIRE(!original.expired());
			RequireMixedRows(second->arrow_array, 0);
			second.reset();
		}
		SECTION("the shared one first") {
			second.reset();
			REQUIRE(!original.expired());
			RequireMixedRows(first->arrow_array, 0);
			first.reset();
		}
		REQUIRE(original.expired());
	}
}

TEST_CASE("A child or dictionary moved out of a retained Arrow export outlives the rest", "[api][query_result_arrow]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto result = con.Query(MIXED_QUERY, make_shared_ptr<ArrowFormat>(1024));
	REQUIRE_NO_FAIL(*result);
	weak_ptr<const ArrowArrayWrapper> original = result->Collection<ArrowFormat>()[0];
	auto first = result->Fetch<ArrowFormat>();
	auto second = ArrowFormat::ShareArray(result->Collection<ArrowFormat>()[0]);

	SECTION("a nested child") {
		auto &child = *first->arrow_array.children[2];
		ArrowArray moved = child;
		child.release = nullptr;
		first.reset();
		second.reset();
		result.reset();
		REQUIRE(!original.expired());
		auto &list = *moved.children[1];
		auto list_offsets = reinterpret_cast<const int32_t *>(list.buffers[1]) + list.offset;
		REQUIRE(Int64Values(*moved.children[0])[1023] == 1023);
		REQUIRE(Int64Values(*list.children[0])[list_offsets[1023] + 1] == 1024);
		moved.release(&moved);
		REQUIRE(moved.release == nullptr);
		REQUIRE(original.expired());
	}
	SECTION("a dictionary") {
		auto &dictionary = *first->arrow_array.children[3]->dictionary;
		ArrowArray moved = dictionary;
		dictionary.release = nullptr;
		first.reset();
		second.reset();
		result.reset();
		REQUIRE(!original.expired());
		REQUIRE(moved.length == 3);
		REQUIRE(StringAt(moved, 0) == "x");
		REQUIRE(StringAt(moved, 1) == "y");
		REQUIRE(StringAt(moved, 2) == "z");
		moved.release(&moved);
		REQUIRE(moved.release == nullptr);
		REQUIRE(original.expired());
	}
}

TEST_CASE("A taken Arrow collection leaves the result and outlives it", "[api][query_result_arrow]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto result = con.Query(MIXED_QUERY, make_shared_ptr<ArrowFormat>(1024));
	REQUIRE_NO_FAIL(*result);

	SECTION("exports of a taken collection outlive the container") {
		auto taken = result->TakeCollection<ArrowFormat>();
		REQUIRE_THROWS_AS(result->Collection<ArrowFormat>(), InvalidInputException);
		REQUIRE_THROWS_AS(result->Fetch<ArrowFormat>(), InvalidInputException);
		result.reset();
		REQUIRE(taken->size() == 3);
		REQUIRE(TotalRows(*taken) == 3000);
		weak_ptr<const ArrowArrayWrapper> original = (*taken)[1];
		vector<unique_ptr<ArrowArrayWrapper>> exported;
		for (auto &owner : *taken) {
			exported.push_back(ArrowFormat::ShareArray(owner));
		}
		taken.reset();
		REQUIRE(!original.expired());
		RequireMixedRows(exported[1]->arrow_array, 1024);
		exported.clear();
		REQUIRE(original.expired());
	}
	SECTION("arrays fetched before the take and exported after it coexist") {
		auto early = result->Fetch<ArrowFormat>();
		auto taken = result->TakeCollection<ArrowFormat>();
		weak_ptr<const ArrowArrayWrapper> original = (*taken)[0];
		auto late = ArrowFormat::ShareArray((*taken)[0]);
		result.reset();
		taken.reset();
		REQUIRE(early->arrow_array.children != late->arrow_array.children);
		REQUIRE(early->arrow_array.children[1]->buffers[2] == late->arrow_array.children[1]->buffers[2]);
		early.reset();
		REQUIRE(!original.expired());
		RequireMixedRows(late->arrow_array, 0);
		late.reset();
		REQUIRE(original.expired());
	}
	SECTION("a taken collection scans back through arrow_scan") {
		auto types = result->GetTypes();
		auto names = IdentifiersToStrings(result->GetNames());
		auto properties = result->client_properties;
		auto taken = result->TakeCollection<ArrowFormat>();
		result.reset();
		vector<unique_ptr<ArrowArrayWrapper>> exported;
		for (auto &owner : *taken) {
			exported.push_back(ArrowFormat::ShareArray(owner));
		}
		taken.reset();
		ServedArrays served(std::move(types), std::move(names), std::move(properties), std::move(exported));
		auto scanned = served.Scan(con);
		REQUIRE(!scanned->HasError());
		auto &collection = scanned->Collection();
		REQUIRE(collection.Count() == 3000);
		auto rows = collection.GetRows();
		REQUIRE(rows.GetValue(0, 3).IsNull());
		REQUIRE(rows.GetValue(1, 7) == Value("v7"));
		REQUIRE(rows.GetValue(2, 2999) ==
		        Value::STRUCT({{"a", Value::BIGINT(2999)},
		                       {"l", Value::LIST(LogicalType::BIGINT, {Value::BIGINT(2999), Value::BIGINT(3000)})}}));
		REQUIRE(rows.GetValue(3, 2).ToString() == "z");
	}
}

TEST_CASE("An Arrow stream outlives the connection that submitted it", "[api][query_result_arrow]") {
	DuckDB db(nullptr);
	auto con = make_uniq<Connection>(db);

	QueryResultStream<ArrowFormat> stream(SubmitArrow(*con, "SELECT i FROM range(3000) t(i)", 1024));
	auto first = stream.Fetch();
	REQUIRE(first);
	REQUIRE(Rows(*first) == 1024);

	// The stream keeps the query, and with it the context, alive
	con.reset();

	idx_t rows = Rows(*first);
	while (auto array = stream.Fetch()) {
		rows += Rows(*array);
	}
	REQUIRE(!stream.HasError());
	REQUIRE(rows == 3000);
}

TEST_CASE("A statement on the connection is refused while an Arrow stream is open", "[api][query_result_arrow]") {
	DuckDB db(nullptr);
	Connection con(db);

	QueryResultStream<ArrowFormat> stream(SubmitArrow(con, "SELECT i FROM range(200000) t(i)", 1000));
	auto first = stream.Fetch();
	REQUIRE(first);
	REQUIRE(Rows(*first) == 1000);

	auto refused = con.Query("SELECT 42");
	REQUIRE(refused->HasError());
	REQUIRE(refused->GetErrorType() == ExceptionType::RESOURCE_IN_USE);

	// The stream reads on, and once it has reported its end the connection takes the next statement
	idx_t rows = Rows(*first);
	while (auto array = stream.Fetch()) {
		rows += Rows(*array);
	}
	REQUIRE(!stream.HasError());
	REQUIRE(rows == 200000);
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
}

TEST_CASE("The chunk accessors reject an Arrow result", "[api][query_result_arrow]") {
	DuckDB db(nullptr);
	Connection con(db);

	auto result = con.Query("SELECT i FROM range(1000) t(i)", make_shared_ptr<ArrowFormat>(1024));
	REQUIRE_NO_FAIL(*result);

	REQUIRE_THROWS_AS(result->FetchRaw(), InvalidInputException);
	REQUIRE_THROWS_AS(result->Collection(), InvalidInputException);
	REQUIRE_THROWS_AS(result->Fetch(), InvalidInputException);
	// The rows are still there, in the format that was asked for
	REQUIRE(result->RowCount() == 1000);
	REQUIRE(TotalRows(result->Collection<ArrowFormat>()) == 1000);
}

#endif
