#include "duckdb/parser/common_table_expression_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

namespace duckdb {

CommonTableExpressionInfo::~CommonTableExpressionInfo() {
}

CommonTableExpressionInfo::CommonTableExpressionInfo(unique_ptr<SelectStatement> query,
                                                     unique_ptr<QueryNode> query_node_p) {
	if (query_node_p) {
		this->query_node = std::move(query_node_p);
	} else if (query) {
		this->query_node = std::move(query->node);
	}
}

unique_ptr<CommonTableExpressionInfo> CommonTableExpressionInfo::Copy() const {
	auto result = make_uniq<CommonTableExpressionInfo>();
	result->aliases = aliases;
	if (query_node) {
		result->query_node = query_node->Copy();
	}
	for (auto &key : key_targets) {
		result->key_targets.push_back(key->Copy());
	}
	for (auto &agg : payload_aggregates) {
		result->payload_aggregates.push_back(agg->Copy());
	}
	result->materialized = materialized;
	result->is_trigger_generated = is_trigger_generated;
	return result;
}

CTEMaterialize CommonTableExpressionInfo::GetMaterializedForSerialization(Serializer &serializer) const {
	if (serializer.ShouldSerialize(StorageVersion::V1_5_0)) {
		return materialized;
	}
	return CTEMaterialize::CTE_MATERIALIZE_DEFAULT;
}

unique_ptr<SelectStatement> CommonTableExpressionInfo::GetQueryForSerialization(Serializer &serializer) const {
	if (!query_node ||
	    (query_node->type != QueryNodeType::SELECT_NODE && query_node->type != QueryNodeType::SET_OPERATION_NODE &&
	     query_node->type != QueryNodeType::RECURSIVE_CTE_NODE)) {
		throw SerializationException("Side-effecting CTEs require storage version v2.0.0 or higher and cannot be "
		                             "serialized to older storage formats");
	}
	auto select = make_uniq<SelectStatement>();
	select->node = query_node->Copy();
	return select;
}

} // namespace duckdb
