//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/collation_binding.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/common/enums/collation_type.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {
struct MapCastInfo;
struct MapCastNode;
struct DBConfig;

//! A scalar function applied to make a value byte-comparable, looked up in the system catalog
struct CollationFunctionName {
	CollationFunctionName(Identifier schema_p, Identifier name_p)
	    : schema(std::move(schema_p)), name(std::move(name_p)) {
	}
	explicit CollationFunctionName(Identifier name_p)
	    : CollationFunctionName(Identifier::DefaultSchema(), std::move(name_p)) {
	}

	Identifier schema;
	Identifier name;
};

//! Returns the (ordered) list of scalar functions that need to be applied to a value of the given type to make it
//! byte-comparable under its collation. Returns an empty list if no collation needs to be applied.
typedef vector<CollationFunctionName> (*get_collation_functions_t)(ClientContext &context, const LogicalType &sql_type,
                                                                   CollationType type);

struct CollationCallback {
	explicit CollationCallback(get_collation_functions_t get_collation_functions_p)
	    : get_collation_functions(get_collation_functions_p) {
	}

	get_collation_functions_t get_collation_functions;
};

struct CollationFunction {
	//! The scalar function that maps a string to its sort key
	CollationFunctionName function;
	//! Whether the collation can be combined with other collations, e.g. nocase.noaccent
	bool combinable;
};

//! A string collation "x" is the scalar function "x" in the "collations" schema of the system catalog: it takes a
//! VARCHAR and returns a VARCHAR or BLOB that is compared (and sorted) instead of the original string.
struct CollationFunctions {
	//! The schema of the system catalog that holds the collation functions
	static constexpr const char *SCHEMA = "collations";
	//! Boolean function property that marks a collation as combinable, an absent property means false
	static constexpr const char *COMBINABLE_PROPERTY = "duckdb.collation.combinable";

	//! The schema that holds the collation functions
	DUCKDB_API static Identifier GetSchema();
	//! Marks the function as a collation function that can be combined with other collations
	DUCKDB_API static void SetCombinable(ScalarFunction &function);
	//! Looks up the function of a collation (autoloading its extension), throws if the collation does not exist
	DUCKDB_API static CollationFunction Lookup(ClientContext &context, const string &collation);
	//! The names of all registered collations
	DUCKDB_API static vector<string> GetCollations(ClientContext &context);
};

class CollationBinding {
public:
	CollationBinding();

public:
	DUCKDB_API static CollationBinding &Get(ClientContext &context);
	DUCKDB_API static CollationBinding &Get(DatabaseInstance &db);

	DUCKDB_API void RegisterCollation(CollationCallback callback);
	DUCKDB_API bool PushCollation(ClientContext &context, unique_ptr<Expression> &source, const LogicalType &sql_type,
	                              CollationType type) const;

private:
	vector<CollationCallback> collations;
};

} // namespace duckdb
