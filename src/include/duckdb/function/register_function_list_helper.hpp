//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/register_function_list_helper.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/config.hpp"

namespace duckdb {

static void FillFunctionParameters(FunctionDescription &function_description, const char *function_name,
                                   vector<string> &parameters, vector<string> &descriptions) {
	for (string &parameter : parameters) {
		vector<string> parameter_name_type = StringUtil::Split(parameter, "::");
		if (parameter_name_type.size() == 1) {
			function_description.parameter_names.push_back(std::move(parameter_name_type[0]));
			function_description.parameter_types.push_back(LogicalType::ANY);
		} else if (parameter_name_type.size() == 2) {
			function_description.parameter_names.push_back(std::move(parameter_name_type[0]));
			function_description.parameter_types.push_back(DBConfig::ParseLogicalType(parameter_name_type[1]));
		} else {
			throw InternalException("Ill formed function variant for function '%s'", function_name);
		}
	}
}

static vector<string> GetExamplesForFunctionAlias(const Identifier &function_name, const Identifier &alias_of,
                                                  vector<string> &all_examples) {
	vector<string> filtered_examples;
	auto &function_name_str = function_name.GetIdentifierName();
	auto &alias_of_str = alias_of.GetIdentifierName();
	bool is_operator = (!function_name_str.empty() && !(function_name_str[0] >= 'a' && function_name_str[0] <= 'z') &&
	                    !(function_name_str[0] >= 'A' && function_name_str[0] <= 'Z'));
	bool alias_of_is_operator = (!alias_of_str.empty() && !(alias_of_str[0] >= 'a' && alias_of_str[0] <= 'z') &&
	                             !(alias_of_str[0] >= 'A' && alias_of_str[0] <= 'Z'));
	// select examples with matching function name
	for (string &example : all_examples) {
		if (example.compare(0, function_name_str.size(), function_name_str) == 0 ||
		    (is_operator && example.find(function_name_str) != string::npos)) {
			filtered_examples.emplace_back(std::move(example));
		}
	}
	// fallback 1: create fitting examples by replacing canonical name by function_name
	if (filtered_examples.empty() && !alias_of.empty() && !alias_of_is_operator && !is_operator) {
		for (string &example : all_examples) {
			if (example.compare(0, alias_of_str.size(), alias_of_str) == 0) {
				filtered_examples.emplace_back(
				    function_name + example.substr(alias_of_str.size(), example.size() - alias_of_str.size()));
			}
		}
	}
	// fallback 2: use available examples anyway
	if (filtered_examples.empty()) {
		filtered_examples = all_examples;
	}
	return filtered_examples;
}

template <class FunctionDefinition, class T>
static void FillFunctionDescriptions(const FunctionDefinition &function, T &info) {
	vector<string> variants = StringUtil::Split(function.parameters, '\1');
	vector<string> descriptions = StringUtil::Split(function.description, '\1');
	vector<string> examples = StringUtil::Split(function.example, '\1');
	vector<string> categories = StringUtil::Split(function.categories, '\1');

	// add single variant for functions that take no arguments
	if (variants.empty()) {
		variants.push_back("");
	}

	for (idx_t variant_index = 0; variant_index < variants.size(); variant_index++) {
		FunctionDescription function_description;
		// parameter_names and parameter_types
		vector<string> parameters = StringUtil::SplitWithParentheses(variants[variant_index], ',');
		FillFunctionParameters(function_description, function.name, parameters, descriptions);
		// description
		if (descriptions.size() == variants.size()) {
			function_description.description = descriptions[variant_index];
		} else if (descriptions.size() == 1) {
			function_description.description = descriptions[0];
		} else if (!descriptions.empty()) {
			throw InternalException("Incorrect number of function descriptions for function '%s'", function.name);
		}
		// examples
		duckdb::vector<string> variant_examples;
		if (examples.size() == variants.size()) {
			variant_examples = StringUtil::Split(examples[variant_index], '\2');
		} else if (examples.size() == 1) {
			variant_examples = StringUtil::Split(examples[0], '\2');
		} else if (!examples.empty()) {
			throw InternalException("Incorrect number of function examples for function '%s'", function.name);
		}
		function_description.examples = GetExamplesForFunctionAlias(function.name, info.alias_of, variant_examples);
		// categories
		if (variant_index < categories.size()) {
			function_description.categories = StringUtil::Split(categories[variant_index], ',');
		} else if (categories.size() == 1) {
			function_description.categories = StringUtil::Split(categories[0], ',');
		}
		info.descriptions.push_back(std::move(function_description));
	}
}

} // namespace duckdb
