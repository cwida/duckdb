//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/language_function_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class ClientContext;
class ExpressionBinder;
class FunctionExpression;
class LanguageMacroFunction;
class ParsedExpression;
class ScalarMacroCatalogEntry;
struct BindResult;
struct CreateMacroInfo;

//! Interface an extension registers to bind calls to macro members written in its language. It
//! returns an ordinary bound expression, so a language may translate its functions to SQL or
//! dispatch to its own interpreter. Core stores only the declarative definition.
class LanguageFunctionBinder {
public:
	virtual ~LanguageFunctionBinder() = default;

	//! Validate a definition at CREATE time. Only called when the language is registered,
	//! definitions created or replayed without it are stored unvalidated. The callback may rewrite
	//! member sources and publish documentation, but must not change the member count, the resolved
	//! signatures (parameters, types, return type, language), or the default parameters. The
	//! frozen types compare by representation: signature types must not be reassigned or mutated
	//! in place, even to equivalent values.
	//! Published documentation must carry a member's parameter types, with ANY for untyped
	//! parameters: documentation in any other shape is not released when its member is replaced or
	//! dropped.
	virtual void ValidateCreate(ClientContext &context, CreateMacroInfo &info) = 0;

	//! Bind a call to the already-selected language member in the caller's context. Core has resolved
	//! the overload, named arguments, defaults, and parameter casts: arguments holds one unbound
	//! expression per member parameter, in declaration order. A declared RETURNS type is cast by core.
	virtual BindResult BindScalarCall(ExpressionBinder &binder, ScalarMacroCatalogEntry &entry,
	                                  LanguageMacroFunction &member, vector<unique_ptr<ParsedExpression>> arguments,
	                                  idx_t depth) = 0;
};

} // namespace duckdb
