#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/function/language_macro_function.hpp"
#include "duckdb/main/language_function_binder.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_data/alter_macro_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/extra_drop_info.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
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

//! A binder whose ValidateCreate performs a configurable action through the narrow create view
class ViewBinder : public LanguageFunctionBinder {
public:
	explicit ViewBinder(std::function<void(LanguageFunctionCreateInfo &)> action_p) : action(std::move(action_p)) {
	}

	void ValidateCreate(ClientContext &context, LanguageFunctionCreateInfo &info) override {
		if (action) {
			action(info);
		}
	}
	BindResult BindScalarCall(ExpressionBinder &binder, ScalarMacroCatalogEntry &entry, LanguageMacroFunction &member,
	                          vector<unique_ptr<ParsedExpression>> arguments, idx_t depth) override {
		// the compiled form is the constant the source denotes, like the testlang demo
		return BindResult(make_uniq<BoundConstantExpression>(Value(member.source)));
	}

private:
	std::function<void(LanguageFunctionCreateInfo &)> action;
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

TEST_CASE("ValidateCreate rewrites a member source through the create view", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterTestLanguage(*db.instance, "rewritelang", make_shared_ptr<ViewBinder>([](LanguageFunctionCreateInfo &info) {
		info.SetMemberSource(0, "rewritten");
	}));

	REQUIRE_NO_FAIL(con.Query("CREATE FUNCTION rewritten_fun() LANGUAGE rewritelang AS 'original'"));
	// the rewritten source is what got stored and what the call compiles to
	auto result = con.Query("SELECT rewritten_fun()");
	REQUIRE(CHECK_COLUMN(result, 0, {"rewritten"}));
	result = con.Query("SELECT macro_definition FROM duckdb_functions() WHERE function_name = 'rewritten_fun'");
	REQUIRE(CHECK_COLUMN(result, 0, {"rewritten"}));
}

TEST_CASE("ValidateCreate reads members and rejects definitions through the create view", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterTestLanguage(*db.instance, "readlang", make_shared_ptr<ViewBinder>([](LanguageFunctionCreateInfo &info) {
		REQUIRE(info.Members().size() == 1);
		auto &member = info.Members()[0]->Cast<LanguageMacroFunction>();
		if (StringUtil::Contains(member.source, "reject")) {
			throw BinderException("readlang: rejected %s", info.GetFunctionName().GetIdentifierName());
		}
	}));

	REQUIRE_NO_FAIL(con.Query("CREATE FUNCTION accepted_fun(x INTEGER) LANGUAGE readlang AS 'fine'"));
	auto result = con.Query("CREATE FUNCTION rejected_fun() LANGUAGE readlang AS 'reject me'");
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "readlang: rejected rejected_fun"));
}

TEST_CASE("ValidateCreate publishes documentation through the create view", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterTestLanguage(*db.instance, "doclang", make_shared_ptr<ViewBinder>([](LanguageFunctionCreateInfo &info) {
		for (auto &member : info.Members()) {
			FunctionDescription description;
			for (idx_t i = 0; i < member->parameters.size(); i++) {
				auto &param_name = member->parameters[i]->Cast<ColumnRefExpression>().GetColumnName();
				description.parameter_names.push_back(param_name.GetIdentifierName());
				const auto &type = member->types[i];
				description.parameter_types.push_back(type.id() == LogicalTypeId::UNKNOWN ? LogicalType::ANY : type);
			}
			description.description = "doclang doc";
			info.AddDescription(std::move(description));
		}
	}));

	REQUIRE_NO_FAIL(con.Query("CREATE FUNCTION documented_fun(x INTEGER) LANGUAGE doclang AS 'd'"));
	auto result = con.Query("SELECT description FROM duckdb_functions() WHERE function_name = 'documented_fun'");
	REQUIRE(CHECK_COLUMN(result, 0, {"doclang doc"}));
}

