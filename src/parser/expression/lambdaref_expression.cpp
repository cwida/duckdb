#include "duckdb/parser/expression/lambdaref_expression.hpp"

#include "duckdb/common/types/hash.hpp"
#include "duckdb/planner/table_binding.hpp"

namespace duckdb {

LambdaRefExpression::LambdaRefExpression(idx_t lambda_idx, Identifier column_name_p)
    : ParsedExpression(ExpressionType::LAMBDA_REF, ExpressionClass::LAMBDA_REF), lambda_idx(lambda_idx),
      column_name(std::move(column_name_p)) {
	alias = column_name;
}

bool LambdaRefExpression::IsScalar() const {
	throw InternalException("lambda reference expressions are transient, IsScalar should never be called");
}

Identifier LambdaRefExpression::GetName() const {
	return column_name;
}

string LambdaRefExpression::ToString() const {
	throw InternalException("lambda reference expressions are transient, ToString should never be called");
}

unique_ptr<ParsedExpression>
LambdaRefExpression::FindMatchingBinding(optional_ptr<vector<DummyBinding>> &lambda_bindings,
                                         const Identifier &column_name) {
	// if this is a lambda parameter, then we temporarily add a BoundLambdaRef,
	// which we capture and remove later

	// inner lambda parameters have precedence over outer lambda parameters, and
	// lambda parameters have precedence over macros and columns

	if (lambda_bindings) {
		for (idx_t i = lambda_bindings->size(); i > 0; i--) {
			if ((*lambda_bindings)[i - 1].internal_parameters) {
				continue;
			}
			if ((*lambda_bindings)[i - 1].HasMatchingBinding(column_name)) {
				D_ASSERT((*lambda_bindings)[i - 1].GetBindingAlias().IsSet());
				return make_uniq<LambdaRefExpression>(i - 1, column_name);
			}
		}
	}

	return nullptr;
}

} // namespace duckdb
