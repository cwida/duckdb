#include "duckdb.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/language_macro_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/language_function_binder.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"

using namespace duckdb;

//! A deliberately trivial language: a function "compiles" to the constant its source text denotes.
//! It exists to exercise the macro-family language function layer from SQL tests.
class TestLangBinder : public LanguageFunctionBinder {
public:
	void ValidateCreate(ClientContext &context, CreateMacroInfo &info) override {
		// deliberately reallocate the member vector: core must not hold member references across this callback
		info.macros.reserve(info.macros.capacity() + 1);
		for (auto &member : info.macros) {
			auto &language_member = member->Cast<LanguageMacroFunction>();
			if (StringUtil::Contains(language_member.source, "invalid")) {
				throw BinderException("testlang: the source of \"%s\" may not contain the word \"invalid\"",
				                      info.GetFunctionName().GetIdentifierName());
			}
		}
		// publish per-member documentation: function metadata must survive member mutations
		for (auto &member : info.macros) {
			auto &language_member = member->Cast<LanguageMacroFunction>();
			FunctionDescription description;
			for (idx_t i = 0; i < member->parameters.size(); i++) {
				auto &param_name = member->parameters[i]->Cast<ColumnRefExpression>().GetColumnName();
				description.parameter_names.push_back(param_name.GetIdentifierName());
				// an untyped parameter is documented as a wildcard, like built-in function docs
				const auto &type = member->types[i];
				description.parameter_types.push_back(type.id() == LogicalTypeId::UNKNOWN ? LogicalType::ANY : type);
			}
			// a per-source description, so tests distinguish stale metadata from replaced metadata
			description.description = "testlang: " + language_member.source;
			info.descriptions.push_back(std::move(description));
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
			auto argument_idx = std::stoull(member.source.substr(4));
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
}
}
