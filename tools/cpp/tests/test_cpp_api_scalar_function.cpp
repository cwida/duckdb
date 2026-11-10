#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <atomic>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: ScalarFunction and FunctionSignature. The
// extension-registration path is covered by the static extension demo
// (test/api/capi/v2/static_extension); these cover the connection path.
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

// out[i] = a[i] + 1
void PlusOneExec(ScalarFunction::ExecInput &input) {
	const auto a = input.GetArg(0).GetView();
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = a.Data<int32_t>()[a.SelAt(i)] + 1;
	}
}

// The three data slots, one struct per slot, plus counters proving each
// callback ran. Reset by the test that uses them.
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
std::atomic<int> init_runs {0};
std::atomic<int> exec_runs {0};

void FlowBind(ScalarFunction::BindInput &input) {
	bind_runs++;
	const auto &factor = input.GetUserData<Factor>();
	input.SetBindData<Offset>(Offset {factor.value + 7});
}

void FlowInit(ScalarFunction::InitInput &input) {
	init_runs++;
	// User data and bind data are both readable here.
	(void)input.GetUserData<Factor>();
	input.SetInitData<int>(input.GetBindData<Offset>().value);
}

// out[i] = a[i] * factor + offset
void FlowExec(ScalarFunction::ExecInput &input) {
	exec_runs++;
	const auto factor = input.GetUserData<Factor>().value;
	REQUIRE(input.GetBindData<Offset>().value == input.GetInitData<int>());
	const auto offset = input.GetInitData<int>();
	const auto a = input.GetArg(0).GetView();
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = a.Data<int32_t>()[a.SelAt(i)] * factor + offset;
	}
}

// out[i] = sum over every argument: the fixed parameter plus the variadic tail.
void VsumExec(ScalarFunction::ExecInput &input) {
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	const auto count = input.GetRowCount();
	const auto arg_count = input.GetArgCount();
	for (idx_t col = 0; col < arg_count; col++) {
		const auto view = input.GetArg(col).GetView();
		for (idx_t i = 0; i < count; i++) {
			const auto value = view.Data<int32_t>()[view.SelAt(i)];
			out[i] = col == 0 ? value : out[i] + value;
		}
	}
}

// Bind data without operator==: the wrapper falls back to comparing slots by
// identity instead of requiring one.
struct OpaqueOffset {
	int value;
};

void OpaqueBind(ScalarFunction::BindInput &input) {
	input.SetBindData<OpaqueOffset>(OpaqueOffset {100});
}

// out[i] = a[i] + opaque offset
void OpaqueExec(ScalarFunction::ExecInput &input) {
	const auto offset = input.GetBindData<OpaqueOffset>().value;
	const auto a = input.GetArg(0).GetView();
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = a.Data<int32_t>()[a.SelAt(i)] + offset;
	}
}

// Resolves an ANY return type to INTEGER through the binding context.
void AnyReturnResolveTypes(ScalarFunction::ResolveTypesInput &input) {
	input.SetReturnType(input.GetContext().GetFactory().ParseType("INTEGER"));
}

// out[i] = a[i] * 2
void DoubleExec(ScalarFunction::ExecInput &input) {
	const auto a = input.GetArg(0).GetView();
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = a.Data<int32_t>()[a.SelAt(i)] * 2;
	}
}

// Fails the query by throwing; the exception class survives the C boundary.
void FailingExec(ScalarFunction::ExecInput &) {
	throw InvalidInputException("scalar exec failed on purpose");
}

// Reads a slot nothing planted: the guard throws rather than derefing null.
void NoUserDataExec(ScalarFunction::ExecInput &input) {
	(void)input.GetUserData<int>();
}

// Bind-time argument introspection, latched for the test to assert after the query.
struct ArgProbe {
	idx_t count = 0;
	std::string types;
	int32_t constant = 0;
	bool tried_non_constant = false;
	bool tried_out_of_range = false;
};
ArgProbe arg_probe;

void ArgProbeBind(ScalarFunction::BindInput &input) {
	arg_probe.count = input.GetArgCount();
	// Bind may run more than once for a call, so latch rather than accumulate.
	arg_probe.types.clear();
	for (idx_t i = 0; i < input.GetArgCount(); i++) {
		arg_probe.types += (i ? "," : "") + input.GetArgType(i).ToText();
	}
	arg_probe.constant = input.GetConstantArgument(1).Get<int32_t>();
	// The first argument is whatever the caller passed, so it may well not be constant.
	arg_probe.tried_non_constant = input.TryGetConstantArgument(0).has_value();
	arg_probe.tried_out_of_range = input.TryGetConstantArgument(input.GetArgCount()).has_value();
}

