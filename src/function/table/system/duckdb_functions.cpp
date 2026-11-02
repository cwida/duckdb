#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/common/atomic.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/pragma_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/window_function_catalog_entry.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {
constexpr const char *AggregateFunctionCatalogEntry::Name;

struct DuckDBFunctionsData : public GlobalTableFunctionState {
	DuckDBFunctionsData() : offset(0), offset_in_entry(0) {
	}

	vector<reference<CatalogEntry>> entries;
	idx_t offset;
	idx_t offset_in_entry;
	//! The offset, published once per chunk (for progress)
	atomic<idx_t> progress_offset {0};
};

static unique_ptr<FunctionData> DuckDBFunctionsBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("database_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("database_oid");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("schema_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("function_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("alias_of");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("function_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("description");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("comment");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));

	names.emplace_back("return_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("parameters");
	return_types.push_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("parameter_types");
	return_types.push_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("varargs");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("macro_definition");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("has_side_effects");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("internal");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("extension_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("function_oid");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("examples");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));

	names.emplace_back("stability");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("categories");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));

	return nullptr;
}

static void ExtractFunctionsFromSchema(ClientContext &context, SchemaCatalogEntry &schema,
                                       DuckDBFunctionsData &result) {
	schema.Scan(context, CatalogType::SCALAR_FUNCTION_ENTRY,
	            [&](CatalogEntry &entry) { result.entries.push_back(entry); });
	schema.Scan(context, CatalogType::TABLE_FUNCTION_ENTRY,
	            [&](CatalogEntry &entry) { result.entries.push_back(entry); });
	schema.Scan(context, CatalogType::PRAGMA_FUNCTION_ENTRY,
	            [&](CatalogEntry &entry) { result.entries.push_back(entry); });
}

unique_ptr<GlobalTableFunctionState> DuckDBFunctionsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBFunctionsData>();

	// scan all the schemas for tables and collect them and collect them
	auto schemas = Catalog::GetAllSchemas(context);
	for (auto &schema : schemas) {
		ExtractFunctionsFromSchema(context, schema.get(), *result);
	};

	std::sort(result->entries.begin(), result->entries.end(),
	          [&](reference<CatalogEntry> a, reference<CatalogEntry> b) {
		          return (int32_t)a.get().type < (int32_t)b.get().type;
	          });
	return std::move(result);
}

Value FunctionStabilityToValue(FunctionStability stability) {
	switch (stability) {
	case FunctionStability::VOLATILE:
		return Value("VOLATILE");
	case FunctionStability::CONSISTENT:
		return Value("CONSISTENT");
	case FunctionStability::CONSISTENT_WITHIN_QUERY:
		return Value("CONSISTENT_WITHIN_QUERY");
	default:
		throw InternalException("Unsupported FunctionStability");
	}
}

//! The legacy "varargs" column cannot tell "*args" and "**kwargs" apart - report whichever the function has
Value VariadicTypeValue(const FunctionSignature &signature) {
	auto variadic = signature.GetArgs();
	if (!variadic) {
		variadic = signature.GetKwargs();
	}
	return variadic ? Value(variadic->GetType().ToString()) : Value();
}

struct ScalarFunctionExtractor {
	static idx_t FunctionCount(ScalarFunctionCatalogEntry &entry) {
		return entry.functions.Size();
	}

	static Value GetFunctionType() {
		return Value("scalar");
	}

	static Value GetReturnType(ScalarFunctionCatalogEntry &entry, idx_t offset) {
		return Value(entry.functions.GetFunctionByOffset(offset)->GetReturnType().ToString());
	}

	static vector<Value> GetParameters(ScalarFunctionCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		for (auto &param : entry.functions.GetFunctionByOffset(offset)->GetSignature().GetParameters()) {
			if (!param.IsVariadic()) {
				results.emplace_back(param.GetName());
			}
		}
		return results;
	}

	static Value GetParameterTypes(ScalarFunctionCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		const auto &fun = *entry.functions.GetFunctionByOffset(offset);
		for (idx_t i = 0; i < fun.GetSignature().GetParameterCount(); i++) {
			if (!fun.GetSignature().GetParameter(i).IsVariadic()) {
				results.emplace_back(fun.GetSignature().GetParameter(i).GetType().ToString());
			}
		}
		return Value::LIST(LogicalType::VARCHAR, std::move(results));
	}

