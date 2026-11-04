#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: ReplacementScan. Each of the three claim forms, the
// scoping rules, and the error path.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

// Collect a single BIGINT column, asserting every row valid.
std::vector<int64_t> CollectScanBigints(QueryResult result) {
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

// What a scan resolves names against, carried as user data.
struct ScanRegistry {
	std::string name;
	const ColumnDataCollection *collection = nullptr;
	int calls = 0;
};

// What the callback saw, latched for the test to assert on afterwards. A callback must not use REQUIRE: it would
// throw through the callback boundary into the engine.
struct ScanObserved {
	std::vector<std::string> parts;
	std::string rendered;
	int calls = 0;
};
ScanObserved scan_observed;

// Claims every name with range(2), recording what it saw.
void ClaimRange(ReplacementScan::Input &input) {
	scan_observed.calls++;
	auto name = input.GetName();
	scan_observed.parts.clear();
	for (idx_t i = 0; i < name.GetPartCount(); i++) {
		scan_observed.parts.emplace_back(name.GetPart(i));
	}
	scan_observed.rendered = name.Render();

	auto &factory = input.GetContext().GetFactory();
	input.SetFunctionName("range");
	input.AddArgument(Value::Create(factory, int64_t {2}));
	input.SetAlias("claimed");
}

// Claims only the registry's name, with its collection.
void ClaimCollection(ReplacementScan::Input &input) {
	auto &registry = input.GetUserData<ScanRegistry>();
	auto name = input.GetName();
	if (name.GetPartCount() != 1 || name.GetName() != registry.name) {
		return; // decline
	}
	registry.calls++;
	input.SetCollection(*registry.collection, {"amount"});
}

void ClaimSubquery(ReplacementScan::Input &input) {
	input.SetSubquery("SELECT 41 + 1 AS v");
}

void ThrowingScan(ReplacementScan::Input &) {
	throw InvalidInputException("the replacement scan refused");
}

// A scan that claims nothing.
void DeclineEverything(ReplacementScan::Input &input) {
	input.GetUserData<ScanRegistry>().calls++;
}

// Builds a single-column BIGINT collection holding the given values.
auto MakeScanCollection(Connection &conn, const std::vector<int64_t> &values) -> ColumnDataCollection {
	auto &factory = conn.GetFactory();
	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("BIGINT"));
	ColumnDataCollection collection(factory, types);

	DataChunk chunk(factory, types);
	auto vec = chunk.GetVector(0);
	auto *data = vec.GetDataMutable<int64_t>();
	for (size_t i = 0; i < values.size(); i++) {
		data[i] = values[i];
	}
	vec.SetSize(values.size());
	collection.Append(chunk);
	return collection;
}

} // namespace

TEST_CASE("Stable C++API: replacement scan claims a table function", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	scan_observed = ScanObserved();
	auto scan = ReplacementScan::Create(conn.GetFactory());
	scan.SetCallback(ClaimRange);
	conn.Register(scan);

	REQUIRE(CollectScanBigints(conn.Execute("SELECT * FROM not_a_table")) == std::vector<int64_t> {0, 1});
	REQUIRE(scan_observed.calls == 1);
	REQUIRE(scan_observed.parts == std::vector<std::string> {"not_a_table"});
	// The scan's alias names the binding, so a qualified reference resolves.
	REQUIRE(CollectScanBigints(conn.Execute("SELECT claimed.range FROM not_a_table")) == std::vector<int64_t> {0, 1});
	// A query-written alias wins over the scan's.
	REQUIRE(CollectScanBigints(conn.Execute("SELECT t.range FROM not_a_table AS t")) == std::vector<int64_t> {0, 1});
}

TEST_CASE("Stable C++API: replacement scan reports the unresolved name", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto scan = ReplacementScan::Create(conn.GetFactory());
	scan.SetCallback(ClaimRange);
	conn.Register(scan);

	// An unqualified reference is a single part -- absence is a shorter path, not an empty placeholder.
	scan_observed = ScanObserved();
	conn.Execute("SELECT * FROM plain_name").Drain();
	REQUIRE(scan_observed.calls == 1);
	REQUIRE(scan_observed.parts == std::vector<std::string> {"plain_name"});
	REQUIRE(scan_observed.rendered == "plain_name");

	// A fully qualified one carries all three, outermost first.
	scan_observed = ScanObserved();
	conn.Execute("SELECT * FROM memory.main.qualified_name").Drain();
	REQUIRE(scan_observed.calls == 1);
	REQUIRE(scan_observed.parts == std::vector<std::string> {"memory", "main", "qualified_name"});
	REQUIRE(scan_observed.rendered == "memory.main.qualified_name");
}

