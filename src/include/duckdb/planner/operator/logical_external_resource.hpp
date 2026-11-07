//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_external_resource.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/parser/statement/external_resource_statement.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

//! Bound payload of a CREATE / REGISTER / DESTROY EXTERNAL RESOURCE statement.
struct BoundExternalResource {
	ExternalResourceOperation operation;
	//! Resource type (provider). Set for CREATE and REGISTER.
	string type;
	//! Local name: the `AS <name>` alias (CREATE/REGISTER) or the target (DESTROY).
	string name;
	//! Bound create params. Set for CREATE.
	unordered_map<string, Value> params;
	//! Bound handle value. Set for REGISTER.
	Value handle;

	void Serialize(Serializer &serializer) const;
	static BoundExternalResource Deserialize(Deserializer &deserializer);
};

//! LogicalExternalResource represents CREATE / REGISTER / DESTROY EXTERNAL RESOURCE, returning (status, name,
//! type, handle).
class LogicalExternalResource : public LogicalOperator {
public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_EXTERNAL_RESOURCE;

public:
	explicit LogicalExternalResource(BoundExternalResource data);

	BoundExternalResource data;

public:
	idx_t EstimateCardinality(ClientContext &context) override;
	vector<ColumnBinding> GetColumnBindings() override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

protected:
	void ResolveTypes() override;
};

} // namespace duckdb
