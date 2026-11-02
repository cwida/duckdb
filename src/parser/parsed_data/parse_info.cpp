#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

string ParseInfo::TypeToString(CatalogType type) {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
		return "TABLE";
	case CatalogType::SCALAR_FUNCTION_ENTRY:
		return "FUNCTION";
	case CatalogType::INDEX_ENTRY:
		return "INDEX";
	case CatalogType::SCHEMA_ENTRY:
		return "SCHEMA";
	case CatalogType::TYPE_ENTRY:
		return "TYPE";
	case CatalogType::VIEW_ENTRY:
		return "VIEW";
	case CatalogType::SEQUENCE_ENTRY:
		return "SEQUENCE";
	case CatalogType::MACRO_ENTRY:
		return "MACRO";
	case CatalogType::TABLE_MACRO_ENTRY:
		return "MACRO TABLE";
	case CatalogType::SECRET_ENTRY:
		return "SECRET";
	case CatalogType::TRIGGER_ENTRY:
		return "TRIGGER";
	default:
		throw InternalException("ParseInfo::TypeToString for CatalogType with type: %s not implemented",
		                        EnumUtil::ToString(type));
	}
}

string RenderOptionList(const case_insensitive_map_t<unique_ptr<ParsedExpression>> &parsed_options,
                        const unordered_map<string, Value> &options) {
	if (parsed_options.empty() && options.empty()) {
		return string();
	}
	vector<string> stringified;
	for (auto &opt : parsed_options) {
		stringified.push_back(StringUtil::Format("%s %s", SQLIdentifier::ToString(opt.first), opt.second->ToString()));
	}
	for (auto &opt : options) {
		stringified.push_back(
		    StringUtil::Format("%s %s", SQLIdentifier::ToString(opt.first), opt.second.ToSQLString()));
	}
	return " (" + StringUtil::Join(stringified, ", ") + ")";
}

} // namespace duckdb
