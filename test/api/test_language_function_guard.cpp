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
// cannot drive the create view handed to a language binder, register a language binder, or
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
		REQUIRE(info.MemberCount() == 1);
		if (StringUtil::Contains(info.MemberSource(0), "reject")) {
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
		for (idx_t member_idx = 0; member_idx < info.MemberCount(); member_idx++) {
			FunctionDescription description;
			for (idx_t i = 0; i < info.MemberParameterCount(member_idx); i++) {
				description.parameter_names.push_back(info.MemberParameterName(member_idx, i));
				const auto &type = info.MemberParameterType(member_idx, i);
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

static unique_ptr<MacroFunction> DateMember() {
	auto member = make_uniq<LanguageMacroFunction>("intlang", "date member");
	member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("x")));
	member->types.push_back(LogicalType::DATE);
	return std::move(member);
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

	// member-granular drop from SQL is an alter, which the generic binder refuses for system catalog entries
	auto result = con.Query("DROP FUNCTION internal_fun(INTEGER)");
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "System Catalog"));

	// a CREATE from SQL resolves into the user catalog and creates a shadowing entry there, it
	// never merges into the system entry
	REQUIRE_NO_FAIL(con.Query("CREATE FUNCTION internal_fun(x DATE) LANGUAGE intlang AS 'date member'"));
	result = con.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'internal_fun' AND "
	                   "database_name = 'memory'");
	REQUIRE(CHECK_COLUMN(result, 0, {1}));
	REQUIRE_NO_FAIL(con.Query("DROP FUNCTION internal_fun"));

	// a direct CREATE on the schema never merges into the existing entry, even with internal authority
	CreateMacroInfo create_info(CatalogType::MACRO_ENTRY);
	create_info.SetFunctionName(Identifier("internal_fun"));
	create_info.internal = true;
	create_info.macros.push_back(DateMember());
	auto create_data = CatalogTransaction::GetSystemTransaction(*db.instance);
	REQUIRE_THROWS_WITH(schema.CreateFunction(create_data, create_info), Catch::Matchers::Contains("already exists"));

	// an overload add is an alter of the entry, and the internal protection applies to it
	AlterEntryData add_data(QualifiedName(Identifier(), Identifier::DefaultSchema(), Identifier("internal_fun")),
	                        OnEntryNotFound::THROW_EXCEPTION);
	AddMacroOverloadInfo add_info(add_data, DateMember(), OnCreateConflict::ERROR_ON_CONFLICT);
	auto add_transaction = CatalogTransaction::GetSystemTransaction(*db.instance);
	REQUIRE_THROWS_WITH(schema.Alter(add_transaction, add_info), Catch::Matchers::Contains("internal system entry"));

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

	// an extension registering internal members one at a time adds them as overload alters that carry
	// the authority the guard checks, like function overloads
	AddMacroOverloadInfo readd_info(add_data, DateMember(), OnCreateConflict::ERROR_ON_CONFLICT);
	readd_info.allow_internal = true;
	auto readd_transaction = CatalogTransaction::GetSystemTransaction(*db.instance);
	schema.Alter(readd_transaction, readd_info);
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

//! A zero-parameter language member whose undeclared RETURNS type carries extension metadata
static unique_ptr<LanguageMacroFunction> UndeclaredReturnsMetadataMember(const string &language) {
	auto ext_info = make_uniq<ExtensionTypeInfo>();
	ext_info->modifiers.emplace_back(Value::INTEGER(1));
	auto member = make_uniq<LanguageMacroFunction>(language, "x");
	member->return_type = LogicalType(LogicalTypeId::UNKNOWN).WithExtensionInfo(std::move(ext_info));
	return member;
}

TEST_CASE("Metadata on an undeclared RETURNS type is rejected at the catalog boundary", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	// an undeclared RETURNS type serializes as its bare default, so attached metadata would
	// silently vanish on reopen: the rejection sits at the catalog boundary every producer funnels
	// through, not only in the CREATE binder
	RegisterTestLanguage(*db.instance, "unkretlang", make_shared_ptr<ViewBinder>(nullptr));

	auto info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	info->SetFunctionName(Identifier("unk_ret_fun"));
	info->macros.push_back(UndeclaredReturnsMetadataMember("unkretlang"));
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	auto result = con.Query(std::move(statement));
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "metadata to an undeclared RETURNS type"));

	// the direct registration path an extension uses bypasses the CREATE binder and is rejected too
	CreateMacroInfo direct_info(CatalogType::MACRO_ENTRY);
	direct_info.SetFunctionName(Identifier("unk_ret_direct"));
	direct_info.macros.push_back(UndeclaredReturnsMetadataMember("unkretlang"));
	auto &system_catalog = Catalog::GetSystemCatalog(*db.instance);
	auto data = CatalogTransaction::GetSystemTransaction(*db.instance);
	auto &schema = system_catalog.GetSchema(data, Identifier::DefaultSchema());
	REQUIRE_THROWS(schema.CreateFunction(data, direct_info));
}

