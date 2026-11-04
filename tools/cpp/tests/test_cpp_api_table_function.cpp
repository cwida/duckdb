#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include "duckdb/common/vector_size.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: TableFunction. The extension-registration path is
// covered by the static extension demo (test/api/capi/v2/static_extension);
// these cover the connection path.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

// Collect a single BIGINT column, asserting every row valid.
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

// Collect a single VARCHAR column, asserting every row valid.
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
// cpp_range(n): one BIGINT column "i" carrying 0..n-1, produced in batches.
// ---------------------------------------------------------------------------

struct RangeBind {
	int64_t count = 0;
};
struct RangeGlobal {
	int64_t position = 0;
};

void RangeBind_(TableFunction::BindInput &input) {
	const auto count = input.GetConstantArgument(0).Get<int64_t>();
	input.AddResultColumn("i", input.GetContext().GetFactory().ParseType("BIGINT"));
	input.SetCardinality(static_cast<idx_t>(count), true);
	input.SetBindData<RangeBind>(RangeBind {count});
}

void RangeInitGlobal(TableFunction::InitGlobalInput &input) {
	input.SetGlobalState<RangeGlobal>(RangeGlobal {0});
}

// How many rows a batch produces. The API exposes no output-chunk capacity, so the tests stay within the smallest
// vector size the build can be configured with.
constexpr int64_t BATCH_ROWS = 2;

void RangeExec(TableFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<RangeBind>();
	auto &global = input.GetGlobalState<RangeGlobal>();

	auto chunk = input.GetOutputChunk();
	auto vec = chunk.GetVector(0);
	auto *out = vec.GetDataMutable<int64_t>();

	auto produced = bind.count - global.position;
	if (produced > BATCH_ROWS) {
		produced = BATCH_ROWS;
	}
	if (produced < 0) {
		produced = 0;
	}
	for (int64_t i = 0; i < produced; i++) {
		out[i] = global.position + i;
	}
	global.position += produced;

	// The first vector's size is the batch's row count; 0 ends the scan.
	vec.SetSize(static_cast<idx_t>(produced));
}

// Registers cpp_range on the connection.
void RegisterRange(Connection &conn, const std::string &name) {
	auto &factory = conn.GetFactory();
	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName(name);
	function.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	function.SetBindCallback(RangeBind_).SetInitGlobalCallback(RangeInitGlobal).SetExecCallback(RangeExec);
	conn.Register(function);
}

// ---------------------------------------------------------------------------
// cpp_pairs(n): two columns, to pin that the row count set on the first vector
// reaches the others.
// ---------------------------------------------------------------------------

void PairsBind_(TableFunction::BindInput &input) {
	const auto count = input.GetConstantArgument(0).Get<int64_t>();
	auto &factory = input.GetContext().GetFactory();
	input.AddResultColumn("a", factory.ParseType("INTEGER"));
	input.AddResultColumn("b", factory.ParseType("VARCHAR"));
	input.SetBindData<RangeBind>(RangeBind {count});
}

void PairsExec(TableFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<RangeBind>();
	auto &global = input.GetGlobalState<RangeGlobal>();

	auto chunk = input.GetOutputChunk();
	auto produced = bind.count - global.position;
	if (produced > BATCH_ROWS) {
		produced = BATCH_ROWS;
	}
	if (produced < 0) {
		produced = 0;
	}

	auto a = chunk.GetVector(0);
	auto b = chunk.GetVector(1);
	auto *values = a.GetDataMutable<int32_t>();
	for (int64_t i = 0; i < produced; i++) {
		values[i] = static_cast<int32_t>(global.position + i);
		// Longer than blob_t::INLINE_LENGTH, so the bytes land in the vector's arena: an assertion build compiles the
		// engine with DUCKDB_DEBUG_NO_INLINE, whose string_t reads the pointer even for values the API would inline.
		b.AssignString(static_cast<idx_t>(i), "row-payload-" + std::to_string(global.position + i));
	}
	global.position += produced;

	// Only the first vector is sized; the engine propagates the count to "b".
	a.SetSize(static_cast<idx_t>(produced));
}

// ---------------------------------------------------------------------------
// cpp_args(a, b := 7, ...): reports the argument slots the bind callback sees.
// ---------------------------------------------------------------------------

struct ArgsBind {
	std::vector<int64_t> values;
};

void ArgsBind_(TableFunction::BindInput &input) {
	ArgsBind bind;
	for (idx_t i = 0; i < input.GetArgCount(); i++) {
		REQUIRE(input.GetArgType(i).GetTypeId() == LogicalTypeId::BIGINT);
		bind.values.push_back(input.GetConstantArgument(i).Get<int64_t>());
	}
	input.AddResultColumn("v", input.GetContext().GetFactory().ParseType("BIGINT"));
	input.SetBindData<ArgsBind>(std::move(bind));
}

// cpp_kwargs(n, *, step := 1, **opts): reports every argument in list order, and latches the argument names.
std::vector<std::string> arg_names;
ArgumentCounts arg_counts;

void KwargsBind_(TableFunction::BindInput &input) {
	ArgsBind bind;
	arg_names.clear();
	for (idx_t i = 0; i < input.GetArgCount(); i++) {
		arg_names.push_back(input.GetArgName(i));
		bind.values.push_back(input.GetConstantArgument(i).Get<int64_t>());
	}
	arg_counts = input.GetArgumentCounts();
	input.AddResultColumn("v", input.GetContext().GetFactory().ParseType("BIGINT"));
	input.SetBindData<ArgsBind>(std::move(bind));
}

