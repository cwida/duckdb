#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/string.hpp"

namespace duckdb {

class ClientContext;
class DatabaseInstance;
class CastFunctionSet;
struct DBConfig;

class TypeManager {
public:
	explicit TypeManager(DBConfig &config);

	~TypeManager();

	//! Get the CastFunctionSet from the TypeManager
	CastFunctionSet &GetCastFunctions();
	const CastFunctionSet &GetCastFunctions() const;

	//! Try to parse and bind a logical type from a string. Throws an exception if the type could not be parsed.
	LogicalType ParseLogicalType(const string &type_str, ClientContext &context) const;
	//! Parse and bind a logical type from a string with the default parser, resolving the built-in types only.
	LogicalType ParseLogicalType(const string &type_str) const;

	//! Get the TypeManager from the DatabaseInstance
	static TypeManager &Get(DatabaseInstance &db);
	static TypeManager &Get(ClientContext &context);

private:
	//! This has to be a function pointer to avoid the compiler inlining the implementation and
	//! blowing up the binary size of extensions that include type_manager.hpp
	LogicalType (*parse_function)(const string &, ClientContext &);

	//! The set of cast functions
	unique_ptr<CastFunctionSet> cast_functions;
};

} // namespace duckdb