TEST_CASE("Member mutations respect internal entry protection", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterTestLanguage(*db.instance, "intlang", make_shared_ptr<ViewBinder>(nullptr));

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

	// the drop guard directly: a member drop without authority is rejected ...
	AlterEntryData drop_data(QualifiedName(Identifier(), Identifier::DefaultSchema(), Identifier("internal_fun")),
	                         OnEntryNotFound::THROW_EXCEPTION);
	RemoveMacroOverloadInfo drop_info(drop_data, CatalogType::MACRO_ENTRY, {LogicalType::INTEGER});
	con.BeginTransaction();
	bool threw = false;
	try {
		schema.Alter(schema.GetCatalogTransaction(*con.context), drop_info);
	} catch (std::exception &ex) {
		threw = StringUtil::Contains(ex.what(), "internal system entry");
	}
	REQUIRE(threw);
	con.Rollback();

	// an extension registering internal members one at a time still merges: the internal info
	// carries the authority the guard checks
	CreateMacroInfo readd_info(CatalogType::MACRO_ENTRY);
	readd_info.SetFunctionName(Identifier("internal_fun"));
	readd_info.internal = true;
	auto readd_member = make_uniq<LanguageMacroFunction>("intlang", "date member");
	readd_member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("x")));
	readd_member->types.push_back(LogicalType::DATE);
	readd_info.macros.push_back(std::move(readd_member));
	auto readd_data = CatalogTransaction::GetSystemTransaction(*db.instance);
	schema.CreateFunction(readd_data, readd_info);
	result = con.Query("SELECT internal_fun(DATE '2020-01-01'), internal_fun('a'::VARCHAR)");
	REQUIRE(CHECK_COLUMN(result, 0, {"date member"}));
	REQUIRE(CHECK_COLUMN(result, 1, {"varchar member"}));
}

static LogicalType UnresolvedUserType(const string &name) {
	vector<unique_ptr<ParsedExpression>> no_children;
	return LogicalType::UNBOUND(make_uniq<TypeExpression>(Identifier(name), std::move(no_children)));
}

TEST_CASE("Nested unbound member types resolve at CREATE", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterTestLanguage(*db.instance, "nestlang", make_shared_ptr<ViewBinder>(nullptr));
	REQUIRE_NO_FAIL(con.Query("CREATE TYPE mood AS ENUM ('happy', 'sad')"));

	// a dialect transformer may emit a concrete container around an unresolved user type, unlike
	// the stock grammar, which wraps the whole unresolved type at the top level
	auto info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	info->SetFunctionName(Identifier("nested_fun"));
	auto list_member = make_uniq<LanguageMacroFunction>("nestlang", "arg:0");
	list_member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("x")));
	list_member->types.push_back(LogicalType::LIST(UnresolvedUserType("mood")));
	list_member->return_type = LogicalType::LIST(UnresolvedUserType("mood"));
	info->macros.push_back(std::move(list_member));
	auto struct_member = make_uniq<LanguageMacroFunction>("nestlang", "struct member");
	struct_member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("x")));
	struct_member->types.push_back(LogicalType::STRUCT({{"m", UnresolvedUserType("mood")}}));
	info->macros.push_back(std::move(struct_member));
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	unique_ptr<SQLStatement> sql_statement = std::move(statement);
	auto result = con.Query(std::move(sql_statement));
	REQUIRE_NO_FAIL(*result);

	// the stored signatures are fully resolved: overload matching selects by the nested user type,
	// the RETURNS cast applies, and introspection shows resolved types
	auto call = con.Query("SELECT typeof(nested_fun(['happy'::mood])), nested_fun({'m': 'happy'::mood})");
	REQUIRE(CHECK_COLUMN(call, 0, {"ENUM('happy', 'sad')[]"}));
	REQUIRE(CHECK_COLUMN(call, 1, {"struct member"}));
	auto types = con.Query("SELECT bool_and(parameter_types[1] NOT LIKE '%mood%') "
	                       "FROM duckdb_functions() WHERE function_name = 'nested_fun'");
	REQUIRE(CHECK_COLUMN(types, 0, {true}));

	// bound sibling subtrees keep their aliases: two members whose aliased payloads differ must
	// stay distinct signatures when the unresolved sibling is bound
	auto alias_info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	alias_info->SetFunctionName(Identifier("aliased_sibling"));
	for (auto payload_alias : {"payload_a", "payload_b"}) {
		auto member = make_uniq<LanguageMacroFunction>("nestlang", payload_alias);
		member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("x")));
		child_list_t<LogicalType> fields;
		fields.emplace_back("payload", LogicalType::LIST(LogicalType::INTEGER).WithAlias(payload_alias));
		fields.emplace_back("m", UnresolvedUserType("mood"));
		member->types.push_back(LogicalType::STRUCT(std::move(fields)));
		alias_info->macros.push_back(std::move(member));
	}
	auto alias_statement = make_uniq<CreateStatement>();
	alias_statement->info = std::move(alias_info);
	unique_ptr<SQLStatement> alias_sql = std::move(alias_statement);
	auto alias_result = con.Query(std::move(alias_sql));
	REQUIRE_NO_FAIL(*alias_result);
	auto alias_types = con.Query("SELECT count(*), bool_and(parameter_types[1] LIKE '%payload_a%' OR "
	                             "parameter_types[1] LIKE '%payload_b%') "
	                             "FROM duckdb_functions() WHERE function_name = 'aliased_sibling'");
	REQUIRE(CHECK_COLUMN(alias_types, 0, {2}));
	REQUIRE(CHECK_COLUMN(alias_types, 1, {true}));
}

