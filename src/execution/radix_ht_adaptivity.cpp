#include "duckdb/execution/radix_ht_adaptivity.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/execution/radix_ht_sink_state.hpp"

namespace duckdb {

bool RadixHTAdaptivity::TryGrow(RadixHTGlobalSinkState &gstate, RadixHTLocalSinkState &lstate) {
	if (gstate.number_of_threads <= RadixHTConfig::GROW_STRATEGY_THREAD_THRESHOLD || gstate.external) {
		return false;
	}
	auto &ht = *lstate.ht;
	const auto &adaptivity = ht.GetAdaptivityState();
	const auto materialized_count = ht.GetMaterializedCount();
	if (!adaptivity.HLLEnabled() || ht.Count() == materialized_count ||
	    (gstate.spill_plan && gstate.StatePressureExceeded(ht))) {
		return false;
	}

	const auto hll_count = MinValue(adaptivity.GetHLLUpperBound(), materialized_count);
	if (hll_count == 0) {
		return false;
	}

	if (ht.Capacity() > NumericLimits<idx_t>::Maximum() / (3 * sizeof(ht_entry_t))) {
		return false;
	}
	const auto minimum_capacity = ht.Capacity() * 2;
	auto next_capacity = MaxValue(GroupedAggregateHashTable::GetCapacityForCount(hll_count), minimum_capacity);
	if (next_capacity > NumericLimits<idx_t>::Maximum() / sizeof(ht_entry_t)) {
		return false;
	}
	const auto current_size = ht.GetSizeInBytes();
	const auto table_size = next_capacity * sizeof(ht_entry_t);
	// Grow only when repeated tuple materialization outweighs the larger pointer table.
	if (static_cast<double>(materialized_count - hll_count) * static_cast<double>(ht.GetLayout().GetRowWidth()) <
	    GROWTH_DUPLICATE_BYTES_FACTOR * static_cast<double>(table_size)) {
		return false;
	}

	if (table_size > NumericLimits<idx_t>::Maximum() - current_size) {
		return false;
	}
	// The old pointer table remains allocated until the new allocation succeeds.
	const auto desired_size = current_size + table_size;
	if (desired_size > gstate.GetThreadLimit() && desired_size <= gstate.memory_limit / gstate.number_of_threads) {
		const annotated_lock_guard<annotated_mutex> guard {gstate.lock};
		auto &memory_state = *gstate.temporary_memory_state;
		const auto request = desired_size * gstate.number_of_threads;
		const auto doubled_request = request <= NumericLimits<idx_t>::Maximum() / 2 ? request * 2 : request;
		memory_state.SetRemainingSizeAndUpdateReservation(gstate.context,
		                                                  MaxValue(memory_state.GetRemainingSize(), doubled_request));
	}
	const auto thread_limit = gstate.GetThreadLimit();
	while (next_capacity >= minimum_capacity) {
		if (current_size <= thread_limit && next_capacity <= (thread_limit - current_size) / sizeof(ht_entry_t)) {
			break;
		}
		next_capacity /= 2;
	}
	if (next_capacity < minimum_capacity) {
		return false;
	}

	gstate.any_abandoned = true;
	ht.Abandon();
	try {
		ht.Resize(next_capacity);
	} catch (const OutOfMemoryException &) {
		// Other operators may consume the reservation before the allocation succeeds.
		ht.EnableHLL(false);
		return false;
	}
	ht.ResumeLookups();
	lstate.local_sink_capacity = next_capacity;
	return true;
}

void RadixHTAdaptivity::MaybeSkipLookups(RadixHTGlobalSinkState &gstate, RadixHTLocalSinkState &lstate) {
	auto &ht = *lstate.ht;
	const auto &adaptivity = ht.GetAdaptivityState();
	if (gstate.external || !adaptivity.HLLEnabled() || adaptivity.LookupsSkipped() ||
	    gstate.number_of_threads <= RadixHTConfig::GROW_STRATEGY_THREAD_THRESHOLD ||
	    adaptivity.GetSinkCount() < LOOKUP_SAMPLE_SIZE || ht.Capacity() != gstate.config.sink_capacity) {
		return;
	}

	const auto cycle_input = adaptivity.GetCycleInputCount();
	D_ASSERT(cycle_input >= ht.Count());
	const auto reduction =
	    cycle_input ? static_cast<double>(cycle_input - ht.Count()) / static_cast<double>(cycle_input) : 0;
	if (reduction < MINIMUM_SMALL_TABLE_REDUCTION) {
		ht.SkipLookups();
	}
}

void RadixHTAdaptivity::MaybeResumeLookups(RadixHTGlobalSinkState &gstate, RadixHTLocalSinkState &lstate) {
	auto &ht = *lstate.ht;
	const auto &adaptivity = ht.GetAdaptivityState();
	if (!gstate.external && !lstate.spilling && adaptivity.HLLEnabled() && adaptivity.LookupsSkipped() &&
	    adaptivity.GetSkippedInputCount() >= LOOKUP_SAMPLE_SIZE) {
		ht.ResumeLookups();
	}
}

} // namespace duckdb
