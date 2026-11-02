#include "duckdb/optimizer/rule/list_comprehension_rewrite.hpp"

#include "duckdb/planner/expression/bound_lambda_expression.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

namespace {

bool IsTargetListFunction(ClientContext &context, const BoundFunctionExpression &expr, const string &target_name) {
	if (expr.Function().GetName() == target_name) {
		D_ASSERT(!expr.GetChildren().empty() && expr.GetChildren()[0]->GetReturnType().id() == LogicalTypeId::LIST);
		return true;
	}

	// Compare function name with catalog to recognize aliases
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(
	    context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), expr.Function().GetName()),
	    OnEntryNotFound::RETURN_NULL);
	if (!entry) {
		return false;
	}
	if (!entry->alias_of.empty()) {
		return entry->alias_of == target_name;
	}
	return entry->name == target_name;
}

bool IsStructPack(const BoundFunctionExpression &expr) {
	return expr.Function().GetName() == "struct_pack";
}

optional_ptr<Expression> FindStructPackChildByName(BoundFunctionExpression &struct_pack, const string &name) {
	D_ASSERT(struct_pack.GetReturnType().id() == LogicalTypeId::STRUCT);
	for (auto &child : struct_pack.GetChildren()) {
		if (child->GetAlias() == name) {
			return child.get();
		}
	}
	return nullptr;
}

bool UsesIndexParameter(Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		auto &ref = expr.Cast<BoundReferenceExpression>();
		if (ref.Index() == 0) {
			return true;
		}
	}
	bool uses_index = false;
	ExpressionIterator::EnumerateChildren(expr, [&uses_index](unique_ptr<Expression> &child) {
		ExpressionIterator::EnumerateExpression(child, [&uses_index](Expression &child) {
			if (uses_index) {
				return;
			}
			if (child.GetExpressionClass() == ExpressionClass::BOUND_REF) {
				auto &ref = child.Cast<BoundReferenceExpression>();
				if (ref.Index() == 0) {
					uses_index = true;
					return;
				}
			}
		});
	});
	return uses_index;
}

void RemoveIndexInputSlot(unique_ptr<Expression> &expr) {
	ExpressionIterator::VisitExpressionClassMutable(expr, ExpressionClass::BOUND_REF,
	                                                [&](unique_ptr<Expression> &child) {
		                                                auto &ref = child->Cast<BoundReferenceExpression>();
		                                                D_ASSERT(ref.Index() > 0);
		                                                ref.IndexMutable()--;
	                                                });
}

