#include "duckdb/common/exception.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/transformer.hpp"

namespace duckdb {

unique_ptr<ParsedExpression> Transformer::TransformArrayAccess(duckdb_libpgquery::PGAIndirection *indirection_node,
                                                               idx_t depth) {
	// transform the source expression
	unique_ptr<ParsedExpression> result;
	result = TransformExpression(indirection_node->arg, depth + 1);

	// now go over the indices
	// note that a single indirection node can contain multiple indices
	// this happens for e.g. more complex accesses (e.g. (foo).field1[42])
	for (auto node = indirection_node->indirection->head; node != nullptr; node = node->next) {
		auto target = reinterpret_cast<duckdb_libpgquery::PGNode *>(node->data.ptr_value);
		D_ASSERT(target);

		switch (target->type) {
		case duckdb_libpgquery::T_PGAIndices: {
			// index access (either slice or extract)
			auto index = (duckdb_libpgquery::PGAIndices *)target;
			vector<unique_ptr<ParsedExpression>> children;
			children.push_back(move(result));
			if (index->is_slice) {
				// slice
				children.push_back(!index->lidx ? make_unique<ConstantExpression>(Value())
				                                : TransformExpression(index->lidx, depth + 1));
				children.push_back(!index->uidx ? make_unique<ConstantExpression>(Value())
				                                : TransformExpression(index->uidx, depth + 1));
				result = make_unique<OperatorExpression>(ExpressionType::ARRAY_SLICE, move(children));
			} else {
				// array access
				D_ASSERT(!index->lidx);
				D_ASSERT(index->uidx);
				children.push_back(TransformExpression(index->uidx, depth + 1));
				result = make_unique<OperatorExpression>(ExpressionType::ARRAY_EXTRACT, move(children));
			}
			break;
		}
		case duckdb_libpgquery::T_PGString: {
			auto val = (duckdb_libpgquery::PGValue *)target;
			vector<unique_ptr<ParsedExpression>> children;
			children.push_back(move(result));
			children.push_back(TransformValue(*val, depth + 1));
			result = make_unique<OperatorExpression>(ExpressionType::STRUCT_EXTRACT, move(children));
			break;
		}
		default:
			throw NotImplementedException("Unimplemented subscript type");
		}
	}
	return result;
}

} // namespace duckdb
