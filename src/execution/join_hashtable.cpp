#include "duckdb/execution/join_hashtable.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/types/row_data_collection.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

using ValidityBytes = JoinHashTable::ValidityBytes;
using ScanStructure = JoinHashTable::ScanStructure;

JoinHashTable::JoinHashTable(BufferManager &buffer_manager, vector<JoinCondition> &conditions,
                             vector<LogicalType> btypes, JoinType type)
    : buffer_manager(buffer_manager), build_types(move(btypes)), entry_size(0), tuple_size(0),
      vfound(Value::BOOLEAN(false)), join_type(type), finalized(false), has_null(false), count(0) {
	for (auto &condition : conditions) {
		D_ASSERT(condition.left->return_type == condition.right->return_type);
		auto type = condition.left->return_type;
		if (condition.comparison == ExpressionType::COMPARE_EQUAL) {
			// all equality conditions should be at the front
			// all other conditions at the back
			// this assert checks that
			D_ASSERT(equality_types.size() == condition_types.size());
			equality_types.push_back(type);
		}
		predicates.push_back(condition.comparison);
		null_values_are_equal.push_back(condition.null_values_are_equal);
		D_ASSERT(!condition.null_values_are_equal ||
		         (condition.null_values_are_equal && condition.comparison == ExpressionType::COMPARE_EQUAL));

		condition_types.push_back(type);
	}
	// at least one equality is necessary
	D_ASSERT(equality_types.size() > 0);

	// Types for the layout
	vector<LogicalType> layout_types(condition_types);
	layout_types.insert(layout_types.end(), build_types.begin(), build_types.end());
	if (IsRightOuterJoin(join_type)) {
		// full/right outer joins need an extra bool to keep track of whether or not a tuple has found a matching entry
		// we place the bool before the NEXT pointer
		layout_types.emplace_back(LogicalType::BOOLEAN);
	}
	layout_types.emplace_back(LogicalType::HASH);
	layout.Initialize(layout_types);

	const auto &offsets = layout.GetOffsets();
	tuple_size = offsets[condition_types.size() + build_types.size()];
	pointer_offset = offsets.back();
	entry_size = layout.GetRowWidth();

	// compute the per-block capacity of this HT
	block_capacity = MaxValue<idx_t>(STANDARD_VECTOR_SIZE, (Storage::BLOCK_ALLOC_SIZE / entry_size) + 1);
	string_heap = make_unique<RowDataCollection>(buffer_manager, Storage::BLOCK_ALLOC_SIZE / 8, 8);
}

JoinHashTable::~JoinHashTable() {
}

void JoinHashTable::ApplyBitmask(Vector &hashes, idx_t count) {
	if (hashes.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		D_ASSERT(!ConstantVector::IsNull(hashes));
		auto indices = ConstantVector::GetData<hash_t>(hashes);
		*indices = *indices & bitmask;
	} else {
		hashes.Normalify(count);
		auto indices = FlatVector::GetData<hash_t>(hashes);
		for (idx_t i = 0; i < count; i++) {
			indices[i] &= bitmask;
		}
	}
}

void JoinHashTable::ApplyBitmask(Vector &hashes, const SelectionVector &sel, idx_t count, Vector &pointers) {
	VectorData hdata;
	hashes.Orrify(count, hdata);

	auto hash_data = (hash_t *)hdata.data;
	auto result_data = FlatVector::GetData<data_ptr_t *>(pointers);
	auto main_ht = (data_ptr_t *)hash_map->node->buffer;
	for (idx_t i = 0; i < count; i++) {
		auto rindex = sel.get_index(i);
		auto hindex = hdata.sel->get_index(rindex);
		auto hash = hash_data[hindex];
		result_data[rindex] = main_ht + (hash & bitmask);
	}
}