bool MatchesStructFieldProjection(Expression &expr, const string &field_name, bool allow_boolean_cast = false) {
	optional_ptr<Expression> base = expr;
	if (allow_boolean_cast && BoundCastExpression::IsCast(*base) && base->GetReturnType() == LogicalType::BOOLEAN) {
		auto &cast_expr = base->Cast<BoundFunctionExpression>();
		if (BoundCastExpression::IsTryCast(cast_expr)) {
			return false;
		}
		// The rewritten filter recreates this single BOOLEAN cast.
		base = BoundCastExpression::ChildMutable(cast_expr).get();
	}
	if (base->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	auto &extract_expr = base->Cast<BoundFunctionExpression>();
	if (extract_expr.Function().GetName() != "struct_extract" || extract_expr.GetChildren().size() != 2) {
		return false;
	}

	auto &struct_arg = extract_expr.GetChildren()[0];
	if (struct_arg->GetExpressionClass() != ExpressionClass::BOUND_REF) {
		return false;
	}
	auto &struct_ref = struct_arg->Cast<BoundReferenceExpression>();
	if (struct_ref.Index() != 0) {
		return false;
	}

	auto &field_arg = extract_expr.GetChildren()[1];
	if (field_arg->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &field_constant = field_arg->Cast<BoundConstantExpression>();
	if (field_constant.GetValue().IsNull() || field_constant.GetValue().type() != LogicalTypeId::VARCHAR) {
		return false;
	}

	return StringValue::Get(field_constant.GetValue()) == field_name;
}

struct ListComprehensionMatch {
	optional_ptr<BoundFunctionExpression> root;
	optional_ptr<BoundFunctionExpression> list_filter_expr;
	optional_ptr<BoundFunctionExpression> inner_apply;
	optional_ptr<ListLambdaBindData> inner_bind;
	optional_ptr<Expression> filter_expr;
	optional_ptr<Expression> result_expr;
	vector<unique_ptr<Expression>> captured_children;
};

vector<unique_ptr<Expression>> CopyCapturedChildren(BoundFunctionExpression &inner_apply) {
	vector<unique_ptr<Expression>> captured_children;
	captured_children.reserve(inner_apply.GetChildren().empty() ? 0 : inner_apply.GetChildren().size() - 1);
	// skip the list and the lambda placeholder - everything after them is a capture
	for (idx_t i = 1; i < inner_apply.GetChildren().size(); i++) {
		if (inner_apply.GetChildren()[i]->GetReturnType().id() == LogicalTypeId::LAMBDA) {
			continue;
		}
		captured_children.push_back(inner_apply.GetChildren()[i]->Copy());
	}
	return captured_children;
}

//! Builds the lambda child that accompanies a rewritten list function. The bind data keeps its own copy
//! of the body, this is the copy that stays in the expression tree
unique_ptr<Expression> MakeLambdaChild(const Expression &lambda_body, const bool has_index,
                                       vector<Identifier> parameter_names) {
	const idx_t parameter_count = has_index ? 2 : 1;
	auto lambda = make_uniq<BoundLambdaExpression>(ExpressionType::LAMBDA, LogicalType::LAMBDA, lambda_body.Copy(),
	                                               parameter_count);
	// carry the original names over, so that the rewritten lambda still prints like the one the user wrote
	if (parameter_names.size() > parameter_count) {
		parameter_names.resize(parameter_count);
	}
	lambda->SetParameterNames(std::move(parameter_names));
	return std::move(lambda);
}

//! Returns the parameter names of the lambda child of a bound list function, if it has one
vector<Identifier> GetLambdaParameterNames(const BoundFunctionExpression &expr) {
	for (auto &child : expr.GetChildren()) {
		if (child->GetReturnType().id() == LogicalTypeId::LAMBDA) {
			return child->Cast<BoundLambdaExpression>().ParameterNames();
		}
	}
	return vector<Identifier>();
}

unique_ptr<ListComprehensionMatch> MatchListComprehensionRewrite(ClientContext &context,
                                                                 BoundFunctionExpression &root) {
	// Check if expression has a list_transform -> list_filter -> list_transform pattern from list comprehension
	if (!IsTargetListFunction(context, root, "list_transform")) {
		return nullptr;
	}
	if (root.GetChildren().empty()) {
		return nullptr;
	}
	if (root.GetChildren()[0]->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &list_filter_expr = root.GetChildren()[0]->Cast<BoundFunctionExpression>();
	if (!IsTargetListFunction(context, list_filter_expr, "list_filter")) {
		return nullptr;
	}
	if (!list_filter_expr.BindInfo() || !root.BindInfo()) {
		return nullptr;
	}
	auto &list_filter_bind = list_filter_expr.BindInfo()->Cast<ListLambdaBindData>();
	auto &root_bind = root.BindInfo()->Cast<ListLambdaBindData>();
	if (!list_filter_bind.lambda_expr || !root_bind.lambda_expr) {
		return nullptr;
	}
	if (!MatchesStructFieldProjection(*list_filter_bind.lambda_expr, "filter", true) ||
	    !MatchesStructFieldProjection(*root_bind.lambda_expr, "result")) {
		return nullptr;
	}
	if (list_filter_expr.GetChildren().empty()) {
		return nullptr;
	}
	if (list_filter_expr.GetChildren()[0]->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &inner_apply = list_filter_expr.GetChildren()[0]->Cast<BoundFunctionExpression>();
	if (!IsTargetListFunction(context, inner_apply, "list_transform") || !inner_apply.BindInfo()) {
		return nullptr;
	}
	auto &inner_bind = inner_apply.BindInfo()->Cast<ListLambdaBindData>();
	if (!inner_bind.lambda_expr) {
		return nullptr;
	}
	auto &inner_base = inner_bind.lambda_expr;
	if (inner_base->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return nullptr;
	}
	auto &struct_pack = inner_base->Cast<BoundFunctionExpression>();
	if (!IsStructPack(struct_pack)) {
		return nullptr;
	}
	auto filter_expr = FindStructPackChildByName(struct_pack, "filter");
	auto result_expr = FindStructPackChildByName(struct_pack, "result");
	if (!filter_expr || !result_expr) {
		return nullptr;
	}
	if (inner_bind.has_index && UsesIndexParameter(*result_expr)) {
		return nullptr;
	}
	if (result_expr->IsVolatile()) {
		return nullptr;
	}

	auto match = make_uniq<ListComprehensionMatch>();
	match->root = root;
	match->list_filter_expr = list_filter_expr;
	match->inner_apply = inner_apply;
	match->inner_bind = inner_bind;
	match->filter_expr = filter_expr;
	match->result_expr = result_expr;
	match->captured_children = CopyCapturedChildren(inner_apply);
	return match;
}

unique_ptr<Expression> BuildListComprehensionRewrite(ClientContext &context, ListComprehensionMatch &match) {
	auto &inner_apply = *match.inner_apply;
	auto &list_filter_expr = *match.list_filter_expr;
	auto &root = *match.root;
	auto &inner_bind = *match.inner_bind;
	auto &filter_expr = *match.filter_expr;
	auto &result_expr = *match.result_expr;

	auto lambda_parameter_names = GetLambdaParameterNames(inner_apply);

	// Build list_filter(list, lambda filter_expr)
	vector<unique_ptr<Expression>> filter_children;
	filter_children.reserve(inner_apply.GetChildren().size());
	for (idx_t i = 0; i < inner_apply.GetChildren().size(); i++) {
		filter_children.push_back(std::move(inner_apply.GetChildrenMutable()[i]));
	}

	auto filter_return_type = filter_children[0]->GetReturnType();
	auto filter_lambda = filter_expr.Copy();
	if (filter_lambda->GetReturnType() != LogicalType::BOOLEAN) {
		filter_lambda = BoundCastExpression::AddCastToType(context, std::move(filter_lambda), LogicalType::BOOLEAN);
	}
	// the moved-over children still carry inner_apply's lambda - replace it with the filter's own lambda
	for (auto &filter_child : filter_children) {
		if (filter_child->GetReturnType().id() == LogicalTypeId::LAMBDA) {
			filter_child = MakeLambdaChild(*filter_lambda, inner_bind.has_index, lambda_parameter_names);
			break;
		}
	}
	auto filter_bind_info =
	    make_uniq<ListLambdaBindData>(filter_return_type, std::move(filter_lambda), inner_bind.has_index);

	auto new_func = list_filter_expr.Function();
	new_func.SetReturnType(filter_return_type);

	auto new_filter = make_uniq<BoundFunctionExpression>(std::move(new_func), std::move(filter_children),
	                                                     std::move(filter_bind_info), list_filter_expr.IsOperator());

	// Build list_apply(list_filter(...), lambda result_expr)
	auto apply_return_type = LogicalType::LIST(result_expr.GetReturnType());
	auto apply_lambda = result_expr.Copy();
	if (inner_bind.has_index) {
		// The apply function included an index but it was not used. Adapt references to exclude index
		RemoveIndexInputSlot(apply_lambda);
	}

	vector<unique_ptr<Expression>> apply_children;
	apply_children.reserve(inner_apply.GetChildren().size());
	apply_children.push_back(std::move(new_filter));
	// the lambda keeps its argument position, the captures follow it
	apply_children.push_back(MakeLambdaChild(*apply_lambda, false, std::move(lambda_parameter_names)));
	for (auto &captured_child : match.captured_children) {
		apply_children.push_back(std::move(captured_child));
	}

	auto apply_bind_info = make_uniq<ListLambdaBindData>(apply_return_type, std::move(apply_lambda));
	return make_uniq<BoundFunctionExpression>(root.Function(), std::move(apply_children), std::move(apply_bind_info),
	                                          root.IsOperator());
}

} // namespace

ListComprehensionRewriteRule::ListComprehensionRewriteRule(ExpressionRewriter &rewriter) : Rule(rewriter) {
	root = make_uniq<ExpressionMatcher>(ExpressionClass::BOUND_FUNCTION);
}

unique_ptr<Expression> ListComprehensionRewriteRule::Apply(LogicalOperator &, vector<reference<Expression>> &bindings,
                                                           bool &, bool) {
	auto &root = bindings[0].get().Cast<BoundFunctionExpression>();
	auto match = MatchListComprehensionRewrite(GetContext(), root);
	if (!match) {
		return nullptr;
	}
	return BuildListComprehensionRewrite(GetContext(), *match);
}

} // namespace duckdb
