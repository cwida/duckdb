#include "duckdb/function/built_in_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/main/extension_entries.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/parser/parsed_data/create_collation_info.hpp"
#include "duckdb/parser/parsed_data/create_copy_function_info.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/create_window_function_info.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

BuiltinFunctions::BuiltinFunctions(CatalogTransaction transaction, Catalog &catalog)
    : transaction(transaction), catalog(catalog) {
}

BuiltinFunctions::~BuiltinFunctions() {
}

void BuiltinFunctions::AddCollation(string name, ScalarFunction function, bool combinable,
                                    bool not_required_for_equality) {
	CreateCollationInfo info(Identifier(std::move(name)), std::move(function), combinable, not_required_for_equality);
	info.internal = true;
	catalog.CreateCollation(transaction, info);
}

void BuiltinFunctions::AddFunction(AggregateFunctionSet set) {
	CreateAggregateFunctionInfo info(std::move(set));
	info.internal = true;
	catalog.CreateFunction(transaction, info);
}

void BuiltinFunctions::AddFunction(AggregateFunction function) {
	CreateAggregateFunctionInfo info(std::move(function));
	info.internal = true;
	catalog.CreateFunction(transaction, info);
}

void BuiltinFunctions::AddFunction(PragmaFunction function) {
	CreatePragmaFunctionInfo info(std::move(function));
	info.internal = true;
	catalog.CreatePragmaFunction(transaction, info);
}

void BuiltinFunctions::AddFunction(const string &name, PragmaFunctionSet functions) {
	CreatePragmaFunctionInfo info(Identifier(name), std::move(functions));
	info.internal = true;
	catalog.CreatePragmaFunction(transaction, info);
}

void BuiltinFunctions::AddFunction(ScalarFunction function) {
	CreateScalarFunctionInfo info(std::move(function));
	info.internal = true;
	catalog.CreateFunction(transaction, info);
}

void BuiltinFunctions::AddFunction(const vector<string> &names, ScalarFunction function) { // NOLINT: false positive
	for (auto &name : names) {
		function.name = Identifier(name);
		AddFunction(function);
	}
}

void BuiltinFunctions::AddFunction(ScalarFunctionSet set) {
	CreateScalarFunctionInfo info(std::move(set));
	info.internal = true;
	catalog.CreateFunction(transaction, info);
}

void BuiltinFunctions::AddFunction(TableFunction function) {
	CreateTableFunctionInfo info(std::move(function));
	info.internal = true;
	catalog.CreateTableFunction(transaction, info);
}

void BuiltinFunctions::AddFunction(TableFunctionSet set) {
	CreateTableFunctionInfo info(std::move(set));
	info.internal = true;
	catalog.CreateTableFunction(transaction, info);
}

void BuiltinFunctions::AddFunction(CopyFunction function) {
	CreateCopyFunctionInfo info(std::move(function));
	info.internal = true;
	catalog.CreateCopyFunction(transaction, info);
}

struct ExtensionFunctionInfo : public ScalarFunctionInfo {
	explicit ExtensionFunctionInfo(string extension_p) : extension(std::move(extension_p)) {
	}

	string extension;
};

static unique_ptr<Expression> BindExtensionFunction(FunctionBindExpressionInput &input) {
	auto &context = input.context;
	auto &arguments = input.children;
	auto &bound_function = input.bound_function;

	// if this is triggered we are trying to call a method that is present in an extension
	// but the extension is not loaded
	// try to autoload the extension
	// first figure out which extension we need to auto-load
	auto &function_info = bound_function.GetExtraFunctionInfo().Cast<ExtensionFunctionInfo>();
	auto &extension_name = function_info.extension;
	auto &db = *context.db;

	if (!ExtensionHelper::CanAutoloadExtension(db, extension_name)) {
		throw BinderException("Trying to call function %s which is present in extension \"%s\" - but the extension "
		                      "is not loaded and could not be auto-loaded",
		                      bound_function.GetName(), extension_name);
	}
	// auto-load the extension
	ExtensionHelper::AutoLoadExtension(db, extension_name);

	// now find the function in the catalog
	auto &catalog = Catalog::GetSystemCatalog(db);
	auto &function_entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(
	    context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), bound_function.GetName()));

	// override the function with the extension function
	auto func = function_entry.functions.GetFunctionByArguments(context, bound_function.GetArguments());
	FunctionBinder function_binder(context);
	return function_binder.BindScalarFunction(func, std::move(arguments));
}

void BuiltinFunctions::AddExtensionFunction(ScalarFunctionSet set) {
	CreateScalarFunctionInfo info(std::move(set));
	info.internal = true;
	catalog.CreateFunction(transaction, info);
}

void BuiltinFunctions::RegisterExtensionOverloads() {
#ifdef GENERATE_EXTENSION_ENTRIES
	// do not insert auto loading placeholders when generating extension entries
	return;
#endif
	ScalarFunctionSet current_set;
	for (auto &entry : EXTENSION_FUNCTION_OVERLOADS) {
		vector<LogicalType> arguments;
		auto splits = StringUtil::Split(entry.signature, ">");
		auto return_type = DBConfig::ParseLogicalType(splits[1]);
		auto parameters = Value(splits[0]).DefaultCastAs(LogicalType::LIST(LogicalType::VARCHAR));
		for (auto &param : ListValue::GetChildren(parameters)) {
			arguments.push_back(DBConfig::ParseLogicalType(param.GetValue<string>()));
		}
		if (entry.type != CatalogType::SCALAR_FUNCTION_ENTRY) {
			throw InternalException(
			    "Extension function overloads only supported for scalar functions currently - %s has a different type",
			    entry.name);
		}

		ScalarFunction function(entry.name, std::move(arguments), std::move(return_type), nullptr);
		function.SetBindExpressionCallback(BindExtensionFunction);

		function.SetExtraFunctionInfo<ExtensionFunctionInfo>(entry.extension);
		if (current_set.name != entry.name) {
			if (!current_set.name.empty()) {
				// create set of functions
				AddExtensionFunction(current_set);
			}
			current_set = ScalarFunctionSet(entry.name);
		}
		// add this function to the set of function overloads
		current_set.AddFunction(std::move(function));
	}
	AddExtensionFunction(std::move(current_set));
}

} // namespace duckdb