//! Build a type the way only the raw C++ constructor can: a parameterless (INVALID) info carrying
//! extension metadata, which LogicalType normalizes to the serializable GENERIC representation
static LogicalType RawConstructedType(LogicalTypeId id, int32_t modifier) {
	auto type_info = make_uniq<LogicalTypeInfo>(LogicalTypeInfoType::INVALID_TYPE_INFO);
	type_info->extension_info = make_uniq<ExtensionTypeInfo>();
	type_info->extension_info->modifiers.emplace_back(Value::INTEGER(modifier));
	return LogicalType(id, std::move(type_info));
}

TEST_CASE("A raw-constructed type representation stays visible to the identity freeze", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	// the raw type constructor used to accept a parameterless info carrying extension metadata,
	// a representation serialization drops entirely: the freeze fingerprints would have matched
	// before and after this mutation. The constructor now normalizes it to GENERIC info
	RegisterTestLanguage(*db.instance, "rawlang", make_shared_ptr<ViewBinder>([](LanguageFunctionCreateInfo &info) {
		auto parameter_type = info.MemberParameterType(0, 0);
		parameter_type.GetTypeInfo().extension_info->modifiers[0].value = Value::INTEGER(9);
	}));

	auto info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	info->SetFunctionName(Identifier("raw_mutated"));
	auto member = make_uniq<LanguageMacroFunction>("rawlang", "x");
	member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("p")));
	member->types.push_back(RawConstructedType(LogicalTypeId::INTEGER, 1));
	REQUIRE(member->types[0].GetTypeInfo().type == LogicalTypeInfoType::GENERIC_TYPE_INFO);
	info->macros.push_back(std::move(member));
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	auto result = con.Query(std::move(statement));
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "changed a resolved signature"));
	// a misbehaving binder fails its CREATE only, the database stays usable
	REQUIRE(result->GetErrorType() == ExceptionType::INVALID_INPUT);
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
	Connection other(db);
	REQUIRE_NO_FAIL(other.Query("SELECT 42"));
}

TEST_CASE("An ANY target type mutation through a returned type copy is rejected", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	// AnyTypeInfo's target is serialized but not visited by the general-purpose TypeVisitor: the
	// fingerprint must observe a mutation of shared metadata below it
	RegisterTestLanguage(*db.instance, "anymut", make_shared_ptr<ViewBinder>([](LanguageFunctionCreateInfo &info) {
		auto parameter_type = info.MemberParameterType(0, 0);
		auto target = AnyType::GetTargetType(parameter_type);
		target.GetTypeInfo().extension_info->modifiers[0].value = Value::INTEGER(9);
	}));

	auto info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	info->SetFunctionName(Identifier("any_mutated"));
	auto member = make_uniq<LanguageMacroFunction>("anymut", "x");
	member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("p")));
	member->types.push_back(LogicalType::ANY_PARAMS(RawConstructedType(LogicalTypeId::INTEGER, 1)));
	info->macros.push_back(std::move(member));
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	auto result = con.Query(std::move(statement));
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "changed a resolved signature"));
}

TEST_CASE("A modifier value type mutation through a returned type copy is rejected", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	// a modifier Value carries its own type, whose shared metadata is serialized with the Value:
	// the fingerprint must observe a mutation of metadata nested inside the extension info itself
	RegisterTestLanguage(*db.instance, "valmut", make_shared_ptr<ViewBinder>([](LanguageFunctionCreateInfo &info) {
		auto parameter_type = info.MemberParameterType(0, 0);
		auto &value_type = parameter_type.GetTypeInfo().extension_info->modifiers[0].value.type();
		value_type.GetTypeInfo().extension_info->modifiers[0].value = Value::INTEGER(9);
	}));

	auto ext_info = make_uniq<ExtensionTypeInfo>();
	ext_info->modifiers.emplace_back(Value(RawConstructedType(LogicalTypeId::INTEGER, 1)));
	auto info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	info->SetFunctionName(Identifier("val_mutated"));
	auto member = make_uniq<LanguageMacroFunction>("valmut", "x");
	member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("p")));
	member->types.push_back(LogicalType(LogicalTypeId::INTEGER).WithExtensionInfo(std::move(ext_info)));
	info->macros.push_back(std::move(member));
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	auto result = con.Query(std::move(statement));
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "changed a resolved signature"));
}