// The return type follows the type of the second argument.
void ArgProbeResolveTypes(ScalarFunction::ResolveTypesInput &input) {
	input.SetReturnType(input.GetArgType(1));
}

// out[i] = the constant the bind callback folded out of the second argument.
void ArgProbeExec(ScalarFunction::ExecInput &input) {
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = arg_probe.constant;
	}
}

} // namespace

TEST_CASE("Stable C++API: scalar function registers and executes", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	auto function = ScalarFunction::Create(conn.GetFactory());
	function.SetName("plus_one").SetExecCallback(PlusOneExec);
	function.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(function);

	REQUIRE(CollectInts(conn.Execute("SELECT plus_one(41)")) == std::vector<int32_t> {42});
	REQUIRE(CollectInts(conn.Execute("SELECT plus_one(r::INTEGER) FROM range(4) t(r)")) ==
	        std::vector<int32_t> {1, 2, 3, 4});
}

TEST_CASE("Stable C++API: scalar function data flows user->bind->init->exec", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	bind_runs = 0;
	init_runs = 0;
	exec_runs = 0;

	auto function = ScalarFunction::Create(conn.GetFactory());
	function.SetName("cpp_flow").SetUserData<Factor>(Factor {3});
	function.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	function.SetBindCallback(FlowBind).SetInitCallback(FlowInit).SetExecCallback(FlowExec);
	conn.Register(function);

	// 5 * 3 + (3 + 7) = 25
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_flow(5)")) == std::vector<int32_t> {25});
	// How often a query binds is not fixed: debug builds re-bind while verifying the plan.
	const auto binds_after_first = bind_runs.load();
	REQUIRE(binds_after_first >= 1);
	REQUIRE(init_runs >= 1);
	REQUIRE(exec_runs >= 1);

	// A second query binds afresh; the user data planted at registration is still there.
	auto rows = CollectInts(conn.Execute("SELECT cpp_flow(r::INTEGER) FROM range(3) t(r)"));
	REQUIRE(rows == std::vector<int32_t> {10, 13, 16});
	REQUIRE(bind_runs > binds_after_first);
}

TEST_CASE("Stable C++API: scalar function bind data without operator== compares by identity", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	auto function = ScalarFunction::Create(conn.GetFactory());
	function.SetName("cpp_opaque").SetBindCallback(OpaqueBind).SetExecCallback(OpaqueExec);
	function.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(function);

	REQUIRE(CollectInts(conn.Execute("SELECT cpp_opaque(1)")) == std::vector<int32_t> {101});
	// Two calls in one query mean two bind-data instances that the engine may compare.
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_opaque(r::INTEGER) + cpp_opaque(0) FROM range(3) t(r)")) ==
	        std::vector<int32_t> {200, 201, 202});
}

TEST_CASE("Stable C++API: scalar function variadic tail via GetArgCount", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	auto function = ScalarFunction::Create(conn.GetFactory());
	function.SetName("cpp_vsum").SetExecCallback(VsumExec);
	function.GetSignature().AddParameter("a", integer).AddArgs("rest", integer).SetReturnType(integer);
	conn.Register(function);

	REQUIRE(CollectInts(conn.Execute("SELECT cpp_vsum(1)")) == std::vector<int32_t> {1});
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_vsum(1, 2, 3, 4)")) == std::vector<int32_t> {10});
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_vsum(r::INTEGER, 10, 100) FROM range(2) t(r)")) ==
	        std::vector<int32_t> {110, 111});
}

TEST_CASE("Stable C++API: scalar function resolve types resolves an ANY return type", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto function = ScalarFunction::Create(conn.GetFactory());
	function.SetName("cpp_double").SetResolveTypesCallback(AnyReturnResolveTypes).SetExecCallback(DoubleExec);
	function.GetSignature()
	    .AddParameter("a", factory.ParseType("INTEGER"))
	    .SetReturnType(factory.CreateType(LogicalTypeId::ANY));
	conn.Register(function);

	auto result = conn.Execute("SELECT cpp_double(21)");
	REQUIRE(result.GetSchema().GetFieldType(0).ToText() == "INTEGER");
	REQUIRE(CollectInts(std::move(result)) == std::vector<int32_t> {42});
}

