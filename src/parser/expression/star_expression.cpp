#include "duckdb/parser/expression/star_expression.hpp"

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"

#include "duckdb/common/exception.hpp"

#include "duckdb/common/serializer/serializer.hpp"

namespace duckdb {

StarExpression::StarExpression(Identifier relation_name_p)
    : ParsedExpression(ExpressionType::STAR, ExpressionClass::STAR), relation_name(std::move(relation_name_p)) {
}

string StarExpression::ToString() const {
	string result;
	if (expr) {
		D_ASSERT(columns);
		result += "COLUMNS(" + expr->ToString() + ")";
		return result;
	}
	if (columns) {
		result += "COLUMNS(";
	}
	result += relation_name.empty() ? "*" : SQLIdentifier(relation_name) + ".*";
	if (!exclude_list.empty()) {
		result += " EXCLUDE (";
		result += StringUtil::Join(exclude_list, ", ", [](const auto &entry) { return entry.ToString(); });
		result += ")";
	}
	if (!replace_list.empty()) {
		result += " REPLACE (";
		result += StringUtil::Join(replace_list, ", ", [](const auto &entry) {
			return entry.second->ToString() + " AS " + SQLIdentifier(entry.first);
		});
		result += ")";
	}
	if (!rename_list.empty()) {
		result += " RENAME (";
		result += StringUtil::Join(rename_list, ", ", [](const auto &entry) {
			return entry.first.ToString() + " AS " + SQLIdentifier(entry.second);
		});
		result += ")";
	}
	if (columns) {
		result += ")";
	}
	return result;
}

bool StarExpression::IsStar(const ParsedExpression &a) {
	if (a.GetExpressionClass() != ExpressionClass::STAR) {
		return false;
	}
	auto &star = a.Cast<StarExpression>();
	return star.columns == false;
}

bool StarExpression::IsColumns(const ParsedExpression &a) {
	if (a.GetExpressionClass() != ExpressionClass::STAR) {
		return false;
	}
	auto &star = a.Cast<StarExpression>();
	return star.columns == true;
}

bool StarExpression::IsColumnsUnpacked(const ParsedExpression &a) {
	if (a.GetExpressionType() != ExpressionType::OPERATOR_UNPACK) {
		return false;
	}
	return true;
}

unique_ptr<ParsedExpression>
StarExpression::DeserializeStarExpression(Identifier &&relation_name, const identifier_set_t &exclude_list,
                                          identifier_map_t<unique_ptr<ParsedExpression>> &&replace_list, bool columns,
                                          unique_ptr<ParsedExpression> expr, bool unpacked,
                                          const qualified_column_set_t &qualified_exclude_list,
                                          qualified_column_map_t<Identifier> &&rename_list) {
	auto result = duckdb::unique_ptr<StarExpression>(new StarExpression(exclude_list, qualified_exclude_list));
	result->relation_name = std::move(relation_name);
	result->replace_list = std::move(replace_list);
	result->columns = columns;
	result->expr = std::move(expr);
	result->rename_list = std::move(rename_list);
	if (unpacked) {
		//! This was previously a member of StarExpression, but not anymore
		//! We wrap it into an OPERATOR_UNPACK instead
		vector<unique_ptr<ParsedExpression>> unpack_children;
		unpack_children.push_back(std::move(result));
		return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_UNPACK, std::move(unpack_children));
	}
	return std::move(result);
}

StarExpression::StarExpression(const identifier_set_t &exclude_list_p, qualified_column_set_t qualified_set)
    : ParsedExpression(ExpressionType::STAR, ExpressionClass::STAR), exclude_list(std::move(qualified_set)) {
	for (auto &entry : exclude_list_p) {
		exclude_list.insert(QualifiedColumnName(Identifier(entry)));
	}
}

identifier_set_t StarExpression::SerializedExcludeList() const {
	// we serialize non-qualified elements in a separate list of only column names for backwards compatibility
	identifier_set_t result;
	for (auto &entry : exclude_list) {
		if (!entry.IsQualified()) {
			result.insert(entry.column);
		}
	}
	return result;
}

qualified_column_set_t StarExpression::SerializedQualifiedExcludeList() const {
	// we serialize only qualified elements in the qualified list for backwards compatibility
	qualified_column_set_t result;
	for (auto &entry : exclude_list) {
		if (entry.IsQualified()) {
			result.insert(entry);
		}
	}
	return result;
}

} // namespace duckdb
