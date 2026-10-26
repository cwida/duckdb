//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/language_function_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/parsed_data/create_function_info.hpp"

namespace duckdb {

class ClientContext;
class ExpressionBinder;
class FunctionExpression;
class LanguageMacroFunction;
class MacroFunction;
class ParsedExpression;
class ScalarMacroCatalogEntry;
struct BindResult;

//! The slice of a definition a ValidateCreate callback can act on: it reads the members, may
//! rewrite a member's source, and publishes documentation. Everything else about the definition
//! (the target, the member count, the resolved signatures, the defaults) is out of reach by type.
//! The view borrows from the definition under validation: it is only valid inside the callback.
class LanguageFunctionCreateInfo {
public:
	LanguageFunctionCreateInfo(const Identifier &function_name, vector<unique_ptr<MacroFunction>> &members,
	                           vector<FunctionDescription> &descriptions)
	    : function_name(function_name), members(members), descriptions(descriptions) {
	}

	//! The unqualified function name, for error messages
	const Identifier &GetFunctionName() const {
		return function_name;
	}
	//! The number of members in the definition under validation
	idx_t MemberCount() const {
		return members.size();
	}
	//! Read one member. DuckDB's unique_ptr propagates mutability through const containers, so
	//! access goes through this accessor, which pins the member itself const
	const MacroFunction &Member(idx_t member_idx) const {
		return *members[member_idx];
	}
	//! Rewrite one member's stored source
	void SetMemberSource(idx_t member_idx, string source);
	//! Publish documentation. A description must carry a member's parameter types, with ANY for
	//! untyped parameters: documentation in any other shape is not released when its member is
	//! replaced or dropped
	void AddDescription(FunctionDescription description) {
		descriptions.push_back(std::move(description));
	}

private:
	const Identifier &function_name;
	vector<unique_ptr<MacroFunction>> &members;
	vector<FunctionDescription> &descriptions;
};

//! Interface an extension registers to bind calls to macro members written in its language. It
//! returns an ordinary bound expression, so a language may translate its functions to SQL or
//! dispatch to its own interpreter. Core stores only the declarative definition.
class LanguageFunctionBinder {
public:
	virtual ~LanguageFunctionBinder() = default;

	//! Validate a definition at CREATE time. Only called when the language is registered,
	//! definitions created or replayed without it are stored unvalidated. The info exposes exactly
	//! what the callback may do: read the members, rewrite member sources, and publish
	//! documentation.
	virtual void ValidateCreate(ClientContext &context, LanguageFunctionCreateInfo &info) = 0;

	//! Bind a call to the already-selected language member in the caller's context. Core has resolved
	//! the overload, named arguments, defaults, and parameter casts: arguments holds one unbound
	//! expression per member parameter, in declaration order. A declared RETURNS type is cast by core.
	virtual BindResult BindScalarCall(ExpressionBinder &binder, ScalarMacroCatalogEntry &entry,
	                                  LanguageMacroFunction &member, vector<unique_ptr<ParsedExpression>> arguments,
	                                  idx_t depth) = 0;
};

} // namespace duckdb
