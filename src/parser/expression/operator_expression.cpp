#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

OperatorExpression::OperatorExpression() : ParsedExpression(ExpressionType::INVALID, ExpressionClass::OPERATOR) {
}

OperatorExpression::OperatorExpression(ExpressionType type, unique_ptr<ParsedExpression> left,
                                       unique_ptr<ParsedExpression> right)
    : ParsedExpression(type, ExpressionClass::OPERATOR) {
	if (left) {
		children.push_back(std::move(left));
	}
	if (right) {
		children.push_back(std::move(right));
	}
}

OperatorExpression::OperatorExpression(ExpressionType type, vector<unique_ptr<ParsedExpression>> children)
    : ParsedExpression(type, ExpressionClass::OPERATOR), children(std::move(children)) {
}

string OperatorExpression::ToString() const {
	return ToString<OperatorExpression, ParsedExpression>(*this);
}

unique_ptr<ParsedExpression> OperatorExpression::EmptySliceBound() {
	return make_uniq<FunctionExpression>("list_value", vector<unique_ptr<ParsedExpression>>());
}

namespace {

//! Storage before v2.0 encodes an omitted slice bound as the constant INTEGER[] []
Value LegacyEmptySliceBoundValue() {
	return Value::LIST(LogicalType::INTEGER, vector<Value>());
}

//! Serializes as the pre-v2.0 ConstantExpression holding the legacy empty slice bound
struct LegacyEmptySliceBound {
	void Serialize(Serializer &serializer) const {
		serializer.WriteProperty<ExpressionClass>(100, "class", ExpressionClass::CONSTANT);
		serializer.WriteProperty<ExpressionType>(101, "type", ExpressionType::VALUE_CONSTANT);
		serializer.WriteProperty<Value>(200, "value", LegacyEmptySliceBoundValue());
	}
};

bool IsSliceBound(const OperatorExpression &expr, idx_t child_idx) {
	return expr.GetExpressionType() == ExpressionType::ARRAY_SLICE && (child_idx == 1 || child_idx == 2);
}

bool IsEmptySliceBoundExpression(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::FUNCTION) {
		return false;
	}
	auto &function = expr.Cast<FunctionExpression>();
	return function.FunctionName() == "list_value" && function.GetArguments().empty();
}

} // namespace

void OperatorExpression::Serialize(Serializer &serializer) const {
	ParsedExpression::Serialize(serializer);
	if (serializer.ShouldSerialize(StorageVersion::V2_0_0) || GetExpressionType() != ExpressionType::ARRAY_SLICE) {
		serializer.WritePropertyWithDefault<vector<unique_ptr<ParsedExpression>>>(200, "children", children);
		return;
	}
	LegacyEmptySliceBound legacy_bound;
	serializer.WriteList(200, "children", children.size(), [&](Serializer::List &list, idx_t i) {
		if (IsSliceBound(*this, i) && IsEmptySliceBoundExpression(*children[i])) {
			list.WriteElement(optional_ptr<const LegacyEmptySliceBound>(legacy_bound));
		} else {
			list.WriteElement(children[i]);
		}
	});
}

unique_ptr<ParsedExpression> OperatorExpression::Deserialize(Deserializer &deserializer) {
	auto result = duckdb::unique_ptr<OperatorExpression>(new OperatorExpression(deserializer.Get<ExpressionType>()));
	deserializer.ReadPropertyWithDefault<vector<unique_ptr<ParsedExpression>>>(200, "children", result->children);
	if (result->GetExpressionType() == ExpressionType::ARRAY_SLICE) {
		auto legacy_bound = ConstantExpression::FromValue(LegacyEmptySliceBoundValue());
		for (idx_t i = 0; i < result->children.size(); i++) {
			if (IsSliceBound(*result, i) && result->children[i]->Equals(*legacy_bound)) {
				result->children[i] = EmptySliceBound();
			}
		}
	}
	return std::move(result);
}

} // namespace duckdb