	static vector<LogicalType> GetParameterLogicalTypes(ScalarFunctionCatalogEntry &entry, idx_t offset) {
		const auto &fun = *entry.functions.GetFunctionByOffset(offset);
		vector<LogicalType> results;
		for (idx_t i = 0; i < fun.GetSignature().GetParameterCount(); i++) {
			if (!fun.GetSignature().GetParameter(i).IsVariadic()) {
				results.emplace_back(fun.GetSignature().GetParameter(i).GetType());
			}
		}
		return results;
	}

	static Value GetVarArgs(ScalarFunctionCatalogEntry &entry, idx_t offset) {
		return VariadicTypeValue(entry.functions.GetFunctionByOffset(offset)->GetSignature());
	}

	static Value GetMacroDefinition(ScalarFunctionCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static Value IsVolatile(ScalarFunctionCatalogEntry &entry, idx_t offset) {
		return Value::BOOLEAN(entry.functions.GetFunctionByOffset(offset)->GetStability() ==
		                      FunctionStability::VOLATILE);
	}

	static Value ResultType(ScalarFunctionCatalogEntry &entry, idx_t offset) {
		return FunctionStabilityToValue(entry.functions.GetFunctionByOffset(offset)->GetStability());
	}
};

struct WindowFunctionExtractor {
	static idx_t FunctionCount(WindowFunctionCatalogEntry &entry) {
		return entry.functions.Size();
	}

	static Value GetFunctionType() {
		return Value("window");
	}

	static Value GetReturnType(WindowFunctionCatalogEntry &entry, idx_t offset) {
		return Value(entry.functions.GetFunctionByOffset(offset)->GetReturnType().ToString());
	}

	static vector<Value> GetParameters(WindowFunctionCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		for (auto &param : entry.functions.GetFunctionByOffset(offset)->GetSignature().GetParameters()) {
			if (!param.IsVariadic()) {
				results.emplace_back(param.GetName());
			}
		}
		return results;
	}

	static Value GetParameterTypes(WindowFunctionCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		const auto &fun = *entry.functions.GetFunctionByOffset(offset);
		for (idx_t i = 0; i < fun.GetSignature().GetParameterCount(); i++) {
			if (!fun.GetSignature().GetParameter(i).IsVariadic()) {
				results.emplace_back(fun.GetSignature().GetParameter(i).GetType().ToString());
			}
		}
		return Value::LIST(LogicalType::VARCHAR, std::move(results));
	}

	static vector<LogicalType> GetParameterLogicalTypes(WindowFunctionCatalogEntry &entry, idx_t offset) {
		const auto &fun = *entry.functions.GetFunctionByOffset(offset);
		vector<LogicalType> results;
		for (idx_t i = 0; i < fun.GetSignature().GetParameterCount(); i++) {
			if (!fun.GetSignature().GetParameter(i).IsVariadic()) {
				results.emplace_back(fun.GetSignature().GetParameter(i).GetType());
			}
		}
		return results;
	}

