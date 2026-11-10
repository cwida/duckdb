//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/named_parameter_map.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/external_dependencies.hpp"
#include "duckdb/common/enums/function_errors.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {
class CatalogEntry;
class Catalog;
class ClientContext;
class Expression;
class ExpressionExecutor;
class Transaction;

class AggregateFunction;
class AggregateFunctionSet;
class CopyFunction;
class PragmaFunction;
class PragmaFunctionSet;
class ScalarFunctionSet;
class ScalarFunction;
class TableFunctionSet;
class TableFunction;
class SimpleFunction;
class FunctionProperties;
class WindowFunction;
class WindowFunctionSet;
class BoundSimpleFunction;

struct PragmaInfo;

//! The default null handling is NULL in, NULL out
enum class FunctionNullHandling : uint8_t { DEFAULT_NULL_HANDLING = 0, SPECIAL_HANDLING = 1 };
//! The stability of the function, used by the optimizer
//! CONSISTENT              -> this function always returns the same result when given the same input, no variance
//! CONSISTENT_WITHIN_QUERY -> this function returns the same result for the same input WITHIN the same query, but the
//!                            result might change across queries (e.g. NOW(), or functions whose bind reads settings
//!                            such as TimeZone). The optimizer treats it as consistent, statements that call it are
//!                            bound again at every execution, and index keys cannot contain it
//! VOLATILE                -> the result of this function might change per row (e.g. RANDOM())
enum class FunctionStability : uint8_t { CONSISTENT = 0, VOLATILE = 1, CONSISTENT_WITHIN_QUERY = 2 };

//! How to handle collations
//! PROPAGATE_COLLATIONS        -> this function combines collation from its inputs and emits them again (default)
//! PUSH_COMBINABLE_COLLATIONS  -> combinable collations are executed for the input arguments
//! IGNORE_COLLATIONS           -> collations are completely ignored by the function
//! PUSH_DISTINCT_COLLATIONS    -> aggregate DISTINCT arguments are replaced by their comparison keys
//!                               Only valid for aggregates that can consume keys instead of original values.
//!                               Ordinary (non-DISTINCT) calls keep their original arguments.
enum class FunctionCollationHandling : uint8_t {
	PROPAGATE_COLLATIONS = 0,
	PUSH_COMBINABLE_COLLATIONS = 1,
	IGNORE_COLLATIONS = 2,
	PUSH_DISTINCT_COLLATIONS = 3
};

struct FunctionData {
public:
	DUCKDB_API virtual ~FunctionData();

	DUCKDB_API virtual unique_ptr<FunctionData> Copy() const = 0;
	DUCKDB_API virtual bool Equals(const FunctionData &other) const = 0;
	DUCKDB_API static bool Equals(const FunctionData *left, const FunctionData *right);
	DUCKDB_API virtual bool SupportStatementCache() const;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
	// FIXME: this function should be removed in the future
	template <class TARGET>
	TARGET &CastNoConst() const {
		return const_cast<TARGET &>(Cast<TARGET>()); // NOLINT: FIXME
	}
};

struct TableFunctionData : public FunctionData {
	// used to pass on projections to table functions that support them. NB, can contain COLUMN_IDENTIFIER_ROW_ID
	vector<idx_t> column_ids;

	DUCKDB_API ~TableFunctionData() override;

	DUCKDB_API unique_ptr<FunctionData> Copy() const override;
	DUCKDB_API bool Equals(const FunctionData &other) const override;
};

