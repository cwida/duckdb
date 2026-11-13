//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/sql_export_helpers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/collate_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/logical_plan_verification_result.hpp"

namespace duckdb {
struct SQLExportHelpers {
	template <class ARGUMENT>
	static unique_ptr<FunctionExpression> SystemFunction(Identifier name, vector<ARGUMENT> arguments) {
		return make_uniq<FunctionExpression>(
		    QualifiedName(Identifier::SystemCatalog(), Identifier::DefaultSchema(), std::move(name)),
		    std::move(arguments));
	}

	static unique_ptr<ParsedExpression> Conjoin(unique_ptr<ParsedExpression> left, unique_ptr<ParsedExpression> right) {
		if (!left) {
			return right;
		}
		if (!right) {
			return left;
		}
		return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(left), std::move(right));
	}

	static unique_ptr<ParsedExpression> Conjoin(vector<unique_ptr<ParsedExpression>> predicates) {
		if (predicates.empty()) {
			return nullptr;
		}
		if (predicates.size() == 1) {
			return std::move(predicates[0]);
		}
		return make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(predicates));
	}

	static LogicalPlanVerificationPath ChildPath(
	    const LogicalPlanVerificationPath &path, idx_t ordinal,
	    LogicalPlanVerificationPathComponentType type = LogicalPlanVerificationPathComponentType::EXPRESSION_CHILD) {
		auto result = path;
		result.components.push_back({type, ordinal});
		return result;
	}

	static LogicalPlanVerificationIssue MakeIssue(LogicalPlanVerificationIssueCode code,
	                                              LogicalPlanVerificationPhase phase,
	                                              optional<LogicalPlanVerificationPath> path,
	                                              optional<LogicalPlanVerificationConstructIdentity> construct,
	                                              string message) {
		LogicalPlanVerificationIssue issue;
		issue.code = code;
		issue.phase = phase;
		issue.path = std::move(path);
		issue.construct = std::move(construct);
		issue.message = std::move(message);
		return issue;
	}

	//! Constant ORDER BY keys have no effect, and a literal key would be bound as a position or rejected
	static bool IsConstantOrderKey(const Expression &bound, const ParsedExpression &exported) {
		return bound.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT ||
		       exported.GetExpressionClass() == ExpressionClass::CONSTANT;
	}

	static unique_ptr<ParsedExpression> OrderExpression(const LogicalType &type,
	                                                    unique_ptr<ParsedExpression> expression) {
		if (type.id() == LogicalTypeId::VARCHAR) {
			if (type.HasAlias()) {
				expression = make_uniq<CastExpression>(LogicalType::VARCHAR, std::move(expression));
			}
			return make_uniq<CollateExpression>("C", std::move(expression));
		}
		return expression;
	}

	static bool IsValidIdentifier(const Identifier &identifier) {
		auto &name = identifier.GetIdentifierName();
		return !name.empty() && name.find('\0') == string::npos && Value::StringIsValid(name);
	}

	static bool IsSQLExportType(LogicalTypeId id) {
		return TypeExpression::IsSQLType(id);
	}

	static bool IsSQLRepresentableType(const LogicalType &type) {
		return TypeExpression::CanRepresent(type);
	}

	static bool IsSQLValueType(const LogicalType &type) {
		return type.IsComplete() &&
		       !TypeVisitor::Contains(type, [](const LogicalType &child) { return !IsSQLExportType(child.id()); });
	}

	static string TypeCollationSignature(const LogicalType &type) {
		string result;
		TypeVisitor::Contains(type, [&](const LogicalType &child) {
			if (child.id() == LogicalTypeId::VARCHAR) {
				const auto collation = StringType::GetCollation(child);
				result += std::to_string(collation.size()) + ":" + collation + ";";
			}
			return false;
		});
		return result;
	}
};
} // namespace duckdb
