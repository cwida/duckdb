//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/join/join_filter_pushdown.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/projection_index.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/runtime_filter_cast.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {
class DataChunk;
class DynamicTableFilterSet;
class LogicalGet;
class JoinHashTable;
class PhysicalOperator;
class PhysicalComparisonJoin;
struct GlobalUngroupedAggregateState;
struct LocalUngroupedAggregateState;

enum class JoinFilterPushdownMode : uint8_t {
	//! The pushed expression can be reconstructed on top of the raw scan value for BF/PRF runtime filters
	RECONSTRUCT_EXPRESSION,
	//! Only storage-domain filters are safe; BF/PRF reconstruction on raw scan values is not
	STORAGE_ONLY
};

struct JoinFilterPushdownColumn {
	//! Index into JoinFilterPushdownInfo::join_condition for this pushed column
	idx_t join_filter_idx = DConstants::INVALID_INDEX;
	//! The probe column index to which this filter should be applied
	ColumnBinding probe_column_index;
	//! The type of the value in storage (LogicalGet)
	LogicalType storage_type;
	//! Whether runtime filters can reconstruct the pushed expression, or whether only storage-domain filters are safe
	JoinFilterPushdownMode mode = JoinFilterPushdownMode::RECONSTRUCT_EXPRESSION;
	//! Casts from the raw scan value to the pushed probe expression, in evaluation order.
	vector<RuntimeFilterCastStep> runtime_filter_casts;
};

enum class DeferredRuntimeFilterType : uint8_t { BLOOM_FILTER, PREFIX_RANGE };

struct DeferredRuntimeFilterPushdown {
	DeferredRuntimeFilterType type = DeferredRuntimeFilterType::BLOOM_FILTER;
	const PhysicalOperator *op = nullptr;
	shared_ptr<DynamicTableFilterSet> dynamic_filters;
	JoinFilterPushdownColumn column;
	ProjectionIndex filter_col_idx;
};

struct JoinFilterGlobalState {
	~JoinFilterGlobalState();

	//! Global Min/Max aggregates for filter pushdown
	unique_ptr<GlobalUngroupedAggregateState> global_aggregate_state;
	vector<DeferredRuntimeFilterPushdown> deferred_runtime_filters;
};

struct JoinFilterLocalState {
	~JoinFilterLocalState();

	//! Local Min/Max aggregates for filter pushdown
	unique_ptr<LocalUngroupedAggregateState> local_aggregate_state;
};

struct JoinFilterPushdownFilter {
	//! The dynamic table filter set where to push filters into
	shared_ptr<DynamicTableFilterSet> dynamic_filters;
	//! The columns for which we should generate filters
	vector<JoinFilterPushdownColumn> columns;
	//! Target scan estimate for filter costing when intervening operators reduce the probe cardinality.
	idx_t estimated_scan_cardinality = 0;
};

struct PushdownFilterTarget {
	PushdownFilterTarget(LogicalGet &get, vector<JoinFilterPushdownColumn> columns_p)
	    : get(get), columns(std::move(columns_p)) {
	}

	LogicalGet &get;
	vector<JoinFilterPushdownColumn> columns;
};

struct JoinFilterPushdownUtil {
	static bool PushdownJoinFilterExpression(const Expression &expr, JoinFilterPushdownColumn &filter);
	static bool JoinTypeIsSupported(JoinType join_type);
};

struct JoinFilterPushdownInfo {
	//! The join condition indexes for which we compute the min/max aggregates
	vector<idx_t> join_condition;
	//! The probes to push the filter into
	vector<JoinFilterPushdownFilter> probe_info;
	//! Min/Max aggregates
	vector<unique_ptr<Expression>> min_max_aggregates;
	//! Whether the build side has a filter -> we might be able to push down a bloom filter into the probe side
	bool build_side_has_filter;

public:
	unique_ptr<JoinFilterGlobalState> GetGlobalState(ClientContext &context, const PhysicalOperator &op) const;
	unique_ptr<JoinFilterLocalState> GetLocalState(JoinFilterGlobalState &gstate) const;

	void Sink(DataChunk &chunk, JoinFilterLocalState &lstate) const;
	void Combine(JoinFilterGlobalState &gstate, JoinFilterLocalState &lstate) const;
	unique_ptr<DataChunk> Finalize(ClientContext &context, JoinFilterGlobalState &gstate,
	                               const PhysicalComparisonJoin &op, optional_ptr<JoinHashTable> ht = nullptr) const;

	unique_ptr<DataChunk> FinalizeMinMax(JoinFilterGlobalState &gstate) const;
	unique_ptr<DataChunk> FinalizeFilters(ClientContext &context, const PhysicalComparisonJoin &op,
	                                      unique_ptr<DataChunk> final_min_max, optional_ptr<JoinHashTable> ht = nullptr,
	                                      bool allow_bloom_filters = true, bool allow_prefix_range_filters = true,
	                                      optional_ptr<JoinFilterGlobalState> gstate = nullptr) const;

private:
	bool PushInFilter(ClientContext &context, const JoinFilterPushdownFilter &info,
	                  const JoinFilterPushdownColumn &column, JoinHashTable &ht, const PhysicalOperator &op,
	                  idx_t filter_idx, ProjectionIndex filter_col_idx) const;

	void DeferRuntimeFilter(DeferredRuntimeFilterType type, const PhysicalOperator &op,
	                        const JoinFilterPushdownFilter &info, const JoinFilterPushdownColumn &column,
	                        ProjectionIndex filter_col_idx, JoinFilterGlobalState &gstate) const;
	bool TryRegisterPrefixRangeFilter(const JoinFilterPushdownFilter &info, ClientContext &context, JoinHashTable &ht,
	                                  const PhysicalOperator &op, const JoinFilterPushdownColumn &column,
	                                  ProjectionIndex filter_col_idx, const Value &min_val, const Value &max_val,
	                                  idx_t max_bits, JoinFilterGlobalState &gstate) const;

	bool CanUseInFilter(const ClientContext &context, optional_ptr<JoinHashTable> ht, const ExpressionType &cmp) const;
	bool CanUseBloomFilter(const JoinFilterPushdownFilter &info, const ClientContext &context,
	                       const PhysicalComparisonJoin &op, const ExpressionType &cmp,
	                       optional_ptr<JoinHashTable> ht = nullptr) const;
	bool CanUsePrefixRangeFilter(const JoinFilterPushdownFilter &info, const ClientContext &context,
	                             const PhysicalComparisonJoin &op, optional_ptr<JoinHashTable> ht,
	                             const ExpressionType &cmp) const;
};

} // namespace duckdb
