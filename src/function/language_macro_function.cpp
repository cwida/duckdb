#include "duckdb/function/language_macro_function.hpp"

#include "duckdb/main/language_function_binder.hpp"

#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
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

//! A complete identity for a frozen type: ordinary type equality ignores collations and treats
//! extension metadata as a wildcard, so compare the serialized representation, which captures
//! aliases, collations, modifiers, and nested types exactly
static string SerializedTypeIdentity(const LogicalType &type) {
	Allocator allocator;
	MemoryStream stream(allocator);
	SerializationOptions options;
	options.storage_compatibility = StorageCompatibility::Latest();
	BinarySerializer serializer(stream, options);
	serializer.Begin();
	serializer.WriteProperty(100, "type", type);
	serializer.End();
	return string(char_ptr_cast(stream.GetData()), stream.GetPosition());
}

LanguageFunctionCreateInfo::LanguageFunctionCreateInfo(const Identifier &function_name,
                                                       vector<unique_ptr<MacroFunction>> &members,
                                                       vector<FunctionDescription> &descriptions)
    : function_name(function_name), members(members), descriptions(descriptions) {
	// freeze the resolved signatures before the callback sees them
	for (auto &member : members) {
		FrozenMemberState state;
		for (auto &type : member->types) {
			state.parameter_type_identities.push_back(SerializedTypeIdentity(type));
		}
		state.return_type_identity = SerializedTypeIdentity(member->return_type);
		frozen_members.push_back(std::move(state));
	}
}

void LanguageFunctionCreateInfo::VerifyMemberState() const {
	for (idx_t member_idx = 0; member_idx < members.size(); member_idx++) {
		auto &member = *members[member_idx];
		auto &state = frozen_members[member_idx];
		bool signature_changed = state.parameter_type_identities.size() != member.types.size() ||
		                         state.return_type_identity != SerializedTypeIdentity(member.return_type);
		for (idx_t i = 0; !signature_changed && i < member.types.size(); i++) {
			signature_changed = state.parameter_type_identities[i] != SerializedTypeIdentity(member.types[i]);
		}
		if (signature_changed) {
			throw InternalException("The language binder for \"%s\" changed a resolved signature of %s() during "
			                        "validation, which is not allowed",
			                        member.Cast<LanguageMacroFunction>().language, GetFunctionName());
		}
	}
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
