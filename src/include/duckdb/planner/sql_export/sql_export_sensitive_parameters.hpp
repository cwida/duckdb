//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/sql_export/sql_export_sensitive_parameters.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/function/function.hpp"

namespace duckdb {
class ClientContext;
class Expression;

//! Resolves rendered invocation arguments against sensitive parameter declarations.
//! A set result is the name to report in the refusal, an empty result allows rendering.
struct SQLExportSensitiveParameters {
	//! Whether the retained value renders as the parameter's declared default - registration metadata, not a secret
	static bool MatchesDeclaredDefault(ClientContext &context, const FunctionParameter &parameter, const Value &value);
	//! The declared parameter that receives the positional argument at the given index
	static optional_ptr<const FunctionParameter> PositionalParameter(const FunctionSignature &signature, idx_t index);
	//! The first sensitive declaration of the signature, including "**kwargs" and typed kwargs options
	static optional<Identifier> FirstSensitiveParameter(const FunctionSignature &signature);
	//! The refusal for the positional argument at the index. A set cast_target means the value sits under a
	//! single cast, whose target must be the declared parameter type for the default exemption to apply.
	static optional<Identifier> RefusePositional(optional_ptr<ClientContext> context,
	                                             const FunctionSignature &signature, idx_t index,
	                                             optional_ptr<const Value> value,
	                                             optional_ptr<const LogicalType> cast_target = nullptr);
	//! The refusal for the named argument
	static optional<Identifier> RefuseNamed(optional_ptr<ClientContext> context, const FunctionSignature &signature,
	                                        const Identifier &name, optional_ptr<const Value> value,
	                                        optional_ptr<const LogicalType> cast_target = nullptr);
	//! The refusal for the bound argument at the index of a [positional | named tail] call layout
	static optional<Identifier> RefuseBoundArgument(optional_ptr<ClientContext> context,
	                                                const FunctionSignature &signature, idx_t positional_count,
	                                                const vector<Identifier> &named_arguments, idx_t index,
	                                                const Expression &argument);
};
} // namespace duckdb
