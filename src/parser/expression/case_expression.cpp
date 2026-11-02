#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/serializer.hpp"

namespace duckdb {

CaseExpression::CaseExpression() : ParsedExpression(ExpressionType::CASE_EXPR, ExpressionClass::CASE) {
}

string CaseExpression::ToString() const {
	if (!case_operand) {
		return ToString<CaseExpression, ParsedExpression>(*this);
	}
	string case_str = "CASE (" + case_operand->ToString() + ")";
	for (auto &check : case_checks) {
		case_str += " WHEN (" + check.when_expr->ToString() + ")";
		case_str += " THEN (" + check.then_expr->ToString() + ")";
	}
	case_str += " ELSE " + else_expr->ToString();
	case_str += " END";
	return case_str;
}

const vector<CaseCheck> &CaseExpression::GetCaseChecksForSerialization(Serializer &serializer) const {
	if (case_operand && !serializer.ShouldSerialize(StorageVersion::V2_1_0)) {
		throw SerializationException("Simple CASE expressions require storage version v2.1.0 or higher and cannot be "
		                             "serialized to older storage formats");
	}
	return case_checks;
}

} // namespace duckdb
