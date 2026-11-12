#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: what a table function that reads files needs of a scan: the filters of the query as hints,
// virtual columns, and metrics.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

std::vector<int64_t> CollectBigints(QueryResult result) {
	std::vector<int64_t> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			REQUIRE(view.IsValid(i));
			out.push_back(view.Data<int64_t>()[view.SelAt(i)]);
		}
	}
	return out;
}

std::vector<int64_t> CollectSortedBigints(QueryResult result) {
	auto out = CollectBigints(std::move(result));
	std::sort(out.begin(), out.end());
	return out;
}

std::vector<std::string> CollectStrings(QueryResult result) {
	std::vector<std::string> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			REQUIRE(view.IsValid(i));
			out.emplace_back(view.Data<varchar_t>()[view.SelAt(i)].view());
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// cpp_hint(n): two BIGINT columns, i = 0..n-1 and j = 2i. It reads the filters of the query, which it records, but
// produces every row regardless: it is up to the engine to apply them.
// ---------------------------------------------------------------------------

struct HintBind {
	int64_t count = 0;
};
struct HintGlobal {
	int64_t position = 0;
};

// What the last scan was told about the filters: the declared column of each, and the filter as text
std::vector<std::pair<idx_t, std::string>> seen_filters;
// The columns of the filters of every call, for the tests to clear
std::vector<idx_t> seen_filter_columns;

std::string Render(const TableFunction::FilterPushdownInput &input, const Expression &expression) {
	switch (expression.GetType()) {
	case ExpressionType::BOUND_COLUMN_REF:
		return "#" + std::to_string(input.GetColumnIndex(expression.GetColumnIndex()));
	case ExpressionType::VALUE_CONSTANT:
		return expression.GetConstantValue().ToText();
	default:
		break;
	}
	std::string name;
	switch (expression.GetType()) {
	case ExpressionType::COMPARE_EQUAL:
		name = "=";
		break;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		name = ">=";
		break;
	case ExpressionType::COMPARE_LESSTHAN:
		name = "<";
		break;
	case ExpressionType::CONJUNCTION_AND:
		name = "AND";
		break;
	default:
		name = "?";
		break;
	}
	std::string result = "(";
	for (idx_t i = 0; i < expression.GetChildCount(); i++) {
		result += (i ? " " + name + " " : "") + Render(input, expression.GetChild(i));
	}
	return result + ")";
}

void HintBindFunction(TableFunction::BindInput &input) {
	input.AddResultColumn("i", input.GetContext().GetFactory().ParseType("BIGINT"));
	input.AddResultColumn("j", input.GetContext().GetFactory().ParseType("BIGINT"));
	input.SetBindData<HintBind>(HintBind {input.GetConstantArgument(0).Get<int64_t>()});
}

// The declared column a predicate on a single column applies to
idx_t FilterColumn(const TableFunction::FilterPushdownInput &input, const Expression &expression) {
	if (expression.GetType() == ExpressionType::BOUND_COLUMN_REF) {
		return input.GetColumnIndex(expression.GetColumnIndex());
	}
	for (idx_t i = 0; i < expression.GetChildCount(); i++) {
		if (expression.GetChild(i).GetType() != ExpressionType::VALUE_CONSTANT) {
			return FilterColumn(input, expression.GetChild(i));
		}
	}
	return 0;
}

// Called while the query is optimized and again as the scan starts; the last call is the one that matters
void HintPushdown(TableFunction::FilterPushdownInput &input) {
	seen_filters.clear();
	for (idx_t i = 0; i < input.GetFilterCount(); i++) {
		auto filter = input.GetFilter(i);
		seen_filters.emplace_back(FilterColumn(input, filter), Render(input, filter));
		seen_filter_columns.push_back(seen_filters.back().first);
	}
}

void HintInitGlobal(TableFunction::InitGlobalInput &input) {
	input.SetGlobalState<HintGlobal>();
}

void HintExec(TableFunction::ExecInput &input) {
	auto &bind = input.GetBindData<HintBind>();
	auto &global = input.GetGlobalState<HintGlobal>();
	auto chunk = input.GetOutputChunk();
	const auto produced = std::min<int64_t>(std::min<int64_t>(bind.count - global.position, 5),
	                                        static_cast<int64_t>(chunk.GetCapacity()));
	for (idx_t v = 0; v < input.GetColumnCount(); v++) {
		auto vector = chunk.GetVector(v);
		auto *data = vector.GetDataMutable<int64_t>();
		const auto column = input.GetColumnIndex(v);
		for (int64_t row = 0; row < produced; row++) {
			data[row] = column == 0 ? global.position + row : 2 * (global.position + row);
		}
		vector.SetSize(static_cast<idx_t>(std::max<int64_t>(produced, 0)));
	}
	global.position += std::max<int64_t>(produced, 0);
}

// ---------------------------------------------------------------------------
// cpp_lines(file): one VARCHAR column "line", a row per line of the file, and the file_row_number virtual column.
// The lines of the tests are longer than blob_t::INLINE_LENGTH: an assertion build compiles the engine with
// DUCKDB_DEBUG_NO_INLINE, whose string_t reads the pointer even for values the API would inline.
// ---------------------------------------------------------------------------

struct LinesBind {
	std::vector<std::string> lines;
};
struct LinesGlobal {
	std::atomic<idx_t> position {0};
};

// How often the exec callback of cpp_lines was called, over all files
std::atomic<idx_t> lines_exec_calls {0};

void LinesBindFunction(TableFunction::BindInput &input) {
	input.AddResultColumn("line", input.GetContext().GetFactory().ParseType("VARCHAR"));
	LinesBind bind;
	std::ifstream file(GetFilePath(input.GetConstantArgument(0)));
	std::string line;
	while (std::getline(file, line)) {
		bind.lines.push_back(line);
	}
	input.SetBindData<LinesBind>(std::move(bind));
}

void LinesInitGlobal(TableFunction::InitGlobalInput &input) {
	input.SetGlobalState<LinesGlobal>();
}

void LinesExec(TableFunction::ExecInput &input) {
	auto &bind = input.GetBindData<LinesBind>();
	auto &global = input.GetGlobalState<LinesGlobal>();
	lines_exec_calls++;
	auto chunk = input.GetOutputChunk();
	const auto start = global.position.fetch_add(chunk.GetCapacity());
	if (start >= bind.lines.size()) {
		return;
	}
	const auto count = std::min<idx_t>(bind.lines.size() - start, chunk.GetCapacity());
	for (idx_t v = 0; v < input.GetColumnCount(); v++) {
		auto vector = chunk.GetVector(v);
		vector.SetSize(count);
		const auto column = input.GetColumnIndex(v);
		if (TableFunction::IsVirtualColumn(column)) {
			// exec runs on worker threads, where Catch assertions are not thread-safe: fail the query instead
			if (TableFunction::GetVirtualColumn(column) != TableFunction::VirtualColumn::FILE_ROW_NUMBER) {
				throw InvalidInputException("unexpected virtual column");
			}
			auto *data = vector.GetDataMutable<int64_t>();
			for (idx_t row = 0; row < count; row++) {
				data[row] = static_cast<int64_t>(start + row);
			}
			continue;
		}
		if (column != 0) {
			throw InvalidInputException("unexpected column");
		}
		for (idx_t row = 0; row < count; row++) {
			vector.AssignString(row, bind.lines[start + row]);
		}
	}
}

// ---------------------------------------------------------------------------
// cpp_metrics(n): reports n row groups, of which the scan has read half
// ---------------------------------------------------------------------------

struct MetricsGlobal {
	std::atomic<idx_t> scanned {0};
	std::atomic<idx_t> position {0};
};

std::atomic<bool> metrics_rejected_unknown {false};

auto MetricRejected(TableFunction::MetricsInput &input, std::string_view name) -> bool {
	try {
		input.SetMetric(name, 1);
	} catch (const InvalidInputException &) {
		return true;
	}
	return false;
}

void MetricsBind(TableFunction::BindInput &input) {
	input.AddResultColumn("i", input.GetContext().GetFactory().ParseType("BIGINT"));
	input.SetBindData<HintBind>(HintBind {input.GetConstantArgument(0).Get<int64_t>()});
}

void MetricsInitGlobal(TableFunction::InitGlobalInput &input) {
	input.SetGlobalState<MetricsGlobal>();
}

void MetricsExec(TableFunction::ExecInput &input) {
	auto &global = input.GetGlobalState<MetricsGlobal>();
	constexpr idx_t ROWS = 3;
	auto chunk = input.GetOutputChunk();
	const auto start = global.position.fetch_add(chunk.GetCapacity());
	if (start >= ROWS) {
		return;
	}
	if (start == 0) {
		global.scanned = 5;
	}
	const auto count = std::min<idx_t>(ROWS - start, chunk.GetCapacity());
	auto vector = chunk.GetVector(0);
	auto *data = vector.GetDataMutable<int64_t>();
	for (idx_t row = 0; row < count; row++) {
		data[row] = static_cast<int64_t>(start + row);
	}
	vector.SetSize(count);
}

void MetricsReport(TableFunction::MetricsInput &input) {
	auto global = input.GetGlobalState<MetricsGlobal>();
	if (!global) {
		return;
	}
	input.SetMetric("operator.row_groups_scanned", global->scanned.exchange(0));
	input.SetMetric("operator.total_row_groups_to_scan", input.GetBindData<HintBind>().count);
	if (MetricRejected(input, "operator.no_such_metric") && MetricRejected(input, "latency") &&
	    MetricRejected(input, "operator.cpu_time")) {
		metrics_rejected_unknown = true;
	}
}

std::optional<QueryMetricsNode> FindOperator(const QueryMetricsNode &node, std::string_view name) {
	for (idx_t i = 0; i < node.GetChildCount(); i++) {
		auto child = node.GetChild(i);
		if (child.GetName() == name) {
			return child;
		}
		auto found = FindOperator(child, name);
		if (found) {
			return found;
		}
	}
	return std::nullopt;
}

} // namespace

