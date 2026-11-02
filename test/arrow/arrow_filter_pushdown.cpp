#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"
#include "duckdb/common/types/arrow_string_view_type.hpp"
#include "duckdb/planner/table_filter_set.hpp"

using namespace duckdb;

// Helper: create an ArrowTestFactory from a query result (all rows, no filter)
static unique_ptr<ArrowTestFactory> MakeArrowFactory(Connection &con, const string &query, bool use_string_view) {
	if (use_string_view) {
		REQUIRE(!con.Query("SET produce_arrow_string_view = true")->HasError());
		REQUIRE(!con.Query("SET arrow_output_version = '1.5'")->HasError());
	} else {
		REQUIRE(!con.Query("SET produce_arrow_string_view = false")->HasError());
		REQUIRE(!con.Query("SET arrow_output_version = '1.0'")->HasError());
	}
	auto client_properties = con.context->GetClientProperties();
	auto result = con.context->Query(query, QueryParameters());
	REQUIRE(!result->HasError());
	auto types = result->GetTypes();
	auto names = IdentifiersToStrings(result->GetNames());
	return make_uniq<ArrowTestFactory>(std::move(types), std::move(names), std::move(result), client_properties,
	                                   *con.context);
}

// Helper: get the EXPLAIN output for an arrow_scan with a filter
static string GetExplainForFilter(Connection &con, ArrowTestFactory &factory, const string &filter_expr) {
	const auto params = ArrowTestHelper::ConstructArrowScan(factory);
	const auto rel = con.TableFunction("arrow_scan", {}, {}, params)->Filter(filter_expr);
	// render operator names raw/upper-case so the string checks below (ARROW_SCAN, FILTER) match
	REQUIRE(!con.Query("SET profiling_renderer_settings = MAP {'operator_casing': 'upper'}")->HasError());
	const auto explain_result = rel->Explain();
	REQUIRE(!explain_result->HasError());
	auto &mat = *explain_result;
	return mat.Collection().GetValue(1, 0).ToString();
}

// Helper: check for a standalone FILTER operator node in the explain output
static bool StandaloneFilter(const std::string &explain_str) {
	// Look for FILTER as a standalone operator node name (not "Filters:" which is a scan attribute)
	std::string::size_type pos = 0;
	while ((pos = explain_str.find("FILTER", pos)) != std::string::npos) {
		// Check this is the standalone node name, not part of "Filters:"
		if (pos + 6 >= explain_str.size() || explain_str[pos + 6] != 's') {
			return true;
		}
		pos += 6;
	}
	return false;
}

// Helper: check for a scan node with filter pushdown in the explain output
static bool FilterInScan(const std::string &explain_str) {
	auto arrow_pos = explain_str.find("ARROW_SCAN");
	if (arrow_pos == std::string::npos) {
		return false;
	}
	return explain_str.find("Filters:", arrow_pos) != std::string::npos;
}

TEST_CASE("Arrow filter pushdown - view types disable pushdown", "[arrow]") {
	DuckDB db;
	Connection con(db);

	// Create a test table with an INT id column and a VARCHAR name column
	REQUIRE(!con.Query("CREATE TABLE src AS SELECT i AS id, i::VARCHAR AS name FROM range(10) tbl(i)")->HasError());

	SECTION("String view column: filter above scan (not pushed)") {
		auto factory = MakeArrowFactory(con, "SELECT * FROM src", true);
		auto explain_str = GetExplainForFilter(con, *factory, "id > 5");
		REQUIRE(StandaloneFilter(explain_str));
		REQUIRE(!FilterInScan(explain_str));
	}

	SECTION("Regular string column: filter pushed into scan") {
		auto factory = MakeArrowFactory(con, "SELECT * FROM src", false);
		auto explain_str = GetExplainForFilter(con, *factory, "id > 5");
		REQUIRE(!StandaloneFilter(explain_str));
		REQUIRE(FilterInScan(explain_str));
	}

	SECTION("Integer-only table: filter pushed into scan") {
		auto factory = MakeArrowFactory(con, "SELECT id FROM src", false);
		auto explain_str = GetExplainForFilter(con, *factory, "id > 5");
		REQUIRE(!StandaloneFilter(explain_str));
		REQUIRE(FilterInScan(explain_str));
	}
}

