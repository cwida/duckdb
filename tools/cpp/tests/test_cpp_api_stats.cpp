#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"

#include <string>

// ---------------------------------------------------------------------------
// Stable C++ API tests: stats callbacks and Stats. The functions here produce data their stats callback denies: a
// query filtering on the denied kind of value returns a wrong count exactly when the optimizer acted on the
// guarantee, which makes the callback's effect observable. A callback that leaves the result untouched gets the
// correct count.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

// What a stats callback does to the result statistics, planted as user data.
struct StatsMode {
	bool set_null = false;
	bool can_have_null = true;
	bool set_valid = false;
	bool can_have_valid = true;
	// Scalar exec only: write NULL instead of 7 into every row
	bool write_nulls = false;
};

// What the last stats callback saw, latched for the test to assert after the query.
struct StatsProbe {
	int calls = 0;
	idx_t arg_count = 0;
	bool arg_can_have_null = false;
	bool arg_can_have_valid = false;
	bool arg_write_refused = false;
	bool result_can_have_null = false;
	bool result_can_have_valid = false;
	idx_t column_index = 0;
};
StatsProbe stats_probe;

int64_t CountOf(Connection &conn, const std::string &sql) {
	auto result = conn.Execute(sql);
	auto chunk = result.FetchChunk();
	REQUIRE(chunk);
	auto view = chunk.GetVector(0).GetView();
	return view.Data<int64_t>()[view.SelAt(0)];
}

void ApplyMode(Stats result, const StatsMode &mode) {
	if (mode.set_null) {
		result.SetCanHaveNull(mode.can_have_null);
	}
	if (mode.set_valid) {
		result.SetCanHaveValid(mode.can_have_valid);
	}
	stats_probe.result_can_have_null = result.CanHaveNull();
	stats_probe.result_can_have_valid = result.CanHaveValid();
}

void ProbeArg(Stats arg) {
	stats_probe.arg_can_have_null = arg.CanHaveNull();
	stats_probe.arg_can_have_valid = arg.CanHaveValid();
	try {
		arg.SetCanHaveNull(false);
	} catch (const InvalidInputException &) {
		stats_probe.arg_write_refused = true;
	}
}

// ---------------------------------------------------------------------------
// cpp_stats_scalar(x INTEGER) -> INTEGER: 7 (or NULL) in every row.
// ---------------------------------------------------------------------------

void SevenExec(ScalarFunction::ExecInput &input) {
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	const auto count = input.GetRowCount();
	for (idx_t i = 0; i < count; i++) {
		out[i] = 7;
	}
	if (input.GetUserData<StatsMode>().write_nulls) {
		result.GetValidityMutable().SetAllInvalid(count);
	}
}

void ScalarStats(ScalarFunction::StatsInput &input) {
	stats_probe.calls++;
	stats_probe.arg_count = input.GetArgCount();
	ProbeArg(input.GetArgStats(0));
	ApplyMode(input.GetResultStats(), input.GetUserData<StatsMode>());
}

void RegisterScalar(Connection &conn, const std::string &name, const StatsMode &mode) {
	const auto integer = conn.GetFactory().ParseType("INTEGER");
	auto function = ScalarFunction::Create(conn.GetFactory());
	function.SetName(name).SetExecCallback(SevenExec).SetStatsCallback(ScalarStats).SetUserData<StatsMode>(mode);
	function.GetSignature().AddParameter("x", integer).SetReturnType(integer);
	conn.Register(function);
}

// ---------------------------------------------------------------------------
// cpp_stats_agg(x INTEGER) -> BIGINT: finalizes every group to 7.
// ---------------------------------------------------------------------------

void AggSize(AggregateFunction::SizeInput &input) {
	input.SetStateSize(sizeof(int64_t));
}
void AggInit(AggregateFunction::InitInput &) {
}
void AggUpdate(AggregateFunction::UpdateInput &) {
}
void AggCombine(AggregateFunction::CombineInput &) {
}
void AggFinalize(AggregateFunction::FinalizeInput &input) {
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int64_t>();
	const auto offset = input.GetResultOffset();
	for (idx_t i = 0; i < input.GetStateCount(); i++) {
		out[offset + i] = 7;
	}
}

void AggStats(AggregateFunction::StatsInput &input) {
	stats_probe.calls++;
	stats_probe.arg_count = input.GetArgCount();
	ProbeArg(input.GetArgStats(0));
	ApplyMode(input.GetResultStats(), input.GetUserData<StatsMode>());
}

void RegisterAggregate(Connection &conn, const std::string &name, const StatsMode &mode) {
	auto function = AggregateFunction::Create(conn.GetFactory());
	function.SetName(name)
	    .WithSignature([&](FunctionSignature &sig) {
		    sig.AddParameter("x", conn.GetFactory().ParseType("INTEGER"))
		        .SetReturnType(conn.GetFactory().ParseType("BIGINT"));
	    })
	    .SetSizeCallback(AggSize)
	    .SetInitCallback(AggInit)
	    .SetUpdateCallback(AggUpdate)
	    .SetCombineCallback(AggCombine)
	    .SetFinalizeCallback(AggFinalize)
	    .SetStatsCallback(AggStats)
	    .SetUserData<StatsMode>(mode);
	conn.Register(function);
}

// ---------------------------------------------------------------------------
// cpp_stats_pair(): INTEGER columns "a" and "b", three rows of 7, one per batch so any vector size fits. The stats
// callback applies the mode to "b" only.
// ---------------------------------------------------------------------------

struct PairGlobal {
	idx_t produced = 0;
};

void PairBind(TableFunction::BindInput &input) {
	const auto integer = input.GetContext().GetFactory().ParseType("INTEGER");
	input.AddResultColumn("a", integer);
	input.AddResultColumn("b", integer);
}

