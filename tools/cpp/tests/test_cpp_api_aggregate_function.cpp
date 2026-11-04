#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <atomic>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: AggregateFunction. The extension-registration path is
// covered by the static extension demo (test/api/capi/v2/static_extension);
// these cover the connection path.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

// Collect a single INTEGER column, asserting every row valid.
std::vector<int32_t> CollectInts(QueryResult result) {
	std::vector<int32_t> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			REQUIRE(view.IsValid(i));
			out.push_back(view.Data<int32_t>()[view.SelAt(i)]);
		}
	}
	return out;
}

// Collect a single BIGINT column, asserting every row valid.
std::vector<int64_t> CollectBigInts(QueryResult result) {
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

// ---------------------------------------------------------------------------
// cpp_sum(INTEGER) -> BIGINT: an int64 running sum kept in each state, plus a
// counter proving the destroy callback ran.
// ---------------------------------------------------------------------------

std::atomic<int> destroy_runs {0};

void SumSize(AggregateFunction::SizeInput &input) {
	input.SetStateSize(sizeof(int64_t));
}

void SumInit(AggregateFunction::InitInput &input) {
	auto states = input.GetStates();
	for (idx_t i = 0; i < input.GetStateCount(); i++) {
		*static_cast<int64_t *>(states[i]) = 0;
	}
}

void SumUpdate(AggregateFunction::UpdateInput &input) {
	const auto a = input.GetArg(0).GetView();
	auto states = input.GetStates();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		*static_cast<int64_t *>(states[i]) += a.Data<int32_t>()[a.SelAt(i)];
	}
}

void SumCombine(AggregateFunction::CombineInput &input) {
	auto sources = input.GetSources();
	auto targets = input.GetTargets();
	for (idx_t i = 0; i < input.GetStateCount(); i++) {
		*static_cast<int64_t *>(targets[i]) += *static_cast<const int64_t *>(sources[i]);
	}
}

void SumFinalize(AggregateFunction::FinalizeInput &input) {
	auto states = input.GetStates();
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int64_t>();
	const auto offset = input.GetResultOffset();
	for (idx_t i = 0; i < input.GetStateCount(); i++) {
		out[offset + i] = *static_cast<const int64_t *>(states[i]);
	}
}

void SumDestroy(AggregateFunction::DestroyInput &input) {
	destroy_runs += static_cast<int>(input.GetStateCount());
}

// ---------------------------------------------------------------------------
// The data slots, one struct per slot, plus counters proving each callback
// ran. Reset by the test that uses them. cpp_flow_sum(a) computes
// sum(a * factor) + offset, the factor from user data and the offset from
// bind data.
// ---------------------------------------------------------------------------

struct Factor {
	int value;
};
struct Offset {
	int value;
	bool operator==(const Offset &other) const {
		return value == other.value;
	}
};

std::atomic<int> bind_runs {0};
std::atomic<int> size_runs {0};
std::atomic<int> init_runs {0};
std::atomic<int> update_runs {0};
std::atomic<int> combine_runs {0};
std::atomic<int> finalize_runs {0};

void FlowBind(AggregateFunction::BindInput &input) {
	bind_runs++;
	const auto &factor = input.GetUserData<Factor>();
	input.SetBindData<Offset>(Offset {factor.value + 7});
}

void FlowSize(AggregateFunction::SizeInput &input) {
	size_runs++;
	// User data and bind data are both readable here.
	(void)input.GetUserData<Factor>();
	(void)input.GetBindData<Offset>();
	input.SetStateSize(sizeof(int64_t));
}

void FlowInit(AggregateFunction::InitInput &input) {
	init_runs++;
	(void)input.GetUserData<Factor>();
	(void)input.GetBindData<Offset>();
	auto states = input.GetStates();
	for (idx_t i = 0; i < input.GetStateCount(); i++) {
		*static_cast<int64_t *>(states[i]) = 0;
	}
}

void FlowUpdate(AggregateFunction::UpdateInput &input) {
	update_runs++;
	const auto factor = input.GetUserData<Factor>().value;
	const auto a = input.GetArg(0).GetView();
	auto states = input.GetStates();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		*static_cast<int64_t *>(states[i]) += a.Data<int32_t>()[a.SelAt(i)] * factor;
	}
}

