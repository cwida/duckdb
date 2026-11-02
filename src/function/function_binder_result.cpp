#include "duckdb/function/function_binder.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/function/type_constructor.hpp"

namespace duckdb {

template <class VARIANT, size_t... I>
static constexpr bool AlternativesFollowTypes(std::index_sequence<I...>) {
	return ((static_cast<size_t>(std::variant_alternative_t<I, VARIANT>::TYPE) == I) && ...);
}
static_assert(AlternativesFollowTypes<FunctionMismatch::Reason>(
                  std::make_index_sequence<std::variant_size_v<FunctionMismatch::Reason>>()),
              "the alternatives of FunctionMismatch::Reason must be in the order of FunctionMismatch::Type");
static_assert(AlternativesFollowTypes<FunctionBindError::Kind>(
                  std::make_index_sequence<std::variant_size_v<FunctionBindError::Kind>>()),
              "the alternatives of FunctionBindError::Kind must be in the order of FunctionBindError::Type");

string FunctionMismatch::MissingParameter::ToString(const SimpleFunction &, const FunctionBindError &) const {
	if (name.empty()) {
		return StringUtil::Format("Required parameter at position %llu is missing", parameter + 1);
	}
	return StringUtil::Format("Required parameter \"%s\" is missing", name.GetIdentifierName());
}

string FunctionMismatch::TooManyArguments::ToString(const SimpleFunction &, const FunctionBindError &) const {
	auto kind = positional ? "positional " : "";
	if (accepted == 0) {
		return StringUtil::Format("Takes no %sarguments", kind);
	}
	return StringUtil::Format("Takes at most %llu %sargument%s", accepted, kind, accepted == 1 ? "" : "s");
}

string FunctionMismatch::UnknownNamedArgument::ToString(const SimpleFunction &, const FunctionBindError &error) const {
	return StringUtil::Format("Has no parameter named \"%s\"",
	                          error.GetCall().GetArgument(argument).name.GetIdentifierName());
}

string FunctionMismatch::AlreadyPassedByPosition::ToString(const SimpleFunction &,
                                                           const FunctionBindError &error) const {
	return StringUtil::Format("Parameter \"%s\" has already been provided as a positional argument",
	                          error.GetCall().GetArgument(argument).name.GetIdentifierName());
}

string FunctionMismatch::IncompatibleArgument::ToString(const SimpleFunction &, const FunctionBindError &error) const {
	auto &name = error.GetCall().GetArgument(argument).name;
	auto &argument_type = error.GetCall().GetArgument(argument).type;
	if (name.empty()) {
		return StringUtil::Format("Argument %llu of type %s cannot be implicitly cast to %s", argument + 1,
		                          argument_type.ToString(), parameter_type.ToString());
	}
	return StringUtil::Format("Argument \"%s\" of type %s cannot be implicitly cast to %s", name.GetIdentifierName(),
	                          argument_type.ToString(), parameter_type.ToString());
}

string FunctionMismatch::TemplateDeduction::ToString(const SimpleFunction &function, const FunctionBindError &) const {
	auto &steps = inferred_types;
	// the inferred types are the first one, then pairs of a type and what the template is promoted to with it
	auto msg = StringUtil::Format("Cannot deduce template type '%s', it was inferred to be:\n", template_name);
	msg += StringUtil::Format("  - '%s', from first occurrence\n", steps[0].ToString());
	for (idx_t i = 2; i + 1 < steps.size(); i += 2) {
		msg += StringUtil::Format("  - '%s', by promoting '%s' + '%s'\n", steps[i].ToString(), steps[i - 2].ToString(),
		                          steps[i - 1].ToString());
	}
	msg += StringUtil::Format("  - '%s', which is incompatible with the previously inferred type",
	                          steps.back().ToString());
	return msg;
}

string FunctionMismatch::TemplateUnresolved::ToString(const SimpleFunction &function, const FunctionBindError &) const {
	return StringUtil::Format("Cannot deduce template type '%s', it is not determined by the arguments", template_name);
}

//! The argument a mismatch is about, if it is about a single one
static optional_idx GetMismatchArgument(const FunctionMismatch &mismatch) {
	switch (mismatch.GetType()) {
	case FunctionMismatch::Type::TOO_MANY_ARGUMENTS:
		return mismatch.Cast<FunctionMismatch::TooManyArguments>().argument;
	case FunctionMismatch::Type::UNKNOWN_NAMED_ARGUMENT:
		return mismatch.Cast<FunctionMismatch::UnknownNamedArgument>().argument;
	case FunctionMismatch::Type::ALREADY_PASSED_BY_POSITION:
		return mismatch.Cast<FunctionMismatch::AlreadyPassedByPosition>().argument;
	case FunctionMismatch::Type::INCOMPATIBLE_ARGUMENT:
		return mismatch.Cast<FunctionMismatch::IncompatibleArgument>().argument;
	case FunctionMismatch::Type::TEMPLATE_DEDUCTION:
		return mismatch.Cast<FunctionMismatch::TemplateDeduction>().argument;
	case FunctionMismatch::Type::MISSING_PARAMETER:
	case FunctionMismatch::Type::TEMPLATE_UNRESOLVED:
		return optional_idx();
	}
	throw InternalException("Unrecognized FunctionMismatch::Type");
}

//! Point an error at an argument, if its location is known
static ErrorData WithArgumentLocation(ErrorData result, const FunctionBindError &error, optional_idx argument) {
	if (argument.IsValid()) {
		result.AddQueryLocation(error.GetCall().GetArgument(argument.GetIndex()).location);
	}
	return result;
}

string FunctionMismatch::ToString(const FunctionBindError &error) const {
	return std::visit([&](const auto &reason) { return reason.ToString(*function, error); }, reason);
}

optional_ptr<const FunctionMismatch> FunctionBindError::NoMatch::GetClosest() const {
	if (mismatches.size() == 1 ||
	    (mismatches.size() > 1 && mismatches[0].GetDistance() < mismatches[1].GetDistance())) {
		return mismatches[0];
	}
	return nullptr;
}

//! The number of candidates an error lists for every reason - one more is listed rather than mentioned as more
static constexpr idx_t MAX_LISTED_CANDIDATES = 3;

//! A list of candidates of an error, with the ones beyond the limit counted
static string CandidatesToString(const vector<reference<const SimpleFunction>> &functions) {
	auto listed = functions.size() <= MAX_LISTED_CANDIDATES + 1 ? functions.size() : MAX_LISTED_CANDIDATES;
	string result;
	for (idx_t i = 0; i < listed; i++) {
		result += "\n    " + functions[i].get().ToString();
	}
	if (listed < functions.size()) {
		result += StringUtil::Format("\n    ... and %llu more", functions.size() - listed);
	}
	return result;
}

//! The parameters of a type, as the type errors show them: (INTEGER, name := VARCHAR)
static string TypeParametersToString(const FunctionCallArguments &call) {
	vector<string> parts;
	for (idx_t i = 0; i < call.GetArgumentCount(); i++) {
		auto &argument = call.GetArgument(i);
		// a modifier matches as a literal, but reads as the type of its value
		auto &type = argument.type;
		auto part = (type.id() == LogicalTypeId::INTEGER_LITERAL ? IntegerLiteral::GetType(type) : type).ToString();
		if (!argument.name.empty()) {
			part = argument.name.GetIdentifierName() + " := " + part;
		}
		parts.push_back(std::move(part));
	}
	return "(" + StringUtil::Join(parts, ", ") + ")";
}

//! The call an error is about, e.g. "abs(INTEGER, VARCHAR)"
static string CallToString(const FunctionBindError &error, optional_ptr<const SimpleFunction> function) {
	auto &call = error.GetCall();
	return Function::CallToString(function ? function->GetCatalogName() : Identifier(),
	                              function ? function->GetSchemaName() : Identifier(), error.GetName(),
	                              call.GetPositionalTypes(), call.GetNamedTypes());
}

ErrorData FunctionBindError::NoMatch::ToError(const FunctionBindError &error) const {
	optional_ptr<const SimpleFunction> first;
	if (!mismatches.empty()) {
		first = mismatches[0].GetFunction();
	}
	// the candidates grouped by why the call does not match them, the group of the closest candidate first
	vector<pair<string, vector<reference<const SimpleFunction>>>> groups;
	for (auto &mismatch : mismatches) {
		auto reason = mismatch.ToString(error);
		auto group =
		    std::find_if(groups.begin(), groups.end(), [&](const auto &entry) { return entry.first == reason; });
		if (group == groups.end()) {
			groups.emplace_back(std::move(reason), vector<reference<const SimpleFunction>>());
			group = groups.end() - 1;
		}
		group->second.push_back(mismatch.GetFunction());
	}
	string message;
	if (first && first->GetKind() == FunctionKind::TYPE) {
		auto &call = error.GetCall();
		message = call.GetArgumentCount() == 0 ? StringUtil::Format("Type %s requires type parameters", error.GetName())
		                                       : StringUtil::Format("Type %s does not accept type parameters %s",
		                                                            error.GetName(), TypeParametersToString(call));
	} else {
		message = StringUtil::Format(
		    "No function matches the given name and argument types '%s'. You might need to add explicit type casts.",
		    CallToString(error, first));
	}
	message += "\nCandidates:";
	for (auto &group : groups) {
		message += CandidatesToString(group.second);
		for (auto &line : StringUtil::Split(group.first, '\n')) {
			message += "\n        " + line;
		}
	}

	// every overload is a candidate, in the order of the set
	vector<reference<const FunctionMismatch>> overloads(mismatches.begin(), mismatches.end());
	std::sort(overloads.begin(), overloads.end(), [](const FunctionMismatch &a, const FunctionMismatch &b) {
		return a.GetOverloadIndex() < b.GetOverloadIndex();
	});
	vector<string> candidates;
	for (auto &overload : overloads) {
		candidates.push_back(overload.get().GetFunction().ToString());
	}
	auto &call = error.GetCall();
	auto result = ErrorData(BinderException::NoMatchingFunction(
	    first ? first->GetCatalogName() : Identifier(), first ? first->GetSchemaName() : Identifier(), error.GetName(),
	    call.GetPositionalTypes(), call.GetNamedTypes(), candidates, message));

	// point at the argument of the closest candidate, if no other is as close - or of a template it cannot deduce
	optional_idx argument;
	if (!mismatches.empty() && (GetClosest() || mismatches[0].IsTemplateMismatch())) {
		argument = GetMismatchArgument(mismatches[0]);
	}
	return WithArgumentLocation(std::move(result), error, argument);
}

ErrorData FunctionBindError::AmbiguousMatch::ToError(const FunctionBindError &error) const {
	auto &first = candidates[0].GetFunction();
	string message;
	if (first.GetKind() == FunctionKind::TYPE) {
		message = StringUtil::Format("Could not choose a best candidate type constructor for the type \"%s%s\". In "
		                             "order to select one, please add explicit type casts.",
		                             error.GetName().GetIdentifierName(), TypeParametersToString(error.GetCall()));
	} else {
		message = StringUtil::Format("Could not choose a best candidate function for the function call \"%s\". In "
		                             "order to select one, please add explicit type casts.",
		                             CallToString(error, first));
	}
	message += "\nCandidates:";
	vector<reference<const SimpleFunction>> functions;
	for (auto &candidate : candidates) {
		functions.push_back(candidate.GetFunction());
	}
	message += CandidatesToString(functions);
	message += "\n        All match the given arguments equally well";
	return ErrorData(ExceptionType::BINDER, message);
}

ErrorData FunctionBindError::DuplicateArgument::ToError(const FunctionBindError &error) const {
	auto &argument_name = error.GetCall().GetArgument(argument).name;
	auto message =
	    function_kind == FunctionKind::TYPE
	        ? StringUtil::Format("Duplicate named type parameter %s for type %s", argument_name, error.GetName())
	        : StringUtil::Format("Duplicate named argument %s in '%s'", argument_name,
	                             error.GetName().GetIdentifierName());
	return WithArgumentLocation(ErrorData(BinderException(message)), error, argument);
}

FunctionBindError::operator ErrorData() const {
	return std::visit([&](const auto &kind) { return kind.ToError(*this); }, kind);
}

void FunctionBindError::Throw() const {
	ErrorData(*this).Throw();
}

string FunctionBindError::Message() const {
	return ErrorData(*this).Message();
}

string FunctionBindError::RawMessage() const {
	return ErrorData(*this).RawMessage();
}

} // namespace duckdb
