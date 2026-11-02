//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/aggregate/aggregate_object.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate_function.hpp"

namespace duckdb {

class BoundAggregateExpression;
class BoundWindowExpression;

struct FunctionDataWrapper {
	explicit FunctionDataWrapper(unique_ptr<FunctionData> function_data_p) : function_data(std::move(function_data_p)) {
	}

	unique_ptr<FunctionData> function_data;
};

struct AggregateObject { // NOLINT: work-around bug in clang-tidy
	AggregateObject(BoundAggregateFunction function, FunctionData *bind_data, idx_t child_count, idx_t payload_size,
	                AggregateType aggr_type, PhysicalType return_type, optional_ptr<const Expression> filter = nullptr);
	explicit AggregateObject(BoundAggregateExpression &aggr);
	explicit AggregateObject(BoundAggregateExpression *aggr);
	explicit AggregateObject(const BoundWindowExpression &window);

	FunctionData *GetFunctionData() const {
		return bind_data_wrapper ? bind_data_wrapper->function_data.get() : nullptr;
	}

	BoundAggregateFunction function;
	shared_ptr<FunctionDataWrapper> bind_data_wrapper;
	idx_t child_count;
	idx_t payload_size;
	AggregateType aggr_type;
	PhysicalType return_type;
	optional_ptr<const Expression> filter = nullptr;

public:
	bool IsDistinct() const {
		return aggr_type == AggregateType::DISTINCT;
	}
	static vector<AggregateObject> CreateAggregateObjects(const vector<BoundAggregateExpression *> &bindings);
};

struct AggregateFilterData {
	AggregateFilterData(ClientContext &context, const Expression &filter_expr,
	                    const vector<LogicalType> &payload_types);

	//! Evaluates the filter on the chunk; the selected rows are stored in true_sel and selected_count
	idx_t Select(DataChunk &chunk);
	//! Evaluates the filter and slices the payload columns in sliced_columns into filtered_payload
	idx_t ApplyFilter(DataChunk &payload);

	ExpressionExecutor filter_executor;
	DataChunk filtered_payload;
	SelectionVector true_sel;
	//! The payload columns read by the aggregates that share this filter
	unsafe_vector<idx_t> sliced_columns;
	//! The number of rows selected by the most recent Select/ApplyFilter
	idx_t selected_count = 0;
};

//! The filters of a set of aggregates: aggregates with an identical filter expression share one AggregateFilterData,
//! so a filter is evaluated once per chunk and every payload column it covers is sliced once
struct AggregateFilterDataSet {
	AggregateFilterDataSet();

	//! One entry per distinct filter expression
	vector<unique_ptr<AggregateFilterData>> filter_data;

public:
	//! Initializes the filters of the aggregates. If payload_holds_children is set the payload chunk holds the
	//! concatenated children of the aggregates and ApplyFilter only slices the children of the aggregates that share
	//! the filter; otherwise every payload column is sliced.
	void Initialize(ClientContext &context, const vector<AggregateObject> &aggregates,
	                const vector<LogicalType> &payload_types, bool payload_holds_children = false);

	bool HasFilters() const {
		return !filter_data.empty();
	}
	//! The index into filter_data of the filter of the aggregate, or DConstants::INVALID_INDEX if it has none
	idx_t GetFilterIndex(idx_t aggr_idx) const;
	//! Whether the aggregate is the first non-distinct aggregate that uses its filter
	bool IsFirstFilterUse(idx_t aggr_idx) const;
	AggregateFilterData &GetFilterData(idx_t aggr_idx);

	//! Applies every filter used by a non-distinct aggregate in aggregate_indices (ascending) once to the payload
	void ApplyFilters(DataChunk &payload, const unsafe_vector<idx_t> &aggregate_indices);
	//! Applies every filter once to the payload
	void ApplyFilters(DataChunk &payload);

private:
	//! Per aggregate: index into filter_data, or DConstants::INVALID_INDEX for aggregates without a filter
	unsafe_vector<idx_t> aggregate_filter_idx;
	//! Per aggregate: the filter applied when updating its state (INVALID_INDEX for distinct aggregates, whose
	//! filter is applied before their distinct hash table)
	unsafe_vector<idx_t> update_filter_idx;
	//! Per aggregate: whether it is the first non-distinct aggregate that uses its filter
	vector<bool> first_filter_use;
};

} // namespace duckdb
