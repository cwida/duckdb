#include "duckdb/planner/operator/logical_create.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

LogicalCreate::LogicalCreate(LogicalOperatorType type, unique_ptr<CreateInfo> info,
                             optional_ptr<SchemaCatalogEntry> schema)
    : LogicalOperator(type), schema(schema), info(std::move(info)) {
}

LogicalCreate::LogicalCreate(LogicalOperatorType type, ClientContext &context, unique_ptr<CreateInfo> info_p)
    : LogicalOperator(type), info(std::move(info_p)) {
	this->schema = Catalog::GetSchema(context, info->GetQualifiedName().Catalog(), info->GetQualifiedName().Schema(),
	                                  OnEntryNotFound::RETURN_NULL);
}

idx_t LogicalCreate::EstimateCardinality(ClientContext &context) {
	return 1;
}

void LogicalCreate::ResolveTypes() {
	types.emplace_back(LogicalType::BIGINT);
}

} // namespace duckdb
