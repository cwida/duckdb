//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/type_constructor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/query_location.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/function_set_base.hpp"

namespace duckdb {
class ClientContext;

//! A single argument passed to a type constructor - an optionally named constant value
class TypeArgument {
public:
	TypeArgument(string name_p, Value value_p) : name(std::move(name_p)), value(std::move(value_p)) {
	}
	//! Arguments that come from source text carry their location, so errors about them can point at it. Arguments
	//! synthesized elsewhere - the C API, the catalog-free default bind - leave it invalid.
	TypeArgument(string name_p, Value value_p, QueryLocation query_location_p)
	    : name(std::move(name_p)), value(std::move(value_p)), query_location(query_location_p) {
	}
	QueryLocation GetQueryLocation() const {
		return query_location;
	}
	const string &GetName() const {
		return name;
	}
	const Value &GetValue() const {
		return value;
	}
	bool HasName() const {
		return !name.empty();
	}
	bool IsNamed(const char *name_to_check) const {
		return StringUtil::CIEquals(name, name_to_check);
	}
	bool IsNotNull() const {
		return !value.IsNull();
	}
	const LogicalType &GetType() const {
		return value.type();
	}

private:
	string name;
	Value value;
	QueryLocation query_location;
};

struct BindLogicalTypeInput {
	optional_ptr<ClientContext> context;
	const LogicalType &base_type;
	const vector<TypeArgument> &modifiers;
	//! Location of the type expression as a whole, for errors that are not about one particular modifier
	QueryLocation query_location;

	//! Location of the given modifier, falling back to the type itself - a modifier filled in from a parameter
	//! default has no source text of its own.
	QueryLocation GetLocation(idx_t modifier_idx) const {
		if (modifier_idx < modifiers.size()) {
			auto location = modifiers[modifier_idx].GetQueryLocation();
			if (location.IsValid()) {
				return location;
			}
		}
		return query_location;
	}
};

//! The type to bind type modifiers to a type
typedef LogicalType (*bind_logical_type_function_t)(BindLogicalTypeInput &input);

//! A single overload of a type constructor: a signature describing the modifiers it accepts, plus the callback that
//! turns them into a LogicalType. The arguments handed to the callback are already matched against the signature.
class TypeConstructor : public SimpleFunction {
public:
	static constexpr const FunctionKind KIND = FunctionKind::TYPE;
	FunctionKind GetKind() const override {
		return KIND;
	}
	DUCKDB_API TypeConstructor(Identifier name, FunctionSignature signature, bind_logical_type_function_t bind);
	DUCKDB_API explicit TypeConstructor(FunctionSignature signature, bind_logical_type_function_t bind);

	//! An empty constructor signature: no modifiers, resolving to a type. Add parameters to it to declare modifiers.
	DUCKDB_API static FunctionSignature Signature();

	//! A constructor that accepts no modifiers and resolves to the entry's own type
	DUCKDB_API static TypeConstructor Identity(Identifier name);

	//! A catch-all constructor wrapping a raw bind function: accepts any modifiers, leaving the bind function to
	//! check them. Used for types registered through the untyped bind_logical_type_function_t API.
	DUCKDB_API static TypeConstructor Unchecked(Identifier name, bind_logical_type_function_t bind);

	bool Equal(const TypeConstructor &other) const {
		return signature.Equal(other.GetSignature());
	}

	bind_logical_type_function_t GetBindFunction() const {
		return bind_function;
	}

	//! As MAP(key TYPE, value TYPE)
	DUCKDB_API string ToString() const override;

private:
	bind_logical_type_function_t bind_function;
};

class TypeConstructorSet : public FunctionSet<TypeConstructor> {
public:
	DUCKDB_API TypeConstructorSet();
	DUCKDB_API explicit TypeConstructorSet(Identifier name);

	//! Select the constructor matching the given arguments and invoke it. The context may be null, in which case only
	//! the built-in implicit cast rules are available. The location is that of the type expression as a whole, and is
	//! only available when the type came from source text.
	DUCKDB_API LogicalType Bind(optional_ptr<ClientContext> context, const LogicalType &base_type,
	                            const vector<TypeArgument> &arguments,
	                            QueryLocation query_location = QueryLocation()) const;
};

} // namespace duckdb
