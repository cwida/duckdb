//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/logical_plan_verification_result.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/enums/logical_operator_type.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

enum class LogicalPlanVerificationPathRoot { LOGICAL_PLAN, STANDALONE_EXPRESSION };

enum class LogicalPlanVerificationPathComponentType : int32_t { OPERATOR_CHILD, OPERATOR_EXPRESSION, EXPRESSION_CHILD };

struct LogicalPlanVerificationPathComponent {
	LogicalPlanVerificationPathComponentType type;
	idx_t ordinal;

	DUCKDB_API bool operator==(const LogicalPlanVerificationPathComponent &other) const;
};

struct LogicalPlanVerificationPath {
	LogicalPlanVerificationPathRoot root = LogicalPlanVerificationPathRoot::LOGICAL_PLAN;
	vector<LogicalPlanVerificationPathComponent> components;

	DUCKDB_API bool IsValid() const;
	DUCKDB_API bool operator==(const LogicalPlanVerificationPath &other) const;
	DUCKDB_API bool operator<(const LogicalPlanVerificationPath &other) const;
};

enum class LogicalPlanVerificationConstructType {
	LOGICAL_OPERATOR,
	EXPRESSION,
	FUNCTION,
	SOURCE_FUNCTION,
	LOGICAL_TYPE,
	BINDING_TYPE_MISMATCH,
	EXTENSION,
	EXPORT_FEATURE
};

struct LogicalPlanVerificationFunctionIdentity {
	string catalog;
	string schema;
	string name;
	vector<LogicalType> arguments;
	LogicalType return_type;

	DUCKDB_API bool IsValid() const;
	DUCKDB_API bool operator==(const LogicalPlanVerificationFunctionIdentity &other) const;
};

struct LogicalPlanVerificationTypeMismatch {
	LogicalType expected_type;
	LogicalType actual_type;

	DUCKDB_API bool IsValid() const;
	DUCKDB_API bool operator==(const LogicalPlanVerificationTypeMismatch &other) const;
};

struct LogicalPlanVerificationConstructIdentity {
	LogicalPlanVerificationConstructType type = LogicalPlanVerificationConstructType::LOGICAL_OPERATOR;
	optional<LogicalOperatorType> logical_operator;
	optional<ExpressionClass> expression;
	optional<LogicalPlanVerificationFunctionIdentity> function;
	optional<LogicalType> logical_type;
	optional<LogicalPlanVerificationTypeMismatch> type_mismatch;
	optional<string> identifier;

	DUCKDB_API static LogicalPlanVerificationConstructIdentity LogicalOperator(LogicalOperatorType type);
	DUCKDB_API static LogicalPlanVerificationConstructIdentity Expression(ExpressionClass expression_class);
	DUCKDB_API static LogicalPlanVerificationConstructIdentity
	Function(LogicalPlanVerificationFunctionIdentity identity);
	DUCKDB_API static LogicalPlanVerificationConstructIdentity
	SourceFunction(LogicalPlanVerificationFunctionIdentity identity);
	DUCKDB_API static LogicalPlanVerificationConstructIdentity LogicalTypeValue(LogicalType type);
	DUCKDB_API static LogicalPlanVerificationConstructIdentity BindingTypeMismatch(LogicalType expected_type,
	                                                                               LogicalType actual_type);
	DUCKDB_API static LogicalPlanVerificationConstructIdentity Extension(string identifier);
	DUCKDB_API static LogicalPlanVerificationConstructIdentity ExportFeature(string identifier);

	DUCKDB_API bool IsValid() const;
	DUCKDB_API bool operator==(const LogicalPlanVerificationConstructIdentity &other) const;
	DUCKDB_API bool operator<(const LogicalPlanVerificationConstructIdentity &other) const;
};

enum class LogicalPlanVerificationIssueCode : int32_t {
	INVALID_BINDING,
	TYPE_MISMATCH,
	UNSUPPORTED_OPERATOR,
	UNSUPPORTED_EXPRESSION,
	UNSUPPORTED_FUNCTION,
	UNSUPPORTED_SOURCE,
	UNSUPPORTED_EXTENSION,
	MALFORMED_EXTENSION_RESULT,
	UNSUPPORTED_EXPORT_FEATURE,
	INTERNAL_INVARIANT
};

enum class LogicalPlanVerificationPhase : int32_t { VERIFY, EXPRESSION_EXPORT, PLAN_EXPORT };

struct LogicalPlanVerificationIssue {
	LogicalPlanVerificationIssueCode code = LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT;
	LogicalPlanVerificationPhase phase = LogicalPlanVerificationPhase::VERIFY;
	optional<LogicalPlanVerificationPath> path;
	optional<LogicalPlanVerificationConstructIdentity> construct;
	vector<pair<string, Value>> facts;
	string message;

	DUCKDB_API bool IsValid() const;
	DUCKDB_API void Normalize();
	DUCKDB_API bool operator==(const LogicalPlanVerificationIssue &other) const;
	DUCKDB_API bool operator<(const LogicalPlanVerificationIssue &other) const;
};

struct LogicalPlanVerificationSuccess {
	bool operator==(const LogicalPlanVerificationSuccess &other) const {
		return true;
	}
};

template <class T>
class LogicalPlanVerificationResult {
public:
	static LogicalPlanVerificationResult Success(T value) {
		return LogicalPlanVerificationResult(optional<T>(std::move(value)), {});
	}

	//! Propagate the normalized issues of a failed result carrying a different value type
	template <class U>
	static LogicalPlanVerificationResult Failure(const LogicalPlanVerificationResult<U> &failed) {
		D_ASSERT(failed.HasError());
		return LogicalPlanVerificationResult(optional<T>(), failed.GetIssues());
	}

	static LogicalPlanVerificationResult Failure(vector<LogicalPlanVerificationIssue> issues) {
		for (auto &issue : issues) {
			if (!issue.IsValid()) {
				return LogicalPlanVerificationResult(optional<T>(), std::move(issues));
			}
		}
		for (auto &issue : issues) {
			issue.Normalize();
		}
		std::sort(issues.begin(), issues.end());
		issues.erase(std::unique(issues.begin(), issues.end()), issues.end());
		return LogicalPlanVerificationResult(optional<T>(), std::move(issues));
	}

	bool IsSuccess() const {
		return value.has_value();
	}

	bool HasError() const {
		return !IsSuccess();
	}

	T &GetValue() {
		if (!value) {
			throw InternalException("Cannot get the value of a failed logical plan verification result");
		}
		return *value;
	}

	const T &GetValue() const {
		if (!value) {
			throw InternalException("Cannot get the value of a failed logical plan verification result");
		}
		return *value;
	}

	const vector<LogicalPlanVerificationIssue> &GetIssues() const {
		return issues;
	}

	bool IsValid() const {
		if (IsSuccess()) {
			return issues.empty();
		}
		if (issues.empty()) {
			return false;
		}
		for (auto &issue : issues) {
			if (!issue.IsValid()) {
				return false;
			}
		}
		return true;
	}

private:
	LogicalPlanVerificationResult(optional<T> value_p, vector<LogicalPlanVerificationIssue> issues_p)
	    : value(std::move(value_p)), issues(std::move(issues_p)) {
	}

private:
	optional<T> value;
	vector<LogicalPlanVerificationIssue> issues;
};

} // namespace duckdb
