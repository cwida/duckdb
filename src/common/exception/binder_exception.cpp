#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function.hpp"

namespace duckdb {

BinderException::BinderException(const string &msg) : Exception(ExceptionType::BINDER, msg) {
}

BinderException::BinderException(const unordered_map<string, string> &extra_info, const string &msg)
    : Exception(extra_info, ExceptionType::BINDER, msg) {
}

BinderException BinderException::ColumnNotFound(const Identifier &name, const vector<Identifier> &similar_bindings,
                                                QueryErrorContext context) {
	auto extra_info = Exception::InitializeExtraInfo("COLUMN_NOT_FOUND", context.query_location);
	string candidate_str = StringUtil::CandidatesMessage(IdentifiersToStrings(similar_bindings), "Candidate bindings");
	extra_info["name"] = name.GetIdentifierName();
	if (!similar_bindings.empty()) {
		extra_info["candidates"] = StringUtil::Join(similar_bindings, ",");
		return BinderException(extra_info, StringUtil::Format("Referenced column \"%s\" not found in FROM clause!%s",
		                                                      name.GetIdentifierName(), candidate_str));
	} else {
		return BinderException(
		    extra_info, StringUtil::Format("Referenced column \"%s\" was not found because the FROM clause is missing",
		                                   name.GetIdentifierName()));
	}
}

BinderException BinderException::NoMatchingFunction(const Identifier &catalog_name, const Identifier &schema_name,
                                                    const Identifier &name, const vector<LogicalType> &arguments,
                                                    const vector<pair<Identifier, LogicalType>> &named_arguments,
                                                    const vector<string> &candidates, const string &message) {
	auto extra_info = Exception::InitializeExtraInfo("NO_MATCHING_FUNCTION", QueryLocation());
	extra_info["name"] = name.GetIdentifierName();
	if (!catalog_name.empty()) {
		extra_info["catalog"] = catalog_name.GetIdentifierName();
	}
	if (!schema_name.empty()) {
		extra_info["schema"] = schema_name.GetIdentifierName();
	}
	extra_info["call"] = Function::CallToString(catalog_name, schema_name, name, arguments, named_arguments);
	if (!candidates.empty()) {
		extra_info["candidates"] = StringUtil::Join(candidates, ",");
	}
	return BinderException(extra_info, message);
}

BinderException BinderException::Unsupported(ParsedExpression &expr, const string &message) {
	auto extra_info = Exception::InitializeExtraInfo("UNSUPPORTED", expr.GetQueryLocation());
	return BinderException(extra_info, message);
}
} // namespace duckdb