void FlowCombine(AggregateFunction::CombineInput &input) {
	combine_runs++;
	(void)input.GetBindData<Offset>();
	auto sources = input.GetSources();
	auto targets = input.GetTargets();
	for (idx_t i = 0; i < input.GetStateCount(); i++) {
		*static_cast<int64_t *>(targets[i]) += *static_cast<const int64_t *>(sources[i]);
	}
}

void FlowFinalize(AggregateFunction::FinalizeInput &input) {
	finalize_runs++;
	const auto offset_value = input.GetBindData<Offset>().value;
	auto states = input.GetStates();
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int64_t>();
	const auto offset = input.GetResultOffset();
	for (idx_t i = 0; i < input.GetStateCount(); i++) {
		out[offset + i] = *static_cast<const int64_t *>(states[i]) + offset_value;
	}
}

// Resolves an ANY return type to INTEGER through the binding context. The
// aggregate keeps the last int32 it saw and finalizes to double that value.
void AnyReturnBind(AggregateFunction::BindInput &input) {
	input.SetReturnType(input.GetContext().GetFactory().ParseType("INTEGER"));
}

void LastSize(AggregateFunction::SizeInput &input) {
	input.SetStateSize(sizeof(int32_t));
}

void LastInit(AggregateFunction::InitInput &input) {
	auto states = input.GetStates();
	for (idx_t i = 0; i < input.GetStateCount(); i++) {
		*static_cast<int32_t *>(states[i]) = 0;
	}
}

void LastUpdate(AggregateFunction::UpdateInput &input) {
	const auto a = input.GetArg(0).GetView();
	auto states = input.GetStates();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		*static_cast<int32_t *>(states[i]) = a.Data<int32_t>()[a.SelAt(i)];
	}
}

void LastCombine(AggregateFunction::CombineInput &input) {
	auto sources = input.GetSources();
	auto targets = input.GetTargets();
	for (idx_t i = 0; i < input.GetStateCount(); i++) {
		const auto source = *static_cast<const int32_t *>(sources[i]);
		if (source != 0) {
			*static_cast<int32_t *>(targets[i]) = source;
		}
	}
}

void DoubleLastFinalize(AggregateFunction::FinalizeInput &input) {
	auto states = input.GetStates();
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	const auto offset = input.GetResultOffset();
	for (idx_t i = 0; i < input.GetStateCount(); i++) {
		out[offset + i] = *static_cast<const int32_t *>(states[i]) * 2;
	}
}

// Fails the query by throwing; the exception class survives the C boundary.
void FailingUpdate(AggregateFunction::UpdateInput &) {
	throw InvalidInputException("aggregate update failed on purpose");
}

// Reads a slot nothing planted: the guard throws rather than derefing null.
void NoUserDataUpdate(AggregateFunction::UpdateInput &input) {
	(void)input.GetUserData<int>();
}

// Bind-time argument introspection, latched for the test to assert after the
// query. The states go unused: the aggregate finalizes to the folded constant.
struct AggArgProbe {
	idx_t count = 0;
	std::string types;
	int32_t constant = 0;
	bool tried_non_constant = false;
	bool tried_out_of_range = false;
};
AggArgProbe agg_arg_probe;

void ArgProbeBind(AggregateFunction::BindInput &input) {
	agg_arg_probe.count = input.GetArgCount();
	// Bind may run more than once for a call, so latch rather than accumulate.
	agg_arg_probe.types.clear();
	for (idx_t i = 0; i < input.GetArgCount(); i++) {
		agg_arg_probe.types += (i ? "," : "") + input.GetArgType(i).ToText();
	}
	agg_arg_probe.constant = input.GetConstantArgument(1).Get<int32_t>();
	// The first argument is whatever the caller passed, so it may well not be constant.
	agg_arg_probe.tried_non_constant = input.TryGetConstantArgument(0).has_value();
	agg_arg_probe.tried_out_of_range = input.TryGetConstantArgument(input.GetArgCount()).has_value();
	input.SetReturnType(input.GetArgType(1));
}