void JoinHashTable::Hash(DataChunk &keys, const SelectionVector &sel, idx_t count, Vector &hashes) {
	if (count == keys.size()) {
		// no null values are filtered: use regular hash functions
		VectorOperations::Hash(keys.data[0], hashes, keys.size());
		for (idx_t i = 1; i < equality_types.size(); i++) {
			VectorOperations::CombineHash(hashes, keys.data[i], keys.size());
		}
	} else {
		// null values were filtered: use selection vector
		VectorOperations::Hash(keys.data[0], hashes, sel, count);
		for (idx_t i = 1; i < equality_types.size(); i++) {
			VectorOperations::CombineHash(hashes, keys.data[i], sel, count);
		}
	}
}
idx_t JoinHashTable::AppendToBlock(HTDataBlock &block, BufferHandle &handle, vector<BlockAppendEntry> &append_entries,
                                   idx_t remaining) {
	idx_t append_count = MinValue<idx_t>(remaining, block.capacity - block.count);
	auto dataptr = handle.node->buffer + block.count * entry_size;
	append_entries.emplace_back(dataptr, append_count);
	block.count += append_count;
	return append_count;
}

static idx_t FilterNullValues(VectorData &vdata, const SelectionVector &sel, idx_t count, SelectionVector &result) {
	idx_t result_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = sel.get_index(i);
		auto key_idx = vdata.sel->get_index(idx);
		if (vdata.validity.RowIsValid(key_idx)) {
			result.set_index(result_count++, idx);
		}
	}
	return result_count;
}

idx_t JoinHashTable::PrepareKeys(DataChunk &keys, unique_ptr<VectorData[]> &key_data,
                                 const SelectionVector *&current_sel, SelectionVector &sel, bool build_side) {
	key_data = keys.Orrify();

	// figure out which keys are NULL, and create a selection vector out of them
	current_sel = &FlatVector::INCREMENTAL_SELECTION_VECTOR;
	idx_t added_count = keys.size();
	if (build_side && IsRightOuterJoin(join_type)) {
		// in case of a right or full outer join, we cannot remove NULL keys from the build side
		return added_count;
	}
	for (idx_t i = 0; i < keys.ColumnCount(); i++) {
		if (!null_values_are_equal[i]) {
			if (key_data[i].validity.AllValid()) {
				continue;
			}
			added_count = FilterNullValues(key_data[i], *current_sel, added_count, sel);
			// null values are NOT equal for this column, filter them out
			current_sel = &sel;
		}
	}
	return added_count;
}

