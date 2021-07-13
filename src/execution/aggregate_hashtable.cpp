#include "duckdb/execution/aggregate_hashtable.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/types/row_data_collection.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <cmath>
#include <map>

namespace duckdb {

using ValidityBytes = RowLayout::ValidityBytes;

GroupedAggregateHashTable::GroupedAggregateHashTable(BufferManager &buffer_manager, vector<LogicalType> group_types,
                                                     vector<LogicalType> payload_types,
                                                     const vector<BoundAggregateExpression *> &bindings,
                                                     HtEntryType entry_type)
    : GroupedAggregateHashTable(buffer_manager, move(group_types), move(payload_types),
                                AggregateObject::CreateAggregateObjects(bindings), entry_type) {
}

GroupedAggregateHashTable::GroupedAggregateHashTable(BufferManager &buffer_manager, vector<LogicalType> group_types)
    : GroupedAggregateHashTable(buffer_manager, move(group_types), {}, vector<AggregateObject>()) {
}

GroupedAggregateHashTable::GroupedAggregateHashTable(BufferManager &buffer_manager, vector<LogicalType> group_types_p,
                                                     vector<LogicalType> payload_types_p,
                                                     vector<AggregateObject> aggregate_objects_p,
                                                     HtEntryType entry_type)
    : BaseAggregateHashTable(buffer_manager, move(payload_types_p)), entry_type(entry_type), capacity(0), entries(0),
      payload_page_offset(0), addresses(LogicalType::POINTER), is_finalized(false), ht_offsets(LogicalTypeId::BIGINT),
      hash_salts(LogicalTypeId::SMALLINT), group_compare_vector(STANDARD_VECTOR_SIZE),
      no_match_vector(STANDARD_VECTOR_SIZE), empty_vector(STANDARD_VECTOR_SIZE) {

	// Append hash column to the end and initialise the row layout
	group_types_p.emplace_back(LogicalType::HASH);
	layout.Initialize(move(group_types_p), move(aggregate_objects_p));

	// HT layout
	hash_offset = layout.GetOffsets()[layout.ColumnCount() - 1];

	tuple_size = layout.GetRowWidth();

	D_ASSERT(tuple_size <= Storage::BLOCK_SIZE);
	tuples_per_block = Storage::BLOCK_SIZE / tuple_size;
	hashes_hdl = buffer_manager.Allocate(Storage::BLOCK_SIZE);
	hashes_hdl_ptr = hashes_hdl->Ptr();

	switch (entry_type) {
	case HtEntryType::HT_WIDTH_64: {
		hash_prefix_shift = (HASH_WIDTH - sizeof(aggr_ht_entry_64::salt)) * 8;
		Resize<aggr_ht_entry_64>(STANDARD_VECTOR_SIZE * 2);
		break;
	}
	case HtEntryType::HT_WIDTH_32: {
		hash_prefix_shift = (HASH_WIDTH - sizeof(aggr_ht_entry_32::salt)) * 8;
		Resize<aggr_ht_entry_32>(STANDARD_VECTOR_SIZE * 2);
		break;
	}
	default:
		throw InternalException("Unknown HT entry width");
	}

	// create additional hash tables for distinct aggrs
	auto &aggregates = layout.GetAggregates();
	distinct_hashes.resize(aggregates.size());

	idx_t payload_idx = 0;
	for (idx_t i = 0; i < aggregates.size(); i++) {
		auto &aggr = aggregates[i];
		if (aggr.distinct) {
			// layout types minus hash column plus aggr return type
			vector<LogicalType> distinct_group_types(layout.GetTypes());
			(void)distinct_group_types.pop_back();
			for (idx_t child_idx = 0; child_idx < aggr.child_count; child_idx++) {
				distinct_group_types.push_back(payload_types[payload_idx]);
			}
			distinct_hashes[i] = make_unique<GroupedAggregateHashTable>(buffer_manager, distinct_group_types);
		}
		payload_idx += aggr.child_count;
	}
	predicates.resize(layout.ColumnCount() - 1, ExpressionType::COMPARE_EQUAL);
	string_heap = make_unique<RowDataCollection>(buffer_manager, (idx_t)Storage::BLOCK_SIZE, 1);
}

GroupedAggregateHashTable::~GroupedAggregateHashTable() {
	Destroy();
}

template <class FUNC>
void GroupedAggregateHashTable::PayloadApply(FUNC fun) {
	if (entries == 0) {
		return;
	}
	idx_t apply_entries = entries;
	idx_t page_nr = 0;
	idx_t page_offset = 0;

	for (auto &payload_chunk_ptr : payload_hds_ptrs) {
		auto this_entries = MinValue(tuples_per_block, apply_entries);
		page_offset = 0;
		for (data_ptr_t ptr = payload_chunk_ptr, end = payload_chunk_ptr + this_entries * tuple_size; ptr < end;
		     ptr += tuple_size) {
			fun(page_nr, page_offset++, ptr);
		}
		apply_entries -= this_entries;
		page_nr++;
	}
	D_ASSERT(apply_entries == 0);
}

void GroupedAggregateHashTable::NewBlock() {
	auto pin = buffer_manager.Allocate(Storage::BLOCK_SIZE);
	payload_hds.push_back(move(pin));
	payload_hds_ptrs.push_back(payload_hds.back()->Ptr());
	payload_page_offset = 0;
}

void GroupedAggregateHashTable::Destroy() {
	// check if there is a destructor
	bool has_destructor = false;
	for (auto &aggr : layout.GetAggregates()) {
		if (aggr.function.destructor) {
			has_destructor = true;
		}
	}
	if (!has_destructor) {
		return;
	}
	// there are aggregates with destructors: loop over the hash table
	// and call the destructor method for each of the aggregates
	data_ptr_t data_pointers[STANDARD_VECTOR_SIZE];
	Vector state_vector(LogicalType::POINTER, (data_ptr_t)data_pointers);
	idx_t count = 0;

	PayloadApply([&](idx_t page_nr, idx_t page_offset, data_ptr_t ptr) {
		data_pointers[count++] = ptr;
		if (count == STANDARD_VECTOR_SIZE) {
			RowOperations::DestroyStates(layout, state_vector, count);
			count = 0;
		}
	});
	RowOperations::DestroyStates(layout, state_vector, count);
}

template <class ENTRY>
void GroupedAggregateHashTable::VerifyInternal() {
	auto hashes_ptr = (ENTRY *)hashes_hdl_ptr;
	D_ASSERT(payload_hds.size() == payload_hds_ptrs.size());
	idx_t count = 0;
	for (idx_t i = 0; i < capacity; i++) {
		if (hashes_ptr[i].page_nr > 0) {
			D_ASSERT(hashes_ptr[i].page_offset < tuples_per_block);
			D_ASSERT(hashes_ptr[i].page_nr <= payload_hds.size());
			auto ptr = payload_hds_ptrs[hashes_ptr[i].page_nr - 1] + ((hashes_ptr[i].page_offset) * tuple_size);
			auto hash = Load<hash_t>(ptr + hash_offset);
			D_ASSERT((hashes_ptr[i].salt) == (hash >> hash_prefix_shift));

			count++;
		}
	}
	D_ASSERT(count == entries);
}

idx_t GroupedAggregateHashTable::MaxCapacity() {
	idx_t max_pages = 0;
	idx_t max_tuples = 0;

	switch (entry_type) {
	case HtEntryType::HT_WIDTH_32:
		max_pages = NumericLimits<uint8_t>::Maximum();
		max_tuples = NumericLimits<uint16_t>::Maximum();
		break;
	default:
		D_ASSERT(entry_type == HtEntryType::HT_WIDTH_64);
		max_pages = NumericLimits<uint32_t>::Maximum();
		max_tuples = NumericLimits<uint16_t>::Maximum();
		break;
	}

	return max_pages * MinValue(max_tuples, (idx_t)Storage::BLOCK_SIZE / tuple_size);
}

void GroupedAggregateHashTable::Verify() {
#ifdef DEBUG
	switch (entry_type) {
	case HtEntryType::HT_WIDTH_32:
		VerifyInternal<aggr_ht_entry_32>();
		break;
	case HtEntryType::HT_WIDTH_64:
		VerifyInternal<aggr_ht_entry_64>();
		break;
	}
#endif
}

template <class ENTRY>
void GroupedAggregateHashTable::Resize(idx_t size) {
	Verify();

	D_ASSERT(!is_finalized);

	if (size <= capacity) {
		throw InternalException("Cannot downsize a hash table!");
	}
	D_ASSERT(size >= STANDARD_VECTOR_SIZE);

	// size needs to be a power of 2
	D_ASSERT((size & (size - 1)) == 0);
	bitmask = size - 1;

	auto byte_size = size * sizeof(ENTRY);
	if (byte_size > (idx_t)Storage::BLOCK_SIZE) {
		hashes_hdl = buffer_manager.Allocate(byte_size);
		hashes_hdl_ptr = hashes_hdl->Ptr();
	}
	memset(hashes_hdl_ptr, 0, byte_size);
	hashes_end_ptr = hashes_hdl_ptr + byte_size;
	capacity = size;

	auto hashes_arr = (ENTRY *)hashes_hdl_ptr;

	PayloadApply([&](idx_t page_nr, idx_t page_offset, data_ptr_t ptr) {
		auto hash = Load<hash_t>(ptr + hash_offset);
		D_ASSERT((hash & bitmask) == (hash % capacity));
		auto entry_idx = (idx_t)hash & bitmask;
		while (hashes_arr[entry_idx].page_nr > 0) {
			entry_idx++;
			if (entry_idx >= capacity) {
				entry_idx = 0;
			}
		}

		D_ASSERT(!hashes_arr[entry_idx].page_nr);
		D_ASSERT(hash >> hash_prefix_shift <= NumericLimits<uint16_t>::Maximum());

		hashes_arr[entry_idx].salt = hash >> hash_prefix_shift;
		hashes_arr[entry_idx].page_nr = page_nr + 1;
		hashes_arr[entry_idx].page_offset = page_offset;
	});

	Verify();
}

idx_t GroupedAggregateHashTable::AddChunk(DataChunk &groups, DataChunk &payload) {
	Vector hashes(LogicalType::HASH);
	groups.Hash(hashes);

	return AddChunk(groups, hashes, payload);
}

idx_t GroupedAggregateHashTable::AddChunk(DataChunk &groups, Vector &group_hashes, DataChunk &payload) {
	D_ASSERT(!is_finalized);

	if (groups.size() == 0) {
		return 0;
	}
	// dummy
	SelectionVector new_groups(STANDARD_VECTOR_SIZE);

	D_ASSERT(groups.ColumnCount() + 1 == layout.ColumnCount());
	for (idx_t i = 0; i < groups.ColumnCount(); i++) {
		D_ASSERT(groups.GetTypes()[i] == layout.GetTypes()[i]);
	}

	Vector addresses(LogicalType::POINTER);
	auto new_group_count = FindOrCreateGroups(groups, group_hashes, addresses, new_groups);
	VectorOperations::AddInPlace(addresses, layout.GetAggrOffset(), payload.size());

	// now every cell has an entry
	// update the aggregates
	idx_t payload_idx = 0;

	auto &aggregates = layout.GetAggregates();
	for (idx_t aggr_idx = 0; aggr_idx < aggregates.size(); aggr_idx++) {
		// for any entries for which a group was found, update the aggregate
		auto &aggr = aggregates[aggr_idx];
		if (aggr.distinct) {
			// construct chunk for secondary hash table probing
			vector<LogicalType> probe_types(groups.GetTypes());
			for (idx_t i = 0; i < aggr.child_count; i++) {
				probe_types.push_back(payload_types[payload_idx]);
			}
			DataChunk probe_chunk;
			probe_chunk.Initialize(probe_types);
			for (idx_t group_idx = 0; group_idx < groups.ColumnCount(); group_idx++) {
				probe_chunk.data[group_idx].Reference(groups.data[group_idx]);
			}
			for (idx_t i = 0; i < aggr.child_count; i++) {
				probe_chunk.data[groups.ColumnCount() + i].Reference(payload.data[payload_idx + i]);
			}
			probe_chunk.SetCardinality(groups);
			probe_chunk.Verify();

			Vector dummy_addresses(LogicalType::POINTER);
			// this is the actual meat, find out which groups plus payload
			// value have not been seen yet
			idx_t new_group_count =
			    distinct_hashes[aggr_idx]->FindOrCreateGroups(probe_chunk, dummy_addresses, new_groups);

			// now fix up the payload and addresses accordingly by creating
			// a selection vector
			if (new_group_count > 0) {
				if (aggr.filter) {
					Vector distinct_addresses(addresses, new_groups, new_group_count);
					DataChunk distinct_payload;
					auto pay_types = payload.GetTypes();
					distinct_payload.Initialize(pay_types);
					distinct_payload.Slice(payload, new_groups, new_group_count);
					distinct_addresses.Verify(new_group_count);
					distinct_addresses.Normalify(new_group_count);
					RowOperations::UpdateFilteredStates(aggr, distinct_addresses, distinct_payload, payload_idx);
				} else {
					Vector distinct_addresses(addresses, new_groups, new_group_count);
					for (idx_t i = 0; i < aggr.child_count; i++) {
						payload.data[payload_idx + i].Slice(new_groups, new_group_count);
						payload.data[payload_idx + i].Verify(new_group_count);
					}
					distinct_addresses.Verify(new_group_count);

					RowOperations::UpdateStates(aggr, distinct_addresses, payload, payload_idx, new_group_count);
				}
			}
		} else if (aggr.filter) {
			RowOperations::UpdateFilteredStates(aggr, addresses, payload, payload_idx);
		} else {
			RowOperations::UpdateStates(aggr, addresses, payload, payload_idx, payload.size());
		}

		// move to the next aggregate
		payload_idx += aggr.child_count;
		VectorOperations::AddInPlace(addresses, aggr.payload_size, payload.size());
	}

	Verify();
	return new_group_count;
}

void GroupedAggregateHashTable::FetchAggregates(DataChunk &groups, DataChunk &result) {
	groups.Verify();
	D_ASSERT(groups.ColumnCount() + 1 == layout.ColumnCount());
	for (idx_t i = 0; i < result.ColumnCount(); i++) {
		D_ASSERT(result.data[i].GetType() == payload_types[i]);
	}
	result.SetCardinality(groups);
	if (groups.size() == 0) {
		return;
	}
	// find the groups associated with the addresses
	// FIXME: this should not use the FindOrCreateGroups, creating them is unnecessary
	Vector addresses(LogicalType::POINTER);
	FindOrCreateGroups(groups, addresses);
	// now fetch the aggregates
	RowOperations::FinalizeStates(layout, addresses, result, 0);
}

template <class ENTRY>
idx_t GroupedAggregateHashTable::FindOrCreateGroupsInternal(DataChunk &groups, Vector &group_hashes, Vector &addresses,
                                                            SelectionVector &new_groups_out) {

	D_ASSERT(!is_finalized);

	if (entries + groups.size() > MaxCapacity()) {
		throw InternalException("Hash table capacity reached");
	}

	// resize at 50% capacity, also need to fit the entire vector
	if (capacity - entries <= groups.size() || entries > capacity / LOAD_FACTOR) {
		Resize<ENTRY>(capacity * 2);
	}

	D_ASSERT(capacity - entries >= groups.size());
	D_ASSERT(groups.ColumnCount() + 1 == layout.ColumnCount());
	// we need to be able to fit at least one vector of data
	D_ASSERT(capacity - entries >= groups.size());
	D_ASSERT(group_hashes.GetType() == LogicalType::HASH);

	group_hashes.Normalify(groups.size());
	auto group_hashes_ptr = FlatVector::GetData<hash_t>(group_hashes);

	D_ASSERT(ht_offsets.GetVectorType() == VectorType::FLAT_VECTOR);
	D_ASSERT(ht_offsets.GetType() == LogicalType::BIGINT);

	D_ASSERT(addresses.GetType() == LogicalType::POINTER);
	addresses.Normalify(groups.size());
	auto addresses_ptr = FlatVector::GetData<data_ptr_t>(addresses);

	// now compute the entry in the table based on the hash using a modulo
	UnaryExecutor::Execute<hash_t, uint64_t>(group_hashes, ht_offsets, groups.size(), [&](hash_t element) {
		D_ASSERT((element & bitmask) == (element % capacity));
		return (element & bitmask);
	});
	auto ht_offsets_ptr = FlatVector::GetData<uint64_t>(ht_offsets);

	// precompute the hash salts for faster comparison below
	D_ASSERT(hash_salts.GetType() == LogicalType::SMALLINT);
	UnaryExecutor::Execute<hash_t, uint16_t>(group_hashes, hash_salts, groups.size(),
	                                         [&](hash_t element) { return (element >> hash_prefix_shift); });
	auto hash_salts_ptr = FlatVector::GetData<uint16_t>(hash_salts);

	// we start out with all entries [0, 1, 2, ..., groups.size()]
	const SelectionVector *sel_vector = &FlatVector::INCREMENTAL_SELECTION_VECTOR;

	idx_t remaining_entries = groups.size();

	// make a chunk that references the groups and the hashes
	DataChunk group_chunk;
	group_chunk.InitializeEmpty(layout.GetTypes());
	for (idx_t grp_idx = 0; grp_idx < groups.ColumnCount(); grp_idx++) {
		group_chunk.data[grp_idx].Reference(groups.data[grp_idx]);
	}
	group_chunk.data[groups.ColumnCount()].Reference(group_hashes);
	group_chunk.SetCardinality(groups);

	// orrify all the groups
	auto group_data = group_chunk.Orrify();

	idx_t new_group_count = 0;
	while (remaining_entries > 0) {
		idx_t new_entry_count = 0;
		idx_t need_compare_count = 0;
		idx_t no_match_count = 0;

		// first figure out for each remaining whether or not it belongs to a full or empty group
		for (idx_t i = 0; i < remaining_entries; i++) {
			const idx_t index = sel_vector->get_index(i);
			const auto ht_entry_ptr = ((ENTRY *)this->hashes_hdl_ptr) + ht_offsets_ptr[index];
			if (ht_entry_ptr->page_nr == 0) { // we use page number 0 as a "unused marker"
				// cell is empty; setup the new entry
				if (payload_page_offset == tuples_per_block || payload_hds.empty()) {
					NewBlock();
				}

				auto entry_payload_ptr = payload_hds_ptrs.back() + (payload_page_offset * tuple_size);

				D_ASSERT(group_hashes_ptr[index] >> hash_prefix_shift <= NumericLimits<uint16_t>::Maximum());
				D_ASSERT(payload_page_offset < tuples_per_block);
				D_ASSERT(payload_hds.size() < NumericLimits<uint32_t>::Maximum());
				D_ASSERT(payload_page_offset + 1 < NumericLimits<uint16_t>::Maximum());

				ht_entry_ptr->salt = group_hashes_ptr[index] >> hash_prefix_shift;

				// page numbers start at one so we can use 0 as empty flag
				// GetPtr undoes this
				ht_entry_ptr->page_nr = payload_hds.size();
				ht_entry_ptr->page_offset = payload_page_offset++;

				// update selection lists for outer loops
				empty_vector.set_index(new_entry_count++, index);
				new_groups_out.set_index(new_group_count++, index);
				entries++;

				addresses_ptr[index] = entry_payload_ptr;

			} else {
				// cell is occupied: add to check list
				// only need to check if hash salt in ptr == prefix of hash in payload
				if (ht_entry_ptr->salt == hash_salts_ptr[index]) {
					group_compare_vector.set_index(need_compare_count++, index);

					auto page_ptr = payload_hds_ptrs[ht_entry_ptr->page_nr - 1];
					auto page_offset = ht_entry_ptr->page_offset * tuple_size;
					addresses_ptr[index] = page_ptr + page_offset;

				} else {
					no_match_vector.set_index(no_match_count++, index);
				}
			}
		}

		// for each of the locations that are empty, serialize the group columns to the locations
		RowOperations::Scatter(group_chunk, group_data.get(), layout, addresses, *string_heap, empty_vector,
		                       new_entry_count);
		RowOperations::InitializeStates(layout, addresses, empty_vector, new_entry_count);

		// now we have only the tuples remaining that might match to an existing group
		// start performing comparisons with each of the groups
		RowOperations::Match(group_chunk, group_data.get(), layout, addresses, predicates, group_compare_vector,
		                     need_compare_count, &no_match_vector, no_match_count);

		// each of the entries that do not match we move them to the next entry in the HT
		for (idx_t i = 0; i < no_match_count; i++) {
			idx_t index = no_match_vector.get_index(i);
			ht_offsets_ptr[index]++;
			if (ht_offsets_ptr[index] >= capacity) {
				ht_offsets_ptr[index] = 0;
			}
		}
		sel_vector = &no_match_vector;
		remaining_entries = no_match_count;
	}

	return new_group_count;
}

// this is to support distinct aggregations where we need to record whether we
// have already seen a value for a group
idx_t GroupedAggregateHashTable::FindOrCreateGroups(DataChunk &groups, Vector &group_hashes, Vector &addresses_out,
                                                    SelectionVector &new_groups_out) {
	switch (entry_type) {
	case HtEntryType::HT_WIDTH_64:
		return FindOrCreateGroupsInternal<aggr_ht_entry_64>(groups, group_hashes, addresses_out, new_groups_out);
	case HtEntryType::HT_WIDTH_32:
		return FindOrCreateGroupsInternal<aggr_ht_entry_32>(groups, group_hashes, addresses_out, new_groups_out);
	default:
		throw InternalException("Unknown HT entry width");
	}
}

void GroupedAggregateHashTable::FindOrCreateGroups(DataChunk &groups, Vector &addresses) {
	// create a dummy new_groups sel vector
	SelectionVector new_groups(STANDARD_VECTOR_SIZE);
	FindOrCreateGroups(groups, addresses, new_groups);
}

idx_t GroupedAggregateHashTable::FindOrCreateGroups(DataChunk &groups, Vector &addresses_out,
                                                    SelectionVector &new_groups_out) {
	Vector hashes(LogicalType::HASH);
	groups.Hash(hashes);
	return FindOrCreateGroups(groups, hashes, addresses_out, new_groups_out);
}

void GroupedAggregateHashTable::FlushMove(Vector &source_addresses, Vector &source_hashes, idx_t count) {
	D_ASSERT(source_addresses.GetType() == LogicalType::POINTER);
	D_ASSERT(source_hashes.GetType() == LogicalType::HASH);

	DataChunk groups;
	groups.Initialize(vector<LogicalType>(layout.GetTypes().begin(), layout.GetTypes().end() - 1));
	groups.SetCardinality(count);
	for (idx_t i = 0; i < groups.ColumnCount(); i++) {
		auto &column = groups.data[i];
		const auto col_offset = layout.GetOffsets()[i];
		RowOperations::Gather(source_addresses, FlatVector::INCREMENTAL_SELECTION_VECTOR, column,
		                      FlatVector::INCREMENTAL_SELECTION_VECTOR, count, col_offset, i);
	}

	SelectionVector new_groups(STANDARD_VECTOR_SIZE);
	Vector group_addresses(LogicalType::POINTER);
	SelectionVector new_groups_sel(STANDARD_VECTOR_SIZE);

	FindOrCreateGroups(groups, source_hashes, group_addresses, new_groups_sel);

	RowOperations::CombineStates(layout, source_addresses, group_addresses, count);
}

void GroupedAggregateHashTable::Combine(GroupedAggregateHashTable &other) {

	D_ASSERT(!is_finalized);

	D_ASSERT(other.layout.GetAggrWidth() == layout.GetAggrWidth());
	D_ASSERT(other.layout.GetDataWidth() == layout.GetDataWidth());
	D_ASSERT(other.layout.GetRowWidth() == layout.GetRowWidth());
	D_ASSERT(other.tuples_per_block == tuples_per_block);

	if (other.entries == 0) {
		return;
	}

	Vector addresses(LogicalType::POINTER);
	auto addresses_ptr = FlatVector::GetData<data_ptr_t>(addresses);

	Vector hashes(LogicalType::HASH);
	auto hashes_ptr = FlatVector::GetData<hash_t>(hashes);

	idx_t group_idx = 0;

	other.PayloadApply([&](idx_t page_nr, idx_t page_offset, data_ptr_t ptr) {
		auto hash = Load<hash_t>(ptr + hash_offset);

		hashes_ptr[group_idx] = hash;
		addresses_ptr[group_idx] = ptr;
		group_idx++;
		if (group_idx == STANDARD_VECTOR_SIZE) {
			FlushMove(addresses, hashes, group_idx);
			group_idx = 0;
		}
	});
	FlushMove(addresses, hashes, group_idx);
	string_heap->Merge(*other.string_heap);
	Verify();
}

struct PartitionInfo {
	PartitionInfo() : addresses(LogicalType::POINTER), hashes(LogicalType::HASH), group_count(0) {
		addresses_ptr = FlatVector::GetData<data_ptr_t>(addresses);
		hashes_ptr = FlatVector::GetData<hash_t>(hashes);
	};
	Vector addresses;
	Vector hashes;
	idx_t group_count;
	data_ptr_t *addresses_ptr;
	hash_t *hashes_ptr;
};

void GroupedAggregateHashTable::Partition(vector<GroupedAggregateHashTable *> &partition_hts, hash_t mask,
                                          idx_t shift) {
	D_ASSERT(partition_hts.size() > 1);
	vector<PartitionInfo> partition_info(partition_hts.size());

	PayloadApply([&](idx_t page_nr, idx_t page_offset, data_ptr_t ptr) {
		auto hash = Load<hash_t>(ptr + hash_offset);

		idx_t partition = (hash & mask) >> shift;
		D_ASSERT(partition < partition_hts.size());

		auto &info = partition_info[partition];

		info.hashes_ptr[info.group_count] = hash;
		info.addresses_ptr[info.group_count] = ptr;
		info.group_count++;
		if (info.group_count == STANDARD_VECTOR_SIZE) {
			D_ASSERT(partition_hts[partition]);
			partition_hts[partition]->FlushMove(info.addresses, info.hashes, info.group_count);
			info.group_count = 0;
		}
	});

	idx_t info_idx = 0;
	idx_t total_count = 0;
	for (auto &partition_entry : partition_hts) {
		auto &info = partition_info[info_idx++];
		partition_entry->FlushMove(info.addresses, info.hashes, info.group_count);

		partition_entry->string_heap->Merge(*string_heap);
		partition_entry->Verify();
		total_count += partition_entry->Size();
	}
	D_ASSERT(total_count == entries);
}

idx_t GroupedAggregateHashTable::Scan(idx_t &scan_position, DataChunk &result) {
	auto data_pointers = FlatVector::GetData<data_ptr_t>(addresses);

	auto remaining = entries - scan_position;
	if (remaining == 0) {
		return 0;
	}
	auto this_n = MinValue((idx_t)STANDARD_VECTOR_SIZE, remaining);

	auto chunk_idx = scan_position / tuples_per_block;
	auto chunk_offset = (scan_position % tuples_per_block) * tuple_size;
	D_ASSERT(chunk_offset + tuple_size <= Storage::BLOCK_SIZE);

	auto read_ptr = payload_hds_ptrs[chunk_idx++];
	for (idx_t i = 0; i < this_n; i++) {
		data_pointers[i] = read_ptr + chunk_offset;
		chunk_offset += tuple_size;
		if (chunk_offset >= tuples_per_block * tuple_size) {
			read_ptr = payload_hds_ptrs[chunk_idx++];
			chunk_offset = 0;
		}
	}

	result.SetCardinality(this_n);
	// fetch the group columns (ignoring the final hash column
	const auto group_cols = layout.ColumnCount() - 1;
	for (idx_t i = 0; i < group_cols; i++) {
		auto &column = result.data[i];
		const auto col_offset = layout.GetOffsets()[i];
		RowOperations::Gather(addresses, FlatVector::INCREMENTAL_SELECTION_VECTOR, column,
		                      FlatVector::INCREMENTAL_SELECTION_VECTOR, result.size(), col_offset, i);
	}

	RowOperations::FinalizeStates(layout, addresses, result, group_cols);

	scan_position += this_n;
	return this_n;
}

void GroupedAggregateHashTable::Finalize() {
	D_ASSERT(!is_finalized);

	// early release hashes, not needed for partition/scan
	hashes_hdl.reset();
	is_finalized = true;
}

} // namespace duckdb