TEST_CASE("Arrow filter pushdown - nested view types disable pushdown", "[arrow]") {
	DuckDB db;
	Connection con(db);

	REQUIRE(!con.Query("CREATE TABLE src AS SELECT i AS id, 'val_' || i::VARCHAR AS name FROM range(10) tbl(i)")
	             ->HasError());

	SECTION("Struct containing string_view") {
		auto factory = MakeArrowFactory(con, "SELECT id, {'s': name} AS nested FROM src", true);
		auto explain_str = GetExplainForFilter(con, *factory, "id > 5");
		REQUIRE(StandaloneFilter(explain_str));
		REQUIRE(!FilterInScan(explain_str));
	}

	SECTION("List containing string_view") {
		auto factory = MakeArrowFactory(con, "SELECT id, [name] AS names FROM src", true);
		auto explain_str = GetExplainForFilter(con, *factory, "id > 5");
		REQUIRE(StandaloneFilter(explain_str));
		REQUIRE(!FilterInScan(explain_str));
	}
}

TEST_CASE("Arrow filter pushdown - filters kept above the scan next to an IN list", "[arrow]") {
	DuckDB db;
	Connection con(db);
	REQUIRE(!con.Query("CREATE TABLE src AS SELECT (['TOYOTA', 'HONDA', 'VPG', 'ISUZU', 'SMART', 'LUCID'])[i % 6 + 1] "
	                   "AS name, (i % 10 = 0)::TINYINT AS flag FROM range(6000) t(i)")
	             ->HasError());
	// with a string view column the filters stay above the scan, which also returns the columns they read. The test
	// factory returns every column and ignores pushed down filters, so each query reads both columns.
	duckdb::vector<string> queries {
	    "SELECT name, count(*) FROM %s WHERE flag < 1 AND name IN ('VPG', 'ISUZU', 'SMART', 'LUCID', 'LOTUS') "
	    "GROUP BY 1 ORDER BY 1",
	    "SELECT name, flag, count(*) FROM %s WHERE flag < 1 AND name IN ('VPG', 'ISUZU', 'SMART', 'LUCID', 'LOTUS') "
	    "GROUP BY ALL ORDER BY ALL",
	    "SELECT * FROM %s WHERE flag < 1 AND name IN ('VPG', 'ISUZU', 'SMART', 'LUCID', 'LOTUS') ORDER BY ALL LIMIT 3"};
	// without unused_columns, a scan of every column has an empty projection list
	for (auto disabled_optimizers : {"", "unused_columns"}) {
		REQUIRE(!con.Query(StringUtil::Format("SET disabled_optimizers = '%s'", disabled_optimizers))->HasError());
		for (auto &query : queries) {
			INFO(query << " without " << disabled_optimizers);
			auto factory = MakeArrowFactory(con, "SELECT * FROM src", true);
			con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(*factory))
			    ->CreateView("data", true, true);
			auto result = con.Query(StringUtil::Format(query, "data"));
			REQUIRE(!result->HasError());
			auto expected = con.Query(StringUtil::Format(query, "src"));
			REQUIRE(!expected->HasError());
			REQUIRE(result->Equals(*expected));
		}
	}
}

// Hand-built Arrow columns that DuckDB's own Arrow export never produces, scanned through a factory that records
// the filters the producer receives. The Arrow scan never re-applies a pushed filter, so a filter the producer
// ignores shows up as extra rows.
namespace arrow_pushdown_gate_test {

void NoopReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void NoopReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

struct Column {
	Column() = default;
	Column(const Column &) = delete;

	void Init(const char *format, const char *name) {
		schema.format = format;
		schema.name = name;
		schema.flags = 2; // ARROW_FLAG_NULLABLE
		schema.release = NoopReleaseSchema;
		array.release = NoopReleaseArray;
	}
	void Finish(int64_t length) {
		array.length = length;
		array.n_buffers = int64_t(buffers.size());
		array.buffers = buffers.data();
		schema.n_children = int64_t(schema_children.size());
		schema.children = schema_children.empty() ? nullptr : schema_children.data();
		array.n_children = int64_t(array_children.size());
		array.children = array_children.empty() ? nullptr : array_children.data();
	}

