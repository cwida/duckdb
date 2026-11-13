#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

namespace {

//! Rewrites filters over a scan into filters over the columns of a row sample of that scan
class SampleFilterBinder {
public:
	explicit SampleFilterBinder(LogicalGet &get) : get(get) {
	}

	//! A table filter references its own columns, which index the scan's projection
	unique_ptr<Expression> BindTableFilter(ProjectionIndex filter_column, const ExpressionFilter &filter) {
		if (filter.expr->IsVolatile()) {
			return nullptr;
		}
		auto result = filter.expr->Copy();
		bool valid = true;
		ExpressionIterator::VisitExpressionMutable<BoundReferenceExpression>(
		    result, [&](BoundReferenceExpression &ref, unique_ptr<Expression> &) {
			    auto index = ref.Index();
			    if (filter.column_indexes.empty() ? index != 0 : index >= filter.column_indexes.size()) {
				    valid = false;
				    return;
			    }
			    auto column = filter.column_indexes.empty() ? filter_column : filter.column_indexes[index];
			    auto sample_column = GetSampleColumn(column);
			    valid = valid && sample_column.IsValid();
			    ref.IndexMutable() = sample_column.IsValid() ? sample_column.GetIndex() : 0;
		    });
		return valid ? std::move(result) : nullptr;
	}

	//! A filter operator on top of the scan references the scan's column bindings
	unique_ptr<Expression> BindFilter(const Expression &filter) {
		if (filter.IsVolatile()) {
			return nullptr;
		}
		auto result = filter.Copy();
		bool valid = true;
		ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
		    result, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &expr) {
			    auto &binding = colref.Binding();
			    if (colref.Depth() > 0 || binding.table_index != get.table_index) {
				    valid = false;
				    return;
			    }
			    auto sample_column = GetSampleColumn(binding.column_index);
			    if (!sample_column.IsValid()) {
				    valid = false;
				    return;
			    }
			    expr = make_uniq<BoundReferenceExpression>(colref.GetReturnType(), sample_column.GetIndex());
		    });
		return valid ? std::move(result) : nullptr;
	}

	const vector<ColumnIndex> &GetColumns() const {
		return columns;
	}

	//! The sample column of a column of the scan's projection, added to the sample if needed
	optional_idx GetSampleColumn(ProjectionIndex projection) {
		if (projection.GetIndex() >= get.GetColumnIds().size()) {
			return optional_idx();
		}
		auto &column = get.GetColumnIndex(projection);
		for (idx_t i = 0; i < columns.size(); i++) {
			if (columns[i] == column) {
				return i;
			}
		}
		columns.push_back(column);
		return columns.size() - 1;
	}

private:
	LogicalGet &get;
	vector<ColumnIndex> columns;
};

} // namespace

double RelationStatisticsHelper::SampledSelectivity(idx_t passing_count, idx_t evaluated_count) {
	D_ASSERT(evaluated_count > 0 && passing_count <= evaluated_count);
	// a filter that no sampled row passes is taken to pass half a row, since the sample may have missed its rows
	auto passing = passing_count == 0 ? 0.5 : static_cast<double>(passing_count);
	return passing / static_cast<double>(evaluated_count);
}

//! Extends a value range by the values of an integral vector
static void UpdateSampledRange(IntegralValueRange &range, Vector &values, idx_t count) {
	Vector bigints(LogicalType::BIGINT, count);
	string error;
	if (!VectorOperations::DefaultTryCast(values, bigints, count, &error)) {
		return;
	}
	for (auto entry : bigints.ValidValues<int64_t>()) {
		auto value = entry.GetValue();
		range.min = range.valid ? MinValue(range.min, value) : value;
		range.max = range.valid ? MaxValue(range.max, value) : value;
		range.valid = true;
	}
}

