#include "duckdb/execution/operator/aggregate/aggregate_object.hpp"

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
                                         const vector<LogicalType> &payload_types)
    : filter_executor(context, &filter_expr), true_sel(STANDARD_VECTOR_SIZE) {
	if (payload_types.empty()) {
		return;
	}
	filtered_payload.InitializeEmpty(payload_types);
}

idx_t AggregateFilterData::Select(DataChunk &chunk) {
	selected_count = filter_executor.SelectExpression(chunk, true_sel);
	return selected_count;
}

idx_t AggregateFilterData::ApplyFilter(DataChunk &payload) {
	Select(payload);
	for (idx_t i = 0; i < sliced_columns.size(); i++) {
		auto col_idx = sliced_columns[i];
		auto &source = payload.data[col_idx];
		auto &target = filtered_payload.data[col_idx];
		// payload columns that reference the same vector share one slice
		bool shared = false;
		for (idx_t prev = 0; prev < i; prev++) {
			auto prev_idx = sliced_columns[prev];
			auto &prev_source = payload.data[prev_idx];
			if (prev_source.GetBufferRef() && prev_source.GetBufferRef() == source.GetBufferRef() &&
			    prev_source.GetType() == source.GetType()) {
				target.Reference(filtered_payload.data[prev_idx]);
				shared = true;
				break;
			}
		}
		if (!shared) {
			target.Slice(source, true_sel, selected_count);
		}
	}
	filtered_payload.SetCardinalityUnsafe(selected_count);
	return selected_count;
}

AggregateFilterDataSet::AggregateFilterDataSet() {
}

void AggregateFilterDataSet::Initialize(ClientContext &context, const vector<AggregateObject> &aggregates,
                                        const vector<LogicalType> &payload_types, bool payload_holds_children) {
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
	aggregate_filter_idx.resize(aggregates.size(), DConstants::INVALID_INDEX);
	update_filter_idx.resize(aggregates.size(), DConstants::INVALID_INDEX);
	first_filter_use.resize(aggregates.size(), false);
	vector<reference<const Expression>> filters;
	idx_t payload_idx = 0;
	for (idx_t aggr_idx = 0; aggr_idx < aggregates.size(); aggr_idx++) {
		auto &aggr = aggregates[aggr_idx];
		if (!aggr.filter) {
			payload_idx += aggr.child_count;
			continue;
		}
		// aggregates with an identical filter share the filter data
		idx_t filter_idx = DConstants::INVALID_INDEX;
		for (idx_t i = 0; i < filters.size(); i++) {
			if (filters[i].get().Equals(*aggr.filter)) {
				filter_idx = i;
				break;
			}
		}
		if (filter_idx == DConstants::INVALID_INDEX) {
			filter_idx = filters.size();
			filters.push_back(*aggr.filter);
			filter_data.push_back(make_uniq<AggregateFilterData>(context, *aggr.filter, payload_types));
			if (!payload_holds_children) {
				auto &sliced_columns = filter_data.back()->sliced_columns;
				for (idx_t col_idx = 0; col_idx < payload_types.size(); col_idx++) {
					sliced_columns.push_back(col_idx);
				}
			}
		}
		aggregate_filter_idx[aggr_idx] = filter_idx;
		if (!aggr.IsDistinct()) {
			update_filter_idx[aggr_idx] = filter_idx;
			auto &data = *filter_data[filter_idx];
			if (payload_holds_children) {
				for (idx_t i = 0; i < aggr.child_count; i++) {
					data.sliced_columns.push_back(payload_idx + i);
				}
			}
			bool first_use = true;
			for (idx_t prev_idx = 0; prev_idx < aggr_idx; prev_idx++) {
				if (update_filter_idx[prev_idx] == filter_idx) {
					first_use = false;
					break;
				}
			}
			first_filter_use[aggr_idx] = first_use;
		}
		payload_idx += aggr.child_count;
	}
}

idx_t AggregateFilterDataSet::GetFilterIndex(idx_t aggr_idx) const {
	if (aggr_idx >= aggregate_filter_idx.size()) {
		return DConstants::INVALID_INDEX;
	}
	return aggregate_filter_idx[aggr_idx];
}

bool AggregateFilterDataSet::IsFirstFilterUse(idx_t aggr_idx) const {
	D_ASSERT(aggr_idx < first_filter_use.size());
	return first_filter_use[aggr_idx];
}

AggregateFilterData &AggregateFilterDataSet::GetFilterData(idx_t aggr_idx) {
	D_ASSERT(aggr_idx < aggregate_filter_idx.size());
	D_ASSERT(aggregate_filter_idx[aggr_idx] != DConstants::INVALID_INDEX);
	return *filter_data[aggregate_filter_idx[aggr_idx]];
}

void AggregateFilterDataSet::ApplyFilters(DataChunk &payload, const unsafe_vector<idx_t> &aggregate_indices) {
	if (filter_data.empty()) {
		return;
	}
	for (idx_t i = 0; i < aggregate_indices.size(); i++) {
		auto aggr_idx = aggregate_indices[i];
		D_ASSERT(aggr_idx < update_filter_idx.size());
		auto filter_idx = update_filter_idx[aggr_idx];
		if (filter_idx == DConstants::INVALID_INDEX) {
			continue;
		}
		// apply the filter once: for the first aggregate in the list that uses it
		bool applied = false;
		for (idx_t prev = 0; prev < i; prev++) {
			if (update_filter_idx[aggregate_indices[prev]] == filter_idx) {
				applied = true;
				break;
			}
		}
		if (!applied) {
			filter_data[filter_idx]->ApplyFilter(payload);
		}
	}
}

void AggregateFilterDataSet::ApplyFilters(DataChunk &payload) {
	for (auto &data : filter_data) {
		data->ApplyFilter(payload);
	}
}

} // namespace duckdb
