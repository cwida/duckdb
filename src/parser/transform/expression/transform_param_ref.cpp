#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/parser/transformer.hpp"
#include "duckdb/common/algorithm.hpp"

namespace duckdb {

unique_ptr<ParsedExpression> Transformer::TransformParamRef(duckdb_libpgquery::PGParamRef *node, idx_t depth) {
	D_ASSERT(node);
	auto expr = make_unique<ParameterExpression>();
	if (node->number == 0) {
		expr->parameter_nr = ParamCount() + 1;
	} else {
		expr->parameter_nr = node->number;
	}
	SetParamCount(MaxValue<idx_t>(ParamCount(), expr->parameter_nr));
	return move(expr);
}

} // namespace duckdb
