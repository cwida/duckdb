#include "duckdb/execution/operator/helper/physical_streaming_sample.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/numeric_utils.hpp"

namespace duckdb {

PhysicalStreamingSample::PhysicalStreamingSample(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                                 unique_ptr<SampleOptions> options, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::STREAMING_SAMPLE, std::move(types), estimated_cardinality),
      sample_options(std::move(options)), percentage(0.0), system_sample_phase(0.0), rows(0) {
	if (sample_options->is_percentage) {
		percentage = sample_options->sample_size.GetValue<double>() / 100;
	} else {
		// Convert target row count to a sampling rate.
		// Prefer the pre-calculated sample_rate from the planner if available (ensures
		// consistency with pushdown path), otherwise derive from estimated_cardinality.
		// Fallback to 1.0 (take all rows) if no estimate is available.
		rows = NumericCast<idx_t>(sample_options->sample_size.GetValue<int64_t>());
		if (sample_options->sample_rate > 0) {
			percentage = sample_options->sample_rate;
		} else if (estimated_cardinality > 0) {
			percentage = static_cast<double>(rows) / static_cast<double>(estimated_cardinality);
		} else {
			percentage = 1.0;
		}
		percentage = MinValue(1.0, MaxValue(0.0, percentage));
		RandomEngine random(sample_options->seed.IsValid() ? static_cast<int64_t>(sample_options->seed.GetIndex())
		                                                   : -1);
		system_sample_phase = random.NextRandom();
	}
}

//===--------------------------------------------------------------------===//
// Operator
//===--------------------------------------------------------------------===//
class StreamingSampleOperatorState : public OperatorState {
public:
	explicit StreamingSampleOperatorState(int64_t seed) : random(seed), system_rows_seen(0) {
	}

	RandomEngine random;

	// Counters for row-count SYSTEM sampling.
	idx_t system_rows_seen;
};

void PhysicalStreamingSample::SystemSamplePercent(DataChunk &input, DataChunk &result, OperatorState &state_p) const {
	// system sampling: we throw one dice per chunk
	auto &state = state_p.Cast<StreamingSampleOperatorState>();
	double rand = state.random.NextRandom();
	if (rand <= percentage) {
		// rand is smaller than sample_size: output chunk
		result.Reference(input);
	}
}

void PhysicalStreamingSample::SystemSampleRows(DataChunk &input, DataChunk &result, OperatorState &state_p) const {
	double rate = percentage;
	if (rate <= 0) {
		return;
	}
	if (rate >= 1) {
		result.Reference(input);
		return;
	}

	// Emit a row whenever rows_seen * rate crosses the next integer threshold.
	// Using a fresh multiply per row (rather than an accumulated sum) avoids
	// floating-point drift where repeated additions of rate never quite reach
	// a whole number (e.g. 10000 * 0.0001 < 1.0). The phase makes seeds affect
	// where the systematic sample starts.
	auto &state = state_p.Cast<StreamingSampleOperatorState>();
	idx_t result_count = 0;
	SelectionVector sel(input.size());
	for (idx_t i = 0; i < input.size(); i++) {
		auto before = std::floor(LossyNumericCast<double>(state.system_rows_seen) * rate + system_sample_phase);
		state.system_rows_seen++;
		auto after = std::floor(LossyNumericCast<double>(state.system_rows_seen) * rate + system_sample_phase);
		if (after > before) {
			sel.set_index(result_count++, i);
		}
	}
	if (result_count > 0) {
		result.Slice(input, sel, result_count);
	}
}

void PhysicalStreamingSample::BernoulliSample(DataChunk &input, DataChunk &result, OperatorState &state_p) const {
	// bernoulli sampling: we throw one dice per tuple
	// then slice the result chunk
	auto &state = state_p.Cast<StreamingSampleOperatorState>();
	idx_t result_count = 0;
	SelectionVector sel(STANDARD_VECTOR_SIZE);
	for (idx_t i = 0; i < input.size(); i++) {
		double rand = state.random.NextRandom();
		if (rand <= percentage) {
			sel.set_index(result_count++, i);
		}
	}
	if (result_count > 0) {
		result.Slice(input, sel, result_count);
	}
}

bool PhysicalStreamingSample::ParallelOperator() const {
	if (!sample_options->is_percentage) {
		// Row-count SYSTEM sampling must see the full input stream through a
		// single OperatorState so the rows_seen counter advances globally.
		return false;
	}
	return !sample_options->repeatable;
}

void PhysicalStreamingSample::InitializeOutputChunk(Allocator &allocator, DataChunk &chunk) const {
	// every output column only references or slices the input
	chunk.Initialize(allocator, types, vector<bool>(types.size(), false));
}

unique_ptr<OperatorState> PhysicalStreamingSample::GetOperatorState(ExecutionContext &context) const {
	if (!ParallelOperator()) {
		// Repeatable single thread: use the specified seed for deterministic results
		return make_uniq<StreamingSampleOperatorState>(static_cast<int64_t>(sample_options->seed.GetIndex()));
	}
	// Non-repeatable parallel: each thread gets a distinct random seed (duckdb#16223)
	RandomEngine random;
	return make_uniq<StreamingSampleOperatorState>(static_cast<int64_t>(random.NextRandomInteger64()));
}

OperatorResultType PhysicalStreamingSample::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                    GlobalOperatorState &gstate, OperatorState &state) const {
	switch (sample_options->method) {
	case SampleMethod::BERNOULLI_SAMPLE:
		BernoulliSample(input, chunk, state);
		break;
	case SampleMethod::SYSTEM_SAMPLE:
		if (sample_options->is_percentage) {
			SystemSamplePercent(input, chunk, state);
		} else {
			SystemSampleRows(input, chunk, state);
		}
		break;
	default:
		throw InternalException("Unsupported sample method for streaming sample");
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

InsertionOrderPreservingMap<string> PhysicalStreamingSample::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	if (sample_options->is_percentage) {
		result["Sample Method"] =
		    EnumUtil::ToString(sample_options->method) + ": " + StringUtil::ToString(100 * percentage) + "%";
	} else {
		result["Sample Method"] = EnumUtil::ToString(sample_options->method) + ": " + to_string(rows) + " rows";
	}
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