TEST_CASE("Stable C++API: scalar function bind reads argument types and constants", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto function = ScalarFunction::Create(conn.GetFactory());
	function.SetName("cpp_arg_probe")
	    .SetResolveTypesCallback(ArgProbeResolveTypes)
	    .SetBindCallback(ArgProbeBind)
	    .SetExecCallback(ArgProbeExec);
	function.GetSignature()
	    .AddParameter("a", factory.CreateType(LogicalTypeId::ANY))
	    .AddParameter("b", factory.ParseType("INTEGER"))
	    .SetReturnType(factory.CreateType(LogicalTypeId::ANY));
	conn.Register(function);

	arg_probe = {};
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_arg_probe('hello', 21)")) == std::vector<int32_t> {21});
	REQUIRE(arg_probe.count == 2);
	// The ANY parameter reports the type the call resolved it to.
	REQUIRE(arg_probe.types == "VARCHAR,INTEGER");
	REQUIRE(arg_probe.constant == 21);
	REQUIRE(arg_probe.tried_non_constant);
	// An index past the last argument is absence, not a failure.
	REQUIRE_FALSE(arg_probe.tried_out_of_range);

	// A column reference has no constant value: TryGetConstantArgument reports absence...
	arg_probe = {};
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_arg_probe(a, 21) FROM (VALUES ('x')) t(a)")) ==
	        std::vector<int32_t> {21});
	REQUIRE_FALSE(arg_probe.tried_non_constant);

	// ...while GetConstantArgument fails the query with the binder's own error.
	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT cpp_arg_probe('hello', b) FROM (VALUES (21)) t(b)").Drain(), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_QUERY_BINDER));
}

