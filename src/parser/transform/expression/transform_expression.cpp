#include "duckdb/common/exception.hpp"
#include "duckdb/parser/expression/default_expression.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

unique_ptr<ParsedExpression> Transformer::TransformResTarget(duckdb_libpgquery::PGResTarget *root, idx_t depth) {
	D_ASSERT(root);

	auto expr = TransformExpression(root->val, depth + 1);
	if (!expr) {
		return nullptr;
	}
	if (root->name) {
		expr->alias = string(root->name);
	}
	return expr;
}

unique_ptr<ParsedExpression> Transformer::TransformNamedArg(duckdb_libpgquery::PGNamedArgExpr *root, idx_t depth) {
	D_ASSERT(root);

	auto expr = TransformExpression((duckdb_libpgquery::PGNode *)root->arg, depth + 1);
	if (root->name) {
		expr->alias = string(root->name);
	}
	return expr;
}

unique_ptr<ParsedExpression> Transformer::TransformExpression(duckdb_libpgquery::PGNode *node, idx_t depth) {
	if (!node) {
		return nullptr;
	}

	if (depth > max_expression_depth) {
		throw ParserException("Expression tree is too deep (maximum depth %d)", max_expression_depth);
	}

	switch (node->type) {
	case duckdb_libpgquery::T_PGColumnRef:
		return TransformColumnRef(reinterpret_cast<duckdb_libpgquery::PGColumnRef *>(node), depth);
	case duckdb_libpgquery::T_PGAConst:
		return TransformConstant(reinterpret_cast<duckdb_libpgquery::PGAConst *>(node), depth);
	case duckdb_libpgquery::T_PGAExpr:
		return TransformAExpr(reinterpret_cast<duckdb_libpgquery::PGAExpr *>(node), depth);
	case duckdb_libpgquery::T_PGFuncCall:
		return TransformFuncCall(reinterpret_cast<duckdb_libpgquery::PGFuncCall *>(node), depth);
	case duckdb_libpgquery::T_PGBoolExpr:
		return TransformBoolExpr(reinterpret_cast<duckdb_libpgquery::PGBoolExpr *>(node), depth);
	case duckdb_libpgquery::T_PGTypeCast:
		return TransformTypeCast(reinterpret_cast<duckdb_libpgquery::PGTypeCast *>(node), depth);
	case duckdb_libpgquery::T_PGCaseExpr:
		return TransformCase(reinterpret_cast<duckdb_libpgquery::PGCaseExpr *>(node), depth);
	case duckdb_libpgquery::T_PGSubLink:
		return TransformSubquery(reinterpret_cast<duckdb_libpgquery::PGSubLink *>(node), depth);
	case duckdb_libpgquery::T_PGCoalesceExpr:
		return TransformCoalesce(reinterpret_cast<duckdb_libpgquery::PGAExpr *>(node), depth);
	case duckdb_libpgquery::T_PGNullTest:
		return TransformNullTest(reinterpret_cast<duckdb_libpgquery::PGNullTest *>(node), depth);
	case duckdb_libpgquery::T_PGResTarget:
		return TransformResTarget(reinterpret_cast<duckdb_libpgquery::PGResTarget *>(node), depth);
	case duckdb_libpgquery::T_PGParamRef:
		return TransformParamRef(reinterpret_cast<duckdb_libpgquery::PGParamRef *>(node), depth);
	case duckdb_libpgquery::T_PGNamedArgExpr:
		return TransformNamedArg(reinterpret_cast<duckdb_libpgquery::PGNamedArgExpr *>(node), depth);
	case duckdb_libpgquery::T_PGSQLValueFunction:
		return TransformSQLValueFunction(reinterpret_cast<duckdb_libpgquery::PGSQLValueFunction *>(node), depth);
	case duckdb_libpgquery::T_PGSetToDefault:
		return make_unique<DefaultExpression>();
	case duckdb_libpgquery::T_PGCollateClause:
		return TransformCollateExpr(reinterpret_cast<duckdb_libpgquery::PGCollateClause *>(node), depth);
	case duckdb_libpgquery::T_PGIntervalConstant:
		return TransformInterval(reinterpret_cast<duckdb_libpgquery::PGIntervalConstant *>(node), depth);
	case duckdb_libpgquery::T_PGLambdaFunction:
		return TransformLambda(reinterpret_cast<duckdb_libpgquery::PGLambdaFunction *>(node), depth);
	case duckdb_libpgquery::T_PGAIndirection:
		return TransformArrayAccess(reinterpret_cast<duckdb_libpgquery::PGAIndirection *>(node), depth);
	case duckdb_libpgquery::T_PGPositionalReference:
		return TransformPositionalReference(reinterpret_cast<duckdb_libpgquery::PGPositionalReference *>(node), depth);
	default:
		throw NotImplementedException("Expr of type %d not implemented\n", (int)node->type);
	}
}

void Transformer::TransformExpressionList(duckdb_libpgquery::PGList &list, vector<unique_ptr<ParsedExpression>> &result,
                                          idx_t depth) {
	for (auto node = list.head; node != nullptr; node = node->next) {
		auto target = reinterpret_cast<duckdb_libpgquery::PGNode *>(node->data.ptr_value);
		D_ASSERT(target);

		auto expr = TransformExpression(target, depth + 1);
		D_ASSERT(expr);

		result.push_back(move(expr));
	}
}

} // namespace duckdb