TEST_CASE("Batch member creation follows the overload identity", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	RegisterTestLanguage(*db.instance, "batchlang", make_shared_ptr<ViewBinder>(nullptr));

	// a dialect may submit several members in one CREATE: the set must satisfy the same conflict
	// identity as incremental creation, or EXPORT emits a set IMPORT cannot re-create
	auto make_member = [](const string &source, bool with_default) {
		auto member = make_uniq<LanguageMacroFunction>("batchlang", source);
		member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("a")));
		member->types.push_back(LogicalType::INTEGER);
		if (with_default) {
			auto default_expr = make_uniq<ConstantExpression>(Literal(LiteralKind::INTEGER, "5"));
			default_expr->SetAlias(Identifier("c"));
			member->default_parameters[Identifier("c")] = std::move(default_expr);
			member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("c")));
			member->types.push_back(LogicalType::INTEGER);
		}
		return member;
	};

	auto info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	info->SetFunctionName(Identifier("batch_fun"));
	info->macros.push_back(make_member("fixed", false));
	info->macros.push_back(make_member("ranged", true));
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	unique_ptr<SQLStatement> sql_statement = std::move(statement);
	auto result = con.Query(std::move(sql_statement));
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "conflicting overload members"));

	// the identity is compared on the resolved signatures: an unresolved spelling of the same
	// type must not slip past the pairwise check
	auto spelled_info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	spelled_info->SetFunctionName(Identifier("batch_fun"));
	auto spelled_member = make_uniq<LanguageMacroFunction>("batchlang", "fixed");
	spelled_member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("a")));
	spelled_member->types.push_back(UnresolvedUserType("integer"));
	spelled_info->macros.push_back(std::move(spelled_member));
	spelled_info->macros.push_back(make_member("ranged", true));
	auto spelled_statement = make_uniq<CreateStatement>();
	spelled_statement->info = std::move(spelled_info);
	unique_ptr<SQLStatement> spelled_sql = std::move(spelled_statement);
	auto spelled_result = con.Query(std::move(spelled_sql));
	REQUIRE_FAIL(spelled_result);
	REQUIRE(StringUtil::Contains(spelled_result->GetError(), "conflicting overload members"));

	// a pairwise valid batch is accepted
	auto valid_info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	valid_info->SetFunctionName(Identifier("batch_fun"));
	valid_info->macros.push_back(make_member("fixed", false));
	auto varchar_member = make_uniq<LanguageMacroFunction>("batchlang", "varchar ranged");
	varchar_member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("a")));
	varchar_member->types.push_back(LogicalType::VARCHAR);
	auto default_expr = make_uniq<ConstantExpression>(Literal(LiteralKind::INTEGER, "5"));
	default_expr->SetAlias(Identifier("c"));
	varchar_member->default_parameters[Identifier("c")] = std::move(default_expr);
	varchar_member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("c")));
	varchar_member->types.push_back(LogicalType::INTEGER);
	valid_info->macros.push_back(std::move(varchar_member));
	auto valid_statement = make_uniq<CreateStatement>();
	valid_statement->info = std::move(valid_info);
	unique_ptr<SQLStatement> valid_sql = std::move(valid_statement);
	auto valid_result = con.Query(std::move(valid_sql));
	REQUIRE_NO_FAIL(*valid_result);
	auto count = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'batch_fun'");
	REQUIRE(CHECK_COLUMN(count, 0, {2}));
}
