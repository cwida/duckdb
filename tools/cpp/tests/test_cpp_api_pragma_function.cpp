#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: PragmaFunction. Query pragmas expand to SQL that runs
// in place of the PRAGMA statement; exec pragmas run for their side effects.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

// Collect a single INTEGER column, asserting every row valid.
std::vector<int32_t> CollectPragmaInts(QueryResult result) {
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

// PRAGMA cpp_answer -> SELECT 42
std::string AnswerQuery(PragmaFunction::QueryInput &input) {
	if (input.GetArgCount() != 0) {
		throw InvalidInputException("a pragma without parameters received arguments");
	}
	return "SELECT 42";
}

// PRAGMA cpp_shift(n INTEGER, *, shift_by INTEGER := 0) -> SELECT n + shift_by
std::string ShiftQuery(PragmaFunction::QueryInput &input) {
	auto counts = input.GetArgumentCounts();
	if (counts.positional_fixed != 1 || counts.named_fixed != 1) {
		throw InvalidInputException("unexpected argument counts");
	}
	auto shift_by = input.FindArg("shift_by");
	if (!shift_by) {
		throw InvalidInputException("the shift_by argument is missing");
	}
	auto n = input.GetConstantArgument(0).Get<int32_t>();
	auto by = input.GetConstantArgument(*shift_by).Get<int32_t>();
	return "SELECT " + std::to_string(n + by) + "::INTEGER";
}

// The side effects of the exec pragma, carried in its user data.
struct Counter {
	int64_t total = 0;
	idx_t calls = 0;
};

// The counter of the registered exec pragma, latched so the test can read it after the user data moved into the
// registered function.
Counter *registered_counter = nullptr;

// PRAGMA cpp_bump(*amounts BIGINT): adds every amount to the counter.
void BumpExec(PragmaFunction::ExecInput &input) {
	auto &counter = input.GetUserData<Counter>();
	registered_counter = &counter;
	for (idx_t i = 0; i < input.GetArgCount(); i++) {
		counter.total += input.GetConstantArgument(i).Get<int64_t>();
	}
	counter.calls++;
}

std::string ThrowingQuery(PragmaFunction::QueryInput &) {
	throw InvalidInputException("the query pragma failed");
}

std::string MissingUserDataQuery(PragmaFunction::QueryInput &input) {
	input.GetUserData<Counter>();
	return "SELECT 0";
}

} // namespace

TEST_CASE("Stable C++API: query pragma without parameters", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto function = PragmaFunction::Create(conn.GetFactory());
	function.SetName("cpp_answer").SetQueryCallback(AnswerQuery).WithDocs([](FunctionDocs &docs) {
		docs.SetDescription("Returns the answer");
	});
	conn.Register(function);

	REQUIRE(CollectPragmaInts(conn.Execute("PRAGMA cpp_answer")) == std::vector<int32_t> {42});
	REQUIRE(CollectPragmaInts(conn.Execute("SELECT (description = 'Returns the answer')::INTEGER "
	                                       "FROM duckdb_functions() WHERE function_name = 'cpp_answer'")) ==
	        std::vector<int32_t> {1});
}

TEST_CASE("Stable C++API: query pragma receives its arguments", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto function = PragmaFunction::Create(factory);
	function.SetName("cpp_shift").SetQueryCallback(ShiftQuery).WithSignature([&](FunctionSignature &sig) {
		auto integer = factory.ParseType("INTEGER");
		sig.AddParameter("n", integer);
		sig.AddParameter("shift_by", integer, Value::Create(factory, int32_t {0}), FunctionParameterKind::NAMED_ONLY);
	});
	conn.Register(function);

	REQUIRE(CollectPragmaInts(conn.Execute("PRAGMA cpp_shift(7)")) == std::vector<int32_t> {7});
	REQUIRE(CollectPragmaInts(conn.Execute("PRAGMA cpp_shift(7, shift_by = 3)")) == std::vector<int32_t> {10});
	REQUIRE_THROWS_AS(conn.Execute("PRAGMA cpp_shift").Drain(), Exception);
}

TEST_CASE("Stable C++API: exec pragma runs for its side effects", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto function = PragmaFunction::Create(factory);
	function.SetName("cpp_bump").SetExecCallback(BumpExec).SetUserData<Counter>();
	function.GetSignature().AddArgs("amounts", factory.ParseType("BIGINT"));
	conn.Register(function);

	registered_counter = nullptr;
	conn.Execute("PRAGMA cpp_bump(5)").Drain();
	REQUIRE(registered_counter != nullptr);
	REQUIRE(registered_counter->calls == 1);
	REQUIRE(registered_counter->total == 5);

	conn.Execute("PRAGMA cpp_bump(1, 2, 3)").Drain();
	REQUIRE(registered_counter->calls == 2);
	REQUIRE(registered_counter->total == 11);
}

TEST_CASE("Stable C++API: pragma callback errors and registration errors", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	{
		auto function = PragmaFunction::Create(factory);
		function.SetName("cpp_throwing").SetQueryCallback(ThrowingQuery);
		conn.Register(function);
		REQUIRE_THROWS_MATCHES(conn.Execute("PRAGMA cpp_throwing").Drain(), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	{
		auto function = PragmaFunction::Create(factory);
		function.SetName("cpp_no_user_data").SetQueryCallback(MissingUserDataQuery);
		conn.Register(function);
		REQUIRE_THROWS_MATCHES(conn.Execute("PRAGMA cpp_no_user_data").Drain(), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// No callback.
	{
		auto function = PragmaFunction::Create(factory);
		function.SetName("cpp_no_callback");
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// Both callbacks.
	{
		auto function = PragmaFunction::Create(factory);
		function.SetName("cpp_both").SetQueryCallback(AnswerQuery).SetExecCallback(BumpExec);
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// A return type.
	{
		auto function = PragmaFunction::Create(factory);
		function.SetName("cpp_typed").SetQueryCallback(AnswerQuery);
		function.GetSignature().SetReturnType(factory.ParseType("INTEGER"));
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// A name that is taken.
	{
		auto function = PragmaFunction::Create(factory);
		function.SetName("cpp_throwing").SetQueryCallback(AnswerQuery);
		REQUIRE_THROWS_AS(conn.Register(function), Exception);
	}
}