// Emits one row per argument slot, in slot order, a batch at a time.
void ArgsExec(TableFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<ArgsBind>();
	auto &global = input.GetGlobalState<RangeGlobal>();

	auto chunk = input.GetOutputChunk();
	auto vec = chunk.GetVector(0);
	auto *out = vec.GetDataMutable<int64_t>();

	idx_t produced = 0;
	while (produced < BATCH_ROWS && global.position < static_cast<int64_t>(bind.values.size())) {
		out[produced++] = bind.values[static_cast<size_t>(global.position++)];
	}
	vec.SetSize(produced);
}

// ---------------------------------------------------------------------------
// State plumbing and the optional hooks.
// ---------------------------------------------------------------------------

struct Seed {
	int64_t tag = 0;
};
struct StateGlobal {
	int64_t remaining = 0;
};
struct StateLocal {
	int64_t tag = 0;
};

std::atomic<int> init_global_runs {0};
std::atomic<int> init_local_runs {0};

void StateBind(TableFunction::BindInput &input) {
	input.AddResultColumn("v", input.GetContext().GetFactory().ParseType("BIGINT"));
	input.SetCardinality(3, true);
	input.SetBindData<RangeBind>(RangeBind {3});
}

void StateInitGlobal(TableFunction::InitGlobalInput &input) {
	init_global_runs++;
	const auto &bind = input.GetBindData<RangeBind>();
	input.SetGlobalState<StateGlobal>(StateGlobal {bind.count});
	input.SetMaxThreads(1);
}

void StateInitLocal(TableFunction::InitLocalInput &input) {
	init_local_runs++;
	// The local state derives its tag from the user data, reached through this phase's input.
	const auto &seed = input.GetUserData<Seed>();
	// Touching the global state here is what a real scan does to claim work.
	(void)input.GetGlobalState<StateGlobal>();
	input.SetLocalState<StateLocal>(StateLocal {seed.tag});
}

void StateExec(TableFunction::ExecInput &input) {
	auto &global = input.GetGlobalState<StateGlobal>();
	const auto &local = input.GetLocalState<StateLocal>();

	auto chunk = input.GetOutputChunk();
	auto vec = chunk.GetVector(0);
	auto *out = vec.GetDataMutable<int64_t>();

	idx_t produced = 0;
	if (global.remaining > 0) {
		global.remaining--;
		out[0] = local.tag;
		produced = 1;
	}
	vec.SetSize(produced);
}

// A bind callback that throws: the exception must surface as the query's error.
void ThrowingBind(TableFunction::BindInput &) {
	throw InvalidInputException("bind refused");
}

// An exec callback that throws once the scan is under way.
void ThrowingExec(TableFunction::ExecInput &) {
	throw InvalidInputException("exec refused");
}

// ---------------------------------------------------------------------------
// cpp_proj(): three BIGINT columns x, y, z, three rows, cell = declared column * 100 + row. The exec callback
// fills whatever the scan asks for through the column mapping.
// ---------------------------------------------------------------------------

struct ProjGlobal {
	idx_t position = 0;
};

constexpr idx_t PROJ_ROWS = 3;

std::vector<idx_t> proj_columns;

void ProjBind(TableFunction::BindInput &input) {
	auto bigint = input.GetContext().GetFactory().ParseType("BIGINT");
	input.AddResultColumn("x", bigint);
	input.AddResultColumn("y", bigint);
	input.AddResultColumn("z", bigint);
}

void ProjInitGlobal(TableFunction::InitGlobalInput &input) {
	proj_columns.clear();
	for (idx_t i = 0; i < input.GetColumnCount(); i++) {
		proj_columns.push_back(input.GetColumnIndex(i));
	}
	input.SetGlobalState<ProjGlobal>();
}

void ProjExec(TableFunction::ExecInput &input) {
	auto &global = input.GetGlobalState<ProjGlobal>();
	idx_t rows = PROJ_ROWS - global.position;
	if (rows > static_cast<idx_t>(BATCH_ROWS)) {
		rows = static_cast<idx_t>(BATCH_ROWS);
	}

	auto chunk = input.GetOutputChunk();
	for (idx_t i = 0; i < input.GetColumnCount(); i++) {
		auto column = input.GetColumnIndex(i);
		auto vec = chunk.GetVector(i);
		auto *out = vec.GetDataMutable<int64_t>();
		for (idx_t row = 0; row < rows; row++) {
			out[row] = static_cast<int64_t>(column * 100 + global.position + row);
		}
		if (i == 0) {
			vec.SetSize(rows);
		}
	}
	global.position += rows;
}

// ---------------------------------------------------------------------------
// cpp_claim(n): column i BIGINT with 0..n-1. Claims every "i < constant" predicate, then deliberately produces two
// rows past the bound, which only reach the result if the engine really stopped applying the predicate.
// ---------------------------------------------------------------------------

struct ClaimBind {
	int64_t count = 0;
	int64_t bound = -1;
};

// Every predicate the pushdown callback saw, rendered.
std::vector<std::string> claim_seen;

std::string RenderExpression(const TableFunction::FilterPushdownInput &input, const Expression &expr) {
	switch (expr.GetType()) {
	case ExpressionType::VALUE_CONSTANT:
		return expr.GetConstantValue().ToText();
	case ExpressionType::BOUND_COLUMN_REF:
		return "col" + std::to_string(input.GetColumnIndex(expr.GetColumnIndex()));
	default:
		break;
	}
	std::string children;
	for (idx_t i = 0; i < expr.GetChildCount(); i++) {
		if (i > 0) {
			children += ", ";
		}
		children += RenderExpression(input, expr.GetChild(i));
	}
	switch (expr.GetType()) {
	case ExpressionType::OPERATOR_CAST:
		return std::string(expr.GetCastMode() == CastMode::TRY ? "try_cast(" : "cast(") + children +
		       ")::" + expr.GetReturnType().ToText();
	case ExpressionType::CONJUNCTION_AND:
		return "and(" + children + ")";
	case ExpressionType::CONJUNCTION_OR:
		return "or(" + children + ")";
	case ExpressionType::OPERATOR_IS_NULL:
		return "is_null(" + children + ")";
	case ExpressionType::INVALID:
		return "invalid(" + children + ")";
	case ExpressionType::BOUND_FUNCTION:
		return expr.GetFunctionQualifiedName().Render() + "(" + children + ")";
	default:
		return expr.GetFunctionName() + "(" + children + ")";
	}
}

