#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/parse_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/extension_callback.hpp"

using namespace duckdb;

namespace {
struct ErrorObserver : ExtensionCallback {
	idx_t calls = 0;
	string query;
	ErrorData original;
	ParserErrorAnnotation annotation;
	ExceptionType failure = ExceptionType::INVALID;

	ParserErrorAnnotation OnParserError(const ClientContext &, const string &sql, const ErrorData &error) override {
		calls++;
		query = sql;
		original = error;
		if (failure != ExceptionType::INVALID) {
			throw Exception(failure, "annotation failed");
		}
		return annotation;
	}
};

static ErrorData ParseError(Connection &con, const string &sql, const string &api) {
	if (api == "query") {
		return con.Query(sql)->GetErrorObject();
	}
	if (api == "prepare") {
		return con.Prepare(sql)->GetErrorObject();
	}
	if (api == "submit") {
		return con.Submit(sql)->GetErrorObject();
	}
	try {
		con.ExtractStatements(sql);
	} catch (const std::exception &ex) {
		return ErrorData(ex);
	}
	throw InternalException("Expected a parser error");
}
} // namespace

TEST_CASE("Parser error annotations preserve native diagnostics across client APIs", "[api][parser_error_annotation]") {
	for (const auto &api : {"query", "prepare", "submit", "extract"}) {
		DuckDB db(nullptr);
		Connection con(db);
		auto observer = make_shared_ptr<ErrorObserver>();
		ExtensionCallback::Register(DBConfig::GetConfig(*db.instance), observer);
		REQUIRE_NO_FAIL(con.Query("SET errors_as_json=true"));
		observer->annotation.hint = "Accepted call: example(stop => expression)";
		observer->annotation.details = {{"extension.example.signatures", "example(stop => expression)"},
		                                {"position", "0"},
		                                {"location", "[0,99]"},
		                                {"error_subtype", "changed"},
		                                {"exception_type", "Binder"},
		                                {"exception_message", "changed"},
		                                {"stack_trace", "changed"},
		                                {"extension.", "empty namespace"}};
		for (const auto &sql : {string("SELECT * FROM example(stop => )"), string("SELECT '") + char(0xff) + "'",
		                        string("SELECT 'unterminated"), string("SELECT 'é';\xC2\xA0SELECT )")}) {
			CAPTURE(api, sql);
			observer->calls = 0;
			auto error = ParseError(con, sql, api);
			REQUIRE(error.Type() == ExceptionType::PARSER);
			REQUIRE(observer->calls == 1);
			REQUIRE(observer->query.size() == sql.size());
			if (StringUtil::Contains(sql, "é")) {
				REQUIRE(observer->original.ExtraInfo().at("position") == std::to_string(sql.find(')')));
				REQUIRE(observer->query != sql);
			}
			auto original = observer->original;
			original.FinalizeError();
			error.FinalizeError();
			for (const auto &entry : original.ExtraInfo()) {
				REQUIRE(error.ExtraInfo().at(entry.first) == entry.second);
			}
			REQUIRE(error.ExtraInfo().size() == original.ExtraInfo().size() + 1);
			REQUIRE(error.ExtraInfo().at("extension.example.signatures") == "example(stop => expression)");
			con.context->ProcessError(error, sql);
			auto json = StringUtil::ParseJSONMap(error.RawMessage());
			REQUIRE(json.at("exception_type") == "Parser");
			REQUIRE(json.at("exception_message") == observer->original.RawMessage() + "\n" + observer->annotation.hint);
			REQUIRE(observer->calls == 1);
		}
	}
}

