#include "duckdb/common/types/type_manager.hpp"

#include "duckdb/common/type_visitor.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

CastFunctionSet &TypeManager::GetCastFunctions() {
	return *cast_functions;
}

const CastFunctionSet &TypeManager::GetCastFunctions() const {
	return *cast_functions;
}

static LogicalType TransformStringToUnboundType(const string &str, ClientContext &context) {
	if (StringUtil::CIEquals(str, "null")) {
		return LogicalType::SQLNULL;
	}
	ColumnList column_list;
	try {
		column_list = Parser(context).ParseColumnList("dummy " + str);
	} catch (const ParserException &e) {
		const vector<string> suggested_types {"BIGINT",
		                                      "INT8",
		                                      "LONG",
		                                      "BIT",
		                                      "BITSTRING",
		                                      "BLOB",
		                                      "BYTEA",
		                                      "BINARY,",
		                                      "VARBINARY",
		                                      "BOOLEAN",
		                                      "BOOL",
		                                      "LOGICAL",
		                                      "DATE",
		                                      "DECIMAL(prec, scale)",
		                                      "DOUBLE",
		                                      "FLOAT8",
		                                      "FLOAT",
		                                      "FLOAT4",
		                                      "REAL",
		                                      "HUGEINT",
		                                      "INTEGER",
		                                      "INT4",
		                                      "INT",
		                                      "SIGNED",
		                                      "INTERVAL",
		                                      "SMALLINT",
		                                      "INT2",
		                                      "SHORT",
		                                      "TIME",
		                                      "TIMESTAMPTZ",
		                                      "TIMESTAMP",
		                                      "DATETIME",
		                                      "TINYINT",
		                                      "INT1",
		                                      "UBIGINT",
		                                      "UHUGEINT",
		                                      "UINTEGER",
		                                      "USMALLINT",
		                                      "UTINYINT",
		                                      "UUID",
		                                      "VARCHAR",
		                                      "CHAR",
		                                      "BPCHAR",
		                                      "TEXT",
		                                      "STRING",
		                                      "MAP(INTEGER, VARCHAR)",
		                                      "UNION(num INTEGER, text VARCHAR)"};
		std::ostringstream error;
		error << "Value \"" << str << "\" can not be converted to a DuckDB Type." << '\n';
		error << "Possible examples as suggestions: " << '\n';
		auto suggestions = StringUtil::TopNJaroWinkler(suggested_types, str);
		for (auto &suggestion : suggestions) {
			error << "* " << suggestion << '\n';
		}
		throw InvalidInputException(error.str());
	}
	return column_list.GetColumn(LogicalIndex(0)).Type();
}

// This has to be called with a level of indirection (through "parse_function") in order to avoid being included in
// extensions that statically link the core DuckDB library.
static LogicalType ParseLogicalTypeInternal(const string &type_str, ClientContext &context) {
	auto type = TransformStringToUnboundType(type_str, context);
	if (type.IsUnbound()) {
		if (!context.transaction.HasActiveTransaction()) {
			throw InternalException(
			    "Context does not have a transaction active, try running ClientContext::BindLogicalType instead");
		}
		auto binder = Binder::CreateBinder(context, nullptr);
		binder->BindLogicalType(type);
	}
	return type;
}

LogicalType TypeManager::ParseLogicalType(const string &type_str, ClientContext &context) const {
	return parse_function(type_str, context);
}

LogicalType TypeManager::ParseLogicalType(const string &type_str) const {
	auto type = UnboundType::TryParseAndDefaultBind(type_str);
	if (TypeVisitor::Contains(type, LogicalTypeId::UNBOUND)) {
		throw InvalidInputException("Type \"%s\" is not a built-in type", type_str);
	}
	return type;
}

TypeManager &TypeManager::Get(DatabaseInstance &db) {
	return DBConfig::GetConfig(db).GetTypeManager();
}

TypeManager &TypeManager::Get(ClientContext &context) {
	return DBConfig::GetConfig(context).GetTypeManager();
}

TypeManager::TypeManager(DBConfig &config_p) {
	cast_functions = make_uniq<CastFunctionSet>(config_p);
	parse_function = ParseLogicalTypeInternal;
}

TypeManager::~TypeManager() {
}

} // namespace duckdb