void ClaimBind_(TableFunction::BindInput &input) {
	const auto count = input.GetConstantArgument(0).Get<int64_t>();
	input.AddResultColumn("i", input.GetContext().GetFactory().ParseType("BIGINT"));
	input.SetBindData<ClaimBind>(ClaimBind {count, -1});
}

void ClaimPushdown(TableFunction::FilterPushdownInput &input) {
	auto &bind = input.GetBindData<ClaimBind>();
	for (idx_t i = 0; i < input.GetFilterCount(); i++) {
		auto filter = input.GetFilter(i);
		claim_seen.push_back(RenderExpression(input, filter));
		if (filter.GetType() != ExpressionType::COMPARE_LESSTHAN) {
			continue;
		}
		auto left = filter.GetChild(0);
		auto right = filter.GetChild(1);
		if (left.GetType() != ExpressionType::BOUND_COLUMN_REF || right.GetType() != ExpressionType::VALUE_CONSTANT ||
		    input.GetColumnIndex(left.GetColumnIndex()) != 0) {
			continue;
		}
		bind.bound = right.GetConstantValue().Get<int64_t>();
		input.Accept(i);
	}
}

void ClaimExec(TableFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<ClaimBind>();
	auto &global = input.GetGlobalState<RangeGlobal>();

	auto chunk = input.GetOutputChunk();
	auto vec = chunk.GetVector(0);
	auto *out = vec.GetDataMutable<int64_t>();

	auto limit = bind.bound < 0 ? bind.count : bind.bound + 2;
	if (limit > bind.count) {
		limit = bind.count;
	}
	auto produced = limit - global.position;
	if (produced > BATCH_ROWS) {
		produced = BATCH_ROWS;
	}
	if (produced < 0) {
		produced = 0;
	}
	for (int64_t i = 0; i < produced; i++) {
		out[i] = global.position + i;
	}
	global.position += produced;
	vec.SetSize(static_cast<idx_t>(produced));
}

// Whether an accessor refused a node it does not apply to, checked inside the callback where the node lives.
bool misuse_refused = false;

void MisusePushdown(TableFunction::FilterPushdownInput &input) {
	auto filter = input.GetFilter(0);
	misuse_refused = false;
	try {
		filter.GetConstantValue();
	} catch (const InvalidInputException &) {
		misuse_refused = true;
	}
}

void ThrowingPushdown(TableFunction::FilterPushdownInput &) {
	throw InvalidInputException("pushdown refused");
}

void RegisterClaim(Connection &conn, const std::string &name, TableFunction::FilterPushdownCallback pushdown) {
	auto &factory = conn.GetFactory();
	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName(name);
	function.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	function.SetBindCallback(ClaimBind_)
	    .SetInitGlobalCallback(RangeInitGlobal)
	    .SetExecCallback(ClaimExec)
	    .SetFilterPushdownCallback(pushdown);
	conn.Register(function);
}

} // namespace

TEST_CASE("Stable C++API: table function projection pushdown", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName("cpp_proj")
	    .SetBindCallback(ProjBind)
	    .SetInitGlobalCallback(ProjInitGlobal)
	    .SetExecCallback(ProjExec)
	    .SetProjectionPushdown(true);
	conn.Register(function);

	REQUIRE(CollectBigints(conn.Execute("SELECT z FROM cpp_proj()")) == std::vector<int64_t> {200, 201, 202});
	REQUIRE(proj_columns == std::vector<idx_t> {2});
	REQUIRE(CollectBigints(conn.Execute("SELECT x + z FROM cpp_proj()")) == std::vector<int64_t> {200, 202, 204});
	REQUIRE(proj_columns.size() == 2);
}

TEST_CASE("Stable C++API: table function filter pushdown", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterClaim(conn, "cpp_claim", ClaimPushdown);

	claim_seen.clear();
	REQUIRE(CollectBigints(conn.Execute("SELECT count(*) FROM cpp_claim(100) WHERE i < 10")) ==
	        std::vector<int64_t> {12});
	REQUIRE(claim_seen == std::vector<std::string> {"<(col0, 10)"});

	// The optimizer may offer the unclaimed predicates more than once, so only membership is pinned here.
	claim_seen.clear();
	REQUIRE(CollectBigints(conn.Execute("SELECT count(*) FROM cpp_claim(100) WHERE i < 10 AND i % 2 = 0")) ==
	        std::vector<int64_t> {6});
	REQUIRE(std::count(claim_seen.begin(), claim_seen.end(), "<(col0, 10)") == 1);
	REQUIRE(std::count(claim_seen.begin(), claim_seen.end(), "=(\"system\".main.\"%\"(col0, 2), 0)") >= 1);

	claim_seen.clear();
	REQUIRE(CollectBigints(conn.Execute(
	            "SELECT count(*) FROM cpp_claim(100) WHERE (i = 1 OR i IS NULL) AND TRY_CAST(i AS VARCHAR) = '1'")) ==
	        std::vector<int64_t> {1});
	std::sort(claim_seen.begin(), claim_seen.end());
	claim_seen.erase(std::unique(claim_seen.begin(), claim_seen.end()), claim_seen.end());
	REQUIRE(claim_seen == std::vector<std::string> {"=(try_cast(col0)::VARCHAR, 1)", "or(=(col0, 1), is_null(col0))"});
}

