#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <cctype>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: collation registration and the transaction start
// timestamp. A scalar function mapping a string to its lower-cased form is the
// sort key of a case-insensitive collation.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

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

// The sort key: the string with its ASCII letters lower-cased.
void LowerExec(ScalarFunction::ExecInput &input) {
	const auto in = input.GetArg(0).GetView();
	auto result = input.GetResult();
	auto validity = result.GetValidityMutable();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			validity.SetInvalid(i);
			continue;
		}
		std::string key(in.Data<varchar_t>()[in.SelAt(i)].view());
		for (auto &c : key) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		result.AssignString(i, key);
	}
}

ScalarFunction MakeLowerKey(Connection &conn, const std::string &name, const char *return_type = "VARCHAR") {
	auto &factory = conn.GetFactory();
	auto function = ScalarFunction::Create(factory);
	function.SetName(name).SetExecCallback(LowerExec);
	function.GetSignature()
	    .AddParameter("str", factory.ParseType("VARCHAR"))
	    .SetReturnType(factory.ParseType(return_type));
	return function;
}

void RegisterCollation(Connection &conn, const std::string &name, const std::string &function_name,
                       bool combinable = false, bool not_required_for_equality = false) {
	auto collation = CollationInfo::Create(conn.GetFactory());
	collation.SetName(name)
	    .SetFunctionName(function_name)
	    .SetCombinable(combinable)
	    .SetNotRequiredForEquality(not_required_for_equality);
	conn.Register(collation);
}

// The start timestamp of the transaction the call runs in.
void TxnStartExec(ScalarFunction::ExecInput &input) {
	const auto micros = input.GetContext().GetTransactionStartTimestamp().micros;
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int64_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = micros;
	}
}

} // namespace

TEST_CASE("Stable C++API: collation registers and compares through the sort key", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto function = MakeLowerKey(conn, "nocase_key");
	conn.Register(function);
	RegisterCollation(conn, "nocase_test", "nocase_key");

	REQUIRE(CollectBigints(conn.Execute("SELECT count(*) FROM (VALUES (1)) t(i) WHERE 'ABC' = 'abc' COLLATE "
	                                    "nocase_test")) == std::vector<int64_t> {1});
	REQUIRE(CollectBigints(conn.Execute("SELECT count(*) FROM (VALUES ('ABC'), ('abc'), ('Abc')) t(s) "
	                                    "WHERE s = 'aBc' COLLATE nocase_test")) == std::vector<int64_t> {3});
	// Without the collation, binary comparison applies
	REQUIRE(CollectBigints(conn.Execute("SELECT count(*) FROM (VALUES ('ABC'), ('abc'), ('Abc')) t(s) "
	                                    "WHERE s = 'aBc'")) == std::vector<int64_t> {0});
	REQUIRE(CollectStrings(conn.Execute("SELECT s FROM (VALUES ('B'), ('a'), ('C')) t(s) "
	                                    "ORDER BY s COLLATE nocase_test")) == std::vector<std::string> {"a", "B", "C"});
	REQUIRE(CollectStrings(conn.Execute("SELECT s FROM (VALUES ('B'), ('a'), ('C')) t(s) ORDER BY s")) ==
	        std::vector<std::string> {"B", "C", "a"});

	// The same function can back another collation, with other options
	RegisterCollation(conn, "nocase_test_2", "nocase_key", false, true);
	REQUIRE(CollectStrings(conn.Execute("SELECT s FROM (VALUES ('B'), ('a'), ('C')) t(s) "
	                                    "ORDER BY s COLLATE nocase_test_2")) ==
	        std::vector<std::string> {"a", "B", "C"});
}

TEST_CASE("Stable C++API: collation info stays valid after registration", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto function = MakeLowerKey(conn, "nocase_key");
	conn.Register(function);
	auto collation = CollationInfo::Create(conn.GetFactory());
	collation.SetName("nocase_a").SetFunctionName("nocase_key");
	conn.Register(collation);
	collation.SetName("nocase_b");
	conn.Register(collation);
	REQUIRE(CollectStrings(conn.Execute("SELECT s FROM (VALUES ('B'), ('a')) t(s) ORDER BY s COLLATE nocase_a")) ==
	        std::vector<std::string> {"a", "B"});
	REQUIRE(CollectStrings(conn.Execute("SELECT s FROM (VALUES ('B'), ('a')) t(s) ORDER BY s COLLATE nocase_b")) ==
	        std::vector<std::string> {"a", "B"});
}