	ArrowSchema schema {};
	ArrowArray array {};
	vector<const void *> buffers;
	vector<ArrowSchema *> schema_children;
	vector<ArrowArray *> array_children;
};

struct Int32Column : Column {
	Int32Column(const char *name, vector<int32_t> values_p) : values(std::move(values_p)) {
		Init("i", name);
		buffers = {nullptr, values.data()};
		Finish(int64_t(values.size()));
	}
	vector<int32_t> values;
};

// utf8 ("u") or string_view ("vu") with every value short enough to be stored inline in the view.
struct StringColumn : Column {
	StringColumn(const char *name, const vector<string> &strings, bool view) {
		if (view) {
			for (auto &s : strings) {
				REQUIRE(s.size() <= ArrowStringViewConstants::MAX_INLINED_BYTES);
				views.emplace_back(int32_t(s.size()), s.data());
			}
			Init("vu", name);
			buffers = {nullptr, views.data(), variadic_sizes.data()};
		} else {
			offsets.push_back(0);
			for (auto &s : strings) {
				data += s;
				offsets.push_back(int32_t(data.size()));
			}
			Init("u", name);
			buffers = {nullptr, offsets.data(), data.data()};
		}
		Finish(int64_t(strings.size()));
	}
	vector<arrow_string_view_t> views;
	vector<int64_t> variadic_sizes {0};
	vector<int32_t> offsets;
	string data;
};

// decimal128(10,2): a width DuckDB stores in 64 bits, which the gate refuses on a plain column.
struct Decimal128Column : Column {
	Decimal128Column(const char *name, const vector<int64_t> &unscaled) {
		for (auto v : unscaled) {
			values.emplace_back(v);
		}
		Init("d:10,2", name);
		buffers = {nullptr, values.data()};
		Finish(int64_t(values.size()));
	}
	vector<hugeint_t> values;
};

struct DictionaryColumn : Column {
	DictionaryColumn(const char *name, Column &dictionary, vector<int8_t> indices_p) : indices(std::move(indices_p)) {
		Init("c", name);
		schema.dictionary = &dictionary.schema;
		array.dictionary = &dictionary.array;
		buffers = {nullptr, indices.data()};
		Finish(int64_t(indices.size()));
	}
	vector<int8_t> indices;
};

struct RunEndColumn : Column {
	RunEndColumn(const char *name, Column &values, vector<int32_t> run_ends_p)
	    : run_ends("run_ends", std::move(run_ends_p)) {
		run_ends.schema.flags = 0;
		values.schema.name = "values";
		Init("+r", name);
		schema_children = {&run_ends.schema, &values.schema};
		array_children = {&run_ends.array, &values.array};
		Finish(run_ends.values.empty() ? 0 : run_ends.values.back());
	}
	Int32Column run_ends;
};

struct StructColumn : Column {
	StructColumn(const char *name, const vector<Column *> &children, int64_t length) {
		Init("+s", name);
		for (auto child : children) {
			schema_children.push_back(&child->schema);
			array_children.push_back(&child->array);
		}
		buffers = {nullptr};
		Finish(length);
	}
};

struct ListColumn : Column {
	ListColumn(const char *name, Column &child, vector<int32_t> offsets_p) : offsets(std::move(offsets_p)) {
		child.schema.name = "item";
		Init("+l", name);
		schema_children = {&child.schema};
		array_children = {&child.array};
		buffers = {nullptr, offsets.data()};
		Finish(int64_t(offsets.size()) - 1);
	}
	vector<int32_t> offsets;
};

// The columns of one record batch. Like a real producer it honors the scan's projection, so every scan gets a
// single-use stream over just the columns the scan asked for, in the order it asked for them.
struct Batch {
	explicit Batch(vector<Column *> columns_p) : columns(std::move(columns_p)) {
	}
	Batch(const Batch &) = delete;

	StructColumn &Record(const vector<string> &names) {
		vector<Column *> selected;
		for (auto &name : names) {
			for (auto column : columns) {
				if (name == column->schema.name) {
					selected.push_back(column);
				}
			}
		}
		REQUIRE(selected.size() == names.size());
		records.push_back(make_uniq<StructColumn>("", selected, columns[0]->array.length));
		records.back()->schema.flags = 0;
		return *records.back();
	}
	vector<string> Names() const {
		vector<string> names;
		for (auto column : columns) {
			names.emplace_back(column->schema.name);
		}
		return names;
	}

	vector<Column *> columns;
	vector<unique_ptr<StructColumn>> records;
};

// Records the column of every filter the scan asks the producer to apply.
class RecordingFactory : public ArrowScanFactory {
public:
	explicit RecordingFactory(Batch &batch_p) : batch(batch_p) {
	}
	void GetSchema(ArrowSchema &schema) override {
		schema = batch.get().Record(batch.get().Names()).schema;
	}
	unique_ptr<ArrowArrayStreamWrapper> ProduceStream(ArrowStreamParameters &parameters) override {
		if (parameters.filters) {
			for (auto &entry : *parameters.filters) {
				received_filters.push_back(parameters.projected_columns.projection_map.at(entry.GetIndex().GetIndex()));
			}
		}
		auto &names = parameters.projected_columns.columns;
		auto &record = batch.get().Record(names.empty() ? batch.get().Names() : names);
		auto wrapper = make_uniq<ArrowArrayStreamWrapper>();
		AdbcError err {};
		REQUIRE(duckdb_adbc::BatchToArrayStream(&record.array, &record.schema, &wrapper->arrow_array_stream, &err) ==
		        ADBC_STATUS_OK);
		return wrapper;
	}

