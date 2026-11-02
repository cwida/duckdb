#include "duckdb/parallel/progress_verifier.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/pipeline.hpp"

#include <cmath>

namespace duckdb {

//! Tolerance used when comparing progress fractions
static constexpr double PROGRESS_EPSILON = 1e-6;
//! Tolerance for a completed pipeline to count as 100% done
static constexpr double COMPLETION_EPSILON = 1e-3;
//! Minimum number of chunks before invalid progress or a missing 100% is flagged
static constexpr idx_t MIN_CHUNKS_SUPPORTED = 2;
//! Minimum number of rows before non-moving or inaccurate progress is flagged
static constexpr idx_t MIN_ROWS_GRANULARITY = 16384;
//! Maximum deviation between the source progress and the fraction of rows produced
static constexpr double MAX_INACCURACY = 0.5;

ProgressVerifier::ProgressVerifier(ClientContext &context, DebugProgressVerification mode, const string &ignore_str)
    : context(context), mode(mode) {
	for (auto &entry : StringUtil::Split(ignore_str, ',')) {
		auto trimmed = entry;
		StringUtil::Trim(trimmed);
		if (!trimmed.empty()) {
			ignore_list.insert(StringUtil::Upper(trimmed));
		}
	}
}

string ProgressVerifier::OperatorName(const PhysicalOperator &op) {
	if (op.type == PhysicalOperatorType::TABLE_SCAN) {
		// table functions implement progress individually - attribute to the function
		return StringUtil::Upper(op.Cast<PhysicalTableScan>().function.GetName().GetIdentifierName());
	}
	return PhysicalOperatorToString(op.type);
}

bool ProgressVerifier::FailsQuery(ProgressInvariant invariant) {
	// selective filters and skewed data legitimately make source progress deviate from the rows produced
	return invariant != ProgressInvariant::INACCURATE;
}

static bool IsWellFormed(const ProgressData &progress) {
	return std::isfinite(progress.done) && std::isfinite(progress.total) && progress.done >= 0 && progress.total >= 0 &&
	       progress.done <= progress.total * (1 + PROGRESS_EPSILON) + PROGRESS_EPSILON;
}

static double ProgressFraction(const ProgressData &progress) {
	if (progress.total <= 0) {
		return 0;
	}
	return MinValue<double>(progress.done / progress.total, 1.0);
}

static string FormatPercentage(double fraction) {
	return StringUtil::Format("%.1f%%", fraction * 100);
}

static string FormatProgress(const ProgressData &progress) {
	return StringUtil::Format("done=%g, total=%g", progress.done, progress.total);
}

ProgressVerifier::PipelineTrace &ProgressVerifier::GetTrace(Pipeline &pipeline) {
	lock_guard<mutex> guard(lock);
	auto entry = traces.find(&pipeline);
	if (entry != traces.end()) {
		return *entry->second;
	}
	auto trace = make_uniq<PipelineTrace>();
	trace->source_name = OperatorName(*pipeline.GetSource());
	trace->sink_name = pipeline.GetSink() ? OperatorName(*pipeline.GetSink()) : "NONE";
	auto &result = *trace;
	traces[&pipeline] = std::move(trace);
	return result;
}

void ProgressVerifier::Sample(Pipeline &pipeline, PipelineTrace &trace, bool is_final) {
	PipelineProgress progress;
	pipeline.GetDetailedProgress(progress);

	auto &run = trace.run;
	const auto sample_idx = run.samples.size();
	ProgressSample sample;
	sample.source_fraction = std::nan("");
	sample.fraction = std::nan("");
	sample.rows = run.rows;

	// invalid progress after completion never reaches the user - only flag it while the pipeline is running
	if (progress.source.invalid) {
		if (!is_final && !run.first_unsupported_source.IsValid()) {
			run.first_unsupported_source = sample_idx;
		}
	} else if (!IsWellFormed(progress.source)) {
		AddViolation(trace, ProgressInvariant::MALFORMED_SOURCE, trace.source_name, FormatProgress(progress.source));
	} else {
		sample.source_fraction = ProgressFraction(progress.source);
		if (progress.pipeline.invalid) {
			if (!is_final && !run.first_unsupported_sink.IsValid()) {
				run.first_unsupported_sink = sample_idx;
			}
		} else if (!IsWellFormed(progress.pipeline)) {
			AddViolation(trace, ProgressInvariant::MALFORMED_SINK, trace.sink_name,
			             StringUtil::Format("%s (source %s)", FormatProgress(progress.pipeline),
			                                FormatProgress(progress.source)));
		} else {
			sample.fraction = ProgressFraction(progress.pipeline);
		}
	}
	run.samples.push_back(sample);
	run.has_final_sample = is_final;
}

void ProgressVerifier::OnSourceChunk(Pipeline &pipeline, idx_t count) {
	auto &trace = GetTrace(pipeline);
	lock_guard<mutex> guard(trace.lock);
	trace.run.rows += count;
	trace.run.chunks++;
	Sample(pipeline, trace, false);
}

void ProgressVerifier::OnEarlyExit(Pipeline &pipeline) {
	auto &trace = GetTrace(pipeline);
	lock_guard<mutex> guard(trace.lock);
	trace.run.early_exit = true;
}

void ProgressVerifier::OnReset(Pipeline &pipeline) {
	optional_ptr<PipelineTrace> trace;
	{
		lock_guard<mutex> guard(lock);
		auto entry = traces.find(&pipeline);
		if (entry == traces.end()) {
			return;
		}
		trace = entry->second.get();
	}
	lock_guard<mutex> guard(trace->lock);
	if (trace->run.samples.empty() && !trace->run.early_exit) {
		return;
	}
	VerifyRun(*trace);
	trace->run = PipelineRun();
}

static optional_idx FindDecrease(const vector<double> &fractions) {
	double previous = -1;
	for (idx_t i = 0; i < fractions.size(); i++) {
		if (std::isnan(fractions[i])) {
			continue;
		}
		if (previous >= 0 && fractions[i] < previous - PROGRESS_EPSILON) {
			return i;
		}
		previous = fractions[i];
	}
	return optional_idx();
}

static double PreviousValid(const vector<double> &fractions, idx_t idx) {
	for (idx_t i = idx; i > 0; i--) {
		if (!std::isnan(fractions[i - 1])) {
			return fractions[i - 1];
		}
	}
	return 0;
}

//! Returns the number of valid fractions, and whether they all have the same value
static idx_t CountConstant(const vector<double> &fractions, bool &constant, double &value) {
	idx_t count = 0;
	double min_value = 2;
	double max_value = -1;
	for (auto fraction : fractions) {
		if (std::isnan(fraction)) {
			continue;
		}
		count++;
		min_value = MinValue(min_value, fraction);
		max_value = MaxValue(max_value, fraction);
	}
	constant = count > 0 && max_value - min_value < PROGRESS_EPSILON;
	value = min_value;
	return count;
}

void ProgressVerifier::VerifyRun(PipelineTrace &trace) {
	auto &run = trace.run;
	if (run.samples.empty()) {
		return;
	}
	vector<double> source_fractions;
	vector<double> fractions;
	for (auto &sample : run.samples) {
		source_fractions.push_back(sample.source_fraction);
		fractions.push_back(sample.fraction);
	}

	// progress must be reported while the pipeline is producing data
	if (run.chunks >= MIN_CHUNKS_SUPPORTED) {
		if (run.first_unsupported_source.IsValid()) {
			AddViolation(trace, ProgressInvariant::UNSUPPORTED_SOURCE, trace.source_name,
			             StringUtil::Format("invalid progress after %llu of %llu chunks",
			                                run.first_unsupported_source.GetIndex() + 1, run.chunks));
		}
		if (run.first_unsupported_sink.IsValid()) {
			AddViolation(trace, ProgressInvariant::UNSUPPORTED_SINK, trace.sink_name,
			             StringUtil::Format("invalid progress after %llu of %llu chunks",
			                                run.first_unsupported_sink.GetIndex() + 1, run.chunks));
		}
	}

	// progress must never decrease - attribute to the source if its own progress decreased
	auto source_decrease = FindDecrease(source_fractions);
	if (source_decrease.IsValid()) {
		auto idx = source_decrease.GetIndex();
		AddViolation(trace, ProgressInvariant::NON_MONOTONIC, trace.source_name,
		             StringUtil::Format("source progress decreased from %s to %s at sample %llu",
		                                FormatPercentage(PreviousValid(source_fractions, idx)),
		                                FormatPercentage(source_fractions[idx]), idx + 1));
	} else {
		auto decrease = FindDecrease(fractions);
		if (decrease.IsValid()) {
			auto idx = decrease.GetIndex();
			AddViolation(trace, ProgressInvariant::NON_MONOTONIC, trace.sink_name,
			             StringUtil::Format("pipeline progress decreased from %s to %s at sample %llu",
			                                FormatPercentage(PreviousValid(fractions, idx)),
			                                FormatPercentage(fractions[idx]), idx + 1));
		}
	}

	auto ran_to_completion = run.has_final_sample && !run.early_exit;
	// a pipeline that ran to completion must report 100%
	if (ran_to_completion && run.chunks >= MIN_CHUNKS_SUPPORTED) {
		auto &final_sample = run.samples.back();
		if (!std::isnan(final_sample.fraction) && final_sample.fraction < 1 - COMPLETION_EPSILON) {
			auto source_incomplete = final_sample.source_fraction < 1 - COMPLETION_EPSILON;
			AddViolation(trace, ProgressInvariant::INCOMPLETE, source_incomplete ? trace.source_name : trace.sink_name,
			             StringUtil::Format("finished at %s (source %s) after %llu chunks",
			                                FormatPercentage(final_sample.fraction),
			                                FormatPercentage(final_sample.source_fraction), run.chunks));
		}
	}

	if (run.rows < MIN_ROWS_GRANULARITY) {
		return;
	}
	// progress must move while the source produces data
	auto chunk_sample_count = run.has_final_sample ? run.samples.size() - 1 : run.samples.size();
	source_fractions.resize(chunk_sample_count);
	fractions.resize(chunk_sample_count);
	bool source_constant, constant;
	double source_value, value;
	auto source_count = CountConstant(source_fractions, source_constant, source_value);
	auto count = CountConstant(fractions, constant, value);
	// progress that reaches 100% too early is not stalled, but inaccurate
	source_constant = source_constant && source_value < 1 - COMPLETION_EPSILON;
	constant = constant && value < 1 - COMPLETION_EPSILON;
	if (source_count > 1 && source_constant) {
		AddViolation(trace, ProgressInvariant::STALLED, trace.source_name,
		             StringUtil::Format("source progress stuck at %s for %llu chunks", FormatPercentage(source_value),
		                                source_count));
	} else if (count > 1 && constant) {
		AddViolation(
		    trace, ProgressInvariant::STALLED, trace.sink_name,
		    StringUtil::Format("pipeline progress stuck at %s for %llu chunks", FormatPercentage(value), count));
	}

	// source progress should roughly follow the fraction of rows produced
	if (ran_to_completion && run.rows > 0) {
		double max_error = 0;
		idx_t max_idx = 0;
		for (idx_t i = 0; i < chunk_sample_count; i++) {
			auto &sample = run.samples[i];
			if (std::isnan(sample.source_fraction)) {
				continue;
			}
			auto actual = static_cast<double>(sample.rows) / static_cast<double>(run.rows);
			auto error = std::fabs(sample.source_fraction - actual);
			if (error > max_error) {
				max_error = error;
				max_idx = i;
			}
		}
		if (max_error > MAX_INACCURACY) {
			auto &sample = run.samples[max_idx];
			AddViolation(
			    trace, ProgressInvariant::INACCURATE, trace.source_name,
			    StringUtil::Format("source reported %s after %s of the rows (%llu of %llu)",
			                       FormatPercentage(sample.source_fraction),
			                       FormatPercentage(static_cast<double>(sample.rows) / static_cast<double>(run.rows)),
			                       sample.rows, run.rows));
		}
	}
}

void ProgressVerifier::AddViolation(const PipelineTrace &trace, ProgressInvariant invariant,
                                    const string &operator_name, const string &detail) {
	lock_guard<mutex> guard(lock);
	auto key = EnumUtil::ToString(invariant) + ":" + operator_name;
	if (reported.find(key) != reported.end()) {
		return;
	}
	reported.insert(key);
	Violation violation;
	violation.invariant = invariant;
	violation.operator_name = operator_name;
	violation.pipeline = trace.source_name + " -> " + trace.sink_name;
	violation.detail = detail;
	violations.push_back(std::move(violation));
}

bool ProgressVerifier::IsIgnored(const Violation &violation) const {
	auto invariant = EnumUtil::ToString(violation.invariant);
	return ignore_list.count(invariant) > 0 || ignore_list.count(invariant + ":" + violation.operator_name) > 0 ||
	       ignore_list.count("*:" + violation.operator_name) > 0;
}

string ProgressVerifier::Finalize(const vector<shared_ptr<Pipeline>> &pipelines) {
	for (auto &pipeline : pipelines) {
		optional_ptr<PipelineTrace> trace;
		{
			lock_guard<mutex> guard(lock);
			auto entry = traces.find(pipeline.get());
			if (entry == traces.end()) {
				// the source of this pipeline never produced any data
				continue;
			}
			trace = entry->second.get();
		}
		lock_guard<mutex> guard(trace->lock);
		Sample(*pipeline, *trace, true);
	}
	vector<reference<PipelineTrace>> all_traces;
	{
		lock_guard<mutex> guard(lock);
		for (auto &entry : traces) {
			all_traces.push_back(*entry.second);
		}
	}
	for (auto &trace : all_traces) {
		lock_guard<mutex> guard(trace.get().lock);
		VerifyRun(trace.get());
		trace.get().run = PipelineRun();
	}

	lock_guard<mutex> guard(lock);
	string error;
	vector<string> failing_keys;
	for (auto &violation : violations) {
		auto invariant = EnumUtil::ToString(violation.invariant);
		if (mode != DebugProgressVerification::ERROR || !FailsQuery(violation.invariant) || IsIgnored(violation)) {
			DUCKDB_LOG(context, ProgressVerificationLogType, invariant, violation.operator_name, violation.pipeline,
			           violation.detail);
			continue;
		}
		error += StringUtil::Format("\n- %s [%s] in pipeline %s: %s", invariant, violation.operator_name,
		                            violation.pipeline, violation.detail);
		failing_keys.push_back(invariant + ":" + violation.operator_name);
	}
	violations.clear();
	if (error.empty()) {
		return error;
	}
	return "Progress verification failed:" + error + "\nIgnore with SET debug_verify_progress_ignore='" +
	       StringUtil::Join(failing_keys, ",") + "'";
}

} // namespace duckdb