void JoinHashTable::Build(DataChunk &keys, DataChunk &payload) {
	D_ASSERT(!finalized);
	D_ASSERT(keys.size() == payload.size());
	if (keys.size() == 0) {
		return;
	}
	// special case: correlated mark join
	if (join_type == JoinType::MARK && !correlated_mark_join_info.correlated_types.empty()) {
		auto &info = correlated_mark_join_info;
		lock_guard<mutex> mj_lock(info.mj_lock);
		// Correlated MARK join
		// for the correlated mark join we need to keep track of COUNT(*) and COUNT(COLUMN) for each of the correlated
		// columns push into the aggregate hash table
		D_ASSERT(info.correlated_counts);
		info.group_chunk.SetCardinality(keys);
		for (idx_t i = 0; i < info.correlated_types.size(); i++) {
			info.group_chunk.data[i].Reference(keys.data[i]);
		}
		if (info.correlated_payload.data.empty()) {
			vector<LogicalType> types;
			types.push_back(keys.data[info.correlated_types.size()].GetType());
			info.correlated_payload.InitializeEmpty(types);
		}
		info.correlated_payload.SetCardinality(keys);
		info.correlated_payload.data[0].Reference(keys.data[info.correlated_types.size()]);
		info.correlated_counts->AddChunk(info.group_chunk, info.correlated_payload);
	}

	// prepare the keys for processing
	unique_ptr<VectorData[]> key_data;
	const SelectionVector *current_sel;
	SelectionVector sel(STANDARD_VECTOR_SIZE);
	idx_t added_count = PrepareKeys(keys, key_data, current_sel, sel, true);
	if (added_count < keys.size()) {
		has_null = true;
	}
	if (added_count == 0) {
		return;
	}

	vector<unique_ptr<BufferHandle>> handles;
	vector<BlockAppendEntry> append_entries;
	// first allocate space of where to serialize the keys and payload columns
	idx_t remaining = added_count;
	{
		// first append to the last block (if any)
		lock_guard<mutex> append_lock(ht_lock);
		count += added_count;

		if (!blocks.empty()) {
			auto &last_block = blocks.back();
			if (last_block.count < last_block.capacity) {
				// last block has space: pin the buffer of this block
				auto handle = buffer_manager.Pin(last_block.block);
				// now append to the block
				idx_t append_count = AppendToBlock(last_block, *handle, append_entries, remaining);
				remaining -= append_count;
				handles.push_back(move(handle));
			}
		}
		while (remaining > 0) {
			// now for the remaining data, allocate new buffers to store the data and append there
			auto block = buffer_manager.RegisterMemory(block_capacity * entry_size, false);
			auto handle = buffer_manager.Pin(block);

			HTDataBlock new_block;
			new_block.count = 0;
			new_block.capacity = block_capacity;
			new_block.block = move(block);

			idx_t append_count = AppendToBlock(new_block, *handle, append_entries, remaining);
			remaining -= append_count;
			handles.push_back(move(handle));
			blocks.push_back(move(new_block));
		}
	}
	// now set up the key_locations based on the append entries
	Vector addresses(LogicalType::POINTER);
	auto key_locations = FlatVector::GetData<data_ptr_t>(addresses);

	idx_t append_idx = 0;
	for (auto &append_entry : append_entries) {
		idx_t next = append_idx + append_entry.count;
		for (; append_idx < next; append_idx++) {
			// Set the validity mask (clearing is less common)
			auto idx = current_sel->get_index(append_idx);
			key_locations[idx] = append_entry.baseptr;
			append_entry.baseptr += entry_size;
		}
	}

	// hash the keys and obtain an entry in the list
	// note that we only hash the keys used in the equality comparison
	Vector hash_values(LogicalType::HASH);
	Hash(keys, *current_sel, added_count, hash_values);

	// build a chunk so we can handle nested types that need more than Orrification
	DataChunk source_chunk;
	source_chunk.InitializeEmpty(layout.GetTypes());

	vector<VectorData> source_data;
	source_data.reserve(layout.ColumnCount());

	// serialize the keys to the key locations
	for (idx_t i = 0; i < keys.ColumnCount(); i++) {
		source_chunk.data[i].Reference(keys.data[i]);
		source_data.emplace_back(move(key_data[i]));
	}
	// now serialize the payload
	D_ASSERT(build_types.size() == payload.ColumnCount());
	for (idx_t i = 0; i < payload.ColumnCount(); i++) {
		source_chunk.data[source_data.size()].Reference(payload.data[i]);
		VectorData pdata;
		payload.data[i].Orrify(payload.size(), pdata);
		source_data.emplace_back(move(pdata));
	}
	if (IsRightOuterJoin(join_type)) {
		// for FULL/RIGHT OUTER joins initialize the "found" boolean to false
		source_chunk.data[source_data.size()].Reference(vfound);
		VectorData fdata;
		vfound.Orrify(keys.size(), fdata);
		source_data.emplace_back(move(fdata));
	}

	// serialise the hashes at the end
	source_chunk.data[source_data.size()].Reference(hash_values);
	VectorData hdata;
	hash_values.Orrify(keys.size(), hdata);
	source_data.emplace_back(move(hdata));

	source_chunk.SetCardinality(keys);

	RowOperations::Scatter(source_chunk, source_data.data(), layout, addresses, *string_heap, *current_sel,
	                       added_count);
}

void JoinHashTable::InsertHashes(Vector &hashes, idx_t count, data_ptr_t key_locations[]) {
	D_ASSERT(hashes.GetType().id() == LogicalTypeId::HASH);

	// use bitmask to get position in array
	ApplyBitmask(hashes, count);

	hashes.Normalify(count);

	D_ASSERT(hashes.GetVectorType() == VectorType::FLAT_VECTOR);
	auto pointers = (data_ptr_t *)hash_map->node->buffer;
	auto indices = FlatVector::GetData<hash_t>(hashes);
	for (idx_t i = 0; i < count; i++) {
		auto index = indices[i];
		// set prev in current key to the value (NOTE: this will be nullptr if
		// there is none)
		Store<data_ptr_t>(pointers[index], key_locations[i] + pointer_offset);

		// set pointer to current tuple
		pointers[index] = key_locations[i];
	}
}

