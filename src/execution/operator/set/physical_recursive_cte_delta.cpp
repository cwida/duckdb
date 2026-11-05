#include "duckdb/execution/operator/set/physical_recursive_cte_delta.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte_keyed_state.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

static vector<LogicalType> GetPreviousStateTypes(const PhysicalRecursiveCTE &op) {
	vector<LogicalType> types {LogicalType::POINTER};
	types.insert(types.end(), op.payload_types.begin(), op.payload_types.end());
	return types;
}

void RecursiveCTEGroupAddressSet::Reset() {
	for (const auto slot : occupied_slots) {
		entries[slot] = nullptr;
	}
	occupied_slots.clear();
}

void RecursiveCTEGroupAddressSet::Reserve(idx_t count_p) {
	if (count_p == 0) {
		return;
	}
	idx_t capacity = 8;
	while (capacity / 2 < count_p) {
		capacity *= 2;
	}
	if (capacity > entries.size()) {
		Resize(capacity);
	}
}

void RecursiveCTEGroupAddressSet::Resize(idx_t capacity) {
	D_ASSERT(capacity >= 8 && (capacity & (capacity - 1)) == 0);
	auto old_entries = std::move(entries);
	auto old_occupied_slots = std::move(occupied_slots);
	entries.assign(capacity, nullptr);
	occupied_slots.clear();
	occupied_slots.reserve(old_occupied_slots.size());
	for (const auto slot : old_occupied_slots) {
		D_ASSERT(old_entries[slot]);
		Insert(old_entries[slot]);
	}
}

bool RecursiveCTEGroupAddressSet::Insert(data_ptr_t address) {
	D_ASSERT(address);
	if (entries.empty() || occupied_slots.size() + 1 > entries.size() / 2) {
		Resize(entries.empty() ? 8 : entries.size() * 2);
	}
	auto entry = Hash(CastPointerToValue(address)) & (entries.size() - 1);
	while (entries[entry]) {
		if (entries[entry] == address) {
			return false;
		}
		entry = (entry + 1) & (entries.size() - 1);
	}
	entries[entry] = address;
	occupied_slots.push_back(entry);
	return true;
}

bool RecursiveCTEGroupAddressSet::Contains(data_ptr_t address) const {
	if (occupied_slots.empty()) {
		return false;
	}
	auto entry = Hash(CastPointerToValue(address)) & (entries.size() - 1);
	while (entries[entry]) {
		if (entries[entry] == address) {
			return true;
		}
		entry = (entry + 1) & (entries.size() - 1);
	}
	return false;
}

RecursiveCTEKeyDeltaState::RecursiveCTEKeyDeltaState(ClientContext &context, const PhysicalRecursiveCTE &op)
    : previous_rows(context, GetPreviousStateTypes(op)), new_keys(context, op.hash_key_types),
      matched_addresses(LogicalType::POINTER), finalize_addresses(LogicalType::POINTER),
      found_groups(STANDARD_VECTOR_SIZE), missing_groups(STANDARD_VECTOR_SIZE), changed_groups(STANDARD_VECTOR_SIZE),
      changed_column_groups(STANDARD_VECTOR_SIZE), equal_groups_a(STANDARD_VECTOR_SIZE),
      equal_groups_b(STANDARD_VECTOR_SIZE), arena(Allocator::Get(context)), row_state(arena) {
	selected_keys.Initialize(Allocator::Get(context), op.hash_key_types);
	aggregate_rows.Initialize(Allocator::Get(context), op.aggregate_types);
	updated_aggregate_rows.Initialize(Allocator::Get(context), op.aggregate_types);
	result_rows.Initialize(Allocator::Get(context), op.GetTypes());
	previous_state_rows.Initialize(Allocator::Get(context), GetPreviousStateTypes(op));
	changed_rows.Initialize(Allocator::Get(context), op.GetTypes());
	previous_scan_rows.Initialize(Allocator::Get(context), GetPreviousStateTypes(op));
	key_scan_rows.Initialize(Allocator::Get(context), op.hash_key_types);
	vector<LogicalType> comparison_types;
	comparison_types.reserve(op.payload_types.size() * 2);
	comparison_types.insert(comparison_types.end(), op.payload_types.begin(), op.payload_types.end());
	comparison_types.insert(comparison_types.end(), op.payload_types.begin(), op.payload_types.end());
	for (auto &comparison : op.payload_comparisons) {
		if (comparison) {
			comparison_rows.InitializeEmpty(comparison_types);
			break;
		}
	}
}

