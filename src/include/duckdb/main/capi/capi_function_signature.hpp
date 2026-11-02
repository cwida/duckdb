//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/capi/capi_function_signature.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string_util.hpp"
#include "duckdb/function/function.hpp"

#include <algorithm>

namespace duckdb {

//! The C APIs let a caller declare parameters and varargs in any order, while a signature expects its parameters
//! ordered by kind. These rebuild the signature in that order.
struct CAPIFunctionSignature {
	//! Adds the parameter after the existing ones of its kind
	static void AddParameter(FunctionSignature &signature, FunctionParameter parameter) {
		auto parameters = signature.GetParameters();
		parameters.push_back(std::move(parameter));
		SetParameters(signature, std::move(parameters));
	}

	//! Adds a positional-only parameter named "col<N>", after the existing positional-only ones
	static void AddPositionalOnly(FunctionSignature &signature, LogicalType type) {
		auto name = Identifier(StringUtil::Format("col%d", signature.GetPositionalOnlyParameterCount()));
		AddParameter(signature,
		             FunctionParameter(std::move(name), std::move(type), {}, FunctionParameterKind::POSITIONAL_ONLY));
	}

	//! Replaces the "*args" by one of the given type, or removes it if the type is INVALID. A named argument the
	//! signature does not declare stays an error, as the callback could not tell its name
	static void SetArgs(FunctionSignature &signature, const LogicalType &type) {
		SetVariadic(signature, type, false);
	}

	//! The same, adding a "**kwargs" of the type too - a named argument the signature does not declare is received
	//! as a trailing value, as v1 functions always received it
	static void SetVarArgs(FunctionSignature &signature, const LogicalType &type) {
		SetVariadic(signature, type, true);
	}

private:
	static void SetVariadic(FunctionSignature &signature, const LogicalType &type, bool with_kwargs) {
		vector<FunctionParameter> parameters;
		for (auto &param : signature.GetParameters()) {
			if (!param.IsVariadic()) {
				parameters.push_back(param);
			}
		}
		if (type.id() != LogicalTypeId::INVALID) {
			parameters.emplace_back("args", type, optional<Value>(), FunctionParameterKind::VAR_POSITIONAL);
			if (with_kwargs) {
				parameters.emplace_back("kwargs", type, optional<Value>(), FunctionParameterKind::VAR_KEYWORD);
			}
		}
		SetParameters(signature, std::move(parameters));
	}

	static void SetParameters(FunctionSignature &signature, vector<FunctionParameter> parameters) {
		// the kinds are declared in the order a signature expects them
		std::stable_sort(
		    parameters.begin(), parameters.end(),
		    [](const FunctionParameter &lhs, const FunctionParameter &rhs) { return lhs.GetKind() < rhs.GetKind(); });
		// a typed "**kwargs" keeps its schema
		auto typed_kwargs = signature.GetTypedKwargs();
		optional<Identifier> kwargs_name;
		if (typed_kwargs && !parameters.empty() && parameters.back().GetKind() == FunctionParameterKind::VAR_KEYWORD) {
			kwargs_name = parameters.back().GetName();
			parameters.pop_back();
		}
		FunctionSignature result(std::move(parameters), signature.GetReturnType());
		if (kwargs_name) {
			result.AddTypedKwargs(*kwargs_name, *typed_kwargs);
		}
		signature = std::move(result);
	}
};

} // namespace duckdb