void JoinHashTable::Finalize() {
	// the build has finished, now iterate over all the nodes and construct the final hash table
	// select a HT that has at least 50% empty space
	idx_t capacity = NextPowerOfTwo(MaxValue<idx_t>(count * 2, (Storage::BLOCK_ALLOC_SIZE / sizeof(data_ptr_t)) + 1));
	// size needs to be a power of 2
	D_ASSERT((capacity & (capacity - 1)) == 0);
	bitmask = capacity - 1;

	// allocate the HT and initialize it with all-zero entries
	hash_map = buffer_manager.Allocate(capacity * sizeof(data_ptr_t));
	memset(hash_map->node->buffer, 0, capacity * sizeof(data_ptr_t));

	Vector hashes(LogicalType::HASH);
	auto hash_data = FlatVector::GetData<hash_t>(hashes);
	data_ptr_t key_locations[STANDARD_VECTOR_SIZE];
	// now construct the actual hash table; scan the nodes
	// as we can the nodes we pin all the blocks of the HT and keep them pinned until the HT is destroyed
	// this is so that we can keep pointers around to the blocks
	// FIXME: if we cannot keep everything pinned in memory, we could switch to an out-of-memory merge join or so
	for (auto &block : blocks) {
		auto handle = buffer_manager.Pin(block.block);
		data_ptr_t dataptr = handle->node->buffer;
		idx_t entry = 0;
		while (entry < block.count) {
			// fetch the next vector of entries from the blocks
			idx_t next = MinValue<idx_t>(STANDARD_VECTOR_SIZE, block.count - entry);
			for (idx_t i = 0; i < next; i++) {
				hash_data[i] = Load<hash_t>((data_ptr_t)(dataptr + pointer_offset));
				key_locations[i] = dataptr;
				dataptr += entry_size;
			}
			// now insert into the hash table
			InsertHashes(hashes, next, key_locations);

			entry += next;
		}
		pinned_handles.push_back(move(handle));
	}

	finalized = true;
}

unique_ptr<ScanStructure> JoinHashTable::Probe(DataChunk &keys) {
	D_ASSERT(count > 0); // should be handled before
	D_ASSERT(finalized);

	// set up the scan structure
	auto ss = make_unique<ScanStructure>(*this);

	if (join_type != JoinType::INNER) {
		ss->found_match = unique_ptr<bool[]>(new bool[STANDARD_VECTOR_SIZE]);
		memset(ss->found_match.get(), 0, sizeof(bool) * STANDARD_VECTOR_SIZE);
	}

	// first prepare the keys for probing
	const SelectionVector *current_sel;
	ss->count = PrepareKeys(keys, ss->key_data, current_sel, ss->sel_vector, false);
	if (ss->count == 0) {
		return ss;
	}

	// hash all the keys
	Vector hashes(LogicalType::HASH);
	Hash(keys, *current_sel, ss->count, hashes);

	// now initialize the pointers of the scan structure based on the hashes
	ApplyBitmask(hashes, *current_sel, ss->count, ss->pointers);

	// create the selection vector linking to only non-empty entries
	idx_t count = 0;
	auto pointers = FlatVector::GetData<data_ptr_t>(ss->pointers);
	for (idx_t i = 0; i < ss->count; i++) {
		auto idx = current_sel->get_index(i);
		pointers[idx] = Load<data_ptr_t>(pointers[idx]);
		if (pointers[idx]) {
			ss->sel_vector.set_index(count++, idx);
		}
	}
	ss->count = count;
	return ss;
}

ScanStructure::ScanStructure(JoinHashTable &ht)
    : pointers(LogicalType::POINTER), sel_vector(STANDARD_VECTOR_SIZE), ht(ht), finished(false) {
}

void ScanStructure::Next(DataChunk &keys, DataChunk &left, DataChunk &result) {
	if (finished) {
		return;
	}

	switch (ht.join_type) {
	case JoinType::INNER:
	case JoinType::RIGHT:
		NextInnerJoin(keys, left, result);
		break;
	case JoinType::SEMI:
		NextSemiJoin(keys, left, result);
		break;
	case JoinType::MARK:
		NextMarkJoin(keys, left, result);
		break;
	case JoinType::ANTI:
		NextAntiJoin(keys, left, result);
		break;
	case JoinType::OUTER:
	case JoinType::LEFT:
		NextLeftJoin(keys, left, result);
		break;
	case JoinType::SINGLE:
		NextSingleJoin(keys, left, result);
		break;
	default:
		throw Exception("Unhandled join type in JoinHashTable");
	}
}