void RecursiveCTEKeyDeltaState::Reset() {
	touched_addresses.Reset();
	new_group_addresses.clear();
	new_group_address_set.Reset();
	new_group_address_set_built = false;
	collections_initialized = false;
	deferred_previous_rows = false;
	deferred_candidate_reuse = false;
	deferred_count = 0;
	touched_count = 0;
	new_count = 0;
	changed_count = 0;
}

void RecursiveCTEKeyDeltaState::PrepareCollections() {
	if (collections_initialized) {
		return;
	}
	previous_rows.ResetForReuse();
	previous_rows.InitializeAppend(previous_append_state);
	new_keys.ResetForReuse();
	new_keys.InitializeAppend(new_key_append_state);
	collections_initialized = true;
}

static idx_t SelectDistinctPayloadRows(const Vector &previous, const Vector &current,
                                       optional_ptr<SelectionVector> candidates, idx_t candidate_count,
                                       SelectionVector &changed, SelectionVector &unchanged) {
	if (!candidates) {
		return VectorOperations::DistinctFrom(previous, current, nullptr, candidate_count, &changed, &unchanged);
	}
	Vector previous_candidates(previous, *candidates, candidate_count);
	Vector current_candidates(current, *candidates, candidate_count);
	return VectorOperations::DistinctFrom(previous_candidates, current_candidates, candidates.get(), candidate_count,
	                                      &changed, &unchanged);
}

//! Whether the finalized payload of every row equals the candidate payload that produced it
static bool FinalizedPayloadsEqualCandidates(RecursiveCTEKeyDeltaState &delta, const PhysicalRecursiveCTE &op,
                                             DataChunk &finalized, DataChunk &candidates, idx_t count) {
	for (idx_t payload_idx = 0; payload_idx < op.payload_idx.size(); payload_idx++) {
		const auto mismatch_count =
		    VectorOperations::DistinctFrom(finalized.data[payload_idx], candidates.data[op.payload_idx[payload_idx]],
		                                   nullptr, count, &delta.changed_column_groups, &delta.equal_groups_a);
		if (mismatch_count != 0) {
			return false;
		}
	}
	return true;
}

//! Rows whose payload differs from the previous payload in some column; previous_payload_offset selects the
//! first previous payload column, as the layouts of the previous rows differ between callers
static idx_t CountChangedPayloadRows(RecursiveCTEKeyDeltaState &delta, const PhysicalRecursiveCTE &op,
                                     DataChunk &previous, idx_t previous_payload_offset, DataChunk &current,
                                     idx_t count) {
	idx_t changed_count = 0;
	idx_t equal_count = count;
	optional_ptr<SelectionVector> equal_groups;
	for (idx_t payload_idx = 0; payload_idx < op.payload_idx.size() && equal_count > 0; payload_idx++) {
		auto &next_equal_groups = payload_idx % 2 == 0 ? delta.equal_groups_a : delta.equal_groups_b;
		const auto changed_column_count =
		    SelectDistinctPayloadRows(previous.data[previous_payload_offset + payload_idx], current.data[payload_idx],
		                              equal_groups, equal_count, delta.changed_column_groups, next_equal_groups);
		changed_count += changed_column_count;
		equal_count -= changed_column_count;
		equal_groups = &next_equal_groups;
	}
	return changed_count;
}

static void CopySelectedAddresses(const Vector &source, const SelectionVector &selection, idx_t count, Vector &target) {
	source.Flatten();
	target.SetVectorType(VectorType::FLAT_VECTOR);
	auto source_data = FlatVector::GetData<data_ptr_t>(source);
	auto target_data = FlatVector::GetDataMutable<data_ptr_t>(target);
	for (idx_t address_idx = 0; address_idx < count; address_idx++) {
		target_data[address_idx] = source_data[selection.get_index(address_idx)];
	}
	FlatVector::SetSize(target, count);
}