TEST_CASE("Stable C++API: scalar function callback errors fail the query", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	auto failing = ScalarFunction::Create(conn.GetFactory());
	failing.SetName("cpp_fail").SetExecCallback(FailingExec);
	failing.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(failing);

	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT cpp_fail(1)").Drain(), InvalidInputException,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	// GetUserData when nothing was planted reports the misuse rather than derefing null.
	auto no_data = ScalarFunction::Create(conn.GetFactory());
	no_data.SetName("cpp_no_user_data").SetExecCallback(NoUserDataExec);
	no_data.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(no_data);

	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT cpp_no_user_data(1)").Drain(), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: scalar function registration validation", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	// No name.
	{
		auto function = ScalarFunction::Create(conn.GetFactory());
		function.SetExecCallback(PlusOneExec);
		function.GetSignature().AddParameter("a", integer).SetReturnType(integer);
		REQUIRE_THROWS_MATCHES(conn.Register(function), InvalidInputException,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}

	// No exec callback.
	{
		auto function = ScalarFunction::Create(conn.GetFactory());
		function.SetName("cpp_no_exec");
		function.GetSignature().AddParameter("a", integer).SetReturnType(integer);
		REQUIRE_THROWS_MATCHES(conn.Register(function), InvalidInputException,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}

	// An ANY return type without a resolve types callback to resolve it.
	{
		auto function = ScalarFunction::Create(conn.GetFactory());
		function.SetName("cpp_unresolved_any").SetExecCallback(PlusOneExec);
		function.GetSignature().AddParameter("a", integer).SetReturnType(factory.CreateType(LogicalTypeId::ANY));
		REQUIRE_THROWS_MATCHES(conn.Register(function), InvalidInputException,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
}

// ---------------------------------------------------------------------------
// ScalarExecutor: the row-at-a-time helper over primitive types.
// ---------------------------------------------------------------------------

namespace {

// Collect a single INTEGER column, keeping NULLs as (false, 0).
std::vector<std::pair<bool, int32_t>> CollectNullableInts(QueryResult result) {
	std::vector<std::pair<bool, int32_t>> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			const auto valid = view.IsValid(i);
			out.emplace_back(valid, valid ? view.Data<int32_t>()[view.SelAt(i)] : 0);
		}
	}
	return out;
}

void ExecutorAddExec(ScalarFunction::ExecInput &input) {
	ScalarExecutor::Execute<int32_t, int32_t, int32_t>(input, [](int32_t a, int32_t b) { return a + b; });
}

void ExecutorSevenExec(ScalarFunction::ExecInput &input) {
	ScalarExecutor::Execute<int32_t>(input, []() { return 7; });
}

// Heterogeneous argument types: INTEGER + BIGINT -> BIGINT.
void ExecutorMixedExec(ScalarFunction::ExecInput &input) {
	ScalarExecutor::Execute<int64_t, int32_t, int64_t>(input, [](int32_t a, int64_t b) { return a * b; });
}

// The executor's type list disagrees with the registered signature's arity.
void ExecutorWrongArityExec(ScalarFunction::ExecInput &input) {
	ScalarExecutor::Execute<int32_t, int32_t, int32_t>(input, [](int32_t a, int32_t b) { return a + b; });
}

} // namespace

TEST_CASE("Stable C++API: ScalarExecutor computes rows and propagates NULLs", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	auto function = ScalarFunction::Create(conn.GetFactory());
	function.SetName("exec_add").SetExecCallback(ExecutorAddExec);
	function.GetSignature().AddParameter("a", integer).AddParameter("b", integer).SetReturnType(integer);
	conn.Register(function);

	// Constant, flat, and filtered (selection vector) inputs.
	REQUIRE(CollectInts(conn.Execute("SELECT exec_add(40, 2)")) == std::vector<int32_t> {42});
	REQUIRE(CollectInts(conn.Execute("SELECT exec_add(r::INTEGER, 10) FROM range(4) t(r)")) ==
	        std::vector<int32_t> {10, 11, 12, 13});
	REQUIRE(CollectInts(conn.Execute("SELECT exec_add(r::INTEGER, 0) FROM range(8) t(r) WHERE r % 2 = 0")) ==
	        std::vector<int32_t> {0, 2, 4, 6});

	// A NULL in either argument yields NULL for that row alone.
	auto rows = CollectNullableInts(conn.Execute("SELECT exec_add(x, 1) FROM (VALUES (1), (NULL), (3)) t(x)"));
	auto expected = std::vector<std::pair<bool, int32_t>> {{true, 2}, {false, 0}, {true, 4}};
	REQUIRE(rows == expected);
}

TEST_CASE("Stable C++API: ScalarExecutor handles nullary and mixed-type functions", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	auto seven = ScalarFunction::Create(conn.GetFactory());
	seven.SetName("exec_seven").SetExecCallback(ExecutorSevenExec);
	seven.GetSignature().SetReturnType(integer);
	conn.Register(seven);

	REQUIRE(CollectInts(conn.Execute("SELECT exec_seven()")) == std::vector<int32_t> {7});
	REQUIRE(CollectInts(conn.Execute("SELECT exec_seven() FROM range(3)")) == std::vector<int32_t> {7, 7, 7});

	auto mixed = ScalarFunction::Create(conn.GetFactory());
	mixed.SetName("exec_mixed").SetExecCallback(ExecutorMixedExec);
	mixed.GetSignature()
	    .AddParameter("a", integer)
	    .AddParameter("b", factory.ParseType("BIGINT"))
	    .SetReturnType(factory.ParseType("BIGINT"));
	conn.Register(mixed);

	REQUIRE(CollectInts(conn.Execute("SELECT exec_mixed(6, 7)::INTEGER")) == std::vector<int32_t> {42});
}

TEST_CASE("Stable C++API: ScalarExecutor refuses an arity mismatch", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	auto function = ScalarFunction::Create(conn.GetFactory());
	function.SetName("exec_wrong_arity").SetExecCallback(ExecutorWrongArityExec);
	function.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(function);

	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT exec_wrong_arity(1)").Drain(), InvalidInputException,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: ScalarExecutor drives vectors directly", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	// Two argument columns and a result column, no scalar function involved.
	std::vector<LogicalType> arg_types;
	arg_types.push_back(factory.ParseType("INTEGER"));
	arg_types.push_back(factory.ParseType("INTEGER"));
	DataChunk args(factory, arg_types);

	constexpr idx_t kCount = 4;
	auto a = args.GetVector(0);
	a.SetSize(kCount);
	auto b = args.GetVector(1);
	b.SetSize(kCount);
	auto *a_data = a.GetDataMutable<int32_t>();
	auto *b_data = b.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < kCount; i++) {
		a_data[i] = static_cast<int32_t>(i);
		b_data[i] = 10;
	}
	a.SetNull(2);

	std::vector<LogicalType> out_types;
	out_types.push_back(factory.ParseType("INTEGER"));
	DataChunk out(factory, out_types);
	auto result = out.GetVector(0);
	result.SetSize(kCount);

	ScalarExecutor::Execute<int32_t, int32_t, int32_t>(a, b, result, kCount,
	                                                   [](int32_t x, int32_t y) -> int32_t { return x + y; });

	auto view = result.GetView();
	REQUIRE(view.IsValid(0));
	REQUIRE(view.Data<int32_t>()[view.SelAt(0)] == 10);
	REQUIRE(view.Data<int32_t>()[view.SelAt(1)] == 11);
	REQUIRE(!view.IsValid(2)); // the NULL argument row propagated
	REQUIRE(view.IsValid(3));
	REQUIRE(view.Data<int32_t>()[view.SelAt(3)] == 13);
}

// ---------------------------------------------------------------------------
// ScalarExecutor composed type forms: std::optional, std::tuple, references.
// ---------------------------------------------------------------------------

namespace {

// optional result: NULL is produced by the callable itself.
void SafeDivExec(ScalarFunction::ExecInput &input) {
	ScalarExecutor::Execute<std::optional<int32_t>, int32_t, int32_t>(
	    input, [](int32_t a, int32_t b) -> std::optional<int32_t> {
		    if (b == 0) {
			    return std::nullopt;
		    }
		    return a / b;
	    });
}

// optional argument: the callable sees NULL rows as nullopt instead of the
// row being skipped. Counts them to prove it was invoked.
std::atomic<int> nullopt_seen {0};

void OptBumpExec(ScalarFunction::ExecInput &input) {
	ScalarExecutor::Execute<std::optional<int32_t>, std::optional<int32_t>>(
	    input, [](std::optional<int32_t> a) -> std::optional<int32_t> {
		    if (!a) {
			    nullopt_seen++;
			    return std::nullopt;
		    }
		    return *a + 1;
	    });
}

// tuple argument: STRUCT(x INTEGER, y INTEGER) -> x + y.
void PointSumExec(ScalarFunction::ExecInput &input) {
	ScalarExecutor::Execute<int32_t, std::tuple<int32_t, int32_t>>(
	    input, [](const std::tuple<int32_t, int32_t> &p) { return std::get<0>(p) + std::get<1>(p); });
}

// tuple argument with an optional field: a NULL y no longer nulls the row.
void PointSumOptYExec(ScalarFunction::ExecInput &input) {
	ScalarExecutor::Execute<int32_t, std::tuple<int32_t, std::optional<int32_t>>>(
	    input, [](const std::tuple<int32_t, std::optional<int32_t>> &p) {
		    return std::get<0>(p) + std::get<1>(p).value_or(0);
	    });
}

// tuple result: INTEGER -> STRUCT(x INTEGER, y INTEGER).
void MakePointExec(ScalarFunction::ExecInput &input) {
	ScalarExecutor::Execute<std::tuple<int32_t, int32_t>, int32_t>(input,
	                                                               [](int32_t a) { return std::make_tuple(a, a * 2); });
}

// reference argument: reads in place instead of copying.
void RefAddExec(ScalarFunction::ExecInput &input) {
	ScalarExecutor::Execute<int32_t, std::reference_wrapper<const int32_t>, std::reference_wrapper<const int32_t>>(
	    input, [](std::reference_wrapper<const int32_t> a, std::reference_wrapper<const int32_t> b) {
		    return a.get() + b.get();
	    });
}

} // namespace

TEST_CASE("Stable C++API: ScalarExecutor optional results and arguments", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	auto safe_div = ScalarFunction::Create(conn.GetFactory());
	safe_div.SetName("exec_safe_div").SetExecCallback(SafeDivExec);
	safe_div.GetSignature().AddParameter("a", integer).AddParameter("b", integer).SetReturnType(integer);
	conn.Register(safe_div);

	auto rows =
	    CollectNullableInts(conn.Execute("SELECT exec_safe_div(a, b) FROM (VALUES (10, 2), (1, 0), (9, 3)) t(a, b)"));
	auto expected = std::vector<std::pair<bool, int32_t>> {{true, 5}, {false, 0}, {true, 3}};
	REQUIRE(rows == expected);

	nullopt_seen = 0;
	auto opt_bump = ScalarFunction::Create(conn.GetFactory());
	opt_bump.SetName("exec_opt_bump").SetExecCallback(OptBumpExec);
	opt_bump.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(opt_bump);

	rows = CollectNullableInts(conn.Execute("SELECT exec_opt_bump(x) FROM (VALUES (1), (NULL), (3)) t(x)"));
	expected = std::vector<std::pair<bool, int32_t>> {{true, 2}, {false, 0}, {true, 4}};
	REQUIRE(rows == expected);
	// The callable ran for the NULL row rather than the row being skipped.
	REQUIRE(nullopt_seen == 1);
}

TEST_CASE("Stable C++API: ScalarExecutor tuple arguments read STRUCT fields", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");
	const auto point = factory.ParseType("STRUCT(x INTEGER, y INTEGER)");

	auto psum = ScalarFunction::Create(conn.GetFactory());
	psum.SetName("exec_psum").SetExecCallback(PointSumExec);
	psum.GetSignature().AddParameter("p", point).SetReturnType(integer);
	conn.Register(psum);

	// A NULL struct row and a NULL non-optional field both null the row.
	auto rows = CollectNullableInts(
	    conn.Execute("SELECT exec_psum(s::STRUCT(x INTEGER, y INTEGER)) FROM (VALUES ({'x': 1, 'y': 2}), (NULL), "
	                 "({'x': 4, 'y': NULL})) t(s)"));
	auto expected = std::vector<std::pair<bool, int32_t>> {{true, 3}, {false, 0}, {false, 0}};
	REQUIRE(rows == expected);

	// With the field declared optional, only the whole-struct NULL nulls the row.
	auto psum_opt = ScalarFunction::Create(conn.GetFactory());
	psum_opt.SetName("exec_psum_opt").SetExecCallback(PointSumOptYExec);
	psum_opt.GetSignature().AddParameter("p", point).SetReturnType(integer);
	conn.Register(psum_opt);

	rows = CollectNullableInts(
	    conn.Execute("SELECT exec_psum_opt(s::STRUCT(x INTEGER, y INTEGER)) FROM (VALUES ({'x': 1, 'y': 2}), (NULL), "
	                 "({'x': 4, 'y': NULL})) t(s)"));
	expected = std::vector<std::pair<bool, int32_t>> {{true, 3}, {false, 0}, {true, 4}};
	REQUIRE(rows == expected);
}

TEST_CASE("Stable C++API: ScalarExecutor tuple result writes STRUCT fields", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto make_point = ScalarFunction::Create(conn.GetFactory());
	make_point.SetName("exec_make_point").SetExecCallback(MakePointExec);
	make_point.GetSignature()
	    .AddParameter("a", factory.ParseType("INTEGER"))
	    .SetReturnType(factory.ParseType("STRUCT(x INTEGER, y INTEGER)"));
	conn.Register(make_point);

	auto rows = CollectNullableInts(conn.Execute("SELECT (exec_make_point(r::INTEGER)).x + (exec_make_point("
	                                             "r::INTEGER)).y FROM range(3) t(r)"));
	auto expected = std::vector<std::pair<bool, int32_t>> {{true, 0}, {true, 3}, {true, 6}};
	REQUIRE(rows == expected);

	// A NULL argument nulls the whole struct row, fields included.
	rows = CollectNullableInts(conn.Execute("SELECT (exec_make_point(x)).y FROM (VALUES (2), (NULL)) t(x)"));
	expected = std::vector<std::pair<bool, int32_t>> {{true, 4}, {false, 0}};
	REQUIRE(rows == expected);
}

TEST_CASE("Stable C++API: ScalarExecutor reference arguments", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	auto ref_add = ScalarFunction::Create(conn.GetFactory());
	ref_add.SetName("exec_ref_add").SetExecCallback(RefAddExec);
	ref_add.GetSignature().AddParameter("a", integer).AddParameter("b", integer).SetReturnType(integer);
	conn.Register(ref_add);

	REQUIRE(CollectInts(conn.Execute("SELECT exec_ref_add(r::INTEGER, 100) FROM range(3) t(r)")) ==
	        std::vector<int32_t> {100, 101, 102});
}

// ---------------------------------------------------------------------------
// Function properties.
// ---------------------------------------------------------------------------

namespace {

std::atomic<idx_t> prop_exec_rows {0};

// out[i] = 1; counts processed rows to observe constant folding.
void CountingOneExec(ScalarFunction::ExecInput &input) {
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = 1;
	}
	prop_exec_rows += input.GetRowCount();
}

} // namespace

