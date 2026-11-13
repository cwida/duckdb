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
class BloomFilter;
class PrefixRangeFilter;
struct GlobalUngroupedAggregateState;
struct LocalUngroupedAggregateState;

enum class JoinFilterPushdownMode : uint8_t {
	//! The pushed expression can be reconstructed on top of the raw scan value for BF/PRF runtime filters
	RECONSTRUCT_EXPRESSION,
	//! Only storage-domain filters are safe; BF/PRF reconstruction on raw scan values is not
	STORAGE_ONLY
};

struct JoinFilterPushdownColumn {
	JoinFilterPushdownColumn() = default;
	JoinFilterPushdownColumn(const JoinFilterPushdownColumn &other)
	    : join_filter_idx(other.join_filter_idx), probe_column_index(other.probe_column_index),
	      storage_type(other.storage_type), mode(other.mode),
	      input_expression(other.input_expression ? other.input_expression->Copy() : nullptr) {
	}
	JoinFilterPushdownColumn &operator=(const JoinFilterPushdownColumn &other) {
		if (this != &other) {
			JoinFilterPushdownColumn copy(other);
			*this = std::move(copy);
		}
		return *this;
	}
	JoinFilterPushdownColumn(JoinFilterPushdownColumn &&other) = default;
	JoinFilterPushdownColumn &operator=(JoinFilterPushdownColumn &&other) = default;

	//! Index into JoinFilterPushdownInfo::join_condition for this pushed column
	idx_t join_filter_idx = DConstants::INVALID_INDEX;
	//! The probe column index to which this filter should be applied
	ColumnBinding probe_column_index;
	//! The type of the value in storage (LogicalGet)
	LogicalType storage_type;
	//! Whether runtime filters can reconstruct the pushed expression, or whether only storage-domain filters are safe
	JoinFilterPushdownMode mode = JoinFilterPushdownMode::RECONSTRUCT_EXPRESSION;
	//! Filter input over BoundReferenceExpression(0) in the existing scan projection's type.
	unique_ptr<Expression> input_expression;
};

enum class DeferredRuntimeFilterType : uint8_t { BLOOM_FILTER, PREFIX_RANGE };

struct DeferredRuntimeFilterPushdown {
	DeferredRuntimeFilterType type = DeferredRuntimeFilterType::BLOOM_FILTER;
	const PhysicalOperator *op = nullptr;
	shared_ptr<DynamicTableFilterSet> dynamic_filters;
	JoinFilterPushdownColumn column;
	ProjectionIndex filter_col_idx;
	//! The join condition the filter is built from
	idx_t condition_idx = 0;
	//! Per-column filters of a composite-key join; when unset, the hash table's own filter is published
	shared_ptr<BloomFilter> column_bloom_filter;
	optional_ptr<PrefixRangeFilter> column_prefix_range_filter;
};

struct JoinFilterGlobalState {
	~JoinFilterGlobalState();

	//! Global Min/Max aggregates for filter pushdown
	unique_ptr<GlobalUngroupedAggregateState> global_aggregate_state;
	vector<DeferredRuntimeFilterPushdown> deferred_runtime_filters;
	//! Per pushed condition (in join_condition order): the single-column bloom filter that Sink fills for a
	//! composite-key join that is likely to become external, nullptr for a condition that gets none
	vector<shared_ptr<BloomFilter>> column_bloom_filters;
};

struct JoinFilterLocalState {
	~JoinFilterLocalState();

	//! Local Min/Max aggregates for filter pushdown
	unique_ptr<LocalUngroupedAggregateState> local_aggregate_state;
	//! The per-column bloom filters to fill during Sink (see JoinFilterGlobalState::column_bloom_filters)
	vector<shared_ptr<BloomFilter>> column_bloom_filters;
	unique_ptr<Vector> column_hashes;
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
	//! Whether a join on several equality columns publishes one bloom / prefix-range filter per equality column
	bool column_runtime_filters = false;
	//! The join conditions (indexes into the join's conditions) that get no per-column runtime filter because the
	//! build side is expected to hold most of the values of the probe column
	vector<idx_t> column_filter_gated_conditions;

public:
	//! Whether the given join condition publishes a per-column bloom / prefix-range filter
	bool HasColumnRuntimeFilter(idx_t condition_idx) const;
	//! Creates the per-column bloom filters that are filled during Sink
	void InitializeColumnBloomFilters(ClientContext &context, const PhysicalComparisonJoin &op,
	                                  JoinFilterGlobalState &gstate) const;

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

	DeferredRuntimeFilterPushdown &DeferRuntimeFilter(DeferredRuntimeFilterType type, const PhysicalOperator &op,
	                                                  const JoinFilterPushdownFilter &info,
	                                                  const JoinFilterPushdownColumn &column,
	                                                  ProjectionIndex filter_col_idx, idx_t condition_idx,
	                                                  JoinFilterGlobalState &gstate) const;
	bool TryRegisterPrefixRangeFilter(const JoinFilterPushdownFilter &info, ClientContext &context, JoinHashTable &ht,
	                                  const PhysicalOperator &op, const JoinFilterPushdownColumn &column,
	                                  ProjectionIndex filter_col_idx, idx_t condition_idx, const Value &min_val,
	                                  const Value &max_val, idx_t max_bits, JoinFilterGlobalState &gstate) const;

	bool CanUseInFilter(const ClientContext &context, optional_ptr<JoinHashTable> ht, const ExpressionType &cmp) const;
	bool CanUseBloomFilter(const JoinFilterPushdownFilter &info, const ClientContext &context,
	                       const PhysicalComparisonJoin &op, const ExpressionType &cmp, idx_t condition_idx,
	                       optional_ptr<JoinHashTable> ht = nullptr) const;
	bool CanUsePrefixRangeFilter(const JoinFilterPushdownFilter &info, const ClientContext &context,
	                             const PhysicalComparisonJoin &op, optional_ptr<JoinHashTable> ht,
	                             const ExpressionType &cmp, idx_t condition_idx) const;
};

} // namespace duckdb
