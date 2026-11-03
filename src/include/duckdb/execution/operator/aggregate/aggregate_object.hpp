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
	AggregateFilterData(ClientContext &context, const Expression &filter_expr, DataChunk &filtered_payload);

	//! Evaluates the filter on the chunk into true_sel and selected_count
	idx_t Select(DataChunk &chunk);
	//! Evaluates the filter and slices the payload columns in sliced_columns into filtered_payload
	idx_t ApplyFilter(DataChunk &payload);

	ExpressionExecutor filter_executor;
	//! Shared by all filters of the set: this filter only fills its own sliced_columns
	DataChunk &filtered_payload;
	SelectionVector true_sel;
	//! The number of rows selected by the last Select
	idx_t selected_count = 0;
	//! The non-distinct aggregates that use this filter
	unsafe_vector<idx_t> aggregates;
	//! The payload columns (the children of those aggregates) that ApplyFilter slices
	unsafe_vector<idx_t> sliced_columns;
};

//! The filters of a set of aggregates: aggregates with an identical filter share one AggregateFilterData
struct AggregateFilterDataSet {
	AggregateFilterDataSet();

	//! One buffer-less chunk for the whole set: the filters slice disjoint payload columns into it
	unique_ptr<DataChunk> filtered_payload;
	//! One entry per distinct filter expression
	vector<unique_ptr<AggregateFilterData>> filter_data;

public:
	//! Initializes the filters of the aggregates; ApplyFilter expects the payload to hold their children
	void Initialize(ClientContext &context, const vector<AggregateObject> &aggregates,
	                const vector<LogicalType> &payload_types);

	AggregateFilterData &GetFilterData(idx_t aggr_idx);
	//! Applies each filter used by one of the aggregates in aggregate_indices (all aggregates if not given) once
	void ApplyFilters(DataChunk &payload, optional_ptr<const unsafe_vector<idx_t>> aggregate_indices = nullptr);

private:
	//! Per aggregate: index into filter_data, or DConstants::INVALID_INDEX for aggregates without a filter
	unsafe_vector<idx_t> aggregate_filter_idx;
};

} // namespace duckdb
