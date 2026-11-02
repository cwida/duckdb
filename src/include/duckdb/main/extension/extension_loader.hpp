//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/extension/extension_loader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/profiler/metric_info.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/main/extension_install_info.hpp"
#include "duckdb/main/extension_manager.hpp"

namespace duckdb {
struct CreateCoordinateSystemInfo;
class TableFunctionCatalogEntry;

class DatabaseInstance;
struct CreateMacroInfo;
struct CreateCollationInfo;
struct CreateAggregateFunctionInfo;
struct CreateScalarFunctionInfo;
struct CreateTableFunctionInfo;
struct CreatePragmaFunctionInfo;
struct CreateWindowFunctionInfo;

struct ExtensionLoaderInfo {
	Identifier extension_name;
	Identifier extension_alias;
	string extension_description;
	Identifier extension_schema = Identifier::DefaultSchema();
};

class ExtensionLoader {
	friend class DuckDB;
	friend class ExtensionHelper;

public:
	explicit ExtensionLoader(const ExtensionActiveLoad &load_info);
	ExtensionLoader(DatabaseInstance &db, const string &extension_name);

	//! Returns the DatabaseInstance associated with this extension loader
	DUCKDB_API DatabaseInstance &GetDatabaseInstance() const;

public:
	//! Set the description of the extension
	DUCKDB_API void SetDescription(const string &description);
	//! Explicitly sets, creates and registers all functions in this dedicated extension schema
	DUCKDB_API void UseDedicatedSchemaForExtension(const Identifier &extension_schema_name);
	//! Explicitly sets, creates and registers all functions in the registered extension schema
	DUCKDB_API void UseDedicatedSchemaForExtension();
	//! Creates a schema in the catalog with the extension name
	DUCKDB_API void CreateSchema(const Identifier &extension_schema_name) const;
	//! Adds the created extension schema to the search path
	DUCKDB_API void AddSchemaToSearchPath(const Identifier &schema_name) const;
	//! Sets the default extension schema for this extension
	DUCKDB_API void UseDefaultSchema(const Identifier &name = DEFAULT_SCHEMA);
	DUCKDB_API static void RefreshSearchPath(ClientContext &context);
	//! Gets registered extension name (or alias)
	DUCKDB_API const Identifier &GetRegisteredExtensionName() const {
		return loader_info.extension_alias.empty() ? loader_info.extension_name : loader_info.extension_alias;
	}

public:
	//! Register a new scalar function - merge overloads if the function already exists
	DUCKDB_API void RegisterFunction(ScalarFunction function);
	DUCKDB_API void RegisterFunction(ScalarFunctionSet function);
	DUCKDB_API void RegisterFunction(CreateScalarFunctionInfo info);

	//! Register a new aggregate function - merge overloads if the function already exists
	DUCKDB_API void RegisterFunction(AggregateFunction function);
	DUCKDB_API void RegisterFunction(AggregateFunctionSet function);
	DUCKDB_API void RegisterFunction(CreateAggregateFunctionInfo info);

	//! Register a new aggregate function - merge overloads if the function already exists
	DUCKDB_API void RegisterFunction(WindowFunction function);
	DUCKDB_API void RegisterFunction(WindowFunctionSet function);
	DUCKDB_API void RegisterFunction(CreateWindowFunctionInfo info);

	//! Register a new table function - merge overloads if the function already exists
	DUCKDB_API void RegisterFunction(TableFunction function);
	DUCKDB_API void RegisterFunction(TableFunctionSet function);
	DUCKDB_API void RegisterFunction(CreateTableFunctionInfo info);

	//! Register a new pragma function - throw an exception if the function already exists
	DUCKDB_API void RegisterFunction(PragmaFunction function);

	//! Register a new pragma function set - throw an exception if the function already exists
	DUCKDB_API void RegisterFunction(PragmaFunctionSet function);
	DUCKDB_API void RegisterFunction(CreatePragmaFunctionInfo info);

	//! Register a CreateSecretFunction
	DUCKDB_API void RegisterFunction(CreateSecretFunction function);

	//! Register a new copy function - throw an exception if the function already exists
	DUCKDB_API void RegisterFunction(CopyFunction function);
	//! Register a new macro function - throw an exception if the function already exists
	DUCKDB_API void RegisterFunction(CreateMacroInfo &info);

	//! Register a new collation
	DUCKDB_API void RegisterCollation(CreateCollationInfo &info);

	//! Register a new coordinate system
	DUCKDB_API void RegisterCoordinateSystem(CreateCoordinateSystemInfo &info);

	//! Returns a reference to the function in the catalog - throws an exception if it does not exist
	DUCKDB_API ScalarFunctionCatalogEntry &GetFunction(const Identifier &name);
	DUCKDB_API TableFunctionCatalogEntry &GetTableFunction(const Identifier &name);
	DUCKDB_API optional_ptr<CatalogEntry> TryGetFunction(const Identifier &name);
	DUCKDB_API optional_ptr<CatalogEntry> TryGetTableFunction(const Identifier &name);

	//! Add a function overload
	DUCKDB_API void AddFunctionOverload(ScalarFunction function);
	DUCKDB_API void AddFunctionOverload(ScalarFunctionSet function);
	DUCKDB_API void AddFunctionOverload(TableFunctionSet function);

	//! Registers a new type
	DUCKDB_API void RegisterType(string type_name, LogicalType type,
	                             bind_logical_type_function_t bind_function = nullptr);
	//! Registers a new type together with the constructors that bind its type modifiers
	DUCKDB_API void RegisterType(string type_name, LogicalType type, TypeConstructorSet constructors);

	//! Registers a new secret type
	DUCKDB_API void RegisterSecretType(SecretType secret_type);

	//! Registers a cast between two types
	DUCKDB_API void RegisterCastFunction(const LogicalType &source, const LogicalType &target,
	                                     bind_cast_function_t function, int64_t implicit_cast_cost = -1);
	DUCKDB_API void RegisterCastFunction(const LogicalType &source, const LogicalType &target, BoundCastInfo function,
	                                     int64_t implicit_cast_cost = -1);

	//! Registers a rule for combining two types in implicit type resolution (LogicalType::TryGetMaxLogicalType)
	DUCKDB_API void RegisterCombineTypesRule(CombineTypesRule rule);

	//! Registers a custom metric so it appears in duckdb_available_metrics, and so QueryProfiler::SetMetric accepts it.
	//! The profiler's own metrics cannot be registered.
	DUCKDB_API void RegisterMetric(MetricInfo info);

private:
	void RegisterType(CreateTypeInfo &info);

	void FinalizeLoad();

private:
	DatabaseInstance &db;
	ExtensionLoaderInfo loader_info;
	optional_ptr<ExtensionInfo> extension_info;
};

} // namespace duckdb

//! Helper macro to define the entrypoint for a C++ extension
//! Usage:
//!
//!		DUCKDB_CPP_EXTENSION_ENTRY(my_extension, loader) {
//!			loader.RegisterFunction(...);
//!		}
//!
#define DUCKDB_CPP_EXTENSION_ENTRY(EXTENSION_NAME, LOADER_NAME)                                                        \
	DUCKDB_EXTENSION_API void EXTENSION_NAME##_duckdb_cpp_init(duckdb::ExtensionLoader &LOADER_NAME)