void RecursiveCTEKeyedState::AppendPreviousUsingKeyDeltaRows(Vector &addresses, idx_t count) {
	D_ASSERT(key_delta);
	auto &delta = *key_delta;
	delta.PrepareCollections();
	delta.previous_state_rows.Reset();
	delta.previous_state_rows.data[0].Reference(addresses);
	for (idx_t payload_idx = 0; payload_idx < op.payload_idx.size(); payload_idx++) {
		delta.previous_state_rows.data[1 + payload_idx].Reference(delta.aggregate_rows.data[payload_idx]);
	}
	delta.previous_state_rows.CheckCardinality(count);
	delta.previous_rows.Append(delta.previous_append_state, delta.previous_state_rows);
}

void RecursiveCTEKeyedState::SnapshotExistingUsingKeyDeltaAddresses(Vector &addresses, idx_t count, bool defer_append) {
	D_ASSERT(key_delta);
	auto &delta = *key_delta;
	if (count == 0) {
		return;
	}
	CopySelectedAddresses(addresses, *FlatVector::IncrementalSelectionVector(), count, delta.finalize_addresses);
	FinalizeAggregateRows(delta.row_state, delta.finalize_addresses, delta.aggregate_rows, count);
	if (defer_append) {
		delta.deferred_previous_rows = true;
		delta.deferred_count = count;
		return;
	}
	AppendPreviousUsingKeyDeltaRows(addresses, count);
}

void RecursiveCTEKeyedState::SnapshotUsingKeyDelta(const Vector &group_addresses, const SelectionVector &new_groups,
                                                   idx_t new_group_count, idx_t row_count, bool allow_candidate_reuse) {
	D_ASSERT(key_delta);
	auto &delta = *key_delta;
	const auto candidate_count = intermediate_table.Count();
	const auto skip_new_group_addresses = allow_candidate_reuse && op.keyed.can_reuse_new_group_candidates &&
	                                      partial_key_indexes.empty() && new_group_count == candidate_count &&
	                                      row_count == candidate_count;
	if (skip_new_group_addresses) {
		D_ASSERT(partial_key_indexes.empty() && new_group_count == row_count);
		delta.new_count += new_group_count;
		delta.touched_count += new_group_count;
		return;
	}
	if (allow_candidate_reuse && op.keyed.can_reuse_new_group_candidates) {
		delta.ReserveNewGroupAddresses(candidate_count);
	}

	group_addresses.Flatten();
	auto source_addresses = FlatVector::GetData<data_ptr_t>(group_addresses);
	const auto update_partial_indexes = !partial_key_indexes.empty();
	auto partial_index_addresses =
	    update_partial_indexes ? FlatVector::GetDataMutable<data_ptr_t>(new_group_addresses) : nullptr;
	if (!update_partial_indexes && !delta.HasNewGroupAddressSet() && new_group_count == row_count) {
		delta.AppendNewGroupAddresses(source_addresses, new_group_count);
		delta.new_count += new_group_count;
		delta.touched_count += new_group_count;
		return;
	}
	for (idx_t new_group_idx = 0; new_group_idx < new_group_count; new_group_idx++) {
		const auto input_idx = new_groups.get_index_unsafe(new_group_idx);
		const auto address = source_addresses[input_idx];
		delta.AddNewGroupAddress(address);
		if (update_partial_indexes) {
			this->new_groups.set_index(new_group_idx, input_idx);
			partial_index_addresses[new_group_idx] = address;
		}
	}
	if (update_partial_indexes) {
		FlatVector::SetSize(new_group_addresses, new_group_count);
	}
	delta.new_count += new_group_count;
	delta.touched_count += new_group_count;
	if (new_group_count == row_count) {
		return;
	}

	auto matched_addresses = FlatVector::GetDataMutable<data_ptr_t>(delta.matched_addresses);
	idx_t matched_count = 0;
	if (allow_candidate_reuse && candidate_count == 1) {
		D_ASSERT(row_count == 1);
		matched_addresses[matched_count++] = source_addresses[0];
	} else {
		delta.EnsureNewGroupAddressSet();
		delta.touched_addresses.Reserve(delta.touched_count + row_count);
		for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
			const auto address = source_addresses[row_idx];
			if (!delta.touched_addresses.Insert(address) || delta.ContainsNewGroupAddress(address)) {
				continue;
			}
			matched_addresses[matched_count++] = address;
		}
	}
	delta.touched_count += matched_count;
	FlatVector::SetSize(delta.matched_addresses, matched_count);
	// Keep prior values vector-local while direct candidate reuse remains possible.
	const auto defer_append = allow_candidate_reuse && op.keyed.can_reuse_changed_group_candidates &&
	                          row_count == candidate_count && new_group_count == 0 && matched_count == row_count;
	SnapshotExistingUsingKeyDeltaAddresses(delta.matched_addresses, matched_count, defer_append);
}