struct FunctionLocalState {
	DUCKDB_API virtual ~FunctionLocalState();

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct FunctionParameters {
	vector<Value> values;
	named_argument_map_t named_parameters;
	//! The pragma function the parameters were bound for
	optional_ptr<const PragmaFunction> function;
};

//! How a parameter receives its arguments, mirroring Python's parameter kinds, in the order a signature declares them
//! POSITIONAL_ONLY -> declared before "/", can only be passed by position. Its name is invisible to a caller, so an
//!                    argument of that name is unmatched and reaches "**kwargs" instead
//! STANDARD        -> can be passed by position or by name
//! VAR_POSITIONAL  -> "*args", receives all remaining positional arguments
//! KEYWORD_ONLY    -> declared after "*args", can only be passed by name
//! VAR_KEYWORD     -> "**kwargs", receives all named arguments that do not match another parameter
enum class FunctionParameterKind : uint8_t {
	POSITIONAL_ONLY = 0,
	STANDARD = 1,
	VAR_POSITIONAL = 2,
	KEYWORD_ONLY = 3,
	VAR_KEYWORD = 4
};

class FunctionParameter {
public:
	FunctionParameter(Identifier name, LogicalType type, optional<Value> value = {},
	                  FunctionParameterKind kind = FunctionParameterKind::STANDARD)
	    : name(std::move(name)), type(std::move(type)), kind(kind) {
		if (value) {
			default_value = make_shared_ptr<Value>(std::move(*value));
		} else {
			default_value = nullptr;
		}
	}

	string ToString() const;

	bool operator==(const FunctionParameter &other) const;
	bool operator!=(const FunctionParameter &other) const;

	auto GetName() const -> const Identifier & {
		return name;
	}
	auto SetName(Identifier name_p) -> void {
		name = std::move(name_p);
	}

	auto GetType() const -> const LogicalType & {
		return type;
	}
	auto SetType(LogicalType type_p) -> void {
		type = std::move(type_p);
	}

	auto GetDefaultValue() const -> optional_ptr<Value> {
		return default_value.get();
	}
	auto SetDefaultValue(Value value) -> void {
		default_value = make_shared_ptr<Value>(std::move(value));
	}
	auto HasDefaultValue() const -> bool {
		return default_value != nullptr;
	}

	auto GetKind() const -> FunctionParameterKind {
		return kind;
	}
	//! Whether this is a "*args" or "**kwargs" parameter
	auto IsVariadic() const -> bool {
		return kind == FunctionParameterKind::VAR_POSITIONAL || kind == FunctionParameterKind::VAR_KEYWORD;
	}
	//! Whether a caller can pass this parameter by position
	auto AcceptsPosition() const -> bool {
		return kind == FunctionParameterKind::STANDARD || kind == FunctionParameterKind::POSITIONAL_ONLY;
	}
	//! Whether a caller can pass this parameter by name
	auto AcceptsName() const -> bool {
		return kind == FunctionParameterKind::STANDARD || kind == FunctionParameterKind::KEYWORD_ONLY;
	}

	//! Whether the parameter receives credential material that must not appear in generated SQL
	auto IsSensitive() const -> bool {
		return sensitive;
	}
	auto SetSensitive() -> void {
		sensitive = true;
	}

private:
	Identifier name;
	LogicalType type;
	shared_ptr<Value> default_value;
	FunctionParameterKind kind;
	bool sensitive = false;
};

//! An option a function receives through its "**kwargs" parameter
class TypedKwarg {
public:
	TypedKwarg(Identifier name, LogicalType type);

	//! The name the function receives the option under
	Identifier name;
	//! Other names a caller can pass the option by
	vector<Identifier> aliases;
	//! The type a passed value is cast to - ANY passes it through as-is, for the function to check
	LogicalType type;
	//! Whether the option receives credential material that must not appear in generated SQL
	bool sensitive = false;

public:
	DUCKDB_API bool operator==(const TypedKwarg &other) const;
	DUCKDB_API bool operator!=(const TypedKwarg &other) const;
	DUCKDB_API string ToString() const;
};

//! The "options" a function receives through its "**kwargs" parameter
//! This is basically just a way to have the binder enforce a set of accepted types and names
//! as trailing keyword-only parameters, without having to declare them as actual parameters.
//! "options" are not really part of the function signature, so they do not affect overload resolution
class TypedKwargs {
public:
	//! Adds an option - a call that leaves it out does not pass it, the function decides what that means
	DUCKDB_API TypedKwargs &Add(Identifier name, LogicalType type);
	//! Adds another name for the option added last
	DUCKDB_API TypedKwargs &Alias(Identifier alias);
	//! Marks the option added last as sensitive - its value must not appear in generated SQL
	DUCKDB_API TypedKwargs &Sensitive();

