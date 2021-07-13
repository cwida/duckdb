#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/common/serializer.hpp"
#include "duckdb/parser/expression_util.hpp"

namespace duckdb {

bool ResultModifier::Equals(const ResultModifier *other) const {
	if (!other) {
		return false;
	}
	return type == other->type;
}

void ResultModifier::Serialize(Serializer &serializer) {
	serializer.Write<ResultModifierType>(type);
}

unique_ptr<ResultModifier> ResultModifier::Deserialize(Deserializer &source) {
	auto type = source.Read<ResultModifierType>();
	switch (type) {
	case ResultModifierType::LIMIT_MODIFIER:
		return LimitModifier::Deserialize(source);
	case ResultModifierType::ORDER_MODIFIER:
		return OrderModifier::Deserialize(source);
	case ResultModifierType::DISTINCT_MODIFIER:
		return DistinctModifier::Deserialize(source);
	default:
		throw InternalException("Unrecognized ResultModifierType for Deserialization");
	}
}

bool LimitModifier::Equals(const ResultModifier *other_p) const {
	if (!ResultModifier::Equals(other_p)) {
		return false;
	}
	auto &other = (LimitModifier &)*other_p;
	if (!BaseExpression::Equals(limit.get(), other.limit.get())) {
		return false;
	}
	if (!BaseExpression::Equals(offset.get(), other.offset.get())) {
		return false;
	}
	return true;
}

unique_ptr<ResultModifier> LimitModifier::Copy() {
	auto copy = make_unique<LimitModifier>();
	if (limit) {
		copy->limit = limit->Copy();
	}
	if (offset) {
		copy->offset = offset->Copy();
	}
	return move(copy);
}

void LimitModifier::Serialize(Serializer &serializer) {
	ResultModifier::Serialize(serializer);
	serializer.WriteOptional(limit);
	serializer.WriteOptional(offset);
}

unique_ptr<ResultModifier> LimitModifier::Deserialize(Deserializer &source) {
	auto mod = make_unique<LimitModifier>();
	mod->limit = source.ReadOptional<ParsedExpression>();
	mod->offset = source.ReadOptional<ParsedExpression>();
	return move(mod);
}

bool DistinctModifier::Equals(const ResultModifier *other_p) const {
	if (!ResultModifier::Equals(other_p)) {
		return false;
	}
	auto &other = (DistinctModifier &)*other_p;
	if (!ExpressionUtil::ListEquals(distinct_on_targets, other.distinct_on_targets)) {
		return false;
	}
	return true;
}

unique_ptr<ResultModifier> DistinctModifier::Copy() {
	auto copy = make_unique<DistinctModifier>();
	for (auto &expr : distinct_on_targets) {
		copy->distinct_on_targets.push_back(expr->Copy());
	}
	return move(copy);
}

void DistinctModifier::Serialize(Serializer &serializer) {
	ResultModifier::Serialize(serializer);
	serializer.WriteList(distinct_on_targets);
}

unique_ptr<ResultModifier> DistinctModifier::Deserialize(Deserializer &source) {
	auto mod = make_unique<DistinctModifier>();
	source.ReadList<ParsedExpression>(mod->distinct_on_targets);
	return move(mod);
}

bool OrderModifier::Equals(const ResultModifier *other_p) const {
	if (!ResultModifier::Equals(other_p)) {
		return false;
	}
	auto &other = (OrderModifier &)*other_p;
	if (orders.size() != other.orders.size()) {
		return false;
	}
	for (idx_t i = 0; i < orders.size(); i++) {
		if (orders[i].type != other.orders[i].type) {
			return false;
		}
		if (!BaseExpression::Equals(orders[i].expression.get(), other.orders[i].expression.get())) {
			return false;
		}
	}
	return true;
}

unique_ptr<ResultModifier> OrderModifier::Copy() {
	auto copy = make_unique<OrderModifier>();
	for (auto &order : orders) {
		copy->orders.emplace_back(order.type, order.null_order, order.expression->Copy());
	}
	return move(copy);
}

string OrderByNode::ToString() const {
	auto str = expression->ToString();
	str += (type == OrderType::ASCENDING) ? " ASC" : " DESC";
	switch (null_order) {
	case OrderByNullType::NULLS_FIRST:
		str += " NULLS FIRST";
		break;
	case OrderByNullType::NULLS_LAST:
		str += " NULLS LAST";
		break;
	default:
		break;
	}
	return str;
}

void OrderByNode::Serialize(Serializer &serializer) {
	serializer.Write<OrderType>(type);
	serializer.Write<OrderByNullType>(null_order);
	expression->Serialize(serializer);
}

OrderByNode OrderByNode::Deserialize(Deserializer &source) {
	auto type = source.Read<OrderType>();
	auto null_order = source.Read<OrderByNullType>();
	auto expression = ParsedExpression::Deserialize(source);
	return OrderByNode(type, null_order, move(expression));
}

void OrderModifier::Serialize(Serializer &serializer) {
	ResultModifier::Serialize(serializer);
	serializer.Write<int64_t>(orders.size());
	for (auto &order : orders) {
		order.Serialize(serializer);
	}
}

unique_ptr<ResultModifier> OrderModifier::Deserialize(Deserializer &source) {
	auto mod = make_unique<OrderModifier>();
	auto order_count = source.Read<int64_t>();
	for (int64_t i = 0; i < order_count; i++) {
		mod->orders.push_back(OrderByNode::Deserialize((source)));
	}
	return move(mod);
}

} // namespace duckdb