void RecursiveCTEKeyedState::ValidateDeferredUsingKeyCandidateReuse(DataChunk &candidates) {
	D_ASSERT(key_delta);
	auto &delta = *key_delta;
	D_ASSERT(delta.deferred_previous_rows && delta.deferred_count == candidates.size());
	FinalizeAggregateRows(delta.row_state, delta.finalize_addresses, delta.updated_aggregate_rows,
	                      delta.deferred_count);

	if (!FinalizedPayloadsEqualCandidates(delta, op, delta.updated_aggregate_rows, candidates, delta.deferred_count)) {
		AppendPreviousUsingKeyDeltaRows(delta.matched_addresses, delta.deferred_count);
		delta.deferred_previous_rows = false;
		return;
	}

	const auto changed_count =
	    CountChangedPayloadRows(delta, op, delta.aggregate_rows, 0, delta.updated_aggregate_rows, delta.deferred_count);
	if (changed_count != delta.deferred_count) {
		AppendPreviousUsingKeyDeltaRows(delta.matched_addresses, delta.deferred_count);
		delta.deferred_previous_rows = false;
		return;
	}
	delta.changed_count = changed_count;
	delta.deferred_candidate_reuse = true;
}

void RecursiveCTEKeyedState::SnapshotPreaggregatedUsingKeyDeltaGroups(DataChunk &keys) {
	D_ASSERT(key_delta);
	auto &delta = *key_delta;
	delta.PrepareCollections();
	const auto group_count = keys.size();
	if (group_count == 0) {
		return;
	}
	const auto found_count = ht->LookupGroups(keys, delta.lookup_state, delta.found_groups);

	idx_t found_idx = 0;
	idx_t missing_count = 0;
	for (idx_t key_idx = 0; key_idx < group_count; key_idx++) {
		if (found_idx < found_count && delta.found_groups.get_index_unsafe(found_idx) == key_idx) {
			found_idx++;
			continue;
		}
		delta.missing_groups.set_index(missing_count++, key_idx);
	}
	D_ASSERT(found_idx == found_count);
	delta.new_count += missing_count;
	delta.touched_count += missing_count;

	if (found_count > 0) {
		delta.EnsureNewGroupAddressSet();
		delta.lookup_state.addresses.Flatten();
		auto source_addresses = FlatVector::GetData<data_ptr_t>(delta.lookup_state.addresses);
		auto matched_addresses = FlatVector::GetDataMutable<data_ptr_t>(delta.matched_addresses);
		delta.touched_addresses.Reserve(delta.touched_count + found_count);
		idx_t matched_count = 0;
		for (idx_t match_idx = 0; match_idx < found_count; match_idx++) {
			const auto input_idx = delta.found_groups.get_index_unsafe(match_idx);
			const auto address = source_addresses[input_idx];
			if (delta.ContainsNewGroupAddress(address) || !delta.touched_addresses.Insert(address)) {
				continue;
			}
			matched_addresses[matched_count++] = address;
		}
		delta.touched_count += matched_count;
		FlatVector::SetSize(delta.matched_addresses, matched_count);
		SnapshotExistingUsingKeyDeltaAddresses(delta.matched_addresses, matched_count);
	}

	if (missing_count > 0) {
		delta.selected_keys.Reset();
		delta.selected_keys.Slice(keys, delta.missing_groups, missing_count);
		delta.new_keys.Append(delta.new_key_append_state, delta.selected_keys);
	}
}

