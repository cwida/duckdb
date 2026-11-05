#pragma once

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte.hpp"

namespace duckdb {

class RecursiveCTEGroupAddressSet {
public:
	void Reset();
	void Reserve(idx_t count);
	bool Insert(data_ptr_t address);
	bool Contains(data_ptr_t address) const;

private:
	void Resize(idx_t capacity);

	vector<data_ptr_t> entries;
	vector<idx_t> occupied_slots;
};

struct RecursiveCTEKeyDeltaState {
	RecursiveCTEKeyDeltaState(ClientContext &context, const PhysicalRecursiveCTE &op);

	void Reset();
	void PrepareCollections();

	void ReserveNewGroupAddresses(idx_t count) {
		new_group_addresses.reserve(count);
	}
	//! Whether the address set exists, so single additions must keep it current and the bulk shortcut is closed
	bool HasNewGroupAddressSet() const {
		return new_group_address_set_built;
	}
	//! Bulk shortcut while no set exists yet
	void AppendNewGroupAddresses(const data_ptr_t *addresses, idx_t count) {
		D_ASSERT(!new_group_address_set_built);
		new_group_addresses.insert(new_group_addresses.end(), addresses, addresses + count);
	}
	void AddNewGroupAddress(data_ptr_t address) {
		new_group_addresses.push_back(address);
		if (new_group_address_set_built) {
			new_group_address_set.Insert(address);
		}
	}
	//! Builds the set over the addresses collected so far; later additions keep it current
	void EnsureNewGroupAddressSet() {
		if (new_group_address_set_built) {
			return;
		}
		new_group_address_set.Reserve(new_group_addresses.size());
		for (auto address : new_group_addresses) {
			new_group_address_set.Insert(address);
		}
		new_group_address_set_built = true;
	}
	bool ContainsNewGroupAddress(data_ptr_t address) const {
		D_ASSERT(new_group_address_set_built);
		return new_group_address_set.Contains(address);
	}
	idx_t NewGroupAddressCount() const {
		return new_group_addresses.size();
	}
	//! A vector over count of the new group addresses from offset on, for the frontier assembly
	Vector NewGroupAddressVector(idx_t offset, idx_t count) {
		D_ASSERT(offset + count <= new_group_addresses.size());
		return Vector(LogicalType::POINTER, reinterpret_cast<data_ptr_t>(new_group_addresses.data() + offset), count);
	}

	ColumnDataCollection previous_rows;
	ColumnDataCollection new_keys;
	ColumnDataAppendState previous_append_state;
	ColumnDataAppendState new_key_append_state;
	DataChunk selected_keys;
	DataChunk aggregate_rows;
	DataChunk updated_aggregate_rows;
	DataChunk result_rows;
	DataChunk previous_state_rows;
	DataChunk changed_rows;
	DataChunk previous_scan_rows;
	DataChunk key_scan_rows;
	DataChunk comparison_rows;
	Vector matched_addresses;
	Vector finalize_addresses;
	SelectionVector found_groups;
	SelectionVector missing_groups;
	SelectionVector changed_groups;
	SelectionVector changed_column_groups;
	SelectionVector equal_groups_a;
	SelectionVector equal_groups_b;
	AggregateHTLookupState lookup_state;
	ArenaAllocator arena;
	RowOperationsState row_state;
	RecursiveCTEGroupAddressSet touched_addresses;
	bool collections_initialized = false;
	bool deferred_previous_rows = false;
	bool deferred_candidate_reuse = false;
	idx_t deferred_count = 0;
	idx_t touched_count = 0;
	idx_t new_count = 0;
	idx_t changed_count = 0;

private:
	//! Addresses of the groups this commit created; the set over them is built lazily by EnsureNewGroupAddressSet
	unsafe_vector<data_ptr_t> new_group_addresses;
	RecursiveCTEGroupAddressSet new_group_address_set;
	bool new_group_address_set_built = false;
};

} // namespace duckdb