TEST_CASE("Stable C++API: scalar function properties", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	// CONSISTENT (the default): a call with a constant argument is folded to a
	// single evaluation instead of running per row.
	auto consistent = ScalarFunction::Create(conn.GetFactory());
	consistent.SetName("prop_consistent").SetExecCallback(CountingOneExec);
	consistent.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(consistent);

	// The same function declared VOLATILE must be evaluated for every row.
	auto vol = ScalarFunction::Create(conn.GetFactory());
	vol.SetName("prop_volatile").SetExecCallback(CountingOneExec).SetStability(FunctionStability::VOLATILE);
	vol.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(vol);

	prop_exec_rows = 0;
	CollectInts(conn.Execute("SELECT prop_consistent(42) FROM range(1000)"));
	const auto consistent_rows = prop_exec_rows.load();

	prop_exec_rows = 0;
	CollectInts(conn.Execute("SELECT prop_volatile(42) FROM range(1000)"));
	const auto volatile_rows = prop_exec_rows.load();

	REQUIRE(volatile_rows >= 1000);
	REQUIRE(consistent_rows < volatile_rows);

	// The catalog reports CONSISTENT_WITHIN_QUERY, and index keys cannot contain the function.
	auto within_query = ScalarFunction::Create(conn.GetFactory());
	within_query.SetName("prop_query")
	    .SetExecCallback(CountingOneExec)
	    .SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	within_query.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(within_query);

	REQUIRE(CollectInts(conn.Execute("SELECT count(*)::INTEGER FROM duckdb_functions() WHERE function_name = "
	                                 "'prop_query' AND stability = 'CONSISTENT_WITHIN_QUERY'")) ==
	        std::vector<int32_t> {1});
	conn.Execute("CREATE TABLE prop_keys (a INTEGER)").Drain();
	REQUIRE_THROWS_MATCHES(conn.Execute("CREATE INDEX prop_index ON prop_keys ((prop_query(a)))").Drain(), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_QUERY_BINDER));

	// SPECIAL null handling: the callback runs for a NULL argument and produces
	// a value; with default handling the result would be NULL.
	auto special = ScalarFunction::Create(conn.GetFactory());
	special.SetName("prop_special").SetExecCallback(CountingOneExec).SetNullHandling(FunctionNullHandling::SPECIAL);
	special.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(special);

	REQUIRE(CollectInts(conn.Execute("SELECT prop_special(NULL::INTEGER)")) == std::vector<int32_t> {1});
}

