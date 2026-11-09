#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/sorting/sorted_run.hpp"
#include "duckdb/common/enums/memory_tag.hpp"

#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/common/sorting/sort.hpp"
#include "duckdb/common/sorting/sort_key.hpp"
#include "duckdb/common/sorting/buffered_merge.hpp"
#include "duckdb/common/types/row/block_iterator.hpp"

#include "vergesort.h"
#include "ska_sort.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// SortedRunScanState
//===--------------------------------------------------------------------===//
SortedRunScanState::SortedRunScanState(ClientContext &context, const Sort &sort_p)
    : sort(sort_p), key_executor(context, *sort.decode_sort_key) {
	key.Initialize(context, {sort.key_layout->GetTypes()[0]});
	decoded_key.Initialize(context, {sort.decode_sort_key->GetReturnType()});
}

void SortedRunScanState::Scan(const SortedRun &sorted_run, const Vector &sort_key_pointers, DataChunk &chunk) {
	const auto sort_key_type = sort.key_layout->GetSortKeyType();
	switch (sort_key_type) {
#define DUCKDB_SORT_KEY_CASE(SORT_KEY_TYPE)                                                                            \
	case SortKeyType::SORT_KEY_TYPE:                                                                                   \
		return TemplatedScan<SortKeyType::SORT_KEY_TYPE>(sorted_run, sort_key_pointers, chunk);
		DUCKDB_FOR_EACH_SORT_KEY_TYPE(DUCKDB_SORT_KEY_CASE)
#undef DUCKDB_SORT_KEY_CASE
	default:
		throw NotImplementedException("SortedRunMergerLocalState::ScanPartition for %s",
		                              EnumUtil::ToString(sort_key_type));
	}
}

void SortedRunScanState::Clear() {
	payload_state.pin_state.row_handles.clear();
	payload_state.pin_state.heap_handles.clear();
}

template <class SORT_KEY, class PHYSICAL_TYPE>
static void TemplatedGetKeyAndPayload(SORT_KEY *const *const sort_keys, SORT_KEY *temp_keys, const idx_t &count,
                                      DataChunk &key, data_ptr_t *const payload_ptrs) {
	const auto key_data = FlatVector::GetDataMutable<PHYSICAL_TYPE>(key.data[0]);
	for (idx_t i = 0; i < count; i++) {
		auto &sort_key = temp_keys[i];
		sort_key = *sort_keys[i];
		sort_key.Deconstruct(key_data[i]);
		if (SORT_KEY::HAS_PAYLOAD) {
			payload_ptrs[i] = sort_key.GetPayload();
		}
	}
	key.SetChildCardinality(count);
}

template <class SORT_KEY>
static void GetKeyAndPayload(SORT_KEY *const *const sort_keys, SORT_KEY *temp_keys, const idx_t &count, DataChunk &key,
                             data_ptr_t *const payload_ptrs) {
	const auto type_id = key.data[0].GetType().id();
	switch (type_id) {
	case LogicalTypeId::BLOB:
		return TemplatedGetKeyAndPayload<SORT_KEY, string_t>(sort_keys, temp_keys, count, key, payload_ptrs);
	case LogicalTypeId::BIGINT:
		return TemplatedGetKeyAndPayload<SORT_KEY, int64_t>(sort_keys, temp_keys, count, key, payload_ptrs);
	default:
		throw NotImplementedException("GetKeyAndPayload for %s", EnumUtil::ToString(type_id));
	}
}

