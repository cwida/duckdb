#include "duckdb/planner/bound_parameter_map.hpp"
#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"

namespace duckdb {

BoundParameterMap::BoundParameterMap(identifier_map_t<BoundParameterData> &parameter_data)
    : parameter_data(parameter_data) {
}

LogicalType BoundParameterMap::GetReturnType(const Identifier &identifier) {
	D_ASSERT(!identifier.empty());
	auto it = parameter_data.find(identifier);
	if (it == parameter_data.end()) {
		return LogicalTypeId::UNKNOWN;
	}
	return it->second.return_type;
}

bound_parameter_map_t *BoundParameterMap::GetParametersPtr() {
	return &parameters;
}

const bound_parameter_map_t &BoundParameterMap::GetParameters() {
	return parameters;
}

const identifier_map_t<BoundParameterData> &BoundParameterMap::GetParameterData() {
	return parameter_data;
}

shared_ptr<BoundParameterData> BoundParameterMap::CreateOrGetData(const Identifier &identifier) {
	auto entry = parameters.find(identifier);
	if (entry == parameters.end()) {
		// no entry yet: create a new one
		auto data = make_shared_ptr<BoundParameterData>();
		data->return_type = GetReturnType(identifier);

		CreateNewParameter(identifier.GetIdentifierName(), data);
		return data;
	}
	return entry->second;
}

unique_ptr<BoundParameterExpression> BoundParameterMap::BindParameterExpression(ParameterExpression &expr) {
	auto &identifier = expr.Identifier();
	D_ASSERT(!parameter_data.count(identifier));

	// No value has been supplied yet,
	// We return a shared pointer to an object that will get populated with a Value later
	// When the BoundParameterExpression gets executed, this will be used to get the corresponding value
	auto param_data = CreateOrGetData(identifier);
	auto bound_expr = make_uniq<BoundParameterExpression>(identifier, expr.GetAlias());

	bound_expr->ParameterDataMutable() = param_data;

	auto param_type = param_data->return_type;
	auto identifier_type = GetReturnType(identifier);

	// we found a type for this bound parameter, but now we found another occurrence with the same identifier,
	// a CAST around this consecutive occurrence might swallow the unknown type of this consecutive occurrence,
	// then, if we do not rebind, we potentially have unknown data types during execution
	if (identifier_type == LogicalType::UNKNOWN && param_type != LogicalType::UNKNOWN) {
		rebind = true;
	}

	bound_expr->SetReturnType(identifier_type);
	return bound_expr;
}

void BoundParameterMap::CreateNewParameter(const string &id, const shared_ptr<BoundParameterData> &param_data) {
	D_ASSERT(!parameters.count(Identifier(id)));
	parameters.emplace(std::make_pair(id, param_data));
}

} // namespace duckdb
