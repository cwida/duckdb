#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/function/language_macro_function.hpp"
#include "duckdb/main/language_function_binder.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/parsed_data/create_language_info.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

// These tests exercise the C++ extension contract around ValidateCreate and internal entries. SQL
// cannot mutate the CreateMacroInfo handed to a language binder, register a language binder, or
// create an internal macro entry, so the setup must be C++. The SQL-observable behavior of valid
// definitions stays in test/sql/function/language_function.

namespace {

//! A binder whose ValidateCreate performs a configurable mutation before core re-checks the definition
class MutatingBinder : public LanguageFunctionBinder {
public:
	explicit MutatingBinder(std::function<void(CreateMacroInfo &)> mutation_p) : mutation(std::move(mutation_p)) {
	}

	void ValidateCreate(ClientContext &context, CreateMacroInfo &info) override {
		if (mutation) {
			mutation(info);
		}
	}
	BindResult BindScalarCall(ExpressionBinder &binder, ScalarMacroCatalogEntry &entry, LanguageMacroFunction &member,
	                          vector<unique_ptr<ParsedExpression>> arguments, idx_t depth) override {
		// the compiled form is the constant the source denotes, like the testlang demo
		return BindResult(make_uniq<BoundConstantExpression>(Value(member.source)));
	}

private:
	std::function<void(CreateMacroInfo &)> mutation;
};

//! Register a language the way ExtensionLoader::RegisterLanguage does, without an extension
static void RegisterTestLanguage(DatabaseInstance &db, const string &language,
                                 shared_ptr<LanguageFunctionBinder> binder) {
	CreateLanguageInfo info((Identifier(language)));
	info.binder = std::move(binder);
	info.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
	auto &system_catalog = Catalog::GetSystemCatalog(db);
	auto data = CatalogTransaction::GetSystemTransaction(db);
	auto &schema = system_catalog.GetSchema(data, Identifier::DefaultSchema()).Cast<DuckSchemaEntry>();
	schema.CreateLanguage(data, info);
}

} // namespace

TEST_CASE("ValidateCreate guard accepts a source-only rewrite", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterTestLanguage(*db.instance, "rewritelang", make_shared_ptr<MutatingBinder>([](CreateMacroInfo &info) {
		info.macros[0]->Cast<LanguageMacroFunction>().source = "rewritten";
	}));

	REQUIRE_NO_FAIL(con.Query("CREATE FUNCTION rewritten_fun() LANGUAGE rewritelang AS 'original'"));
	// the rewritten source is what got stored and what the call compiles to
	auto result = con.Query("SELECT rewritten_fun()");
	REQUIRE(CHECK_COLUMN(result, 0, {"rewritten"}));
	result = con.Query("SELECT macro_definition FROM duckdb_functions() WHERE function_name = 'rewritten_fun'");
	REQUIRE(CHECK_COLUMN(result, 0, {"rewritten"}));
}

TEST_CASE("ValidateCreate guard rejects a return type mutation", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterTestLanguage(*db.instance, "retlang", make_shared_ptr<MutatingBinder>([](CreateMacroInfo &info) {
		info.macros[0]->return_type = LogicalType::INTEGER;
	}));

	auto result = con.Query("CREATE FUNCTION mutated_fun() LANGUAGE retlang AS 'x'");
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "changed the member count, a resolved signature"));
}

TEST_CASE("ValidateCreate guard rejects a semantic alias mutation on a default's type arguments", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	// swap the DECIMAL(4,2) modifiers into named form (scale := 4, width := 2): expression equality
	// ignores aliases, but binding consumes them as named constructor arguments
	RegisterTestLanguage(*db.instance, "guardlang", make_shared_ptr<MutatingBinder>([](CreateMacroInfo &info) {
		auto &default_expr = *info.macros[0]->default_parameters[Identifier("x")];
		auto &args = default_expr.Cast<CastExpression>().TargetTypeMutable().GetChildren();
		REQUIRE(args.size() == 2);
		args[0]->SetAlias(Identifier("scale"));
		args[1]->SetAlias(Identifier("width"));
	}));

	auto result = con.Query("CREATE FUNCTION guarded(x := NULL::DECIMAL(4,2)) LANGUAGE guardlang AS 'arg:0'");
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "default parameters"));
}

TEST_CASE("Member mutations respect internal entry protection", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterTestLanguage(*db.instance, "intlang", make_shared_ptr<MutatingBinder>(nullptr));

	// register an internal two-member language entry in the system catalog, the way an extension
	// registering functions does
	CreateMacroInfo info(CatalogType::MACRO_ENTRY);
	info.SetFunctionName(Identifier("internal_fun"));
	info.internal = true;
	auto int_member = make_uniq<LanguageMacroFunction>("intlang", "int member");
	int_member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("x")));
	int_member->types.push_back(LogicalType::INTEGER);
	info.macros.push_back(std::move(int_member));
	auto varchar_member = make_uniq<LanguageMacroFunction>("intlang", "varchar member");
	varchar_member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("x")));
	varchar_member->types.push_back(LogicalType::VARCHAR);
	info.macros.push_back(std::move(varchar_member));
	auto &system_catalog = Catalog::GetSystemCatalog(*db.instance);
	auto data = CatalogTransaction::GetSystemTransaction(*db.instance);
	auto &schema = system_catalog.GetSchema(data, Identifier::DefaultSchema());
	schema.CreateFunction(data, info);

	// member-granular drop from SQL is blocked by the generic internal-entry drop protection
	auto result = con.Query("DROP FUNCTION internal_fun(INTEGER)");
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "internal"));

	// a CREATE from SQL resolves into the user catalog and creates a shadowing entry there, it
	// never merges into the system entry
	REQUIRE_NO_FAIL(con.Query("CREATE FUNCTION internal_fun(x DATE) LANGUAGE intlang AS 'date member'"));
	result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'internal_fun' AND "
	                   "database_name = 'memory'");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE_NO_FAIL(con.Query("DROP FUNCTION internal_fun"));

	// reaching the schema entry directly with a non-internal overload add is rejected: the member
	// rewrite is a drop+create and the internal protection applies to it
	CreateMacroInfo add_info(CatalogType::MACRO_ENTRY);
	add_info.SetFunctionName(Identifier("internal_fun"));
	auto date_member = make_uniq<LanguageMacroFunction>("intlang", "date member");
	date_member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("x")));
	date_member->types.push_back(LogicalType::DATE);
	add_info.macros.push_back(std::move(date_member));
	auto add_data = CatalogTransaction::GetSystemTransaction(*db.instance);
	REQUIRE_THROWS(schema.CreateFunction(add_data, add_info));

	// the entry is untouched and still callable
	result = con.Query("SELECT internal_fun(1), internal_fun('a'::VARCHAR)");
	REQUIRE(CHECK_COLUMN(result, 0, {"int member"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"varchar member"}));
}
