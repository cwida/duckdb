#include "duckdb/execution/operator/aggregate/aggregate_object.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"

namespace duckdb {

AggregateObject::AggregateObject(BoundAggregateFunction function, FunctionData *bind_data, idx_t child_count,
                                 idx_t payload_size, AggregateType aggr_type, PhysicalType return_type,
                                 optional_ptr<const Expression> filter)
    : function(std::move(function)),
      bind_data_wrapper(bind_data ? make_shared_ptr<FunctionDataWrapper>(bind_data->Copy()) : nullptr),
      child_count(child_count), payload_size(payload_size), aggr_type(aggr_type), return_type(return_type),
      filter(filter) {
}

AggregateObject::AggregateObject(BoundAggregateExpression &aggr)
    : AggregateObject(aggr.Function(), aggr.BindInfoMutable().get(), aggr.GetChildren().size(),
                      AlignValue(aggr.Function().GetStateSize(aggr.BindInfoMutable().get())), aggr.GetAggregateType(),
                      aggr.GetReturnType().InternalType(), aggr.GetFilter().get()) {
}

AggregateObject::AggregateObject(BoundAggregateExpression *aggr) : AggregateObject(*aggr) {
}

AggregateObject::AggregateObject(const BoundWindowExpression &window)
    : AggregateObject(*window.AggregateFunction(), window.BindInfo().get(), window.GetChildren().size(),
                      AlignValue(window.AggregateFunction()->GetStateSize(window.BindInfo().get())),
                      window.Distinct() ? AggregateType::DISTINCT : AggregateType::NON_DISTINCT,
                      window.GetReturnType().InternalType(), window.Filter().get()) {
}

vector<AggregateObject> AggregateObject::CreateAggregateObjects(const vector<BoundAggregateExpression *> &bindings) {
	vector<AggregateObject> aggregates;
	aggregates.reserve(bindings.size());
	for (auto &binding : bindings) {
		aggregates.emplace_back(binding);
	}
	return aggregates;
}

AggregateFilterData::AggregateFilterData(ClientContext &context, const Expression &filter_expr,
                                         DataChunk &filtered_payload)
    : filter_executor(context, &filter_expr), filtered_payload(filtered_payload), true_sel(STANDARD_VECTOR_SIZE) {
}

idx_t AggregateFilterData::Select(DataChunk &chunk) {
	selected_count = filter_executor.SelectExpression(chunk, true_sel);
	return selected_count;
}

idx_t AggregateFilterData::ApplyFilter(DataChunk &payload) {
	Select(payload);
	for (auto col_idx : sliced_columns) {
		filtered_payload.data[col_idx].Slice(payload.data[col_idx], true_sel, selected_count);
	}
	return selected_count;
}

AggregateFilterDataSet::AggregateFilterDataSet() {
}

void AggregateFilterDataSet::Initialize(ClientContext &context, const vector<AggregateObject> &aggregates,
                                        const vector<LogicalType> &payload_types) {
	bool has_filters = false;
	for (auto &aggregate : aggregates) {
		if (aggregate.filter) {
			has_filters = true;
			break;
		}
	}
	if (!has_filters) {
		// no filters: nothing to do
		return;
	}
	filtered_payload = make_uniq<DataChunk>();
	filtered_payload->InitializeEmpty(payload_types);
	aggregate_filter_idx.resize(aggregates.size(), DConstants::INVALID_INDEX);
	expression_map_t<idx_t> filter_indexes;
	idx_t payload_idx = 0;
	for (idx_t aggr_idx = 0; aggr_idx < aggregates.size(); aggr_idx++) {
		auto &aggr = aggregates[aggr_idx];
		if (aggr.filter) {
			// aggregates with an identical filter share the filter data
			auto entry = filter_indexes.find(*aggr.filter);
			if (entry == filter_indexes.end()) {
				entry = filter_indexes.emplace(*aggr.filter, filter_data.size()).first;
				filter_data.push_back(make_uniq<AggregateFilterData>(context, *aggr.filter, *filtered_payload));
			}
			aggregate_filter_idx[aggr_idx] = entry->second;
			if (!aggr.IsDistinct()) {
				auto &data = *filter_data[entry->second];
				data.aggregates.push_back(aggr_idx);
				for (idx_t i = 0; i < aggr.child_count; i++) {
					data.sliced_columns.push_back(payload_idx + i);
				}
			}
		}
		payload_idx += aggr.child_count;
	}
}

AggregateFilterData &AggregateFilterDataSet::GetFilterData(idx_t aggr_idx) {
	D_ASSERT(aggr_idx < aggregate_filter_idx.size());
	D_ASSERT(aggregate_filter_idx[aggr_idx] != DConstants::INVALID_INDEX);
	return *filter_data[aggregate_filter_idx[aggr_idx]];
}

static bool UsesFilter(const unsafe_vector<idx_t> &aggregate_indices, const AggregateFilterData &data) {
	for (auto aggr_idx : data.aggregates) {
		if (std::find(aggregate_indices.begin(), aggregate_indices.end(), aggr_idx) != aggregate_indices.end()) {
			return true;
		}
	}
	return false;
}

void AggregateFilterDataSet::ApplyFilters(DataChunk &payload,
                                          optional_ptr<const unsafe_vector<idx_t>> aggregate_indices) {
	for (auto &data : filter_data) {
		if (!aggregate_indices || UsesFilter(*aggregate_indices, *data)) {
			data->ApplyFilter(payload);
		}
	}
}

} // namespace duckdb
