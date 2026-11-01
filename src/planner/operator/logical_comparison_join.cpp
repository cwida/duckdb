#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/type_visitor.hpp"

namespace duckdb {

LogicalComparisonJoin::LogicalComparisonJoin(JoinType join_type, LogicalOperatorType logical_type)
    : LogicalJoin(join_type, logical_type) {
}

InsertionOrderPreservingMap<string> LogicalComparisonJoin::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Join Type"] = EnumUtil::ToChars(join_type);

	string conditions_info;
	for (idx_t i = 0; i < conditions.size(); i++) {
		if (i > 0) {
			conditions_info += "\n";
		}
		auto &condition = conditions[i];
		if (condition.IsComparison()) {
			auto expr = BoundComparisonExpression::Create(condition.GetComparisonType(), condition.GetLHS().Copy(),
			                                              condition.GetRHS().Copy());
			conditions_info += expr->ToString();
		} else {
			conditions_info += condition.GetJoinExpression().ToString();
		}
	}
	result["Conditions"] = conditions_info;
	SetParamsEstimatedCardinality(result);

	return result;
}

bool LogicalComparisonJoin::HasEquality(idx_t &range_count) const {
	bool result = false;
	for (size_t c = 0; c < conditions.size(); ++c) {
		auto &cond = conditions[c];
		if (cond.IsComparison()) {
			switch (cond.GetComparisonType()) {
			case ExpressionType::COMPARE_EQUAL:
			case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
				result = true;
				break;
			case ExpressionType::COMPARE_LESSTHAN:
			case ExpressionType::COMPARE_GREATERTHAN:
			case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
				++range_count;
				break;
			case ExpressionType::COMPARE_NOTEQUAL:
			case ExpressionType::COMPARE_DISTINCT_FROM:
				break;
			default:
				throw NotImplementedException("Unimplemented comparison join");
			}
		}
	}
	return result;
}

bool LogicalComparisonJoin::HasArbitraryConditions() const {
	for (size_t c = 0; c < conditions.size(); ++c) {
		auto &cond = conditions[c];
		if (!cond.IsComparison()) {
			return true;
		}
	}
	return false;
}

static bool MarkJoinTypesMatch(const LogicalType &left, const LogicalType &right) {
	if (left != right) {
		return false;
	}
	vector<string> collations;
	TypeVisitor::Contains(left, [&](const LogicalType &type) {
		if (type.id() == LogicalTypeId::VARCHAR) {
			collations.push_back(StringType::GetCollation(type));
		}
		return false;
	});
	idx_t index = 0;
	const auto mismatch = TypeVisitor::Contains(right, [&](const LogicalType &type) {
		if (type.id() != LogicalTypeId::VARCHAR) {
			return false;
		}
		if (index >= collations.size()) {
			return true;
		}
		const auto &collation = collations[index];
		index++;
		return collation != StringType::GetCollation(type);
	});
	return !mismatch && index == collations.size();
}

bool LogicalComparisonJoin::HasNullSafeMarkJoinConditions() const {
	if (join_type != JoinType::MARK) {
		return false;
	}
	bool has_hash_key = false;
	for (auto &condition : conditions) {
		if (!condition.IsComparison()) {
			return false;
		}
		switch (condition.GetComparisonType()) {
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
			has_hash_key = true;
			break;
		case ExpressionType::COMPARE_DISTINCT_FROM:
			break;
		default:
			return false;
		}
	}
	return has_hash_key;
}

bool LogicalComparisonJoin::TryGetMarkJoinGroupTypes(vector<LogicalType> &group_types) const {
	group_types.clear();
	if (join_type != JoinType::MARK || conditions.size() < 2) {
		return false;
	}
	vector<LogicalType> result;
	for (idx_t i = 0; i < conditions.size(); i++) {
		auto &condition = conditions[i];
		// Reconstruct matching bound types only; SQL binding may first insert casts.
		if (!condition.IsComparison() || condition.GetLHS().GetReturnType() != condition.GetRHS().GetReturnType()) {
			return false;
		}
		if (i + 1 < conditions.size()) {
			if (condition.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM ||
			    !MarkJoinTypesMatch(condition.GetLHS().GetReturnType(), condition.GetRHS().GetReturnType())) {
				return false;
			}
			result.push_back(condition.GetLHS().GetReturnType());
			continue;
		}
		auto type_id = condition.GetLHS().GetReturnType().id();
		// Native UNION quantifiers are supported, but grouped SQL reconstruction is not yet supported.
		if (type_id == LogicalTypeId::UNION ||
		    (type_id == LogicalTypeId::TUPLE && condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL)) {
			return false;
		}
		switch (condition.GetComparisonType()) {
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOTEQUAL:
			break;
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			if (condition.GetLHS().GetReturnType().IsNested()) {
				return false;
			}
			break;
		default:
			return false;
		}
	}
	group_types = std::move(result);
	return true;
}

} // namespace duckdb
