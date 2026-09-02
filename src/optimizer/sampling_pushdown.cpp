#include "duckdb/optimizer/sampling_pushdown.hpp"
#include "duckdb/optimizer/sampling_pushdown_registry.hpp"
#include "duckdb/common/random_engine.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_sample.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"

namespace duckdb {

static bool CanPushSampling(ClientContext &context, const LogicalGet &get, const SampleOptions &sample_options) {
	if (auto supports_sampling_pushdown =
	        SamplingPushdownRegistry::Lookup(context, get.function.GetName().GetIdentifierName())) {
		if (!get.bind_data) {
			return false;
		}
		return supports_sampling_pushdown(*get.bind_data, sample_options);
	}
	return get.function.sampling_pushdown;
}

unique_ptr<LogicalOperator> SamplingPushdown::Optimize(unique_ptr<LogicalOperator> op) {
	if (op->type == LogicalOperatorType::LOGICAL_SAMPLE && !op->children.empty() &&
	    op->children[0]->type == LogicalOperatorType::LOGICAL_GET && op->children[0]->children.empty()) {
		auto &sample_op = op->Cast<LogicalSample>();
		auto &get = op->children[0]->Cast<LogicalGet>();
		const auto &sample_options = *sample_op.sample_options;
		const bool has_filters = get.table_filters.HasFilters() || get.dynamic_filters;
		const bool can_push_system_sample = sample_options.method == SampleMethod::SYSTEM_SAMPLE && !has_filters &&
		                                    CanPushSampling(context, get, sample_options);

		if (can_push_system_sample) {
			const bool is_row_count_sampling = !sample_options.is_percentage;
			int64_t row_limit = 0;
			if (is_row_count_sampling) {
				if (!sample_op.sample_options->seed.IsValid()) {
					auto &random_engine = RandomEngine::Get(context);
					sample_op.sample_options->SetSeed(random_engine.NextRandomInteger());
				}
				// For row-count sampling, calculate the sampling rate based on estimated cardinality.
				// Use EstimateCardinality which can query table function stats if has_estimated_cardinality is not set.
				row_limit = sample_options.sample_size.GetValue<int64_t>();
				const idx_t estimated_card = get.EstimateCardinality(context);
				if (estimated_card > 0) {
					sample_op.sample_options->sample_rate =
					    static_cast<double>(row_limit) / static_cast<double>(estimated_card);
				} else {
					sample_op.sample_options->sample_rate = 1.0;
				}
			}

			get.extra_info.sample_options = std::move(sample_op.sample_options);
			op = std::move(op->children[0]);

			if (is_row_count_sampling) {
				// Wrap with LIMIT to ensure exact row count and enable early stopping.
				// The pushdown sampling may oversample due to the distributed chunk-based
				// approach, so LIMIT ensures we stop as soon as the target is reached.
				auto limit = make_uniq<LogicalLimit>(BoundLimitNode::ConstantValue(row_limit), BoundLimitNode());
				limit->children.push_back(std::move(op));
				op = std::move(limit);
			}
		}
	}
	for (auto &child : op->children) {
		child = Optimize(std::move(child));
	}
	return op;
}

} // namespace duckdb