	//! A schema holding the options of this one followed by those of the other
	DUCKDB_API TypedKwargs Merge(const TypedKwargs &other) const;

	//! The option a name or an alias refers to, or nullptr if the schema declares no such name
	DUCKDB_API optional_ptr<const TypedKwarg> Find(const Identifier &name) const;
	//! The options in the order they were added
	DUCKDB_API const vector<TypedKwarg> &GetOptions() const;
	//! Every name and alias the schema declares
	DUCKDB_API vector<Identifier> GetNames() const;

	DUCKDB_API bool operator==(const TypedKwargs &other) const;
	DUCKDB_API bool operator!=(const TypedKwargs &other) const;
	DUCKDB_API hash_t Hash() const;

	//! Adding does not check the names - this does, once the schema is complete
	//! @throws InvalidInputException if a name or an alias is declared twice
	DUCKDB_API void Verify() const;

private:
	vector<TypedKwarg> options;
};

class FunctionSignature {
public:
	FunctionSignature() = default;

	FunctionSignature(vector<FunctionParameter> parameters, LogicalType return_type)
	    : parameters(std::move(parameters)), return_type(std::move(return_type)) {
	}
	FunctionSignature(vector<LogicalType> arguments, LogicalType return_type) : return_type(std::move(return_type)) {
		for (auto &arg : arguments) {
			AddParameter(std::move(arg));
		}
	}

	string ToString() const;

	bool operator==(const FunctionSignature &other) const;
	bool operator!=(const FunctionSignature &other) const;

	bool Equal(const FunctionSignature &other) const;
	//! Whether both declare the same overload. As in C++ and Postgres, the types identify an overload, and a call that
	//! matches two distinct ones is reported as ambiguous when it is made. They are the same when:
	//! - the parameters a caller can pass by position match in order, by type - whatever their names, and whether
	//!   they are positional-only or not
	//! - "*args" and "**kwargs" match, by type
	//! - the keyword-only parameters match by name and type, in any order - a caller can only pass them by name
	//! Defaults and the options of "**kwargs" are ignored: every call the overload without a default accepts, both
	//! accept at the same cost, and options take no part in overload selection
	DUCKDB_API bool IsSameOverload(const FunctionSignature &other) const;

public:
	auto GetParameter(idx_t index) const -> const FunctionParameter & {
		return parameters[index];
	}
	auto GetParameter(idx_t index) -> FunctionParameter & {
		return parameters[index];
	}
	auto GetParameters() const -> const vector<FunctionParameter> & {
		return parameters;
	}
	auto GetParameterCount() const -> idx_t {
		return parameters.size();
	}
	auto GetReturnType() const -> const LogicalType & {
		return return_type;
	}
	auto SetReturnType(LogicalType return_type_p) -> void {
		return_type = std::move(return_type_p);
	}

	auto IsVariadic() const -> bool {
		for (auto &param : parameters) {
			if (param.IsVariadic()) {
				return true;
			}
		}
		return false;
	}

	auto GetArgs() const -> optional_ptr<const FunctionParameter> {
		for (auto &param : parameters) {
			if (param.GetKind() == FunctionParameterKind::VAR_POSITIONAL) {
				return &param;
			}
		}
		return nullptr;
	}

	auto GetKwargs() const -> optional_ptr<const FunctionParameter> {
		for (auto &param : parameters) {
			if (param.GetKind() == FunctionParameterKind::VAR_KEYWORD) {
				return &param;
			}
		}
		return nullptr;
	}

	//! The schema the "**kwargs" parameter receives, or nullptr if it accepts any kwargs
	auto GetTypedKwargs() const -> optional_ptr<const TypedKwargs> {
		return typed_kwargs.get();
	}

