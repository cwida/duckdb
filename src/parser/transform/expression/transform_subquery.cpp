#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

unique_ptr<ParsedExpression> Transformer::TransformSubquery(duckdb_libpgquery::PGSubLink *root, idx_t depth) {
	D_ASSERT(root);
	auto subquery_expr = make_unique<SubqueryExpression>();

	subquery_expr->subquery = TransformSelect(root->subselect);
	D_ASSERT(subquery_expr->subquery);
	D_ASSERT(subquery_expr->subquery->node->GetSelectList().size() > 0);

	switch (root->subLinkType) {
	case duckdb_libpgquery::PG_EXISTS_SUBLINK: {
		subquery_expr->subquery_type = SubqueryType::EXISTS;
		break;
	}
	case duckdb_libpgquery::PG_ANY_SUBLINK:
	case duckdb_libpgquery::PG_ALL_SUBLINK: {
		// comparison with ANY() or ALL()
		subquery_expr->subquery_type = SubqueryType::ANY;
		subquery_expr->child = TransformExpression(root->testexpr, depth + 1);
		// get the operator name
		if (!root->operName) {
			// simple IN
			subquery_expr->comparison_type = ExpressionType::COMPARE_EQUAL;
		} else {
			auto operator_name =
			    string((reinterpret_cast<duckdb_libpgquery::PGValue *>(root->operName->head->data.ptr_value))->val.str);
			subquery_expr->comparison_type = OperatorToExpressionType(operator_name);
		}
		D_ASSERT(subquery_expr->comparison_type == ExpressionType::COMPARE_EQUAL ||
		         subquery_expr->comparison_type == ExpressionType::COMPARE_NOTEQUAL ||
		         subquery_expr->comparison_type == ExpressionType::COMPARE_GREATERTHAN ||
		         subquery_expr->comparison_type == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
		         subquery_expr->comparison_type == ExpressionType::COMPARE_LESSTHAN ||
		         subquery_expr->comparison_type == ExpressionType::COMPARE_LESSTHANOREQUALTO);
		if (root->subLinkType == duckdb_libpgquery::PG_ALL_SUBLINK) {
			// ALL sublink is equivalent to NOT(ANY) with inverted comparison
			// e.g. [= ALL()] is equivalent to [NOT(<> ANY())]
			// first invert the comparison type
			subquery_expr->comparison_type = NegateComparisionExpression(subquery_expr->comparison_type);
			return make_unique<OperatorExpression>(ExpressionType::OPERATOR_NOT, move(subquery_expr));
		}
		break;
	}
	case duckdb_libpgquery::PG_EXPR_SUBLINK: {
		// return a single scalar value from the subquery
		// no child expression to compare to
		subquery_expr->subquery_type = SubqueryType::SCALAR;
		break;
	}
	default:
		throw NotImplementedException("Subquery of type %d not implemented\n", (int)root->subLinkType);
	}
	return move(subquery_expr);
}

} // namespace duckdb