idx_t ScanStructure::ResolvePredicates(DataChunk &keys, SelectionVector &match_sel, SelectionVector *no_match_sel) {
	// Start with the scan selection
	for (idx_t i = 0; i < this->count; ++i) {
		match_sel.set_index(i, this->sel_vector.get_index(i));
	}
	idx_t no_match_count = 0;

	return RowOperations::Match(keys, key_data.get(), ht.layout, pointers, ht.predicates, match_sel, this->count,
	                            no_match_sel, no_match_count);
}

idx_t ScanStructure::ScanInnerJoin(DataChunk &keys, SelectionVector &result_vector) {
	while (true) {
		// resolve the predicates for this set of keys
		idx_t result_count = ResolvePredicates(keys, result_vector, nullptr);

		// after doing all the comparisons set the found_match vector
		if (found_match) {
			for (idx_t i = 0; i < result_count; i++) {
				auto idx = result_vector.get_index(i);
				found_match[idx] = true;
			}
		}
		if (result_count > 0) {
			return result_count;
		}
		// no matches found: check the next set of pointers
		AdvancePointers();
		if (this->count == 0) {
			return 0;
		}
	}
}

void ScanStructure::AdvancePointers(const SelectionVector &sel, idx_t sel_count) {
	// now for all the pointers, we move on to the next set of pointers
	idx_t new_count = 0;
	auto ptrs = FlatVector::GetData<data_ptr_t>(this->pointers);
	for (idx_t i = 0; i < sel_count; i++) {
		auto idx = sel.get_index(i);
		ptrs[idx] = Load<data_ptr_t>(ptrs[idx] + ht.pointer_offset);
		if (ptrs[idx]) {
			this->sel_vector.set_index(new_count++, idx);
		}
	}
	this->count = new_count;
}

void ScanStructure::AdvancePointers() {
	AdvancePointers(this->sel_vector, this->count);
}

void ScanStructure::GatherResult(Vector &result, const SelectionVector &result_vector,
                                 const SelectionVector &sel_vector, const idx_t count, const idx_t col_no) {
	const auto col_offset = ht.layout.GetOffsets()[col_no];
	RowOperations::Gather(pointers, sel_vector, result, result_vector, count, col_offset, col_no);
}

void ScanStructure::GatherResult(Vector &result, const SelectionVector &sel_vector, const idx_t count,
                                 const idx_t col_idx) {
	GatherResult(result, FlatVector::INCREMENTAL_SELECTION_VECTOR, sel_vector, count, col_idx);
}

void ScanStructure::NextInnerJoin(DataChunk &keys, DataChunk &left, DataChunk &result) {
	D_ASSERT(result.ColumnCount() == left.ColumnCount() + ht.build_types.size());
	if (this->count == 0) {
		// no pointers left to chase
		return;
	}

	SelectionVector result_vector(STANDARD_VECTOR_SIZE);

	idx_t result_count = ScanInnerJoin(keys, result_vector);
	if (result_count > 0) {
		if (IsRightOuterJoin(ht.join_type)) {
			// full/right outer join: mark join matches as FOUND in the HT
			auto ptrs = FlatVector::GetData<data_ptr_t>(pointers);
			for (idx_t i = 0; i < result_count; i++) {
				auto idx = result_vector.get_index(i);
				// NOTE: threadsan reports this as a data race because this can be set concurrently by separate threads
				// Technically it is, but it does not matter, since the only value that can be written is "true"
				Store<bool>(true, ptrs[idx] + ht.tuple_size);
			}
		}
		// matches were found
		// construct the result
		// on the LHS, we create a slice using the result vector
		result.Slice(left, result_vector, result_count);

		// on the RHS, we need to fetch the data from the hash table
		for (idx_t i = 0; i < ht.build_types.size(); i++) {
			auto &vector = result.data[left.ColumnCount() + i];
			D_ASSERT(vector.GetType() == ht.build_types[i]);
			GatherResult(vector, result_vector, result_count, i + ht.condition_types.size());
		}
		AdvancePointers();
	}
}

