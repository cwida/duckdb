#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer.hpp"
#include "duckdb/parser/expression_util.hpp"

namespace duckdb {

ConjunctionExpression::ConjunctionExpression(ExpressionType type)
    : ParsedExpression(type, ExpressionClass::CONJUNCTION) {
}

ConjunctionExpression::ConjunctionExpression(ExpressionType type, vector<unique_ptr<ParsedExpression>> children)
    : ParsedExpression(type, ExpressionClass::CONJUNCTION) {
	for (auto &child : children) {
		AddExpression(move(child));
	}
}

ConjunctionExpression::ConjunctionExpression(ExpressionType type, unique_ptr<ParsedExpression> left,
                                             unique_ptr<ParsedExpression> right)
    : ParsedExpression(type, ExpressionClass::CONJUNCTION) {
	AddExpression(move(left));
	AddExpression(move(right));
}

void ConjunctionExpression::AddExpression(unique_ptr<ParsedExpression> expr) {
	if (expr->type == type) {
		// expr is a conjunction of the same type: merge the expression lists together
		auto &other = (ConjunctionExpression &)*expr;
		for (auto &child : other.children) {
			children.push_back(move(child));
		}
	} else {
		children.push_back(move(expr));
	}
}

string ConjunctionExpression::ToString() const {
	string result = children[0]->ToString();
	for (idx_t i = 1; i < children.size(); i++) {
		result += " " + ExpressionTypeToOperator(type) + " " + children[i]->ToString();
	}
	return result;
}

bool ConjunctionExpression::Equals(const ConjunctionExpression *a, const ConjunctionExpression *b) {
	return ExpressionUtil::SetEquals(a->children, b->children);
}

unique_ptr<ParsedExpression> ConjunctionExpression::Copy() const {
	vector<unique_ptr<ParsedExpression>> copy_children;
	for (auto &expr : children) {
		copy_children.push_back(expr->Copy());
	}
	auto copy = make_unique<ConjunctionExpression>(type, move(copy_children));
	copy->CopyProperties(*this);
	return move(copy);
}

void ConjunctionExpression::Serialize(Serializer &serializer) {
	ParsedExpression::Serialize(serializer);
	serializer.WriteList<ParsedExpression>(children);
}

unique_ptr<ParsedExpression> ConjunctionExpression::Deserialize(ExpressionType type, Deserializer &source) {
	auto result = make_unique<ConjunctionExpression>(type);
	source.ReadList<ParsedExpression>(result->children);
	return move(result);
}

} // namespace duckdb