	static Value GetVarArgs(WindowFunctionCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static Value GetMacroDefinition(WindowFunctionCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static Value IsVolatile(WindowFunctionCatalogEntry &entry, idx_t offset) {
		return Value::BOOLEAN(false);
	}

	static Value ResultType(WindowFunctionCatalogEntry &entry, idx_t offset) {
		return FunctionStabilityToValue(FunctionStability::CONSISTENT);
	}
};

struct AggregateFunctionExtractor {
	static idx_t FunctionCount(AggregateFunctionCatalogEntry &entry) {
		return entry.functions.Size();
	}

	static Value GetFunctionType() {
		return Value("aggregate");
	}

	static Value GetReturnType(AggregateFunctionCatalogEntry &entry, idx_t offset) {
		return Value(entry.functions.GetFunctionByOffset(offset)->GetReturnType().ToString());
	}

	static vector<Value> GetParameters(AggregateFunctionCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		for (auto &param : entry.functions.GetFunctionByOffset(offset)->GetSignature().GetParameters()) {
			if (!param.IsVariadic()) {
				results.emplace_back(param.GetName());
			}
		}
		return results;
	}

	static Value GetParameterTypes(AggregateFunctionCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		const auto &fun = *entry.functions.GetFunctionByOffset(offset);
		for (idx_t i = 0; i < fun.GetSignature().GetParameterCount(); i++) {
			if (!fun.GetSignature().GetParameter(i).IsVariadic()) {
				results.emplace_back(fun.GetSignature().GetParameter(i).GetType().ToString());
			}
		}
		return Value::LIST(LogicalType::VARCHAR, std::move(results));
	}

	static vector<LogicalType> GetParameterLogicalTypes(AggregateFunctionCatalogEntry &entry, idx_t offset) {
		const auto &fun = *entry.functions.GetFunctionByOffset(offset);
		vector<LogicalType> results;
		for (idx_t i = 0; i < fun.GetSignature().GetParameterCount(); i++) {
			if (!fun.GetSignature().GetParameter(i).IsVariadic()) {
				results.emplace_back(fun.GetSignature().GetParameter(i).GetType());
			}
		}
		return results;
	}

	static Value GetVarArgs(AggregateFunctionCatalogEntry &entry, idx_t offset) {
		return VariadicTypeValue(entry.functions.GetFunctionByOffset(offset)->GetSignature());
	}

	static Value GetMacroDefinition(AggregateFunctionCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static Value IsVolatile(AggregateFunctionCatalogEntry &entry, idx_t offset) {
		return Value::BOOLEAN(entry.functions.GetFunctionByOffset(offset)->GetStability() ==
		                      FunctionStability::VOLATILE);
	}

	static Value ResultType(AggregateFunctionCatalogEntry &entry, idx_t offset) {
		return FunctionStabilityToValue(entry.functions.GetFunctionByOffset(offset)->GetStability());
	}
};

struct MacroExtractor {
	static idx_t FunctionCount(ScalarMacroCatalogEntry &entry) {
		return entry.macros.size();
	}

	static Value GetFunctionType() {
		return Value("macro");
	}

	static Value GetReturnType(ScalarMacroCatalogEntry &entry, idx_t offset) {
		auto &macro_entry = *entry.macros[offset];
		if (macro_entry.return_type.id() != LogicalTypeId::UNKNOWN) {
			return Value(macro_entry.return_type.ToString());
		}
		return Value();
	}

	static vector<Value> GetParameters(ScalarMacroCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		auto &macro_entry = *entry.macros[offset];
		for (auto &param : macro_entry.parameters) {
			D_ASSERT(param->GetExpressionType() == ExpressionType::COLUMN_REF);
			auto &colref = param->Cast<ColumnRefExpression>();
			results.emplace_back(colref.GetColumnName());
		}
		return results;
	}

	static Value GetParameterTypes(ScalarMacroCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		auto &macro_entry = *entry.macros[offset];
		for (idx_t i = 0; i < macro_entry.parameters.size(); i++) {
			const auto has_type = !macro_entry.types.empty() && macro_entry.types[i] != LogicalType::UNKNOWN;
			results.emplace_back(has_type ? macro_entry.types[i].ToString() : Value(LogicalType::VARCHAR));
		}
		return Value::LIST(LogicalType::VARCHAR, std::move(results));
	}

	static vector<LogicalType> GetParameterLogicalTypes(ScalarMacroCatalogEntry &entry, idx_t offset) {
		vector<LogicalType> results;
		auto &macro_entry = *entry.macros[offset];
		for (idx_t i = 0; i < macro_entry.parameters.size(); i++) {
			results.emplace_back(macro_entry.types.empty() ? LogicalType::UNKNOWN : macro_entry.types[i]);
		}
		return results;
	}

	static Value GetVarArgs(ScalarMacroCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static Value GetMacroDefinition(ScalarMacroCatalogEntry &entry, idx_t offset) {
		auto &macro_entry = *entry.macros[offset];
		D_ASSERT(macro_entry.type == MacroType::SCALAR_MACRO);
		auto &func = macro_entry.Cast<ScalarMacroFunction>();
		return func.expression->ToString();
	}

	static Value IsVolatile(ScalarMacroCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static Value ResultType(ScalarMacroCatalogEntry &entry, idx_t offset) {
		return Value();
	}
};

struct TableMacroExtractor {
	static idx_t FunctionCount(TableMacroCatalogEntry &entry) {
		return entry.macros.size();
	}

	static Value GetFunctionType() {
		return Value("table_macro");
	}

	static Value GetReturnType(TableMacroCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static vector<Value> GetParameters(TableMacroCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		auto &macro_entry = *entry.macros[offset];
		for (auto &param : macro_entry.parameters) {
			D_ASSERT(param->GetExpressionType() == ExpressionType::COLUMN_REF);
			auto &colref = param->Cast<ColumnRefExpression>();
			results.emplace_back(colref.GetColumnName());
		}
		return results;
	}

	static Value GetParameterTypes(TableMacroCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		auto &macro_entry = *entry.macros[offset];
		for (idx_t i = 0; i < macro_entry.parameters.size(); i++) {
			const auto has_type = !macro_entry.types.empty() && macro_entry.types[i] != LogicalType::UNKNOWN;
			results.emplace_back(has_type ? macro_entry.types[i].ToString() : Value(LogicalType::VARCHAR));
		}
		return Value::LIST(LogicalType::VARCHAR, std::move(results));
	}

	static vector<LogicalType> GetParameterLogicalTypes(TableMacroCatalogEntry &entry, idx_t offset) {
		vector<LogicalType> results;
		auto &macro_entry = *entry.macros[offset];
		for (idx_t i = 0; i < macro_entry.parameters.size(); i++) {
			results.emplace_back(macro_entry.types.empty() ? LogicalType::UNKNOWN : macro_entry.types[i]);
		}
		return results;
	}

	static Value GetVarArgs(TableMacroCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static Value GetMacroDefinition(TableMacroCatalogEntry &entry, idx_t offset) {
		auto &macro_entry = *entry.macros[offset];
		if (macro_entry.type == MacroType::TABLE_MACRO) {
			auto &func = macro_entry.Cast<TableMacroFunction>();
			return func.query_node->ToString();
		}
		return Value();
	}

	static Value IsVolatile(TableMacroCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static Value ResultType(TableMacroCatalogEntry &entry, idx_t offset) {
		return Value();
	}
};

//! The options a "**kwargs" parameter declares are listed after the parameters, as they are passed by name like them
static void AddOptions(const FunctionSignature &signature, vector<Value> &results, bool types) {
	auto option_schema = signature.GetTypedKwargs();
	if (!option_schema) {
		return;
	}
	for (auto &option : option_schema->GetOptions()) {
		results.emplace_back(types ? Value(option.type.ToString()) : Value(option.name));
	}
}

struct TableFunctionExtractor {
	static idx_t FunctionCount(TableFunctionCatalogEntry &entry) {
		return entry.functions.Size();
	}

	static Value GetFunctionType() {
		return Value("table");
	}

	static Value GetReturnType(TableFunctionCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static vector<Value> GetParameters(TableFunctionCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		const auto &fun = *entry.functions.GetFunctionByOffset(offset);
		// the variadic parameters are reported in the "varargs" column instead
		for (auto &param : fun.GetSignature().GetParameters()) {
			if (!param.IsVariadic()) {
				results.emplace_back(param.GetName());
			}
		}
		AddOptions(fun.GetSignature(), results, false);
		return results;
	}

	static Value GetParameterTypes(TableFunctionCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		const auto &fun = *entry.functions.GetFunctionByOffset(offset);

		for (auto &param : fun.GetSignature().GetParameters()) {
			if (!param.IsVariadic()) {
				results.emplace_back(param.GetType().ToString());
			}
		}
		AddOptions(fun.GetSignature(), results, true);
		return Value::LIST(LogicalType::VARCHAR, std::move(results));
	}

	static vector<LogicalType> GetParameterLogicalTypes(TableFunctionCatalogEntry &entry, idx_t offset) {
		const auto &signature = entry.functions.GetFunctionByOffset(offset)->GetSignature();
		vector<LogicalType> result;
		for (idx_t i = 0; i < signature.GetPositionalParameterCount(); i++) {
			result.push_back(signature.GetParameter(i).GetType());
		}
		return result;
	}

	static Value GetVarArgs(TableFunctionCatalogEntry &entry, idx_t offset) {
		const auto &fun = *entry.functions.GetFunctionByOffset(offset);
		auto args = fun.GetSignature().GetArgs();
		return args ? Value(args->GetType().ToString()) : Value();
	}

	static Value GetMacroDefinition(TableFunctionCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static Value IsVolatile(TableFunctionCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static Value ResultType(TableFunctionCatalogEntry &entry, idx_t offset) {
		return Value();
	}
};

struct PragmaFunctionExtractor {
	static idx_t FunctionCount(PragmaFunctionCatalogEntry &entry) {
		return entry.functions.Size();
	}

	static Value GetFunctionType() {
		return Value("pragma");
	}

	static Value GetReturnType(PragmaFunctionCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static vector<Value> GetParameters(PragmaFunctionCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		const auto &fun = *entry.functions.GetFunctionByOffset(offset);
		// the variadic parameters are reported in the "varargs" column instead
		for (auto &param : fun.GetSignature().GetParameters()) {
			if (!param.IsVariadic()) {
				results.emplace_back(param.GetName());
			}
		}
		AddOptions(fun.GetSignature(), results, false);
		return results;
	}

	static Value GetParameterTypes(PragmaFunctionCatalogEntry &entry, idx_t offset) {
		vector<Value> results;
		const auto &fun = *entry.functions.GetFunctionByOffset(offset);

		for (auto &param : fun.GetSignature().GetParameters()) {
			if (!param.IsVariadic()) {
				results.emplace_back(param.GetType().ToString());
			}
		}
		AddOptions(fun.GetSignature(), results, true);
		return Value::LIST(LogicalType::VARCHAR, std::move(results));
	}

	static vector<LogicalType> GetParameterLogicalTypes(PragmaFunctionCatalogEntry &entry, idx_t offset) {
		const auto &fun = *entry.functions.GetFunctionByOffset(offset);
		const auto &signature = fun.GetSignature();
		vector<LogicalType> result;
		for (idx_t i = 0; i < signature.GetPositionalParameterCount(); i++) {
			result.push_back(signature.GetParameter(i).GetType());
		}
		return result;
	}

	static Value GetVarArgs(PragmaFunctionCatalogEntry &entry, idx_t offset) {
		const auto &fun = *entry.functions.GetFunctionByOffset(offset);
		auto args = fun.GetSignature().GetArgs();
		return args ? Value(args->GetType().ToString()) : Value();
	}

	static Value GetMacroDefinition(PragmaFunctionCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static Value IsVolatile(PragmaFunctionCatalogEntry &entry, idx_t offset) {
		return Value();
	}

	static Value ResultType(PragmaFunctionCatalogEntry &entry, idx_t offset) {
		return Value();
	}
};

static vector<Value> ToValueVector(vector<string> &string_vector) {
	vector<Value> result;
	for (string &str : string_vector) {
		result.emplace_back(str);
	}
	return result;
}

template <class T, class OP>
static Value GetParameterNames(CatalogEntry &entry, idx_t function_idx, FunctionDescription &function_description,
                               Value &parameter_types) {
	// default param names from function signature (if it exists),
	// otherwise fall back to function.json entry
	auto &function = entry.Cast<T>();
	vector<Value> parameter_names = OP::GetParameters(function, function_idx);
	idx_t parameter_count = ListValue::GetChildren(parameter_types).size();
	if (parameter_names.size() != parameter_count) {
		parameter_names.clear();
		for (idx_t param_idx = 0; param_idx < parameter_count; param_idx++) {
			if (param_idx < function_description.parameter_names.size()) {
				parameter_names.emplace_back(function_description.parameter_names[param_idx]);
			} else {
				parameter_names.emplace_back("col" + to_string(param_idx));
			}
		}
	}
	return Value::LIST(LogicalType::VARCHAR, parameter_names);
}

// returns values:
// 0: exact type match; N: match using N <ANY> values; Invalid(): no match
static optional_idx CalcDescriptionSpecificity(FunctionDescription &description,
                                               const vector<LogicalType> &parameter_types) {
	if (description.parameter_types.size() != parameter_types.size()) {
		return optional_idx::Invalid();
	}
	idx_t any_count = 0;
	for (idx_t i = 0; i < description.parameter_types.size(); i++) {
		if (description.parameter_types[i].id() == LogicalTypeId::ANY) {
			any_count++;
		} else if (description.parameter_types[i] != parameter_types[i]) {
			return optional_idx::Invalid();
		}
	}
	return any_count;
}

// Find FunctionDescription object with matching number of arguments and types
static optional_idx GetFunctionDescriptionIndex(vector<FunctionDescription> &function_descriptions,
                                                vector<LogicalType> &function_parameter_types) {
	if (function_descriptions.size() == 1) {
		// one description, use it even if nr of parameters don't match
		idx_t nr_function_parameters = function_parameter_types.size();
		for (idx_t i = 0; i < function_descriptions[0].parameter_types.size(); i++) {
			if (i < nr_function_parameters && function_descriptions[0].parameter_types[i] != LogicalTypeId::ANY &&
			    function_descriptions[0].parameter_types[i] != function_parameter_types[i]) {
				return optional_idx::Invalid();
			}
		}
		return optional_idx(0);
	}

	// multiple descriptions, search most specific description
	optional_idx best_description_idx;
	// specificity_score: 0: exact type match; N: match using N <ANY> values; Invalid(): no match
	optional_idx best_specificity_score;
	optional_idx specificity_score;
	for (idx_t descr_idx = 0; descr_idx < function_descriptions.size(); descr_idx++) {
		specificity_score = CalcDescriptionSpecificity(function_descriptions[descr_idx], function_parameter_types);
		if (specificity_score.IsValid() &&
		    (!best_specificity_score.IsValid() || specificity_score.GetIndex() < best_specificity_score.GetIndex())) {
			best_specificity_score = specificity_score;
			best_description_idx = descr_idx;
		}
	}
	return best_description_idx;
}

template <class T, class OP>
bool ExtractFunctionData(CatalogEntry &entry, idx_t function_idx, DataChunk &output) {
	auto &function = entry.Cast<T>();
	vector<LogicalType> parameter_types_vector = OP::GetParameterLogicalTypes(function, function_idx);
	Value parameter_types_value = OP::GetParameterTypes(function, function_idx);
	optional_idx description_idx = GetFunctionDescriptionIndex(function.descriptions, parameter_types_vector);
	FunctionDescription function_description =
	    description_idx.IsValid() ? function.descriptions[description_idx.GetIndex()] : FunctionDescription();

	idx_t col = 0;

	// database_name, LogicalType::VARCHAR
	output.data[col++].Append(Value(function.schema.catalog.GetName()));

	// database_oid, BIGINT
	output.data[col++].Append(Value::BIGINT(NumericCast<int64_t>(function.schema.catalog.GetOid())));

	// schema_name, LogicalType::VARCHAR
	output.data[col++].Append(Value(function.schema.name));

	// function_name, LogicalType::VARCHAR
	output.data[col++].Append(Value(function.name));

	// alias_of, LogicalType::VARCHAR
	output.data[col++].Append(
	    function.alias_of.empty() || function.alias_of == function.name ? Value() : Value(function.alias_of));

	// function_type, LogicalType::VARCHAR
	output.data[col++].Append(Value(OP::GetFunctionType()));

	// function_description, LogicalType::VARCHAR
	output.data[col++].Append(function_description.description.empty() ? Value()
	                                                                   : Value(function_description.description));

	// comment, LogicalType::VARCHAR
	output.data[col++].Append(Value(function.comment));

	// tags, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)
	output.data[col++].Append(Value::MAP(function.tags));

	// return_type, LogicalType::VARCHAR
	output.data[col++].Append(OP::GetReturnType(function, function_idx));

	// parameters, LogicalType::LIST(LogicalType::VARCHAR)
	output.data[col++].Append(
	    GetParameterNames<T, OP>(function, function_idx, function_description, parameter_types_value));

	// parameter_types, LogicalType::LIST(LogicalType::VARCHAR)
	output.data[col++].Append(parameter_types_value);

	// varargs, LogicalType::VARCHAR
	output.data[col++].Append(OP::GetVarArgs(function, function_idx));

	// macro_definition, LogicalType::VARCHAR
	output.data[col++].Append(OP::GetMacroDefinition(function, function_idx));

	// has_side_effects, LogicalType::BOOLEAN
	output.data[col++].Append(OP::IsVolatile(function, function_idx));

	// internal, LogicalType::BOOLEAN
	output.data[col++].Append(Value::BOOLEAN(function.internal));

	// extension_name, LogicalType::VARCHAR - display empty as NULL
	output.data[col++].Append(function.extension_name.empty() ? Value() : Value(function.extension_name));

	// function_oid, LogicalType::BIGINT
	output.data[col++].Append(Value::BIGINT(NumericCast<int64_t>(function.oid)));

	// examples, LogicalType::LIST(LogicalType::VARCHAR)
	output.data[col++].Append(Value::LIST(LogicalType::VARCHAR, ToValueVector(function_description.examples)));

	// stability, LogicalType::VARCHAR
	output.data[col++].Append(OP::ResultType(function, function_idx));

	// categories, LogicalType::LIST(LogicalType::VARCHAR)
	output.data[col++].Append(Value::LIST(LogicalType::VARCHAR, ToValueVector(function_description.categories)));

	return function_idx + 1 == OP::FunctionCount(function);
}

static bool Finished(const DuckDBFunctionsData &data) {
	if (data.offset < data.entries.size()) {
		return false;
	}
	return true;
}

void DuckDBFunctionsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBFunctionsData>();
	if (Finished(data)) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = data.entries[data.offset].get().Cast<FunctionEntry>();
		bool finished;

		switch (entry.type) {
		case CatalogType::SCALAR_FUNCTION_ENTRY:
			finished = ExtractFunctionData<ScalarFunctionCatalogEntry, ScalarFunctionExtractor>(
			    entry, data.offset_in_entry, output);
			break;
		case CatalogType::AGGREGATE_FUNCTION_ENTRY:
			finished = ExtractFunctionData<AggregateFunctionCatalogEntry, AggregateFunctionExtractor>(
			    entry, data.offset_in_entry, output);
			break;
		case CatalogType::TABLE_MACRO_ENTRY:
			finished =
			    ExtractFunctionData<TableMacroCatalogEntry, TableMacroExtractor>(entry, data.offset_in_entry, output);
			break;
		case CatalogType::MACRO_ENTRY:
			finished =
			    ExtractFunctionData<ScalarMacroCatalogEntry, MacroExtractor>(entry, data.offset_in_entry, output);
			break;
		case CatalogType::TABLE_FUNCTION_ENTRY:
			finished = ExtractFunctionData<TableFunctionCatalogEntry, TableFunctionExtractor>(
			    entry, data.offset_in_entry, output);
			break;
		case CatalogType::PRAGMA_FUNCTION_ENTRY:
			finished = ExtractFunctionData<PragmaFunctionCatalogEntry, PragmaFunctionExtractor>(
			    entry, data.offset_in_entry, output);
			break;
		case CatalogType::WINDOW_FUNCTION_ENTRY:
			finished = ExtractFunctionData<WindowFunctionCatalogEntry, WindowFunctionExtractor>(
			    entry, data.offset_in_entry, output);
			break;
		default:
			throw InternalException("FIXME: unrecognized function type in duckdb_functions");
		}
		if (finished) {
			// finished with this function, move to the next function
			data.offset++;
			data.offset_in_entry = 0;
		} else {
			// more functions remain
			data.offset_in_entry++;
		}
		count++;
	}
	data.progress_offset.store(data.offset, std::memory_order_relaxed);
}

static double DuckDBFunctionsProgress(ClientContext &context, const FunctionData *bind_data,
                                      const GlobalTableFunctionState *global_state) {
	auto &data = global_state->Cast<DuckDBFunctionsData>();
	if (data.entries.empty()) {
		return 100.0;
	}
	auto offset = data.progress_offset.load(std::memory_order_relaxed);
	return 100.0 * static_cast<double>(offset) / static_cast<double>(data.entries.size());
}

void DuckDBFunctionsFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction functions("duckdb_functions", {}, DuckDBFunctionsFunction, DuckDBFunctionsBind, DuckDBFunctionsInit);
	functions.table_scan_progress = DuckDBFunctionsProgress;
	set.AddFunction(functions);
}

} // namespace duckdb
