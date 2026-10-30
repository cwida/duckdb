#include "duckdb/planner/expression.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/parser/expression_util.hpp"

namespace duckdb {

Expression::Expression(ExpressionType type, ExpressionClass expression_class, LogicalType return_type)
    : BaseExpression(type, expression_class), return_type(std::move(return_type)) {
}

unique_ptr<Expression> Expression::PreserveReturnType(const LogicalType &type, unique_ptr<Expression> replacement) {
	D_ASSERT(replacement);
	D_ASSERT(replacement->GetReturnType() == type);
	replacement->SetReturnType(type);
	return replacement;
}

Expression::~Expression() {
}

bool Expression::IsAggregate() const {
	bool is_aggregate = false;
	ExpressionIterator::EnumerateChildren(*this, [&](const Expression &child) { is_aggregate |= child.IsAggregate(); });
	return is_aggregate;
}

bool Expression::IsWindow() const {
	bool is_window = false;
	ExpressionIterator::EnumerateChildren(*this, [&](const Expression &child) { is_window |= child.IsWindow(); });
	return is_window;
}

bool Expression::IsScalar() const {
	bool is_scalar = true;
	ExpressionIterator::EnumerateChildren(*this, [&](const Expression &child) {
		if (!child.IsScalar()) {
			is_scalar = false;
		}
	});
	return is_scalar;
}

bool Expression::IsVolatile() const {
	bool is_volatile = false;
	ExpressionIterator::EnumerateChildren(*this, [&](const Expression &child) {
		if (child.IsVolatile()) {
			is_volatile = true;
		}
	});
	return is_volatile;
}

bool Expression::IsConsistent() const {
	bool is_consistent = true;
	ExpressionIterator::EnumerateChildren(*this, [&](const Expression &child) {
		if (!child.IsConsistent()) {
			is_consistent = false;
		}
	});
	return is_consistent;
}

bool Expression::CanThrow() const {
	bool can_throw = false;
	ExpressionIterator::EnumerateChildren(*this, [&](const Expression &child) { can_throw |= child.CanThrow(); });
	return can_throw;
}

bool Expression::RequiresRebind() const {
	bool requires_rebind = false;
	ExpressionIterator::EnumerateChildren(*this, [&](const Expression &child) {
		if (child.RequiresRebind()) {
			requires_rebind = true;
		}
	});
	return requires_rebind;
}

bool Expression::PropagatesNullValues() const {
	if (type == ExpressionType::OPERATOR_IS_NULL || type == ExpressionType::OPERATOR_IS_NOT_NULL ||
	    type == ExpressionType::COMPARE_NOT_DISTINCT_FROM || type == ExpressionType::COMPARE_DISTINCT_FROM ||
	    type == ExpressionType::CONJUNCTION_OR || type == ExpressionType::CONJUNCTION_AND ||
	    type == ExpressionType::OPERATOR_COALESCE || type == ExpressionType::CASE_EXPR ||
	    type == ExpressionType::OPERATOR_HAS_ERROR) {
		return false;
	}
	// x IN (NULL, x) is TRUE and x NOT IN (NULL, x) is FALSE: a NULL element does not decide the result
	if (type == ExpressionType::COMPARE_IN || type == ExpressionType::COMPARE_NOT_IN) {
		return false;
	}
	bool propagate_null_values = true;
	ExpressionIterator::EnumerateChildren(*this, [&](const Expression &child) {
		if (!child.PropagatesNullValues()) {
			propagate_null_values = false;
		}
	});
	return propagate_null_values;
}

bool Expression::IsFoldable() const {
	bool is_foldable = true;
	ExpressionIterator::EnumerateChildren(*this, [&](const Expression &child) {
		if (!child.IsFoldable()) {
			is_foldable = false;
		}
	});
	return is_foldable;
}

bool Expression::HasParameter() const {
	bool has_parameter = false;
	ExpressionIterator::EnumerateChildren(*this,
	                                      [&](const Expression &child) { has_parameter |= child.HasParameter(); });
	return has_parameter;
}

bool Expression::HasSubquery() const {
	bool has_subquery = false;
	ExpressionIterator::EnumerateChildren(*this, [&](const Expression &child) { has_subquery |= child.HasSubquery(); });
	return has_subquery;
}

hash_t Expression::Hash() const {
	hash_t hash = duckdb::Hash<uint32_t>(static_cast<uint32_t>(type));
	hash = CombineHash(hash, return_type.Hash());
	ExpressionIterator::EnumerateChildren(*this,
	                                      [&](const Expression &child) { hash = CombineHash(child.Hash(), hash); });
	return hash;
}

bool Expression::Equals(const unique_ptr<Expression> &left, const unique_ptr<Expression> &right) {
	if (left.get() == right.get()) {
		return true;
	}
	if (!left || !right) {
		return false;
	}
	return left->Equals(*right);
}

bool Expression::ListEquals(const vector<unique_ptr<Expression>> &left, const vector<unique_ptr<Expression>> &right) {
	return ExpressionUtil::ListEquals(left, right);
}

void Expression::SetVerificationStats(unique_ptr<BaseStatistics> stats) {
	verification_stats = std::move(stats);
}

} // namespace duckdb
