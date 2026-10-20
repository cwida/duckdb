#include "duckdb/parser/peg/ast/macro_parameter.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/function/language_macro_function.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/function/scalar_macro_function.hpp"

namespace duckdb {

static void AddMacroParameters(MacroFunction &function, vector<MacroParameter> &macro_parameters) {
	bool default_value_found = false;
	identifier_set_t parameter_names;
	for (auto &parameter : macro_parameters) {
		D_ASSERT(!parameter.name.empty());
		if (parameter_names.find(parameter.name) != parameter_names.end()) {
			throw ParserException("Duplicate parameter %s in macro definition", parameter.name);
		}
		parameter_names.insert(parameter.name);
		if (parameter.is_default) {
			auto default_expr = std::move(parameter.expression);
			default_expr->SetAlias(parameter.name);
			function.default_parameters[parameter.name] = std::move(default_expr);
			function.parameters.push_back(make_uniq<ColumnRefExpression>(parameter.name));
			default_value_found = true;
		} else {
			if (default_value_found) {
				throw ParserException("Parameter without a default follows parameter with a default");
			}
			function.parameters.push_back(std::move(parameter.expression));
		}
		function.types.push_back(parameter.type);
	}
}
unique_ptr<CreateStatement> PEGTransformerFactory::TransformCreateMacroStmt(
    PEGTransformer &transformer, const bool &macro_or_function, const optional<bool> &if_not_exists,
    const QualifiedName &qualified_name, vector<unique_ptr<MacroFunction>> macro_definition) {
	auto result = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);

	info->SetQualifiedName(qualified_name);

	info->on_conflict = if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;
	for (auto &macro_function : macro_definition) {
		info->macros.push_back(std::move(macro_function));
	}
	D_ASSERT(!info->macros.empty());
	auto macro_type = info->macros[0]->type;
	if (info->macros.size() > 1) {
		for (idx_t i = 1; i < info->macros.size(); ++i) {
			if (info->macros[i]->type != macro_type) {
				throw ParserException("Cannot mix table and scalar macro function definitions");
			}
		}
	}
	info->type = macro_type == MacroType::TABLE_MACRO ? CatalogType::TABLE_MACRO_ENTRY : CatalogType::MACRO_ENTRY;
	result->info = std::move(info);
	transformer.PivotEntryCheck("macro");
	return result;
}

unique_ptr<CreateStatement> PEGTransformerFactory::TransformCreateLanguageFunctionStmt(
    PEGTransformer &transformer, const optional<bool> &if_not_exists, const QualifiedName &qualified_name,
    optional<vector<MacroParameter>> macro_parameters, const optional<LogicalType> &returns_type,
    const Identifier &language, const string &source) {
	auto result = make_uniq<CreateStatement>();
	auto info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);

	info->SetQualifiedName(qualified_name);
	info->on_conflict = if_not_exists ? OnCreateConflict::IGNORE_ON_CONFLICT : OnCreateConflict::ERROR_ON_CONFLICT;

	auto member = make_uniq<LanguageMacroFunction>();
	member->language = language.GetIdentifierName();
	member->source = source;
	if (returns_type) {
		member->return_type = *returns_type;
	}
	if (macro_parameters) {
		AddMacroParameters(*member, *macro_parameters);
	}
	info->macros.push_back(std::move(member));

	result->info = std::move(info);
	transformer.PivotEntryCheck("macro");
	return result;
}

bool PEGTransformerFactory::TransformMacroKeyword(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformFunctionKeyword(PEGTransformer &transformer) {
	return false;
}

unique_ptr<MacroFunction> PEGTransformerFactory::TransformMacroDefinition(
    PEGTransformer &transformer, optional<vector<MacroParameter>> macro_parameters,
    const optional<LogicalType> &returns_type, unique_ptr<MacroFunction> macro_definition_body) {
	if (returns_type) {
		if (macro_definition_body->type == MacroType::TABLE_MACRO) {
			throw ParserException("RETURNS types are not supported for table macros");
		}
		macro_definition_body->return_type = *returns_type;
	}
	if (macro_parameters) {
		AddMacroParameters(*macro_definition_body, *macro_parameters);
	}
	return macro_definition_body;
}

unique_ptr<MacroFunction>
PEGTransformerFactory::TransformTableMacroDefinition(PEGTransformer &transformer,
                                                     unique_ptr<SelectStatement> select_statement_internal) {
	auto result = make_uniq<TableMacroFunction>();
	result->query_node = std::move(select_statement_internal->node);
	return std::move(result);
}

unique_ptr<MacroFunction>
PEGTransformerFactory::TransformScalarMacroDefinition(PEGTransformer &transformer,
                                                      unique_ptr<ParsedExpression> expression) {
	auto result = make_uniq<ScalarMacroFunction>();
	result->expression = std::move(expression);
	return std::move(result);
}

vector<MacroParameter> PEGTransformerFactory::TransformMacroParameters(PEGTransformer &transformer,
                                                                       vector<MacroParameter> macro_parameter) {
	return macro_parameter;
}

MacroParameter PEGTransformerFactory::TransformSimpleParameter(PEGTransformer &transformer,
                                                               const Identifier &type_func_name,
                                                               const optional<LogicalType> &type) {
	MacroParameter result;
	result.name = Identifier(type_func_name);
	result.expression = make_uniq<ColumnRefExpression>(Identifier(type_func_name));
	if (type) {
		result.type = *type;
	}
	result.is_default = false;
	return result;
}

} // namespace duckdb
