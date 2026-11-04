//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/capi_v2/capi_v2_internal.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

// DuckDB C++ internals (also pulls in duckdb.h which defines idx_t, etc.)
#include "duckdb.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/parse_iterator.hpp"
#include "duckdb/main/table_description.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/sql_statement.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"
#include "duckdb/main/db_instance_cache.hpp"

// DuckDB internals used by the option set/get bridge.
#include "duckdb/main/setting_info.hpp"
#include "duckdb/execution/operator/helper/physical_set.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/common/query_context.hpp"
#include "duckdb/common/file_system.hpp"

// The engine implements the whole V2 C API, including the unstable surface
#ifndef DUCKDB_V2_API_ALLOW_UNSTABLE
#define DUCKDB_V2_API_ALLOW_UNSTABLE 1
#endif
// V2 C API header -- all types use duckdb_v2_ prefix, no collision with V1.
#include "duckdb_v2.h"

#include <new>
#include <type_traits>

// ABI guard: the bridge reinterpret_casts duckdb_v2_bytes <-> duckdb::string_t,
// so the layouts must match. sizeof/alignof tie them together; the offsetof
// checks pin duckdb_v2_bytes's field offsets (string_t's union is private, so
// offsetof can't reach into it).
static_assert(sizeof(duckdb_v2_bytes) == sizeof(duckdb::string_t),
              "duckdb_v2_bytes must match the size of duckdb::string_t");
static_assert(alignof(duckdb_v2_bytes) == alignof(duckdb::string_t),
              "duckdb_v2_bytes must match the alignment of duckdb::string_t");
static_assert(offsetof(duckdb_v2_bytes, value.pointer.length) == 0,
              "duckdb_v2_bytes value.pointer.length must be at offset 0");
static_assert(offsetof(duckdb_v2_bytes, value.pointer.prefix) == 4,
              "duckdb_v2_bytes value.pointer.prefix must be at offset 4");
static_assert(offsetof(duckdb_v2_bytes, value.pointer.ptr) == 8,
              "duckdb_v2_bytes value.pointer.ptr must be at offset 8");
static_assert(offsetof(duckdb_v2_bytes, value.inlined.inlined) == 4,
              "duckdb_v2_bytes value.inlined.inlined must be at offset 4");

