#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

// COALESCE(a,b,c) returns the first argument that is NOT NULL, so
// rewrite into CASE(a IS NOT NULL, a, CASE(b IS NOT NULL, b, c))
unique_ptr<ParsedExpression> Transformer::TransformCoalesce(duckdb_libpgquery::PGAExpr *root, idx_t depth) {
	D_ASSERT(root);

	auto coalesce_args = reinterpret_cast<duckdb_libpgquery::PGList *>(root->lexpr);
	D_ASSERT(coalesce_args->length > 0); // parser ensures this already

	auto coalesce_op = make_unique<OperatorExpression>(ExpressionType::OPERATOR_COALESCE);
	for (auto cell = coalesce_args->head; cell; cell = cell->next) {
		// get the value of the COALESCE
		auto value_expr =
		    TransformExpression(reinterpret_cast<duckdb_libpgquery::PGNode *>(cell->data.ptr_value), depth + 1);
		coalesce_op->children.push_back(move(value_expr));
	}
	return move(coalesce_op);
}

} // namespace duckdb