void ArgProbeUpdate(AggregateFunction::UpdateInput &) {
}

void ArgProbeCombine(AggregateFunction::CombineInput &) {
}

void ArgProbeFinalize(AggregateFunction::FinalizeInput &input) {
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	const auto offset = input.GetResultOffset();
	for (idx_t i = 0; i < input.GetStateCount(); i++) {
		out[offset + i] = agg_arg_probe.constant;
	}
}

} // namespace

TEST_CASE("Stable C++API: aggregate function registers and executes", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	destroy_runs = 0;

	auto function = AggregateFunction::Create(conn.GetFactory());
	function.SetName("cpp_sum")
	    .WithSignature([&](FunctionSignature &sig) {
		    sig.AddParameter("a", factory.ParseType("INTEGER")).SetReturnType(factory.ParseType("BIGINT"));
	    })
	    .SetSizeCallback(SumSize)
	    .SetInitCallback(SumInit)
	    .SetUpdateCallback(SumUpdate)
	    .SetCombineCallback(SumCombine)
	    .SetFinalizeCallback(SumFinalize)
	    .SetDestroyCallback(SumDestroy);
	conn.Register(function);

	// Ungrouped, more rows than one chunk, so the callbacks see full vectors.
	REQUIRE(CollectBigInts(conn.Execute("SELECT cpp_sum(r::INTEGER) FROM range(5000) t(r)")) ==
	        std::vector<int64_t> {5000LL * 4999 / 2});
	// A constant argument vector.
	REQUIRE(CollectBigInts(conn.Execute("SELECT cpp_sum(5) FROM range(10)")) == std::vector<int64_t> {50});
	// Grouped: every row lands in its group's state; result offsets are
	// exercised by finalizing many groups.
	REQUIRE(CollectBigInts(conn.Execute("SELECT sum(s)::BIGINT FROM (SELECT cpp_sum(r::INTEGER) AS s "
	                                    "FROM range(5000) t(r) GROUP BY r % 3000)")) ==
	        std::vector<int64_t> {5000LL * 4999 / 2});
	// The destroy callback ran on the finished states.
	REQUIRE(destroy_runs >= 1);
}

TEST_CASE("Stable C++API: aggregate function data flows user->bind->callbacks", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	bind_runs = 0;
	size_runs = 0;
	init_runs = 0;
	update_runs = 0;
	combine_runs = 0;
	finalize_runs = 0;

	auto function = AggregateFunction::Create(conn.GetFactory());
	function.SetName("cpp_flow_sum").SetUserData<Factor>(Factor {3});
	function.GetSignature().AddParameter("a", factory.ParseType("INTEGER")).SetReturnType(factory.ParseType("BIGINT"));
	function.SetBindCallback(FlowBind)
	    .SetSizeCallback(FlowSize)
	    .SetInitCallback(FlowInit)
	    .SetUpdateCallback(FlowUpdate)
	    .SetCombineCallback(FlowCombine)
	    .SetFinalizeCallback(FlowFinalize);
	conn.Register(function);

	// 5 * 3 + (3 + 7) = 25
	REQUIRE(CollectBigInts(conn.Execute("SELECT cpp_flow_sum(5)")) == std::vector<int64_t> {25});
	// How often a query binds is not fixed: debug builds re-bind while verifying the plan.
	const auto binds_after_first = bind_runs.load();
	REQUIRE(binds_after_first >= 1);
	REQUIRE(size_runs >= 1);
	REQUIRE(init_runs >= 1);
	REQUIRE(update_runs >= 1);
	REQUIRE(finalize_runs >= 1);

	// A second query binds afresh; the user data planted at registration is still there.
	// (0 + 1 + 2) * 3 + 10 = 19
	REQUIRE(CollectBigInts(conn.Execute("SELECT cpp_flow_sum(r::INTEGER) FROM range(3) t(r)")) ==
	        std::vector<int64_t> {19});
	REQUIRE(bind_runs > binds_after_first);
}