namespace duckdb {

class AggregateFunctionProperties;

namespace capiv2 {

//----------------------------------------------------------------------------------------------------------------------
// Conversion Helpers
//----------------------------------------------------------------------------------------------------------------------
inline auto Convert(duckdb_v2_str str) -> std::string_view {
	if (!str.ptr) {
		if (str.len > 0) {
			throw InvalidInputException("byte range cannot be null unless it is empty");
		}
		return std::string_view();
	}
	return std::string_view(str.ptr, str.len);
}
inline auto Convert(const duckdb_v2_str *str) -> std::string_view {
	if (!str) {
		throw InvalidInputException("byte range cannot be null");
	}
	return Convert(*str);
}

// Validates identifier UTF-8 and returns its text; throws on invalid input. Defined in capi_v2_utf8.cpp.
auto ConvertIdentifierName(duckdb_v2_identifier_t name) -> std::string_view;
inline auto ConvertIdentifierName(const duckdb_v2_identifier_t *name) -> std::string_view {
	if (!name) {
		throw InvalidInputException("identifier cannot be null");
	}
	return ConvertIdentifierName(*name);
}

inline auto Convert(std::string_view str) -> duckdb_v2_str {
	return duckdb_v2_str {str.data(), str.size()};
}
inline auto Convert(const string &str) -> duckdb_v2_str {
	return duckdb_v2_str {str.data(), str.size()};
}
inline auto Convert(const Identifier &ident) -> duckdb_v2_identifier_t {
	return duckdb_v2_identifier_t {ident.c_str(), ident.size()};
}
inline auto Convert(duckdb_v2_hugeint_t value) -> hugeint_t {
	return hugeint_t(value.upper, value.lower);
}
inline auto Convert(duckdb_v2_uhugeint_t value) -> uhugeint_t {
	return uhugeint_t(value.upper, value.lower);
}
inline auto Convert(const duckdb_v2_hugeint_t *value) -> hugeint_t {
	if (!value) {
		throw InvalidInputException("hugeint value cannot be null");
	}
	return Convert(*value);
}
inline auto Convert(const duckdb_v2_uhugeint_t *value) -> uhugeint_t {
	if (!value) {
		throw InvalidInputException("uhugeint value cannot be null");
	}
	return Convert(*value);
}
inline auto Convert(hugeint_t value) -> duckdb_v2_hugeint_t {
	return duckdb_v2_hugeint_t {value.lower, value.upper};
}
inline auto Convert(uhugeint_t value) -> duckdb_v2_uhugeint_t {
	return duckdb_v2_uhugeint_t {value.lower, value.upper};
}
inline auto Convert(interval_t value) -> duckdb_v2_interval_t {
	return duckdb_v2_interval_t {value.months, value.days, value.micros};
}
inline auto Convert(duckdb_v2_interval_t value) -> interval_t {
	interval_t out;
	out.months = value.months;
	out.days = value.days;
	out.micros = value.micros;
	return out;
}
inline auto Convert(const duckdb_v2_interval_t *value) -> interval_t {
	if (!value) {
		throw InvalidInputException("interval value cannot be null");
	}
	return Convert(*value);
}

// The V2 enum surfaces core's StatementType under the same numeric values; every spec member is pinned, and the count
// pins the highest one - appending a member in core fails to compile until the v2 spec mirrors it.
#define DUCKDB_V2_ASSERT_STATEMENT_TYPE(member)                                                                        \
	static_assert(static_cast<uint8_t>(StatementType::member##_STATEMENT) == DUCKDB_V2_STATEMENT_TYPE_##member,        \
	              "StatementType::" #member "_STATEMENT must mirror DUCKDB_V2_STATEMENT_TYPE_" #member)
DUCKDB_V2_ASSERT_STATEMENT_TYPE(INVALID);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(SELECT);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(INSERT);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(UPDATE);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(CREATE);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(DELETE);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(PREPARE);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(EXECUTE);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(ALTER);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(TRANSACTION);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(COPY);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(ANALYZE);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(VARIABLE_SET);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(CREATE_FUNC);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(EXPLAIN);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(DROP);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(EXPORT);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(PRAGMA);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(VACUUM);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(CALL);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(SET);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(LOAD);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(RELATION);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(EXTENSION);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(LOGICAL_PLAN);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(ATTACH);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(DETACH);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(MULTI);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(COPY_DATABASE);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(UPDATE_EXTENSIONS);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(MERGE_INTO);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(CONNECT);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(DISCONNECT);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(EXTERNAL_RESOURCE);
DUCKDB_V2_ASSERT_STATEMENT_TYPE(PASSTHROUGH);
#undef DUCKDB_V2_ASSERT_STATEMENT_TYPE
static_assert(static_cast<uint8_t>(StatementType::ENUM_SIZE) == DUCKDB_V2_STATEMENT_TYPE_PASSTHROUGH + 1,
              "a StatementType was added: give it a DUCKDB_V2_STATEMENT_TYPE id in the v2 spec and pin it above");
inline auto Convert(StatementType type) -> DUCKDB_V2_STATEMENT_TYPE {
	return static_cast<DUCKDB_V2_STATEMENT_TYPE>(type);
}

//----------------------------------------------------------------------------------------------------------------------
// Handle Types
//----------------------------------------------------------------------------------------------------------------------

class CV2Environment {
public:
	unique_ptr<DBInstanceCache> cache;
	std::atomic<idx_t> instance_count {0};
};

inline auto Convert(CV2Environment *env) -> duckdb_v2_environment_handle {
	return reinterpret_cast<duckdb_v2_environment_handle>(env);
}

inline auto Convert(duckdb_v2_environment_handle env) -> CV2Environment * {
	return reinterpret_cast<CV2Environment *>(env);
}

class CV2Option;
class CV2Instance;
class CV2OptionSource;

//! A config handle: the scope config options are read from and written at.
class CV2Config {
public:
	virtual ~CV2Config() = default;

	//! The effective value of `name`, and the scope DuckDB attributes it to. Throws for an unknown option.
	virtual DUCKDB_V2_SETTING_SCOPE ReadValue(std::string_view name, Value &result) = 0;
	//! Writes `name` at `scope`, which the option must support.
	virtual void Write(const Identifier &name, const Value &value, DUCKDB_V2_SETTING_SCOPE scope) = 0;
	//! Where option descriptors are read from. The same from every config of one instance.
	virtual unique_ptr<CV2Option> GetOption(std::string_view name) = 0;
	virtual unique_ptr<CV2Option> GetOptionByIndex(idx_t index) = 0;
	virtual idx_t GetOptionCount() = 0;
};

inline auto Convert(duckdb_v2_config_handle config) -> CV2Config * {
	return reinterpret_cast<CV2Config *>(config);
}

inline auto Convert(CV2Config *config) -> duckdb_v2_config_handle {
	return reinterpret_cast<duckdb_v2_config_handle>(config);
}

//! An instance's config: GLOBAL only.
class CV2InstanceConfig final : public CV2Config {
public:
	explicit CV2InstanceConfig(CV2Instance &instance) : instance(instance) {
	}

	DUCKDB_V2_SETTING_SCOPE ReadValue(std::string_view name, Value &result) override;
	void Write(const Identifier &name, const Value &value, DUCKDB_V2_SETTING_SCOPE scope) override;
	unique_ptr<CV2Option> GetOption(std::string_view name) override;
	unique_ptr<CV2Option> GetOptionByIndex(idx_t index) override;
	idx_t GetOptionCount() override;

private:
	CV2Instance &instance;
};

//! A factory handle: the scope values, types and data chunks are created in.
class CV2Factory {
public:
	virtual ~CV2Factory() = default;

	//! The database whose allocator and buffer manager back what is created.
	virtual DatabaseInstance &GetDatabase() = 0;
	//! The client whose catalog and settings apply, or nullptr when only the built-ins do.
	virtual optional_ptr<ClientContext> TryGetClientContext() = 0;
	//! Runs `fn` with a transaction active on the client, for catalog lookups.
	virtual void WithTransaction(const std::function<void(ClientContext &)> &fn) = 0;
};

inline auto Convert(duckdb_v2_factory_handle factory) -> CV2Factory * {
	return reinterpret_cast<CV2Factory *>(factory);
}

inline auto Convert(CV2Factory *factory) -> duckdb_v2_factory_handle {
	return reinterpret_cast<duckdb_v2_factory_handle>(factory);
}

//! An instance's factory: built-in types and casts only, no catalog.
class CV2InstanceFactory final : public CV2Factory {
public:
	explicit CV2InstanceFactory(CV2Instance &instance) : instance(instance) {
	}

	DatabaseInstance &GetDatabase() override;
	optional_ptr<ClientContext> TryGetClientContext() override {
		return nullptr;
	}
	void WithTransaction(const std::function<void(ClientContext &)> &fn) override;

private:
	CV2Instance &instance;
};

//! The common type of a non-empty set of types, folded left to right in the factory's scope. Throws
//! NotImplementedException when there is none. Defined in capi_v2_logical_type.cpp.
auto ResolveCommonType(CV2Factory &factory, const vector<LogicalType> &types) -> LogicalType;

//! The SQL ATTACH `(KEY value)` options of one attach, as the text values a quoted literal produces. Bound to the
//! instance handle it was created from, which is what future per-instance resources (an allocator, say) would be
//! taken from.
class CV2AttachOptions {
public:
	explicit CV2AttachOptions(CV2Instance &instance) : instance(instance) {
	}

	CV2Instance &instance;
	unordered_map<string, Value> options;
};

inline auto Convert(duckdb_v2_attach_options_handle options) -> CV2AttachOptions * {
	return reinterpret_cast<CV2AttachOptions *>(options);
}

inline auto Convert(CV2AttachOptions *options) -> duckdb_v2_attach_options_handle {
	return reinterpret_cast<duckdb_v2_attach_options_handle>(options);
}

//! Throws unless `target` is the database `what` was created for, through its factory.
inline void CheckRegistrationTarget(const DatabaseInstance &created_for, const DatabaseInstance &target,
                                    const char *what) {
	if (&created_for != &target) {
		throw InvalidInputException("the %s was created through a factory of a different database", what);
	}
}

//! A file system handle: the file system plus the query its reads and writes are attributed to, if any.
class CV2FileSystem {
public:
	CV2FileSystem(FileSystem &fs, QueryContext query) : fs(fs), query(query) {
	}

	FileSystem &fs;
	QueryContext query;
};

//! An instance handle: a DuckDB instance, built from its startup options when the handle is created.
class CV2Instance {
public:
	CV2Instance(CV2Environment &env, DBConfig &startup_config);

	//! Attaches the database at `path` under `name` (derived from the path when empty), like ATTACH, optionally as
	//! the default for new connections.
	void Attach(const string &path, const Identifier &name, optional_ptr<const CV2AttachOptions> options,
	            bool make_default);
	//! Detaches the database attached under `name`.
	void Detach(const Identifier &name);
	//! Makes the database attached under `name` the default for new connections.
	void SetDefault(const Identifier &name);
	DuckDB &GetDatabase() {
		return *database;
	}

private:
	//! Declared first: the members below are built from it.
	shared_ptr<DuckDB> database;

public:
	CV2Environment &env;
	CV2InstanceFactory factory;
	CV2InstanceConfig config_handle;
	CV2FileSystem file_system;
};

inline auto Convert(duckdb_v2_instance_handle instance) -> CV2Instance * {
	return reinterpret_cast<CV2Instance *>(instance);
}

inline auto Convert(CV2Instance *instance) -> duckdb_v2_instance_handle {
	return reinterpret_cast<duckdb_v2_instance_handle>(instance);
}

class CV2Context;

//! A client's config: the connection's cascade (SESSION, then GLOBAL, then the default), written like SQL `SET`.
class CV2ClientConfig final : public CV2Config {
public:
	explicit CV2ClientConfig(ClientContext &context) : context(context) {
	}

	DUCKDB_V2_SETTING_SCOPE ReadValue(std::string_view name, Value &result) override;
	void Write(const Identifier &name, const Value &value, DUCKDB_V2_SETTING_SCOPE scope) override;
	unique_ptr<CV2Option> GetOption(std::string_view name) override;
	unique_ptr<CV2Option> GetOptionByIndex(idx_t index) override;
	idx_t GetOptionCount() override;

private:
	ClientContext &context;
};

//! A context's factory: the context's catalog and settings.
class CV2ContextFactory final : public CV2Factory {
public:
	explicit CV2ContextFactory(CV2Context &context) : context(context) {
	}

	DatabaseInstance &GetDatabase() override;
	optional_ptr<ClientContext> TryGetClientContext() override;
	void WithTransaction(const std::function<void(ClientContext &)> &fn) override;

private:
	CV2Context &context;
};

//! A context handle: the client context plus how a call through it obtains a transaction.
class CV2Context {
public:
	explicit CV2Context(ClientContext &context)
	    : context(context), factory(*this), config_handle(context),
	      file_system(FileSystem::GetFileSystem(context), QueryContext(context)) {
	}
	virtual ~CV2Context() = default;

	//! Runs `fn` with a transaction active on the context.
	virtual void WithContext(const std::function<void(ClientContext &)> &fn) = 0;

	ClientContext &context;
	CV2ContextFactory factory;
	CV2ClientConfig config_handle;
	CV2FileSystem file_system;
};

inline DatabaseInstance &CV2ContextFactory::GetDatabase() {
	return DatabaseInstance::GetDatabase(context.context);
}

inline optional_ptr<ClientContext> CV2ContextFactory::TryGetClientContext() {
	return &context.context;
}

inline void CV2ContextFactory::WithTransaction(const std::function<void(ClientContext &)> &fn) {
	context.WithContext(fn);
}

//! A context handed to a callback: the context lock is held and a transaction is already active.
class CV2CallbackContext final : public CV2Context {
public:
	explicit CV2CallbackContext(ClientContext &context) : CV2Context(context) {
	}

	void WithContext(const std::function<void(ClientContext &)> &fn) override {
		fn(context);
	}
};

//! A context taken from a connection: each call joins the connection's transaction, or runs in one of its own.
class CV2ConnectionContext final : public CV2Context {
public:
	explicit CV2ConnectionContext(ClientContext &context) : CV2Context(context) {
	}

	void WithContext(const std::function<void(ClientContext &)> &fn) override {
		context.RunFunctionInTransaction([&]() { fn(context); });
	}
};

inline auto Convert(duckdb_v2_context_handle ctx) -> CV2Context * {
	return reinterpret_cast<CV2Context *>(ctx);
}

inline auto Convert(CV2Context *ctx) -> duckdb_v2_context_handle {
	return reinterpret_cast<duckdb_v2_context_handle>(ctx);
}

//! A connection handle: the connection plus the context handle it lends out.
class CV2Connection : public Connection {
public:
	explicit CV2Connection(DuckDB &database) : Connection(database), context_handle(*context) {
	}

	CV2ConnectionContext context_handle;
};

inline auto Convert(duckdb_v2_connection_handle conn) -> CV2Connection * {
	return reinterpret_cast<CV2Connection *>(conn);
}

inline auto Convert(CV2Connection *conn) -> duckdb_v2_connection_handle {
	return reinterpret_cast<duckdb_v2_connection_handle>(conn);
}

using CV2SQLStatement = duckdb::SQLStatement;

inline auto Convert(duckdb_v2_sql_statement_handle stmt) -> CV2SQLStatement * {
	return reinterpret_cast<CV2SQLStatement *>(stmt);
}
inline auto Convert(CV2SQLStatement *stmt) -> duckdb_v2_sql_statement_handle {
	return reinterpret_cast<duckdb_v2_sql_statement_handle>(stmt);
}

//! The extension handle's backing struct is the load state in capi_v2_extension.cpp, not an ExtensionLoader, so it
//! cannot be Convert'ed with a cast: the loader is resolved through the state instead. Valid only while the
//! extension's entrypoint is running.
auto GetExtensionLoader(duckdb_v2_extension_handle handle) -> ExtensionLoader &;

//! Translate the generic (key, value) function property channel into engine properties; defined in
//! capi_v2_func_properties.cpp and shared by the scalar and aggregate set_property entry points.
void SetScalarFunctionProperty(FunctionProperties &props, DUCKDB_V2_FUNCTION_PROPERTY_KEY key,
                               DUCKDB_V2_FUNCTION_PROPERTY_VALUE value);
void SetAggregateFunctionProperty(AggregateFunctionProperties &props, DUCKDB_V2_FUNCTION_PROPERTY_KEY key,
                                  DUCKDB_V2_FUNCTION_PROPERTY_VALUE value);

using CV2FunctionSignature = duckdb::FunctionSignature;

inline auto Convert(duckdb_v2_function_signature_handle func) -> CV2FunctionSignature * {
	return reinterpret_cast<CV2FunctionSignature *>(func);
}

inline auto Convert(CV2FunctionSignature *func) -> duckdb_v2_function_signature_handle {
	return reinterpret_cast<duckdb_v2_function_signature_handle>(func);
}

//! Where an option's current value is read from: a client context's cascade (SESSION -> GLOBAL -> default).
class CV2OptionSource {
public:
	explicit CV2OptionSource(ClientContext &context) : context(&context), config(DBConfig::GetConfig(context)) {
	}
	//! GLOBAL -> default only.
	explicit CV2OptionSource(DatabaseInstance &db) : config(DBConfig::GetConfig(db)) {
	}

	const DBConfig &GetConfig() const {
		return config;
	}
	//! The effective value of `name`, and the scope DuckDB attributes it to. Throws for an unknown option.
	DUCKDB_V2_SETTING_SCOPE ReadValue(std::string_view name, Value &result) const;

private:
	optional_ptr<ClientContext> context;
	const DBConfig &config;
};

class CV2Option {
public:
	Identifier name;
	Value default_value;
	string description;
	bool supports_global = false;
	bool supports_session = false;
	DUCKDB_V2_SETTING_SCOPE default_scope = DUCKDB_V2_SETTING_SCOPE_GLOBAL;
	vector<string> aliases;

	static unique_ptr<CV2Option> FromIndex(const CV2OptionSource &source, idx_t index);
	static unique_ptr<CV2Option> FromName(const CV2OptionSource &source, std::string_view name);
	static idx_t Count(const CV2OptionSource &source);
};

//! The engine's write scope for a V2 scope.
inline SetScope ConvertSetScope(DUCKDB_V2_SETTING_SCOPE scope) {
	switch (scope) {
	case DUCKDB_V2_SETTING_SCOPE_GLOBAL:
		return SetScope::GLOBAL;
	case DUCKDB_V2_SETTING_SCOPE_SESSION:
		return SetScope::SESSION;
	default:
		return SetScope::AUTOMATIC;
	}
}

inline auto Convert(duckdb_v2_option_handle opt) -> CV2Option * {
	return reinterpret_cast<CV2Option *>(opt);
}

inline auto Convert(CV2Option *opt) -> duckdb_v2_option_handle {
	return reinterpret_cast<duckdb_v2_option_handle>(opt);
}

using CV2LogicalType = duckdb::LogicalType;

//! A logical type handle is the LogicalTypeInfo of the type. An owned handle holds one reference to it; a borrowed
//! handle holds none and stays valid for as long as the type it was taken from.

//! Views the type behind a handle without touching its reference count
class CV2LogicalTypeRef {
public:
	explicit CV2LogicalTypeRef(duckdb_v2_logical_type_handle handle)
	    : type(CV2LogicalType::AdoptTypeInfo(*reinterpret_cast<const duckdb::LogicalTypeInfo *>(handle))) {
	}
	~CV2LogicalTypeRef() {
		std::move(type).ReleaseTypeInfo();
	}
	CV2LogicalTypeRef(const CV2LogicalTypeRef &) = delete;
	CV2LogicalTypeRef &operator=(const CV2LogicalTypeRef &) = delete;

	//! Only on a named view - a reference into a temporary view would dangle
	const CV2LogicalType &operator*() const & {
		return type;
	}
	const CV2LogicalType &operator*() const && = delete;
	const CV2LogicalType *operator->() const {
		return &type;
	}

private:
	CV2LogicalType type;
};

inline auto Convert(duckdb_v2_logical_type_handle handle) -> CV2LogicalTypeRef {
	return CV2LogicalTypeRef(handle);
}
inline auto ConvertTypeInfo(const duckdb::LogicalTypeInfo &type_info) -> duckdb_v2_logical_type_handle {
	// the handle is opaque - the type info is only ever read through it
	return reinterpret_cast<duckdb_v2_logical_type_handle>(reinterpret_cast<uintptr_t>(&type_info));
}
//! Creates an owned handle, transferring the reference of "type" to it
inline auto Convert(CV2LogicalType type) -> duckdb_v2_logical_type_handle {
	return ConvertTypeInfo(std::move(type).ReleaseTypeInfo());
}
//! Creates a borrowed handle to "type"
inline auto ConvertBorrowed(const CV2LogicalType &type) -> duckdb_v2_logical_type_handle {
	return ConvertTypeInfo(type.GetTypeInfo());
}
//! Takes back the reference held by an owned handle
inline auto TakeOwnership(duckdb_v2_logical_type_handle handle) -> CV2LogicalType {
	return CV2LogicalType::AdoptTypeInfo(*reinterpret_cast<const duckdb::LogicalTypeInfo *>(handle));
}

using CV2QualifiedName = duckdb::QualifiedName;

inline auto Convert(duckdb_v2_qname_handle name) -> CV2QualifiedName * {
	return reinterpret_cast<CV2QualifiedName *>(name);
}
inline auto Convert(CV2QualifiedName *name) -> duckdb_v2_qname_handle {
	return reinterpret_cast<duckdb_v2_qname_handle>(name);
}

using CV2TableDescription = duckdb::TableDescription;

inline auto Convert(duckdb_v2_table_description_handle desc) -> CV2TableDescription * {
	return reinterpret_cast<CV2TableDescription *>(desc);
}
inline auto Convert(CV2TableDescription *desc) -> duckdb_v2_table_description_handle {
	return reinterpret_cast<duckdb_v2_table_description_handle>(desc);
}

using CV2ColumnDescription = duckdb::ColumnDefinition;

inline auto Convert(duckdb_v2_column_description_handle column) -> CV2ColumnDescription * {
	return reinterpret_cast<CV2ColumnDescription *>(column);
}
inline auto Convert(CV2ColumnDescription *column) -> duckdb_v2_column_description_handle {
	return reinterpret_cast<duckdb_v2_column_description_handle>(column);
}

using CV2ColumnDataCollection = duckdb::ColumnDataCollection;

inline auto Convert(duckdb_v2_column_data_collection_handle cdc) -> CV2ColumnDataCollection * {
	return reinterpret_cast<CV2ColumnDataCollection *>(cdc);
}
inline auto Convert(CV2ColumnDataCollection *cdc) -> duckdb_v2_column_data_collection_handle {
	return reinterpret_cast<duckdb_v2_column_data_collection_handle>(cdc);
}

using CV2Value = duckdb::Value;

inline auto Convert(duckdb_v2_value_handle val) -> CV2Value * {
	return reinterpret_cast<CV2Value *>(val);
}

inline auto Convert(CV2Value *val) -> duckdb_v2_value_handle {
	return reinterpret_cast<duckdb_v2_value_handle>(val);
}

using CV2Expression = duckdb::Expression;

inline auto Convert(duckdb_v2_expression_handle expression) -> CV2Expression * {
	return reinterpret_cast<CV2Expression *>(expression);
}

inline auto Convert(CV2Expression *expression) -> duckdb_v2_expression_handle {
	return reinterpret_cast<duckdb_v2_expression_handle>(expression);
}

using CV2DataChunk = duckdb::DataChunk;

inline auto Convert(duckdb_v2_data_chunk_handle chunk) -> CV2DataChunk * {
	return reinterpret_cast<CV2DataChunk *>(chunk);
}

inline auto Convert(CV2DataChunk *chunk) -> duckdb_v2_data_chunk_handle {
	return reinterpret_cast<duckdb_v2_data_chunk_handle>(chunk);
}

using CV2Vector = duckdb::Vector;

inline auto Convert(duckdb_v2_vector_handle vec) -> CV2Vector * {
	return reinterpret_cast<CV2Vector *>(vec);
}

inline auto Convert(CV2Vector *vec) -> duckdb_v2_vector_handle {
	return reinterpret_cast<duckdb_v2_vector_handle>(vec);
}

struct CV2Schema {
	struct Field {
		string name;
		LogicalType type;
	};
	std::deque<Field> fields;
};

inline auto Convert(duckdb_v2_schema_handle schema) -> CV2Schema * {
	return reinterpret_cast<CV2Schema *>(schema);
}
inline auto Convert(CV2Schema *schema) -> duckdb_v2_schema_handle {
	return reinterpret_cast<duckdb_v2_schema_handle>(schema);
}

//----------------------------------------------------------------------------------------------------------------------
// Error Handling
//----------------------------------------------------------------------------------------------------------------------

// Report a null argument through the same slot contract as WithErrorHandler, without the exception round-trip.
// Defined in capi_v2.cpp.
auto NullArgumentError(duckdb_v2_error_info_handle *err, const char *function, const char *argument) noexcept
    -> DUCKDB_V2_ERROR;

//! Raised for metadata asked for before execution bound the statement that produces it. Reported as
//! ERROR_INPUT_METADATA_DEFERRED
class MetadataDeferredException : public InvalidInputException {
public:
	explicit MetadataDeferredException(const string &msg) : InvalidInputException(msg) {
	}
};

// Classify the exception currently being handled into a V2 error code and detail strings. Must be called from inside
// a catch block. Never throws: if rendering the detail itself fails, it degrades to a bare code with empty detail
// (RESOURCE_OUT_OF_MEMORY on allocation failure). Defined in capi_v2.cpp.
auto RenderCaughtError(DUCKDB_V2_ERROR &code, string &text, optional<string> &raw_message) noexcept -> void;

// The null test behind DUCKDB_CHECK_ARG: a pointer/handle is invalid when null; a string/identifier view is invalid
// when it is null, or when its pointer is null while it carries a non-zero length.
template <class T>
bool IsNullArgument(const T &arg) {
	static_assert(std::is_pointer_v<T>, "DUCKDB_CHECK_ARG takes a pointer or handle");
	return arg == nullptr;
}

inline bool IsNullArgument(const duckdb_v2_str *arg) {
	return !arg || (!arg->ptr && arg->len > 0);
}

// Check if an argument is null and return DUCKDB_V2_ERROR_INPUT_INVALID with a message if it is.
// For use at the top of an API entry point, before WithErrorHandler.
// `err` must be in scope and `__func__` names the entry point in the message.
#define DUCKDB_CHECK_ARG(arg)                                                                                          \
	do {                                                                                                               \
		if (duckdb::capiv2::IsNullArgument(arg)) {                                                                     \
			return duckdb::capiv2::NullArgumentError(err, __func__, #arg);                                             \
		}                                                                                                              \
	} while (0)

// Error code <-> exception type conversion
auto GetErrorCodeFromExceptionType(ExceptionType type) -> DUCKDB_V2_ERROR;
auto TryGetExceptionTypeFromErrorCode(DUCKDB_V2_ERROR code) -> optional<ExceptionType>;

// Backing struct for the opaque duckdb_v2_error_info_handle handle. Allocated
// only on failure paths and only when the caller requested detail (i.e.
// passed a non-null err out-parameter).
struct CV2ErrorInfo {
	DUCKDB_V2_ERROR code = DUCKDB_V2_ERROR_NONE;

	// message: full "<Type> Error: <raw>" (ErrorData::Message()). raw_message: the
	// body with that prefix stripped (ErrorData::RawMessage()), in the engine's
	// rendered form (caret block, or JSON under errors_as_json); empty for a
	// directly-set message. Both written on the error path (WithErrorHandler).
	string message;
	optional<string> raw_message;

	bool HasError() const {
		return code != DUCKDB_V2_ERROR_NONE;
	}

	[[noreturn]] void ThrowAsException() const {
		// Only throw if there's actually an error!
		D_ASSERT(HasError());

		// Rethrow with the exception class the code maps to, so the error class
		// round-trips (mirrors InvokeWithErrorSlot); InvalidInput is the fallback
		// for codes with no specific type.
		if (const auto type = TryGetExceptionTypeFromErrorCode(code)) {
			throw duckdb::Exception(*type, message);
		}
		throw duckdb::InvalidInputException(message);
	}
};

inline auto Convert(duckdb_v2_error_info_handle info) -> CV2ErrorInfo * {
	return reinterpret_cast<CV2ErrorInfo *>(info);
}

inline auto Convert(CV2ErrorInfo *info) -> duckdb_v2_error_info_handle {
	return reinterpret_cast<duckdb_v2_error_info_handle>(info);
}

//! Invoke a function, and convert any exception into an error code + populate an optional error info handle.
//! Nothing may escape across the C ABI, so rendering and reporting the error are themselves guarded: if either
//! fails (allocation failure), the failure degrades to a bare error code with whatever detail could be produced.
template <class T>
DUCKDB_V2_ERROR WithErrorHandler(duckdb_v2_error_info_handle *err, T callback) noexcept {
	auto code = static_cast<DUCKDB_V2_ERROR>(DUCKDB_V2_ERROR_NONE);
	auto text = string();
	optional<string> raw_message;

	try {
		// Invoke the callback
		callback();
	} catch (...) {
		RenderCaughtError(code, text, raw_message);
	}

	// Success leaves the slot untouched.
	// The return code is authoritative, as allocating on every successful call is unnecessary overhead.
	// A stale info from an earlier failure may therefore survive a later successful call, but that is ok.
	// (because the caller checks the code not the slot, and reads *err only after a failing return)
	if (code == DUCKDB_V2_ERROR_NONE) {
		return code;
	}

	// Failure: report detail through the slot if the caller provided one.
	if (err) {
		if (!*err) {
			// Allocate a new error info handle if not provided; if even that fails, the code alone reports it.
			try {
				*err = Convert(new CV2ErrorInfo());
			} catch (const std::bad_alloc &) {
				return DUCKDB_V2_ERROR_RESOURCE_OUT_OF_MEMORY;
			} catch (...) { // NOLINT(bugprone-empty-catch)
				return code;
			}
		}
		auto &out = *Convert(*err);
		out.code = code;
		out.message = std::move(text);
		out.raw_message = std::move(raw_message);
	}

	return code;
}

//----------------------------------------------------------------------------------------------------------------------
// Connection Slot
//----------------------------------------------------------------------------------------------------------------------

struct ConnectionBusySlotV2 : public ClientContextState {
	// The number of `connection_interrupt` calls. A result records it when its statement starts, and an INTERRUPT
	// error is a consumer cancellation (-> CANCELLED status) only if it moved since: a DuckDB-initiated interrupt,
	// e.g. a `max_execution_time` timeout, shares the INTERRUPT exception type (-> error). The DuckDB's internal
	// interrupt_state is not suitable for this, it is set to stop sibling tasks on any error.
	std::atomic<idx_t> cancel_requests {0};
};

inline shared_ptr<ConnectionBusySlotV2> GetBusySlot(ClientContext &context) {
	constexpr auto BUSY_SLOT_STATE_KEY = "v2_connection_busy_slot";
	return context.registered_state->GetOrCreate<ConnectionBusySlotV2>(BUSY_SLOT_STATE_KEY);
}

//----------------------------------------------------------------------------------------------------------------------
// Text sinks
//----------------------------------------------------------------------------------------------------------------------
//! Hands the complete text to a caller-supplied sink, in exactly one call, so
//! the sink sees the full length up front. Its only influence on the outcome is
//! the error slot: a code it sets is rethrown as the matching exception type,
//! which WithErrorHandler then maps straight back.
inline void InvokeTextSink(duckdb_v2_text_sink_fn sink, duckdb_v2_str text, void *user_data) {
	// The slot is always live: sinks populate it, they never allocate or destroy it.
	CV2ErrorInfo info;
	auto handle = Convert(&info);
	sink(&text, user_data, &handle);
	if (info.HasError()) {
		info.ThrowAsException();
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Caller-supplied output buffers
//----------------------------------------------------------------------------------------------------------------------
//! Reports `required` through out_length, then fills a caller-supplied buffer.
//! A null out_data is a size query: report and return without writing.
//! Shared by the entry points that hand back freshly materialized bytes.

template <class WRITE>
void FillCallerBuffer(uint8_t *out_data, idx_t out_capacity, idx_t *out_length, idx_t required,
                      const char *function_name, WRITE write) {
	*out_length = required;
	if (!out_data) {
		return;
	}
	if (out_capacity < required) {
		throw Exception(ExceptionType::OBJECT_SIZE,
		                StringUtil::Format("%s needs a buffer of %llu bytes, but out_capacity is %llu", function_name,
		                                   static_cast<uint64_t>(required), static_cast<uint64_t>(out_capacity)));
	}
	write(out_data);
}

//! Text flavour of FillCallerBuffer: reports the length without the null
//! terminator, but always writes one, so the buffer is usable as a C string.
//! The capacity must therefore cover the terminator too.
inline void FillCallerText(char *out_text, idx_t out_capacity, idx_t *out_length, const string &text,
                           const char *function_name) {
	*out_length = text.size();
	if (!out_text) {
		return;
	}
	const idx_t required = text.size() + 1;
	if (out_capacity < required) {
		throw Exception(ExceptionType::OBJECT_SIZE,
		                StringUtil::Format("%s needs a buffer of %llu bytes including the null terminator, but "
		                                   "out_capacity is %llu",
		                                   function_name, static_cast<uint64_t>(required),
		                                   static_cast<uint64_t>(out_capacity)));
	}
	memcpy(out_text, text.c_str(), required);
}

//----------------------------------------------------------------------------------------------------------------------
// Opaque User Data
//----------------------------------------------------------------------------------------------------------------------
// Owning RAII wrapper for an opaque resource with optional destroy and equals callbacks.
class CV2UserData {
public:
	CV2UserData() = default;

	explicit CV2UserData(void *data, duckdb_v2_opaque_destroy_fn destroy_cb = nullptr,
	                     duckdb_v2_opaque_equals_fn equals_cb = nullptr)
	    : data(data), destroy_cb(destroy_cb), equals_cb(equals_cb) {
	}

	// Moveable
	CV2UserData(CV2UserData &&other) noexcept
	    : data(other.data), destroy_cb(other.destroy_cb), equals_cb(other.equals_cb) {
		other.data = nullptr;
		other.destroy_cb = nullptr;
		other.equals_cb = nullptr;
	}

	CV2UserData &operator=(CV2UserData &&other) noexcept {
		std::swap(data, other.data);
		std::swap(destroy_cb, other.destroy_cb);
		std::swap(equals_cb, other.equals_cb);
		return *this;
	}

	// Not copyable
	CV2UserData(const CV2UserData &) = delete;
	CV2UserData &operator=(const CV2UserData &) = delete;

	~CV2UserData() {
		if (data && destroy_cb) {
			destroy_cb(data);
		}
	}

	bool operator==(const CV2UserData &other) const {
		if (equals_cb) {
			return equals_cb(data, other.data);
		}
		return data == other.data;
	}

	bool operator!=(const CV2UserData &other) const {
		return !(*this == other);
	}

	bool Equals(const CV2UserData &other) const {
		return *this == other;
	}

	void *GetData() const {
		return data;
	}

private:
	void *data = nullptr;
	duckdb_v2_opaque_destroy_fn destroy_cb = nullptr;
	duckdb_v2_opaque_equals_fn equals_cb = nullptr;
};

//----------------------------------------------------------------------------------------------------------------------
// Misc
//----------------------------------------------------------------------------------------------------------------------

inline void BuildParameterMap(const duckdb_v2_identifier_t *parameter_names,
                              const duckdb_v2_value_handle *parameter_values, idx_t parameter_count,
                              const char *function_name, identifier_map_t<BoundParameterData> &out) {
	for (idx_t i = 0; i < parameter_count; i++) {
		auto name = parameter_names ? parameter_names[i] : duckdb_v2_identifier_t {nullptr, 0};
		// A {NULL, 0} view is a valid (empty) name; only a null pointer with a nonzero length is malformed.
		if (!name.ptr && name.len > 0) {
			throw InvalidInputException("malformed parameter name passed to %s", function_name);
		}
		if (!parameter_values[i]) {
			throw InvalidInputException("null parameter value passed to %s", function_name);
		}
		// Named iff the name view is non-empty; otherwise positional
		auto str = ConvertIdentifierName(name);
		Identifier key = (name.ptr && name.len > 0) ? Identifier(str) : Identifier(std::to_string(i + 1));
		out[key] = BoundParameterData(*Convert(parameter_values[i]));
	}
}

} // namespace capiv2

} // namespace duckdb