template <SortKeyType SORT_KEY_TYPE>
void SortedRunScanState::TemplatedScan(const SortedRun &sorted_run, const Vector &sort_key_pointers, DataChunk &chunk) {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	const idx_t count = sort_key_pointers.size();

	const auto &output_projection_columns = sort.output_projection_columns;
	idx_t opc_idx = 0;

	const auto sort_keys = FlatVector::GetData<SORT_KEY *const>(sort_key_pointers);
	const auto payload_ptrs = FlatVector::GetDataMutable<data_ptr_t>(payload_state.chunk_state.row_locations);
	bool gathered_payload = false;

	// Decode from key
	if (!output_projection_columns[0].is_payload) {
		key.Reset();
		key_buffer.resize(count * sizeof(SORT_KEY));
		auto temp_keys = reinterpret_cast<SORT_KEY *>(key_buffer.data());
		GetKeyAndPayload(sort_keys, temp_keys, count, key, payload_ptrs);

		decoded_key.Reset();
		key_executor.Execute(key, decoded_key);

		const auto &decoded_key_entries = StructVector::GetEntries(decoded_key.data[0]);
		for (; opc_idx < output_projection_columns.size(); opc_idx++) {
			const auto &opc = output_projection_columns[opc_idx];
			if (opc.is_payload) {
				break;
			}
			chunk.data[opc.output_col_idx].Reference(decoded_key_entries[opc.layout_col_idx]);
		}

		gathered_payload = true;
	}

	// If there are no payload columns, we're done here
	if (opc_idx != output_projection_columns.size()) {
		if (!gathered_payload) {
			// Gather row pointers from keys
			for (idx_t i = 0; i < count; i++) {
				payload_ptrs[i] = sort_keys[i]->GetPayload();
			}
		}

		// Init scan state
		auto &payload_data = *sorted_run.payload_data;
		if (payload_state.pin_state.properties == TupleDataPinProperties::INVALID) {
			payload_data.InitializeScan(payload_state, TupleDataPinProperties::ALREADY_PINNED);
		}
		TupleDataCollection::ResetCachedCastVectors(payload_state.chunk_state, payload_state.chunk_state.column_ids);

		// Now gather from payload
		for (; opc_idx < output_projection_columns.size(); opc_idx++) {
			const auto &opc = output_projection_columns[opc_idx];
			D_ASSERT(opc.is_payload);
			payload_data.Gather(payload_state.chunk_state.row_locations, *FlatVector::IncrementalSelectionVector(),
			                    count, opc.layout_col_idx, chunk.data[opc.output_col_idx],
			                    *FlatVector::IncrementalSelectionVector(),
			                    payload_state.chunk_state.cached_cast_vectors[opc.layout_col_idx]);
		}
	}

	chunk.SetChildCardinality(count);
}

//===--------------------------------------------------------------------===//
// SortedRun
//===--------------------------------------------------------------------===//
SortedRun::SortedRun(ClientContext &context_p, const Sort &sort_p, bool is_index_sort_p)
    : context(context_p), sort(sort_p),
      key_data(make_uniq<TupleDataCollection>(context, sort.key_layout, MemoryTag::ORDER_BY)),
      payload_data(sort.payload_layout && sort.payload_layout->ColumnCount() != 0
                       ? make_uniq<TupleDataCollection>(context, sort.payload_layout, MemoryTag::ORDER_BY)
                       : nullptr),
      is_index_sort(is_index_sort_p), finalized(false) {
	key_data->InitializeAppend(key_append_state, TupleDataPinProperties::KEEP_EVERYTHING_PINNED);
	if (payload_data) {
		payload_data->InitializeAppend(payload_append_state, TupleDataPinProperties::KEEP_EVERYTHING_PINNED);
	}
}

unique_ptr<SortedRun> SortedRun::CreateRunForMaterialization() const {
	auto res = make_uniq<SortedRun>(context, sort, is_index_sort);
	res->key_append_state.pin_state.properties = TupleDataPinProperties::UNPIN_AFTER_DONE;
	res->payload_append_state.pin_state.properties = TupleDataPinProperties::UNPIN_AFTER_DONE;
	res->finalized = true;
	return res;
}

SortedRun::~SortedRun() {
}

template <SortKeyType SORT_KEY_TYPE>
static void TemplatedSetPayloadPointer(const Vector &key_locations, const Vector &payload_locations) {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	D_ASSERT(key_locations.size() == payload_locations.size());

	const auto key_locations_ptr = FlatVector::GetData<SORT_KEY *>(key_locations);
	const auto payload_locations_ptr = FlatVector::GetData<data_ptr_t>(payload_locations);

	for (idx_t i = 0; i < key_locations.size(); i++) {
		key_locations_ptr[i]->SetPayload(payload_locations_ptr[i]);
	}
}