TEST_CASE("A resolved signature mutation through a returned type copy is rejected", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	// a returned parameter type shares its interned metadata, and extension_info stays a mutable
	// pointee behind the const LogicalTypeInfo: without the signature freeze this collapses two
	// distinct overload signatures after the batch conflict check already accepted them
	RegisterTestLanguage(*db.instance, "sigmut", make_shared_ptr<ViewBinder>([](LanguageFunctionCreateInfo &info) {
		auto parameter_type = info.MemberParameterType(0, 0);
		parameter_type.GetTypeInfo().extension_info->modifiers.emplace_back(Value::INTEGER(9));
	}));

	auto ext_info = make_uniq<ExtensionTypeInfo>();
	ext_info->modifiers.emplace_back(Value::INTEGER(1));
	auto info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	info->SetFunctionName(Identifier("sig_mutated"));
	auto member = make_uniq<LanguageMacroFunction>("sigmut", "x");
	member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("p")));
	member->types.push_back(LogicalType(LogicalTypeId::INTEGER).WithExtensionInfo(std::move(ext_info)));
	info->macros.push_back(std::move(member));
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	auto result = con.Query(std::move(statement));
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "changed a resolved signature"));
}

TEST_CASE("A RETURNS type mutation through a returned type copy is rejected", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	// the declared RETURNS type is a cast boundary core enforces on every call: a retained-metadata
	// mutation after validation would change the cast the batch check already accepted
	RegisterTestLanguage(*db.instance, "retmut", make_shared_ptr<ViewBinder>([](LanguageFunctionCreateInfo &info) {
		auto return_type = info.MemberReturnType(0);
		return_type.GetTypeInfo().extension_info->modifiers.emplace_back(Value::INTEGER(9));
	}));

	auto ext_info = make_uniq<ExtensionTypeInfo>();
	ext_info->modifiers.emplace_back(Value::INTEGER(1));
	auto info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	info->SetFunctionName(Identifier("ret_mutated"));
	auto member = make_uniq<LanguageMacroFunction>("retmut", "x");
	member->return_type = LogicalType(LogicalTypeId::INTEGER).WithExtensionInfo(std::move(ext_info));
	info->macros.push_back(std::move(member));
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	auto result = con.Query(std::move(statement));
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "changed a resolved signature"));
}

TEST_CASE("A batch overload collapse through a returned type copy is rejected", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	// the batch conflict check accepted two members distinct only in a modifier value: mutating the
	// second member's modifier to equal the first collapses the overload identities after the check
	RegisterTestLanguage(*db.instance, "collapselang",
	                     make_shared_ptr<ViewBinder>([](LanguageFunctionCreateInfo &info) {
		                     auto parameter_type = info.MemberParameterType(1, 0);
		                     parameter_type.GetTypeInfo().extension_info->modifiers[0].value = Value::INTEGER(1);
	                     }));

	auto info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	info->SetFunctionName(Identifier("collapsed_fun"));
	for (int32_t modifier = 1; modifier <= 2; modifier++) {
		auto ext_info = make_uniq<ExtensionTypeInfo>();
		ext_info->modifiers.emplace_back(Value::INTEGER(modifier));
		auto member = make_uniq<LanguageMacroFunction>("collapselang", "x");
		member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("p")));
		member->types.push_back(LogicalType(LogicalTypeId::INTEGER).WithExtensionInfo(std::move(ext_info)));
		info->macros.push_back(std::move(member));
	}
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	auto result = con.Query(std::move(statement));
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "changed a resolved signature"));
}

TEST_CASE("A nested type metadata mutation through a returned type copy is rejected", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	// a returned container type shares its children's interned metadata too: the serialized
	// identity must observe a mutation below the top level and past parameter index zero
	RegisterTestLanguage(*db.instance, "nestmut", make_shared_ptr<ViewBinder>([](LanguageFunctionCreateInfo &info) {
		auto parameter_type = info.MemberParameterType(0, 1);
		auto &child = StructType::GetChildTypes(parameter_type)[0].second;
		child.GetTypeInfo().extension_info->modifiers.emplace_back(Value::INTEGER(9));
	}));

	auto ext_info = make_uniq<ExtensionTypeInfo>();
	ext_info->modifiers.emplace_back(Value::INTEGER(1));
	auto inner = LogicalType(LogicalTypeId::INTEGER).WithExtensionInfo(std::move(ext_info));
	auto info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	info->SetFunctionName(Identifier("nest_mutated"));
	auto member = make_uniq<LanguageMacroFunction>("nestmut", "x");
	member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("a")));
	member->types.push_back(LogicalType::INTEGER);
	member->parameters.push_back(make_uniq<ColumnRefExpression>(Identifier("b")));
	member->types.push_back(LogicalType::STRUCT({{"i", inner}}));
	info->macros.push_back(std::move(member));
	auto statement = make_uniq<CreateStatement>();
	statement->info = std::move(info);
	auto result = con.Query(std::move(statement));
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->GetError(), "changed a resolved signature"));
}
