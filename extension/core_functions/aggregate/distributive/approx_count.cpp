#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hyperloglog.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "core_functions/aggregate/distributive_functions.hpp"

namespace duckdb {

// Algorithms from
// "New cardinality estimation algorithms for HyperLogLog sketches"
// Otmar Ertl, arXiv:1702.01284
namespace {

using ApproxCountDistinctHLL = HyperLogLogP<10>;

struct ApproxDistinctCountState {
	ApproxCountDistinctHLL hll;
};

struct ApproxCountDistinctFunction {
	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		target.hll.Merge(source.hll);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		target = UnsafeNumericCast<T>(state.hll.Count());
	}

	static bool IgnoreNull() {
		return true;
	}
};

void ApproxCountDistinctUpdateFunction(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &state_vector,
                                       idx_t count) {
	D_ASSERT(input_count == 1);
	auto &input = inputs[0];

	auto input_validity = input.Validity();

	if (count > STANDARD_VECTOR_SIZE) {
		throw InternalException("ApproxCountDistinct - count must be at most vector size");
	}
	Vector hash_vec(LogicalType::HASH, count);
	VectorOperations::Hash(input, hash_vec, count);

	auto states = state_vector.Values<ApproxDistinctCountState *>();
	auto hashes = hash_vec.Values<hash_t>();
	for (idx_t i = 0; i < count; i++) {
		if (!input_validity.IsValid(i)) {
			continue;
		}
		auto agg_state = states[i].GetValue();
		const auto hash = hashes[i].GetValue();
		agg_state->hll.InsertElement(hash);
	}
}

//===--------------------------------------------------------------------===//
// State Export
//===--------------------------------------------------------------------===//
//! Exported state: STRUCT(precision UTINYINT, registers UTINYINT[]) - the sketch itself. A HyperLogLog is a fixed
//! array of 2^precision one-byte registers, and merging two sketches takes the per-register maximum, so an exported
//! state combines (combine_aggr) and finalizes exactly as the in-memory one does. The static field layouts describe
//! scalars and linked lists, not a fixed array, hence the explicit callbacks.
LogicalType ApproxCountDistinctExportType() {
	child_list_t<LogicalType> children;
	children.emplace_back("precision", LogicalType::UTINYINT);
	children.emplace_back("registers", LogicalType::LIST(LogicalType::UTINYINT));
	return LogicalType::STRUCT(std::move(children));
}

//! The shape of the exported state: STRUCT(precision, registers UTINYINT[])
using APPROX_COUNT_DISTINCT_EXPORT_TYPE = VectorStructType<uint8_t, VectorListType<uint8_t>>;

AggregateStateLayout ApproxCountDistinctGetStateType(AggregateLayoutInput &) {
	AggregateStateLayout layout;
	layout.type = ApproxCountDistinctExportType();
	layout.total_state_size = AlignValue<idx_t>(sizeof(ApproxDistinctCountState));
	return layout;
}

void ApproxCountDistinctExportState(Vector &state_vector, AggregateFinalizeInputData &, Vector &result, idx_t count,
                                    idx_t offset) {
	auto states = state_vector.Values<ApproxDistinctCountState *>();
	auto writer = FlatVector::Writer<APPROX_COUNT_DISTINCT_EXPORT_TYPE>(result, count, offset);
	for (idx_t i = 0; i < count; i++) {
		// an empty sketch (all registers zero) is exported as-is rather than as NULL: finalize does not import NULL
		// states, and approx_count_distinct of no values is 0, not NULL
		auto &hll = states[i].GetValue()->hll;
		writer.WriteValue([&](auto &precision_writer, auto &registers_writer) {
			precision_writer.WriteValue(UnsafeNumericCast<uint8_t>(ApproxCountDistinctHLL::PRECISION));
			idx_t register_idx = 0;
			for (auto &register_writer : registers_writer.WriteList(ApproxCountDistinctHLL::M)) {
				register_writer.WriteValue(hll.GetRegister(register_idx++));
			}
		});
	}
}

void ApproxCountDistinctImportState(AggregateImportInputData &input) {
	const auto &layout = input.layout;
	const auto &input_vec = input.input_vec;
	const auto count = input_vec.size();
	const auto dest_buffer = input.dest_buffer;
	auto entries = input_vec.Values<APPROX_COUNT_DISTINCT_EXPORT_TYPE>();
	for (idx_t i = 0; i < count; i++) {
		auto &state = *reinterpret_cast<ApproxDistinctCountState *>(dest_buffer + i * layout.total_state_size);
		state.hll = ApproxCountDistinctHLL();
		const auto entry = entries[i];
		if (!entry.IsValid()) {
			// NULL input (e.g. to_aggregate_state(NULL, ...)) - leave the sketch empty
			continue;
		}
		const auto precision_entry = entry.template GetChildValue<0>();
		const auto register_list = entry.template GetChildValue<1>();
		if (!precision_entry.IsValid() || !register_list.IsValid()) {
			throw InvalidInputException("Invalid approx_count_distinct state - the state fields cannot be NULL");
		}
		if (precision_entry.GetValue() != ApproxCountDistinctHLL::PRECISION) {
			throw InvalidInputException("Invalid approx_count_distinct state - expected precision %d, got %d",
			                            ApproxCountDistinctHLL::PRECISION, precision_entry.GetValue());
		}
		if (register_list.GetListLength() != ApproxCountDistinctHLL::M) {
			throw InvalidInputException("Invalid approx_count_distinct state - expected %llu registers, got %llu",
			                            ApproxCountDistinctHLL::M, register_list.GetListLength());
		}
		idx_t register_idx = 0;
		for (const auto register_entry : register_list.GetChildValues()) {
			if (!register_entry.IsValid()) {
				throw InvalidInputException("Invalid approx_count_distinct state - the registers cannot be NULL");
			}
			const auto value = register_entry.GetValue();
			// a register holds the position of the first set bit of a hash, which is at most Q + 1
			if (value > ApproxCountDistinctHLL::Q + 1) {
				throw InvalidInputException("Invalid approx_count_distinct state - register value %d exceeds %d", value,
				                            ApproxCountDistinctHLL::Q + 1);
			}
			state.hll.SetRegister(register_idx++, value);
		}
	}
}

AggregateFunction GetApproxCountDistinctFunction(const LogicalType &input_type) {
	auto fun = AggregateFunction(
	    {}, LogicalTypeId::BIGINT, AggregateFunction::StateSize<ApproxDistinctCountState>,
	    AggregateFunction::StateInitialize<ApproxDistinctCountState, ApproxCountDistinctFunction>,
	    ApproxCountDistinctUpdateFunction,
	    AggregateFunction::StateCombine<ApproxDistinctCountState, ApproxCountDistinctFunction>,
	    AggregateFunction::StateFinalize<ApproxDistinctCountState, int64_t, ApproxCountDistinctFunction>, nullptr);
	fun.GetSignature().AddParameter("any", input_type);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetStateExportCallbacks(ApproxCountDistinctGetStateType, ApproxCountDistinctExportState,
	                            ApproxCountDistinctImportState);
	return fun;
}

} // namespace

AggregateFunction ApproxCountDistinctFun::GetFunction() {
	return GetApproxCountDistinctFunction(LogicalType::ANY);
}

} // namespace duckdb