void ScanStructure::ScanKeyMatches(DataChunk &keys) {
	// the semi-join, anti-join and mark-join we handle a differently from the inner join
	// since there can be at most STANDARD_VECTOR_SIZE results
	// we handle the entire chunk in one call to Next().
	// for every pointer, we keep chasing pointers and doing comparisons.
	// this results in a boolean array indicating whether or not the tuple has a match
	SelectionVector match_sel(STANDARD_VECTOR_SIZE), no_match_sel(STANDARD_VECTOR_SIZE);
	while (this->count > 0) {
		// resolve the predicates for the current set of pointers
		idx_t match_count = ResolvePredicates(keys, match_sel, &no_match_sel);
		idx_t no_match_count = this->count - match_count;

		// mark each of the matches as found
		for (idx_t i = 0; i < match_count; i++) {
			found_match[match_sel.get_index(i)] = true;
		}
		// continue searching for the ones where we did not find a match yet
		AdvancePointers(no_match_sel, no_match_count);
	}
}

template <bool MATCH>
void ScanStructure::NextSemiOrAntiJoin(DataChunk &keys, DataChunk &left, DataChunk &result) {
	D_ASSERT(left.ColumnCount() == result.ColumnCount());
	D_ASSERT(keys.size() == left.size());
	// create the selection vector from the matches that were found
	SelectionVector sel(STANDARD_VECTOR_SIZE);
	idx_t result_count = 0;
	for (idx_t i = 0; i < keys.size(); i++) {
		if (found_match[i] == MATCH) {
			// part of the result
			sel.set_index(result_count++, i);
		}
	}
	// construct the final result
	if (result_count > 0) {
		// we only return the columns on the left side
		// reference the columns of the left side from the result
		result.Slice(left, sel, result_count);
	} else {
		D_ASSERT(result.size() == 0);
	}
}

void ScanStructure::NextSemiJoin(DataChunk &keys, DataChunk &left, DataChunk &result) {
	// first scan for key matches
	ScanKeyMatches(keys);
	// then construct the result from all tuples with a match
	NextSemiOrAntiJoin<true>(keys, left, result);

	finished = true;
}

void ScanStructure::NextAntiJoin(DataChunk &keys, DataChunk &left, DataChunk &result) {
	// first scan for key matches
	ScanKeyMatches(keys);
	// then construct the result from all tuples that did not find a match
	NextSemiOrAntiJoin<false>(keys, left, result);

	finished = true;
}

void ScanStructure::ConstructMarkJoinResult(DataChunk &join_keys, DataChunk &child, DataChunk &result) {
	// for the initial set of columns we just reference the left side
	result.SetCardinality(child);
	for (idx_t i = 0; i < child.ColumnCount(); i++) {
		result.data[i].Reference(child.data[i]);
	}
	auto &mark_vector = result.data.back();
	mark_vector.SetVectorType(VectorType::FLAT_VECTOR);
	// first we set the NULL values from the join keys
	// if there is any NULL in the keys, the result is NULL
	auto bool_result = FlatVector::GetData<bool>(mark_vector);
	auto &mask = FlatVector::Validity(mark_vector);
	for (idx_t col_idx = 0; col_idx < join_keys.ColumnCount(); col_idx++) {
		if (ht.null_values_are_equal[col_idx]) {
			continue;
		}
		VectorData jdata;
		join_keys.data[col_idx].Orrify(join_keys.size(), jdata);
		if (!jdata.validity.AllValid()) {
			for (idx_t i = 0; i < join_keys.size(); i++) {
				auto jidx = jdata.sel->get_index(i);
				mask.Set(i, jdata.validity.RowIsValidUnsafe(jidx));
			}
		}
	}
	// now set the remaining entries to either true or false based on whether a match was found
	if (found_match) {
		for (idx_t i = 0; i < child.size(); i++) {
			bool_result[i] = found_match[i];
		}
	} else {
		memset(bool_result, 0, sizeof(bool) * child.size());
	}
	// if the right side contains NULL values, the result of any FALSE becomes NULL
	if (ht.has_null) {
		for (idx_t i = 0; i < child.size(); i++) {
			if (!bool_result[i]) {
				mask.SetInvalid(i);
			}
		}
	}
}

