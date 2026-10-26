#include "duckdb/function/language_macro_function.hpp"

#include "duckdb/main/language_function_binder.hpp"

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"

namespace duckdb {

LanguageMacroFunction::LanguageMacroFunction(string language, string source)
    : MacroFunction(MacroType::LANGUAGE_MACRO), language(std::move(language)), source(std::move(source)) {
}

LanguageMacroFunction::LanguageMacroFunction() : MacroFunction(MacroType::LANGUAGE_MACRO) {
}

unique_ptr<MacroFunction> LanguageMacroFunction::Copy() const {
	auto result = make_uniq<LanguageMacroFunction>(language, source);
	CopyProperties(*result);
	return std::move(result);
}

string LanguageMacroFunction::ToSQL() const {
	// MacroFunction::ToSQL renders "(params) [RETURNS type] AS ", splice the LANGUAGE in front of AS
	auto prefix = MacroFunction::ToSQL();
	D_ASSERT(StringUtil::EndsWith(prefix, " AS "));
	prefix.erase(prefix.size() - 4);
	return prefix + " LANGUAGE " + SQLIdentifier::ToString(language) + " AS " + SQLString::ToString(source);
}

const string &LanguageFunctionCreateInfo::MemberLanguage(idx_t member_idx) const {
	return members[member_idx]->Cast<LanguageMacroFunction>().language;
}

const string &LanguageFunctionCreateInfo::MemberSource(idx_t member_idx) const {
	return members[member_idx]->Cast<LanguageMacroFunction>().source;
}

idx_t LanguageFunctionCreateInfo::MemberParameterCount(idx_t member_idx) const {
	return members[member_idx]->parameters.size();
}

string LanguageFunctionCreateInfo::MemberParameterName(idx_t member_idx, idx_t param_idx) const {
	return members[member_idx]->parameters[param_idx]->Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName();
}

LogicalType LanguageFunctionCreateInfo::MemberParameterType(idx_t member_idx, idx_t param_idx) const {
	auto &member = *members[member_idx];
	if (param_idx >= member.types.size()) {
		return LogicalType::UNKNOWN;
	}
	return member.types[param_idx];
}

LogicalType LanguageFunctionCreateInfo::MemberReturnType(idx_t member_idx) const {
	return members[member_idx]->return_type;
}

void LanguageFunctionCreateInfo::SetMemberSource(idx_t member_idx, string source) {
	members[member_idx]->Cast<LanguageMacroFunction>().source = std::move(source);
}

} // namespace duckdb