static void SetPayloadPointer(const Vector &key_locations, const Vector &payload_locations,
                              const SortKeyType &sort_key_type) {
	switch (sort_key_type) {
#define DUCKDB_SORT_KEY_CASE(SORT_KEY_TYPE)                                                                            \
	case SortKeyType::SORT_KEY_TYPE:                                                                                   \
		return TemplatedSetPayloadPointer<SortKeyType::SORT_KEY_TYPE>(key_locations, payload_locations);
		DUCKDB_FOR_EACH_PAYLOAD_SORT_KEY_TYPE(DUCKDB_SORT_KEY_CASE)
#undef DUCKDB_SORT_KEY_CASE
	default:
		throw NotImplementedException("SetPayloadPointer for %s", EnumUtil::ToString(sort_key_type));
	}
}

void SortedRun::Sink(DataChunk &key, DataChunk &payload) {
	D_ASSERT(!finalized);
	key_data->Append(key_append_state, key);
	if (payload_data) {
		D_ASSERT(key.size() == payload.size());
		payload_data->Append(payload_append_state, payload);
		SetPayloadPointer(key_append_state.chunk_state.row_locations, payload_append_state.chunk_state.row_locations,
		                  key_data->GetLayout().GetSortKeyType());
	}
}

template <class SORT_KEY>
struct SkaExtractKey {
	using result_type = uint64_t;
	SkaExtractKey(bool requires_next_sort_p, idx_t ska_sort_width_p, const vector<idx_t> &sort_skippable_bytes_p,
	              atomic<ClientInterruptState> &interrupt_state_p)
	    : requires_next_sort(requires_next_sort_p), ska_sort_width(ska_sort_width_p),
	      sort_skippable_bytes(sort_skippable_bytes_p), interrupt_state(interrupt_state_p) {
	}

	const result_type &operator()(const SORT_KEY &key) const {
		return key.part0; // FIXME: this should only be used if there is a part0
	}

	bool ByteIsSkippable(const idx_t &offset) const {
		return std::find(sort_skippable_bytes.begin(), sort_skippable_bytes.end(), offset) !=
		       sort_skippable_bytes.end();
	}

	bool Interrupted() const {
		return interrupt_state.load(std::memory_order_relaxed) == ClientInterruptState::INTERRUPTED;
	}

	bool requires_next_sort;
	idx_t ska_sort_width;
	const vector<idx_t> &sort_skippable_bytes;
	atomic<ClientInterruptState> &interrupt_state;
};

template <class SORT_KEY>
struct SkaExtractSuffixKey : SkaExtractKey<SORT_KEY> {
	SkaExtractSuffixKey(const SkaExtractKey<SORT_KEY> &extract_key, uint64_t varying_bits_p)
	    : SkaExtractKey<SORT_KEY>(extract_key), varying_bits(varying_bits_p) {
		this->ska_sort_width = sizeof(uint64_t);
		this->requires_next_sort = true;
	}

	static uint64_t ExtractWord(const SORT_KEY &key, idx_t offset) {
		D_ASSERT(offset >= sizeof(uint64_t) && offset % sizeof(uint64_t) == 0);
		if constexpr (SORT_KEY::INLINE_LENGTH > sizeof(uint64_t)) {
			if (offset == sizeof(uint64_t)) {
				return key.part1;
			}
		}
		const auto size = key.GetSize();
		if (offset >= size) {
			return 0;
		}
		const auto ptr = key.GetSuffixData() + (offset - SORT_KEY::INLINE_LENGTH);
		if (size - offset >= sizeof(uint64_t)) {
			return BSwapIfLE(Load<uint64_t>(ptr));
		}
		uint64_t word = 0;
		memcpy(&word, ptr, size - offset);
		return BSwapIfLE(word);
	}

	static bool PrefixMatches(const SORT_KEY &reference, const SORT_KEY &key, idx_t offset) {
		if constexpr (SORT_KEY::INLINE_LENGTH > sizeof(uint64_t)) {
			if (offset > sizeof(uint64_t) && reference.part1 != key.part1) {
				return false;
			}
		}
		if (offset <= SORT_KEY::INLINE_LENGTH) {
			return true;
		}
		return reference.GetSize() >= offset && key.GetSize() >= offset &&
		       memcmp(reference.GetSuffixData(), key.GetSuffixData(), offset - SORT_KEY::INLINE_LENGTH) == 0;
	}

	bool ByteIsSkippable(const idx_t &byte_offset) const {
		D_ASSERT(byte_offset < sizeof(uint64_t));
		return ((varying_bits >> ((sizeof(uint64_t) - 1 - byte_offset) * 8)) & 0xff) == 0;
	}