TEST_CASE("Stable C++API: table function sees the filters when the scan starts", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto function = TableFunction::Create(factory);
	function.SetName("cpp_hint");
	function.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	function.SetBindCallback(HintBindFunction)
	    .SetInitGlobalCallback(HintInitGlobal)
	    .SetExecCallback(HintExec)
	    .SetProjectionPushdown(true)
	    .SetFilterPushdownCallback(HintPushdown);
	conn.Register(function);

	// No filters, nothing offered
	seen_filters.clear();
	REQUIRE(CollectBigints(conn.Execute("SELECT i FROM cpp_hint(10)")).size() == 10);
	REQUIRE(seen_filters.empty());

	// The function produces every row, so it is the engine that leaves the ones that pass
	REQUIRE(CollectSortedBigints(conn.Execute("SELECT i FROM cpp_hint(10) WHERE i >= 5 AND i < 8")) ==
	        std::vector<int64_t> {5, 6, 7});
	REQUIRE_FALSE(seen_filters.empty());
	for (auto &filter : seen_filters) {
		// The filter applies to the declared column i, and is written against it
		REQUIRE(filter.first == 0);
		REQUIRE(filter.second.find("#0") != std::string::npos);
	}
	std::string all_filters;
	for (auto &filter : seen_filters) {
		all_filters += filter.second;
	}
	REQUIRE(all_filters.find("5") != std::string::npos);

	// A filter on a column the query does not otherwise read keeps that column in the scan
	seen_filters.clear();
	REQUIRE(CollectBigints(conn.Execute("SELECT j FROM cpp_hint(10) WHERE i = 3")) == std::vector<int64_t> {6});
	REQUIRE(seen_filters.size() == 1);
	REQUIRE(seen_filters[0].first == 0);
	REQUIRE(seen_filters[0].second == "(#0 = 3)");

	// Filters on both columns
	seen_filters.clear();
	REQUIRE(CollectBigints(conn.Execute("SELECT i FROM cpp_hint(10) WHERE j = 8 AND i < 7")) ==
	        std::vector<int64_t> {4});
	std::vector<idx_t> columns;
	for (auto &filter : seen_filters) {
		columns.push_back(filter.first);
	}
	std::sort(columns.begin(), columns.end());
	REQUIRE(columns.front() == 0);
	REQUIRE(columns.back() == 1);

	// Everything it produces is filtered, batch after batch, and the scan still goes on to the end
	REQUIRE(CollectBigints(conn.Execute("SELECT i FROM cpp_hint(100) WHERE i = 99")) == std::vector<int64_t> {99});
	REQUIRE(CollectBigints(conn.Execute("SELECT count(*) FROM cpp_hint(100) WHERE i > 1000")) ==
	        std::vector<int64_t> {0});

	// A scan whose rows all fail the filters can still be interrupted
	conn.Execute("SET max_execution_time = 200").Drain();
	REQUIRE_THROWS_AS(conn.Execute("SELECT count(*) FROM cpp_hint(1000000000000000) WHERE i < 0").Drain(),
	                  InterruptException);
}

