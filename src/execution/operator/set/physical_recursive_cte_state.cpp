#include "duckdb/execution/operator/set/physical_recursive_cte_state.hpp"

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/pipeline_executor.hpp"

namespace duckdb {

RecursiveCTEPartialKeyIndex::RecursiveCTEPartialKeyIndex(Allocator &allocator,
                                                         const vector<LogicalType> &full_key_types,
                                                         vector<idx_t> key_indices_p)
    : key_indices(std::move(key_indices_p)), hashes(LogicalType::HASH) {
	vector<LogicalType> partial_key_types;
	for (auto key_idx : key_indices) {
		D_ASSERT(key_idx < full_key_types.size());
		partial_key_types.push_back(full_key_types[key_idx]);
	}
	partial_keys.Initialize(allocator, partial_key_types);
	selected_keys.Initialize(allocator, partial_key_types);
	Resize(1024);
}

void RecursiveCTEPartialKeyIndex::Resize(idx_t capacity) {
	D_ASSERT(capacity > 0 && (capacity & (capacity - 1)) == 0);
	heads.assign(capacity, DConstants::INVALID_INDEX);
	for (idx_t entry_idx = 0; entry_idx < entries.size(); entry_idx++) {
		auto &entry = entries[entry_idx];
		const auto bucket = entry.hash & (capacity - 1);
		entry.next = heads[bucket];
		heads[bucket] = entry_idx;
	}
}

void RecursiveCTEPartialKeyIndex::AddGroups(DataChunk &full_keys, const SelectionVector &key_selection,
                                            Vector &group_addresses, const SelectionVector &address_selection,
                                            idx_t group_count) {
	if (group_count == 0) {
		return;
	}
	while (entries.size() + group_count > heads.size()) {
		Resize(heads.size() * 2);
	}
	partial_keys.Reset();
	for (idx_t partial_idx = 0; partial_idx < key_indices.size(); partial_idx++) {
		partial_keys.data[partial_idx].Reference(full_keys.data[key_indices[partial_idx]]);
	}
	partial_keys.CheckCardinality(full_keys.size());
	selected_keys.Reset();
	selected_keys.Slice(partial_keys, key_selection, group_count);
	selected_keys.Hash(hashes);

	const auto hash_values = hashes.Values<hash_t>();
	const auto addresses = FlatVector::GetData<data_ptr_t>(group_addresses);
	for (idx_t group_idx = 0; group_idx < group_count; group_idx++) {
		const auto hash = hash_values[group_idx].GetValue();
		const auto bucket = hash & (heads.size() - 1);
		entries.push_back({hash, addresses[address_selection.get_index(group_idx)], heads[bucket]});
		heads[bucket] = entries.size() - 1;
	}
}

idx_t RecursiveCTEPartialKeyIndex::GetHead(hash_t hash) const {
	return heads[hash & (heads.size() - 1)];
}

const RecursiveCTEPartialKeyIndex::Entry &RecursiveCTEPartialKeyIndex::GetEntry(idx_t entry_idx) const {
	return entries[entry_idx];
}

idx_t RecursiveCTEPartialKeyIndex::Count() const {
	return entries.size();
}

idx_t RecursiveCTEPartialKeyIndex::SizeInBytes() const {
	return heads.capacity() * sizeof(idx_t) + entries.capacity() * sizeof(Entry);
}

} // namespace duckdb
