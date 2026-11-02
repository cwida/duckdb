
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/optimizer/rule/in_clause_simplification.hpp"
#include "duckdb/optimizer/matcher/type_matcher_id.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/unordered_set.hpp"

namespace duckdb {

InClauseSimplificationRule::InClauseSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match on InClauseExpression that has a ConstantExpression as a check
	auto op = make_uniq<InClauseExpressionMatcher>();
	op->policy = SetMatcher::Policy::SOME;
	root = std::move(op);
}

unique_ptr<Expression> InClauseSimplificationRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                         bool &changes_made, bool is_root) {
	auto &expr = bindings[0].get().Cast<BoundOperatorExpression>();
	if (!BoundCastExpression::IsCast(*expr.GetChildrenMutable()[0])) {
		return nullptr;
	}
	auto &cast_expression = expr.GetChildrenMutable()[0]->Cast<BoundFunctionExpression>();
	if (BoundCastExpression::IsTryCast(cast_expression)) {
		// a TRY_CAST turns failing values into NULL, which is lost when the cast is removed
		return nullptr;
	}
	if (BoundCastExpression::Child(cast_expression).GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	//! The goal here is to remove the cast from the probe expression
	//! and apply a cast to the constant expressions. We can only do this
	//! if the semantics do not change, which only happens when BOTH casts
	//! are invertible.
	auto target_type = BoundCastExpression::SourceType(cast_expression);
	if (!BoundCastExpression::CastIsInvertible(target_type, cast_expression.GetReturnType())) {
		return nullptr;
	}
	vector<unique_ptr<BoundConstantExpression>> cast_list;
	//! First check if we can cast all children
	for (size_t i = 1; i < expr.GetChildrenMutable().size(); i++) {
		if (expr.GetChildrenMutable()[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return nullptr;
		}
		D_ASSERT(expr.GetChildrenMutable()[i]->IsFoldable());
		auto constant_value = ExpressionExecutor::EvaluateScalar(GetContext(), *expr.GetChildrenMutable()[i]);
		if (!BoundCastExpression::CastIsInvertible(constant_value.type(), target_type)) {
			return nullptr;
		}
		auto new_constant = constant_value.DefaultTryCastAs(target_type);
		if (!new_constant) {
			return nullptr;
		} else {
			auto new_constant_expr = make_uniq<BoundConstantExpression>(std::move(*new_constant));
			cast_list.push_back(std::move(new_constant_expr));
		}
	}
	//! We can cast, so we move the new constant
	for (size_t i = 1; i < expr.GetChildrenMutable().size(); i++) {
		expr.GetChildrenMutable()[i] = std::move(cast_list[i - 1]);

		//		expr->children[i] = std::move(new_constant_expr);
	}
	//! We can cast the full list, so we move the column
	expr.GetChildrenMutable()[0] = std::move(BoundCastExpression::ChildMutable(cast_expression));
	return nullptr;
}

class EnumInProbeMatcher : public ExpressionMatcher {
public:
	EnumInProbeMatcher() : ExpressionMatcher() {
	}

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override {
		// Support both enum_col IN (...) and enum_col::VARCHAR IN (...).
		if (expr.GetReturnType().id() == LogicalTypeId::ENUM) {
			bindings.push_back(expr);
			return true;
		}
		if (!BoundCastExpression::IsCast(expr) || expr.GetReturnType() != LogicalType::VARCHAR) {
			return false;
		}
		auto &cast_expr = expr.Cast<BoundFunctionExpression>();
		if (BoundCastExpression::Child(cast_expr).GetReturnType().id() != LogicalTypeId::ENUM) {
			return false;
		}
		bindings.push_back(expr);
		return true;
	}
};

class EnumInConstantMatcher : public ExpressionMatcher {
public:
	EnumInConstantMatcher() : ExpressionMatcher(ExpressionClass::BOUND_CONSTANT) {
	}

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override {
		if (!ExpressionMatcher::Match(expr, bindings)) {
			return false;
		}
		auto &constant = expr.Cast<BoundConstantExpression>().GetValue();
		if (constant.IsNull() || constant.type().id() == LogicalTypeId::VARCHAR ||
		    constant.type().id() == LogicalTypeId::ENUM) {
			return true;
		}
		bindings.pop_back();
		return false;
	}
};

InEnumSimplificationRule::InEnumSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match enum IN constants and enum::VARCHAR IN constants
	auto op = make_uniq<InUniformExpressionMatcher>();

	op->probe_matcher = make_uniq<EnumInProbeMatcher>();
	op->child_matcher = make_uniq<EnumInConstantMatcher>();

	root = std::move(op);
}

static optional_ptr<const Expression> GetEnumInProbe(const Expression &expr) {
	if (expr.GetReturnType().id() == LogicalTypeId::ENUM) {
		return expr;
	}
	if (!BoundCastExpression::IsCast(expr) || expr.GetReturnType() != LogicalType::VARCHAR) {
		return nullptr;
	}
	auto &cast_expr = expr.Cast<BoundFunctionExpression>();
	auto &child = BoundCastExpression::Child(cast_expr);
	if (child.GetReturnType().id() != LogicalTypeId::ENUM) {
		return nullptr;
	}
	return child;
}

unique_ptr<Expression> InEnumSimplificationRule::Apply(LogicalOperator &op, vector<reference<Expression>> &bindings,
                                                       bool &changes_made, bool is_root) {
	auto &expr = bindings[0].get().Cast<BoundOperatorExpression>();
	auto &children = expr.GetChildrenMutable();
	auto enum_expr = GetEnumInProbe(*children[0]);
	if (enum_expr == nullptr) {
		return nullptr;
	}
	auto &enum_type = enum_expr->GetReturnType();

	// Look up the domain for the original ENUM
	string_map_t<uint64_t> enum_domain;
	auto strings = FlatVector::GetData<string_t>(EnumType::GetValuesInsertOrder(enum_type));
	for (uint64_t i = 0; i < EnumType::GetSize(enum_type); ++i) {
		enum_domain[strings[i]] = i;
	}

	vector<unique_ptr<Expression>> in_children;
	in_children.emplace_back(enum_expr->Copy());

	unordered_set<uint64_t> matched_enum_values {};
	for (idx_t i = 1; i < children.size(); ++i) {
		auto &child = *children[i];
		auto &v = child.Cast<BoundConstantExpression>().GetValue();
		if (v.IsNull()) {
			//	Keep NULLs
			in_children.emplace_back(make_uniq<BoundConstantExpression>(Value(enum_type)));
		} else {
			if (v.type().id() != LogicalTypeId::VARCHAR && v.type().id() != LogicalTypeId::ENUM) {
				return nullptr;
			}
			// Check values against the probe enum domain before rebuilding typed enum constants.
			auto s = v.ToString();
			auto it = enum_domain.find(s);
			if (it != enum_domain.end()) {
				//	The literal is in the ENUM domain, so translate it
				in_children.emplace_back(make_uniq<BoundConstantExpression>(Value::ENUM(it->second, enum_type)));
				matched_enum_values.insert(it->second);
			}
			// Not in the domain, so ignore it.
		}
	}

	//	If all ENUM values are present, replace with a constant for non-NULL inputs.
	//	If SOME of them are in the domain, swap out the children for the valid ENUM values.
	if (matched_enum_values.size() == EnumType::GetSize(enum_type)) {
		const bool in_clause = (expr.GetExpressionType() == ExpressionType::COMPARE_IN);
		return rewriter.ConstantOrNull(GetContext(), in_children[0]->Copy(), Value::BOOLEAN(in_clause));
	} else if (in_children.size() > 1) {
		children.swap(in_children);
	} else {
		// constant_or_null(not_in, child[0])
		const bool not_in = (expr.GetExpressionType() == ExpressionType::COMPARE_NOT_IN);
		return rewriter.ConstantOrNull(GetContext(), in_children[0]->Copy(), Value::BOOLEAN(not_in));
	}

	return nullptr;
}

EnumCompareSimplificationRule::EnumCompareSimplificationRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	// match enum::VARCHAR = string literal
	auto op = make_uniq<ComparisonExpressionMatcher>();
	vector<ExpressionType> types;
	types.emplace_back(ExpressionType::COMPARE_EQUAL);
	types.emplace_back(ExpressionType::COMPARE_NOTEQUAL);
	types.emplace_back(ExpressionType::COMPARE_DISTINCT_FROM);
	types.emplace_back(ExpressionType::COMPARE_NOT_DISTINCT_FROM);
	op->expr_type = make_uniq<ManyExpressionTypeMatcher>(types);

	//	One side must be enum::VARCHAR
	auto cast_matcher = make_uniq<CastExpressionMatcher>();
	cast_matcher->type = make_uniq<SpecificTypeMatcher>(LogicalType::VARCHAR);
	auto enum_matcher = make_uniq<ExpressionMatcher>();
	enum_matcher->type = make_uniq<TypeMatcherId>(LogicalTypeId::ENUM);
	cast_matcher->matcher = std::move(enum_matcher);
	op->matchers.emplace_back(std::move(cast_matcher));

	//	The other must must be a constant string
	auto const_matcher = make_uniq<ExpressionMatcher>(ExpressionClass::BOUND_CONSTANT);
	const_matcher->type = make_uniq<SpecificTypeMatcher>(LogicalType::VARCHAR);
	op->matchers.emplace_back(std::move(const_matcher));

	op->policy = SetMatcher::Policy::UNORDERED;

	root = std::move(op);
}