	vector<string> received_filters;

private:
	reference<Batch> batch;
};

// A database with the batch registered as view "t" through the recording factory.
struct Scan {
	explicit Scan(Batch &batch) : db(nullptr), con(db), factory(make_shared_ptr<RecordingFactory>(batch)) {
		con.TableFunction("arrow_scan", {}, {}, factory)->CreateView("t", true, true);
	}
	vector<int32_t> Rows(const string &sql) {
		auto result = con.Query(sql);
		if (result->HasError()) {
			FAIL(result->GetError());
		}
		vector<int32_t> rows;
		for (auto &row : *result) {
			rows.push_back(row.GetValue<int32_t>(0));
		}
		return rows;
	}
	const vector<string> &ReceivedFilters() const {
		return factory->received_filters;
	}

	DuckDB db;
	Connection con;
	shared_ptr<RecordingFactory> factory;
};

TEST_CASE("Arrow filter pushdown - dictionary columns are judged by their value type", "[arrow]") {
	Int32Column x("x", {1, 2, 3});

	SECTION("dictionary of string_view is refused like the plain view type") {
		StringColumn values("", {"red", "blue"}, true);
		DictionaryColumn d("d", values, {0, 1, 0});
		Batch batch({&x, &d});
		Scan scan(batch);
		REQUIRE(scan.Rows("SELECT x FROM t WHERE d = 'red' ORDER BY x") == vector<int32_t> {1, 3});
		REQUIRE(scan.ReceivedFilters().empty());
	}

	SECTION("dictionary of utf8 stays pushable") {
		StringColumn values("", {"red", "blue"}, false);
		DictionaryColumn d("d", values, {0, 1, 0});
		Batch batch({&x, &d});
		Scan scan(batch);
		scan.Rows("SELECT x FROM t WHERE d = 'red'");
		REQUIRE(scan.ReceivedFilters().size() == 1);
	}

	SECTION("dictionary of a 64-bit decimal is refused like the plain decimal") {
		Decimal128Column values("", {125, 250});
		DictionaryColumn d("d", values, {0, 1, 0});
		Batch batch({&x, &d});
		Scan scan(batch);
		REQUIRE(scan.Rows("SELECT x FROM t WHERE d = 1.25 ORDER BY x") == vector<int32_t> {1, 3});
		REQUIRE(scan.ReceivedFilters().empty());
	}

	SECTION("dictionary of string_view inside a struct is refused") {
		StringColumn values("", {"red", "blue"}, true);
		DictionaryColumn d("d", values, {0, 1, 0});
		StructColumn s("s", {&d}, 3);
		Batch batch({&x, &s});
		Scan scan(batch);
		REQUIRE(scan.Rows("SELECT x FROM t WHERE s.d = 'red' ORDER BY x") == vector<int32_t> {1, 3});
		REQUIRE(scan.ReceivedFilters().empty());
	}

	SECTION("dictionary of utf8 inside a struct stays pushable") {
		StringColumn values("", {"red", "blue"}, false);
		DictionaryColumn d("d", values, {0, 1, 0});
		StructColumn s("s", {&d}, 3);
		Batch batch({&x, &s});
		Scan scan(batch);
		scan.Rows("SELECT x FROM t WHERE s.d = 'red'");
		REQUIRE(scan.ReceivedFilters() == vector<string> {"s"});
	}

	SECTION("Top-N ordered by a dictionary of string_view hands the producer no filter") {
		StringColumn values("", {"red", "blue"}, true);
		DictionaryColumn d("d", values, {0, 1, 0});
		Batch batch({&x, &d});
		Scan scan(batch);
		REQUIRE(scan.Rows("SELECT x FROM t ORDER BY d, x LIMIT 2") == vector<int32_t> {2, 1});
		REQUIRE(scan.ReceivedFilters().empty());
	}
}

TEST_CASE("Arrow filter pushdown - a dictionary of string_view does not disable pushdown for other columns",
          "[arrow]") {
	Int32Column x("x", {1, 2, 3});
	StringColumn values("", {"red", "blue"}, true);
	DictionaryColumn d("d", values, {0, 1, 0});
	Batch batch({&x, &d});
	Scan scan(batch);
	scan.Rows("SELECT x FROM t WHERE x > 1");
	REQUIRE(scan.ReceivedFilters().size() == 1);
}

TEST_CASE("Arrow filter pushdown - run-end encoded columns disable pushdown for the table", "[arrow]") {
	Int32Column x("x", {1, 2, 3, 4, 5});

	SECTION("int32 values, filter on the encoded column") {
		Int32Column values("", {10, 20, 10});
		RunEndColumn d("d", values, {2, 3, 5});
		Batch batch({&x, &d});
		Scan scan(batch);
		REQUIRE(scan.Rows("SELECT x FROM t WHERE d = 10 ORDER BY x") == vector<int32_t> {1, 2, 4, 5});
		REQUIRE(scan.ReceivedFilters().empty());
	}

	SECTION("int32 values, filter on another column") {
		Int32Column values("", {10, 20, 10});
		RunEndColumn d("d", values, {2, 3, 5});
		Batch batch({&x, &d});
		Scan scan(batch);
		REQUIRE(scan.Rows("SELECT x FROM t WHERE x = 3") == vector<int32_t> {3});
		REQUIRE(scan.ReceivedFilters().empty());
	}

	SECTION("utf8 values, filter on another column plans and returns the right rows") {
		StringColumn values("", {"a", "b", "a"}, false);
		RunEndColumn d("d", values, {2, 3, 5});
		Batch batch({&x, &d});
		Scan scan(batch);
		REQUIRE(scan.Rows("SELECT x FROM t WHERE x = 3") == vector<int32_t> {3});
		REQUIRE(scan.ReceivedFilters().empty());
	}

	SECTION("utf8 values inside a list, filter on another column") {
		StringColumn values("", {"a", "b", "a"}, false);
		RunEndColumn ree("", values, {2, 3, 5});
		ListColumn d("d", ree, {0, 1, 2, 3, 4, 5});
		Batch batch({&x, &d});
		Scan scan(batch);
		REQUIRE(scan.Rows("SELECT x FROM t WHERE x = 3") == vector<int32_t> {3});
		REQUIRE(scan.ReceivedFilters().empty());
	}

	SECTION("utf8 values, Top-N plans and returns the right rows") {
		StringColumn values("", {"a", "b", "a"}, false);
		RunEndColumn d("d", values, {2, 3, 5});
		Batch batch({&x, &d});
		Scan scan(batch);
		REQUIRE(scan.Rows("SELECT x FROM t ORDER BY x DESC LIMIT 2") == vector<int32_t> {5, 4});
		REQUIRE(scan.ReceivedFilters().empty());
	}
}

TEST_CASE("Arrow filter pushdown - join filters respect the scan's pushdown type gate", "[arrow]") {
	Int32Column x("x", {1, 2});

	SECTION("join on a string_view key hands the producer no filter") {
		StringColumn col("col", {"abc", "efg"}, true);
		Batch batch({&x, &col});
		Scan scan(batch);
		REQUIRE(scan.Rows("SELECT x FROM t JOIN (SELECT 'abc' AS k) b ON t.col = b.k") == vector<int32_t> {1});
		REQUIRE(scan.ReceivedFilters().empty());
	}

	SECTION("join on an integer key of a table with a string_view column hands the producer no filter") {
		StringColumn col("col", {"abc", "efg"}, true);
		Batch batch({&x, &col});
		Scan scan(batch);
		REQUIRE(scan.Rows("SELECT x FROM t JOIN (SELECT 2 AS k) b ON t.x = b.k") == vector<int32_t> {2});
		REQUIRE(scan.ReceivedFilters().empty());
	}

	SECTION("join on an integer key of a plain table hands the producer a filter") {
		StringColumn col("col", {"abc", "efg"}, false);
		Batch batch({&x, &col});
		Scan scan(batch);
		REQUIRE(scan.Rows("SELECT x FROM t JOIN (SELECT 2 AS k) b ON t.x = b.k") == vector<int32_t> {2});
		REQUIRE(scan.ReceivedFilters().size() == 1);
	}

	SECTION("join on two keys keeps the filter on the accepted key only") {
		Int32Column pad("pad", {0, 0});
		Decimal128Column d("d", {125, 250});
		Batch batch({&pad, &x, &d});
		Scan scan(batch);
		REQUIRE(scan.Rows("SELECT x FROM t JOIN (SELECT 2 AS k, 2.50 AS kd) b ON t.x = b.k AND t.d = b.kd") ==
		        vector<int32_t> {2});
		REQUIRE(scan.ReceivedFilters() == vector<string> {"x"});
	}
}

} // namespace arrow_pushdown_gate_test
