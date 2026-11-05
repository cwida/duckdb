#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

#include "duckdb/parser/expression/function_expression.hpp"

#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/function/function_serialization.hpp"
#include "duckdb/function/scalar/generic_common.hpp"

namespace duckdb {

BoundAggregateExpression::BoundAggregateExpression(BoundAggregateFunction function,
                                                   vector<unique_ptr<Expression>> children,
                                                   unique_ptr<Expression> filter, unique_ptr<FunctionData> bind_info,
                                                   AggregateType aggr_type)
    : Expression(ExpressionType::BOUND_AGGREGATE, ExpressionClass::BOUND_AGGREGATE, function.GetReturnType()),
      function(std::move(function)), children(std::move(children)), bind_info(std::move(bind_info)),
      aggr_type(aggr_type), state_export_mode(AggregateStateExportMode::NONE), filter(std::move(filter)) {
	D_ASSERT(!this->function.GetName().empty());
}

bool BoundAggregateExpression::IsVolatile() const {
	return function.GetStability() == FunctionStability::VOLATILE || Expression::IsVolatile();
}

bool BoundAggregateExpression::CanThrow() const {
	return function.GetErrorMode() == FunctionErrors::CAN_THROW_RUNTIME_ERROR || Expression::CanThrow();
}

string BoundAggregateExpression::ToString() const {
	auto distinct = IsDistinct();
	auto &function_name = function.GetName();

	string result;
	result += SQLIdentifier(function_name);
	result += "(";
	if (distinct) {
		result += "DISTINCT ";
	}
	result += StringUtil::Join(children, children.size(), ", ",
	                           [&](const unique_ptr<Expression> &child) { return child->ToString(); });

	// ordered aggregate
	if (order_bys && !order_bys->orders.empty()) {
		if (children.empty()) {
			result += ") WITHIN GROUP (";
		}
		result += " ORDER BY ";
		for (idx_t i = 0; i < order_bys->orders.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += order_bys->orders[i].ToString();
		}
	}
	result += ")";

	if (state_export_mode == AggregateStateExportMode::STATE_EXPORT) {
		result += " EXPORT_STATE";
	}

	// filtered aggregate
	if (filter) {
		result += " FILTER (WHERE " + filter->ToString() + ")";
	}

	return result;
}

hash_t BoundAggregateExpression::Hash() const {
	hash_t result = Expression::Hash();
	result = CombineHash(result, function.Hash());
	result = CombineHash(result, duckdb::Hash(IsDistinct()));
	return result;
}

bool BoundAggregateExpression::Equals(const BaseExpression &other_p) const {
	if (!Expression::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<BoundAggregateExpression>();
	if (other.aggr_type != aggr_type) {
		return false;
	}
	if (other.function != function) {
		return false;
	}
	if (children.size() != other.children.size()) {
		return false;
	}
	if (!Expression::Equals(other.filter, filter)) {
		return false;
	}
	for (idx_t i = 0; i < children.size(); i++) {
		if (!Expression::Equals(*children[i], *other.children[i])) {
			return false;
		}
		// expression equality ignores the return type, but a collation lives in the return type and decides
		// which argument values compare equal - two aggregates that disagree on it are not the same aggregate
		if (!children[i]->GetReturnType().EqualsIncludingCollation(other.children[i]->GetReturnType())) {
			return false;
		}
	}
	if (state_export_mode != other.state_export_mode) {
		return false;
	}
	if (!FunctionData::Equals(bind_info.get(), other.BindInfo().get())) {
		return false;
	}
	if (!BoundOrderModifier::Equals(order_bys, other.order_bys)) {
		return false;
	}
	return true;
}

bool BoundAggregateExpression::PropagatesNullValues() const {
	return function.GetProperties().GetNullHandling() == FunctionNullHandling::SPECIAL_HANDLING
	           ? false
	           : Expression::PropagatesNullValues();
}

unique_ptr<Expression> BoundAggregateExpression::Copy() const {
	vector<unique_ptr<Expression>> new_children;
	new_children.reserve(children.size());
	for (auto &child : children) {
		new_children.push_back(child->Copy());
	}
	auto new_bind_info = bind_info ? bind_info->Copy() : nullptr;
	auto new_filter = filter ? filter->Copy() : nullptr;
	auto copy = make_uniq<BoundAggregateExpression>(function, std::move(new_children), std::move(new_filter),
	                                                std::move(new_bind_info), aggr_type);
	copy->CopyProperties(*this);
	copy->state_export_mode = state_export_mode;
	copy->order_bys = order_bys ? order_bys->Copy() : nullptr;
	return std::move(copy);
}

void BoundAggregateExpression::Serialize(Serializer &serializer) const {
	Expression::Serialize(serializer);
	serializer.WriteProperty(200, "return_type", return_type);
	serializer.WriteProperty(201, "children", children);
	FunctionSerializer::Serialize(serializer, function, bind_info.get());
	serializer.WriteProperty(203, "aggregate_type", aggr_type);
	serializer.WritePropertyWithDefault(204, "filter", filter, unique_ptr<Expression>());
	serializer.WritePropertyWithDefault(205, "order_bys", order_bys, unique_ptr<BoundOrderModifier>());
	serializer.WritePropertyWithDefault(206, "state_export", state_export_mode, AggregateStateExportMode::NONE);
}

unique_ptr<Expression> BoundAggregateExpression::Deserialize(Deserializer &deserializer) {
	auto return_type = deserializer.ReadProperty<LogicalType>(200, "return_type");
	auto children = deserializer.ReadProperty<vector<unique_ptr<Expression>>>(201, "children");
	auto entry = FunctionSerializer::Deserialize<BoundAggregateFunction, AggregateFunctionCatalogEntry>(
	    deserializer, CatalogType::AGGREGATE_FUNCTION_ENTRY, children, return_type);
	auto aggregate_type = deserializer.ReadProperty<AggregateType>(203, "aggregate_type");
	auto filter =
	    deserializer.ReadPropertyWithExplicitDefault<unique_ptr<Expression>>(204, "filter", unique_ptr<Expression>());
	auto result = make_uniq<BoundAggregateExpression>(std::move(entry.first), std::move(children), std::move(filter),
	                                                  std::move(entry.second), aggregate_type);
	deserializer.ReadPropertyWithExplicitDefault(205, "order_bys", result->order_bys, unique_ptr<BoundOrderModifier>());
	deserializer.ReadPropertyWithExplicitDefault(206, "state_export", result->state_export_mode,
	                                             AggregateStateExportMode::NONE);
	if (result->state_export_mode == AggregateStateExportMode::STATE_EXPORT) {
		if (!return_type.IsAggregateState()) {
			throw SerializationException("Aggregate State export should return an aggregate state type");
		}
		ExportAggregateFunction::SetStateExport(*result, return_type);
	} else if (result->return_type != return_type) {
		// return type mismatch - push a cast
		auto &context = deserializer.Get<ClientContext &>();
		return BoundCastExpression::AddCastToType(context, std::move(result), return_type);
	}
	return Expression::PreserveReturnType(return_type, std::move(result));
}

} // namespace duckdb