TEST_CASE("Stable C++API: aggregate function bind resolves an ANY return type", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto function = AggregateFunction::Create(conn.GetFactory());
	function.SetName("cpp_double_last")
	    .SetBindCallback(AnyReturnBind)
	    .SetSizeCallback(LastSize)
	    .SetInitCallback(LastInit)
	    .SetUpdateCallback(LastUpdate)
	    .SetCombineCallback(LastCombine)
	    .SetFinalizeCallback(DoubleLastFinalize);
	function.GetSignature()
	    .AddParameter("a", factory.ParseType("INTEGER"))
	    .SetReturnType(factory.CreateType(LogicalTypeId::ANY));
	conn.Register(function);

	auto result = conn.Execute("SELECT cpp_double_last(21)");
	REQUIRE(result.GetSchema().GetFieldType(0).ToText() == "INTEGER");
	REQUIRE(CollectInts(std::move(result)) == std::vector<int32_t> {42});
}

TEST_CASE("Stable C++API: aggregate function bind reads argument types and constants", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto function = AggregateFunction::Create(conn.GetFactory());
	function.SetName("cpp_agg_arg_probe")
	    .SetBindCallback(ArgProbeBind)
	    .SetSizeCallback(LastSize)
	    .SetInitCallback(LastInit)
	    .SetUpdateCallback(ArgProbeUpdate)
	    .SetCombineCallback(ArgProbeCombine)
	    .SetFinalizeCallback(ArgProbeFinalize);
	function.GetSignature()
	    .AddParameter("a", factory.CreateType(LogicalTypeId::ANY))
	    .AddParameter("b", factory.ParseType("INTEGER"))
	    .SetReturnType(factory.CreateType(LogicalTypeId::ANY));
	conn.Register(function);

	agg_arg_probe = {};
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_agg_arg_probe('hello', 21)")) == std::vector<int32_t> {21});
	REQUIRE(agg_arg_probe.count == 2);
	// The ANY parameter reports the type the call resolved it to.
	REQUIRE(agg_arg_probe.types == "VARCHAR,INTEGER");
	REQUIRE(agg_arg_probe.constant == 21);
	REQUIRE(agg_arg_probe.tried_non_constant);
	// An index past the last argument is absence, not a failure.
	REQUIRE_FALSE(agg_arg_probe.tried_out_of_range);

	// A column reference has no constant value: TryGetConstantArgument reports absence...
	agg_arg_probe = {};
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_agg_arg_probe(a, 21) FROM (VALUES ('x')) t(a)")) ==
	        std::vector<int32_t> {21});
	REQUIRE_FALSE(agg_arg_probe.tried_non_constant);

	// ...while GetConstantArgument fails the query with the binder's own error.
	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT cpp_agg_arg_probe('hello', b) FROM (VALUES (21)) t(b)").Drain(),
	                       Exception, HasErrorCode(DUCKDB_V2_ERROR_QUERY_BINDER));
}