void ScanStructure::NextMarkJoin(DataChunk &keys, DataChunk &input, DataChunk &result) {
	D_ASSERT(result.ColumnCount() == input.ColumnCount() + 1);
	D_ASSERT(result.data.back().GetType() == LogicalType::BOOLEAN);
	// this method should only be called for a non-empty HT
	D_ASSERT(ht.count > 0);

	ScanKeyMatches(keys);
	if (ht.correlated_mark_join_info.correlated_types.empty()) {
		ConstructMarkJoinResult(keys, input, result);
	} else {
		auto &info = ht.correlated_mark_join_info;
		// there are correlated columns
		// first we fetch the counts from the aggregate hashtable corresponding to these entries
		D_ASSERT(keys.ColumnCount() == info.group_chunk.ColumnCount() + 1);
		info.group_chunk.SetCardinality(keys);
		for (idx_t i = 0; i < info.group_chunk.ColumnCount(); i++) {
			info.group_chunk.data[i].Reference(keys.data[i]);
		}
		info.correlated_counts->FetchAggregates(info.group_chunk, info.result_chunk);

		// for the initial set of columns we just reference the left side
		result.SetCardinality(input);
		for (idx_t i = 0; i < input.ColumnCount(); i++) {
			result.data[i].Reference(input.data[i]);
		}
		// create the result matching vector
		auto &last_key = keys.data.back();
		auto &result_vector = result.data.back();
		// first set the nullmask based on whether or not there were NULL values in the join key
		result_vector.SetVectorType(VectorType::FLAT_VECTOR);
		auto bool_result = FlatVector::GetData<bool>(result_vector);
		auto &mask = FlatVector::Validity(result_vector);
		switch (last_key.GetVectorType()) {
		case VectorType::CONSTANT_VECTOR:
			if (ConstantVector::IsNull(last_key)) {
				mask.SetAllInvalid(input.size());
			}
			break;
		case VectorType::FLAT_VECTOR:
			mask.Copy(FlatVector::Validity(last_key), input.size());
			break;
		default: {
			VectorData kdata;
			last_key.Orrify(keys.size(), kdata);
			for (idx_t i = 0; i < input.size(); i++) {
				auto kidx = kdata.sel->get_index(i);
				mask.Set(i, kdata.validity.RowIsValid(kidx));
			}
			break;
		}
		}

		auto count_star = FlatVector::GetData<int64_t>(info.result_chunk.data[0]);
		auto count = FlatVector::GetData<int64_t>(info.result_chunk.data[1]);
		// set the entries to either true or false based on whether a match was found
		for (idx_t i = 0; i < input.size(); i++) {
			D_ASSERT(count_star[i] >= count[i]);
			bool_result[i] = found_match ? found_match[i] : false;
			if (!bool_result[i] && count_star[i] > count[i]) {
				// RHS has NULL value and result is false: set to null
				mask.SetInvalid(i);
			}
			if (count_star[i] == 0) {
				// count == 0, set nullmask to false (we know the result is false now)
				mask.SetValid(i);
			}
		}
	}
	finished = true;
}

void ScanStructure::NextLeftJoin(DataChunk &keys, DataChunk &left, DataChunk &result) {
	// a LEFT OUTER JOIN is identical to an INNER JOIN except all tuples that do
	// not have a match must return at least one tuple (with the right side set
	// to NULL in every column)
	NextInnerJoin(keys, left, result);
	if (result.size() == 0) {
		// no entries left from the normal join
		// fill in the result of the remaining left tuples
		// together with NULL values on the right-hand side
		idx_t remaining_count = 0;
		SelectionVector sel(STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < left.size(); i++) {
			if (!found_match[i]) {
				sel.set_index(remaining_count++, i);
			}
		}
		if (remaining_count > 0) {
			// have remaining tuples
			// slice the left side with tuples that did not find a match
			result.Slice(left, sel, remaining_count);

			// now set the right side to NULL
			for (idx_t i = left.ColumnCount(); i < result.ColumnCount(); i++) {
				Vector &vec = result.data[i];
				vec.SetVectorType(VectorType::CONSTANT_VECTOR);
				ConstantVector::SetNull(vec, true);
			}
		}
		finished = true;
	}
}