	uint64_t varying_bits;
};

template <class SORT_KEY, class ITERATOR, class MERGE>
static void SortTiedBucket(ClientContext &context, const ITERATOR &begin, const ITERATOR &end,
                           SkaExtractKey<SORT_KEY> &extract_key, const MERGE &merge) {
	if constexpr (SORT_KEY::CONSTANT_SIZE) {
		duckdb_ska_sort::detail::StdSortFallback(begin, end, extract_key, merge);
	} else {
		context.InterruptCheck();
		D_ASSERT(end - begin >= 2);
		using SUFFIX = SkaExtractSuffixKey<SORT_KEY>;
		constexpr idx_t MIN_RADIX_COUNT = 1024;
		constexpr idx_t SAMPLE_COUNT = 16;
		constexpr idx_t MAX_PREFIX_OFFSET = 256;
		const auto count = NumericCast<idx_t>(end - begin);
		if (count < MIN_RADIX_COUNT) {
			duckdb_ska_sort::detail::StdSortFallback(begin, end, extract_key, merge);
			return;
		}

		SORT_KEY samples[SAMPLE_COUNT];
		const auto stride = (count - 1) / (SAMPLE_COUNT - 1);
		for (idx_t i = 0; i < SAMPLE_COUNT; i++) {
			samples[i] = *(begin + (i == SAMPLE_COUNT - 1 ? count - 1 : i * stride));
		}
		uint64_t words[SAMPLE_COUNT];
		uint64_t varying_bits = 0;
		idx_t offset = sizeof(uint64_t);
		while (offset <= MAX_PREFIX_OFFSET) {
			varying_bits = 0;
			for (idx_t i = 0; i < SAMPLE_COUNT; i++) {
				words[i] = SUFFIX::ExtractWord(samples[i], offset);
				varying_bits |= words[0] ^ words[i];
			}
			if (varying_bits != 0) {
				break;
			}
			offset += sizeof(uint64_t);
		}
		if (offset > MAX_PREFIX_OFFSET) {
			duckdb_ska_sort::detail::StdSortFallback(begin, end, extract_key, merge);
			return;
		}
		for (const auto word : words) {
			if (NumericCast<idx_t>(std::count(words, words + SAMPLE_COUNT, word)) >= SAMPLE_COUNT / 4) {
				duckdb_ska_sort::detail::StdSortFallback(begin, end, extract_key, merge);
				return;
			}
		}

		// The probe only selects a candidate; every skipped word must be equal across the whole bucket.
		idx_t scanned = 0;
		for (auto it = begin + 1; it != end; ++it) {
			if (++scanned % STANDARD_VECTOR_SIZE == 0) {
				context.InterruptCheck();
			}
			if (it->part0 != samples[0].part0 || !SUFFIX::PrefixMatches(samples[0], *it, offset)) {
				duckdb_ska_sort::detail::StdSortFallback(begin, end, extract_key, merge);
				return;
			}
			varying_bits |= words[0] ^ SUFFIX::ExtractWord(*it, offset);
		}
		// The common first word can cache the suffix during this synchronous refinement.
		struct PrefixRestorer {
			ITERATOR begin;
			ITERATOR end;
			uint64_t prefix;

			~PrefixRestorer() {
				for (auto it = begin; it != end; ++it) {
					it->part0 = prefix;
				}
			}
		} restore {begin, end, samples[0].part0};
		scanned = 0;
		for (auto it = begin; it != end; ++it) {
			if (++scanned % STANDARD_VECTOR_SIZE == 0) {
				context.InterruptCheck();
			}
			it->part0 = SUFFIX::ExtractWord(*it, offset);
		}
		SUFFIX suffix(extract_key, varying_bits);
		duckdb_ska_sort::ska_sort(begin, end, suffix, merge);
	}
}