// ---------------------------------------------------------------------------
// Parameter kinds: cpp_kw(x, /, *args, scale := 1, **kwargs) = (x + sum(args) + sum(kwargs)) * scale.
// ---------------------------------------------------------------------------

namespace {

struct KindBind {
	idx_t scale_index;
	bool operator==(const KindBind &other) const {
		return scale_index == other.scale_index;
	}
};

struct {
	ArgumentCounts counts;
	std::vector<std::string> names;
	std::optional<int32_t> scale;
	LogicalTypeId scale_type = LogicalTypeId::INVALID;
	bool missing_found = true;
} kind_probe;

void KindBindCb(ScalarFunction::BindInput &input) {
	kind_probe.counts = input.GetArgumentCounts();
	kind_probe.names.clear();
	for (idx_t i = 0; i < input.GetArgCount(); i++) {
		kind_probe.names.push_back(input.GetArgName(i));
	}
	kind_probe.missing_found = input.FindArg("missing").has_value();
	// "scale" is named-only, so the call always has it
	const auto scale_index = input.FindArg("scale").value();
	kind_probe.scale_type = input.GetArgType(scale_index).GetTypeId();
	const auto scale = input.TryGetConstantArgument(scale_index);
	kind_probe.scale = scale ? std::optional<int32_t>(scale->Get<int32_t>()) : std::nullopt;
	input.SetBindData<KindBind>(KindBind {scale_index});
}

void KindExecCb(ScalarFunction::ExecInput &input) {
	const auto scale_index = input.GetBindData<KindBind>().scale_index;
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	const auto count = input.GetRowCount();
	for (idx_t i = 0; i < count; i++) {
		out[i] = 0;
	}
	auto accumulate = [&](Vector vector, bool multiply) {
		const auto view = vector.GetView();
		for (idx_t i = 0; i < count; i++) {
			const auto value = view.Data<int32_t>()[view.SelAt(i)];
			out[i] = multiply ? out[i] * value : out[i] + value;
		}
	};
	for (idx_t i = 0; i < input.GetArgCount(); i++) {
		if (i != scale_index) {
			accumulate(input.GetArg(i), false);
		}
	}
	accumulate(input.GetArg(scale_index), true);
}

} // namespace

