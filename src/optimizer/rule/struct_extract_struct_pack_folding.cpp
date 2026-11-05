#include "duckdb/optimizer/rule/struct_extract_struct_pack_folding.hpp"

#include "duckdb/function/scalar/struct_utils.hpp"
#include "duckdb/optimizer/matcher/expression_matcher.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

StructExtractStructPackFoldingRule::StructExtractStructPackFoldingRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	auto func = make_uniq<FunctionExpressionMatcher>();
	// a bracket subscript on a struct binds to the "array_extract" overload - fold both the same way
	func->function = make_uniq<ManyFunctionMatcher>(identifier_set_t {"struct_extract", "array_extract"});
	func->policy = SetMatcher::Policy::ORDERED;
	auto packed = make_uniq<FunctionExpressionMatcher>();
	packed->function = make_uniq<SpecificFunctionMatcher>("struct_pack");
	packed->policy = SetMatcher::Policy::SOME;
	func->matchers.push_back(std::move(packed));
	func->matchers.push_back(make_uniq<ExpressionMatcher>());
	root = std::move(func);
}

unique_ptr<Expression> StructExtractStructPackFoldingRule::Apply(LogicalOperator &op,
                                                                 vector<reference<Expression>> &bindings,
                                                                 bool &changes_made, bool is_root) {
	auto &extract = bindings[0].get().Cast<BoundFunctionExpression>();
	auto &packed = bindings[1].get().Cast<BoundFunctionExpression>();
	auto &bind_info = extract.BindInfo();
	if (!bind_info) {
		return nullptr;
	}
	auto &extract_data = bind_info->Cast<StructExtractBindData>();
	auto &packed_children = packed.GetChildrenMutable();
	if (extract_data.index >= packed_children.size()) {
		return nullptr;
	}
	// struct_extract(struct_pack(e0, .., en), ki) is equivalent to ei
	auto result = std::move(packed_children[extract_data.index]);
	auto &return_type = extract.GetReturnType();
	// AddCastToType skips the cast when only the collation differs - keep the collation of the extract
	auto cast = BoundCastExpression::AddCastToType(GetContext(), std::move(result), return_type);
	return Expression::PreserveReturnType(return_type, std::move(cast));
}

} // namespace duckdb