bool RecursiveCTEKeyedState::TryReuseChangedGroupCandidates(idx_t candidate_count) {
	D_ASSERT(key_delta);
	auto &delta = *key_delta;
	if (delta.deferred_candidate_reuse) {
		D_ASSERT(delta.new_count == 0 && delta.touched_count == candidate_count &&
		         delta.changed_count == candidate_count);
		return true;
	}
	if (!op.keyed.can_reuse_changed_group_candidates || delta.new_count != 0 ||
	    delta.touched_count != candidate_count || delta.previous_rows.Count() != candidate_count) {
		return false;
	}

	ColumnDataScanState previous_scan_state;
	ColumnDataScanState candidate_scan_state;
	delta.previous_rows.InitializeScan(previous_scan_state);
	intermediate_table.InitializeScan(candidate_scan_state);
	idx_t changed_count = 0;
	while (delta.previous_rows.Scan(previous_scan_state, delta.previous_scan_rows)) {
		if (!intermediate_table.Scan(candidate_scan_state, update_rows) ||
		    update_rows.size() != delta.previous_scan_rows.size()) {
			return false;
		}
		const auto row_count = update_rows.size();
		auto &group_addresses = delta.previous_scan_rows.data[0];
		CopySelectedAddresses(group_addresses, *FlatVector::IncrementalSelectionVector(), row_count,
		                      delta.finalize_addresses);
		FinalizeAggregateRows(delta.row_state, delta.finalize_addresses, delta.aggregate_rows, row_count);

		if (!FinalizedPayloadsEqualCandidates(delta, op, delta.aggregate_rows, update_rows, row_count)) {
			return false;
		}

		// The previous rows carry the group address in their first column
		changed_count +=
		    CountChangedPayloadRows(delta, op, delta.previous_scan_rows, 1, delta.aggregate_rows, row_count);
	}
	if (intermediate_table.Scan(candidate_scan_state, update_rows)) {
		return false;
	}
	if (changed_count != candidate_count) {
		return false;
	}
	delta.changed_count = changed_count;
	return true;
}