TEST_CASE("Stable C++API: scalar function parameter kinds lay out the argument list", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	auto function = ScalarFunction::Create(conn.GetFactory());
	function.SetName("cpp_kw").SetBindCallback(KindBindCb).SetExecCallback(KindExecCb);
	function.WithSignature([&](FunctionSignature &sig) {
		sig.AddKwargs("kwargs", integer);
		sig.AddParameter("scale", integer, Value::Create(factory, int32_t {1}), FunctionParameterKind::NAMED_ONLY);
		sig.AddArgs("args", integer);
		sig.AddParameter("x", integer, FunctionParameterKind::POSITIONAL_ONLY);
		sig.SetReturnType(integer);
	});
	conn.Register(function);

	REQUIRE(CollectInts(conn.Execute("SELECT cpp_kw(4)")) == std::vector<int32_t> {4});
	REQUIRE(kind_probe.counts.positional_fixed == 1);
	REQUIRE(kind_probe.counts.positional_variadic == 0);
	REQUIRE(kind_probe.counts.named_fixed == 1);
	REQUIRE(kind_probe.counts.named_variadic == 0);
	REQUIRE(kind_probe.names == std::vector<std::string> {"x", "scale"});
	REQUIRE(kind_probe.scale == 1);
	REQUIRE(kind_probe.scale_type == LogicalTypeId::INTEGER);
	REQUIRE_FALSE(kind_probe.missing_found);

	REQUIRE(CollectInts(conn.Execute("SELECT cpp_kw(1, 2, 3, extra := 4, scale := 2, x := 5)")) ==
	        std::vector<int32_t> {30});
	REQUIRE(kind_probe.counts.positional_variadic == 2);
	REQUIRE(kind_probe.counts.named_variadic == 2);
	REQUIRE(kind_probe.counts.NamedOffset() == 3);
	REQUIRE(kind_probe.names == std::vector<std::string> {"x", "", "", "scale", "extra", "x"});
	REQUIRE(kind_probe.scale == 2);

	// A column passed as "scale" has no constant value at bind time.
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_kw(r::INTEGER, scale := r::INTEGER) FROM range(3) t(r)")) ==
	        std::vector<int32_t> {0, 1, 4});
	REQUIRE_FALSE(kind_probe.scale.has_value());
}