TEST_CASE("Stable C++API: table function virtual columns need projection pushdown", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto virtual_function = TableFunction::Create(factory);
	virtual_function.SetName("cpp_virtual_without");
	virtual_function.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	virtual_function.SetBindCallback(HintBindFunction)
	    .SetExecCallback(HintExec)
	    .AddVirtualColumn(TableFunction::VirtualColumn::FILE_ROW_NUMBER);
	REQUIRE_THROWS_AS(conn.Register(virtual_function), Exception);
}

TEST_CASE("Stable C++API: table function virtual columns", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	const auto directory = std::filesystem::temp_directory_path() / "cpp_api_virtual_columns";
	std::filesystem::create_directories(directory);
	const auto first = (directory / "a.lines").string();
	const auto second = (directory / "b.lines").string();
	std::ofstream(first) << "x is a long line\ny is a long line\nz is a long line\n";
	std::ofstream(second) << "p is a long line\nq is a long line\n";

	auto single = TableFunction::Create(factory);
	single.SetName("cpp_lines");
	single.GetSignature().AddParameter("file", factory.CreateType(LogicalTypeId::ANY));
	single.SetBindCallback(LinesBindFunction)
	    .SetInitGlobalCallback(LinesInitGlobal)
	    .SetExecCallback(LinesExec)
	    .SetProjectionPushdown(true)
	    .AddVirtualColumn(TableFunction::VirtualColumn::FILE_ROW_NUMBER);
	conn.Register(single);

	auto multi = MultiFileFunction::Create(factory);
	multi.SetName("cpp_read_lines").SetSingleFileFunction("cpp_lines").SetReaderType("Lines").SetFileExtension("lines");
	conn.Register(multi);

	const std::string files = "['" + first + "', '" + second + "']";
	REQUIRE(CollectBigints(conn.Execute("SELECT count(*) FROM cpp_read_lines(" + files + ")")) ==
	        std::vector<int64_t> {5});
	REQUIRE(CollectStrings(conn.Execute("SELECT line || ':' || file_row_number FROM cpp_read_lines(" + files +
	                                    ") ORDER BY line")) ==
	        std::vector<std::string> {"p is a long line:0", "q is a long line:1", "x is a long line:0",
	                                  "y is a long line:1", "z is a long line:2"});
	REQUIRE(CollectStrings(conn.Execute("SELECT line FROM cpp_read_lines(" + files +
	                                    ") WHERE file_row_number = 1 ORDER BY line")) ==
	        std::vector<std::string> {"q is a long line", "y is a long line"});
	REQUIRE(CollectBigints(conn.Execute("SELECT max(file_row_number) FROM cpp_read_lines(" + files + ")")) ==
	        std::vector<int64_t> {2});

	// Not selected by default
	auto described = conn.Execute("SELECT * FROM cpp_read_lines('" + first + "')");
	REQUIRE(described.FetchChunk().GetVectorCount() == 1);

	std::filesystem::remove_all(directory);
}