TEST_CASE("Stable C++API: expression accessors refuse other node types and errors propagate", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterClaim(conn, "cpp_misuse", MisusePushdown);
	RegisterClaim(conn, "cpp_throwing", ThrowingPushdown);

	REQUIRE(CollectBigints(conn.Execute("SELECT count(*) FROM cpp_misuse(5) WHERE i < 3")) == std::vector<int64_t> {3});
	REQUIRE(misuse_refused);
	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT count(*) FROM cpp_throwing(5) WHERE i < 3").Drain(), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: table function registers and scans", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	RegisterRange(conn, "cpp_range");

	REQUIRE(CollectBigints(conn.Execute("SELECT * FROM cpp_range(4)")) == std::vector<int64_t> {0, 1, 2, 3});
	REQUIRE(CollectBigints(conn.Execute("SELECT sum(i) FROM cpp_range(10)")) == std::vector<int64_t> {45});
	// A scan producing nothing at all, and one spanning several batches.
	REQUIRE(CollectBigints(conn.Execute("SELECT * FROM cpp_range(0)")).empty());
	REQUIRE(CollectBigints(conn.Execute("SELECT count(*) FROM cpp_range(5000)")) == std::vector<int64_t> {5000});
}

TEST_CASE("Stable C++API: table function with several result columns", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName("cpp_pairs");
	function.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	function.SetBindCallback(PairsBind_).SetInitGlobalCallback(RangeInitGlobal).SetExecCallback(PairsExec);
	conn.Register(function);

	// Both columns carry all rows: the count set on "a" reached "b".
	REQUIRE(CollectBigints(conn.Execute("SELECT count(b) FROM cpp_pairs(5)")) == std::vector<int64_t> {5});
	REQUIRE(CollectBigints(conn.Execute("SELECT sum(a) FROM cpp_pairs(5)")) == std::vector<int64_t> {10});
	REQUIRE(CollectStrings(conn.Execute("SELECT string_agg(b, ',' ORDER BY a) FROM cpp_pairs(3)")) ==
	        std::vector<std::string> {"row-payload-0,row-payload-1,row-payload-2"});
}

TEST_CASE("Stable C++API: table function parameter defaults, named arguments and varargs", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto bigint = factory.ParseType("BIGINT");

	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName("cpp_args");
	function.WithSignature([&](FunctionSignature &sig) {
		sig.AddParameter("a", bigint);
		sig.AddParameter("b", bigint, Value::Create(factory, int64_t {7}));
		sig.AddArgs("rest", bigint);
	});
	function.SetBindCallback(ArgsBind_).SetInitGlobalCallback(RangeInitGlobal).SetExecCallback(ArgsExec);
	conn.Register(function);

	// Only the required parameter: the defaulted one is still present, carrying its default.
	REQUIRE(CollectBigints(conn.Execute("SELECT * FROM cpp_args(1)")) == std::vector<int64_t> {1, 7});
	// A defaulted parameter passed by name.
	REQUIRE(CollectBigints(conn.Execute("SELECT * FROM cpp_args(1, b => 5)")) == std::vector<int64_t> {1, 5});
	// Positional arguments fill the slots in order, as in Python: b takes 30, and the variadic tail takes the rest.
	REQUIRE(CollectBigints(conn.Execute("SELECT * FROM cpp_args(1, 30, 40)")) == std::vector<int64_t> {1, 30, 40});
}

TEST_CASE("Stable C++API: table function named-only parameters and kwargs", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto bigint = factory.ParseType("BIGINT");

	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName("cpp_kwargs");
	function.WithSignature([&](FunctionSignature &sig) {
		sig.AddParameter("n", bigint);
		sig.AddParameter("step", bigint, Value::Create(factory, int64_t {1}), FunctionParameterKind::NAMED_ONLY);
		sig.AddKwargs("opts", bigint);
	});
	function.SetBindCallback(KwargsBind_).SetInitGlobalCallback(RangeInitGlobal).SetExecCallback(ArgsExec);
	conn.Register(function);

	// The omitted named-only parameter carries its default.
	REQUIRE(CollectBigints(conn.Execute("SELECT * FROM cpp_kwargs(5)")) == std::vector<int64_t> {5, 1});
	REQUIRE(arg_names == std::vector<std::string> {"n", "step"});
	REQUIRE(arg_counts.named_variadic == 0);
	// The standard parameter passed by name stays in the args list; named-only parameters lead the kwargs list,
	// followed by "**kwargs" in call order.
	REQUIRE(CollectBigints(conn.Execute("SELECT * FROM cpp_kwargs(extra => 9, n => 5, step => 3, more => 8)")) ==
	        std::vector<int64_t> {5, 3, 9, 8});
	REQUIRE(arg_names == std::vector<std::string> {"n", "step", "extra", "more"});
	REQUIRE(arg_counts.named_fixed == 1);
	REQUIRE(arg_counts.named_variadic == 2);
}

TEST_CASE("Stable C++API: table function user data, global and local state", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	init_global_runs = 0;
	init_local_runs = 0;

	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName("cpp_state").SetUserData<Seed>(Seed {42});
	function.SetBindCallback(StateBind)
	    .SetInitGlobalCallback(StateInitGlobal)
	    .SetInitLocalCallback(StateInitLocal)
	    .SetExecCallback(StateExec);
	conn.Register(function);

	// The bind data seeded three rows, each carrying the local state's tag, which came from the user data.
	REQUIRE(CollectBigints(conn.Execute("SELECT * FROM cpp_state()")) == std::vector<int64_t> {42, 42, 42});
	REQUIRE(init_global_runs >= 1);
	REQUIRE(init_local_runs >= 1);
}