	auto AddParameter(Identifier name, LogicalType type, optional<Value> default_value = {},
	                  FunctionParameterKind kind = FunctionParameterKind::STANDARD) -> FunctionSignature & {
		parameters.emplace_back(std::move(name), std::move(type), std::move(default_value), kind);
		return *this;
	}
	//! Adds a positional-only parameter named "col<N>", after its position - a caller cannot pass it by a name that
	//! was never declared
	auto AddParameter(LogicalType type) -> FunctionSignature & {
		auto name = Identifier(StringUtil::Format("col%d", parameters.size()));
		return AddPositionalOnly(std::move(name), std::move(type));
	}
	auto AddKeywordOnly(Identifier name, LogicalType type, optional<Value> default_value = {}) -> FunctionSignature & {
		return AddParameter(std::move(name), std::move(type), std::move(default_value),
		                    FunctionParameterKind::KEYWORD_ONLY);
	}
	auto AddPositionalOnly(Identifier name, LogicalType type) -> FunctionSignature & {
		return AddParameter(std::move(name), std::move(type), {}, FunctionParameterKind::POSITIONAL_ONLY);
	}
	auto AddArgs(Identifier name, LogicalType type) -> FunctionSignature & {
		return AddParameter(std::move(name), std::move(type), {}, FunctionParameterKind::VAR_POSITIONAL);
	}
	auto AddKwargs(Identifier name, LogicalType type) -> FunctionSignature & {
		return AddParameter(std::move(name), std::move(type), {}, FunctionParameterKind::VAR_KEYWORD);
	}

	//! Marks the parameter added last as sensitive - its value must not appear in generated SQL
	auto Sensitive() -> FunctionSignature & {
		if (parameters.empty()) {
			throw InternalException("FunctionSignature::Sensitive called before any parameter was added");
		}
		parameters.back().SetSensitive();
		return *this;
	}

	//! Adds a "**name" parameter that receives only the options of the given schema
	DUCKDB_API auto AddTypedKwargs(Identifier name, TypedKwargs schema) -> FunctionSignature &;
	//! The same, with the schema declared by the given callback
	DUCKDB_API auto WithTypedKwargs(Identifier name, const std::function<void(TypedKwargs &)> &configure)
	    -> FunctionSignature &;
	//! Adds options to the schema of the "**kwargs" parameter
	//! @throws InternalException if the signature has no typed "**kwargs" parameter
	DUCKDB_API auto ExtendTypedKwargs(const std::function<void(TypedKwargs &)> &configure) -> FunctionSignature &;

	//! Returns the index of the parameter a caller can pass by the given name. Skips the variadic parameters and the
	//! positional-only ones, whose names a caller cannot use
	auto GetParameterIndexByName(const Identifier &name) const -> optional_idx {
		// Parameter names are matched case-insensitively, consistent with SQL identifier semantics.
		for (idx_t i = 0; i < parameters.size(); i++) {
			if (parameters[i].AcceptsName() && parameters[i].GetName() == name) {
				return i;
			}
		}
		return optional_idx();
	}

	//! The number of leading parameters that can be passed by position
	auto GetPositionalParameterCount() const -> idx_t {
		idx_t result = 0;
		while (result < parameters.size() && parameters[result].AcceptsPosition()) {
			result++;
		}
		return result;
	}

	//! The number of leading parameters that can ONLY be passed by position
	auto GetPositionalOnlyParameterCount() const -> idx_t {
		idx_t result = 0;
		while (result < parameters.size() && parameters[result].GetKind() == FunctionParameterKind::POSITIONAL_ONLY) {
			result++;
		}
		return result;
	}

	auto GetRequiredParameterCount() const -> idx_t {
		idx_t result = 0;
		for (const auto &param : parameters) {
			if (!param.IsVariadic() && !param.HasDefaultValue()) {
				result++;
			}
		}
		return result;
	}

	//! Puts the named arguments in binding order: every keyword-only parameter in declaration order, with its default
	//! if the call left it out, then the arguments "**kwargs" receives in the order they were passed
	//! A default is cast to its parameter's type, as FunctionBinder::CastToParameterType casts an argument
	DUCKDB_API void FillNamedDefaults(ClientContext &context, named_argument_map_t &named_parameters) const;