template <SortKeyType SORT_KEY_TYPE>
static void TemplatedSort(ClientContext &context, TupleDataCollection &key_data, const bool is_index_sort) {
	const auto &layout = key_data.GetLayout();
	D_ASSERT(SORT_KEY_TYPE == layout.GetSortKeyType());
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	using BLOCK_ITERATOR_STATE = BlockIteratorState<BlockIteratorStateType::IN_MEMORY>;
	using BLOCK_ITERATOR = block_iterator_t<const BLOCK_ITERATOR_STATE, SORT_KEY>;

	const BLOCK_ITERATOR_STATE state(key_data);
	auto begin = BLOCK_ITERATOR(state, 0);
	auto end = BLOCK_ITERATOR(state, key_data.Count());

	const auto requires_next_sort =
	    is_index_sort ? false : !SORT_KEY::CONSTANT_SIZE || SORT_KEY::INLINE_LENGTH != sizeof(uint64_t);
	const auto ska_sort_width = MinValue<idx_t>(layout.GetSortWidth(), sizeof(uint64_t));
	const auto &sort_skippable_bytes = layout.GetSortSkippableBytes();
	auto ska_extract_key =
	    SkaExtractKey<SORT_KEY>(requires_next_sort, ska_sort_width, sort_skippable_bytes, context.interrupt_state);

	// vergesort merges the sorted runs it finds with a temporary buffer of up to half the keys - allocate that
	// buffer through the buffer allocator instead of letting std::inplace_merge take it from operator new. The same
	// buffer serves the merges of the nested vergesort that ska_sort runs over the buckets its radix passes cannot
	// order, so that no merge anywhere in this sort allocates on its own (BufferedMerge never does)
	SortMergeBuffer merge_buffer(BufferAllocator::Get(context), key_data.Count(), sizeof(SORT_KEY));
	const BufferedMergePolicy<SORT_KEY> merge(merge_buffer);
	const auto sort_ties = [&context](const BLOCK_ITERATOR &b, const BLOCK_ITERATOR &e,
	                                  SkaExtractKey<SORT_KEY> &extract_key,
	                                  const BufferedMergePolicy<SORT_KEY> &policy) {
		SortTiedBucket(context, b, e, extract_key, policy);
	};
	const auto fallback = [&ska_extract_key, &merge, &sort_ties](const BLOCK_ITERATOR &fb_begin,
	                                                             const BLOCK_ITERATOR &fb_end) {
		duckdb_ska_sort::ska_sort(fb_begin, fb_end, ska_extract_key, merge, sort_ties);
	};
	duckdb_vergesort::vergesort(begin, end, std::less<SORT_KEY>(), fallback, merge);

	context.InterruptCheck();
}

static void SortSwitch(ClientContext &context, TupleDataCollection &key_data, bool is_index_sort) {
	const auto sort_key_type = key_data.GetLayout().GetSortKeyType();
	switch (sort_key_type) {
#define DUCKDB_SORT_KEY_CASE(SORT_KEY_TYPE)                                                                            \
	case SortKeyType::SORT_KEY_TYPE:                                                                                   \
		return TemplatedSort<SortKeyType::SORT_KEY_TYPE>(context, key_data, is_index_sort);
		DUCKDB_FOR_EACH_SORT_KEY_TYPE(DUCKDB_SORT_KEY_CASE)
#undef DUCKDB_SORT_KEY_CASE
	default:
		throw NotImplementedException("TemplatedSort for %s", EnumUtil::ToString(sort_key_type));
	}
}

template <class SORT_KEY>
static void ReorderKeyData(TupleDataCollection &new_key_data, TupleDataAppendState &new_key_data_append_state,
                           TupleDataChunkState &input, const idx_t &count) {
	D_ASSERT(!SORT_KEY::CONSTANT_SIZE);
	const auto row_locations = FlatVector::GetData<const SORT_KEY *>(input.row_locations);
	const auto heap_locations = FlatVector::GetDataMutable<data_ptr_t>(input.heap_locations);
	const auto heap_sizes = FlatVector::GetDataMutable<idx_t>(input.heap_sizes);
	for (idx_t i = 0; i < count; i++) {
		const auto &sort_key = *row_locations[i];
		heap_locations[i] = sort_key.GetData();
		heap_sizes[i] = sort_key.GetHeapSize();
	}

	new_key_data_append_state.chunk_state.heap_sizes.Reference(input.heap_sizes);
	new_key_data.Build(new_key_data_append_state.pin_state, new_key_data_append_state.chunk_state, 0, count);
	new_key_data.CopyRows(new_key_data_append_state.chunk_state, input, *FlatVector::IncrementalSelectionVector(),
	                      count);
}

