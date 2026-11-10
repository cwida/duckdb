#include "duckdb/function/function.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

namespace duckdb {

TypedKwarg::TypedKwarg(Identifier name_p, LogicalType type_p) : name(std::move(name_p)), type(std::move(type_p)) {
}

bool TypedKwarg::operator==(const TypedKwarg &other) const {
	return name == other.name && aliases == other.aliases && type == other.type;
}

bool TypedKwarg::operator!=(const TypedKwarg &other) const {
	return !(*this == other);
}

string TypedKwarg::ToString() const {
	return StringUtil::Format("%s %s", SQLIdentifier(name), type.ToString());
}

TypedKwargs &TypedKwargs::Add(Identifier name, LogicalType type) {
	options.emplace_back(std::move(name), std::move(type));
	return *this;
}

TypedKwargs &TypedKwargs::Alias(Identifier alias) {
	if (options.empty()) {
		throw InternalException("TypedKwargs::Alias called before any option was added");
	}
	options.back().aliases.push_back(std::move(alias));
	return *this;
}

TypedKwargs &TypedKwargs::Sensitive() {
	if (options.empty()) {
		throw InternalException("TypedKwargs::Sensitive called before any option was added");
	}
	options.back().sensitive = true;
	return *this;
}

TypedKwargs TypedKwargs::Merge(const TypedKwargs &other) const {
	auto result = *this;
	result.options.insert(result.options.end(), other.options.begin(), other.options.end());
	return result;
}

optional_ptr<const TypedKwarg> TypedKwargs::Find(const Identifier &name) const {
	// names are matched case-insensitively, like parameter names
	for (auto &option : options) {
		if (option.name == name) {
			return option;
		}
		for (auto &alias : option.aliases) {
			if (alias == name) {
				return option;
			}
		}
	}
	return nullptr;
}

const vector<TypedKwarg> &TypedKwargs::GetOptions() const {
	return options;
}

void TypedKwargs::Verify() const {
	identifier_set_t names;
	for (auto &option : options) {
		if (!names.insert(option.name).second) {
			throw InvalidInputException("Duplicate option name: %s", option.name);
		}
		for (auto &alias : option.aliases) {
			if (!names.insert(alias).second) {
				throw InvalidInputException("Duplicate option name: %s", alias);
			}
		}
	}
}

vector<Identifier> TypedKwargs::GetNames() const {
	vector<Identifier> result;
	for (auto &option : options) {
		result.push_back(option.name);
		for (auto &alias : option.aliases) {
			result.push_back(alias);
		}
	}
	return result;
}

bool TypedKwargs::operator==(const TypedKwargs &other) const {
	return options == other.options;
}

bool TypedKwargs::operator!=(const TypedKwargs &other) const {
	return !(*this == other);
}

hash_t TypedKwargs::Hash() const {
	hash_t hash = duckdb::Hash(options.size());
	for (auto &option : options) {
		hash = CombineHash(hash, IdentifierHashFunction()(option.name));
		hash = CombineHash(hash, option.type.Hash());
	}
	return hash;
}

bool FunctionProperties::operator==(const FunctionProperties &rhs) const {
	return stability == rhs.stability && null_handling == rhs.null_handling && errors == rhs.errors &&
	       collation_handling == rhs.collation_handling && capture_argument_aliases == rhs.capture_argument_aliases &&
	       requires_expression_names == rhs.requires_expression_names &&
	       requires_ordered_execution == rhs.requires_ordered_execution;
}

bool FunctionProperties::operator!=(const FunctionProperties &rhs) const {
	return !(*this == rhs);
}

FunctionData::~FunctionData() {
}

bool FunctionData::Equals(const FunctionData *left, const FunctionData *right) {
	if (left == right) {
		return true;
	}
	if (!left || !right) {
		return false;
	}
	return left->Equals(*right);
}

TableFunctionData::~TableFunctionData() {
}

unique_ptr<FunctionData> TableFunctionData::Copy() const {
	throw InternalException("Copy not supported for TableFunctionData");
}

bool TableFunctionData::Equals(const FunctionData &other) const {
	return false;
}

bool FunctionData::SupportStatementCache() const {
	return true;
}

Function::Function(Identifier name_p) : name(std::move(name_p)) {
}
Function::~Function() {
}

SimpleFunction::SimpleFunction(Identifier name_p, FunctionSignature signature_p)
    : Function(std::move(name_p)), signature(std::move(signature_p)) {
}

SimpleFunction::SimpleFunction(Identifier name_p, vector<LogicalType> arguments_p, LogicalType return_type,
                               LogicalType varargs_p)
    : Function(std::move(name_p)), signature(std::move(arguments_p), std::move(return_type)) {
	if (varargs_p.id() != LogicalTypeId::INVALID) {
		signature.AddArgs("args", varargs_p).AddKwargs("kwargs", std::move(varargs_p));
	}
}

SimpleFunction::~SimpleFunction() {
}

static bool RequiresCatalogAndSchemaNamePrefix(const Identifier &catalog_name, const Identifier &schema_name) {
	return !catalog_name.empty() && catalog_name != Identifier::SystemCatalog() && !schema_name.empty() &&
	       schema_name != Identifier::DefaultSchema();
}

string FunctionParameter::ToString() const {
	if (kind == FunctionParameterKind::VAR_POSITIONAL) {
		return StringUtil::Format("*%s %s", SQLIdentifier(name), type.ToString());
	}
	if (kind == FunctionParameterKind::VAR_KEYWORD) {
		return StringUtil::Format("**%s %s", SQLIdentifier(name), type.ToString());
	}
	if (default_value) {
		return StringUtil::Format("%s %s := %s", SQLIdentifier(name), type.ToString(), default_value->ToString());
	}
	return StringUtil::Format("%s %s", SQLIdentifier(name), type.ToString());
}

// signatures are copied and moved freely - keep the move cheap
static_assert(std::is_nothrow_move_constructible<FunctionSignature>::value, "FunctionSignature must stay movable");
static_assert(std::is_nothrow_move_assignable<FunctionSignature>::value, "FunctionSignature must stay movable");

auto FunctionSignature::AddTypedKwargs(Identifier name, TypedKwargs schema) -> FunctionSignature & {
	typed_kwargs = make_shared_ptr<TypedKwargs>(std::move(schema));
	return AddKwargs(std::move(name), LogicalType::ANY);
}

auto FunctionSignature::WithTypedKwargs(Identifier name, const std::function<void(TypedKwargs &)> &configure)
    -> FunctionSignature & {
	TypedKwargs schema;
	configure(schema);
	return AddTypedKwargs(std::move(name), std::move(schema));
}

auto FunctionSignature::ExtendTypedKwargs(const std::function<void(TypedKwargs &)> &configure) -> FunctionSignature & {
	if (!typed_kwargs) {
		throw InternalException("ExtendTypedKwargs called on a signature without typed \"**kwargs\"");
	}
	if (typed_kwargs.use_count() > 1) {
		// other signatures copied from this one share its schema - extend a copy of it
		typed_kwargs = make_shared_ptr<TypedKwargs>(*typed_kwargs);
	}
	configure(*typed_kwargs);
	return *this;
}

string FunctionSignature::ToString() const {
	vector<string> params;
	params.reserve(parameters.size());
	// A keyword-only parameter that no "*args" precedes closes the positional parameters by itself, which Python
	// spells as a bare "*" in that position; a "/" likewise closes the positional-only parameters
	const auto positional_only_count = GetPositionalOnlyParameterCount();
	auto needs_separator = !GetArgs();
	for (idx_t i = 0; i < parameters.size(); i++) {
		auto &param = parameters[i];
		if (i == positional_only_count && positional_only_count > 0) {
			params.push_back("/");
		}
		if (needs_separator && param.GetKind() == FunctionParameterKind::KEYWORD_ONLY) {
			params.push_back("*");
			needs_separator = false;
		}
		params.push_back(param.ToString());
	}
	if (positional_only_count > 0 && positional_only_count == parameters.size()) {
		params.push_back("/");
	}
	auto head = StringUtil::Format("(%s)", StringUtil::Join(params, ", "));
	if (return_type.IsValid()) {
		return head + " -> " + return_type.ToString();
	}
	return head;
}

string SimpleFunction::ToString() const {
	if (RequiresCatalogAndSchemaNamePrefix(GetCatalogName(), GetSchemaName())) {
		return StringUtil::Format("%s.%s.%s%s", SQLIdentifier(GetCatalogName()), SQLIdentifier(GetSchemaName()),
		                          SQLIdentifier(name), signature.ToString());
	}
	return SQLIdentifier::ToString(name.GetIdentifierName()) + signature.ToString();
}

// add your initializer for new functions here
void BuiltinFunctions::Initialize() {
	RegisterTableScanFunctions();
	RegisterSQLiteFunctions();
	RegisterReadFunctions();
	RegisterTableFunctions();
	RegisterArrowFunctions();

	RegisterPragmaFunctions();

	RegisterCopyFunctions();

	// initialize collations
	AddCollation("nocase", LowerFun::GetFunction(), true);
	AddCollation("noaccent", StripAccentsFun::GetFunction(), true);
	AddCollation("nfc", NFCNormalizeFun::GetFunction());

	RegisterExtensionOverloads();
}

hash_t FunctionSignature::Hash() const {
	hash_t hash = return_type.Hash();
	for (auto &param : parameters) {
		hash = duckdb::CombineHash(hash, param.GetType().Hash());
		hash = duckdb::CombineHash(hash, duckdb::Hash(static_cast<uint8_t>(param.GetKind())));
	}
	if (typed_kwargs) {
		hash = duckdb::CombineHash(hash, typed_kwargs->Hash());
	}
	return hash;
}

void FunctionSignature::FillNamedDefaults(ClientContext &context, named_argument_map_t &named_parameters) const {
	// keyword-only parameters are slots, bound in the order they are declared - the arguments "**kwargs" receives
	// follow them in the order they were passed
	named_argument_map_t result;
	for (auto &param : parameters) {
		if (param.GetKind() != FunctionParameterKind::KEYWORD_ONLY) {
			continue;
		}
		auto entry = named_parameters.find(param.GetName());
		if (entry != named_parameters.end()) {
			result.insert(param.GetName(), std::move(entry->second));
		} else if (param.HasDefaultValue()) {
			result.insert(param.GetName(),
			              FunctionBinder::CastToParameterType(context, *param.GetDefaultValue(), param.GetType()));
		}
	}
	if (result.empty()) {
		return;
	}
	for (auto &entry : named_parameters) {
		if (!result.contains(entry.first)) {
			result.insert(entry.first, std::move(entry.second));
		}
	}
	named_parameters = std::move(result);
}

void FunctionSignature::Verify() const {
	// Check for duplicate parameter names
	identifier_set_t seen_names;
	for (const auto &param : parameters) {
		if (seen_names.find(param.GetName()) != seen_names.end()) {
			throw InvalidInputException("Duplicate parameter name: %s", param.GetName());
		}
		seen_names.insert(param.GetName());
	}

	// Also check for default values that are not at the end of the positional parameters
	bool found_default_value = false;
	for (const auto &param : parameters) {
		if (param.GetKind() != FunctionParameterKind::STANDARD) {
			continue;
		}
		if (param.HasDefaultValue()) {
			found_default_value = true;
		} else if (found_default_value) {
			throw InvalidInputException(
			    "Parameters with default values must be at the end of the parameter list. Parameter %s does not "
			    "have a default value but follows a parameter with a default value.",
			    param.GetName());
		}
	}

	// And that the parameter kinds are in order:
	// positional-only, "/", standard parameters, "*args", keyword-only parameters, "**kwargs"
	bool found_args = false;
	bool found_kwargs = false;
	bool found_keyword_only = false;
	bool found_standard = false;
	for (const auto &param : parameters) {
		if (found_kwargs) {
			throw InvalidInputException("Parameter '%s' follows '**kwargs', which must be the last parameter",
			                            param.ToString());
		}
		if (param.IsVariadic() && param.HasDefaultValue()) {
			throw InvalidInputException("Variadic parameter '%s' cannot have a default value", param.ToString());
		}
		switch (param.GetKind()) {
		case FunctionParameterKind::POSITIONAL_ONLY:
			if (found_standard || found_args || found_keyword_only) {
				throw InvalidInputException(
				    "Positional-only parameter '%s' must be declared before every parameter that can be passed by name",
				    param.ToString());
			}
			break;
		case FunctionParameterKind::STANDARD:
			if (found_args || found_keyword_only) {
				throw InvalidInputException("Parameter '%s' follows '*args' and must therefore be keyword-only",
				                            param.ToString());
			}
			found_standard = true;
			break;
		case FunctionParameterKind::VAR_POSITIONAL:
			if (found_args) {
				throw InvalidInputException("A function signature can only have one '*args' parameter");
			}
			if (found_keyword_only) {
				throw InvalidInputException("Parameter '%s' cannot follow a keyword-only parameter", param.ToString());
			}
			found_args = true;
			break;
		case FunctionParameterKind::VAR_KEYWORD:
			found_kwargs = true;
			break;
		case FunctionParameterKind::KEYWORD_ONLY:
			found_keyword_only = true;
			break;
		}
	}

	if (typed_kwargs) {
		if (!found_kwargs) {
			throw InvalidInputException("A function signature with options must have a '**kwargs' parameter");
		}
		typed_kwargs->Verify();
		// a named argument binds to a parameter of its name first, so an option of the same name is never reached
		for (auto &param : parameters) {
			if (param.AcceptsName() && typed_kwargs->Find(param.GetName())) {
				throw InvalidInputException("Option %s has the same name as a parameter", param.GetName());
			}
		}
	}
}

hash_t SimpleFunction::Hash() const {
	return signature.Hash();
}

string Function::CallToString(const Identifier &catalog_name, const Identifier &schema_name, const Identifier &name,
                              const vector<LogicalType> &arguments,
                              const vector<pair<Identifier, LogicalType>> &named_arguments,
                              const LogicalType &varargs) {
	string result;
	if (RequiresCatalogAndSchemaNamePrefix(catalog_name, schema_name)) {
		result += catalog_name + "." + schema_name + ".";
	}
	result += name + "(";
	vector<string> string_arguments;
	for (auto &arg : arguments) {
		string_arguments.push_back(arg.ToString());
	}

	for (const auto &[arg_name, arg_type] : named_arguments) {
		string_arguments.push_back(StringUtil::Format("%s := %s", arg_name, arg_type.ToString()));
	}

	if (varargs.IsValid()) {
		string_arguments.push_back("[" + varargs.ToString() + "...]");
	}
	result += StringUtil::Join(string_arguments, ", ");
	return result + ")";
}

string Function::CallToString(const Identifier &catalog_name, const Identifier &schema_name, const Identifier &name,
                              const vector<LogicalType> &arguments, const LogicalType &varargs,
                              const LogicalType &return_type) {
	string result =
	    CallToString(catalog_name, schema_name, name, arguments, vector<pair<Identifier, LogicalType>> {}, varargs);
	result += " -> " + return_type.ToString();
	return result;
}

hash_t BoundSimpleFunction::Hash() const {
	hash_t hash = return_type.Hash();
	for (auto &arg : arguments) {
		hash = duckdb::CombineHash(hash, arg.Hash());
	}
	return hash;
}

idx_t BoundSimpleFunction::GetStandardArgumentCount(const FunctionSignature &signature) const {
	return positional_arguments - GetVarArgsCount(signature);
}

idx_t BoundSimpleFunction::GetVarArgsCount(const FunctionSignature &signature) const {
	const auto standard_count = signature.GetPositionalParameterCount();
	return positional_arguments > standard_count ? positional_arguments - standard_count : 0;
}

idx_t BoundSimpleFunction::GetKeywordOnlyArgumentCount(const FunctionSignature &signature) const {
	return named_arguments.size() - GetKwargsCount(signature);
}

idx_t BoundSimpleFunction::GetKwargsCount(const FunctionSignature &signature) const {
	idx_t result = 0;
	for (auto &name : named_arguments) {
		if (!signature.GetParameterIndexByName(name).IsValid()) {
			result++;
		}
	}
	return result;
}

void BoundSimpleFunction::SetCallArguments(const FunctionSignature &signature, const vector<Value> &parameters,
                                           const named_argument_map_t &named_parameters) {
	const auto positional_count = signature.GetPositionalParameterCount();
	arguments.clear();
	for (idx_t i = 0; i < positional_count; i++) {
		arguments.push_back(signature.GetParameter(i).GetType());
	}
	auto args = signature.GetArgs();
	for (idx_t i = positional_count; i < parameters.size(); i++) {
		auto is_typed = args && args->GetType().id() != LogicalTypeId::ANY;
		arguments.push_back(is_typed ? args->GetType() : parameters[i].type());
	}
	const auto positional_argument_count = arguments.size();

	// only the options the signature receives by name, in the order they were passed
	vector<Identifier> names;
	for (auto &entry : named_parameters) {
		auto param_idx = signature.GetParameterIndexByName(entry.first);
		if (!param_idx.IsValid()) {
			if (signature.GetKwargs()) {
				names.push_back(entry.first);
				arguments.push_back(entry.second.type());
			}
			continue;
		}
		auto &param = signature.GetParameter(param_idx.GetIndex());
		if (!param.AcceptsPosition()) {
			names.push_back(entry.first);
			arguments.push_back(param.GetType());
		}
	}
	SetNamedArguments(positional_argument_count, std::move(names));
}

FunctionParameterKind BoundSimpleFunction::GetArgumentParameterKind(const FunctionSignature &signature,
                                                                    idx_t argument_index) const {
	if (argument_index >= positional_arguments + named_arguments.size()) {
		throw InternalException("%s: Argument index %llu is out of range", GetName(), argument_index);
	}
	if (argument_index < signature.GetPositionalParameterCount()) {
		return signature.GetParameter(argument_index).GetKind();
	}
	if (argument_index < positional_arguments) {
		return FunctionParameterKind::VAR_POSITIONAL;
	}
	auto &name = named_arguments[argument_index - positional_arguments];
	return signature.GetParameterIndexByName(name).IsValid() ? FunctionParameterKind::KEYWORD_ONLY
	                                                         : FunctionParameterKind::VAR_KEYWORD;
}

string BoundSimpleFunction::ToString() const {
	return Function::CallToString(GetCatalogName(), GetSchemaName(), GetName(), arguments, LogicalTypeId::INVALID,
	                              return_type);
}

bool FunctionParameter::operator==(const FunctionParameter &other) const {
	return type == other.type && name == other.name && kind == other.kind;
}

bool FunctionParameter::operator!=(const FunctionParameter &other) const {
	return !(*this == other);
}

//! Whether two signatures declare the same options - compared by content, as each registration builds its own
static bool OptionSchemasEqual(optional_ptr<const TypedKwargs> lhs, optional_ptr<const TypedKwargs> rhs) {
	if (!lhs || !rhs) {
		return !lhs && !rhs;
	}
	return *lhs == *rhs;
}

bool FunctionSignature::operator==(const FunctionSignature &other) const {
	return parameters == other.parameters && return_type == other.return_type &&
	       OptionSchemasEqual(GetTypedKwargs(), other.GetTypedKwargs());
}

bool FunctionSignature::operator!=(const FunctionSignature &other) const {
	return !(*this == other);
}

bool FunctionSignature::IsSameOverload(const FunctionSignature &other) const {
	// the parameters a caller can pass by position, "*args" and "**kwargs", in declaration order
	auto ordered = [](const FunctionSignature &signature) {
		vector<reference<const FunctionParameter>> result;
		for (auto &param : signature.GetParameters()) {
			if (param.GetKind() != FunctionParameterKind::KEYWORD_ONLY) {
				result.push_back(param);
			}
		}
		return result;
	};
	auto lhs_ordered = ordered(*this);
	auto rhs_ordered = ordered(other);
	if (lhs_ordered.size() != rhs_ordered.size()) {
		return false;
	}
	for (idx_t i = 0; i < lhs_ordered.size(); i++) {
		auto &lhs = lhs_ordered[i].get();
		auto &rhs = rhs_ordered[i].get();
		// a positional-only and a standard parameter both take a position, which is all a call can tell apart
		auto takes_position = lhs.AcceptsPosition();
		if (takes_position != rhs.AcceptsPosition() || (!takes_position && lhs.GetKind() != rhs.GetKind())) {
			return false;
		}
		if (lhs.GetType() != rhs.GetType()) {
			return false;
		}
	}
	// the keyword-only parameters, by name
	auto keywords = [](const FunctionSignature &signature) {
		identifier_map_t<LogicalType> result;
		for (auto &param : signature.GetParameters()) {
			if (param.GetKind() == FunctionParameterKind::KEYWORD_ONLY) {
				result.emplace(param.GetName(), param.GetType());
			}
		}
		return result;
	};
	auto lhs_keywords = keywords(*this);
	auto rhs_keywords = keywords(other);
	if (lhs_keywords.size() != rhs_keywords.size()) {
		return false;
	}
	for (auto &entry : lhs_keywords) {
		auto match = rhs_keywords.find(entry.first);
		if (match == rhs_keywords.end() || match->second != entry.second) {
			return false;
		}
	}
	return true;
}

bool FunctionSignature::Equal(const FunctionSignature &other) const {
	if (parameters.size() != other.parameters.size()) {
		return false;
	}
	for (idx_t i = 0; i < parameters.size(); i++) {
		if (parameters[i].GetType() != other.parameters[i].GetType() ||
		    parameters[i].GetKind() != other.parameters[i].GetKind()) {
			return false;
		}
	}
	if (return_type != other.return_type) {
		return false;
	}
	return OptionSchemasEqual(GetTypedKwargs(), other.GetTypedKwargs());
}

//----------------------------------------------------------------------------------------------------------------------
// Bind Function Input
//----------------------------------------------------------------------------------------------------------------------
//! An unresolved parameter or an as-yet-unknown type (e.g. a macro/prepared argument), possibly cast to the argument
//! type of the function
static bool IsUnresolvedArgument(const Expression &expr) {
	if (expr.HasParameter() || expr.GetReturnType().id() == LogicalTypeId::UNKNOWN) {
		return true;
	}
	if (BoundCastExpression::IsCast(expr)) {
		return IsUnresolvedArgument(BoundCastExpression::Child(expr.Cast<BoundFunctionExpression>()));
	}
	return false;
}

Value BindFunctionInput::GetConstant(idx_t arg_idx, bool accept_null) const {
	if (arg_idx >= arguments.size()) {
		throw InternalException("%s: Argument index %llu is out of range", function.GetName(), arg_idx);
	}
	const auto &expr = *arguments[arg_idx];
	// defer binding until the argument is resolved
	if (IsUnresolvedArgument(expr)) {
		throw ParameterNotResolvedException();
	}
	// Use the argument name if available, otherwise use the argument index
	string argument_name;
	const auto resolved_count = function.GetPositionalArgumentCount() + function.GetNamedArguments().size();
	if (arg_idx < resolved_count && !GetArgumentName(arg_idx).empty()) {
		argument_name = StringUtil::Format("The %s argument", GetArgumentName(arg_idx));
	} else {
		argument_name = StringUtil::Format("Argument #%llu", arg_idx + 1);
	}
	if (!expr.IsFoldable()) {
		throw BinderException(expr, "%s in function %s must be a constant expression", argument_name,
		                      function.GetName());
	}
	auto value = ExpressionExecutor::EvaluateScalar(context, expr);
	if (!accept_null && value.IsNull()) {
		throw BinderException(expr, "%s in function %s must not be NULL", argument_name, function.GetName());
	}
	return value;
}

Value BindFunctionInput::GetConstant(const Identifier &name, bool accept_null) const {
	const auto arg_idx = GetArgumentIndex(name);
	if (!arg_idx.IsValid()) {
		throw InternalException("Function %s does not have a parameter named %s", function.GetName(), name);
	}
	return GetConstant(arg_idx.GetIndex(), accept_null);
}

optional<Value> BindFunctionInput::TryGetConstant(idx_t arg_idx) const {
	if (arg_idx >= arguments.size()) {
		return {};
	}
	const auto &expr = *arguments[arg_idx];
	if (IsUnresolvedArgument(expr)) {
		return {};
	}
	if (!expr.IsFoldable()) {
		return {};
	}
	Value result;
	if (!ExpressionExecutor::TryEvaluateScalar(context, expr, result)) {
		return {};
	}
	return result;
}

optional<Value> BindFunctionInput::TryGetConstant(const Identifier &name) const {
	const auto arg_idx = GetArgumentIndex(name);
	if (arg_idx.IsValid()) {
		return TryGetConstant(arg_idx.GetIndex());
	}
	return {};
}

const Identifier &BindFunctionInput::GetArgumentName(idx_t arg_idx) const {
	static const Identifier UNNAMED;
	// The binder lays the arguments out in the order of the signature: the positional ones fill the standard
	// parameters, then "*args" - the named ones are keyword-only parameters or "**kwargs"
	const auto positional_count = function.GetPositionalArgumentCount();
	if (arg_idx >= positional_count) {
		auto &named_arguments = function.GetNamedArguments();
		if (arg_idx - positional_count >= named_arguments.size()) {
			throw InternalException("%s: Argument index %llu is out of range", function.GetName(), arg_idx);
		}
		return named_arguments[arg_idx - positional_count];
	}
	if (arg_idx < signature.GetPositionalParameterCount()) {
		return signature.GetParameter(arg_idx).GetName();
	}
	return UNNAMED;
}

optional_idx BindFunctionInput::GetArgumentIndex(const Identifier &name) const {
	const auto resolved_count = function.GetPositionalArgumentCount() + function.GetNamedArguments().size();
	for (idx_t arg_idx = 0; arg_idx < resolved_count; arg_idx++) {
		if (GetArgumentName(arg_idx) == name) {
			return optional_idx(arg_idx);
		}
	}
	return optional_idx();
}

} // namespace duckdb
