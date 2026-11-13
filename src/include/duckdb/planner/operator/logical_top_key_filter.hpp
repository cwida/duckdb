//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_top_key_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/bound_query_node.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {
struct DynamicFilterData;

//! LogicalTopKeyFilter is a streaming filter below a grouped aggregate whose Top-N orders by a prefix of the group
//! keys. It keeps the smallest "limit" distinct key tuples it has seen and only passes the rows whose key is not
//! larger than the largest of them, so that the aggregate only sees the rows of groups that can be part of the result.
class LogicalTopKeyFilter : public LogicalOperator {
public:
	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_TOP_KEY_FILTER;

public:
	LogicalTopKeyFilter(vector<BoundOrderByNode> orders, idx_t limit);
	~LogicalTopKeyFilter() override;

	//! The keys (expressions over the child) and their order
	vector<BoundOrderByNode> orders;
	//! The number of distinct key tuples that pass
	idx_t limit;
	//! Dynamic table filter on the first key (if any)
	shared_ptr<DynamicFilterData> dynamic_filter;

public:
	vector<ColumnBinding> GetColumnBindings() override {
		return children[0]->GetColumnBindings();
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

protected:
	void ResolveTypes() override {
		types = children[0]->types;
	}
};
} // namespace duckdb