TEST_CASE("Stable C++API: qualified name", "[cpp_api]") {
	auto one = QualifiedName::Create({"tbl"});
	REQUIRE(one.GetPartCount() == 1);
	REQUIRE(one.GetPart(0) == "tbl");
	REQUIRE(one.GetName() == "tbl");
	REQUIRE(one.Render() == "tbl");

	auto three = QualifiedName::Create({"cat", "sch", "tbl"});
	REQUIRE(three.GetPartCount() == 3);
	REQUIRE(three.GetName() == "tbl");
	REQUIRE(three.Render() == "cat.sch.tbl");

	// Rendering round-trips through Parse, quoting only where the identifier needs it.
	auto quoted = QualifiedName::Create({"we.ird", "tbl"});
	REQUIRE(quoted.Render() == "\"we.ird\".tbl");
	REQUIRE(QualifiedName::Parse(quoted.Render()) == quoted);

	// Equality is case-insensitive, and hashes agree with it.
	REQUIRE(QualifiedName::Parse("Cat.Sch.Tbl") == three);
	REQUIRE(QualifiedName::Parse("Cat.Sch.Tbl").Hash() == three.Hash());
	REQUIRE(one != three);

	// Partial qualification is fewer parts, never an empty placeholder.
	REQUIRE(QualifiedName::Parse("sch.tbl").GetPartCount() == 2);
	REQUIRE_THROWS_MATCHES(QualifiedName::Create({}), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE_THROWS_MATCHES(QualifiedName::Create({"a", "b", "c", "d"}), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE_THROWS_MATCHES(QualifiedName::Create({"a", ""}), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE_THROWS_AS(one.GetPart(1), Exception);
}

TEST_CASE("Stable C++API: replacement scan reports the name and can decline", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto scan = ReplacementScan::Create(conn.GetFactory());
	scan.SetCallback(DeclineEverything).SetUserData<ScanRegistry>();
	conn.Register(scan);

	// Declining leaves the reference unresolved.
	REQUIRE_THROWS_AS(conn.Execute("SELECT * FROM missing_table").Drain(), Exception);
}

TEST_CASE("Stable C++API: replacement scan claims a column data collection", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto collection = MakeScanCollection(conn, {10, 20});

	ScanRegistry seed;
	seed.name = "my_batch";
	seed.collection = &collection;

	auto scan = ReplacementScan::Create(conn.GetFactory());
	scan.SetCallback(ClaimCollection).SetUserData<ScanRegistry>(seed);
	conn.Register(scan);

	REQUIRE(CollectScanBigints(conn.Execute("SELECT amount FROM my_batch ORDER BY amount")) ==
	        std::vector<int64_t> {10, 20});

	// The motivating case: a client-side buffer as the source of an INSERT.
	conn.Execute("CREATE TABLE sink (v BIGINT)").Drain();
	conn.Execute("INSERT INTO sink SELECT * FROM my_batch").Drain();
	REQUIRE(CollectScanBigints(conn.Execute("SELECT v FROM sink ORDER BY v")) == std::vector<int64_t> {10, 20});

	// Names it does not recognise are declined.
	REQUIRE_THROWS_AS(conn.Execute("SELECT * FROM some_other_name").Drain(), Exception);
}

TEST_CASE("Stable C++API: replacement scan claims a subquery", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto scan = ReplacementScan::Create(conn.GetFactory());
	scan.SetCallback(ClaimSubquery);
	conn.Register(scan);

	REQUIRE(CollectScanBigints(conn.Execute("SELECT v::BIGINT FROM anything")) == std::vector<int64_t> {42});
}

TEST_CASE("Stable C++API: replacement scan scoping", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto other = db.Connect();

	auto scan = ReplacementScan::Create(conn.GetFactory());
	scan.SetCallback(ClaimSubquery);
	conn.Register(scan);

	// Visible on the registering connection only.
	REQUIRE(CollectScanBigints(conn.Execute("SELECT v::BIGINT FROM anything")) == std::vector<int64_t> {42});
	REQUIRE_THROWS_AS(other.Execute("SELECT * FROM anything").Drain(), Exception);

	// A database-scoped scan reaches every connection.
	auto db_scan = ReplacementScan::Create(db.GetFactory());
	db_scan.SetCallback(ClaimSubquery);
	db.Register(db_scan);
	REQUIRE(CollectScanBigints(other.Execute("SELECT v::BIGINT FROM anything")) == std::vector<int64_t> {42});
}

TEST_CASE("Stable C++API: replacement scan errors", "[cpp_api]") {
	SECTION("a throwing callback fails the query") {
		auto db = Instance(":memory:");
		auto conn = db.Connect();

		auto scan = ReplacementScan::Create(conn.GetFactory());
		scan.SetCallback(ThrowingScan);
		conn.Register(scan);
		REQUIRE_THROWS_MATCHES(conn.Execute("SELECT * FROM anything").Drain(), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	SECTION("registration requires a callback, and happens once") {
		auto db = Instance(":memory:");
		auto conn = db.Connect();

		auto scan = ReplacementScan::Create(conn.GetFactory());
		REQUIRE_THROWS_MATCHES(conn.Register(scan), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
		scan.SetCallback(ClaimSubquery);
		conn.Register(scan);
		REQUIRE_THROWS_MATCHES(conn.Register(scan), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
}
