#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

unique_ptr<ParsedExpression> Transformer::TransformCase(duckdb_libpgquery::PGCaseExpr *root, idx_t depth) {
	D_ASSERT(root);

	auto case_node = make_unique<CaseExpression>();
	for (auto cell = root->args->head; cell != nullptr; cell = cell->next) {
		CaseCheck case_check;

		auto w = reinterpret_cast<duckdb_libpgquery::PGCaseWhen *>(cell->data.ptr_value);
		auto test_raw = TransformExpression(reinterpret_cast<duckdb_libpgquery::PGNode *>(w->expr), depth + 1);
		unique_ptr<ParsedExpression> test;
		auto arg = TransformExpression(reinterpret_cast<duckdb_libpgquery::PGNode *>(root->arg), depth + 1);
		if (arg) {
			case_check.when_expr =
			    make_unique<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, move(arg), move(test_raw));
		} else {
			case_check.when_expr = move(test_raw);
		}
		case_check.then_expr = TransformExpression(reinterpret_cast<duckdb_libpgquery::PGNode *>(w->result), depth + 1);
		case_node->case_checks.push_back(move(case_check));
	}

	if (root->defresult) {
		case_node->else_expr =
		    TransformExpression(reinterpret_cast<duckdb_libpgquery::PGNode *>(root->defresult), depth + 1);
	} else {
		case_node->else_expr = make_unique<ConstantExpression>(Value(LogicalType::SQLNULL));
	}
	return move(case_node);
}

} // namespace duckdb