	DUCKDB_API void Verify() const;

	hash_t Hash() const;

private:
	vector<FunctionParameter> parameters;
	shared_ptr<TypedKwargs> typed_kwargs; // optional "schema" for the accepted **kwargs
	LogicalType return_type;
};

//! Function is the base class used for any type of function (scalar, aggregate or simple function)
class Function {
public:
	DUCKDB_API explicit Function(Identifier name);
	DUCKDB_API virtual ~Function();

	//! The name of the function
	Identifier name;
	//! Additional Information to specify function from it's name
	string extra_info;

public:
	auto SetName(Identifier name_p) -> void {
		name = std::move(name_p);
	}
	auto SetSchemaName(Identifier schema_name_p) -> void {
		qualified_name = QualifiedName(GetCatalogName(), std::move(schema_name_p), name);
	}
	auto SetCatalogName(Identifier catalog_name_p) -> void {
		auto path = qualified_name.Path();
		if (path.size() < 3) {
			qualified_name = QualifiedName(std::move(catalog_name_p), GetSchemaName(), name);
		} else {
			path.pop_back();
			path[0] = std::move(catalog_name_p);
			qualified_name = QualifiedName(std::move(path), name);
		}
	}
	void SetQualifiedName(QualifiedName name_p) {
		name = name_p.Name();
		qualified_name = std::move(name_p);
	}
	QualifiedName GetQualifiedName() const {
		return qualified_name.WithName(name);
	}

	const Identifier &GetName() const {
		return name;
	}
	const Identifier &GetSchemaName() const {
		return qualified_name.Schema();
	}
	const Identifier &GetCatalogName() const {
		return qualified_name.Catalog();
	}

	//! Returns the formatted string name(arg1, arg2, ...)
	DUCKDB_API static string CallToString(const Identifier &catalog_name, const Identifier &schema_name,
	                                      const Identifier &name, const vector<LogicalType> &arguments,
	                                      const vector<pair<Identifier, LogicalType>> &named_arguments,
	                                      const LogicalType &varargs = LogicalType::INVALID);
	//! Returns the formatted string name(arg1, arg2..) -> return_type
	DUCKDB_API static string CallToString(const Identifier &catalog_name, const Identifier &schema_name,
	                                      const Identifier &name, const vector<LogicalType> &arguments,
	                                      const LogicalType &varargs, const LogicalType &return_type);

private:
	QualifiedName qualified_name;
};

//! The kinds of functions that are selected by their arguments - every subclass of SimpleFunction is one kind
enum class FunctionKind : uint8_t { SCALAR, AGGREGATE, WINDOW, TABLE, PRAGMA, TYPE };

class SimpleFunction : public Function {
public:
	DUCKDB_API SimpleFunction(Identifier name, FunctionSignature signature);
	DUCKDB_API SimpleFunction(Identifier name, vector<LogicalType> arguments, LogicalType return_type,
	                          LogicalType varargs = LogicalType(LogicalTypeId::INVALID));
	DUCKDB_API ~SimpleFunction() override;

protected:
	FunctionSignature signature;

public:
	//! How the function reads in errors, e.g. as a candidate of a call
	DUCKDB_API virtual string ToString() const;
	//! What kind of function this is
	virtual FunctionKind GetKind() const = 0;
	DUCKDB_API hash_t Hash() const;
	//! The properties of scalar, aggregate and window functions - other functions have none
	virtual auto GetFunctionProperties() const -> optional_ptr<const FunctionProperties> {
		return nullptr;
	}

	FunctionSignature &GetSignature() {
		return signature;
	}
	const FunctionSignature &GetSignature() const {
		return signature;
	}

	void SetReturnType(LogicalType return_type_p) {
		signature.SetReturnType(std::move(return_type_p));
	}
	const LogicalType &GetReturnType() const {
		return signature.GetReturnType();
	}
};

class FunctionProperties {
public:
	auto GetStability() const -> FunctionStability {
		return stability;
	}
	auto SetStability(FunctionStability value) -> void {
		stability = value;
	}