TEST_CASE("Stable C++API: table function virtual column indexes", "[cpp_api]") {
	REQUIRE_FALSE(TableFunction::IsVirtualColumn(0));
	REQUIRE_FALSE(TableFunction::IsVirtualColumn(1000));
	REQUIRE(TableFunction::IsVirtualColumn(TableFunction::VIRTUAL_COLUMN_START));
	REQUIRE(TableFunction::GetVirtualColumn(TableFunction::VIRTUAL_COLUMN_START) ==
	        TableFunction::VirtualColumn::FILE_ROW_NUMBER);
	REQUIRE_THROWS_AS(TableFunction::GetVirtualColumn(3), InvalidInputException);
	REQUIRE_THROWS_AS(TableFunction::GetVirtualColumn(TableFunction::VIRTUAL_COLUMN_START + 1000),
	                  InvalidInputException);
}

TEST_CASE("Stable C++API: table function metrics", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto function = TableFunction::Create(factory);
	function.SetName("cpp_metrics");
	function.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	function.SetBindCallback(MetricsBind)
	    .SetInitGlobalCallback(MetricsInitGlobal)
	    .SetExecCallback(MetricsExec)
	    .SetMetricsCallback(MetricsReport);
	conn.Register(function);

	conn.Execute("SET enable_profiling = 'no_output'").Drain();
	conn.Execute("SET tracked_metrics = ['operator.row_groups_scanned', 'operator.total_row_groups_to_scan']").Drain();
	conn.EnableProfiling();
	REQUIRE(CollectSortedBigints(conn.Execute("SELECT i FROM cpp_metrics(12)")) == std::vector<int64_t> {0, 1, 2});

	auto query_metrics = conn.GetQueryMetrics();
	auto scan = FindOperator(query_metrics.GetRoot(), "TABLE_SCAN");
	REQUIRE(scan.has_value());
	auto scanned = scan->GetMetric("operator.row_groups_scanned");
	auto total = scan->GetMetric("operator.total_row_groups_to_scan");
	REQUIRE(scanned.has_value());
	REQUIRE(total.has_value());
	REQUIRE(scanned->Get<uint64_t>() == 5);
	REQUIRE(total->Get<uint64_t>() == 12);
	REQUIRE(metrics_rejected_unknown.load());
}