void PairInitGlobal(TableFunction::InitGlobalInput &input) {
	input.SetGlobalState<PairGlobal>();
}

void PairExec(TableFunction::ExecInput &input) {
	auto &global = input.GetGlobalState<PairGlobal>();
	const idx_t produced = global.produced < 3 ? 1 : 0;
	global.produced += produced;
	auto chunk = input.GetOutputChunk();
	for (idx_t col = 0; col < 2; col++) {
		auto *out = chunk.GetVector(col).GetDataMutable<int32_t>();
		for (idx_t i = 0; i < produced; i++) {
			out[i] = 7;
		}
	}
	// The first vector's size is the batch's row count; 0 ends the scan.
	chunk.GetVector(0).SetSize(produced);
}

void PairStats(TableFunction::StatsInput &input) {
	stats_probe.calls++;
	const auto column = input.GetColumnIndex();
	if (column == 1) {
		stats_probe.column_index = column;
		ApplyMode(input.GetResultStats(), input.GetUserData<StatsMode>());
	}
}

void RegisterPair(Connection &conn, const std::string &name, const StatsMode &mode) {
	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName(name)
	    .SetBindCallback(PairBind)
	    .SetInitGlobalCallback(PairInitGlobal)
	    .SetExecCallback(PairExec)
	    .SetStatsCallback(PairStats)
	    .SetUserData<StatsMode>(mode);
	conn.Register(function);
}

// Tables whose column statistics the engine knows from storage: "t" has no NULLs, "tn" has one.
void CreateTables(Connection &conn) {
	conn.Execute("CREATE TABLE t AS SELECT range::INTEGER AS i FROM range(3)").Drain();
	conn.Execute("CREATE TABLE tn(i INTEGER)").Drain();
	conn.Execute("INSERT INTO tn VALUES (1), (NULL), (3)").Drain();
}

} // namespace

TEST_CASE("Stable C++API: scalar stats callback narrows the result", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	CreateTables(conn);

	StatsMode no_null;
	no_null.set_null = true;
	no_null.can_have_null = false;
	no_null.write_nulls = true;
	RegisterScalar(conn, "cpp_stats_no_null", no_null);

	StatsMode all_null;
	all_null.set_valid = true;
	all_null.can_have_valid = false;
	RegisterScalar(conn, "cpp_stats_all_null", all_null);

	StatsMode untouched;
	untouched.write_nulls = true;
	RegisterScalar(conn, "cpp_stats_untouched", untouched);

	stats_probe = {};
	REQUIRE(CountOf(conn, "SELECT count(*) FROM t WHERE cpp_stats_no_null(i) IS NULL") == 0);
	REQUIRE(stats_probe.calls > 0);
	REQUIRE(stats_probe.arg_count == 1);
	REQUIRE(!stats_probe.arg_can_have_null);
	REQUIRE(stats_probe.arg_can_have_valid);
	REQUIRE(stats_probe.arg_write_refused);
	REQUIRE(!stats_probe.result_can_have_null);

	stats_probe = {};
	REQUIRE(CountOf(conn, "SELECT count(*) FROM t WHERE cpp_stats_all_null(i) IS NOT NULL") == 0);
	REQUIRE(!stats_probe.result_can_have_valid);

	stats_probe = {};
	REQUIRE(CountOf(conn, "SELECT count(*) FROM tn WHERE cpp_stats_untouched(i) IS NULL") == 3);
	REQUIRE(stats_probe.calls > 0);
	REQUIRE(stats_probe.arg_can_have_null);
	REQUIRE(stats_probe.result_can_have_null);
	REQUIRE(stats_probe.result_can_have_valid);
}

TEST_CASE("Stable C++API: aggregate stats callback narrows the result", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	CreateTables(conn);

	StatsMode all_null;
	all_null.set_valid = true;
	all_null.can_have_valid = false;
	RegisterAggregate(conn, "cpp_stats_agg_all_null", all_null);
	RegisterAggregate(conn, "cpp_stats_agg_untouched", StatsMode {});

	stats_probe = {};
	REQUIRE(CountOf(conn, "SELECT count(*) FROM (SELECT cpp_stats_agg_all_null(i) AS a FROM t) WHERE a IS NOT NULL") ==
	        0);
	REQUIRE(stats_probe.calls > 0);
	REQUIRE(!stats_probe.result_can_have_valid);

	stats_probe = {};
	REQUIRE(CountOf(conn,
	                "SELECT count(*) FROM (SELECT cpp_stats_agg_untouched(i) AS a FROM tn) WHERE a IS NOT NULL") == 1);
	REQUIRE(stats_probe.arg_count == 1);
	REQUIRE(stats_probe.arg_can_have_null);
	REQUIRE(stats_probe.arg_write_refused);
}

TEST_CASE("Stable C++API: table stats callback narrows a column", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	StatsMode all_null;
	all_null.set_valid = true;
	all_null.can_have_valid = false;
	RegisterPair(conn, "cpp_stats_pair", all_null);
	RegisterPair(conn, "cpp_stats_pair_untouched", StatsMode {});

	stats_probe = {};
	REQUIRE(CountOf(conn, "SELECT count(*) FROM cpp_stats_pair() WHERE b IS NOT NULL") == 0);
	REQUIRE(stats_probe.calls > 0);
	REQUIRE(stats_probe.column_index == 1);
	REQUIRE(!stats_probe.result_can_have_valid);

	// Column "a" is described as unknown, so its filter is evaluated
	REQUIRE(CountOf(conn, "SELECT count(*) FROM cpp_stats_pair() WHERE a IS NOT NULL") == 3);
	REQUIRE(CountOf(conn, "SELECT count(*) FROM cpp_stats_pair_untouched() WHERE b IS NOT NULL") == 3);
}