	auto GetNullHandling() const -> FunctionNullHandling {
		return null_handling;
	}
	auto SetNullHandling(FunctionNullHandling value) -> void {
		null_handling = value;
	}

	auto GetErrorMode() const -> FunctionErrors {
		return errors;
	}
	auto SetErrorMode(FunctionErrors value) -> void {
		errors = value;
	}

	auto GetCollationHandling() const -> FunctionCollationHandling {
		return collation_handling;
	}
	auto SetCollationHandling(FunctionCollationHandling value) -> void {
		collation_handling = value;
	}

	auto GetCaptureArgumentAliases() const -> bool {
		return capture_argument_aliases;
	}
	auto SetCaptureArgumentAliases(bool value) -> void {
		capture_argument_aliases = value;
	}
	auto RequiresExpressionNames() const -> bool {
		return requires_expression_names;
	}
	auto SetRequiresExpressionNames(bool value) -> void {
		requires_expression_names = value;
	}

	auto RequiresOrderedExecution() const -> bool {
		return requires_ordered_execution;
	}
	auto SetRequiresOrderedExecution(bool value) -> void {
		requires_ordered_execution = value;
	}

	// Helpers
	auto SetFallible() -> void {
		errors = FunctionErrors::CAN_THROW_RUNTIME_ERROR;
	}
	auto SetVolatile() -> void {
		stability = FunctionStability::VOLATILE;
	}

	bool operator==(const FunctionProperties &rhs) const;
	bool operator!=(const FunctionProperties &rhs) const;

public:
	FunctionStability stability = FunctionStability::CONSISTENT;
	//! How this function handles NULL values
	FunctionNullHandling null_handling = FunctionNullHandling::DEFAULT_NULL_HANDLING;
	//! Whether or not this function can throw an error
	FunctionErrors errors = FunctionErrors::CANNOT_ERROR;
	//! Collation handling of the function
	FunctionCollationHandling collation_handling = FunctionCollationHandling::PROPAGATE_COLLATIONS;
	//! Whether the binder should capture argument expression aliases as named-argument names when binding this
	//! function. This preserves the legacy behavior of functions such as struct_pack/row, which derived their
	//! (struct field) names from argument aliases and therefore allowed positional arguments after named ones.
	bool capture_argument_aliases = false;
	//! Whether results depend on argument expression names or the call's result alias
	bool requires_expression_names = false;
	//! Whether calls to this function must follow input order
	bool requires_ordered_execution = false;
};

class BoundSimpleFunction {
protected:
	QualifiedName qualified_name;
	string extra_info;

	//! The set of arguments of the function
	vector<LogicalType> arguments;
	//! The number of leading arguments that are matched to the parameters by position
	idx_t positional_arguments = 0;
	//! The names of the remaining arguments, which are matched to the parameters by name
	vector<Identifier> named_arguments;
	//! Return type of the function
	LogicalType return_type;

public:
	void SetName(Identifier name_p) {
		qualified_name = qualified_name.WithName(std::move(name_p));
	}

	const Identifier &GetName() const {
		return qualified_name.Name();
	}
	const Identifier &GetSchemaName() const {
		return qualified_name.Schema();
	}
	const Identifier &GetCatalogName() const {
		return qualified_name.Catalog();
	}

	const QualifiedName &GetQualifiedName() const {
		return qualified_name;
	}
	void SetQualifiedName(QualifiedName name_p) {
		qualified_name = std::move(name_p);
	}

	const string &GetExtraInfo() const {
		return extra_info;
	}

	DUCKDB_API string ToString() const;
	DUCKDB_API hash_t Hash() const;

	auto GetArguments() const -> const vector<LogicalType> & {
		return arguments;
	}
	auto GetArguments() -> vector<LogicalType> & {
		return arguments;
	}