TEST_CASE("Stable C++API: aggregate function callback errors fail the query", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	auto failing = AggregateFunction::Create(conn.GetFactory());
	failing.SetName("cpp_agg_fail")
	    .SetSizeCallback(LastSize)
	    .SetInitCallback(LastInit)
	    .SetUpdateCallback(FailingUpdate)
	    .SetCombineCallback(LastCombine)
	    .SetFinalizeCallback(DoubleLastFinalize);
	failing.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(failing);

	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT cpp_agg_fail(1)").Drain(), InvalidInputException,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	// GetUserData when nothing was planted reports the misuse rather than derefing null.
	auto no_data = AggregateFunction::Create(conn.GetFactory());
	no_data.SetName("cpp_agg_no_user_data")
	    .SetSizeCallback(LastSize)
	    .SetInitCallback(LastInit)
	    .SetUpdateCallback(NoUserDataUpdate)
	    .SetCombineCallback(LastCombine)
	    .SetFinalizeCallback(DoubleLastFinalize);
	no_data.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(no_data);

	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT cpp_agg_no_user_data(1)").Drain(), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: aggregate function registration validation", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	// No name.
	{
		auto function = AggregateFunction::Create(conn.GetFactory());
		function.SetSizeCallback(SumSize)
		    .SetInitCallback(SumInit)
		    .SetUpdateCallback(SumUpdate)
		    .SetCombineCallback(SumCombine)
		    .SetFinalizeCallback(SumFinalize);
		function.GetSignature().AddParameter("a", integer).SetReturnType(integer);
		REQUIRE_THROWS_MATCHES(conn.Register(function), InvalidInputException,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}

	// A required callback missing.
	{
		auto function = AggregateFunction::Create(conn.GetFactory());
		function.SetName("cpp_no_update")
		    .SetSizeCallback(SumSize)
		    .SetInitCallback(SumInit)
		    .SetCombineCallback(SumCombine)
		    .SetFinalizeCallback(SumFinalize);
		function.GetSignature().AddParameter("a", integer).SetReturnType(integer);
		REQUIRE_THROWS_MATCHES(conn.Register(function), InvalidInputException,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}

	// An ANY return type without a bind callback to resolve it.
	{
		auto function = AggregateFunction::Create(conn.GetFactory());
		function.SetName("cpp_agg_unresolved_any")
		    .SetSizeCallback(SumSize)
		    .SetInitCallback(SumInit)
		    .SetUpdateCallback(SumUpdate)
		    .SetCombineCallback(SumCombine)
		    .SetFinalizeCallback(SumFinalize);
		function.GetSignature().AddParameter("a", integer).SetReturnType(factory.CreateType(LogicalTypeId::ANY));
		REQUIRE_THROWS_MATCHES(conn.Register(function), InvalidInputException,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
}

// ---------------------------------------------------------------------------
// Function properties.
// ---------------------------------------------------------------------------

TEST_CASE("Stable C++API: aggregate function properties", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	// Setting common and aggregate-specific properties leaves the function
	// registrable and correct.
	auto function = AggregateFunction::Create(conn.GetFactory());
	function.SetName("prop_sum")
	    .WithSignature([&](FunctionSignature &sig) {
		    sig.AddParameter("a", factory.ParseType("INTEGER")).SetReturnType(factory.ParseType("BIGINT"));
	    })
	    .SetSizeCallback(SumSize)
	    .SetInitCallback(SumInit)
	    .SetUpdateCallback(SumUpdate)
	    .SetCombineCallback(SumCombine)
	    .SetFinalizeCallback(SumFinalize)
	    .SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY)
	    .SetCollationHandling(FunctionCollationHandling::IGNORE)
	    .SetOrderDependence(AggregateFunction::OrderDependence::INDEPENDENT)
	    .SetDistinctDependence(AggregateFunction::DistinctDependence::INDEPENDENT);
	conn.Register(function);

	REQUIRE(CollectBigInts(conn.Execute("SELECT prop_sum(r::INTEGER) FROM range(1000) t(r)")) ==
	        std::vector<int64_t> {1000LL * 999 / 2});
}

TEST_CASE("Stable C++API: aggregate function description and examples", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto function = AggregateFunction::Create(factory);
	function.SetName("cpp_documented_sum")
	    .WithDocs([](FunctionDocs &docs) {
		    docs.SetDescription("Sums a.").AddExample("Sum a column", "cpp_documented_sum(x)");
	    })
	    .WithSignature([&](FunctionSignature &sig) {
		    sig.AddParameter("a", factory.ParseType("INTEGER")).SetReturnType(factory.ParseType("BIGINT"));
	    })
	    .SetSizeCallback(SumSize)
	    .SetInitCallback(SumInit)
	    .SetUpdateCallback(SumUpdate)
	    .SetCombineCallback(SumCombine)
	    .SetFinalizeCallback(SumFinalize)
	    .SetDestroyCallback(SumDestroy);
	conn.Register(function);

	auto result = conn.Execute("SELECT description, array_to_string(examples, '|') FROM duckdb_functions() "
	                           "WHERE function_name = 'cpp_documented_sum'");
	auto chunk = result.FetchChunk();
	REQUIRE(chunk.GetRowCount() == 1);
	REQUIRE(chunk.GetVector(0).GetValue(0).ToText() == "Sums a.");
	REQUIRE(chunk.GetVector(1).GetValue(0).ToText() == "cpp_documented_sum(x)");
}
