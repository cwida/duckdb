#include "duckdb/execution/operator/helper/physical_limit.hpp"
#include "duckdb/execution/operator/helper/physical_streaming_limit.hpp"
#include "duckdb/execution/operator/helper/physical_reservoir_sample.hpp"
#include "duckdb/execution/operator/helper/physical_streaming_sample.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/operator/logical_sample.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/exception/parser_exception.hpp"

namespace duckdb {

PhysicalOperator &PhysicalPlanGenerator::CreatePlan(LogicalSample &op) {
	D_ASSERT(op.children.size() == 1);
	// Only reached when a LogicalSample survives optimization. Sampling pushdown removes
	// LogicalSample over a plain table GET, so that path uses LogicalLimit + scan instead
	// and never hits the row-count LIMIT wrap below for the same sample.

	// For SYSTEM_SAMPLE with row count, we need to get the child's estimated cardinality
	// BEFORE calling CreatePlan (which consumes the child).
	idx_t child_cardinality = 0;
	if (op.sample_options->method == SampleMethod::SYSTEM_SAMPLE && !op.sample_options->is_percentage) {
		auto &first_child = *op.children[0];
		child_cardinality = first_child.EstimateCardinality(context);
	}

	auto &plan = CreatePlan(*op.children[0]);
	if (!op.sample_options->seed.IsValid()) {
		auto &random_engine = RandomEngine::Get(context);
		op.sample_options->SetSeed(random_engine.NextSeed32());
	}

	switch (op.sample_options->method) {
	case SampleMethod::RESERVOIR_SAMPLE: {
		auto &sample = Make<PhysicalReservoirSample>(op.types, std::move(op.sample_options), op.estimated_cardinality);
		sample.children.push_back(plan);
		return sample;
	}
	case SampleMethod::BERNOULLI_SAMPLE: {
		if (!op.sample_options->is_percentage) {
			throw ParserException("Sample method %s cannot be used with a discrete sample count, either switch to "
			                      "reservoir sampling or use a sample_size",
			                      EnumUtil::ToString(op.sample_options->method));
		}
		auto &sample = Make<PhysicalStreamingSample>(op.types, std::move(op.sample_options), op.estimated_cardinality);
		sample.children.push_back(plan);
		return sample;
	}
	case SampleMethod::SYSTEM_SAMPLE: {
		const bool is_percentage = op.sample_options->is_percentage;
		int64_t rows = 0;
		if (!is_percentage) {
			rows = op.sample_options->sample_size.GetValue<int64_t>();
			// To ensure consistency between optimized and unoptimized paths,
			// we calculate the rate based on the estimated cardinality of the child.
			if (child_cardinality > 0) {
				op.sample_options->sample_rate = static_cast<double>(rows) / static_cast<double>(child_cardinality);
			} else {
				op.sample_options->sample_rate = 1.0;
			}
		}

		auto &sample = Make<PhysicalStreamingSample>(op.types, std::move(op.sample_options), op.estimated_cardinality);
		sample.children.push_back(plan);

		if (!is_percentage) {
			// Mirror sampling_pushdown.cpp: cap row count when LogicalSample is still present.
			// As the sampling operator uses a distributed chunk-based approach it may
			// oversample, so we wrap it with a LIMIT to ensure we stop as soon as the target is reached
			// This also happens when no estimated cardinality is available.
			auto limit_val = BoundLimitNode::ConstantValue(rows);
			auto offset_val = BoundLimitNode();
			// PhysicalLimit requires batch-index support from the pipeline source.
			// Sources like CTE scans don't provide it, so fall back to a streaming
			// limit which has no such requirement.
			const bool preserve_order = PreserveInsertionOrder(sample);
			if (preserve_order && UseBatchIndex(sample)) {
				auto &limit = Make<PhysicalLimit>(op.types, std::move(limit_val), std::move(offset_val),
				                                  op.estimated_cardinality);
				limit.children.push_back(sample);
				return limit;
			}
			auto &limit = Make<PhysicalStreamingLimit>(op.types, std::move(limit_val), std::move(offset_val),
			                                           op.estimated_cardinality, !preserve_order);
			limit.children.push_back(sample);
			return limit;
		}
		return sample;
	}
	default:
		throw InternalException("Unimplemented sample method");
	}
}

} // namespace duckdb
