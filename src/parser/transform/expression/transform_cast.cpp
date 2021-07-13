#include "duckdb/common/limits.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/transformer.hpp"
#include "duckdb/common/operator/cast_operators.hpp"

namespace duckdb {

unique_ptr<ParsedExpression> Transformer::TransformTypeCast(duckdb_libpgquery::PGTypeCast *root, idx_t depth) {
	D_ASSERT(root);

	// get the type to cast to
	auto type_name = root->typeName;
	LogicalType target_type = TransformTypeName(type_name);

	// check for a constant BLOB value, then return ConstantExpression with BLOB
	if (target_type == LogicalType::BLOB && root->arg->type == duckdb_libpgquery::T_PGAConst) {
		auto c = reinterpret_cast<duckdb_libpgquery::PGAConst *>(root->arg);
		if (c->val.type == duckdb_libpgquery::T_PGString) {
			return make_unique<ConstantExpression>(Value::BLOB(string(c->val.val.str)));
		}
	}
	// transform the expression node
	auto expression = TransformExpression(root->arg, depth + 1);
	bool try_cast = root->tryCast;

	// now create a cast operation
	return make_unique<CastExpression>(target_type, move(expression), try_cast);
}

} // namespace duckdb
