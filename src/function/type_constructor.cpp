#include "duckdb/function/type_constructor.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/function/function_binder.hpp"

namespace duckdb {

namespace {

LogicalType BindIdentityType(BindLogicalTypeInput &input) {
	return input.base_type;
}

} // namespace

FunctionSignature TypeConstructor::Signature() {
	return FunctionSignature(vector<FunctionParameter>(), LogicalType::TYPE());
}

TypeConstructor::TypeConstructor(Identifier name, FunctionSignature signature_p, bind_logical_type_function_t bind)
    : SimpleFunction(std::move(name), std::move(signature_p)), bind_function(bind) {
	D_ASSERT(bind_function);
	signature.Verify();
}

TypeConstructor::TypeConstructor(FunctionSignature signature_p, bind_logical_type_function_t bind)
    : TypeConstructor(Identifier(), std::move(signature_p), bind) {
}

TypeConstructor TypeConstructor::Identity(Identifier name) {
	return TypeConstructor(std::move(name), Signature(), BindIdentityType);
}

TypeConstructor TypeConstructor::Unchecked(Identifier name, bind_logical_type_function_t bind) {
	// accepts anything, the bind function validates the arguments itself
	auto signature = Signature();
	signature.AddArgs("args", LogicalType::ANY).AddKwargs("kwargs", LogicalType::ANY);
	return TypeConstructor(std::move(name), std::move(signature), bind);
}

TypeConstructorSet::TypeConstructorSet() : FunctionSet<TypeConstructor>(Identifier()) {
}

TypeConstructorSet::TypeConstructorSet(Identifier name) : FunctionSet<TypeConstructor>(std::move(name)) {
}

namespace {

//! The type an argument is matched against. A modifier is always a constant, so an integer one matches as an
//! integer literal exactly as it would in a function call - that is what lets DECIMAL(10) bind to a UTINYINT
//! parameter, as the literal is known to fit. Unlike ExpressionBinder::GetExpressionReturnType this does not treat
//! strings as literals: a string literal is implicitly castable to anything, which would let DECIMAL('32') through.
//! Only overload matching uses this; errors report the value's own type.
LogicalType MatchType(const TypeArgument &arg) {
	auto &value = arg.GetValue();
	auto &type = value.type();
	if (type.IsIntegral() && !value.IsNull()) {
		return LogicalType::INTEGER_LITERAL(value);
	}
	return type;
}

//! How a constructor reads in an error: MAP(key TYPE, value TYPE). Deliberately not SimpleFunction::ToString(),
//! which quotes identifiers and appends the "-> TYPE" return that every constructor trivially shares.
string ConstructorToString(const Identifier &type_name, const TypeConstructor &constructor) {
	const auto &sig = constructor.GetSignature();
	vector<string> parts;
	for (auto &param : sig.GetParameters()) {
		switch (param.GetKind()) {
		case FunctionParameterKind::VAR_POSITIONAL:
			parts.push_back(param.GetType().ToString() + "...");
			continue;
		case FunctionParameterKind::VAR_KEYWORD:
			parts.push_back(param.GetName().GetIdentifierName() + " := " + param.GetType().ToString() + "...");
			continue;
		default:
			break;
		}
		string part = param.GetName().GetIdentifierName() + " " + param.GetType().ToString();
		if (param.HasDefaultValue()) {
			part += " := " + param.GetDefaultValue()->ToString();
		}
		parts.push_back(std::move(part));
	}
	return type_name.GetIdentifierName() + "(" + StringUtil::Join(parts, ", ") + ")";
}

//! How a modifier is referred to in errors: by its parameter name where it has one, by its position otherwise.
string ArgumentName(const Identifier &name, idx_t position) {
	if (name.empty()) {
		return StringUtil::Format("%llu", position + 1);
	}
	return StringUtil::Format("%s", name);
}

//! Cast a modifier to the type its parameter declares. Overload selection already established the cast is possible.
Value CastArgument(const Identifier &type_name, const string &arg_name, const Value &value, const LogicalType &target,
                   QueryLocation location) {
	if (value.IsNull()) {
		throw BinderException(location, "Type parameter %s for type %s cannot be NULL", arg_name, type_name);
	}
	if (target.id() == LogicalTypeId::ANY || value.type() == target) {
		return value;
	}
	string error;
	auto result = value.DefaultTryCastAs(target, &error);
	if (!result) {
		throw BinderException(location,
		                      "Type parameter %s for type %s must be of type %s, but %s could not be converted: %s",
		                      arg_name, type_name, target.ToString(), value.ToString(), error);
	}
	return *result;
}

//! Reorder the call arguments into the constructor's declared parameter order, filling in defaults, and cast every
//! value to the type its parameter declares. Any argument beyond the declared parameters is appended in call order
//! with its name preserved.
vector<TypeArgument> NormalizeArguments(const Identifier &type_name, const TypeConstructor &constructor,
                                        const vector<TypeArgument> &arguments, QueryLocation type_location) {
	const auto &sig = constructor.GetSignature();
	const auto param_count = sig.GetPositionalParameterCount();

	vector<TypeArgument> result;
	vector<optional_ptr<const TypeArgument>> slots(param_count);
	// the position is the argument's index in the call, used to refer to unnamed arguments in errors
	vector<pair<idx_t, reference<const TypeArgument>>> varargs;

	idx_t positional_count = 0;
	for (idx_t i = 0; i < arguments.size(); i++) {
		auto &arg = arguments[i];
		if (arg.HasName()) {
			continue;
		}
		if (positional_count < param_count) {
			slots[positional_count] = arg;
		} else {
			varargs.emplace_back(i, arg);
		}
		positional_count++;
	}

	for (idx_t i = 0; i < arguments.size(); i++) {
		auto &arg = arguments[i];
		if (!arg.HasName()) {
			continue;
		}
		auto name = Identifier(arg.GetName());
		auto param_idx = sig.GetParameterIndexByName(name);
		if (!param_idx.IsValid()) {
			// not a declared parameter - it can only be a named vararg
			varargs.emplace_back(i, arg);
			continue;
		}
		auto index = param_idx.GetIndex();
		if (slots[index]) {
			throw BinderException(arg.GetQueryLocation(),
			                      "Type parameter %s for type %s was provided both by position and by name",
			                      sig.GetParameter(index).GetName(), type_name);
		}
		slots[index] = arg;
	}

	for (idx_t i = 0; i < param_count; i++) {
		auto &param = sig.GetParameter(i);
		auto supplied = slots[i];
		// a parameter filled in from its default has no source text - point at the type instead
		auto location = supplied ? supplied->GetQueryLocation() : type_location;
		optional_ptr<const Value> value = supplied ? &supplied->GetValue() : param.GetDefaultValue().get();
		if (!value) {
			throw BinderException(type_location, "Missing type parameter %s for type %s", param.GetName(), type_name);
		}
		auto arg_name = ArgumentName(param.GetName(), i);
		result.emplace_back(param.GetName().GetIdentifierName(),
		                    CastArgument(type_name, arg_name, *value, param.GetType(), location), location);
	}

	for (auto &entry : varargs) {
		auto &arg = entry.second.get();
		auto arg_name = ArgumentName(Identifier(arg.GetName()), entry.first);
		auto location = arg.GetQueryLocation();
		// a named modifier is received by "**kwargs", an unnamed one by "*args"
		auto param = arg.HasName() ? sig.GetKwargs() : sig.GetArgs();
		auto &target = param ? param->GetType() : LogicalType::ANY;
		result.emplace_back(arg.GetName(), CastArgument(type_name, arg_name, arg.GetValue(), target, location),
		                    location);
	}
	return result;
}

} // namespace

string TypeConstructor::ToString() const {
	return ConstructorToString(GetName(), *this);
}

LogicalType TypeConstructorSet::Bind(optional_ptr<ClientContext> context, const LogicalType &base_type,
                                     const vector<TypeArgument> &arguments, QueryLocation query_location) const {
	D_ASSERT(!functions.empty());

	// Fast path for the common case: a single constructor taking no modifiers, which is every type that is not
	// parameterised. It also gets a dedicated error, as the generic one would list a single empty candidate.
	if (functions.size() == 1) {
		const auto &sig = functions[0]->GetSignature();
		if (sig.GetParameterCount() == 0) {
			if (!arguments.empty()) {
				throw BinderException(query_location, "Type %s does not take any type parameters", name);
			}
			vector<TypeArgument> modifiers;
			BindLogicalTypeInput input {context, base_type, modifiers, query_location};
			return functions[0]->GetBindFunction()(input);
		}
	}

	vector<LogicalType> positional;
	vector<pair<Identifier, LogicalType>> named;
	for (auto &arg : arguments) {
		if (arg.HasName()) {
			named.emplace_back(Identifier(arg.GetName()), MatchType(arg));
		} else if (named.empty()) {
			positional.push_back(MatchType(arg));
		} else {
			throw BinderException(arg.GetQueryLocation(),
			                      "Type parameter %s for type %s cannot follow a named type parameter",
			                      arg.GetValue().ToString(), name);
		}
	}

	// The candidates are selected with the regular function machinery - its errors name the call, not a function
	auto selection = FunctionBinder::SelectFunction(context, *this, {positional, named});
	if (!selection) {
		ErrorData error = selection.GetError();
		error.AddQueryLocation(query_location);
		error.Throw();
	}
	auto constructor_ptr = selection.GetBinding().GetFunction<TypeConstructor>();
	auto &constructor = *constructor_ptr;

	auto modifiers = NormalizeArguments(name, constructor, arguments, query_location);
	BindLogicalTypeInput input {context, base_type, modifiers, query_location};
	return constructor.GetBindFunction()(input);
}

} // namespace duckdb