idx_t RecursiveCTEKeyedState::FinalizeUsingKeyDelta(bool update_partial_indexes, bool collect_metrics) {
	D_ASSERT(key_delta);
	auto &delta = *key_delta;
	ColumnDataScanState previous_scan_state;
	if (delta.collections_initialized) {
		delta.previous_rows.InitializeScan(previous_scan_state);
	}
	while (delta.collections_initialized && delta.previous_rows.Scan(previous_scan_state, delta.previous_scan_rows)) {
		const auto row_count = delta.previous_scan_rows.size();
		auto &group_addresses = delta.previous_scan_rows.data[0];
		ht->GatherGroups(delta.lookup_state, group_addresses, *FlatVector::IncrementalSelectionVector(), row_count,
		                 delta.key_scan_rows);
		CopySelectedAddresses(group_addresses, *FlatVector::IncrementalSelectionVector(), row_count,
		                      delta.finalize_addresses);
		FinalizeStateRows(delta.row_state, delta.finalize_addresses, delta.key_scan_rows, delta.aggregate_rows,
		                  delta.result_rows);
		const idx_t previous_payload_offset = 1;
		if (op.keyed.has_payload_comparisons) {
			delta.comparison_rows.Reset();
			for (idx_t payload_idx = 0; payload_idx < op.payload_idx.size(); payload_idx++) {
				delta.comparison_rows.data[payload_idx].Reference(
				    delta.previous_scan_rows.data[previous_payload_offset + payload_idx]);
				delta.comparison_rows.data[op.payload_idx.size() + payload_idx].Reference(
				    delta.result_rows.data[op.payload_idx[payload_idx]]);
			}
			delta.comparison_rows.CheckCardinality(row_count);
		}

		idx_t changed_count = 0;
		idx_t equal_count = row_count;
		optional_ptr<SelectionVector> equal_groups;
		for (idx_t payload_idx = 0; payload_idx < op.payload_idx.size() && equal_count > 0; payload_idx++) {
			auto &next_equal_groups = payload_idx % 2 == 0 ? delta.equal_groups_a : delta.equal_groups_b;
			idx_t changed_column_count;
			if (payload_comparison_executors[payload_idx]) {
				changed_column_count = payload_comparison_executors[payload_idx]->SelectExpression(
				    delta.comparison_rows, delta.changed_column_groups, next_equal_groups, equal_groups, equal_count);
			} else {
				changed_column_count =
				    SelectDistinctPayloadRows(delta.previous_scan_rows.data[previous_payload_offset + payload_idx],
				                              delta.result_rows.data[op.payload_idx[payload_idx]], equal_groups,
				                              equal_count, delta.changed_column_groups, next_equal_groups);
			}
			for (idx_t changed_idx = 0; changed_idx < changed_column_count; changed_idx++) {
				delta.changed_groups.set_index(changed_count++,
				                               delta.changed_column_groups.get_index_unsafe(changed_idx));
			}
			equal_count -= changed_column_count;
			equal_groups = &next_equal_groups;
		}
		if (changed_count > 0) {
			delta.changed_count += changed_count;
			delta.changed_rows.Reset();
			delta.changed_rows.Slice(delta.result_rows, delta.changed_groups, changed_count);
			op.working_table->Append(working_append_state, delta.changed_rows);
		}
	}

	idx_t index_work_ns = 0;
	auto finalize_new_groups = [&](Vector &group_addresses, idx_t row_count) {
		ht->GatherGroups(delta.lookup_state, group_addresses, *FlatVector::IncrementalSelectionVector(), row_count,
		                 delta.key_scan_rows);
		if (update_partial_indexes) {
			const auto index_start =
			    collect_metrics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
			for (auto &index : partial_key_indexes) {
				index->AddGroups(delta.key_scan_rows, *FlatVector::IncrementalSelectionVector(), group_addresses,
				                 *FlatVector::IncrementalSelectionVector(), row_count);
			}
			if (collect_metrics) {
				const auto index_end = std::chrono::steady_clock::now();
				const auto elapsed_ns = NumericCast<idx_t>(
				    std::chrono::duration_cast<std::chrono::nanoseconds>(index_end - index_start).count());
				index_work_ns += elapsed_ns;
				GetEpochMetrics().RecordPartialIndexMaintenance(elapsed_ns);
				metrics.RecordPartialIndexBuild(NumericCast<idx_t>(elapsed_ns / 1000));
			}
		}
		CopySelectedAddresses(group_addresses, *FlatVector::IncrementalSelectionVector(), row_count,
		                      delta.finalize_addresses);
		FinalizeStateRows(delta.row_state, delta.finalize_addresses, delta.key_scan_rows, delta.aggregate_rows,
		                  delta.result_rows);
		op.working_table->Append(working_append_state, delta.result_rows);
	};

	for (idx_t offset = 0; offset < delta.NewGroupAddressCount(); offset += STANDARD_VECTOR_SIZE) {
		const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, delta.NewGroupAddressCount() - offset);
		auto group_addresses = delta.NewGroupAddressVector(offset, count);
		finalize_new_groups(group_addresses, count);
	}

	ColumnDataScanState new_key_scan_state;
	if (delta.collections_initialized) {
		delta.new_keys.InitializeScan(new_key_scan_state);
	}
	while (delta.collections_initialized && delta.new_keys.Scan(new_key_scan_state, delta.key_scan_rows)) {
		const auto row_count = delta.key_scan_rows.size();
		const auto found_count = ht->LookupGroups(delta.key_scan_rows, delta.lookup_state, delta.found_groups);
		if (found_count != row_count) {
			throw InternalException("USING KEY delta finalization could not find %d of %d new groups",
			                        row_count - found_count, row_count);
		}
		finalize_new_groups(delta.lookup_state.addresses, row_count);
	}
	return index_work_ns;
}

} // namespace duckdb
