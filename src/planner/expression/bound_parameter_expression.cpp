#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

BoundParameterExpression::BoundParameterExpression(const duckdb::Identifier &identifier, duckdb::Identifier alias_p)
    : Expression(ExpressionType::VALUE_PARAMETER, ExpressionClass::BOUND_PARAMETER,
                 LogicalType(LogicalTypeId::UNKNOWN)),
      identifier(identifier) {
	alias = std::move(alias_p);
}

BoundParameterExpression::BoundParameterExpression(bound_parameter_map_t &global_parameter_set,
                                                   duckdb::Identifier identifier, LogicalType return_type,
                                                   shared_ptr<BoundParameterData> parameter_data)
    : Expression(ExpressionType::VALUE_PARAMETER, ExpressionClass::BOUND_PARAMETER, std::move(return_type)),
      identifier(std::move(identifier)) {
	// check if we have already deserialized a parameter with this number
	auto entry = global_parameter_set.find(this->identifier);
	if (entry == global_parameter_set.end()) {
		// we have not - store the entry we deserialized from this parameter expression
		global_parameter_set[this->identifier] = parameter_data;
	} else {
		// we have! use the previously deserialized entry
		parameter_data = entry->second;
	}
	this->parameter_data = std::move(parameter_data);
}

void BoundParameterExpression::Invalidate(Expression &expr) {
	if (expr.GetExpressionType() != ExpressionType::VALUE_PARAMETER) {
		throw InternalException("BoundParameterExpression::Invalidate requires a parameter as input");
	}
	auto &bound_parameter = expr.Cast<BoundParameterExpression>();
	bound_parameter.return_type = LogicalTypeId::SQLNULL;
	bound_parameter.parameter_data->return_type = LogicalTypeId::INVALID;
}

void BoundParameterExpression::InvalidateRecursive(Expression &expr) {
	if (expr.GetExpressionType() == ExpressionType::VALUE_PARAMETER) {
		Invalidate(expr);
		return;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](Expression &child) { InvalidateRecursive(child); });
}

bool BoundParameterExpression::IsScalar() const {
	return true;
}
bool BoundParameterExpression::HasParameter() const {
	return true;
}
bool BoundParameterExpression::IsFoldable() const {
	return false;
}

string BoundParameterExpression::ToString() const {
	return "$" + identifier;
}

bool BoundParameterExpression::Equals(const BaseExpression &other_p) const {
	if (!Expression::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<BoundParameterExpression>();
	return identifier == other.identifier;
}

hash_t BoundParameterExpression::Hash() const {
	hash_t result = Expression::Hash();
	result = CombineHash(duckdb::Hash(identifier.c_str(), identifier.size()), result);
	return result;
}

unique_ptr<Expression> BoundParameterExpression::Copy() const {
	auto result = make_uniq<BoundParameterExpression>(identifier);
	result->parameter_data = parameter_data;
	result->return_type = return_type;
	result->CopyProperties(*this);
	return std::move(result);
}

} // namespace duckdb