TEST_CASE("Stable C++API: table function callbacks report failure by throwing", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto bad_bind = TableFunction::Create(conn.GetFactory());
	bad_bind.SetName("cpp_bad_bind").SetBindCallback(ThrowingBind).SetExecCallback(StateExec);
	conn.Register(bad_bind);

	auto bad_exec = TableFunction::Create(conn.GetFactory());
	bad_exec.SetName("cpp_bad_exec")
	    .SetBindCallback(StateBind)
	    .SetInitGlobalCallback(StateInitGlobal)
	    .SetInitLocalCallback(StateInitLocal)
	    .SetExecCallback(ThrowingExec);
	conn.Register(bad_exec);

	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT * FROM cpp_bad_bind()").Drain(), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT * FROM cpp_bad_exec()").Drain(), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
}

TEST_CASE("Stable C++API: table function registration refusals", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto bigint = factory.ParseType("BIGINT");

	// No name.
	{
		auto function = TableFunction::Create(conn.GetFactory());
		function.SetBindCallback(StateBind).SetExecCallback(StateExec);
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// No bind callback: a table function has no other way to declare its columns.
	{
		auto function = TableFunction::Create(conn.GetFactory());
		function.SetName("cpp_no_bind").SetExecCallback(StateExec);
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// No exec callback.
	{
		auto function = TableFunction::Create(conn.GetFactory());
		function.SetName("cpp_no_exec").SetBindCallback(StateBind);
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// A return type on the signature: the columns come from bind instead.
	{
		auto function = TableFunction::Create(conn.GetFactory());
		function.SetName("cpp_return_type").SetBindCallback(StateBind).SetExecCallback(StateExec);
		function.GetSignature().SetReturnType(bigint);
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
}

// ---------------------------------------------------------------------------
// Partition callbacks.
// ---------------------------------------------------------------------------

namespace {

// Collect one VARCHAR column by index; EXPLAIN puts the rendered plan in column 1.
std::vector<std::string> CollectStringsAt(QueryResult result, idx_t column) {
	std::vector<std::string> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(column).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			REQUIRE(view.IsValid(i));
			out.emplace_back(view.Data<varchar_t>()[view.SelAt(i)].view());
		}
	}
	return out;
}

bool ExplainContains(Connection &conn, const std::string &sql, const std::string &needle) {
	for (auto &line : CollectStringsAt(conn.Execute("EXPLAIN " + sql), 1)) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// The error a query fails with; asserts that it does fail.
std::string QueryError(Connection &conn, const std::string &sql) {
	try {
		conn.Execute(sql).Drain();
	} catch (const Exception &ex) {
		return ex.what();
	}
	FAIL("query succeeded: " + sql);
	return "";
}

// Whether a misuse inside a callback was refused with INPUT_INVALID; a REQUIRE there would throw through the callback.
template <class F>
bool RefusedAsInvalid(F &&f) {
	try {
		f();
	} catch (const Exception &ex) {
		return ex.GetCode() == DUCKDB_V2_ERROR_INPUT_INVALID;
	}
	return false;
}

// ---------------------------------------------------------------------------
// cpp_part(n): BIGINT part_col and val, n groups of three rows, one group per batch, spanning several chunks when the
// vector size is smaller than a group; part_col carries the group and val group * 10 + row. The partition data callback
// reports the group as both batch index and partition value.
// ---------------------------------------------------------------------------

struct PartBind {
	int64_t groups = 0;
};
struct PartGlobal {
	int64_t position = 0;
	int64_t last_group = -1;
};

constexpr int64_t PART_ROWS_PER_GROUP = 3;

bool part_oob_value_refused = false;
bool part_wrong_type_refused = false;
bool part_oob_batch_refused = false;
bool part_oob_info_refused = false;

void PartBind_(TableFunction::BindInput &input) {
	auto bigint = input.GetContext().GetFactory().ParseType("BIGINT");
	input.AddResultColumn("part_col", bigint);
	input.AddResultColumn("val", bigint);
	input.SetBindData<PartBind>(PartBind {input.GetConstantArgument(0).Get<int64_t>()});
}

void PartInitGlobal(TableFunction::InitGlobalInput &input) {
	input.SetGlobalState<PartGlobal>();
}

void PartExec(TableFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<PartBind>();
	auto &global = input.GetGlobalState<PartGlobal>();
	auto chunk = input.GetOutputChunk();
	auto part_vec = chunk.GetVector(0);
	if (global.position >= bind.groups * PART_ROWS_PER_GROUP) {
		part_vec.SetSize(0);
		return;
	}
	auto group = global.position / PART_ROWS_PER_GROUP;
	auto offset = global.position % PART_ROWS_PER_GROUP;
	auto rows = std::min<int64_t>(PART_ROWS_PER_GROUP - offset, STANDARD_VECTOR_SIZE);
	global.last_group = group;
	auto *part = part_vec.GetDataMutable<int64_t>();
	auto *val = chunk.GetVector(1).GetDataMutable<int64_t>();
	for (int64_t i = 0; i < rows; i++) {
		part[i] = group;
		val[i] = group * 10 + offset + i;
	}
	global.position += rows;
	part_vec.SetSize(static_cast<idx_t>(rows));
}

void PartData(TableFunction::PartitionDataInput &input) {
	const auto &global = input.GetGlobalState<PartGlobal>();
	auto &factory = input.GetContext().GetFactory();

	part_oob_batch_refused = RefusedAsInvalid([&]() { input.SetBatchIndex(static_cast<idx_t>(1) << 62); });
	input.SetBatchIndex(static_cast<idx_t>(global.last_group));
	if (!input.RequiresPartitionColumns()) {
		return;
	}
	if (input.GetPartitionColumnCount() != 1 || input.GetPartitionColumnIndex(0) != 0) {
		return;
	}
	auto wrong_type = Value::Create(factory, static_cast<int32_t>(global.last_group));
	part_wrong_type_refused = RefusedAsInvalid([&]() { input.SetPartitionValue(0, wrong_type); });
	auto value = Value::Create(factory, global.last_group);
	part_oob_value_refused = RefusedAsInvalid([&]() { input.SetPartitionValue(1, value); });
	input.SetPartitionValue(0, value);
}

void PartInfo(TableFunction::PartitioningInput &input) {
	part_oob_info_refused =
	    RefusedAsInvalid([&]() { input.SetPartitionInfo(static_cast<TableFunction::PartitionInfo>(99)); });
	if (input.GetPartitionColumnCount() == 1 && input.GetPartitionColumnIndex(0) == 0) {
		input.SetPartitionInfo(TableFunction::PartitionInfo::SINGLE_VALUE_PARTITIONS);
	}
}

void AlwaysPartitioned(TableFunction::PartitioningInput &input) {
	input.SetPartitionInfo(TableFunction::PartitionInfo::SINGLE_VALUE_PARTITIONS);
}

// Batch index 0, 1, 0: decreasing on the third group.
void DecreasingBatchData(TableFunction::PartitionDataInput &input) {
	const auto &global = input.GetGlobalState<PartGlobal>();
	input.SetBatchIndex(global.last_group == 2 ? 0 : static_cast<idx_t>(global.last_group));
	if (input.RequiresPartitionColumns()) {
		auto &factory = input.GetContext().GetFactory();
		input.SetPartitionValue(0, Value::Create(factory, global.last_group));
	}
}

// Batch index 0 for every group while the partition value changes with the group.
void ConstantBatchData(TableFunction::PartitionDataInput &input) {
	const auto &global = input.GetGlobalState<PartGlobal>();
	input.SetBatchIndex(0);
	if (input.RequiresPartitionColumns()) {
		auto &factory = input.GetContext().GetFactory();
		input.SetPartitionValue(0, Value::Create(factory, global.last_group));
	}
}

void NoBatchIndexData(TableFunction::PartitionDataInput &) {
}

void NoPartitionValueData(TableFunction::PartitionDataInput &input) {
	input.SetBatchIndex(0);
}

void ThrowingPartitionData(TableFunction::PartitionDataInput &) {
	throw InvalidInputException("partition data refused");
}

void ThrowingPartitioning(TableFunction::PartitioningInput &) {
	throw InvalidInputException("partition info refused");
}

void RegisterPart(Connection &conn, const std::string &name, TableFunction::PartitioningCallback partitioning,
                  TableFunction::PartitionDataCallback partition_data) {
	auto &factory = conn.GetFactory();
	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName(name);
	function.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	function.SetBindCallback(PartBind_)
	    .SetInitGlobalCallback(PartInitGlobal)
	    .SetExecCallback(PartExec)
	    .SetPartitioningCallback(partitioning)
	    .SetPartitionDataCallback(partition_data);
	conn.Register(function);
}

// ---------------------------------------------------------------------------
// cpp_proj_part(n): cpp_part's rows behind projection pushdown, with an INTEGER "pad" declared first so part_col
// (declared index 1) sits at scan position 0 whenever pad is pruned.
// ---------------------------------------------------------------------------

std::atomic<idx_t> proj_part_reported_column {0};

void ProjPartBind(TableFunction::BindInput &input) {
	auto bigint = input.GetContext().GetFactory().ParseType("BIGINT");
	input.AddResultColumn("pad", input.GetContext().GetFactory().ParseType("INTEGER"));
	input.AddResultColumn("part_col", bigint);
	input.AddResultColumn("val", bigint);
	input.SetBindData<PartBind>(PartBind {input.GetConstantArgument(0).Get<int64_t>()});
}

void ProjPartExec(TableFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<PartBind>();
	auto &global = input.GetGlobalState<PartGlobal>();
	auto chunk = input.GetOutputChunk();
	if (global.position >= bind.groups * PART_ROWS_PER_GROUP) {
		chunk.GetVector(0).SetSize(0);
		return;
	}
	auto group = global.position / PART_ROWS_PER_GROUP;
	auto offset = global.position % PART_ROWS_PER_GROUP;
	auto rows = std::min<int64_t>(PART_ROWS_PER_GROUP - offset, STANDARD_VECTOR_SIZE);
	global.last_group = group;
	global.position += rows;
	for (idx_t i = 0; i < input.GetColumnCount(); i++) {
		auto vec = chunk.GetVector(i);
		for (int64_t row = 0; row < rows; row++) {
			switch (input.GetColumnIndex(i)) {
			case 0:
				vec.GetDataMutable<int32_t>()[row] = -1;
				break;
			case 1:
				vec.GetDataMutable<int64_t>()[row] = group;
				break;
			default:
				vec.GetDataMutable<int64_t>()[row] = group * 10 + offset + row;
				break;
			}
		}
		if (i == 0) {
			vec.SetSize(static_cast<idx_t>(rows));
		}
	}
}

void ProjPartData(TableFunction::PartitionDataInput &input) {
	const auto &global = input.GetGlobalState<PartGlobal>();
	input.SetBatchIndex(static_cast<idx_t>(global.last_group));
	if (!input.RequiresPartitionColumns()) {
		return;
	}
	proj_part_reported_column = input.GetPartitionColumnIndex(0);
	auto &factory = input.GetContext().GetFactory();
	input.SetPartitionValue(0, Value::Create(factory, global.last_group));
}

void ProjPartInfo(TableFunction::PartitioningInput &input) {
	if (input.GetPartitionColumnCount() == 1 && input.GetPartitionColumnIndex(0) == 1) {
		input.SetPartitionInfo(TableFunction::PartitionInfo::SINGLE_VALUE_PARTITIONS);
	}
}

// ---------------------------------------------------------------------------
// cpp_batch_order(): one BIGINT column over two batches raced so that batch 1 reaches the sink first; only the
// partition data callback lets the ordered result sink restore insertion order.
// ---------------------------------------------------------------------------

struct BatchOrderGlobal {
	std::atomic<int32_t> claimed {0};
	std::atomic<int32_t> started {0};
};
struct BatchOrderLocal {
	int32_t id = 0;
	bool done = false;
};

std::atomic<bool> batch_order_requires_batch_index_seen {false};

void BatchOrderBind(TableFunction::BindInput &input) {
	input.AddResultColumn("b", input.GetContext().GetFactory().ParseType("BIGINT"));
}

void BatchOrderInitGlobal(TableFunction::InitGlobalInput &input) {
	input.SetGlobalState<BatchOrderGlobal>();
	input.SetMaxThreads(2);
}

void BatchOrderInitLocal(TableFunction::InitLocalInput &input) {
	auto &global = input.GetGlobalState<BatchOrderGlobal>();
	auto id = global.claimed.fetch_add(1);
	global.started.fetch_add(1);
	// Bounded barrier: both local states must exist before either races ahead in exec, or the completion order
	// below would not be deterministic. Bounded so a scheduler that never launches the second task cannot hang.
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (global.started.load() < 2 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	input.SetLocalState<BatchOrderLocal>(BatchOrderLocal {id, false});
}

void BatchOrderExec(TableFunction::ExecInput &input) {
	auto &local = input.GetLocalState<BatchOrderLocal>();
	auto vec = input.GetOutputChunk().GetVector(0);
	if (local.done) {
		vec.SetSize(0);
		return;
	}
	// Batch 0 finishes later than batch 1 despite being claimed first: only batch-index ordering restores order.
	if (local.id == 0) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	vec.GetDataMutable<int64_t>()[0] = local.id;
	local.done = true;
	vec.SetSize(1);
}

void BatchOrderData(TableFunction::PartitionDataInput &input) {
	if (input.RequiresBatchIndex()) {
		batch_order_requires_batch_index_seen = true;
	}
	input.SetBatchIndex(static_cast<idx_t>(input.GetLocalState<BatchOrderLocal>().id));
}

void RegisterBatchOrder(Connection &conn, const std::string &name, bool with_partition_data) {
	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName(name)
	    .SetBindCallback(BatchOrderBind)
	    .SetInitGlobalCallback(BatchOrderInitGlobal)
	    .SetInitLocalCallback(BatchOrderInitLocal)
	    .SetExecCallback(BatchOrderExec);
	if (with_partition_data) {
		function.SetPartitionDataCallback(BatchOrderData);
	}
	conn.Register(function);
}

} // namespace

TEST_CASE("Stable C++API: table function partition callbacks feed a partitioned aggregate", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterPart(conn, "cpp_part", PartInfo, PartData);

	// The partitioning callback claims only part_col: GROUP BY part_col unlocks the partitioned aggregate, val does
	// not.
	REQUIRE(
	    ExplainContains(conn, "SELECT part_col, count(*) FROM cpp_part(3) GROUP BY part_col", "Partitioned Aggregate"));
	REQUIRE_FALSE(ExplainContains(conn, "SELECT val, count(*) FROM cpp_part(3) GROUP BY val", "Partitioned Aggregate"));

	auto rows = Collect2<int64_t, int64_t>(
	    conn.Execute("SELECT part_col, count(*) FROM cpp_part(3) GROUP BY part_col ORDER BY part_col"), 0, 1);
	REQUIRE(rows == std::vector<std::pair<int64_t, int64_t>> {{0, 3}, {1, 3}, {2, 3}});

	// Each misuse along the way was refused synchronously, without derailing the correct call right after it.
	REQUIRE(part_oob_batch_refused);
	REQUIRE(part_wrong_type_refused);
	REQUIRE(part_oob_value_refused);
	REQUIRE(part_oob_info_refused);
}

TEST_CASE("Stable C++API: table function partition data reports declared columns under projection pushdown",
          "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName("cpp_proj_part");
	function.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	function.SetBindCallback(ProjPartBind)
	    .SetInitGlobalCallback(PartInitGlobal)
	    .SetExecCallback(ProjPartExec)
	    .SetPartitioningCallback(ProjPartInfo)
	    .SetPartitionDataCallback(ProjPartData)
	    .SetProjectionPushdown(true);
	conn.Register(function);

	// Only part_col is scanned, so declared index 1 sits at scan position 0; the callback must still see 1.
	REQUIRE(ExplainContains(conn, "SELECT part_col, count(*) FROM cpp_proj_part(3) GROUP BY part_col",
	                        "Partitioned Aggregate"));
	proj_part_reported_column = 0;
	auto rows = Collect2<int64_t, int64_t>(
	    conn.Execute("SELECT part_col, max(val) FROM cpp_proj_part(3) GROUP BY part_col ORDER BY part_col"), 0, 1);
	REQUIRE(rows == std::vector<std::pair<int64_t, int64_t>> {{0, 2}, {1, 12}, {2, 22}});
	REQUIRE(proj_part_reported_column.load() == 1);
}

TEST_CASE("Stable C++API: table function partition data restores batch order", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	conn.Execute("SET threads=2").Drain();
	RegisterBatchOrder(conn, "cpp_batch_order", true);
	RegisterBatchOrder(conn, "cpp_batch_order_nopart", false);

	batch_order_requires_batch_index_seen = false;
	REQUIRE(CollectBigints(conn.Execute("SELECT * FROM cpp_batch_order()")) == std::vector<int64_t> {0, 1});
	// Otherwise everything ran on one thread and the ordering assertion above passed vacuously.
	REQUIRE(batch_order_requires_batch_index_seen.load());

	// Without the callback the sink cannot restore order; the rows still all arrive.
	REQUIRE(CollectBigints(conn.Execute("SELECT sum(b) FROM cpp_batch_order_nopart()")) == std::vector<int64_t> {1});
}

TEST_CASE("Stable C++API: table function partition data contract violations fail the query", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterPart(conn, "cpp_decreasing_batch", AlwaysPartitioned, DecreasingBatchData);
	RegisterPart(conn, "cpp_constant_batch", AlwaysPartitioned, ConstantBatchData);
	RegisterPart(conn, "cpp_no_batch_index", AlwaysPartitioned, NoBatchIndexData);
	RegisterPart(conn, "cpp_no_partition_value", AlwaysPartitioned, NoPartitionValueData);
	RegisterPart(conn, "cpp_throwing_partition_data", AlwaysPartitioned, ThrowingPartitionData);

	auto decreasing = QueryError(conn, "SELECT part_col, count(*) FROM cpp_decreasing_batch(3) GROUP BY part_col");
	REQUIRE(decreasing.find("cpp_decreasing_batch") != std::string::npos);
	REQUIRE(decreasing.find("must not decrease") != std::string::npos);
	// The connection stays usable: the guard fires before the engine-internal error that would invalidate it.
	REQUIRE(CollectBigints(conn.Execute("SELECT 1::BIGINT")) == std::vector<int64_t> {1});

	auto constant = QueryError(conn, "SELECT part_col, count(*) FROM cpp_constant_batch(3) GROUP BY part_col");
	REQUIRE(constant.find("without changing the batch index") != std::string::npos);

	auto missing_batch = QueryError(conn, "SELECT part_col, count(*) FROM cpp_no_batch_index(3) GROUP BY part_col");
	REQUIRE(missing_batch.find("did not set a batch index") != std::string::npos);

	auto missing_value = QueryError(conn, "SELECT part_col, count(*) FROM cpp_no_partition_value(3) GROUP BY part_col");
	REQUIRE(missing_value.find("index 0") != std::string::npos);

	auto thrown = QueryError(conn, "SELECT part_col, count(*) FROM cpp_throwing_partition_data(3) GROUP BY part_col");
	REQUIRE(thrown.find("partition data refused") != std::string::npos);
}

TEST_CASE("Stable C++API: table function partitioning failures only surface for a partitioned aggregate", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterPart(conn, "cpp_throwing_partitioning", ThrowingPartitioning, NoBatchIndexData);

	// A plain scan never consults the partitioning callback.
	REQUIRE(CollectBigints(conn.Execute("SELECT count(*) FROM cpp_throwing_partitioning(3)")) ==
	        std::vector<int64_t> {9});
	auto message = QueryError(conn, "SELECT part_col, count(*) FROM cpp_throwing_partitioning(3) GROUP BY part_col");
	REQUIRE(message.find("partition info refused") != std::string::npos);
}

TEST_CASE("Stable C++API: table function partitioning requires partition data", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName("cpp_info_only");
	function.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	function.SetBindCallback(PartBind_)
	    .SetInitGlobalCallback(PartInitGlobal)
	    .SetExecCallback(PartExec)
	    .SetPartitioningCallback(AlwaysPartitioned);
	REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	function.SetPartitionDataCallback(NoBatchIndexData);
	conn.Register(function);
	REQUIRE(CollectBigints(conn.Execute("SELECT count(*) FROM cpp_info_only(2)")) == std::vector<int64_t> {6});

	// Clearing the callbacks again is accepted and registers a plain function.
	function.SetName("cpp_info_cleared").SetPartitioningCallback(nullptr).SetPartitionDataCallback(nullptr);
	conn.Register(function);
	REQUIRE_FALSE(ExplainContains(conn, "SELECT part_col, count(*) FROM cpp_info_cleared(2) GROUP BY part_col",
	                              "Partitioned Aggregate"));
}

TEST_CASE("Stable C++API: table function overloads keep their descriptions", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto one = TableFunction::Create(factory);
	one.SetName("cpp_documented_range").WithDocs([](FunctionDocs &docs) {
		docs.SetDescription("Counts to n.").AddExample("Count to three", "FROM cpp_documented_range(3)");
	});
	one.GetSignature().AddParameter("n", factory.ParseType("BIGINT"));
	one.SetBindCallback(RangeBind_).SetInitGlobalCallback(RangeInitGlobal).SetExecCallback(RangeExec);
	conn.Register(one);

	// Adding an overload keeps the first one's description.
	auto two = TableFunction::Create(factory);
	two.SetName("cpp_documented_range");
	two.GetDocs().SetDescription("Counts to n, as text.");
	two.GetSignature().AddParameter("n", factory.ParseType("VARCHAR"));
	two.SetBindCallback(RangeBind_).SetInitGlobalCallback(RangeInitGlobal).SetExecCallback(RangeExec);
	conn.Register(two);

	auto result = conn.Execute("SELECT description, array_to_string(examples, '|') FROM duckdb_functions() "
	                           "WHERE function_name = 'cpp_documented_range' ORDER BY parameter_types::VARCHAR");
	auto chunk = result.FetchChunk();
	REQUIRE(chunk.GetRowCount() == 2);
	REQUIRE(chunk.GetVector(0).GetValue(0).ToText() == "Counts to n.");
	REQUIRE(chunk.GetVector(1).GetValue(0).ToText() == "FROM cpp_documented_range(3)");
	REQUIRE(chunk.GetVector(0).GetValue(1).ToText() == "Counts to n, as text.");
}
