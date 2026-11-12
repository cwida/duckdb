#include "duckdb/optimizer/runtime_filter_cast.hpp"

#include "duckdb/execution/operator/join/join_filter_pushdown.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

bool RuntimeFilterCastUtil::RuntimeFilterCastCanFail(const LogicalType &source_type, const LogicalType &target_type) {
	if (source_type == target_type) {
		return false;
	}
	if (source_type.id() == LogicalTypeId::VARIANT || target_type.id() == LogicalTypeId::VARIANT ||
	    !source_type.IsIntegral() || !target_type.IsIntegral()) {
		return true;
	}
	const auto source_size = GetTypeIdSize(source_type.InternalType());
	const auto target_size = GetTypeIdSize(target_type.InternalType());
	if (source_size > target_size) {
		return true;
	}
	if (source_type.IsSigned() == target_type.IsSigned()) {
		return false;
	}
	if (source_type.IsSigned()) {
		return true;
	}
	return source_size >= target_size;
}

bool RuntimeFilterCastUtil::IsBuiltinCast(const Expression &expr) {
	if (!BoundCastExpression::IsCast(expr)) {
		return false;
	}
	auto &cast = expr.Cast<BoundFunctionExpression>();
	if (!BoundCastExpression::HasValidBindData(cast)) {
		return false;
	}
	auto &bound_cast = BoundCastExpression::GetBoundCast(cast);
	if (bound_cast.GetCastData()) {
		return false;
	}
	CastFunctionSet default_casts;
	auto input = GetCastFunctionInput::Default();
	auto default_cast =
	    default_casts.GetCastFunction(BoundCastExpression::Child(cast).GetReturnType(), cast.GetReturnType(), input);
	return !default_cast.GetCastData() &&
	       (bound_cast.Equals(default_cast) || bound_cast.HasSameStatisticsCallback(default_cast));
}

static bool RuntimeFilterCastNeedsTryCast(const BoundFunctionExpression &cast) {
	return BoundCastExpression::IsTryCast(cast) || !RuntimeFilterCastUtil::IsBuiltinCast(cast) ||
	       RuntimeFilterCastUtil::RuntimeFilterCastCanFail(BoundCastExpression::Child(cast).GetReturnType(),
	                                                       cast.GetReturnType());
}

bool RuntimeFilterCastUtil::RuntimeFilterUsesTryCast(const JoinFilterPushdownColumn &column) {
	if (!column.input_expression) {
		return false;
	}
	bool result = false;
	ExpressionIterator::VisitExpressionClass(
	    *column.input_expression, ExpressionClass::BOUND_FUNCTION, [&](const Expression &expr) {
		    if (BoundCastExpression::IsCast(expr)) {
			    result |= RuntimeFilterCastNeedsTryCast(expr.Cast<BoundFunctionExpression>());
		    }
	    });
	return result;
}

static bool ExpressionPreservesStorageOrdering(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		return true;
	}
	if (!BoundCastExpression::IsCast(expr)) {
		return false;
	}
	auto &cast = expr.Cast<BoundFunctionExpression>();
	return !RuntimeFilterCastNeedsTryCast(cast) && ExpressionPreservesStorageOrdering(BoundCastExpression::Child(cast));
}

bool RuntimeFilterCastUtil::PreservesStorageOrdering(const JoinFilterPushdownColumn &column) {
	return column.mode == JoinFilterPushdownMode::RECONSTRUCT_EXPRESSION &&
	       (!column.input_expression || ExpressionPreservesStorageOrdering(*column.input_expression));
}

LogicalType RuntimeFilterCastUtil::GetRuntimeFilterInputType(const JoinFilterPushdownColumn &column,
                                                             const LogicalType &runtime_type) {
	if (column.mode == JoinFilterPushdownMode::RECONSTRUCT_EXPRESSION && column.input_expression) {
		return column.input_expression->GetReturnType();
	}
	return runtime_type;
}

bool RuntimeFilterCastUtil::RequiresRuntimeFilterExpressionReconstruction(const JoinFilterPushdownColumn &column,
                                                                          const LogicalType &runtime_type) {
	return column.mode == JoinFilterPushdownMode::RECONSTRUCT_EXPRESSION && column.input_expression &&
	       !PreservesStorageOrdering(column);
}

static void PreserveRuntimeFilterCastErrors(unique_ptr<Expression> &expr, bool &preserves_cast_errors) {
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { PreserveRuntimeFilterCastErrors(child, preserves_cast_errors); });
	if (!BoundCastExpression::IsCast(*expr)) {
		return;
	}
	auto &cast = expr->Cast<BoundFunctionExpression>();
	if (BoundCastExpression::IsTryCast(cast) || !RuntimeFilterCastNeedsTryCast(cast)) {
		return;
	}
	auto bound_cast = BoundCastExpression::GetBoundCast(cast).Copy();
	auto &target_type = cast.GetReturnType();
	auto child = std::move(BoundCastExpression::ChildMutable(cast));
	auto safe_cast =
	    BoundCastExpression::IsDefaultCast(cast)
	        ? BoundCastExpression::CreateDefault(std::move(child), target_type, std::move(bound_cast), true)
	        : BoundCastExpression::Create(std::move(child), target_type, std::move(bound_cast), true);
	safe_cast->SetAlias(cast.GetAlias());
	safe_cast->SetQueryLocation(cast.GetQueryLocation());
	expr = std::move(safe_cast);
	preserves_cast_errors = true;
}

unique_ptr<Expression> RuntimeFilterCastUtil::CreateRuntimeFilterInputExpression(ClientContext &context,
                                                                                 const JoinFilterPushdownColumn &column,
                                                                                 bool &preserves_cast_errors) {
	D_ASSERT(column.storage_type.IsValid());
	preserves_cast_errors = false;
	auto input = column.input_expression ? column.input_expression->Copy()
	                                     : make_uniq<BoundReferenceExpression>(column.storage_type, idx_t(0));
	PreserveRuntimeFilterCastErrors(input, preserves_cast_errors);
	return input;
}

} // namespace duckdb
