#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/expression/lambdaref_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/expression_binder/case_operand_binding.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/table_binding.hpp"

namespace duckdb {

CaseOperandBinding::CaseOperandBinding(ExpressionBinder &binder_p)
    : binder(binder_p), previous(binder_p.case_operand_binding) {
	binder.case_operand_binding = this;
}

CaseOperandBinding::~CaseOperandBinding() {
	binder.case_operand_binding = previous;
}

void CaseOperandBinding::Remember(const BoundSubqueryExpression &expr, const SubqueryExpression &source,
                                  bool direct_try) {
	subqueries[expr] = {source.Copy(), direct_try};
	if (previous) {
		previous->Remember(expr, source, direct_try);
	}
}

unique_ptr<Expression> CaseOperandBinding::CopySubquery(BoundSubqueryExpression &expr) {
	auto entry = subqueries.find(expr);
	if (entry == subqueries.end()) {
		throw InternalException("Missing CASE operand subquery source");
	}
	auto original = entry->second.expression->Copy();
	auto &source = original->Cast<SubqueryExpression>();
	const bool direct_try = entry->second.direct_try;
	const bool rethrow_try_errors = expr.RethrowsTryErrors();
	auto query = source.Subquery()->node->Copy();
	auto result = make_uniq<BoundSubqueryExpression>(expr.GetReturnType());
	result->SetAlias(expr.GetAlias());
	result->SetQueryLocation(expr.GetQueryLocation());
	result->SubqueryMutable() =
	    binder.BindSubqueryNode(*query, result->GetBinderMutable(), direct_try || rethrow_try_errors);
	if ((direct_try || rethrow_try_errors) && result->Subquery().plan) {
		auto &indexes = result->GetBinder()->GetTrySubqueryPlanIndexes();
		auto struct_type =
		    binder.LowerTrySubquery(source, indexes, *result->SubqueryMutable().plan, rethrow_try_errors, false);
		if (rethrow_try_errors) {
			D_ASSERT(struct_type);
			result->SubqueryMutable().types[0] = *struct_type;
			result->SetRethrowTryErrors();
		}
	}
	result->SubqueryTypeMutable() = expr.GetSubqueryType();
	result->ComparisonTypeMutable() = expr.ComparisonType();
	result->ChildTypesMutable() = expr.GetChildTypes();
	result->ChildTargetsMutable() = expr.GetChildTargets();
	for (auto &child : expr.GetChildrenMutable()) {
		result->GetChildrenMutable().push_back(Copy(child));
	}
	Remember(*result, source, direct_try);
	return std::move(result);
}

namespace {

//! Substitute copyable references temporarily; restore the source even if copying throws.
class CaseSubqueryReferences {
public:
	~CaseSubqueryReferences() {
		Restore();
	}

	void Extract(unique_ptr<Expression> &expr) {
		if (expr->GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
			auto placeholder = make_uniq<BoundReferenceExpression>(expr->GetAlias(), expr->GetReturnType(),
			                                                       DConstants::INVALID_INDEX - originals.size());
			originals.emplace_back(expr, std::move(expr));
			expr = std::move(placeholder);
			return;
		}
		ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) { Extract(child); });
	}

	void Restore() {
		for (auto &original : originals) {
			if (original.second) {
				original.first.get() = std::move(original.second);
			}
		}
	}

	vector<pair<reference<unique_ptr<Expression>>, unique_ptr<Expression>>> originals;
};

} // namespace

unique_ptr<Expression> CaseOperandBinding::Copy(unique_ptr<Expression> &expr) {
	if (!expr->HasSubquery()) {
		return expr->Copy();
	}
	CaseSubqueryReferences references;
	references.Extract(expr);
	auto result = expr->Copy();
	references.Restore();
	std::function<void(unique_ptr<Expression> &)> restore_subqueries = [&](unique_ptr<Expression> &child) {
		if (child->GetExpressionClass() == ExpressionClass::BOUND_REF) {
			auto index = DConstants::INVALID_INDEX - child->Cast<BoundReferenceExpression>().Index();
			if (index < references.originals.size()) {
				auto &original = references.originals[index].first.get()->Cast<BoundSubqueryExpression>();
				child = CopySubquery(original);
				return;
			}
		}
		ExpressionIterator::EnumerateChildren(*child, restore_subqueries);
	};
	restore_subqueries(result);
	return result;
}

static unique_ptr<ParsedExpression> CreateCaseInvokeExpression(const CaseExpression &expr, idx_t lambda_index,
                                                               Identifier parameter_name,
                                                               QueryLocation operand_location,
                                                               unique_ptr<ParsedExpression> case_operand) {
	auto case_body = make_uniq<CaseExpression>();
	for (auto &check : expr.CaseChecks()) {
		CaseCheck invoke_check;
		auto lambda_ref = make_uniq<LambdaRefExpression>(lambda_index, parameter_name);
		lambda_ref->SetQueryLocation(operand_location);
		invoke_check.when_expr = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(lambda_ref),
		                                                         check.when_expr->Copy());
		invoke_check.when_expr->SetQueryLocation(operand_location);
		invoke_check.then_expr = check.then_expr->Copy();
		case_body->CaseChecksMutable().push_back(std::move(invoke_check));
	}
	case_body->ElseMutable() = expr.Else().Copy();

	vector<string> parameters;
	parameters.push_back(parameter_name.GetIdentifierName());
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(make_uniq<LambdaExpression>(std::move(parameters), std::move(case_body)));
	arguments.push_back(std::move(case_operand));
	auto invoke_name = QualifiedName(Identifier(SYSTEM_CATALOG), Identifier(DEFAULT_SCHEMA), Identifier("invoke"));
	auto result = make_uniq<FunctionExpression>(invoke_name, std::move(arguments));
	result->SetAlias(expr.GetAlias());
	result->SetQueryLocation(expr.GetQueryLocation());
	return std::move(result);
}

