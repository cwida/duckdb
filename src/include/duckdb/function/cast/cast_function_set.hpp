//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/cast/cast_function_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/combine_types_rule.hpp"

namespace duckdb {
struct MapCastInfo;
struct MapCastNode;
struct DBConfig;

typedef BoundCastInfo (*bind_cast_function_t)(BindCastInput &input, const LogicalType &source,
                                              const LogicalType &target);
typedef int64_t (*implicit_cast_cost_t)(const LogicalType &from, const LogicalType &to);

struct GetCastFunctionInput {
	explicit GetCastFunctionInput(optional_ptr<ClientContext> context = nullptr) : context(context) {
	}
	explicit GetCastFunctionInput(ClientContext &context) : context(&context) {
	}

	optional_ptr<ClientContext> context;
	QueryLocation query_location;
};

struct BindCastFunction {
	BindCastFunction(bind_cast_function_t function, // NOLINT: allow implicit cast
	                 unique_ptr<BindCastInfo> info = nullptr);

	bind_cast_function_t function;
	unique_ptr<BindCastInfo> info;
};

class CastFunctionSet {
public:
	CastFunctionSet();
	explicit CastFunctionSet(DBConfig &config);

public:
	DUCKDB_API static CastFunctionSet &Get(ClientContext &context);
	DUCKDB_API static const CastFunctionSet &Get(const ClientContext &context);
	DUCKDB_API static CastFunctionSet &Get(DatabaseInstance &db);

	//! Returns a cast function (from source -> target)
	//! Note that this always returns a function - since a cast is ALWAYS possible if the value is NULL
	DUCKDB_API BoundCastInfo GetCastFunction(const LogicalType &source, const LogicalType &target,
	                                         GetCastFunctionInput &input);
	//! Returns the implicit cast cost of casting from source -> target
	//! -1 means an implicit cast is not possible
	DUCKDB_API int64_t ImplicitCastCost(optional_ptr<ClientContext> context, const LogicalType &source,
	                                    const LogicalType &target);
	DUCKDB_API static int64_t ImplicitCastCost(ClientContext &context, const LogicalType &source,
	                                           const LogicalType &target);
	DUCKDB_API static int64_t ImplicitCastCost(DatabaseInstance &db, const LogicalType &source,
	                                           const LogicalType &target);
	//! Register a new cast function from source to target
	DUCKDB_API void RegisterCastFunction(const LogicalType &source, const LogicalType &target, BoundCastInfo function,
	                                     int64_t implicit_cast_cost = -1);
	DUCKDB_API void RegisterCastFunction(const LogicalType &source, const LogicalType &target,
	                                     bind_cast_function_t bind, int64_t implicit_cast_cost = -1);
	//! Whether a registered cast can change this default conversion, including nested casts.
	//! Data-dependent VARIANT conversions are treated conservatively.
	DUCKDB_API bool CanOverrideDefaultCast(const LogicalType &source, const LogicalType &target) const;

	//! Register a combine rule for LogicalType::TryGetMaxLogicalType, consulted before previously registered rules
	//! and the built-in rules
	DUCKDB_API void RegisterCombineTypesRule(CombineTypesRule rule);
	//! Run `rules` against (left, right); the first matching rule wins and writes its outcome to `success`.
	//! Returns false if no rule matched.
	static bool TryCombineTypes(const vector<CombineTypesRule> &rules, LogicalTypeResolver &resolver,
	                            const LogicalType &left, const LogicalType &right, LogicalType &result, bool &success);
	//! Same, over this set's rules (registered + built-in)
	bool TryCombineTypes(LogicalTypeResolver &resolver, const LogicalType &left, const LogicalType &right,
	                     LogicalType &result, bool &success);

private:
	optional_ptr<DBConfig> config;
	//! Built-in fallback used after all registered cast providers have declined the cast
	BindCastFunction default_bind_function;
	//! Registered cast providers, searched newest-first before the built-in fallback
	vector<BindCastFunction> registered_bind_functions;
	vector<CombineTypesRule> combine_rules;
	//! If any custom cast functions have been defined using RegisterCastFunction, this holds the map
	optional_ptr<MapCastInfo> map_info;
	//! A default-only binding probe observes registrations without invoking them.
	optional_ptr<const CastFunctionSet> registration_probe;
	bool registered_cast_found = false;

private:
	void RegisterCastFunction(const LogicalType &source, const LogicalType &target, MapCastNode node);
	bool HasRegisteredCast(const LogicalType &source, const LogicalType &target) const;
};

} // namespace duckdb
