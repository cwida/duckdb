#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

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

std::string Render(const Expression &expression) {
	switch (expression.GetType()) {
	case ExpressionType::BOUND_COLUMN_REF:
		return "#" + std::to_string(expression.GetColumnIndex());
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
		result += (i ? " " + name + " " : "") + Render(expression.GetChild(i));
	}
	return result + ")";
}

void HintBindFunction(TableFunction::BindInput &input) {
	input.AddResultColumn("i", input.GetContext().GetFactory().ParseType("BIGINT"));
	input.AddResultColumn("j", input.GetContext().GetFactory().ParseType("BIGINT"));
	input.SetBindData<HintBind>(HintBind {input.GetConstantArgument(0).Get<int64_t>()});
}

void HintInitGlobal(TableFunction::InitGlobalInput &input) {
	seen_filters.clear();
	for (idx_t i = 0; i < input.GetFilterCount(); i++) {
		const auto position = input.GetFilterColumn(i);
		seen_filters.emplace_back(input.GetColumnIndex(position), Render(input.GetFilter(i)));
	}
	input.SetGlobalState<HintGlobal>();
}

void HintExec(TableFunction::ExecInput &input) {
	auto &bind = input.GetBindData<HintBind>();
	auto &global = input.GetGlobalState<HintGlobal>();
	auto chunk = input.GetOutputChunk();
	const auto produced = std::min<int64_t>(bind.count - global.position, 5);
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
// ---------------------------------------------------------------------------

struct LinesBind {
	std::vector<std::string> lines;
};
struct LinesGlobal {
	std::atomic<bool> done {false};
};

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
	if (global.done.exchange(true)) {
		return;
	}
	auto chunk = input.GetOutputChunk();
	for (idx_t v = 0; v < input.GetColumnCount(); v++) {
		auto vector = chunk.GetVector(v);
		vector.SetSize(bind.lines.size());
		const auto column = input.GetColumnIndex(v);
		if (TableFunction::IsVirtualColumn(column)) {
			REQUIRE(TableFunction::GetVirtualColumn(column) == TableFunction::VirtualColumn::FILE_ROW_NUMBER);
			auto *data = vector.GetDataMutable<int64_t>();
			for (idx_t row = 0; row < bind.lines.size(); row++) {
				data[row] = static_cast<int64_t>(row);
			}
			continue;
		}
		REQUIRE(column == 0);
		for (idx_t row = 0; row < bind.lines.size(); row++) {
			vector.AssignString(row, bind.lines[row]);
		}
	}
}

// ---------------------------------------------------------------------------
// cpp_metrics(n): reports n row groups, of which the scan has read half
// ---------------------------------------------------------------------------

struct MetricsGlobal {
	std::atomic<idx_t> scanned {0};
};

void MetricsBind(TableFunction::BindInput &input) {
	input.AddResultColumn("i", input.GetContext().GetFactory().ParseType("BIGINT"));
	input.SetBindData<HintBind>(HintBind {input.GetConstantArgument(0).Get<int64_t>()});
}

void MetricsInitGlobal(TableFunction::InitGlobalInput &input) {
	input.SetGlobalState<MetricsGlobal>();
}

void MetricsExec(TableFunction::ExecInput &input) {
	auto &global = input.GetGlobalState<MetricsGlobal>();
	if (global.scanned.load() > 0) {
		return;
	}
	global.scanned = 5;
	auto chunk = input.GetOutputChunk();
	auto vector = chunk.GetVector(0);
	auto *data = vector.GetDataMutable<int64_t>();
	for (int64_t row = 0; row < 3; row++) {
		data[row] = row;
	}
	vector.SetSize(3);
}

void MetricsReport(TableFunction::MetricsInput &input) {
	auto global = input.GetGlobalState<MetricsGlobal>();
	if (!global) {
		return;
	}
	input.SetRowGroups(global->scanned.exchange(0), input.GetBindData<HintBind>().count);
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

TEST_CASE("Stable C++API: table function filter hints", "[cpp_api]") {
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
	    .SetFilterHints(true);
	conn.Register(function);

	// No filters, no hints
	REQUIRE(CollectBigints(conn.Execute("SELECT i FROM cpp_hint(10)")).size() == 10);
	REQUIRE(seen_filters.empty());

	// The function produces every row, so it is the engine that leaves the ones that pass
	REQUIRE(CollectBigints(conn.Execute("SELECT i FROM cpp_hint(10) WHERE i >= 5 AND i < 8")) ==
	        std::vector<int64_t> {5, 6, 7});
	REQUIRE_FALSE(seen_filters.empty());
	for (auto &filter : seen_filters) {
		// The filter applies to the column i, whichever place that has in the scan, and is written against it
		REQUIRE(filter.first == 0);
		REQUIRE(filter.second.find("#0") != std::string::npos);
	}
	std::string all_filters;
	for (auto &filter : seen_filters) {
		all_filters += filter.second;
	}
	REQUIRE(all_filters.find("5") != std::string::npos);

	// A filter on a column the query does not otherwise read keeps that column in the scan
	REQUIRE(CollectBigints(conn.Execute("SELECT j FROM cpp_hint(10) WHERE i = 3")) == std::vector<int64_t> {6});
	REQUIRE(seen_filters.size() == 1);
	REQUIRE(seen_filters[0].first == 0);
	REQUIRE(seen_filters[0].second == "(#0 = 3)");

	// Filters on both columns
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
}

TEST_CASE("Stable C++API: table function filter hints need projection pushdown", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto function = TableFunction::Create(factory);
	function.SetName("cpp_hint_without");
	function.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	function.SetBindCallback(HintBindFunction).SetExecCallback(HintExec).SetFilterHints(true);
	REQUIRE_THROWS_AS(conn.Register(function), Exception);

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
	std::ofstream(first) << "x\ny\nz\n";
	std::ofstream(second) << "p\nq\n";

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
	        std::vector<std::string> {"p:0", "q:1", "x:0", "y:1", "z:2"});
	REQUIRE(CollectStrings(conn.Execute("SELECT line FROM cpp_read_lines(" + files +
	                                    ") WHERE file_row_number = 1 ORDER BY line")) ==
	        std::vector<std::string> {"q", "y"});
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
	REQUIRE(CollectBigints(conn.Execute("SELECT i FROM cpp_metrics(12)")) == std::vector<int64_t> {0, 1, 2});

	auto query_metrics = conn.GetQueryMetrics();
	auto scan = FindOperator(query_metrics.GetRoot(), "TABLE_SCAN");
	REQUIRE(scan.has_value());
	auto scanned = scan->GetMetric("operator.row_groups_scanned");
	auto total = scan->GetMetric("operator.total_row_groups_to_scan");
	REQUIRE(scanned.has_value());
	REQUIRE(total.has_value());
	REQUIRE(scanned->Get<uint64_t>() == 5);
	REQUIRE(total->Get<uint64_t>() == 12);
}