TEST_CASE("Parser error annotations compose without changing later callbacks' input",
          "[api][parser_error_annotation]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto first = make_shared_ptr<ErrorObserver>();
	auto second = make_shared_ptr<ErrorObserver>();
	ExtensionCallback::Register(DBConfig::GetConfig(*db.instance), first);
	ExtensionCallback::Register(DBConfig::GetConfig(*db.instance), second);
	const string sql = "SELECT )";
	auto unchanged = con.Query(sql)->GetErrorObject();
	auto expected = first->original;
	con.context->ProcessError(expected, sql);
	REQUIRE(unchanged.Message() == expected.Message());
	first->annotation = {"First hint", {{"extension.example.key", "first"}}};
	second->annotation = {"Second hint", {{"extension.example.key", "second"}}};
	auto annotated = con.Query(sql)->GetErrorObject();
	expected = first->original;
	con.context->ProcessError(expected, sql);
	REQUIRE(annotated.ExtraInfo().at("extension.example.key") == "first");
	REQUIRE(StringUtil::Contains(annotated.Message(), "First hint\nSecond hint"));
	REQUIRE(annotated.Message().substr(annotated.Message().find("LINE 1")) ==
	        expected.Message().substr(expected.Message().find("LINE 1")));
	REQUIRE(first->original.RawMessage() == second->original.RawMessage());
	REQUIRE(first->original.ExtraInfo() == second->original.ExtraInfo());
	first->failure = ExceptionType::INVALID_INPUT;
	annotated = con.Query(sql)->GetErrorObject();
	REQUIRE(annotated.ExtraInfo().at("extension.example.key") == "second");
	REQUIRE_FALSE(StringUtil::Contains(annotated.Message(), "First hint"));
	second->annotation = {};
	first->annotation = {"", {{"position", "0"}}};
	for (auto failure : {ExceptionType::INVALID_INPUT, ExceptionType::INVALID}) {
		first->failure = failure;
		unchanged = con.Query(sql)->GetErrorObject();
		expected = first->original;
		con.context->ProcessError(expected, sql);
		REQUIRE(unchanged.Message() == expected.Message());
		REQUIRE(unchanged.ExtraInfo() == expected.ExtraInfo());
	}
	first->calls = 0;
	Parser parser(*con.context);
	REQUIRE_THROWS(parser.ParseQuery(sql));
	REQUIRE_FAIL(con.Query("SELECT * FROM query('SELECT )')"));
	REQUIRE_FAIL(con.Query("SELECT missing_column"));
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
	REQUIRE(first->calls == 0);
}

TEST_CASE("Parser error annotations preserve invalidation policies and severe failures",
          "[api][parser_error_annotation]") {
	for (const auto &api : {"query", "prepare", "submit", "extract"}) {
		for (auto failure :
		     {ExceptionType::INVALID, ExceptionType::INVALID_INPUT, ExceptionType::INTERNAL, ExceptionType::FATAL}) {
			CAPTURE(api, failure);
			DuckDB db(nullptr);
			Connection con(db);
			auto observer = make_shared_ptr<ErrorObserver>();
			ExtensionCallback::Register(DBConfig::GetConfig(*db.instance), observer);
			observer->annotation.hint = "Additional guidance";
			observer->failure = failure;
			REQUIRE_NO_FAIL(
			    con.Query("SET default_transaction_invalidation_policy='SYNTACTIC_ERRORS_DO_NOT_INVALIDATE'"));
			REQUIRE_NO_FAIL(con.Query("BEGIN"));
			auto error = ParseError(con, "SELECT )", api);
			const bool severe = failure == ExceptionType::INTERNAL || failure == ExceptionType::FATAL;
			REQUIRE(error.Type() == (severe ? failure : ExceptionType::PARSER));
			REQUIRE(con.Query("SELECT 42")->HasError() == severe);
			Connection other(db);
			REQUIRE(other.Query("SELECT 42")->HasError() == (failure == ExceptionType::FATAL));
		}
	}
	DuckDB db(nullptr);
	Connection con(db);
	auto observer = make_shared_ptr<ErrorObserver>();
	ExtensionCallback::Register(DBConfig::GetConfig(*db.instance), observer);
	observer->annotation.hint = "Additional guidance";
	REQUIRE_NO_FAIL(con.Query("SET default_transaction_invalidation_policy='STANDARD_POLICY'"));
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	ParseIterator iterator(*con.context, "SELECT )");
	REQUIRE_THROWS_AS(iterator.Peek(), ParserException);
	REQUIRE_FALSE(iterator.Peek());
	REQUIRE(observer->calls == 1);
	REQUIRE_FAIL(con.Query("SELECT 42"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
	REQUIRE_NO_FAIL(con.Query("BEGIN"));
	REQUIRE_THROWS(con.ExtractStatements("SELECT )", false));
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
	REQUIRE_NO_FAIL(con.Query("ROLLBACK"));
}