TEST_CASE("Stable C++API: collation function may return BLOB", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto function = MakeLowerKey(conn, "blob_key", "BLOB");
	conn.Register(function);
	RegisterCollation(conn, "nocase_blob", "blob_key");
	REQUIRE(CollectStrings(conn.Execute("SELECT s FROM (VALUES ('B'), ('a'), ('C')) t(s) "
	                                    "ORDER BY s COLLATE nocase_blob")) == std::vector<std::string> {"a", "B", "C"});
}

TEST_CASE("Stable C++API: collation picks the VARCHAR overload of the function", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	// An INTEGER overload registered first, then the VARCHAR one under the same name
	auto int_overload = ScalarFunction::Create(factory);
	int_overload.SetName("overloaded_key").SetExecCallback(LowerExec);
	int_overload.GetSignature()
	    .AddParameter("a", factory.ParseType("INTEGER"))
	    .SetReturnType(factory.ParseType("VARCHAR"));
	conn.Register(int_overload);
	auto function = MakeLowerKey(conn, "overloaded_key");
	REQUIRE_NOTHROW(conn.Register(function));
	RegisterCollation(conn, "nocase_overloaded", "overloaded_key");
	REQUIRE(CollectStrings(conn.Execute("SELECT s FROM (VALUES ('B'), ('a')) t(s) "
	                                    "ORDER BY s COLLATE nocase_overloaded")) ==
	        std::vector<std::string> {"a", "B"});
}

TEST_CASE("Stable C++API: collation registration validates the function", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	SECTION("missing name") {
		auto collation = CollationInfo::Create(factory);
		collation.SetFunctionName("lower");
		REQUIRE_THROWS_MATCHES(conn.Register(collation), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	SECTION("missing function name") {
		auto collation = CollationInfo::Create(factory);
		collation.SetName("bad_collation");
		REQUIRE_THROWS_MATCHES(conn.Register(collation), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	SECTION("unknown function") {
		REQUIRE_THROWS_MATCHES(RegisterCollation(conn, "bad_collation", "no_such_function"), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	SECTION("wrong parameter count") {
		auto function = ScalarFunction::Create(factory);
		function.SetName("two_args").SetExecCallback(LowerExec);
		function.GetSignature()
		    .AddParameter("a", factory.ParseType("VARCHAR"))
		    .AddParameter("b", factory.ParseType("VARCHAR"))
		    .SetReturnType(factory.ParseType("VARCHAR"));
		conn.Register(function);
		REQUIRE_THROWS_MATCHES(RegisterCollation(conn, "bad_collation", "two_args"), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	SECTION("wrong parameter type") {
		auto function = ScalarFunction::Create(factory);
		function.SetName("int_arg").SetExecCallback(LowerExec);
		function.GetSignature()
		    .AddParameter("a", factory.ParseType("INTEGER"))
		    .SetReturnType(factory.ParseType("VARCHAR"));
		conn.Register(function);
		REQUIRE_THROWS_MATCHES(RegisterCollation(conn, "bad_collation", "int_arg"), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	SECTION("wrong return type") {
		auto function = MakeLowerKey(conn, "int_key", "INTEGER");
		conn.Register(function);
		REQUIRE_THROWS_MATCHES(RegisterCollation(conn, "bad_collation", "int_key"), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	REQUIRE_THROWS_AS(conn.Execute("SELECT 'a' COLLATE bad_collation").Drain(), Exception);
}

TEST_CASE("Stable C++API: context reports the transaction start timestamp", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	auto function = ScalarFunction::Create(factory);
	function.SetName("txn_start_micros").SetExecCallback(TxnStartExec);
	function.GetSignature().SetReturnType(factory.ParseType("BIGINT"));
	conn.Register(function);

	const auto first = CollectBigints(conn.Execute("SELECT txn_start_micros()"));
	REQUIRE(first.size() == 1);
	// After 2020-01-01
	REQUIRE(first[0] > 1577836800LL * 1000000);

	conn.Execute("BEGIN").Drain();
	const auto in_txn_a = CollectBigints(conn.Execute("SELECT txn_start_micros()"));
	conn.Execute("SELECT count(*) FROM range(100000)").Drain();
	const auto in_txn_b = CollectBigints(conn.Execute("SELECT txn_start_micros()"));
	conn.Execute("COMMIT").Drain();
	REQUIRE(in_txn_a == in_txn_b);
	REQUIRE(in_txn_a[0] >= first[0]);

	// A new transaction starts later
	const auto after = CollectBigints(conn.Execute("SELECT txn_start_micros()"));
	REQUIRE(after[0] > in_txn_a[0]);
}
