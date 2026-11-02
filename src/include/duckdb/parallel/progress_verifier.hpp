//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/progress_verifier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/debug_progress_verification.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"

namespace duckdb {
class ClientContext;
class PhysicalOperator;
class Pipeline;
struct PipelineProgress;

enum class ProgressInvariant : uint8_t {
	//! The source reported invalid progress while it was producing data
	UNSUPPORTED_SOURCE,
	//! The sink turned valid source progress into invalid progress
	UNSUPPORTED_SINK,
	//! The source reported progress that is not finite or outside of [0, total]
	MALFORMED_SOURCE,
	//! The sink reported progress that is not finite or outside of [0, total]
	MALFORMED_SINK,
	//! The progress of the pipeline decreased
	NON_MONOTONIC,
	//! The pipeline ran to completion but did not report 100% progress
	INCOMPLETE,
	//! The progress of the pipeline did not change while it produced many chunks
	STALLED,
	//! The source progress deviates strongly from the fraction of rows produced (log-only)
	INACCURATE
};

//! The ProgressVerifier samples the progress of every pipeline after each source chunk and at the end of the query,
//! and verifies that the reported progress is well-formed, monotonic, moving and complete
class ProgressVerifier {
public:
	ProgressVerifier(ClientContext &context, DebugProgressVerification mode, const string &ignore_list);

public:
	//! Samples the progress of a pipeline after a non-empty source chunk was pushed through the pipeline
	void OnSourceChunk(Pipeline &pipeline, idx_t count);
	//! Called when a pipeline executor finished without exhausting its source (e.g. because of a LIMIT)
	void OnEarlyExit(Pipeline &pipeline);
	//! Called when the source or sink state of a pipeline is reset - this starts a new run of the pipeline
	void OnReset(Pipeline &pipeline);
	//! Takes a final sample of the given (completed) pipelines and verifies all traces
	//! Returns an error message if any violation that fails the query was found
	string Finalize(const vector<shared_ptr<Pipeline>> &pipelines);

	//! The name that is used to attribute a violation to an operator
	static string OperatorName(const PhysicalOperator &op);

private:
	struct ProgressSample {
		//! Progress fraction reported by the source - or NaN if the source progress was invalid
		double source_fraction;
		//! Progress fraction of the pipeline - or NaN if the pipeline progress was invalid
		double fraction;
		//! Rows produced by the source at the time of the sample
		idx_t rows;
	};
	struct PipelineRun {
		vector<ProgressSample> samples;
		idx_t rows = 0;
		idx_t chunks = 0;
		bool early_exit = false;
		bool has_final_sample = false;
		optional_idx first_unsupported_source;
		optional_idx first_unsupported_sink;
	};
	struct PipelineTrace {
		//! Held while sampling, so samples are recorded in the order in which they were taken
		mutex lock;
		string source_name;
		string sink_name;
		PipelineRun run;
	};
	struct Violation {
		ProgressInvariant invariant;
		string operator_name;
		string pipeline;
		string detail;
	};

private:
	PipelineTrace &GetTrace(Pipeline &pipeline);
	void Sample(Pipeline &pipeline, PipelineTrace &trace, bool is_final);
	void VerifyRun(PipelineTrace &trace);
	void AddViolation(const PipelineTrace &trace, ProgressInvariant invariant, const string &operator_name,
	                  const string &detail);
	bool IsIgnored(const Violation &violation) const;
	static bool FailsQuery(ProgressInvariant invariant);

private:
	ClientContext &context;
	DebugProgressVerification mode;
	unordered_set<string> ignore_list;

	//! Protects the traces map and the violations
	mutex lock;
	unordered_map<const Pipeline *, unique_ptr<PipelineTrace>> traces;
	vector<Violation> violations;
	unordered_set<string> reported;
};

} // namespace duckdb