BindResult ExpressionBinder::BindExpression(CaseExpression &expr, idx_t depth) {
	ErrorData error;
	vector<pair<unique_ptr<Expression>, unique_ptr<Expression>>> checks;
	unique_ptr<Expression> else_expr;
	if (expr.CaseOperand()) {
		CaseOperandBinding operand_binding(*this);
		auto parameter_name = expr.CaseOperand()->GetName();
		auto operand_location = expr.CaseOperand()->GetQueryLocation();
		auto lambda_index = lambda_bindings ? lambda_bindings->size() : 0;
		auto case_operand_expr = expr.CaseOperand()->Copy();
		auto case_operand = BindChild(case_operand_expr, depth, error);
		if (error.HasError()) {
			return BindResult(std::move(error));
		}

		if (expr.CaseChecks().size() == 1 || !case_operand->IsVolatile()) {
			vector<unique_ptr<Expression>> operands;
			operands.push_back(std::move(case_operand));
			for (idx_t i = 1; i < expr.CaseChecks().size(); i++) {
				operands.push_back(operand_binding.Copy(operands[0]));
			}
			for (idx_t i = 0; i < expr.CaseChecks().size(); i++) {
				auto &check = expr.CaseChecksMutable()[i];
				auto when_expr = BindChild(check.when_expr, depth, error);
				auto then_expr = BindChild(check.then_expr, depth, error);
				unique_ptr<Expression> comparison;
				if (when_expr) {
					comparison = CreateBoundComparison(ExpressionType::COMPARE_EQUAL, std::move(operands[i]),
					                                   std::move(when_expr), error);
				}
				checks.emplace_back(std::move(comparison), std::move(then_expr));
			}
			else_expr = BindChild(expr.ElseMutable(), depth, error);
		} else {
			// SQL export must distinguish the generated parameter from captured outer parameters.
			if (lambda_bindings) {
				auto name_in_use = [&](const Identifier &name) {
					for (auto &binding : *lambda_bindings) {
						if (binding.HasMatchingBinding(name)) {
							return true;
						}
					}
					return false;
				};
				auto original_name = parameter_name.GetIdentifierName();
				idx_t suffix = 1;
				while (name_in_use(parameter_name)) {
					parameter_name = Identifier(original_name + "_" + std::to_string(suffix++));
				}
			}
			auto invoke_expr = CreateCaseInvokeExpression(expr, lambda_index, std::move(parameter_name),
			                                              operand_location, std::move(expr.CaseOperandMutable()));
			auto stack_checker = StackCheck(*invoke_expr);
			auto &invoke_function = invoke_expr->Cast<FunctionExpression>();
			auto &invoke_entry = BindFunction(invoke_function).Cast<ScalarFunctionCatalogEntry>();
			vector<unique_ptr<Expression>> bound_children(invoke_function.GetArguments().size());
			bound_children[1] = std::move(case_operand);
			auto result = BindLambdaFunction(invoke_function, invoke_entry, depth, std::move(bound_children), true);
			return result;
		}
	} else {
		// first try to bind the children of the case expression
		for (auto &check : expr.CaseChecksMutable()) {
			auto when_expr = BindChild(check.when_expr, depth, error);
			auto then_expr = BindChild(check.then_expr, depth, error);
			checks.emplace_back(std::move(when_expr), std::move(then_expr));
		}
		else_expr = BindChild(expr.ElseMutable(), depth, error);
	}
	if (error.HasError()) {
		return BindResult(std::move(error));
	}
	// the children have been successfully resolved
	// figure out the result type of the CASE expression
	auto return_type = ExpressionBinder::GetExpressionReturnType(*else_expr);
	for (auto &check : checks) {
		auto then_type = ExpressionBinder::GetExpressionReturnType(*check.second);
		if (!LogicalType::TryGetMaxLogicalType(context, return_type, then_type, return_type)) {
			throw BinderException(
			    expr, "Cannot mix values of type %s and %s in CASE expression - an explicit cast is required",
			    return_type.ToString(), then_type.ToString());
		}
	}

	// bind all the individual components of the CASE statement
	auto result = make_uniq<BoundCaseExpression>(return_type);
	for (auto &check : checks) {
		BoundCaseCheck result_check;
		result_check.when_expr =
		    BoundCastExpression::AddCastToType(context, std::move(check.first), LogicalType::BOOLEAN);
		result_check.then_expr = BoundCastExpression::AddCastToType(context, std::move(check.second), return_type);
		result->CaseChecksMutable().push_back(std::move(result_check));
	}
	result->ElseMutable() = BoundCastExpression::AddCastToType(context, std::move(else_expr), return_type);
	return BindResult(std::move(result));
}
} // namespace duckdb
