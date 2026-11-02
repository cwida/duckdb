#include "duckdb/planner/logical_plan_verification_result.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/unordered_set.hpp"

namespace duckdb {

bool LogicalPlanVerificationPathComponent::operator==(const LogicalPlanVerificationPathComponent &other) const {
	return type == other.type && ordinal == other.ordinal;
}

bool LogicalPlanVerificationPath::IsValid() const {
	bool expression_path;
	switch (root) {
	case LogicalPlanVerificationPathRoot::LOGICAL_PLAN:
		expression_path = false;
		break;
	case LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION:
		expression_path = true;
		break;
	default:
		return false;
	}
	for (auto &component : components) {
		switch (component.type) {
		case LogicalPlanVerificationPathComponentType::OPERATOR_CHILD:
			if (expression_path) {
				return false;
			}
			break;
		case LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION:
			if (expression_path) {
				return false;
			}
			expression_path = true;
			break;
		case LogicalPlanVerificationPathComponentType::EXPRESSION_CHILD:
			if (!expression_path) {
				return false;
			}
			break;
		default:
			return false;
		}
	}
	return true;
}

bool LogicalPlanVerificationPath::operator==(const LogicalPlanVerificationPath &other) const {
	return root == other.root && components == other.components;
}

static uint8_t PathComponentOrder(LogicalPlanVerificationPathComponentType type) {
	switch (type) {
	case LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION:
		return 0;
	case LogicalPlanVerificationPathComponentType::OPERATOR_CHILD:
		return 1;
	case LogicalPlanVerificationPathComponentType::EXPRESSION_CHILD:
		return 2;
	}
	throw InternalException("Unknown logical plan verification path component type");
}

bool LogicalPlanVerificationPath::operator<(const LogicalPlanVerificationPath &other) const {
	if (root != other.root) {
		return root < other.root;
	}
	for (idx_t index = 0; index < MinValue(components.size(), other.components.size()); index++) {
		auto &left = components[index];
		auto &right = other.components[index];
		if (left.type != right.type) {
			return PathComponentOrder(left.type) < PathComponentOrder(right.type);
		}
		if (left.ordinal != right.ordinal) {
			return left.ordinal < right.ordinal;
		}
	}
	return components.size() < other.components.size();
}

bool LogicalPlanVerificationFunctionIdentity::IsValid() const {
	if (name.empty() || !return_type.IsComplete()) {
		return false;
	}
	for (auto &argument : arguments) {
		if (!argument.IsComplete()) {
			return false;
		}
	}
	return true;
}

bool LogicalPlanVerificationFunctionIdentity::operator==(const LogicalPlanVerificationFunctionIdentity &other) const {
	return catalog == other.catalog && schema == other.schema && name == other.name && arguments == other.arguments &&
	       return_type == other.return_type;
}

bool LogicalPlanVerificationTypeMismatch::IsValid() const {
	return expected_type.IsComplete() && actual_type.IsComplete();
}

bool LogicalPlanVerificationTypeMismatch::operator==(const LogicalPlanVerificationTypeMismatch &other) const {
	return expected_type == other.expected_type && actual_type == other.actual_type;
}

LogicalPlanVerificationConstructIdentity
LogicalPlanVerificationConstructIdentity::LogicalOperator(LogicalOperatorType type) {
	LogicalPlanVerificationConstructIdentity result;
	result.type = LogicalPlanVerificationConstructType::LOGICAL_OPERATOR;
	result.logical_operator = type;
	return result;
}

LogicalPlanVerificationConstructIdentity
LogicalPlanVerificationConstructIdentity::Expression(ExpressionClass expression_class) {
	LogicalPlanVerificationConstructIdentity result;
	result.type = LogicalPlanVerificationConstructType::EXPRESSION;
	result.expression = expression_class;
	return result;
}

LogicalPlanVerificationConstructIdentity
LogicalPlanVerificationConstructIdentity::Function(LogicalPlanVerificationFunctionIdentity identity) {
	LogicalPlanVerificationConstructIdentity result;
	result.type = LogicalPlanVerificationConstructType::FUNCTION;
	result.function = std::move(identity);
	return result;
}

LogicalPlanVerificationConstructIdentity
LogicalPlanVerificationConstructIdentity::SourceFunction(LogicalPlanVerificationFunctionIdentity identity) {
	LogicalPlanVerificationConstructIdentity result;
	result.type = LogicalPlanVerificationConstructType::SOURCE_FUNCTION;
	result.function = std::move(identity);
	return result;
}

LogicalPlanVerificationConstructIdentity LogicalPlanVerificationConstructIdentity::LogicalTypeValue(LogicalType type) {
	LogicalPlanVerificationConstructIdentity result;
	result.type = LogicalPlanVerificationConstructType::LOGICAL_TYPE;
	result.logical_type = std::move(type);
	return result;
}

LogicalPlanVerificationConstructIdentity
LogicalPlanVerificationConstructIdentity::BindingTypeMismatch(LogicalType expected_type, LogicalType actual_type) {
	LogicalPlanVerificationConstructIdentity result;
	result.type = LogicalPlanVerificationConstructType::BINDING_TYPE_MISMATCH;
	result.type_mismatch = LogicalPlanVerificationTypeMismatch {std::move(expected_type), std::move(actual_type)};
	return result;
}

LogicalPlanVerificationConstructIdentity LogicalPlanVerificationConstructIdentity::Extension(string identifier) {
	LogicalPlanVerificationConstructIdentity result;
	result.type = LogicalPlanVerificationConstructType::EXTENSION;
	result.identifier = std::move(identifier);
	return result;
}

LogicalPlanVerificationConstructIdentity LogicalPlanVerificationConstructIdentity::ExportFeature(string identifier) {
	LogicalPlanVerificationConstructIdentity result;
	result.type = LogicalPlanVerificationConstructType::EXPORT_FEATURE;
	result.identifier = std::move(identifier);
	return result;
}

bool LogicalPlanVerificationConstructIdentity::IsValid() const {
	idx_t payload_count = logical_operator.has_value() + expression.has_value() + function.has_value() +
	                      logical_type.has_value() + type_mismatch.has_value() + identifier.has_value();
	if (payload_count != 1) {
		return false;
	}
	switch (type) {
	case LogicalPlanVerificationConstructType::LOGICAL_OPERATOR:
		return logical_operator.has_value() && *logical_operator != LogicalOperatorType::LOGICAL_INVALID;
	case LogicalPlanVerificationConstructType::EXPRESSION:
		return expression.has_value() && *expression != ExpressionClass::INVALID;
	case LogicalPlanVerificationConstructType::FUNCTION:
	case LogicalPlanVerificationConstructType::SOURCE_FUNCTION:
		return function.has_value() && function->IsValid();
	case LogicalPlanVerificationConstructType::LOGICAL_TYPE:
		return logical_type.has_value() && logical_type->IsComplete();
	case LogicalPlanVerificationConstructType::BINDING_TYPE_MISMATCH:
		return type_mismatch.has_value() && type_mismatch->IsValid();
	case LogicalPlanVerificationConstructType::EXTENSION:
	case LogicalPlanVerificationConstructType::EXPORT_FEATURE:
		return identifier.has_value() && !identifier->empty();
	default:
		return false;
	}
}

bool LogicalPlanVerificationConstructIdentity::operator==(const LogicalPlanVerificationConstructIdentity &other) const {
	return type == other.type && logical_operator == other.logical_operator && expression == other.expression &&
	       function == other.function && logical_type == other.logical_type && type_mismatch == other.type_mismatch &&
	       identifier == other.identifier;
}

template <class T>
static bool SerializedLess(const T &left, const T &right) {
	MemoryStream left_stream;
	MemoryStream right_stream;
	BinarySerializer::Serialize(left, left_stream);
	BinarySerializer::Serialize(right, right_stream);
	auto compare_count = MinValue(left_stream.GetPosition(), right_stream.GetPosition());
	for (idx_t index = 0; index < compare_count; index++) {
		if (left_stream.GetData()[index] != right_stream.GetData()[index]) {
			return left_stream.GetData()[index] < right_stream.GetData()[index];
		}
	}
	return left_stream.GetPosition() < right_stream.GetPosition();
}

template <class T>
static bool SerializedEquals(const T &left, const T &right) {
	MemoryStream left_stream;
	MemoryStream right_stream;
	BinarySerializer::Serialize(left, left_stream);
	BinarySerializer::Serialize(right, right_stream);
	if (left_stream.GetPosition() != right_stream.GetPosition()) {
		return false;
	}
	for (idx_t index = 0; index < left_stream.GetPosition(); index++) {
		if (left_stream.GetData()[index] != right_stream.GetData()[index]) {
			return false;
		}
	}
	return true;
}

static bool FunctionIdentityLess(const LogicalPlanVerificationFunctionIdentity &left,
                                 const LogicalPlanVerificationFunctionIdentity &right) {
	if (left.catalog != right.catalog) {
		return left.catalog < right.catalog;
	}
	if (left.schema != right.schema) {
		return left.schema < right.schema;
	}
	if (left.name != right.name) {
		return left.name < right.name;
	}
	for (idx_t index = 0; index < MinValue(left.arguments.size(), right.arguments.size()); index++) {
		if (left.arguments[index] != right.arguments[index]) {
			return SerializedLess(left.arguments[index], right.arguments[index]);
		}
	}
	if (left.arguments.size() != right.arguments.size()) {
		return left.arguments.size() < right.arguments.size();
	}
	if (left.return_type != right.return_type) {
		return SerializedLess(left.return_type, right.return_type);
	}
	return false;
}

static bool TypeMismatchLess(const LogicalPlanVerificationTypeMismatch &left,
                             const LogicalPlanVerificationTypeMismatch &right) {
	if (left.expected_type != right.expected_type) {
		return SerializedLess(left.expected_type, right.expected_type);
	}
	if (left.actual_type != right.actual_type) {
		return SerializedLess(left.actual_type, right.actual_type);
	}
	return false;
}

bool LogicalPlanVerificationConstructIdentity::operator<(const LogicalPlanVerificationConstructIdentity &other) const {
	if (type != other.type) {
		return type < other.type;
	}
	switch (type) {
	case LogicalPlanVerificationConstructType::LOGICAL_OPERATOR:
		return logical_operator < other.logical_operator;
	case LogicalPlanVerificationConstructType::EXPRESSION:
		return expression < other.expression;
	case LogicalPlanVerificationConstructType::FUNCTION:
	case LogicalPlanVerificationConstructType::SOURCE_FUNCTION:
		if (function == other.function) {
			return false;
		}
		if (!function.has_value()) {
			return true;
		}
		if (!other.function.has_value()) {
			return false;
		}
		return FunctionIdentityLess(*function, *other.function);
	case LogicalPlanVerificationConstructType::LOGICAL_TYPE:
		if (logical_type == other.logical_type) {
			return false;
		}
		if (!logical_type.has_value()) {
			return true;
		}
		if (!other.logical_type.has_value()) {
			return false;
		}
		return SerializedLess(*logical_type, *other.logical_type);
	case LogicalPlanVerificationConstructType::BINDING_TYPE_MISMATCH:
		if (type_mismatch == other.type_mismatch) {
			return false;
		}
		if (!type_mismatch.has_value()) {
			return true;
		}
		if (!other.type_mismatch.has_value()) {
			return false;
		}
		return TypeMismatchLess(*type_mismatch, *other.type_mismatch);
	case LogicalPlanVerificationConstructType::EXTENSION:
	case LogicalPlanVerificationConstructType::EXPORT_FEATURE:
		return identifier < other.identifier;
	default:
		return false;
	}
}

static bool PhaseAllowsIssue(LogicalPlanVerificationPhase phase, LogicalPlanVerificationIssueCode code) {
	switch (phase) {
	case LogicalPlanVerificationPhase::VERIFY: {
		switch (code) {
		case LogicalPlanVerificationIssueCode::INVALID_BINDING:
		case LogicalPlanVerificationIssueCode::TYPE_MISMATCH:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_OPERATOR:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPRESSION:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_FUNCTION:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_SOURCE:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXTENSION:
		case LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT:
		case LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT:
			return true;
		default:
			return false;
		}
	}
	case LogicalPlanVerificationPhase::EXPRESSION_EXPORT:
		switch (code) {
		case LogicalPlanVerificationIssueCode::INVALID_BINDING:
		case LogicalPlanVerificationIssueCode::TYPE_MISMATCH:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPRESSION:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_FUNCTION:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXTENSION:
		case LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE:
		case LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT:
			return true;
		default:
			return false;
		}
	case LogicalPlanVerificationPhase::PLAN_EXPORT: {
		switch (code) {
		case LogicalPlanVerificationIssueCode::INVALID_BINDING:
		case LogicalPlanVerificationIssueCode::TYPE_MISMATCH:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_OPERATOR:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPRESSION:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_FUNCTION:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_SOURCE:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXTENSION:
		case LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT:
		case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE:
		case LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT:
			return true;
		default:
			return false;
		}
	}
	default:
		return false;
	}
}

static bool IsStableFactType(const LogicalType &type) {
	if (!type.IsComplete() || type.IsNested()) {
		return false;
	}
	switch (type.id()) {
	case LogicalTypeId::TYPE:
	case LogicalTypeId::POINTER:
	case LogicalTypeId::LEGACY_AGGREGATE_STATE:
	case LogicalTypeId::LAMBDA:
		return false;
	default:
		break;
	}
	switch (type.InternalType()) {
	case PhysicalType::INVALID:
	case PhysicalType::UNKNOWN:
	case PhysicalType::BIT:
		return false;
	default:
		return true;
	}
}

static bool FactsAreValid(const vector<pair<string, Value>> &facts) {
	unordered_set<string> names;
	for (auto &fact : facts) {
		if (fact.first.empty() || !IsStableFactType(fact.second.type()) || !names.insert(fact.first).second) {
			return false;
		}
	}
	return true;
}

static bool FactLess(const pair<string, Value> &left, const pair<string, Value> &right) {
	if (left.first != right.first) {
		return left.first < right.first;
	}
	return SerializedLess(left.second, right.second);
}

static bool FactEquals(const pair<string, Value> &left, const pair<string, Value> &right) {
	return left.first == right.first && SerializedEquals(left.second, right.second);
}

void LogicalPlanVerificationIssue::Normalize() {
	std::sort(facts.begin(), facts.end(), FactLess);
}

bool LogicalPlanVerificationIssue::IsValid() const {
	if (!PhaseAllowsIssue(phase, code) || (path.has_value() && !path->IsValid()) ||
	    (construct.has_value() && !construct->IsValid()) || !FactsAreValid(facts)) {
		return false;
	}
	switch (code) {
	case LogicalPlanVerificationIssueCode::INVALID_BINDING:
		return path.has_value();
	case LogicalPlanVerificationIssueCode::TYPE_MISMATCH:
		return path.has_value() && construct.has_value() &&
		       construct->type == LogicalPlanVerificationConstructType::BINDING_TYPE_MISMATCH;
	case LogicalPlanVerificationIssueCode::UNSUPPORTED_OPERATOR:
		return path.has_value() && construct.has_value() &&
		       construct->type == LogicalPlanVerificationConstructType::LOGICAL_OPERATOR &&
		       construct->logical_operator.has_value() &&
		       construct->logical_operator.value() != LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR;
	case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPRESSION:
		return path.has_value() && construct.has_value() &&
		       construct->type == LogicalPlanVerificationConstructType::EXPRESSION;
	case LogicalPlanVerificationIssueCode::UNSUPPORTED_FUNCTION:
		return path.has_value() && construct.has_value() &&
		       construct->type == LogicalPlanVerificationConstructType::FUNCTION;
	case LogicalPlanVerificationIssueCode::UNSUPPORTED_SOURCE:
		return path.has_value() && construct.has_value() &&
		       construct->type == LogicalPlanVerificationConstructType::SOURCE_FUNCTION;
	case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXTENSION:
		return path.has_value() && construct.has_value() &&
		       construct->type == LogicalPlanVerificationConstructType::EXTENSION;
	case LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT:
		return construct.has_value() && construct->type == LogicalPlanVerificationConstructType::EXTENSION;
	case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE:
		return path.has_value() && construct.has_value() &&
		       construct->type != LogicalPlanVerificationConstructType::BINDING_TYPE_MISMATCH;
	case LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT:
		return true;
	default:
		return false;
	}
}

bool LogicalPlanVerificationIssue::operator==(const LogicalPlanVerificationIssue &other) const {
	if (code != other.code || phase != other.phase || !(path == other.path) || !(construct == other.construct) ||
	    facts.size() != other.facts.size()) {
		return false;
	}
	for (idx_t fact_index = 0; fact_index < facts.size(); fact_index++) {
		if (!FactEquals(facts[fact_index], other.facts[fact_index])) {
			return false;
		}
	}
	return true;
}

bool LogicalPlanVerificationIssue::operator<(const LogicalPlanVerificationIssue &other) const {
	if (!(path == other.path)) {
		if (!path.has_value()) {
			return true;
		}
		if (!other.path.has_value()) {
			return false;
		}
		return *path < *other.path;
	}
	if (phase != other.phase) {
		return phase < other.phase;
	}
	if (code != other.code) {
		return code < other.code;
	}
	if (!(construct == other.construct)) {
		if (!construct.has_value()) {
			return true;
		}
		if (!other.construct.has_value()) {
			return false;
		}
		return *construct < *other.construct;
	}
	return std::lexicographical_compare(facts.begin(), facts.end(), other.facts.begin(), other.facts.end(), FactLess);
}

} // namespace duckdb