TEST_CASE("Stable C++API: filter pushdown sees the index of a virtual column", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	const auto directory = std::filesystem::temp_directory_path() / "cpp_api_virtual_column_filters";
	std::filesystem::create_directories(directory);
	const auto file = (directory / "a.lines").string();
	std::ofstream(file) << "x is a long line\ny is a long line\nz is a long line\n";

	auto single = TableFunction::Create(factory);
	single.SetName("cpp_lines_hint");
	single.GetSignature().AddParameter("file", factory.CreateType(LogicalTypeId::ANY));
	single.SetBindCallback(LinesBindFunction)
	    .SetInitGlobalCallback(LinesInitGlobal)
	    .SetExecCallback(LinesExec)
	    .SetProjectionPushdown(true)
	    .SetFilterPushdownCallback(HintPushdown)
	    .AddVirtualColumn(TableFunction::VirtualColumn::FILE_ROW_NUMBER);
	conn.Register(single);

	auto multi = MultiFileFunction::Create(factory);
	multi.SetName("cpp_read_lines_hint")
	    .SetSingleFileFunction("cpp_lines_hint")
	    .SetReaderType("Lines")
	    .SetFileExtension("lines");
	conn.Register(multi);

	// Scanned directly: the callback runs while the query is optimized and as the scan starts
	for (auto function : {"cpp_lines_hint", "cpp_read_lines_hint"}) {
		INFO(function);
		seen_filter_columns.clear();
		REQUIRE(CollectStrings(conn.Execute(std::string("SELECT line FROM ") + function + "('" + file +
		                                    "') WHERE file_row_number = 1")) ==
		        std::vector<std::string> {"y is a long line"});
		REQUIRE_FALSE(seen_filter_columns.empty());
		for (auto column : seen_filter_columns) {
			REQUIRE(TableFunction::IsVirtualColumn(column));
			REQUIRE(TableFunction::GetVirtualColumn(column) == TableFunction::VirtualColumn::FILE_ROW_NUMBER);
		}
	}

	std::filesystem::remove_all(directory);
}