vector<idx_t>
RelationStatisticsHelper::SampleFilterPassCounts(ClientContext &context, LogicalGet &get,
                                                 const vector<vector<reference<const Expression>>> &filter_stages,
                                                 optional_ptr<vector<SampledColumnRange>> ranges) {
	auto sample_size = Settings::Get<FilterSelectivitySampleSizeSetting>(context);
	if (sample_size == 0 || !get.function.get_sample || !get.bind_data || !get.children.empty()) {
		return {};
	}
	SampleFilterBinder binder(get);
	vector<vector<unique_ptr<Expression>>> stages(filter_stages.size() + 1);
	for (auto &entry : get.table_filters) {
		if (ExpressionFilter::IsOptionalFilter(entry.Filter())) {
			continue;
		}
		auto &filter = ExpressionFilter::GetExpressionFilter(entry.Filter(), "SampleFilterPassCounts");
		auto expr = binder.BindTableFilter(entry.GetIndex(), filter);
		if (!expr) {
			return {};
		}
		stages[0].push_back(std::move(expr));
	}
	for (idx_t stage_idx = 0; stage_idx < filter_stages.size(); stage_idx++) {
		for (auto &filter : filter_stages[stage_idx]) {
			auto expr = binder.BindFilter(filter.get());
			if (!expr) {
				return {};
			}
			stages[stage_idx + 1].push_back(std::move(expr));
		}
	}
	if (binder.GetColumns().empty()) {
		return {};
	}
	vector<optional_idx> range_columns;
	if (ranges) {
		for (auto &range : *ranges) {
			range.range = IntegralValueRange();
			range_columns.push_back(binder.GetSampleColumn(range.column));
		}
	}
	TableFunctionGetSampleInput input(get.bind_data.get(), binder.GetColumns(), sample_size);
	auto sample = get.function.get_sample(context, input);
	if (!sample) {
		return {};
	}

	vector<idx_t> result(stages.size() + 1, 0);
	try {
		vector<vector<unique_ptr<ExpressionExecutor>>> executors(stages.size());
		for (idx_t stage_idx = 0; stage_idx < stages.size(); stage_idx++) {
			for (auto &expr : stages[stage_idx]) {
				executors[stage_idx].push_back(make_uniq<ExpressionExecutor>(context, *expr));
			}
		}
		ColumnDataScanState scan_state;
		DataChunk chunk;
		sample->InitializeScan(scan_state);
		sample->InitializeScanChunk(scan_state, chunk);
		while (sample->Scan(scan_state, chunk)) {
			result[0] += chunk.size();
			for (idx_t stage_idx = 0; stage_idx < executors.size(); stage_idx++) {
				for (auto &executor : executors[stage_idx]) {
					if (chunk.size() == 0) {
						break;
					}
					SelectionVector sel(STANDARD_VECTOR_SIZE);
					auto count = executor->SelectExpression(chunk, sel);
					chunk.Slice(sel, count);
				}
				result[stage_idx + 1] += chunk.size();
			}
			for (idx_t i = 0; i < range_columns.size() && chunk.size() > 0; i++) {
				if (range_columns[i].IsValid()) {
					UpdateSampledRange((*ranges)[i].range, chunk.data[range_columns[i].GetIndex()], chunk.size());
				}
			}
		}
	} catch (InternalException &) {
		throw;
	} catch (...) {
		// a filter can fail on a sampled row that the query itself never evaluates it on
		return {};
	}
	return result;
}

optional<double> RelationStatisticsHelper::SampleFilterSelectivity(ClientContext &context, LogicalFilter &filter) {
	// the filters between this one and the scan are evaluated first, as they are below it in the plan
	vector<vector<reference<const Expression>>> stages;
	reference<LogicalOperator> op = filter;
	while (op.get().type == LogicalOperatorType::LOGICAL_FILTER) {
		stages.emplace_back();
		for (auto &expr : op.get().expressions) {
			stages.back().push_back(*expr);
		}
		op = *op.get().children[0];
	}
	if (op.get().type != LogicalOperatorType::LOGICAL_GET) {
		return {};
	}
	std::reverse(stages.begin(), stages.end());
	auto counts = SampleFilterPassCounts(context, op.get().Cast<LogicalGet>(), stages);
	if (counts.size() < 2) {
		return {};
	}
	auto evaluated_count = counts[counts.size() - 2];
	if (evaluated_count == 0) {
		return {};
	}
	return SampledSelectivity(counts.back(), evaluated_count);
}

} // namespace duckdb