unique_ptr<Expression> EnumCompareSimplificationRule::Apply(LogicalOperator &op,
                                                            vector<reference<Expression>> &bindings, bool &changes_made,
                                                            bool is_root) {
	auto &expr = bindings[0].get().Cast<BoundFunctionExpression>();

	optional_ptr<BoundFunctionExpression> cast_expr;
	optional_ptr<BoundConstantExpression> const_expr;
	idx_t cast_idx = 0;
	if (BoundComparisonExpression::Left(expr).GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		cast_expr = BoundComparisonExpression::RightMutable(expr)->Cast<BoundFunctionExpression>();
		const_expr = BoundComparisonExpression::LeftMutable(expr)->Cast<BoundConstantExpression>();
		cast_idx = 1;
	} else {
		cast_expr = BoundComparisonExpression::LeftMutable(expr)->Cast<BoundFunctionExpression>();
		const_expr = BoundComparisonExpression::RightMutable(expr)->Cast<BoundConstantExpression>();
	}

	auto &enum_expr = BoundCastExpression::Child(*cast_expr);
	auto &enum_type = enum_expr.GetReturnType();

	vector<unique_ptr<Expression>> cmp_children;
	cmp_children.emplace_back(enum_expr.Copy());

	auto &v = const_expr->GetValue();
	if (v.IsNull()) {
		//	Keep NULLs
		cmp_children.emplace_back(make_uniq<BoundConstantExpression>(Value(enum_type)));
	} else {
		// Look up the string in the domain for the original ENUM
		const auto s = v.ToString();
		auto strings = FlatVector::GetData<string_t>(EnumType::GetValuesInsertOrder(enum_type));
		for (uint64_t i = 0; i < EnumType::GetSize(enum_type); ++i) {
			if (strings[i] == s) {
				//	The literal is in the ENUM domain, so translate it
				cmp_children.emplace_back(make_uniq<BoundConstantExpression>(Value::ENUM(i, enum_type)));
				if (cast_idx) {
					// Restore the original orientation
					std::swap(cmp_children[0], cmp_children[1]);
				}
				break;
			}
		}
	}

	if (cmp_children.size() == 2) {
		//	In the domain, so rewrite as enum = constant
		BoundComparisonExpression::LeftMutable(expr) = std::move(cmp_children[0]);
		BoundComparisonExpression::RightMutable(expr) = std::move(cmp_children[1]);
	} else {
		//	Not in the domain, so rewrite as ConstantOrNull(enum, ne)
		switch (expr.GetExpressionType()) {
		case ExpressionType::COMPARE_EQUAL:
			return rewriter.ConstantOrNull(GetContext(), std::move(cmp_children[0]), Value::BOOLEAN(false));
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
			return make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
		case ExpressionType::COMPARE_NOTEQUAL:
			return rewriter.ConstantOrNull(GetContext(), std::move(cmp_children[0]), Value::BOOLEAN(true));
		case ExpressionType::COMPARE_DISTINCT_FROM:
			return make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
		default:
			break;
		}
	}

	return nullptr;
}

} // namespace duckdb