template <class SORT_KEY>
static void ReorderPayloadData(TupleDataCollection &new_payload_data,
                               TupleDataAppendState &new_payload_data_append_state, SORT_KEY *const *const key_ptrs,
                               TupleDataChunkState &input, const idx_t &count) {
	D_ASSERT(SORT_KEY::HAS_PAYLOAD);
	const auto row_locations = FlatVector::GetDataMutable<data_ptr_t>(input.row_locations);
	for (idx_t i = 0; i < count; i++) {
		const auto &sort_key = *key_ptrs[i];
		row_locations[i] = sort_key.GetPayload();
	}

	if (!new_payload_data.GetLayout().AllConstant()) {
		new_payload_data.FindHeapPointers(input, count);
	}
	new_payload_data_append_state.chunk_state.heap_sizes.Reference(input.heap_sizes);
	new_payload_data.Build(new_payload_data_append_state.pin_state, new_payload_data_append_state.chunk_state, 0,
	                       count);
	new_payload_data.CopyRows(new_payload_data_append_state.chunk_state, input,
	                          *FlatVector::IncrementalSelectionVector(), count);
}

template <SortKeyType SORT_KEY_TYPE>
static void TemplatedReorder(ClientContext &context, unique_ptr<TupleDataCollection> &key_data,
                             unique_ptr<TupleDataCollection> &payload_data) {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	using BLOCK_ITERATOR_STATE = BlockIteratorState<BlockIteratorStateType::IN_MEMORY>;

	// Initialize new key data (if necessary)
	unique_ptr<TupleDataCollection> new_key_data;
	TupleDataAppendState new_key_data_append_state;
	if (!SORT_KEY::CONSTANT_SIZE) {
		new_key_data = key_data->CreateUnique();
		new_key_data->InitializeAppend(new_key_data_append_state, TupleDataPinProperties::UNPIN_AFTER_DONE);
	}

	// Initialize new payload data (if necessary)
	unique_ptr<TupleDataCollection> new_payload_data;
	TupleDataAppendState new_payload_data_append_state;
	if (SORT_KEY::HAS_PAYLOAD) {
		new_payload_data = payload_data->CreateUnique();
		new_payload_data->InitializeAppend(new_payload_data_append_state, TupleDataPinProperties::UNPIN_AFTER_DONE);
	}

	// These states will be populated for appends
	TupleDataChunkState new_key_data_input;
	TupleDataChunkState new_payload_data_input;
	const auto key_ptrs = FlatVector::GetDataMutable<SORT_KEY *>(new_key_data_input.row_locations);

	// Iterate over sort keys
	const idx_t total_count = key_data->Count();
	const BLOCK_ITERATOR_STATE state(*key_data);
	auto it = block_iterator_t<const BLOCK_ITERATOR_STATE, SORT_KEY>(state, 0);

	idx_t index = 0;
	while (index < total_count) {
		context.InterruptCheck();

		const auto next = MinValue<idx_t>(total_count - index, STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < next; i++) {
			key_ptrs[i] = &*it++;
		}
		if (SORT_KEY::HAS_PAYLOAD) {
			ReorderPayloadData<SORT_KEY>(*new_payload_data, new_payload_data_append_state, key_ptrs,
			                             new_payload_data_input, next);
			const auto new_payload_locations =
			    FlatVector::GetData<const data_ptr_t>(new_payload_data_append_state.chunk_state.row_locations);
			for (idx_t i = 0; i < next; i++) {
				auto &key = *key_ptrs[i];
				key.SetPayload(new_payload_locations[i]);
			}
		}
		if (!SORT_KEY::CONSTANT_SIZE) {
			ReorderKeyData<SORT_KEY>(*new_key_data, new_key_data_append_state, new_key_data_input, next);
		}
		index += next;
	}
	D_ASSERT(index == total_count);

	key_data->Unpin();
	if (!SORT_KEY::CONSTANT_SIZE) {
		new_key_data->FinalizePinState(new_key_data_append_state.pin_state);
		key_data = std::move(new_key_data);
	}

	if (SORT_KEY::HAS_PAYLOAD) {
		new_payload_data->FinalizePinState(new_payload_data_append_state.pin_state);
		new_payload_data->Unpin();
		payload_data = std::move(new_payload_data);
	}
}