	auto GetPositionalArgumentCount() const -> idx_t {
		return positional_arguments;
	}
	auto GetNamedArguments() const -> const vector<Identifier> & {
		return named_arguments;
	}
	auto SetNamedArguments(idx_t positional_arguments_p, vector<Identifier> named_arguments_p) -> void {
		positional_arguments = positional_arguments_p;
		named_arguments = std::move(named_arguments_p);
	}

	//! The arguments are laid out as [standard | *args | keyword-only | **kwargs], these need the signature of the
	//! function to tell them apart
	DUCKDB_API auto GetStandardArgumentCount(const FunctionSignature &signature) const -> idx_t;
	DUCKDB_API auto GetVarArgsCount(const FunctionSignature &signature) const -> idx_t;
	DUCKDB_API auto GetKeywordOnlyArgumentCount(const FunctionSignature &signature) const -> idx_t;
	DUCKDB_API auto GetKwargsCount(const FunctionSignature &signature) const -> idx_t;

	//! Records the arguments of a call with constant arguments, as the function binder placed them for `signature`
	DUCKDB_API void SetCallArguments(const FunctionSignature &signature, const vector<Value> &parameters,
	                                 const named_argument_map_t &named_parameters);

protected:
	DUCKDB_API auto GetArgumentParameterKind(const FunctionSignature &signature, idx_t argument_index) const
	    -> FunctionParameterKind;

public:
	auto GetReturnType() const -> const LogicalType & {
		return return_type;
	}
	auto GetReturnType() -> LogicalType & {
		return return_type;
	}
	auto SetReturnType(LogicalType return_type_p) -> void {
		return_type = std::move(return_type_p);
	}
};

//! Shared state of the "bind" callback inputs of scalar, aggregate and window functions: the arguments the function
//! was called with, their resolved names, and helpers to extract constant arguments during binding.
class BindFunctionInput {
public:
	BindFunctionInput(ClientContext &context_p, const BoundSimpleFunction &function_p,
	                  const FunctionSignature &signature_p, vector<unique_ptr<Expression>> &arguments_p)
	    : context(context_p), function(function_p), signature(signature_p), arguments(arguments_p) {
	}

	ClientContext &GetClientContext() const {
		return context;
	}
	vector<unique_ptr<Expression>> &GetArguments() const {
		return arguments;
	}
	//! The resolved name of an argument: the parameter name for standard and keyword-only parameters, the name the
	//! caller wrote for "**kwargs", and an empty name for "*args"
	DUCKDB_API const Identifier &GetArgumentName(idx_t arg_idx) const;

	//! Get the constant value of an argument.
	//! Throws ParameterNotResolvedException if unresolved, and BinderException for non-constant arguments.
	//! When 'accept_null' is false, also throws if the (constant) value is NULL.
	DUCKDB_API Value GetConstant(idx_t arg_idx, bool accept_null = true) const;
	DUCKDB_API Value GetConstant(const Identifier &name, bool accept_null = true) const;

	//! Shorthand for GetConstant(<arg>, false)
	DUCKDB_API Value GetNonNullConstant(idx_t index) const {
		return GetConstant(index, false);
	}
	DUCKDB_API Value GetNonNullConstant(const Identifier &name) const {
		return GetConstant(name, false);
	}

	//! Try to get the constant value of an argument.
	//! Returns none if the argument...
	//! - is not constant (unresolved parameter or a non-foldable expression)
	//! - index is out of range
	//! - was not found when looking up by name
	//! - raises an evaluation error (internal errors still propagate)
	//! Use this when a non-constant argument should fall back to the runtime value instead of being an error.
	DUCKDB_API optional<Value> TryGetConstant(idx_t arg_idx) const;
	DUCKDB_API optional<Value> TryGetConstant(const Identifier &name) const;

private:
	//! Resolve a named argument to its position, or an empty optional_idx if no argument with that name was provided.
	optional_idx GetArgumentIndex(const Identifier &name) const;

private:
	ClientContext &context;
	const BoundSimpleFunction &function;
	const FunctionSignature &signature;
	vector<unique_ptr<Expression>> &arguments;
};

} // namespace duckdb