namespace {

// ---------------------------------------------------------------------------
// cpp_numbers(file): one column "i" of the type on the first line of the file, with a row per other line.
// ---------------------------------------------------------------------------

struct NumbersBind {
	bool is_bigint = false;
	std::vector<int64_t> values;
};

void NumbersBindFunction(TableFunction::BindInput &input) {
	std::ifstream file(GetFilePath(input.GetConstantArgument(0)));
	std::string line;
	std::getline(file, line);
	NumbersBind bind;
	bind.is_bigint = line == "BIGINT";
	input.AddResultColumn("i", input.GetContext().GetFactory().ParseType(line));
	while (std::getline(file, line)) {
		bind.values.push_back(std::stoll(line));
	}
	input.SetBindData<NumbersBind>(std::move(bind));
}

void NumbersExec(TableFunction::ExecInput &input) {
	auto &bind = input.GetBindData<NumbersBind>();
	auto &global = input.GetGlobalState<LinesGlobal>();
	auto chunk = input.GetOutputChunk();
	const auto start = global.position.fetch_add(chunk.GetCapacity());
	if (start >= bind.values.size()) {
		return;
	}
	const auto count = std::min<idx_t>(bind.values.size() - start, chunk.GetCapacity());
	auto vector = chunk.GetVector(0);
	for (idx_t row = 0; row < count; row++) {
		if (bind.is_bigint) {
			vector.GetDataMutable<int64_t>()[row] = bind.values[start + row];
		} else {
			vector.GetDataMutable<int32_t>()[row] = static_cast<int32_t>(bind.values[start + row]);
		}
	}
	vector.SetSize(count);
}

} // namespace

TEST_CASE("Stable C++API: filters of a multi-file scan apply to the values in the type of the scan", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	const auto directory = std::filesystem::temp_directory_path() / "cpp_api_multi_file_filter_types";
	std::filesystem::create_directories(directory);
	const auto first = (directory / "a.numbers").string();
	const auto second = (directory / "b.numbers").string();
	std::ofstream(first) << "INTEGER\n1\n2\n";
	std::ofstream(second) << "BIGINT\n3000000000\n5\n";

	auto single = TableFunction::Create(factory);
	single.SetName("cpp_numbers");
	single.GetSignature().AddParameter("file", factory.CreateType(LogicalTypeId::ANY));
	single.SetBindCallback(NumbersBindFunction)
	    .SetInitGlobalCallback(LinesInitGlobal)
	    .SetExecCallback(NumbersExec)
	    .SetProjectionPushdown(true)
	    .SetFilterPushdownCallback(HintPushdown);
	conn.Register(single);

	auto multi = MultiFileFunction::Create(factory);
	multi.SetName("cpp_read_numbers")
	    .SetSingleFileFunction("cpp_numbers")
	    .SetReaderType("Numbers")
	    .SetFileExtension("numbers");
	conn.Register(multi);

	// The INTEGER file is read as BIGINT, the type of the scan, before the filters apply
	const auto source = "FROM cpp_read_numbers(['" + first + "', '" + second + "'], union_by_name = true) ";
	REQUIRE(CollectSortedBigints(conn.Execute("SELECT i " + source)) == std::vector<int64_t> {1, 2, 5, 3000000000});
	REQUIRE(CollectSortedBigints(conn.Execute("SELECT i " + source + "WHERE i >= 2147483648")) ==
	        std::vector<int64_t> {3000000000});
	REQUIRE(CollectSortedBigints(conn.Execute("SELECT i " + source + "WHERE i = 2 OR i = 5")) ==
	        std::vector<int64_t> {2, 5});
	REQUIRE(CollectSortedBigints(conn.Execute("SELECT i " + source + "WHERE i < 3")) == std::vector<int64_t> {1, 2});
	REQUIRE(CollectSortedBigints(conn.Execute("SELECT i " + source + "WHERE i > 1 AND i < 2147483648")) ==
	        std::vector<int64_t> {2, 5});

	std::filesystem::remove_all(directory);
}