static void Reorder(ClientContext &context, unique_ptr<TupleDataCollection> &key_data,
                    unique_ptr<TupleDataCollection> &payload_data) {
	const auto sort_key_type = key_data->GetLayout().GetSortKeyType();
	switch (sort_key_type) {
#define DUCKDB_SORT_KEY_CASE(SORT_KEY_TYPE)                                                                            \
	case SortKeyType::SORT_KEY_TYPE:                                                                                   \
		return TemplatedReorder<SortKeyType::SORT_KEY_TYPE>(context, key_data, payload_data);
		DUCKDB_FOR_EACH_REORDERABLE_SORT_KEY_TYPE(DUCKDB_SORT_KEY_CASE)
#undef DUCKDB_SORT_KEY_CASE
	default:
		throw NotImplementedException("TemplatedReorderPayload for %s", EnumUtil::ToString(sort_key_type));
	}
}

void SortedRun::Finalize(bool external) {
	D_ASSERT(!finalized);

	// Finalize the append
	key_data->FinalizePinState(key_append_state.pin_state);
	key_data->VerifyEverythingPinned();
	if (payload_data) {
		D_ASSERT(key_data->Count() == payload_data->Count());
		payload_data->FinalizePinState(payload_append_state.pin_state);
		payload_data->VerifyEverythingPinned();
	}

	// Sort the fixed-size portion of the keys
	SortSwitch(context, *key_data, is_index_sort);

	if (external) {
		ReorderForExternal();
	}

	finalized = true;
}

void SortedRun::MakeExternal() {
	D_ASSERT(finalized);
	if (!external) {
		ReorderForExternal();
	}
}

void SortedRun::ReorderForExternal() {
	D_ASSERT(!external);
	// Reorder variable-size portion of keys and/or payload data (if necessary)
	const auto sort_key_type = key_data->GetLayout().GetSortKeyType();
	if (!SortKeyUtils::IsConstantSize(sort_key_type) || SortKeyUtils::HasPayload(sort_key_type)) {
		Reorder(context, key_data, payload_data);
	} else {
		// This ensures keys are unpinned even if they are constant size and have no payload
		key_data->Unpin();
	}
	external = true;
}

static void DestroyData(TupleDataCollection &data, const idx_t tuple_idx_begin, const idx_t tuple_idx_end) {
	if (tuple_idx_begin == tuple_idx_end) {
		return;
	}
	// Only the final chunk of a sorted run can be partial.
	const auto chunk_idx_start = tuple_idx_begin / STANDARD_VECTOR_SIZE;
	const auto chunk_idx_end = tuple_idx_end == data.Count() ? data.ChunkCount() : tuple_idx_end / STANDARD_VECTOR_SIZE;
	if (chunk_idx_start == chunk_idx_end) {
		return;
	}
	data.DestroyChunks(chunk_idx_start, chunk_idx_end);
}

void SortedRun::DestroyKeyData(const idx_t tuple_idx_begin, const idx_t tuple_idx_end) {
	DestroyData(*key_data, tuple_idx_begin, tuple_idx_end);
}

void SortedRun::DestroyPayloadData(const idx_t tuple_idx_begin, const idx_t tuple_idx_end) {
	D_ASSERT(payload_data);
	if (tuple_idx_begin == tuple_idx_end) {
		return;
	}
	const auto at_end = tuple_idx_end == payload_data->Count();
	const auto chunk_end = at_end ? payload_data->ChunkCount() : tuple_idx_end / STANDARD_VECTOR_SIZE;
	// Payload fetching is bounded to the fragment, so fully consumed parts can retire within a chunk.
	payload_data->DestroyChunks(tuple_idx_begin / STANDARD_VECTOR_SIZE, chunk_end,
	                            tuple_idx_begin % STANDARD_VECTOR_SIZE,
	                            at_end ? 0 : tuple_idx_end % STANDARD_VECTOR_SIZE);
}

idx_t SortedRun::Count() const {
	return key_data->Count();
}

idx_t SortedRun::SizeInBytes() const {
	idx_t size = key_data->SizeInBytes();
	if (payload_data) {
		size += payload_data->SizeInBytes();
	}
	return size;
}

idx_t SortedRun::SizeInBytesForSorting() const {
	// the fixed-size sort keys are the rows of the key layout, so the row width is the key size the merge buffer uses
	return SizeInBytes() + SortMergeBuffer::MaximumSize(key_data->Count(), key_data->GetLayout().GetRowWidth());
}

} // namespace duckdb