void ScanStructure::NextSingleJoin(DataChunk &keys, DataChunk &input, DataChunk &result) {
	// single join
	// this join is similar to the semi join except that
	// (1) we actually return data from the RHS and
	// (2) we return NULL for that data if there is no match
	idx_t result_count = 0;
	SelectionVector result_sel(STANDARD_VECTOR_SIZE);
	SelectionVector match_sel(STANDARD_VECTOR_SIZE), no_match_sel(STANDARD_VECTOR_SIZE);
	while (this->count > 0) {
		// resolve the predicates for the current set of pointers
		idx_t match_count = ResolvePredicates(keys, match_sel, &no_match_sel);
		idx_t no_match_count = this->count - match_count;

		// mark each of the matches as found
		for (idx_t i = 0; i < match_count; i++) {
			// found a match for this index
			auto index = match_sel.get_index(i);
			found_match[index] = true;
			result_sel.set_index(result_count++, index);
		}
		// continue searching for the ones where we did not find a match yet
		AdvancePointers(no_match_sel, no_match_count);
	}
	// reference the columns of the left side from the result
	D_ASSERT(input.ColumnCount() > 0);
	for (idx_t i = 0; i < input.ColumnCount(); i++) {
		result.data[i].Reference(input.data[i]);
	}
	// now fetch the data from the RHS
	for (idx_t i = 0; i < ht.build_types.size(); i++) {
		auto &vector = result.data[input.ColumnCount() + i];
		// set NULL entries for every entry that was not found
		auto &mask = FlatVector::Validity(vector);
		mask.SetAllInvalid(input.size());
		for (idx_t j = 0; j < result_count; j++) {
			mask.SetValid(result_sel.get_index(j));
		}
		// for the remaining values we fetch the values
		GatherResult(vector, result_sel, result_sel, result_count, i + ht.condition_types.size());
	}
	result.SetCardinality(input.size());

	// like the SEMI, ANTI and MARK join types, the SINGLE join only ever does one pass over the HT per input chunk
	finished = true;
}

void JoinHashTable::ScanFullOuter(DataChunk &result, JoinHTScanState &state) {
	// scan the HT starting from the current position and check which rows from the build side did not find a match
	Vector addresses(LogicalType::POINTER);
	auto key_locations = FlatVector::GetData<data_ptr_t>(addresses);
	idx_t found_entries = 0;
	{
		lock_guard<mutex> state_lock(state.lock);
		for (; state.block_position < blocks.size(); state.block_position++, state.position = 0) {
			auto &block = blocks[state.block_position];
			auto &handle = pinned_handles[state.block_position];
			auto baseptr = handle->node->buffer;
			for (; state.position < block.count; state.position++) {
				auto tuple_base = baseptr + state.position * entry_size;
				auto found_match = Load<bool>(tuple_base + tuple_size);
				if (!found_match) {
					key_locations[found_entries++] = tuple_base;
					if (found_entries == STANDARD_VECTOR_SIZE) {
						state.position++;
						break;
					}
				}
			}
			if (found_entries == STANDARD_VECTOR_SIZE) {
				break;
			}
		}
	}
	result.SetCardinality(found_entries);
	if (found_entries > 0) {
		idx_t left_column_count = result.ColumnCount() - build_types.size();
		const auto &sel_vector = FlatVector::INCREMENTAL_SELECTION_VECTOR;
		// set the left side as a constant NULL
		for (idx_t i = 0; i < left_column_count; i++) {
			Vector &vec = result.data[i];
			vec.SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(vec, true);
		}
		// gather the values from the RHS
		for (idx_t i = 0; i < build_types.size(); i++) {
			auto &vector = result.data[left_column_count + i];
			D_ASSERT(vector.GetType() == build_types[i]);
			const auto col_no = condition_types.size() + i;
			const auto col_offset = layout.GetOffsets()[col_no];
			RowOperations::Gather(addresses, sel_vector, vector, sel_vector, found_entries, col_offset, col_no);
		}
	}
}

idx_t JoinHashTable::FillWithHTOffsets(data_ptr_t *key_locations, JoinHTScanState &state) {

	// iterate over blocks
	idx_t key_count = 0;
	while (state.block_position < blocks.size()) {
		auto &block = blocks[state.block_position];
		auto handle = buffer_manager.Pin(block.block);
		auto base_ptr = handle->node->buffer;
		// go through all the tuples within this block
		while (state.position < block.count) {
			auto tuple_base = base_ptr + state.position * entry_size;
			// store its locations
			key_locations[key_count++] = tuple_base;
			state.position++;
		}
		state.block_position++;
		state.position = 0;
	}
	return key_count;
}
} // namespace duckdb
