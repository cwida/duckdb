#include "duckdb.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/language_macro_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/language_function_binder.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"

using namespace duckdb;

//! A deliberately trivial language: a function "compiles" to the constant its source text denotes.
//! It exists to exercise the macro-family language function layer from SQL tests.
class TestLangBinder : public LanguageFunctionBinder {
public:
	void ValidateCreate(ClientContext &context, LanguageFunctionCreateInfo &info) override {
		for (idx_t member_idx = 0; member_idx < info.MemberCount(); member_idx++) {
			if (StringUtil::Contains(info.MemberSource(member_idx), "invalid")) {
				throw BinderException("testlang: the source of %s may not contain the word \"invalid\"",
				                      info.GetFunctionName());
			}
		}
		// publish per-member documentation: function metadata must survive member mutations
		for (idx_t member_idx = 0; member_idx < info.MemberCount(); member_idx++) {
			FunctionDescription description;
			for (idx_t i = 0; i < info.MemberParameterCount(member_idx); i++) {
				description.parameter_names.push_back(info.MemberParameterName(member_idx, i));
				// an untyped parameter is documented as a wildcard, like built-in function docs
				const auto &type = info.MemberParameterType(member_idx, i);
				description.parameter_types.push_back(type.id() == LogicalTypeId::UNKNOWN ? LogicalType::ANY : type);
			}
			// a per-source description, so tests distinguish stale metadata from replaced metadata
			description.description = "testlang: " + info.MemberSource(member_idx);
			info.AddDescription(std::move(description));
		}
	}

	BindResult BindScalarCall(ExpressionBinder &binder, ScalarMacroCatalogEntry &entry, LanguageMacroFunction &member,
	                          vector<unique_ptr<ParsedExpression>> arguments, idx_t depth) override {
		// a testlang constant ignores its arguments, but argument resolution errors must still surface
		ErrorData error;
		vector<unique_ptr<Expression>> bound_arguments;
		for (auto &argument : arguments) {
			auto bound_argument = binder.BindChild(argument, depth, error);
			if (error.HasError()) {
				return BindResult(std::move(error));
			}
			bound_arguments.push_back(std::move(bound_argument));
		}
		// the source 'arg:<n>' compiles to the n-th argument, so tests observe the declaration
		// order, default insertion, and parameter casts core hands to the language binder
		if (StringUtil::StartsWith(member.source, "arg:")) {
			idx_t argument_idx;
			const auto index_text = member.source.substr(4);
			if (index_text.empty() || !TryCast::Operation<string_t, idx_t>(string_t(index_text), argument_idx)) {
				return BindResult(BinderException("testlang: malformed argument index \"%s\"", index_text));
			}
			if (argument_idx >= bound_arguments.size()) {
				return BindResult(BinderException("testlang: argument index %llu is out of range", argument_idx));
			}
			return BindResult(std::move(bound_arguments[argument_idx]));
		}
		// otherwise the compiled form is the constant the source denotes, core applies the
		// declared RETURNS cast
		return BindResult(make_uniq<BoundConstantExpression>(Value(member.source)));
	}
};

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(language_function_demo, loader) {
	loader.RegisterLanguage("testlang", make_shared_ptr<TestLangBinder>());
	// an aliased collated type, so tests pin that signature rendering preserves type identity
	loader.RegisterType("ci_text", LogicalType::VARCHAR_COLLATION("nocase").WithAlias("ci_text"));
}
}
