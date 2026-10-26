#include "duckdb/function/macro_function.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder.hpp"

#include "duckdb/common/sql_identifier.hpp"

#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/common/serializer/serializer.hpp"

namespace duckdb {

MacroFunction::MacroFunction(MacroType type) : type(type) {
}

string FormatMacroFunction(const MacroFunction &function, const Identifier &name) {
	auto result = SQLIdentifier::ToString(name.GetIdentifierName()) + "(";
	string parameters;
	for (idx_t param_idx = 0; param_idx < function.parameters.size(); param_idx++) {
		if (!parameters.empty()) {
			parameters += ", ";
		}
		const auto &param_name = function.parameters[param_idx]->Cast<ColumnRefExpression>().GetColumnName();
		parameters += param_name;

		if (!function.types.empty() && function.types[param_idx] != LogicalType::UNKNOWN) {
			parameters += " " + function.types[param_idx].ToString();
		}

		auto it = function.default_parameters.find(param_name);
		if (it != function.default_parameters.end()) {
			parameters += " := ";
			parameters += it->second->ToString();
		}
	}
	result += parameters + ")";
	return result;
}

MacroBindResult MacroFunction::BindMacroFunction(
    Binder &binder, const vector<unique_ptr<MacroFunction>> &functions, const Identifier &name,
    FunctionExpression &function_expr, vector<unique_ptr<ParsedExpression>> &positional_arguments,
    InsertionOrderPreservingMap<unique_ptr<ParsedExpression>, Identifier, identifier_map_t<idx_t>> &named_arguments,
    idx_t depth) {
	ExpressionBinder expr_binder(binder, binder.context);
	expr_binder.lambda_bindings = binder.lambda_bindings;

	// Figure out whether we even need to bind arguments
	bool requires_bind = false;
	for (auto &function : functions) {
		for (const auto &type : function->types) {
			if (type.id() != LogicalTypeId::UNKNOWN) {
				requires_bind = true;
				break;
			}
		}
		if (requires_bind) {
			break;
		}
	}

	// Find argument types and separate positional and default arguments
	vector<LogicalType> positional_arg_types;
	InsertionOrderPreservingMap<LogicalType, Identifier, identifier_map_t<idx_t>> named_arg_types;
	// the speculative binds below may register correlated columns referenced by the arguments: restore the binder's
	// correlated columns afterwards, binding the expanded macro body re-registers those that are actually used
	auto saved_correlated_columns = binder.correlated_columns;
	for (auto &arg : function_expr.GetArgumentsMutable()) {
		auto arg_copy = arg.GetExpression().Copy();
		LogicalType arg_type = LogicalType::UNKNOWN;
		if (requires_bind) {
			// bind at the same scope depth as the macro call: "depth" counts subquery scopes, not expression nesting
			const auto arg_bind_result = expr_binder.BindExpression(arg_copy, depth);
			arg_type = arg_bind_result.HasError() ? LogicalType::UNKNOWN : arg_bind_result.expression->GetReturnType();
		}
		if (arg.HasName()) {
			// Default argument
			if (named_arguments.find(arg.GetName()) != named_arguments.end()) {
				return MacroBindResult(
				    StringUtil::Format("Macro %s() has named argument repeated %s", name, arg.GetName()));
			}
			named_arg_types.insert(arg.GetName(), std::move(arg_type));
			named_arguments[arg.GetName()] = std::move(arg.GetExpressionMutable());
		} else if (!named_arguments.empty()) {
			return MacroBindResult(
			    StringUtil::Format("Macro %s() has positional argument following named argument", name));
		} else {
			// Positional argument
			positional_arguments.push_back(std::move(arg.GetExpressionMutable()));
			positional_arg_types.push_back(std::move(arg_type));
		}
	}
	binder.correlated_columns = std::move(saved_correlated_columns);

	// Check for each macro function if it matches the number of positional arguments
	auto lowest_cost = NumericLimits<idx_t>::Maximum();
	vector<idx_t> result_indices;
	for (idx_t function_idx = 0; function_idx < functions.size(); function_idx++) {
		auto &function = functions[function_idx];

		// At some point we want to guarantee that this has the same size as "parameters", but for now we fill to match
		auto parameter_types = function->types;
		parameter_types.resize(function->parameters.size(), LogicalType::UNKNOWN);

		// Check if we can exclude the match based on argument count (also avoids out-of-bounds below)
		if (positional_arguments.size() > function->parameters.size() ||
		    positional_arguments.size() + named_arguments.size() > function->parameters.size()) {
			continue;
		}

		// Also check if we can exclude the match based on the supplied named arguments
		bool bail = false;
		for (auto &kv : named_arguments) {
			bool found = false;
			for (const auto &parameter : function->parameters) {
				if (kv.first == parameter->Cast<ColumnRefExpression>().GetColumnName()) {
					found = true;
					break;
				}
			}
			if (!found) {
				bail = true;
				break;
			}
		}
		if (bail) {
			continue; // One of the supplied named arguments is not present in the macro definition
		}

		// Loop through arguments, positionals first, then named, summing up cost for best matching macro as we go
		idx_t param_idx = 0;
		idx_t macro_cost = 0;

		// Figure out best function fit, bail if any positional arguments are duplicated in named arguments
		for (; param_idx < positional_arguments.size(); param_idx++) {
			const auto &param_name = function->parameters[param_idx]->Cast<ColumnRefExpression>().GetColumnName();
			if (named_arguments.find(param_name) != named_arguments.end()) {
				bail = true;
				break;
			}
			const auto &param_type = parameter_types[param_idx];
			if (param_type == LogicalType::UNKNOWN) {
				macro_cost += 1000000;
			} else {
				const auto cast_cost =
				    CastFunctionSet::ImplicitCastCost(binder.context, positional_arg_types[param_idx], param_type);
				if (cast_cost < 0) {
					bail = true;
					break;
				}
				macro_cost += NumericCast<idx_t>(cast_cost);
			}
		}
		if (bail) {
			continue; // Couldn't find one of the supplied named arguments, or one of the casts is not possible
		}

		// Match remaining arguments with named/defaults
		for (; param_idx < function->parameters.size(); param_idx++) {
			const auto &param_name = function->parameters[param_idx]->Cast<ColumnRefExpression>().GetColumnName();
			const auto arg_it = named_arguments.find(param_name);
			const auto default_it = function->default_parameters.find(param_name);
			if (arg_it == named_arguments.end() && default_it == function->default_parameters.end()) {
				break; // The parameter has no argument (supplied/default)!
			}
			if (arg_it == named_arguments.end()) {
				continue; // Using default, no cost
			}

			// Supplied arg, add cost
			const auto &param_type = parameter_types[param_idx];
			if (param_type == LogicalType::UNKNOWN) {
				macro_cost += 1000000;
			} else {
				const auto cast_cost =
				    CastFunctionSet::ImplicitCastCost(binder.context, named_arg_types[param_name], param_type);
				if (cast_cost < 0) {
					break; // No cast possible
				}
				macro_cost += NumericCast<idx_t>(cast_cost);
			}
		}

		if (param_idx == function->parameters.size() && macro_cost <= lowest_cost) {
			// Found a matching function
			if (macro_cost < lowest_cost) {
				lowest_cost = macro_cost;
				result_indices.clear();
			}
			result_indices.push_back(function_idx);
		}
	}

	if (result_indices.size() != 1) {
		string error;
		if (result_indices.empty()) {
			// No matching function found
			error = StringUtil::Format("Macro %s() does not support the supplied arguments.", SQLIdentifier(name));
			error += " You might need to add explicit type casts.\n";
			error += "Candidate macros:";
			for (auto &function : functions) {
				error += "\n\t" + FormatMacroFunction(*function, name);
			}
		} else {
			// Multiple matching functions found
			error = StringUtil::Format("Macro %s() has multiple overloads that match the supplied arguments.\n",
			                           SQLIdentifier(name));
			error += "In order to select one, please supply all arguments by name, and/or add explicit type casts.\n";
			error += "Candidate macros:";
			for (const auto &result_idx : result_indices) {
				error += "\n\t" + FormatMacroFunction(*functions[result_idx], name);
			}
		}

		return MacroBindResult(error);
	}

	const auto &macro_idx = result_indices[0];
	const auto &macro_def = *functions[macro_idx];

	// Cast positionals to proper types
	auto parameter_types = macro_def.types;
	parameter_types.resize(macro_def.parameters.size(), LogicalType::UNKNOWN);
	idx_t param_idx = 0;
	for (; param_idx < positional_arguments.size(); param_idx++) {
		if (parameter_types[param_idx] != LogicalType::UNKNOWN) {
			// This macro parameter is typed, add a cast
			auto &positional_arg = positional_arguments[param_idx];
			positional_arg = make_uniq<CastExpression>(parameter_types[param_idx], std::move(positional_arg));
		}
	}

	// Add the default values for parameters that have defaults, for which no argument was supplied
	for (; param_idx < macro_def.parameters.size(); param_idx++) {
		const auto &param_name = macro_def.parameters[param_idx]->Cast<ColumnRefExpression>().GetColumnName();
		auto named_arg_it = named_arguments.find(param_name);
		if (named_arg_it != named_arguments.end()) {
			// The user has supplied an argument for this parameter
			if (parameter_types[param_idx] != LogicalType::UNKNOWN) {
				// This macro parameter is typed, add a cast
				auto &named_arg = named_arg_it->second;
				named_arg = make_uniq<CastExpression>(parameter_types[param_idx], std::move(named_arg));
			}
			continue;
		}

		const auto default_it = macro_def.default_parameters.find(param_name);
		D_ASSERT(default_it != macro_def.default_parameters.end());
		auto &named_arg = named_arguments[param_name];
		named_arg = default_it->second->Copy();
		if (parameter_types[param_idx] != LogicalType::UNKNOWN) {
			// This macro parameter is typed, add a cast
			named_arg = make_uniq<CastExpression>(parameter_types[param_idx], std::move(named_arg));
		}
	}

	return MacroBindResult(macro_idx);
}

unique_ptr<DummyBinding> MacroFunction::CreateDummyBinding(
    const MacroFunction &macro_def, const Identifier &name, vector<unique_ptr<ParsedExpression>> &positional_arguments,
    InsertionOrderPreservingMap<unique_ptr<ParsedExpression>, Identifier, identifier_map_t<idx_t>> &named_arguments) {
	// create a MacroBinding to bind this macro's parameters to its arguments
	vector<LogicalType> types = macro_def.types;
	types.resize(macro_def.parameters.size(), LogicalType::UNKNOWN);
	vector<Identifier> names;
	for (idx_t i = 0; i < positional_arguments.size(); i++) {
		names.emplace_back(macro_def.parameters[i]->Cast<ColumnRefExpression>().GetColumnName());
	}
	for (auto &kv : named_arguments) {
		names.emplace_back(kv.first);
		positional_arguments.push_back(std::move(kv.second)); // push defaults into positionals
	}

	auto res = make_uniq<DummyBinding>(types, names, name.GetIdentifierName());
	res->arguments = &positional_arguments;
	return res;
}

void MacroFunction::CopyProperties(MacroFunction &other) const {
	other.type = type;
	for (auto &param : parameters) {
		other.parameters.push_back(param->Copy());
	}
	for (auto &kv : default_parameters) {
		other.default_parameters[kv.first] = kv.second->Copy();
	}
	other.types = types;
	other.return_type = return_type;
}

vector<unique_ptr<ParsedExpression>>
MacroFunction::GetPositionalParametersForSerialization(Serializer &serializer) const {
	// this runs at the start of member serialization: reject a definition the target storage
	// version cannot represent instead of silently dropping its fields
	if (!serializer.ShouldSerialize(StorageVersion::V2_1_0)) {
		if (return_type.id() != LogicalTypeId::UNKNOWN) {
			throw SerializationException(
			    "Cannot write a macro with a declared RETURNS type for a storage version below v2.1.0. Re-attach "
			    "with (STORAGE_VERSION 'v2.1.0') or drop the macro");
		}
	}
	vector<unique_ptr<ParsedExpression>> result;
	if (serializer.ShouldSerialize(StorageVersion::V1_4_0)) {
		// We serialize all positional parameters as-is
		for (auto &param : parameters) {
			result.push_back(param->Copy());
		}
		return result;
	}
	// Serializing targeting an older version - delete all named parameters from the list of positional parameters
	for (auto &param : parameters) {
		auto &colref = param->Cast<ColumnRefExpression>();
		if (default_parameters.find(Identifier(colref.GetName())) != default_parameters.end()) {
			// This is a default parameter - do not serialize
			continue;
		}
		result.push_back(param->Copy());
	}
	return result;
}

void MacroFunction::FinalizeDeserialization() {
	// In older versions of DuckDB, parameters with defaults were not stored in "parameters", this adds them
	for (auto &kv : default_parameters) {
		bool found = false;
		for (const auto &parameter : parameters) {
			if (kv.first == parameter->Cast<ColumnRefExpression>().GetColumnName()) {
				found = true;
				break;
			}
		}
		if (!found) {
			parameters.push_back(make_uniq<ColumnRefExpression>(kv.first));
		}
	}
	// In older versions of DuckDB macros were always untyped
	if (parameters.size() != types.size()) {
		types.resize(parameters.size(), LogicalType::UNKNOWN);
	}
}

bool MacroFunction::HasUntypedParameter() const {
	if (types.size() < parameters.size()) {
		return true;
	}
	for (auto &type : types) {
		if (type.id() == LogicalTypeId::UNKNOWN) {
			return true;
		}
	}
	return false;
}

bool MacroFunction::MatchesSignature(const vector<LogicalType> &signature) const {
	if (parameters.size() != signature.size() || HasUntypedParameter()) {
		return false;
	}
	for (idx_t i = 0; i < signature.size(); i++) {
		if (types[i] != signature[i]) {
			return false;
		}
	}
	return true;
}

string MacroFunction::SignatureToString() const {
	string result = "(";
	for (idx_t i = 0; i < parameters.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		const auto &param_name = parameters[i]->Cast<ColumnRefExpression>().GetColumnName();
		result += SQLIdentifier::ToString(param_name);
		if (i < types.size() && types[i].id() != LogicalTypeId::UNKNOWN) {
			result += " " + types[i].ToString();
		}
	}
	result += ")";
	return result;
}

string MacroFunction::ToSQL() const {
	vector<string> param_strings;
	for (idx_t param_idx = 0; param_idx < parameters.size(); param_idx++) {
		const auto &param_name = parameters[param_idx]->Cast<ColumnRefExpression>().GetColumnName();
		auto param_string = SQLIdentifier::ToString(param_name);
		if (types[param_idx] != LogicalType::UNKNOWN) {
			param_string += " " + types[param_idx].ToString();
		}
		auto it = default_parameters.find(param_name);
		if (it != default_parameters.end()) {
			param_string += " := " + it->second->ToString();
		}
		param_strings.emplace_back(std::move(param_string));
	}
	string returns;
	if (return_type.id() != LogicalTypeId::UNKNOWN) {
		returns = " RETURNS " + return_type.ToString();
	}
	return StringUtil::Format("(%s)%s AS ", StringUtil::Join(param_strings, ", "), returns);
}

} // namespace duckdb