TEST_CASE("Stable C++API: scalar function overloads carry their own description and examples", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	auto one = ScalarFunction::Create(factory);
	one.SetName("cpp_documented").SetExecCallback(PlusOneExec);
	one.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	one.GetDocs()
	    .SetDescription("Adds one.")
	    .AddExample("Add one", "cpp_documented(41)")
	    .AddExample("", "cpp_documented(-1)")
	    .AddCategory("numeric")
	    .AddCategory("example");
	conn.Register(one);

	auto two = ScalarFunction::Create(factory);
	two.SetName("cpp_documented").SetExecCallback(PlusOneExec);
	two.GetSignature().AddParameter("a", integer).AddParameter("b", integer).SetReturnType(integer);
	two.GetDocs().SetDescription("Adds one, ignoring b.");
	conn.Register(two);

	auto result = conn.Execute(
	    "SELECT description, array_to_string(examples, '|'), parameters::VARCHAR, array_to_string(categories, '|') "
	    "FROM duckdb_functions() WHERE function_name = 'cpp_documented' ORDER BY len(parameters)");
	auto chunk = result.FetchChunk();
	REQUIRE(chunk.GetRowCount() == 2);
	REQUIRE(chunk.GetVector(0).GetValue(0).ToText() == "Adds one.");
	REQUIRE(chunk.GetVector(1).GetValue(0).ToText() == "cpp_documented(41)|cpp_documented(-1)");
	REQUIRE(chunk.GetVector(2).GetValue(0).ToText() == "[a]");
	REQUIRE(chunk.GetVector(0).GetValue(1).ToText() == "Adds one, ignoring b.");
	REQUIRE(chunk.GetVector(1).GetValue(1).ToText() == "");
	REQUIRE(chunk.GetVector(2).GetValue(1).ToText() == "[a, b]");
	REQUIRE(chunk.GetVector(3).GetValue(0).ToText() == "numeric|example");
	REQUIRE(chunk.GetVector(3).GetValue(1).ToText() == "");
}