namespace {

// How often the statistics callback of cpp_lines_stats was called
std::atomic<idx_t> lines_stats_calls {0};

// The bounds of the lines of the file
void LinesStats(TableFunction::StatsInput &input) {
	lines_stats_calls++;
	auto &bind = input.GetBindData<LinesBind>();
	auto bounds = std::minmax_element(bind.lines.begin(), bind.lines.end());
	auto &factory = input.GetContext().GetFactory();
	std::vector<std::pair<std::string, Value>> fields;
	fields.emplace_back("min", Value::Create(factory, varchar_t(*bounds.first)));
	fields.emplace_back("max", Value::Create(factory, varchar_t(*bounds.second)));
	input.GetResultStats().SetValue(Value::CreateStruct(factory, fields));
}

} // namespace

TEST_CASE("Stable C++API: multi-file function uses the stats callback of its table function", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	const auto directory = std::filesystem::temp_directory_path() / "cpp_api_multi_file_stats";
	std::filesystem::create_directories(directory);
	const auto first = (directory / "a.lines").string();
	const auto second = (directory / "b.lines").string();
	std::ofstream(first) << "b is a long line\nc is a long line\n";
	std::ofstream(second) << "x is a long line\ny is a long line\nz is a long line\n";

	auto single = TableFunction::Create(factory);
	single.SetName("cpp_lines_stats");
	single.GetSignature().AddParameter("file", factory.CreateType(LogicalTypeId::ANY));
	single.SetBindCallback(LinesBindFunction)
	    .SetInitGlobalCallback(LinesInitGlobal)
	    .SetExecCallback(LinesExec)
	    .SetProjectionPushdown(true)
	    .SetStatsCallback(LinesStats);
	conn.Register(single);

	auto multi = MultiFileFunction::Create(factory);
	multi.SetName("cpp_read_lines_stats")
	    .SetSingleFileFunction("cpp_lines_stats")
	    .SetReaderType("Lines")
	    .SetFileExtension("lines");
	conn.Register(multi);

	auto count = [&](const std::string &files, const std::string &predicate) {
		lines_stats_calls = 0;
		lines_exec_calls = 0;
		return CollectBigints(
		    conn.Execute("SELECT count(*) FROM cpp_read_lines_stats(" + files + ") WHERE line " + predicate));
	};

	// The statistics rule out the predicate, so no file is scanned
	const auto one_file = "'" + first + "'";
	REQUIRE(count(one_file, "> 'd'") == std::vector<int64_t> {0});
	REQUIRE(lines_stats_calls > 0);
	REQUIRE(lines_exec_calls == 0);
	REQUIRE(count(one_file, "> 'b'") == std::vector<int64_t> {2});
	REQUIRE(lines_exec_calls > 0);

	// The statistics of a scan of several files are those of all of them, which union_by_name opens at bind
	const auto both_files = "['" + first + "', '" + second + "'], union_by_name = true";
	REQUIRE(count(both_files, "< 'a'") == std::vector<int64_t> {0});
	REQUIRE(lines_stats_calls > 0);
	REQUIRE(lines_exec_calls == 0);
	REQUIRE(count(both_files, "> 'zz'") == std::vector<int64_t> {0});
	REQUIRE(lines_exec_calls == 0);
	REQUIRE(count(both_files, "> 'x'") == std::vector<int64_t> {3});
	REQUIRE(lines_exec_calls > 0);
	REQUIRE(count(both_files, "< 'c'") == std::vector<int64_t> {1});
	REQUIRE(lines_exec_calls > 0);

	std::filesystem::remove_all(directory);
}
