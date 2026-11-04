#include "duckdb/execution/join_hashtable.hpp"

#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/radix_partitioning.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/ht_entry.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/mark_join_row_comparison.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/planner/joinside.hpp"

namespace duckdb {

using ValidityBytes = JoinHashTable::ValidityBytes;
using ScanStructure = JoinHashTable::ScanStructure;
using ProbeSpill = JoinHashTable::ProbeSpill;
using ProbeSpillLocalState = JoinHashTable::ProbeSpillLocalAppendState;

JoinHashTable::SharedState::SharedState()
    : salt_v(LogicalType::UBIGINT), keys_to_compare_sel(STANDARD_VECTOR_SIZE), keys_no_match_sel(STANDARD_VECTOR_SIZE) {
}

JoinHashTable::ProbeState::ProbeState()
    : SharedState(), ht_offsets_and_salts_v(LogicalType::UBIGINT), hashes_dense_v(LogicalType::HASH),
      non_empty_sel(STANDARD_VECTOR_SIZE) {
}

JoinHashTable::ProbeDictionaryState::ProbeDictionaryState()
    : hashes(LogicalType::HASH), new_dictionary_pointers(LogicalType::POINTER), unique_entries(STANDARD_VECTOR_SIZE),
      match_sel(STANDARD_VECTOR_SIZE) {
}

JoinHashTable::InsertState::InsertState(const JoinHashTable &ht)
    : SharedState(), remaining_sel(STANDARD_VECTOR_SIZE), key_match_sel(STANDARD_VECTOR_SIZE),
      rhs_row_locations(LogicalType::POINTER) {
	ht.data_collection->InitializeChunk(lhs_data, ht.equality_predicate_columns);
	ht.data_collection->InitializeChunkState(chunk_state, ht.equality_predicate_columns);
}

JoinHashTable::JoinHashTable(ClientContext &context_p, const PhysicalOperator &op_p,
                             const vector<JoinCondition> &conditions_p, vector<LogicalType> btypes, JoinType type_p,
                             const idx_t initial_radix_bits, const vector<idx_t> &output_columns_p,
                             unique_ptr<ResidualPredicateInfo> residual_p, optional_ptr<Expression> predicate_ptr,
                             const vector<idx_t> &output_in_probe)
    : context(context_p), op(op_p), buffer_manager(BufferManager::GetBufferManager(context)), conditions(conditions_p),
      build_types(std::move(btypes)), output_columns(output_columns_p), entry_size(0), tuple_size(0),
      vfound(Value::BOOLEAN(false), count_t(STANDARD_VECTOR_SIZE)), join_type(type_p), finalized(false),
      has_null(false), residual_predicate(predicate_ptr), radix_bits(initial_radix_bits) {
	// store residual predicate information
	residual_info = std::move(residual_p);
	lhs_output_in_probe = output_in_probe;

	for (idx_t i = 0; i < conditions.size(); ++i) {
		auto &condition = conditions[i];
		D_ASSERT(condition.IsComparison());
		D_ASSERT(condition.GetLHS().GetReturnType() == condition.GetRHS().GetReturnType());
		auto type = condition.GetLHS().GetReturnType();
		if (condition.GetComparisonType() == ExpressionType::COMPARE_EQUAL ||
		    condition.GetComparisonType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			// ensure that all equality conditions are at the front,
			// and that all other conditions are at the back
			D_ASSERT(equality_types.size() == condition_types.size());
			equality_types.push_back(type);
			equality_predicates.push_back(condition.GetComparisonType());
			equality_predicate_columns.push_back(i);

		} else {
			// all non-equality conditions are at the back
			non_equality_predicates.push_back(condition.GetComparisonType());
			non_equality_predicate_columns.push_back(i);
		}

		null_values_are_equal.push_back(condition.GetComparisonType() == ExpressionType::COMPARE_DISTINCT_FROM ||
		                                condition.GetComparisonType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM);

		condition_types.push_back(type);
	}
	// at least one equality is necessary
	D_ASSERT(!equality_types.empty());

	if (join_type == JoinType::SINGLE) {
		single_join_error_on_multiple_rows = Settings::Get<ScalarSubqueryErrorOnMultipleRowsSetting>(context);
	}

	if (non_equality_predicates.empty() && !residual_predicate &&
	    (join_type == JoinType::SEMI || join_type == JoinType::ANTI || join_type == JoinType::MARK)) {
		insert_duplicate_keys = false;
	}

	InitializePartitionMasks();
	// Layout-dependent state is published lazily on the first Sink chunk (see FinishInitWithLayout).
}

void JoinHashTable::FinishInitWithLayout(shared_ptr<TupleDataLayout> published_layout,
                                         shared_ptr<const DictSurvivingInfo> dict_info_p) {
	D_ASSERT(!layout_ptr);
	layout_ptr = std::move(published_layout);
	D_ASSERT(!dict_info_p || dict_info_p->columns.size() == build_types.size());
	dict_info = std::move(dict_info_p);

	// Initialize the row matcher that are used for filtering during the probing only if there are non-equality preds
	if (!non_equality_predicates.empty()) {
		row_matcher_probe = unique_ptr<RowMatcher>(new RowMatcher());
		row_matcher_probe_no_match_sel = unique_ptr<RowMatcher>(new RowMatcher());

		row_matcher_probe->Initialize(false, *layout_ptr, non_equality_predicates, non_equality_predicate_columns);
		row_matcher_probe_no_match_sel->Initialize(true, *layout_ptr, non_equality_predicates,
		                                           non_equality_predicate_columns);

		needs_chain_matcher = true;
	} else {
		needs_chain_matcher = false;
	}

	chains_longer_than_one.store(false, std::memory_order_relaxed);
	// a fresh matcher: Initialize appends, and a recursive CTE iteration publishes the layout again
	row_matcher_build = RowMatcher();
	row_matcher_build.Initialize(true, *layout_ptr, equality_predicates);

	const auto &offsets = layout_ptr->GetOffsets();
	tuple_size = offsets[condition_types.size() + build_types.size()];
	pointer_offset = offsets.back();
	entry_size = layout_ptr->GetRowWidth();

	data_collection =
	    make_uniq<TupleDataCollection>(buffer_manager, layout_ptr, MemoryTag::HASH_TABLE, nullptr, context);
	sink_collection = make_uniq<RadixPartitionedTupleData>(buffer_manager, layout_ptr, MemoryTag::HASH_TABLE,
	                                                       radix_bits, layout_ptr->ColumnCount() - 1, context);

	dead_end = make_unsafe_uniq_array_uninitialized<data_t>(layout_ptr->GetRowWidth());
	memset(dead_end.get(), 0, layout_ptr->GetRowWidth());
}

JoinHashTable::~JoinHashTable() {
}

void JoinHashTable::InitializeUncorrelatedMarkJoin() {
	D_ASSERT(join_type == JoinType::MARK);
	D_ASSERT(mark_join_info.correlated_types.empty());
	mark_join_info.uncorrelated_has_null = false;
	mark_join_info.uncorrelated_condition_rows = make_uniq<ColumnDataCollection>(context, condition_types);
}

bool JoinHashTable::HasUncorrelatedMarkJoin() const {
	return mark_join_info.uncorrelated_condition_rows != nullptr;
}

void JoinHashTable::Merge(JoinHashTable &other) {
	{
		lock_guard<mutex> guard(data_lock);
		data_collection->Combine(*other.data_collection);
	}

	if (join_type == JoinType::MARK) {
		auto &info = mark_join_info;
		lock_guard<mutex> mj_lock(info.mj_lock);
		has_null = has_null || other.has_null;
		if (!info.correlated_types.empty()) {
			auto &other_info = other.mark_join_info;
			info.correlated_counts->Combine(*other_info.correlated_counts);
		}
		if (info.uncorrelated_condition_rows && other.mark_join_info.uncorrelated_condition_rows) {
			info.uncorrelated_has_null = info.uncorrelated_has_null || other.mark_join_info.uncorrelated_has_null;
			info.uncorrelated_condition_rows->Combine(*other.mark_join_info.uncorrelated_condition_rows);
		}
	}

	// every thread-local table shares this operator's filter - a different one here would mean its rows
	// never reached the filter that gets published
	D_ASSERT(!other.bloom_filter || bloom_filter.get() == other.bloom_filter.get());

	sink_collection->Combine(*other.sink_collection);

	// dict_info is the descriptor shared by every per-thread JHT of this join; there is nothing to reconcile
}

static void ApplyBitmaskAndGetSaltBuild(Vector &hashes_v, Vector &salt_v, const idx_t &bitmask, idx_t shift) {
	const idx_t count = hashes_v.size();
	if (hashes_v.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		auto &hash = *ConstantVector::GetData<hash_t>(hashes_v);
		salt_v.SetVectorType(VectorType::CONSTANT_VECTOR);

		*ConstantVector::GetData<hash_t>(salt_v) = ht_entry_t::ExtractSalt(hash);
		salt_v.Flatten();

		hash = (hash >> shift) & bitmask;
		hashes_v.Flatten();
	} else {
		hashes_v.Flatten();
		auto salt_data = FlatVector::Writer<hash_t>(salt_v, count_t(count));
		auto hashes = FlatVector::GetDataMutable<hash_t>(hashes_v);
		for (idx_t i = 0; i < count; i++) {
			salt_data.WriteValue(ht_entry_t::ExtractSalt(hashes[i]));
			hashes[i] = (hashes[i] >> shift) & bitmask;
		}
	}
}

template <bool HAS_SEL>
idx_t GetOptionalIndex(optional_ptr<const SelectionVector> sel, const idx_t idx) {
	return HAS_SEL ? sel->get_index(idx) : idx;
}

static void AddPointerToCompare(SelectionVector &compare_sel, const ht_entry_t &entry, data_ptr_t *row_ptr_insert_to,
                                idx_t *ht_offsets_and_salts, idx_t row_ht_offset, idx_t &keys_to_compare_count,
                                const idx_t &row_index) {
	compare_sel.set_index(keys_to_compare_count, row_index);
	row_ptr_insert_to[row_index] = entry.GetPointer();
	// If the key does not match, we have to continue linear probing, we need to store the ht_offset and the salt
	// for this element based on the row_index. We can't get the offset from the hash as we already might have
	// some linear probing steps when arriving here.
	ht_offsets_and_salts[row_index] = row_ht_offset | entry.GetSaltWithNulls();
	keys_to_compare_count += 1;
}

template <bool USE_SALTS, bool HAS_SEL>
static idx_t ProbeForPointersInternal(JoinHashTable::ProbeState &state, JoinHashTable &ht,
                                      unsafe_optional_ptr<ht_entry_t> entries, Vector &pointers_result_v,
                                      optional_ptr<const SelectionVector> row_sel, idx_t &count) {
	auto hashes_dense = FlatVector::GetDataMutable<hash_t>(state.hashes_dense_v);
	const auto bitmask = ht.bitmask;

	const auto row_ptr_insert_to = FlatVector::GetDataMutable<data_ptr_t>(pointers_result_v);
	const auto ht_offsets_and_salts = FlatVector::GetDataMutable<idx_t>(state.ht_offsets_and_salts_v);
	idx_t keys_to_compare_count = 0;

	for (idx_t i = 0; i < count; i++) {
		PrefetchHashTableEntry(i, count,
		                       [&](idx_t next_idx) { return &entries.get()[hashes_dense[next_idx] & bitmask]; });
		auto row_hash = hashes_dense[i]; // hashes have been flattened before -> always access dense
		auto row_ht_offset = row_hash & bitmask;

		if (USE_SALTS) {
			// increment the ht_offset of the entry as long as the next entry is occupied and salt does not match
			while (true) {
				const ht_entry_t entry = entries.get()[row_ht_offset];
				const bool occupied = entry.IsOccupied();

				// the entry is empty -> no match possible
				if (!occupied) {
					break;
				}

				const hash_t row_salt = ht_entry_t::ExtractSalt(row_hash);
				const bool salt_match = entry.GetSalt() == row_salt;
				if (salt_match) {
					// we know that the entry is occupied and the salt matches -> compare the keys
					auto row_index = GetOptionalIndex<HAS_SEL>(row_sel, i);
					AddPointerToCompare(state.keys_to_compare_sel, entry, row_ptr_insert_to, ht_offsets_and_salts,
					                    row_ht_offset, keys_to_compare_count, row_index);
					break;
				}

				// full and salt do not match -> continue probing
				IncrementAndWrap(row_ht_offset, bitmask);
			}
		} else {
			const ht_entry_t entry = entries.get()[row_ht_offset];
			const bool occupied = entry.IsOccupied();
			if (occupied) {
				// the entry is occupied -> compare the keys
				auto row_index = GetOptionalIndex<HAS_SEL>(row_sel, i);
				AddPointerToCompare(state.keys_to_compare_sel, entry, row_ptr_insert_to, ht_offsets_and_salts,
				                    row_ht_offset, keys_to_compare_count, row_index);
			}
		}
	}

	return keys_to_compare_count;
}

/// for each entry, do linear probing until
/// a) an empty entry is found
///	   -> no match
/// b) an entry is found where (and the salt matches if USE_SALTS is true)
///	   -> match, add to compare sel and increase found count
template <bool USE_SALTS>
static idx_t ProbeForPointers(JoinHashTable::ProbeState &state, JoinHashTable &ht,
                              unsafe_optional_ptr<ht_entry_t> entries, Vector &pointers_result_v,
                              optional_ptr<const SelectionVector> row_sel, idx_t count, const bool has_row_sel) {
	if (has_row_sel) {
		return ProbeForPointersInternal<USE_SALTS, true>(state, ht, entries, pointers_result_v, row_sel, count);
	} else {
		return ProbeForPointersInternal<USE_SALTS, false>(state, ht, entries, pointers_result_v, row_sel, count);
	}
}

//! Gets a pointer to the entry in the HT for each of the hashes_v using linear probing. Will update the key_match_sel
//! vector and the count argument to the number and position of the matches
template <bool USE_SALTS>
static void GetRowPointersInternal(DataChunk &keys, TupleDataChunkState &key_state, JoinHashTable::ProbeState &state,
                                   Vector &hashes_v, optional_ptr<const SelectionVector> row_sel, idx_t &count,
                                   JoinHashTable &ht, unsafe_optional_ptr<ht_entry_t> entries,
                                   Vector &pointers_result_v, SelectionVector &match_sel, bool has_row_sel) {
	// Densify original hashes, mapping resident buckets once. Retries below already contain offsets and salt.
	const auto input_count = count;
	const auto bucket_shift = ht.GetBucketShift();
	const auto bitmask = ht.bitmask;
	if (has_row_sel || bucket_shift) {
		auto hashes_unified = hashes_v.Values<hash_t>();
		auto hashes_dense = FlatVector::Writer<idx_t>(state.hashes_dense_v, input_count);

		for (idx_t i = 0; i < input_count; i++) {
			const auto row_index = has_row_sel ? row_sel->get_index(i) : i;
			const auto hash = hashes_unified[row_index].GetValue();
			hashes_dense.WriteValue(bucket_shift ? (hash & ht_entry_t::SALT_MASK) | ((hash >> bucket_shift) & bitmask)
			                                     : hash);
		}
	} else {
		VectorOperations::Copy(hashes_v, state.hashes_dense_v, input_count, 0, 0);
	}

	// the number of keys that match for all iterations of the following loop
	idx_t match_count = 0;

	idx_t keys_no_match_count;
	idx_t elements_to_probe_count = count;

	do {
		const idx_t keys_to_compare_count = ProbeForPointers<USE_SALTS>(state, ht, entries, pointers_result_v, row_sel,
		                                                                elements_to_probe_count, has_row_sel);

		// if there are no keys to compare, we are done
		if (keys_to_compare_count == 0) {
			break;
		}

		// Perform row comparisons, after Match function call salt_match_sel will point to the keys that match
		keys_no_match_count = 0;
		const idx_t keys_match_count =
		    ht.row_matcher_build.Match(keys, key_state.vector_data, state.keys_to_compare_sel, keys_to_compare_count,
		                               pointers_result_v, &state.keys_no_match_sel, keys_no_match_count);

		D_ASSERT(keys_match_count + keys_no_match_count == keys_to_compare_count);

		// add the indices to the match_sel
		for (idx_t i = 0; i < keys_match_count; i++) {
			const auto row_index = state.keys_to_compare_sel.get_index(i);
			match_sel.set_index(match_count, row_index);
			match_count++;
		}

		const auto ht_offsets_and_salts = FlatVector::GetData<idx_t>(state.ht_offsets_and_salts_v);
		const auto hashes_dense = FlatVector::GetDataMutable<hash_t>(state.hashes_dense_v);

		// For all the non-matches, increment the offset to continue probing but keep the salt intact
		for (idx_t i = 0; i < keys_no_match_count; i++) {
			const auto row_index = state.keys_no_match_sel.get_index(i);
			auto ht_offset_and_salt = ht_offsets_and_salts[row_index];
			IncrementAndWrap(ht_offset_and_salt, bitmask | ht_entry_t::SALT_MASK);
			hashes_dense[i] = ht_offset_and_salt; // populate dense again
		}

		// in the next iteration, we have a selection vector with the keys that do not match
		row_sel = state.keys_no_match_sel;
		has_row_sel = true;

		elements_to_probe_count = keys_no_match_count;

	} while (DUCKDB_UNLIKELY(keys_no_match_count > 0));

	// set the count to the number of matches
	count = match_count;
}

inline bool JoinHashTable::UseSalt() const {
	// only use salt for large hash tables
	return this->capacity > USE_SALT_THRESHOLD;
}

void JoinHashTable::GetRowPointers(DataChunk &keys, TupleDataChunkState &key_state, ProbeState &state, Vector &hashes_v,
                                   optional_ptr<const SelectionVector> sel, idx_t &count, Vector &pointers_result_v,
                                   SelectionVector &match_sel, const bool has_sel) {
	auto entries = GetEntries();
	if (UseSalt()) {
		GetRowPointersInternal<true>(keys, key_state, state, hashes_v, sel, count, *this, entries, pointers_result_v,
		                             match_sel, has_sel);
	} else {
		GetRowPointersInternal<false>(keys, key_state, state, hashes_v, sel, count, *this, entries, pointers_result_v,
		                              match_sel, has_sel);
	}
}

void JoinHashTable::Hash(DataChunk &keys, const SelectionVector &sel, idx_t count, Vector &hashes) {
	if (count == keys.size()) {
		// no null values are filtered: use regular hash functions
		VectorOperations::Hash(keys.data[0], hashes, count);
		for (idx_t i = 1; i < equality_types.size(); i++) {
			VectorOperations::CombineHash(hashes, keys.data[i], count);
		}
	} else {
		// null values were filtered: use selection vector
		VectorOperations::Hash(keys.data[0], hashes, sel, count);
		for (idx_t i = 1; i < equality_types.size(); i++) {
			VectorOperations::CombineHash(hashes, keys.data[i], sel, count);
		}
	}
}

static idx_t FilterNullValues(UnifiedVectorFormat &vdata, const SelectionVector &sel, idx_t count,
                              SelectionVector &result) {
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

// A dictionary of D slots uses indices 0..D-1, so D slots fit a W-byte index iff D <= 2^(8*W).
static constexpr idx_t DICT_INDEX_UINT8_CAPACITY = 256;
static constexpr idx_t DICT_INDEX_UINT16_CAPACITY = 65536;

//! Narrowest unsigned-integer byte width that can hold any index into a dictionary of dict_size slots
static uint8_t DictIndexWidth(idx_t dict_size) {
	if (dict_size <= DICT_INDEX_UINT8_CAPACITY) {
		return sizeof(uint8_t);
	}
	if (dict_size <= DICT_INDEX_UINT16_CAPACITY) {
		return sizeof(uint16_t);
	}
	return sizeof(uint32_t);
}

//! Materialise a flat width-INDEX_T index vector (allocation owned by `buffers`, view in `vectors`) from the
//! dict sel. UnsafeNumericCast is safe: the publisher sized the width to the dictionary, so every index fits.
template <class INDEX_T>
static void ScatterDictIndices(const SelectionVector &dict_sel, idx_t count, const LogicalType &index_type,
                               Allocator &allocator, vector<AllocatedData> &buffers, vector<Vector> &vectors) {
	buffers.emplace_back(allocator.Allocate(count * sizeof(INDEX_T)));
	vectors.emplace_back(index_type, buffers.back().get(), count);
	auto idx_data = FlatVector::GetDataMutable<INDEX_T>(vectors.back());
	for (idx_t r = 0; r < count; r++) {
		idx_data[r] = UnsafeNumericCast<INDEX_T>(dict_sel.get_index(r));
	}
}

//! Dispatch on the chosen index width (1/2/4 B) to the matching templated ScatterDictIndices
static void ScatterDictIndices(uint8_t index_width, const SelectionVector &dict_sel, idx_t count, Allocator &allocator,
                               vector<AllocatedData> &buffers, vector<Vector> &vectors) {
	switch (index_width) {
	case sizeof(uint8_t):
		ScatterDictIndices<uint8_t>(dict_sel, count, LogicalType::UTINYINT, allocator, buffers, vectors);
		break;
	case sizeof(uint16_t):
		ScatterDictIndices<uint16_t>(dict_sel, count, LogicalType::USMALLINT, allocator, buffers, vectors);
		break;
	default:
		ScatterDictIndices<uint32_t>(dict_sel, count, LogicalType::UINTEGER, allocator, buffers, vectors);
		break;
	}
}

//! Load the per-match dict index (width INDEX_T) from each matched row's slot at col_offset into build_sel_vec
template <class INDEX_T>
static void LoadDictIndices(const data_ptr_t *ptrs, const SelectionVector &ptr_sel, idx_t count, idx_t col_offset,
                            SelectionVector &build_sel_vec) {
	for (idx_t i = 0; i < count; i++) {
		build_sel_vec.set_index(i, Load<INDEX_T>(ptrs[ptr_sel.get_index(i)] + col_offset));
	}
}

//! Dispatch on the chosen index width (1/2/4 B) to the matching templated LoadDictIndices
static void LoadDictIndices(uint8_t index_width, const data_ptr_t *ptrs, const SelectionVector &ptr_sel, idx_t count,
                            idx_t col_offset, SelectionVector &build_sel_vec) {
	switch (index_width) {
	case sizeof(uint8_t):
		LoadDictIndices<uint8_t>(ptrs, ptr_sel, count, col_offset, build_sel_vec);
		break;
	case sizeof(uint16_t):
		LoadDictIndices<uint16_t>(ptrs, ptr_sel, count, col_offset, build_sel_vec);
		break;
	default:
		LoadDictIndices<uint32_t>(ptrs, ptr_sel, count, col_offset, build_sel_vec);
		break;
	}
}

bool JoinHashTable::ColumnReferencedByResidual(idx_t build_col_idx) const {
	if (!residual_info) {
		return false;
	}
	const auto layout_col = condition_types.size() + build_col_idx;
	for (const auto &kv : residual_info->build_input_to_layout_map) {
		if (kv.second == layout_col) {
			return true;
		}
	}
	return false;
}

uint8_t JoinHashTable::GetDictSurvivingIndexWidth(idx_t build_col_idx, const Vector &incoming) const {
	const auto &type = build_types[build_col_idx];
	// Vector::Dictionary rejects nested types
	switch (type.InternalType()) {
	case PhysicalType::STRUCT:
	case PhysicalType::LIST:
	case PhysicalType::ARRAY:
		return 0;
	default:
		break;
	}
	// residual predicates read from the row slot directly; a narrowed slot would corrupt them
	if (ColumnReferencedByResidual(build_col_idx)) {
		return 0;
	}
	if (incoming.GetVectorType() != VectorType::DICTIONARY_VECTOR) {
		return 0;
	}
	if (!DictionaryVector::IsGlobalDictionary(incoming)) {
		return 0;
	}
	// an empty/invalid dictionary keeps native width
	const auto dict_size = DictionaryVector::DictionarySize(incoming);
	if (!dict_size.IsValid()) {
		return 0;
	}
	// only narrow when strictly smaller than the native slot (never regress; e.g. INTEGER over a D<=255 dict).
	// One extra slot is reserved for the NULL index that encodes NULL-only chunks (see CreateDictSurvivingColumn).
	const uint8_t index_width = DictIndexWidth(dict_size.GetIndex() + 1);
	const auto native_bytes = GetTypeIdSize(type.InternalType());
	if (index_width >= native_bytes) {
		return 0;
	}
	return index_width;
}

LogicalType DictSurvivingInfo::IndexType(uint8_t index_width) {
	switch (index_width) {
	case sizeof(uint8_t):
		return LogicalType::UTINYINT;
	case sizeof(uint16_t):
		return LogicalType::USMALLINT;
	default:
		D_ASSERT(index_width == sizeof(uint32_t));
		return LogicalType::UINTEGER;
	}
}

DictSurvivingColumn JoinHashTable::CreateDictSurvivingColumn(const Vector &incoming, uint8_t index_width) {
	D_ASSERT(index_width != 0);
	D_ASSERT(incoming.GetVectorType() == VectorType::DICTIONARY_VECTOR);
	D_ASSERT(DictionaryVector::IsGlobalDictionary(incoming));
	// The upstream child is a zero-copy gather: its long strings point into the producer's row-store heap, recycled
	// before we gather on probe. Deep-copy into a self-owned entry so it outlives them, and append one NULL slot at
	// index upstream_count: NULL-only chunks (the unmatched-build scan of an upstream FULL/RIGHT join fills the probe
	// columns with NULL) are stored as that index, so the regular dictionary gather reconstructs them.
	const auto &upstream_child = DictionaryVector::Child(incoming);
	const auto upstream_count = upstream_child.size();
	// GetDictSurvivingIndexWidth sized the width to upstream_count + 1
	D_ASSERT(upstream_count < (idx_t(1) << (8 * index_width)));
	DictSurvivingColumn column;
	column.index_width = index_width;
	// CreateReusableGlobalDictionary assigns a fresh id: this is a different dictionary than the upstream one (one
	// more slot), so a downstream consumer must not treat the two as interchangeable.
	column.entry = DictionaryVector::CreateReusableGlobalDictionary(upstream_child.GetType(), upstream_count + 1);
	if (upstream_count > 0) {
		VectorOperations::Copy(upstream_child, column.entry->data, upstream_count, 0, 0);
	}
	FlatVector::SetNull(column.entry->data, upstream_count, true);
	column.upstream_id = DictionaryVector::DictionaryId(incoming);
	return column;
}

void JoinHashTable::EncodeDictSurvivingColumn(const DictSurvivingColumn &column, idx_t build_col_idx,
                                              const Vector &incoming, idx_t count, vector<AllocatedData> &buffers,
                                              vector<Vector> &vectors) {
	if (incoming.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
		// Slot was narrowed against one specific upstream dictionary; a different one would scatter foreign indices.
		if (DictionaryVector::DictionaryId(incoming) != column.upstream_id) {
			throw InternalException("dict-surviving join: narrowed column %llu received a chunk with an unexpected "
			                        "dictionary; build pipeline is not single-source",
			                        static_cast<uint64_t>(build_col_idx));
		}
		// Indices are < upstream size by construction, and the publisher sized the width to upstream size + 1
		ScatterDictIndices(column.index_width, DictionaryVector::SelVector(incoming), count,
		                   buffer_manager.GetBufferAllocator(), buffers, vectors);
		return;
	}
	if (!VectorOperations::HasNotNull(incoming)) {
		// e.g. the unmatched-build scan of an upstream FULL/RIGHT join: every row stores the reserved NULL index
		const auto null_index =
		    Value::Numeric(DictSurvivingInfo::IndexType(column.index_width), NumericCast<int64_t>(column.NullIndex()));
		vectors.emplace_back(null_index, count_t(count));
		return;
	}
	// There is no fallback for values that are not in the pinned dictionary. Throw, not D_ASSERT: storing the
	// native bytes into a narrowed slot would corrupt the row store.
	throw InternalException("dict-surviving join: narrowed column %llu received a non-dictionary chunk with "
	                        "values; build pipeline is not single-source",
	                        static_cast<uint64_t>(build_col_idx));
}

static bool MarkJoinNullRows(const UnifiedVectorFormat &format, idx_t count,
                             optional_ptr<bool> rows_with_null = nullptr) {
	if (format.validity.CannotHaveNull()) {
		return false;
	}
	bool has_null = false;
	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		auto format_idx = format.sel->get_index(row_idx);
		if (!format.validity.RowIsValid(format_idx)) {
			has_null = true;
			if (rows_with_null) {
				rows_with_null.get()[row_idx] = true;
			}
		}
	}
	return has_null;
}

static bool MarkJoinKeysHaveNull(DataChunk &keys, optional_ptr<bool> rows_with_null = nullptr) {
	bool has_null = false;
	for (idx_t col_idx = 0; col_idx < keys.ColumnCount(); col_idx++) {
		UnifiedVectorFormat format;
		if (keys.data[col_idx].GetType().IsNested()) {
			Vector comparison(LogicalType::BOOLEAN, keys.size());
			VectorOperations::Equals(keys.data[col_idx], keys.data[col_idx], comparison);
			comparison.ToUnifiedFormat(format);
		} else {
			keys.data[col_idx].ToUnifiedFormat(format);
		}
		if (MarkJoinNullRows(format, keys.size(), rows_with_null)) {
			has_null = true;
		}
	}
	return has_null;
}

void JoinHashTable::Build(PartitionedTupleDataAppendState &append_state, DataChunk &keys, DataChunk &payload) {
	D_ASSERT(!finalized);
	D_ASSERT(keys.size() == payload.size());
	if (keys.size() == 0) {
		return;
	}
	// special case: correlated mark join
	if (join_type == JoinType::MARK && !mark_join_info.correlated_types.empty()) {
		auto &info = mark_join_info;
		lock_guard<mutex> mj_lock(info.mj_lock);
		// Correlated MARK join
		// for the correlated mark join we need to keep track of COUNT(*) and COUNT(COLUMN) for each of the correlated
		// columns push into the aggregate hash table
		D_ASSERT(info.correlated_counts);
		for (idx_t i = 0; i < info.correlated_types.size(); i++) {
			info.group_chunk.data[i].Reference(keys.data[i]);
		}
		info.group_chunk.SetChildCardinality(keys.size());
		if (info.correlated_payload.data.empty()) {
			vector<LogicalType> types;
			types.push_back(keys.data[info.correlated_types.size()].GetType());
			info.correlated_payload.InitializeEmpty(types);
		}
		info.correlated_payload.data[0].Reference(keys.data[info.correlated_types.size()]);
		info.correlated_payload.SetChildCardinality(keys.size());
		info.correlated_counts->AddChunk(info.group_chunk, info.correlated_payload, AggregateType::NON_DISTINCT);
	}
	if (mark_join_info.uncorrelated_condition_rows) {
		// Keep all rows: probe-side NULLs can be UNKNOWN against non-NULL rows in other external hash partitions.
		mark_join_info.uncorrelated_has_null = mark_join_info.uncorrelated_has_null || MarkJoinKeysHaveNull(keys);
		mark_join_info.uncorrelated_condition_rows->Append(keys);
	}

	// build a chunk to append to the data collection [keys, payload, (optional "found" boolean), hash]
	DataChunk source_chunk;
	source_chunk.InitializeEmpty(layout_ptr->GetTypes());
	for (idx_t i = 0; i < keys.ColumnCount(); i++) {
		source_chunk.data[i].Reference(keys.data[i]);
	}
	idx_t col_offset = keys.ColumnCount();
	D_ASSERT(build_types.size() == payload.ColumnCount());
	// Scratch index vectors for narrowed columns, allocated via the buffer manager so the bytes are accounted.
	// buffers own the bytes, vectors are flat views; both must outlive the ToUnifiedFormat / AppendUnified below.
	vector<AllocatedData> dict_idx_buffers;
	vector<Vector> dict_idx_vectors;
	for (idx_t i = 0; i < payload.ColumnCount(); i++) {
		auto &incoming = payload.data[i];
		auto dict_column = GetDictSurvivingColumn(i);
		if (!dict_column) {
			source_chunk.data[col_offset + i].Reference(incoming);
			continue;
		}
		// Narrowed column: store dictionary indices (or the reserved NULL index) at the chosen width, not the value
		EncodeDictSurvivingColumn(*dict_column, i, incoming, payload.size(), dict_idx_buffers, dict_idx_vectors);
		source_chunk.data[col_offset + i].Reference(dict_idx_vectors.back());
	}
	col_offset += payload.ColumnCount();
	if (PropagatesBuildSide(join_type)) {
		// for FULL/RIGHT OUTER joins initialize the "found" boolean to false
		source_chunk.data[col_offset].Reference(vfound);
		col_offset++;
	}
	Vector hash_values(LogicalType::HASH);
	source_chunk.data[col_offset].Reference(hash_values);
	source_chunk.SetChildCardinality(keys.size());

	// ToUnifiedFormat the source chunk
	TupleDataCollection::ToUnifiedFormat(append_state.chunk_state, source_chunk);

	// prepare the keys for processing
	optional_ptr<const SelectionVector> current_sel;
	SelectionVector sel(STANDARD_VECTOR_SIZE);
	idx_t added_count = PrepareKeys(keys, append_state.chunk_state.vector_data, current_sel, sel, true);
	if (added_count < keys.size()) {
		has_null = true;
	}
	if (added_count == 0) {
		return;
	}

	// hash the keys and obtain an entry in the list
	// note that we only hash the keys used in the equality comparison
	Hash(keys, *current_sel, added_count, hash_values);
	if (bloom_filter) {
		bloom_filter->InsertHashes(hash_values, *current_sel, added_count);
	}

	// Re-reference and ToUnifiedFormat the hash column after computing it
	source_chunk.data[col_offset].Reference(hash_values);
	hash_values.ToUnifiedFormat(append_state.chunk_state.vector_data.back().unified);

	// We already called TupleDataCollection::ToUnifiedFormat, so we can AppendUnified here
	sink_collection->AppendUnified(append_state, source_chunk, *current_sel, added_count);
}

idx_t JoinHashTable::PrepareKeys(DataChunk &keys, vector<TupleDataVectorFormat> &vector_data,
                                 optional_ptr<const SelectionVector> &current_sel, SelectionVector &sel,
                                 bool build_side) {
	// figure out which keys are NULL, and create a selection vector out of them
	current_sel = FlatVector::IncrementalSelectionVector();
	idx_t added_count = keys.size();
	if (build_side && PropagatesBuildSide(join_type)) {
		// in case of a right or full outer join, we cannot remove NULL keys from the build side
		return added_count;
	}

	for (idx_t col_idx = 0; col_idx < keys.ColumnCount(); col_idx++) {
		// see internal issue 3717.
		if (join_type == JoinType::MARK && !mark_join_info.correlated_types.empty()) {
			continue;
		}
		if (null_values_are_equal[col_idx]) {
			continue;
		}
		auto &col_key_data = vector_data[col_idx].unified;
		if (col_key_data.validity.CannotHaveNull()) {
			continue;
		}
		added_count = FilterNullValues(col_key_data, *current_sel, added_count, sel);
		// null values are NOT equal for this column, filter them out
		current_sel = sel;
	}
	return added_count;
}

static void StorePointer(const const_data_ptr_t &pointer, const data_ptr_t &target) {
	Store<uint64_t>(cast_pointer_to_uint64(pointer), target);
}

static data_ptr_t LoadPointer(const const_data_ptr_t &source) {
	return cast_uint64_to_pointer(Load<uint64_t>(source));
}

//! If we consider to insert into an entry we expect to be empty, if it was filled in the meantime the insert will not
//! happen and we need to return the pointer to the to row with which the new entry would have collided. In any other
//! case we return a nullptr
template <bool PARALLEL, bool EXPECT_EMPTY>
static inline data_ptr_t InsertRowToEntry(atomic<ht_entry_t> &entry, const data_ptr_t &row_ptr_to_insert,
                                          const hash_t &salt, const idx_t &pointer_offset) {
	const ht_entry_t desired_entry(salt, row_ptr_to_insert);
	if (PARALLEL) {
		if (EXPECT_EMPTY) {
			// Add nullptr to the end of the list to mark the end
			StorePointer(nullptr, row_ptr_to_insert + pointer_offset);

			ht_entry_t expected_entry;
			entry.compare_exchange_strong(expected_entry, desired_entry, std::memory_order_release,
			                              std::memory_order_acquire);

			// The expected entry is updated with the encountered entry by the compare exchange
			// So, this returns a nullptr if it was empty, and a non-null if it was not (which cancels the insert)
			return expected_entry.GetPointerOrNull();
		} else {
			// At this point we know that the keys match, so we can try to insert until we succeed
			ht_entry_t expected_entry = entry.load(std::memory_order_acquire);
			D_ASSERT(expected_entry.IsOccupied());
			do {
				data_ptr_t current_row_pointer = expected_entry.GetPointer();
				StorePointer(current_row_pointer, row_ptr_to_insert + pointer_offset);
			} while (!entry.compare_exchange_weak(expected_entry, desired_entry, std::memory_order_release,
			                                      std::memory_order_relaxed));

			return nullptr;
		}
	} else {
		// If we are not in parallel mode, we can just do the operation without any checks
		data_ptr_t current_row_pointer = entry.load(std::memory_order_relaxed).GetPointerOrNull();
		StorePointer(current_row_pointer, row_ptr_to_insert + pointer_offset);
		entry = desired_entry;
		return nullptr;
	}
}
static inline void PerformKeyComparison(JoinHashTable::InsertState &state, JoinHashTable &ht,
                                        const TupleDataCollection &data_collection, Vector &row_locations,
                                        const idx_t count, idx_t &key_match_count, idx_t &key_no_match_count) {
	// Get the data for the rows that need to be compared
	state.lhs_data.Reset();
	state.lhs_data.SetChildCardinality(count); // the right size

	// The target selection vector says where to write the results into the lhs_data, we just want to write
	// sequentially as otherwise we trigger a bug in the Gather function
	data_collection.ResetCachedCastVectors(state.chunk_state, ht.equality_predicate_columns);
	data_collection.Gather(row_locations, state.keys_to_compare_sel, count, ht.equality_predicate_columns,
	                       state.lhs_data, *FlatVector::IncrementalSelectionVector(),
	                       state.chunk_state.cached_cast_vectors);
	TupleDataCollection::ToUnifiedFormat(state.chunk_state, state.lhs_data);

	for (idx_t i = 0; i < count; i++) {
		state.key_match_sel.set_index(i, i);
	}

	// Perform row comparisons
	key_match_count =
	    ht.row_matcher_build.Match(state.lhs_data, state.chunk_state.vector_data, state.key_match_sel, count,
	                               state.rhs_row_locations, &state.keys_no_match_sel, key_no_match_count);

	D_ASSERT(key_match_count + key_no_match_count == count);
}

template <bool PARALLEL>
static inline void InsertMatchesAndIncrementMisses(unsafe_optional_ptr<atomic<ht_entry_t>> entries,
                                                   JoinHashTable::InsertState &state, JoinHashTable &ht,
                                                   const data_ptr_t lhs_row_locations[], idx_t ht_offsets[],
                                                   const hash_t hash_salts[], const idx_t capacity_mask,
                                                   const idx_t key_match_count, const idx_t key_no_match_count) {
	if (key_match_count != 0) {
		ht.chains_longer_than_one.store(true, std::memory_order_relaxed);
	}

	// Insert the rows that match
	if (ht.insert_duplicate_keys) {
		if (key_match_count != 0) {
			ht.chains_longer_than_one.store(true, std::memory_order_relaxed);
		}
		const auto pointer_offset = ht.pointer_offset;
		for (idx_t i = 0; i < key_match_count; i++) {
			const auto need_compare_idx = state.key_match_sel.get_index(i);
			const auto entry_index = state.keys_to_compare_sel.get_index(need_compare_idx);

			const auto &ht_offset = ht_offsets[entry_index];
			auto &entry = entries.get()[ht_offset];
			const auto row_ptr_to_insert = lhs_row_locations[entry_index];

			const auto salt = hash_salts[entry_index];
			InsertRowToEntry<PARALLEL, false>(entry, row_ptr_to_insert, salt, pointer_offset);
		}
	}

	// Linear probing: each of the entries that do not match move to the next entry in the HT
	for (idx_t i = 0; i < key_no_match_count; i++) {
		const auto need_compare_idx = state.keys_no_match_sel.get_index(i);
		const auto entry_index = state.keys_to_compare_sel.get_index(need_compare_idx);

		auto &ht_offset = ht_offsets[entry_index];
		IncrementAndWrap(ht_offset, capacity_mask);

		state.remaining_sel.set_index(i, entry_index);
	}
}

template <bool PARALLEL>
static void InsertHashesLoop(unsafe_optional_ptr<atomic<ht_entry_t>> entries, Vector &row_locations, Vector &hashes_v,
                             JoinHashTable::InsertState &state, const TupleDataCollection &data_collection,
                             JoinHashTable &ht) {
	D_ASSERT(hashes_v.GetType().id() == LogicalType::HASH);
	ApplyBitmaskAndGetSaltBuild(hashes_v, state.salt_v, ht.bitmask, ht.GetBucketShift());
	const idx_t count = hashes_v.size();

	const auto &layout = data_collection.GetLayout();

	// the salts offset for each row to insert
	const auto ht_offsets = FlatVector::GetDataMutable<idx_t>(hashes_v);
	const auto hash_salts = FlatVector::GetData<hash_t>(state.salt_v);
	// the row locations of the rows that are already in the hash table
	const auto rhs_row_locations = FlatVector::GetDataMutable<data_ptr_t>(state.rhs_row_locations);
	// the row locations of the rows that are to be inserted
	const auto lhs_row_locations = FlatVector::GetData<data_ptr_t>(row_locations);

	// we start off with the entire chunk
	idx_t remaining_count = count;
	optional_ptr<const SelectionVector> remaining_sel = FlatVector::IncrementalSelectionVector();

	const auto all_valid = layout.CannotHaveNull();
	const auto column_count = layout.ColumnCount();

	if (PropagatesBuildSide(ht.join_type)) {
		// if we propagate the build side, we may have added rows with NULL keys to the HT
		// these may need to be filtered out depending on the comparison type (exactly like PrepareKeys does)
		for (idx_t col_idx = 0; col_idx < ht.conditions.size(); col_idx++) {
			// if null values are NOT equal for this column we filter them out
			if (ht.NullValuesAreEqual(col_idx)) {
				continue;
			}

			idx_t entry_idx;
			idx_t idx_in_entry;
			ValidityBytes::GetEntryIndex(col_idx, entry_idx, idx_in_entry);

			idx_t new_remaining_count = 0;
			for (idx_t i = 0; i < remaining_count; i++) {
				const auto idx = remaining_sel->get_index(i);
				const auto valid =
				    all_valid ||
				    ValidityBytes::RowIsValid(
				        ValidityBytes(lhs_row_locations[idx], column_count).GetValidityEntryUnsafe(entry_idx),
				        idx_in_entry);
				if (valid) {
					state.remaining_sel.set_index(new_remaining_count++, idx);
				}
			}
			remaining_count = new_remaining_count;
			remaining_sel = state.remaining_sel;
		}
	}

	// use the ht bitmask to make the modulo operation faster but keep the salt bits intact
	idx_t capacity_mask = ht.bitmask | ht_entry_t::SALT_MASK;
	const auto pointer_offset = ht.pointer_offset;
	while (remaining_count > 0) {
		idx_t salt_match_count = 0;

		// iterate over each entry to find out whether it belongs to an existing list or will start a new list
		for (idx_t i = 0; i < remaining_count; i++) {
			PrefetchHashTableEntry(i, remaining_count, [&](idx_t next_idx) {
				return &entries.get()[ht_offsets[remaining_sel->get_index(next_idx)]];
			});
			const idx_t row_index = remaining_sel->get_index(i);
			auto &ht_offset = ht_offsets[row_index];
			auto &salt = hash_salts[row_index];

			// increment the ht_offset of the entry as long as next entry is occupied and salt does not match
			ht_entry_t entry;
			bool occupied;
			while (true) {
				atomic<ht_entry_t> &atomic_entry = entries.get()[ht_offset];
				entry = atomic_entry.load(std::memory_order_acquire);
				occupied = entry.IsOccupied();

				// condition for incrementing the ht_offset: occupied and row_salt does not match -> move to next entry
				if (!occupied) {
					break;
				}
				if (entry.GetSalt() == salt) {
					break;
				}

				IncrementAndWrap(ht_offset, capacity_mask);
			}

			if (!occupied) { // insert into free
				auto &atomic_entry = entries.get()[ht_offset];
				const auto row_ptr_to_insert = lhs_row_locations[row_index];
				const auto potential_collided_ptr =
				    InsertRowToEntry<PARALLEL, true>(atomic_entry, row_ptr_to_insert, salt, pointer_offset);

				if (PARALLEL) {
					// if the insertion was not successful, the entry was occupied in the meantime, so we have to
					// compare the keys and insert the row to the next entry
					if (DUCKDB_UNLIKELY(potential_collided_ptr != nullptr)) {
						// if the entry was occupied, we need to compare the keys and insert the row to the next entry
						// we need to compare the keys and insert the row to the next entry
						state.keys_to_compare_sel.set_index(salt_match_count, row_index);
						rhs_row_locations[salt_match_count] = potential_collided_ptr;
						salt_match_count += 1;
					}
				}

			} else { // compare with full entry
				state.keys_to_compare_sel.set_index(salt_match_count, row_index);
				rhs_row_locations[salt_match_count] = entry.GetPointer();
				salt_match_count += 1;
			}
		}

		// at this step, for all the rows to insert we stepped either until we found an empty entry or an entry with
		// a matching salt, we now need to compare the keys for the ones that have a matching salt
		idx_t key_no_match_count = 0;
		if (salt_match_count != 0) {
			idx_t key_match_count = 0;
			PerformKeyComparison(state, ht, data_collection, row_locations, salt_match_count, key_match_count,
			                     key_no_match_count);
			InsertMatchesAndIncrementMisses<PARALLEL>(entries, state, ht, lhs_row_locations, ht_offsets, hash_salts,
			                                          capacity_mask, key_match_count, key_no_match_count);
		}

		// update the overall selection vector to only point the entries that still need to be inserted
		// as there was no match found for them yet
		remaining_sel = state.remaining_sel;
		remaining_count = key_no_match_count;
	}
}

void JoinHashTable::InsertHashes(Vector &hashes_v, TupleDataChunkState &chunk_state, InsertState &insert_state,
                                 bool parallel) {
	// a sealed filter was accumulated during Sink and is already complete; only a finalize-built one fills here
	if (bloom_filter && !bloom_filter->IsSealed()) {
		bloom_filter->InsertHashes(hashes_v);
	}
	auto atomic_entries = GetAtomicEntries();
	auto &row_locations = chunk_state.row_locations;
	if (parallel) {
		InsertHashesLoop<true>(atomic_entries, row_locations, hashes_v, insert_state, *data_collection, *this);
	} else {
		InsertHashesLoop<false>(atomic_entries, row_locations, hashes_v, insert_state, *data_collection, *this);
	}
}

unique_ptr<PrefixRangeFilter::BuildState> JoinHashTable::InitializePrefixRangeBuildState() {
	D_ASSERT(prefix_range_filter);
	return prefix_range_filter->InitializeBuildState(context);
}

idx_t JoinHashTable::GetPrefixRangeBuildStateSize() const {
	D_ASSERT(prefix_range_filter);
	return prefix_range_filter->GetBuildStateSize();
}

void JoinHashTable::BuildPrefixRangeFilter() {
	if (!ShouldBuildPrefixRangeFilter()) {
		return;
	}

	auto prefix_range_state = InitializePrefixRangeBuildState();
	TupleDataChunkIterator iterator(*data_collection, TupleDataPinProperties::KEEP_EVERYTHING_PINNED, 0,
	                                data_collection->ChunkCount(), false);
	do {
		const auto count = iterator.GetCurrentChunkCount();
		if (count == 0) {
			continue;
		}
		InsertPrefixRangeChunk(iterator.GetChunkState(), count, *prefix_range_state);
	} while (iterator.Next());
	MergePrefixRangeBuildState(*prefix_range_state);
}

void JoinHashTable::InsertPrefixRangeChunk(TupleDataChunkState &chunk_state, idx_t count,
                                           PrefixRangeFilter::BuildState &state, bool parallel) {
	D_ASSERT(prefix_range_filter);
	Vector build_keys(layout_ptr->GetTypes()[0], count);
	auto &sel = *FlatVector::IncrementalSelectionVector();
	data_collection->Gather(chunk_state.row_locations, sel, count, 0, build_keys, sel, nullptr);
	FlatVector::SetSize(build_keys, count_t(count));
	if (parallel) {
		prefix_range_filter->InsertKeysParallel(build_keys, state);
	} else {
		prefix_range_filter->InsertKeys(build_keys, state);
	}
}

void JoinHashTable::MergePrefixRangeBuildState(PrefixRangeFilter::BuildState &state) {
	D_ASSERT(prefix_range_filter);
	prefix_range_filter->MergeBuildState(state);
}

void JoinHashTable::AllocatePointerTable(bool use_radix_layout) {
	capacity = PointerTableCapacity(Count());
	D_ASSERT(IsPowerOfTwo(capacity));

	constexpr uint64_t MAX_HASHTABLE_CAPACITY = (1ULL << 48) - 1;
	if (capacity >= MAX_HASHTABLE_CAPACITY) {
		throw InternalException("Hashtable capacity exceeds 48-bit limit (2^48 - 1)");
	}

	if (should_build_bloom_filter && (!bloom_filter || bloom_filter->IsSealed())) {
		// (re)create the finalize-built filter now that the actual build count is known
		bloom_filter = recycled_bloom_filter ? std::move(recycled_bloom_filter) : make_shared_ptr<BloomFilter>();
		bloom_filter->Initialize(context, MaxValue<idx_t>(Count(), 1));
	}

	if (hash_map.get()) {
		// There is already a hash map
		auto current_capacity = hash_map.GetSize() / sizeof(ht_entry_t);
		if (capacity > current_capacity) {
			// Need more space
			hash_map = buffer_manager.GetBufferAllocator().AllocateHuge(capacity * sizeof(ht_entry_t));
		} else {
			// Just use the current hash map
			capacity = current_capacity;
		}
	} else {
		// Allocate a hash map
		hash_map = buffer_manager.GetBufferAllocator().AllocateHuge(capacity * sizeof(ht_entry_t));
	}
	D_ASSERT(hash_map.GetSize() == capacity * sizeof(ht_entry_t));

	bitmask = capacity - 1;
	const auto capacity_bits = RadixPartitioning::RadixBitsOfPowerOfTwo(capacity);
	bucket_shift =
	    use_radix_layout && radix_bits > 0 && capacity_bits >= radix_bits ? RadixPartitioning::Shift(capacity_bits) : 0;

	DUCKDB_LOG(context, PhysicalOperatorLogType, op, "JoinHashTable", "Build",
	           {{"rows", to_string(data_collection->Count())},
	            {"size", to_string(data_collection->SizeInBytes() + hash_map.GetSize())}});
}

void JoinHashTable::InitializePointerTable(idx_t entry_idx_from, idx_t entry_idx_to) {
	// initialize HT with all-zero entries
	auto entries = GetEntries();
	std::fill_n(entries.get() + entry_idx_from, entry_idx_to - entry_idx_from, ht_entry_t());
}

void JoinHashTable::Finalize(idx_t chunk_idx_from, idx_t chunk_idx_to, bool parallel,
                             optional_ptr<PrefixRangeFilter::BuildState> prefix_range_state,
                             bool prefix_range_parallel) {
	// Pointer table should be allocated
	D_ASSERT(hash_map.get());

	Vector hashes(LogicalType::HASH);

	TupleDataChunkIterator iterator(*data_collection, TupleDataPinProperties::KEEP_EVERYTHING_PINNED, chunk_idx_from,
	                                chunk_idx_to, false);
	const auto row_locations = iterator.GetRowLocations();

	InsertState insert_state(*this);
	do {
		const auto count = iterator.GetCurrentChunkCount();
		auto hash_data = FlatVector::Writer<hash_t>(hashes, count_t(count));
		for (idx_t i = 0; i < count; i++) {
			hash_data.WriteValue(Load<hash_t>(row_locations[i] + pointer_offset));
		}
		TupleDataChunkState &chunk_state = iterator.GetChunkState();

		InsertHashes(hashes, chunk_state, insert_state, parallel);
		if (prefix_range_state) {
			InsertPrefixRangeChunk(chunk_state, count, *prefix_range_state, prefix_range_parallel);
		}
	} while (iterator.Next());
}

void JoinHashTable::InitializeScanStructure(ScanStructure &scan_structure, DataChunk &keys,
                                            TupleDataChunkState &key_state,
                                            optional_ptr<const SelectionVector> &current_sel) {
	D_ASSERT(Count() > 0); // should be handled before
	D_ASSERT(finalized);

	// set up the scan structure
	scan_structure.is_null = false;
	scan_structure.finished = false;
	if (join_type != JoinType::INNER) {
		memset(scan_structure.found_match.get(), 0, sizeof(bool) * STANDARD_VECTOR_SIZE);
	}

	// first prepare the keys for probing
	TupleDataCollection::ToUnifiedFormat(key_state, keys);
	scan_structure.count = PrepareKeys(keys, key_state.vector_data, current_sel, scan_structure.sel_vector, false);

	if (scan_structure.count < keys.size()) {
		scan_structure.has_null_value_filter = true;
	} else {
		scan_structure.has_null_value_filter = false;
	}
}

//! Per-chunk fast-reject so non-dictionary chunks skip the TryProbeDictionary call entirely
static bool LHSChunkIsDictionaryEligible(const Vector &lhs_key) {
	if (lhs_key.GetVectorType() != VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	return DictionaryVector::DictionarySize(lhs_key).IsValid();
}

//! Per-chunk fast-reject so non-constant chunks skip the TryProbeConstant call entirely
static bool LHSChunkIsConstant(const Vector &lhs_key) {
	return lhs_key.GetVectorType() == VectorType::CONSTANT_VECTOR;
}

void JoinHashTable::Probe(ScanStructure &scan_structure, DataChunk &keys, TupleDataChunkState &key_state,
                          ProbeState &probe_state, optional_ptr<Vector> precomputed_hashes) {
	// dispatch into the compressed-vector fast paths when the operator gated them on. precomputed_hashes
	// is incompatible: the caller hashed the full N-row chunk; the fast paths re-hash a sliced D-row chunk.
	if (probe_state.dict_state && !precomputed_hashes) {
		auto &lhs_key = keys.data[0];
		if (LHSChunkIsConstant(lhs_key) && TryProbeConstant(scan_structure, keys, key_state, probe_state)) {
			return;
		}
		if (LHSChunkIsDictionaryEligible(lhs_key) && TryProbeDictionary(scan_structure, keys, key_state, probe_state)) {
			return;
		}
	}

	optional_ptr<const SelectionVector> current_sel;
	InitializeScanStructure(scan_structure, keys, key_state, current_sel);
	if (scan_structure.count == 0) {
		return;
	}
	if (precomputed_hashes) {
		GetRowPointers(keys, key_state, probe_state, *precomputed_hashes, current_sel, scan_structure.count,
		               scan_structure.pointers, scan_structure.sel_vector, scan_structure.has_null_value_filter);
	} else {
		Vector hashes(LogicalType::HASH);
		// hash all the keys
		Hash(keys, *current_sel, scan_structure.count, hashes);

		// now initialize the pointers of the scan structure based on the hashes
		GetRowPointers(keys, key_state, probe_state, hashes, current_sel, scan_structure.count, scan_structure.pointers,
		               scan_structure.sel_vector, scan_structure.has_null_value_filter);
	}
}

bool JoinHashTable::TryProbeDictionary(ScanStructure &scan_structure, DataChunk &keys, TupleDataChunkState &key_state,
                                       ProbeState &probe_state) {
	static constexpr idx_t MAX_DICTIONARY_SIZE_THRESHOLD = 20000;
	static constexpr idx_t DICTIONARY_THRESHOLD = 2;

	D_ASSERT(equality_types.size() == 1);
	D_ASSERT(Count() > 0);
	D_ASSERT(finalized);

	auto &dict_col = keys.data[0];
	if (dict_col.GetVectorType() != VectorType::DICTIONARY_VECTOR) {
		return false;
	}
	const auto opt_dict_size = DictionaryVector::DictionarySize(dict_col);
	if (!opt_dict_size.IsValid()) {
		// dict size not known - this is not a dictionary that comes from the storage
		return false;
	}
	const idx_t dict_size = opt_dict_size.GetIndex();
	const auto &dictionary_id = DictionaryVector::DictionaryId(dict_col);
	if (dictionary_id.empty()) {
		// no id - can't cache across vectors, only use dict path if it is much smaller than the chunk
		if (dict_size * DICTIONARY_THRESHOLD >= keys.size()) {
			return false;
		}
	} else if (dict_size >= MAX_DICTIONARY_SIZE_THRESHOLD) {
		return false;
	}
	if (dict_size == 0) {
		// the cache below is never allocated for an empty dictionary - avoid dereferencing a null Vector
		return false;
	}

	auto &dictionary_vector = DictionaryVector::Child(dict_col);
	auto &offsets = DictionaryVector::SelVector(dict_col);
	D_ASSERT(probe_state.dict_state);
	auto &dict_state = *probe_state.dict_state;

	if (dict_state.dictionary_id.empty() || dict_state.dictionary_id != dictionary_id) {
		// new dictionary - initialize the per-id cache
		if (dict_size > dict_state.capacity) {
			dict_state.dictionary_pointers = make_uniq<Vector>(LogicalType::POINTER, dict_size);
			dict_state.found_entry = make_unsafe_uniq_array<bool>(dict_size);
			dict_state.capacity = dict_size;
		}
		memset(dict_state.found_entry.get(), 0, dict_size * sizeof(bool));
		// zero the cache - an unresolved slot reads back as nullptr, the miss sentinel
		memset(FlatVector::GetDataMutable(*dict_state.dictionary_pointers), 0, dict_size * sizeof(data_ptr_t));
		dict_state.dictionary_id = dictionary_id;
		dict_state.resolved_count = 0;
		// storage dictionaries reserve a NULL sentinel slot that non-NULL probe rows never
		// reference; pre-mark NULL child slots resolved so resolved_count can still reach dict_size.
		// Equality joins only: PrepareKeys drops NULL probe rows, so the NULL slot is never probed.
		// Under NOT DISTINCT FROM those rows are kept and the walk below must probe it itself.
		if (!null_values_are_equal[0] && dictionary_vector.GetVectorType() == VectorType::FLAT_VECTOR) {
			const auto &child_validity = FlatVector::Validity(dictionary_vector);
			if (child_validity.CanHaveNull()) {
				for (idx_t i = 0; i < dict_size; i++) {
					if (!child_validity.RowIsValid(i)) {
						dict_state.found_entry[i] = true;
						dict_state.resolved_count++;
					}
				}
			}
		}
	} else if (dict_size > dict_state.capacity) {
		throw InternalException("JoinHashTable - using cached dictionary data but dictionary has changed (dictionary "
		                        "id %s - dict size %d, current capacity %d)",
		                        dict_state.dictionary_id, dict_size, dict_state.capacity);
	}

	// collect each dict slot once - skip the walk entirely once every slot is resolved
	auto &found_entry = dict_state.found_entry;
	auto &unique_entries = dict_state.unique_entries;
	idx_t unique_count = 0;
	if (dict_state.resolved_count < dict_size) {
		for (idx_t i = 0; i < keys.size(); i++) {
			const auto dict_idx = offsets.get_index(i);
			unique_entries.set_index(unique_count, dict_idx);
			unique_count += !found_entry[dict_idx];
			found_entry[dict_idx] = true;
		}
		dict_state.resolved_count += unique_count;
	}

	if (unique_count > 0) {
		auto &unique_values = dict_state.unique_values;
		if (unique_values.ColumnCount() == 0) {
			unique_values.InitializeEmpty(vector<LogicalType> {equality_types[0]});
			TupleDataCollection::InitializeChunkState(dict_state.unique_key_state, {equality_types[0]});
		}
		unique_values.data[0].Slice(dictionary_vector, unique_entries, unique_count);
		unique_values.CheckCardinality(unique_count);

		TupleDataCollection::ToUnifiedFormat(dict_state.unique_key_state, unique_values);

		auto &hashes = dict_state.hashes;
		VectorOperations::Hash(unique_values.data[0], hashes, unique_count);

		idx_t count = unique_count;
		GetRowPointers(unique_values, dict_state.unique_key_state, probe_state, hashes, nullptr, count,
		               dict_state.new_dictionary_pointers, dict_state.match_sel, false);

		// remap hits from position to dict slot; missed slots stay nullptr from the rebind memset
		const auto new_dict_ptrs = FlatVector::GetData<data_ptr_t>(dict_state.new_dictionary_pointers);
		auto unique_dict_ptrs = FlatVector::GetDataMutable<data_ptr_t>(*dict_state.dictionary_pointers);
		for (idx_t i = 0; i < count; i++) {
			const auto position = dict_state.match_sel.get_index(i);
			const auto dict_idx = unique_entries.get_index(position);
			unique_dict_ptrs[dict_idx] = new_dict_ptrs[position];
		}
	}

	// fan out the cache into the scan_structure shape that Probe() produces
	scan_structure.is_null = false;
	scan_structure.finished = false;
	if (join_type != JoinType::INNER) {
		memset(scan_structure.found_match.get(), 0, sizeof(bool) * STANDARD_VECTOR_SIZE);
	}
	TupleDataCollection::ToUnifiedFormat(key_state, keys);
	optional_ptr<const SelectionVector> current_sel;
	const idx_t prepared_count =
	    PrepareKeys(keys, key_state.vector_data, current_sel, scan_structure.sel_vector, false);
	scan_structure.has_null_value_filter = prepared_count < keys.size();

	auto scan_structure_ptrs = FlatVector::GetDataMutable<data_ptr_t>(scan_structure.pointers);
	const auto unique_dict_ptrs = FlatVector::GetData<data_ptr_t>(*dict_state.dictionary_pointers);
	idx_t kept = 0;
	for (idx_t i = 0; i < prepared_count; i++) {
		const auto row_index = current_sel->get_index(i);
		const auto dict_idx = offsets.get_index(row_index);
		const auto ptr = unique_dict_ptrs[dict_idx];
		scan_structure_ptrs[row_index] = ptr;
		scan_structure.sel_vector.set_index(kept, row_index);
		// miss rows leave a stale pointer, but Next() only reads pointers at positions in sel_vector[0..count)
		kept += (ptr != nullptr);
	}
	scan_structure.count = kept;
	return true;
}

bool JoinHashTable::TryProbeConstant(ScanStructure &scan_structure, DataChunk &keys, TupleDataChunkState &key_state,
                                     ProbeState &probe_state) {
	D_ASSERT(equality_types.size() == 1);
	D_ASSERT(Count() > 0);
	D_ASSERT(finalized);

	auto &constant_col = keys.data[0];
	if (constant_col.GetVectorType() != VectorType::CONSTANT_VECTOR) {
		return false;
	}
#ifndef DEBUG
	if (keys.size() <= 1) {
		// resolving once only pays off when the result fans out to multiple probe rows
		return false;
	}
#endif

	// constant vectors carry no dictionary id, so there is no cross-chunk cache - resolve the
	// single distinct key with one hash-table lookup per chunk
	D_ASSERT(probe_state.dict_state);
	auto &dict_state = *probe_state.dict_state;
	auto &unique_values = dict_state.unique_values;
	if (unique_values.ColumnCount() == 0) {
		unique_values.InitializeEmpty(vector<LogicalType> {equality_types[0]});
		TupleDataCollection::InitializeChunkState(dict_state.unique_key_state, {equality_types[0]});
	}
	SelectionVector sel(1);
	sel.set_index(0, 0);
	unique_values.data[0].Slice(constant_col, sel, 1);
	unique_values.Flatten();

	TupleDataCollection::ToUnifiedFormat(dict_state.unique_key_state, unique_values);

	auto &hashes = dict_state.hashes;
	VectorOperations::Hash(unique_values.data[0], hashes, 1);

	// resolved_ptr is a chain-head, not a confirmed match - a real key-miss is still rejected by
	// the chain walk in Next(). A NULL constant key resolves correctly via RowMatcher null equality.
	idx_t count = 1;
	GetRowPointers(unique_values, dict_state.unique_key_state, probe_state, hashes, nullptr, count,
	               dict_state.new_dictionary_pointers, dict_state.match_sel, false);
	const auto resolved_ptr =
	    count == 0 ? nullptr : FlatVector::GetData<data_ptr_t>(dict_state.new_dictionary_pointers)[0];

	// fan out the resolved pointer into the scan_structure shape that Probe() produces
	scan_structure.is_null = false;
	scan_structure.finished = false;
	if (join_type != JoinType::INNER) {
		memset(scan_structure.found_match.get(), 0, sizeof(bool) * STANDARD_VECTOR_SIZE);
	}
	TupleDataCollection::ToUnifiedFormat(key_state, keys);
	optional_ptr<const SelectionVector> current_sel;
	const idx_t prepared_count =
	    PrepareKeys(keys, key_state.vector_data, current_sel, scan_structure.sel_vector, false);
	scan_structure.has_null_value_filter = prepared_count < keys.size();

	auto scan_structure_ptrs = FlatVector::GetDataMutable<data_ptr_t>(scan_structure.pointers);
	idx_t kept = 0;
	if (resolved_ptr) {
		// PrepareKeys dropped NULL probe rows - the rest all share the one resolved chain-head
		for (idx_t i = 0; i < prepared_count; i++) {
			const auto row_index = current_sel->get_index(i);
			scan_structure_ptrs[row_index] = resolved_ptr;
			scan_structure.sel_vector.set_index(kept++, row_index);
		}
	}
	scan_structure.count = kept;
	return true;
}

ScanStructure::ScanStructure(JoinHashTable &ht_p, TupleDataChunkState &key_state_p)
    : key_state(key_state_p), pointers(LogicalType::POINTER), count(0), sel_vector(STANDARD_VECTOR_SIZE),
      chain_match_sel_vector(STANDARD_VECTOR_SIZE), chain_no_match_sel_vector(STANDARD_VECTOR_SIZE),
      found_match(make_unsafe_uniq_array_uninitialized<bool>(STANDARD_VECTOR_SIZE)), ht(ht_p), finished(false),
      is_null(true), rhs_pointers(LogicalType::POINTER), lhs_sel_vector(STANDARD_VECTOR_SIZE), last_match_count(0),
      last_sel_vector(STANDARD_VECTOR_SIZE) {
	if (ht.residual_predicate) {
		residual_executor = make_uniq<ExpressionExecutor>(ht.context);
		residual_executor->AddExpression(*ht.residual_predicate);

		// initialize residual state
		residual_state = make_uniq<ResidualPredicateProbeState>();

		// determine column types needed
		idx_t total_columns = 0;
		for (const auto &entry : ht.residual_info->probe_input_to_probe_map) {
			total_columns = MaxValue(total_columns, entry.first + 1);
		}
		for (const auto &entry : ht.residual_info->build_input_to_layout_map) {
			total_columns = MaxValue(total_columns, entry.first + 1);
		}

		vector<LogicalType> eval_types(total_columns, LogicalType::INVALID);
		vector<bool> initialize_columns(total_columns, false);

		// fill in probe types
		for (const auto &entry : ht.residual_info->probe_input_to_probe_map) {
			idx_t orig_idx = entry.first;
			idx_t probe_data_col = entry.second;
			eval_types[orig_idx] = ht.residual_info->probe_types[probe_data_col];
			initialize_columns[orig_idx] = true;
		}

		// fill in build types
		for (const auto &entry : ht.residual_info->build_input_to_layout_map) {
			idx_t col_with_offset = entry.first;
			idx_t layout_col = entry.second;
			eval_types[col_with_offset] = ht.layout_ptr->GetTypes()[layout_col];
			initialize_columns[col_with_offset] = true;
		}

		// initialize chunks ONCE
		residual_state->Initialize(Allocator::Get(ht.context), eval_types, initialize_columns);
	}
}

void ScanStructure::Reset() {
	count = 0;
	finished = false;
	is_null = true;
	has_null_value_filter = false;
	last_match_count = 0;
}

void ScanStructure::Next(DataChunk &keys, DataChunk &probe_data, DataChunk &result) {
	D_ASSERT(keys.size() == probe_data.size());

	if (finished) {
		return;
	}

	switch (ht.join_type) {
	case JoinType::INNER:
	case JoinType::RIGHT:
		NextInnerJoin(keys, probe_data, result);
		break;
	case JoinType::SEMI:
		NextSemiJoin(keys, probe_data, result);
		break;
	case JoinType::MARK:
		NextMarkJoin(keys, probe_data, result);
		break;
	case JoinType::ANTI:
		NextAntiJoin(keys, probe_data, result);
		break;
	case JoinType::RIGHT_ANTI:
	case JoinType::RIGHT_SEMI:
		NextRightSemiOrAntiJoin(keys, probe_data);
		break;
	case JoinType::OUTER:
		NextLeftJoin(keys, probe_data, result);
		break;
	case JoinType::LEFT:
		if (!ht.chains_longer_than_one.load(std::memory_order_relaxed)) {
			NextUniqueLeftJoin(keys, probe_data, result);
		} else {
			NextLeftJoin(keys, probe_data, result);
		}
		break;
	case JoinType::SINGLE:
		NextSingleJoin(keys, probe_data, result);
		break;
	default:
		throw InternalException("Unhandled join type in JoinHashTable");
	}

	if (PointersExhausted()) {
		FlushProbeMatches();
	}
}

bool ScanStructure::PointersExhausted() const {
	// AdvancePointers creates a "new_count" for every pointer advanced during the
	// previous advance pointers call. If no pointers are advanced, new_count = 0.
	// count is then set to new_count.
	return count == 0;
}

idx_t ScanStructure::ResolvePredicates(DataChunk &keys, DataChunk &probe_data, SelectionVector &match_sel,
                                       optional_ptr<SelectionVector> no_match_sel) {
	// Initialize the found_match array to the current sel_vector
	for (idx_t i = 0; i < this->count; ++i) {
		match_sel.set_index(i, this->sel_vector.get_index(i));
	}

	// If there is a matcher for the probing side because of non-equality predicates, use it
	idx_t result_count;
	idx_t no_match_count = 0;
	if (ht.needs_chain_matcher) {
		auto &matcher = no_match_sel ? ht.row_matcher_probe_no_match_sel : ht.row_matcher_probe;
		D_ASSERT(matcher);

		// we need to only use the vectors with the indices of the columns that are used in the probe phase, namely
		// the non-equality columns
		result_count = matcher->Match(keys, key_state.vector_data, match_sel, this->count, pointers, no_match_sel.get(),
		                              no_match_count);
	} else {
		// no match sel is the opposite of match sel
		result_count = this->count;
	}

	if (ht.residual_predicate && ht.residual_info && result_count > 0) {
		// Pass no_match_count as offset so residual non-matches are appended after non-equality non-matches
		// instead of overwriting them (which would cause AdvancePointers to read stale data)
		result_count = ApplyResidualPredicate(probe_data, match_sel, result_count, no_match_sel, no_match_count);
	}

	// accumulate matches locally, will be flushed globally if PointersExhausted is true and we
	// finished walking the chains
	local_probe_matches += result_count;
	return result_count;
}

void ScanStructure::FlushProbeMatches() {
	if (local_probe_matches == 0) {
		return;
	}
	ht.total_probe_matches.fetch_add(local_probe_matches, std::memory_order_relaxed);
	local_probe_matches = 0;
}

idx_t ScanStructure::ApplyResidualPredicate(DataChunk &probe_data, SelectionVector &match_sel, idx_t match_count,
                                            optional_ptr<SelectionVector> no_match_sel, idx_t no_match_offset) {
	D_ASSERT(residual_state);
	D_ASSERT(residual_executor);

	// reset chunks for reuse (no reallocation!)
	residual_state->eval_chunk.Reset();
	residual_state->eval_chunk.SetChildCardinality(match_count);

	// copy probe columns at their ORIGINAL positions
	for (const auto &entry : ht.residual_info->probe_input_to_probe_map) {
		idx_t orig_idx = entry.first;
		idx_t probe_data_col = entry.second;
		residual_state->eval_chunk.data[orig_idx].Slice(probe_data.data[probe_data_col], match_sel, match_count);
	}

	// gather RHS columns from hash table
	// TODO: these columns are gathered again when building the final result
	for (const auto &entry : ht.residual_info->build_input_to_layout_map) {
		idx_t col_with_offset = entry.first;
		idx_t layout_col = entry.second;
		auto &target_vector = residual_state->eval_chunk.data[col_with_offset];
		GatherResult(target_vector, match_sel, match_count, layout_col);
		FlatVector::SetSize(target_vector, count_t(match_count));
	}

	SelectionVector &selected_sel = residual_state->selected_sel;
	SelectionVector &remaining_sel = residual_state->remaining_sel;

	idx_t new_match_count = residual_executor->SelectExpression(residual_state->eval_chunk, selected_sel, remaining_sel,
	                                                            nullptr, match_count);

	if (no_match_sel) {
		idx_t residual_no_match_count = match_count - new_match_count;
		for (idx_t i = 0; i < residual_no_match_count; i++) {
			idx_t dense_idx = remaining_sel.get_index(i);
			no_match_sel->set_index(no_match_offset + i, match_sel.get_index(dense_idx));
		}
	}

	for (idx_t i = 0; i < new_match_count; i++) {
		idx_t dense_idx = selected_sel.get_index(i);
		match_sel.set_index(i, match_sel.get_index(dense_idx));
	}

	return new_match_count;
}

idx_t ScanStructure::ScanInnerJoin(DataChunk &keys, DataChunk &probe_data, SelectionVector &result_vector) {
	while (true) {
		// resolve the equality_predicates for this set of keys
		idx_t result_count = ResolvePredicates(keys, probe_data, result_vector, nullptr);

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

template <bool USE_DICT_EMISSION>
static void AdvancePointersLoop(JoinHashTable &ht, Vector &pointers, SelectionVector &out_sel,
                                const SelectionVector &sel, const idx_t sel_count, idx_t &out_count) {
	idx_t new_count = 0;
	auto ptrs = FlatVector::GetDataMutable<data_ptr_t>(pointers);
	for (idx_t i = 0; i < sel_count; i++) {
		auto idx = sel.get_index(i);
		ptrs[idx] = ht.GetNextPointer<USE_DICT_EMISSION>(ptrs[idx]);
		if (ptrs[idx]) {
			out_sel.set_index(new_count++, idx);
		}
	}
	out_count = new_count;
}

void ScanStructure::AdvancePointers(const SelectionVector &sel, const idx_t sel_count) {
	if (!ht.chains_longer_than_one.load(std::memory_order_relaxed)) {
		this->count = 0;
		return;
	}
	if (ht.use_dict_emission) {
		AdvancePointersLoop<true>(ht, pointers, sel_vector, sel, sel_count, count);
	} else {
		AdvancePointersLoop<false>(ht, pointers, sel_vector, sel, sel_count, count);
	}
}

void ScanStructure::AdvancePointers() {
	AdvancePointers(this->sel_vector, this->count);
}

void ScanStructure::GatherResult(Vector &result, const SelectionVector &result_vector,
                                 const SelectionVector &sel_vector, const idx_t count, const idx_t col_no) {
	ht.data_collection->Gather(pointers, sel_vector, count, col_no, result, result_vector, nullptr);
}

void ScanStructure::GatherResult(Vector &result, const SelectionVector &sel_vector, const idx_t count,
                                 const idx_t col_idx) {
	GatherResult(result, *FlatVector::IncrementalSelectionVector(), sel_vector, count, col_idx);
}

void ScanStructure::GatherResult(Vector &result, const idx_t count, const idx_t col_idx) {
	ht.data_collection->Gather(rhs_pointers, *FlatVector::IncrementalSelectionVector(), count, col_idx, result,
	                           *FlatVector::IncrementalSelectionVector(), nullptr);
}

void JoinHashTable::GatherRHS(Vector &row_ptrs, const SelectionVector &ptr_sel, const idx_t count, DataChunk &result,
                              idx_t rhs_col_offset) const {
	if (use_dict_emission) {
		const auto ptrs = FlatVector::GetData<data_ptr_t>(row_ptrs);
		EmitDictVectors(ptrs, ptr_sel, count, result, rhs_col_offset);
		return;
	}
	const auto &result_sel = *FlatVector::IncrementalSelectionVector();
	const auto ptrs = FlatVector::GetData<data_ptr_t>(row_ptrs);
	const auto &offsets = layout_ptr->GetOffsets();
	const auto cond_count = condition_types.size();

	for (idx_t col_idx = 0; col_idx < output_columns.size(); col_idx++) {
		auto &vector = result.data[rhs_col_offset + col_idx];
		const auto output_col_idx = output_columns[col_idx];

		// Pinned column: re-emit the upstream dictionary. Layout is [conditions, build, (found), hash], so payload
		// columns sit at layout index >= cond_count; guard the subtraction so it cannot underflow into a spurious idx.
		if (output_col_idx >= cond_count) {
			auto dict_column = GetDictSurvivingColumn(output_col_idx - cond_count);
			if (dict_column) {
				SelectionVector build_sel_vec(count);
				const auto col_offset = offsets[output_col_idx];
				// Read the dict index at the width chosen at layout-publication time
				LoadDictIndices(dict_column->index_width, ptrs, ptr_sel, count, col_offset, build_sel_vec);
				vector.Dictionary(dict_column->entry, build_sel_vec, count);
				continue;
			}
		}

		D_ASSERT(vector.GetType() == layout_ptr->GetTypes()[output_col_idx]);
		// The native gather writes straight into a flat buffer. A reused output chunk (e.g. across recursive-CTE
		// iterations) may still hold a DICTIONARY_VECTOR left by a prior dict-emitting iteration; reset it to flat.
		if (vector.GetVectorType() != VectorType::FLAT_VECTOR) {
			vector.Initialize();
		}
		data_collection->Gather(row_ptrs, ptr_sel, count, output_col_idx, vector, result_sel, nullptr);
		FlatVector::SetSize(vector, count_t(count));
	}
}

static bool SelectionIsFullIdentity(const SelectionVector &sel, idx_t count, idx_t source_count) {
	if (count != source_count) {
		return false;
	}
	for (idx_t i = 0; i < count; i++) {
		if (sel.get_index_unsafe(i) != i) {
			return false;
		}
	}
	return true;
}

void ScanStructure::UpdateCompactionBuffer(idx_t base_count, SelectionVector &result_vector, idx_t result_count) {
	// matches were found
	// record the result
	// on the LHS, we store result vector
	for (idx_t i = 0; i < result_count; i++) {
		lhs_sel_vector.set_index(base_count + i, result_vector.get_index(i));
	}

	// on the RHS, we collect their pointers
	VectorOperations::Copy(pointers, rhs_pointers, result_vector, result_count, 0, base_count);
}

void ScanStructure::NextInnerJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result) {
	if (ht.join_type != JoinType::RIGHT_SEMI && ht.join_type != JoinType::RIGHT_ANTI) {
		D_ASSERT(result.ColumnCount() == ht.lhs_output_in_probe.size() + ht.output_columns.size());
	}

	idx_t base_count = 0;
	idx_t result_count;
	while (this->count > 0) {
		// if we have saved the match result, we need not call ScanInnerJoin again
		if (last_match_count == 0) {
			result_count = ScanInnerJoin(keys, probe_data, chain_match_sel_vector);
		} else {
			chain_match_sel_vector.Initialize(last_sel_vector);
			result_count = last_match_count;
			last_match_count = 0;
		}

		if (result_count > 0) {
			// the result chunk cannot contain more data, we record the match result for future use
			if (base_count + result_count > STANDARD_VECTOR_SIZE) {
				last_sel_vector.Initialize(chain_match_sel_vector);
				last_match_count = result_count;
				break;
			}

			if (PropagatesBuildSide(ht.join_type)) {
				// full/right outer join: mark join matches as FOUND in the HT
				auto ptrs = FlatVector::GetData<data_ptr_t>(pointers);
				for (idx_t i = 0; i < result_count; i++) {
					auto idx = chain_match_sel_vector.get_index(i);
					// NOTE: threadsan reports this as a data race because this can be set concurrently by separate
					// threads Technically it is, but it does not matter, since the only value that can be written is
					// "true"
					Store<bool>(true, ptrs[idx] + ht.tuple_size);
				}
			}

			if (ht.join_type != JoinType::RIGHT_SEMI && ht.join_type != JoinType::RIGHT_ANTI) {
				// fast path: no chains longer than one
				if (!ht.chains_longer_than_one.load(std::memory_order_relaxed)) {
					// extract only OUTPUT columns from probe_data
					const auto full_identity =
					    SelectionIsFullIdentity(chain_match_sel_vector, result_count, probe_data.size());
					for (idx_t i = 0; i < ht.lhs_output_in_probe.size(); i++) {
						idx_t probe_col_idx = ht.lhs_output_in_probe[i];
						if (full_identity) {
							result.data[i].Reference(probe_data.data[probe_col_idx]);
						} else {
							result.data[i].Slice(probe_data.data[probe_col_idx], chain_match_sel_vector, result_count);
						}
					}
					// on the RHS, we need to fetch the data from the hash table
					ht.GatherRHS(pointers, chain_match_sel_vector, result_count, result, ht.lhs_output_in_probe.size());

					AdvancePointers();
					return;
				}

				// Common Path: use a buffer to store temporary data
				UpdateCompactionBuffer(base_count, chain_match_sel_vector, result_count);
				base_count += result_count;
			}
		}
		AdvancePointers();
	}

	if (base_count > 0) {
		// extract only OUTPUT columns from probe_data using compaction buffer
		const auto full_identity = SelectionIsFullIdentity(lhs_sel_vector, base_count, probe_data.size());
		for (idx_t i = 0; i < ht.lhs_output_in_probe.size(); i++) {
			idx_t probe_col_idx = ht.lhs_output_in_probe[i];
			if (full_identity) {
				result.data[i].Reference(probe_data.data[probe_col_idx]);
			} else {
				result.data[i].Slice(probe_data.data[probe_col_idx], lhs_sel_vector, base_count);
			}
		}
		// 2) gather RHS vectors
		ht.GatherRHS(rhs_pointers, *FlatVector::IncrementalSelectionVector(), base_count, result,
		             ht.lhs_output_in_probe.size());
	}
}

void ScanStructure::ScanKeyMatches(DataChunk &keys, DataChunk &probe_data) {
	// the semi-join, anti-join and mark-join we handle a differently from the inner join
	// since there can be at most STANDARD_VECTOR_SIZE results
	// we handle the entire chunk in one call to Next().
	// for every pointer, we keep chasing pointers and doing comparisons.
	// this results in a boolean array indicating whether or not the tuple has a match
	// Start with the scan selection

	while (this->count > 0) {
		// resolve the equality_predicates for the current set of pointers
		idx_t match_count = ResolvePredicates(keys, probe_data, chain_match_sel_vector, &chain_no_match_sel_vector);
		idx_t no_match_count = this->count - match_count;

		// mark each of the matches as found
		for (idx_t i = 0; i < match_count; i++) {
			found_match[chain_match_sel_vector.get_index(i)] = true;
		}
		// continue searching for the ones where we did not find a match yet
		AdvancePointers(chain_no_match_sel_vector, no_match_count);
	}
}

template <bool MATCH>
void ScanStructure::NextSemiOrAntiJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result) {
	D_ASSERT(ht.lhs_output_in_probe.size() == result.ColumnCount());

	// create the selection vector from the matches that were found
	SelectionVector sel(STANDARD_VECTOR_SIZE);
	optional<array_ptr<sel_t>> true_sel, false_sel;
	if (MATCH) {
		true_sel = array_ptr<sel_t>(sel.data(), sel.Capacity());
	} else {
		false_sel = array_ptr<sel_t>(sel.data(), sel.Capacity());
	}
	auto true_count = SelectionVector::SelectFromBoolean(found_match.get(), nullptr, keys.size(), true_sel, false_sel);
	auto result_count = MATCH ? true_count : keys.size() - true_count;
	// construct the final result
	if (result_count > 0) {
		// extract only OUTPUT columns from probe_data
		for (idx_t i = 0; i < ht.lhs_output_in_probe.size(); i++) {
			idx_t probe_col_idx = ht.lhs_output_in_probe[i];
			result.data[i].Slice(probe_data.data[probe_col_idx], sel, result_count);
		}
	} else {
		D_ASSERT(result.size() == 0);
	}
}

void ScanStructure::NextSemiJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result) {
	// first scan for key matches
	ScanKeyMatches(keys, probe_data);

	// then construct the result from all tuples with a match
	NextSemiOrAntiJoin<true>(keys, probe_data, result);

	finished = true;
}

void ScanStructure::NextAntiJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result) {
	// first scan for key matches
	ScanKeyMatches(keys, probe_data);

	// then construct the result from all tuples that did not find a match
	NextSemiOrAntiJoin<false>(keys, probe_data, result);

	finished = true;
}

template <bool USE_DICT_EMISSION>
static void MarkChainsAsFoundLoop(JoinHashTable &ht, data_ptr_t ptrs[], const SelectionVector &chain_match_sel_vector,
                                  const idx_t result_count, const data_ptr_t dead_end_ptr) {
	for (idx_t i = 0; i < result_count; i++) {
		const auto idx = chain_match_sel_vector.get_index(i);
		auto &ptr = ptrs[idx];
		if (Load<bool>(ptr + ht.tuple_size)) { // Early out: chain has been fully marked as found before
			ptr = dead_end_ptr;
			continue;
		}

		// Fully mark chain as found
		while (true) {
			// NOTE: threadsan reports this as a data race because this can be set concurrently by separate
			// threads Technically it is, but it does not matter, since the only value that can be written is
			// "true"
			Store<bool>(true, ptr + ht.tuple_size);
			auto next_ptr = ht.GetNextPointer<USE_DICT_EMISSION>(ptr);
			if (!next_ptr) {
				break;
			}
			ptr = next_ptr;
		}
	}
}

void ScanStructure::NextRightSemiOrAntiJoin(DataChunk &keys, DataChunk &probe_data) {
	const auto ptrs = FlatVector::GetDataMutable<data_ptr_t>(pointers);
	while (!PointersExhausted()) {
		// resolve the equality_predicates for this set of keys
		idx_t result_count = ResolvePredicates(keys, probe_data, chain_match_sel_vector, nullptr);

		if (ht.non_equality_predicates.empty() && !ht.residual_predicate) {
			// we only have equality predicates - the match is found for the entire chain
			const auto dead_end_ptr = ht.dead_end.get();
			if (ht.use_dict_emission) {
				MarkChainsAsFoundLoop<true>(ht, ptrs, chain_match_sel_vector, result_count, dead_end_ptr);
			} else {
				MarkChainsAsFoundLoop<false>(ht, ptrs, chain_match_sel_vector, result_count, dead_end_ptr);
			}
		} else {
			// we have non-equality predicates - we need to evaluate the join condition for every row
			// for each match found in the current pass - mark the match as found
			for (idx_t i = 0; i < result_count; i++) {
				auto idx = chain_match_sel_vector.get_index(i);
				Store<bool>(true, ptrs[idx] + ht.tuple_size);
			}
		}

		// check the next set of pointers
		AdvancePointers();
	}

	finished = true;
}

static bool MarkJoinChunkHasUnknown(DataChunk &left, idx_t left_row, DataChunk &right) {
	D_ASSERT(left.ColumnCount() == right.ColumnCount());
	bool row_is_false[STANDARD_VECTOR_SIZE] = {false};
	bool row_is_unknown[STANDARD_VECTOR_SIZE] = {false};
	for (idx_t col_idx = 0; col_idx < left.ColumnCount(); col_idx++) {
		MarkJoinRowComparison::CompareEquality(left.data[col_idx], left_row, left.size(), right.data[col_idx],
		                                       right.size(), row_is_false, row_is_unknown);
	}
	for (idx_t right_row = 0; right_row < right.size(); right_row++) {
		if (!row_is_false[right_row] && row_is_unknown[right_row]) {
			return true;
		}
	}
	return false;
}

void JoinHashTable::ConstructMarkJoinResult(DataChunk &join_keys, DataChunk &probe_data, DataChunk &result,
                                            optional_ptr<const bool> found_match) {
	// extract OUTPUT columns from probe_data
	for (idx_t i = 0; i < lhs_output_in_probe.size(); i++) {
		idx_t probe_col_idx = lhs_output_in_probe[i];
		result.data[i].Reference(probe_data.data[probe_col_idx]);
	}

	auto &mark_vector = result.data.back();
	mark_vector.SetVectorType(VectorType::FLAT_VECTOR);
	FlatVector::SetSize(mark_vector, count_t(probe_data.size()));

	// set the cardinality AFTER referencing the probe columns: a referenced probe column may be a constant vector
	// (e.g. a constant join key) whose size must be aligned to the chunk cardinality
	result.SetChildCardinality(probe_data.size());

	// first we set the NULL values from the join keys
	// if there is any NULL in the keys, the result is NULL
	auto bool_result = FlatVector::GetDataMutable<bool>(mark_vector);
	auto &mask = FlatVector::ValidityMutable(mark_vector);
	mask.SetAllValid(probe_data.size());
	if (!HasUncorrelatedMarkJoin()) {
		for (idx_t col_idx = 0; col_idx < join_keys.ColumnCount(); col_idx++) {
			if (null_values_are_equal[col_idx]) {
				continue;
			}
			UnifiedVectorFormat jdata;
			join_keys.data[col_idx].ToUnifiedFormat(jdata);
			if (jdata.validity.CanHaveNull()) {
				for (idx_t i = 0; i < join_keys.size(); i++) {
					auto jidx = jdata.sel->get_index(i);
					if (!jdata.validity.RowIsValidUnsafe(jidx)) {
						mask.SetInvalid(i);
					}
				}
			}
		}
	}

	// now set the remaining entries to either true or false based on whether a match was found
	for (idx_t i = 0; i < probe_data.size(); i++) {
		bool_result[i] = found_match && found_match.get()[i];
	}

	// if the right side contains NULL values, the result of any FALSE becomes NULL
	if (!HasUncorrelatedMarkJoin() && has_null) {
		for (idx_t i = 0; i < probe_data.size(); i++) {
			if (!bool_result[i]) {
				mask.SetInvalid(i);
			}
		}
	}
	if (!HasUncorrelatedMarkJoin() || !mark_join_info.uncorrelated_condition_rows ||
	    mark_join_info.uncorrelated_condition_rows->Count() == 0) {
		return;
	}
	bool requires_refinement[STANDARD_VECTOR_SIZE] = {false};
	if (!mark_join_info.uncorrelated_has_null && !MarkJoinKeysHaveNull(join_keys, requires_refinement)) {
		return;
	}

	SelectionVector refinement_sel(STANDARD_VECTOR_SIZE);
	idx_t refinement_count = 0;
	for (idx_t probe_idx = 0; probe_idx < join_keys.size(); probe_idx++) {
		if ((mark_join_info.uncorrelated_has_null || requires_refinement[probe_idx]) && !bool_result[probe_idx] &&
		    mask.RowIsValid(probe_idx)) {
			refinement_sel.set_index(refinement_count++, probe_idx);
		}
	}
	if (refinement_count == 0) {
		return;
	}

	ColumnDataScanState scan_state;
	mark_join_info.uncorrelated_condition_rows->InitializeScan(scan_state);
	DataChunk rhs_chunk;
	mark_join_info.uncorrelated_condition_rows->InitializeScanChunk(rhs_chunk);
	while (refinement_count > 0 && mark_join_info.uncorrelated_condition_rows->Scan(scan_state, rhs_chunk)) {
		idx_t remaining_count = 0;
		for (idx_t candidate_idx = 0; candidate_idx < refinement_count; candidate_idx++) {
			auto probe_idx = refinement_sel.get_index(candidate_idx);
			if (MarkJoinChunkHasUnknown(join_keys, probe_idx, rhs_chunk)) {
				mask.SetInvalid(probe_idx);
			} else {
				refinement_sel.set_index(remaining_count++, probe_idx);
			}
		}
		refinement_count = remaining_count;
	}
}

void ScanStructure::ConstructMarkJoinResult(DataChunk &join_keys, DataChunk &probe_data, DataChunk &result) {
	ht.ConstructMarkJoinResult(join_keys, probe_data, result, found_match.get());
}

void ScanStructure::NextMarkJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result) {
	D_ASSERT(result.ColumnCount() == ht.lhs_output_in_probe.size() + 1);
	D_ASSERT(result.data.back().GetType() == LogicalType::BOOLEAN);
	// this method should only be called for a non-empty HT
	D_ASSERT(ht.Count() > 0);

	ScanKeyMatches(keys, probe_data);

	if (ht.mark_join_info.correlated_types.empty()) {
		ConstructMarkJoinResult(keys, probe_data, result);
	} else {
		auto &info = ht.mark_join_info;
		lock_guard<mutex> mj_lock(info.mj_lock);

		// there are correlated columns
		// first we fetch the counts from the aggregate hashtable corresponding to these entries
		D_ASSERT(keys.ColumnCount() == info.group_chunk.ColumnCount() + 1);
		for (idx_t i = 0; i < info.group_chunk.ColumnCount(); i++) {
			info.group_chunk.data[i].Reference(keys.data[i]);
		}
		info.group_chunk.SetChildCardinality(keys.size());
		info.correlated_counts->FetchAggregates(info.group_chunk, info.result_chunk);

		// extract OUTPUT columns from probe_data
		result.SetChildCardinality(probe_data.size());
		for (idx_t i = 0; i < ht.lhs_output_in_probe.size(); i++) {
			idx_t probe_col_idx = ht.lhs_output_in_probe[i];
			result.data[i].Reference(probe_data.data[probe_col_idx]);
		}

		// create the result matching vector
		auto &last_key = keys.data.back();
		auto &result_vector = result.data.back();
		// first set the null mask based on whether there were NULL values in the join key
		result_vector.SetVectorType(VectorType::FLAT_VECTOR);
		auto bool_result = FlatVector::GetDataMutable<bool>(result_vector);
		auto &mask = FlatVector::ValidityMutable(result_vector);

		// Set null mask based on NULL values in join key
		switch (last_key.GetVectorType()) {
		case VectorType::CONSTANT_VECTOR:
			if (ConstantVector::IsNull(last_key)) {
				mask.SetAllInvalid(probe_data.size());
			}
			break;
		case VectorType::FLAT_VECTOR:
			mask.Copy(FlatVector::ValidityMutable(last_key), probe_data.size());
			break;
		default: {
			UnifiedVectorFormat kdata;
			last_key.ToUnifiedFormat(kdata);
			for (idx_t i = 0; i < probe_data.size(); i++) {
				auto kidx = kdata.sel->get_index(i);
				mask.Set(i, kdata.validity.RowIsValid(kidx));
			}
			break;
		}
		}

		auto count_star = FlatVector::GetData<int64_t>(info.result_chunk.data[0]);
		auto count = FlatVector::GetData<int64_t>(info.result_chunk.data[1]);

		// set the entries to either true or false based on whether a match was found
		for (idx_t i = 0; i < probe_data.size(); i++) {
			D_ASSERT(count_star[i] >= count[i]);
			bool_result[i] = found_match ? found_match[i] : false;
			if (!bool_result[i] && count_star[i] > count[i]) {
				// RHS has NULL value and result is false: set to null
				mask.SetInvalid(i);
			}
			if (count_star[i] == 0) {
				// count == 0, set null mask to false (we know the result is false now)
				mask.SetValid(i);
			}
		}
		FlatVector::SetSize(result_vector, count_t(probe_data.size()));
	}
	finished = true;
}

void ScanStructure::NextLeftJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result) {
	// a LEFT OUTER JOIN is identical to an INNER JOIN except all tuples that do
	// not have a match must return at least one tuple (with the right side set
	// to NULL in every column)
	NextInnerJoin(keys, probe_data, result);

	if (result.size() == 0) {
		// no entries left from the normal join
		// fill in the result of the remaining left tuples
		// together with NULL values on the right-hand side
		idx_t remaining_count = 0;
		SelectionVector sel(STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < probe_data.size(); i++) {
			if (!found_match[i]) {
				sel.set_index(remaining_count++, i);
			}
		}

		if (remaining_count > 0) {
			// extract only OUTPUT columns from probe_data
			for (idx_t i = 0; i < ht.lhs_output_in_probe.size(); i++) {
				idx_t probe_col_idx = ht.lhs_output_in_probe[i];
				result.data[i].Slice(probe_data.data[probe_col_idx], sel, remaining_count);
			}
			// now set the right side to NULL
			for (idx_t i = ht.lhs_output_in_probe.size(); i < result.ColumnCount(); i++) {
				Vector &vec = result.data[i];
				ConstantVector::SetNull(vec, count_t(remaining_count));
			}
		}
		finished = true;
	}
}

void ScanStructure::NextSingleJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result) {
	idx_t result_count = 0;
	SelectionVector result_sel(STANDARD_VECTOR_SIZE);

	while (this->count > 0) {
		idx_t match_count = ResolvePredicates(keys, probe_data, chain_match_sel_vector, &chain_no_match_sel_vector);
		idx_t no_match_count = this->count - match_count;

		// Mark each of the matches as found
		for (idx_t i = 0; i < match_count; i++) {
			// found a match for this index
			auto index = chain_match_sel_vector.get_index(i);
			found_match[index] = true;
			result_sel.set_index(result_count++, index);
		}

		// continue searching for the ones where we did not find a match yet
		AdvancePointers(chain_no_match_sel_vector, no_match_count);
	}

	// extract OUTPUT columns from probe_data
	D_ASSERT(ht.lhs_output_in_probe.size() > 0);
	for (idx_t i = 0; i < ht.lhs_output_in_probe.size(); i++) {
		idx_t probe_col_idx = ht.lhs_output_in_probe[i];
		result.data[i].Reference(probe_data.data[probe_col_idx]);
	}

	// now fetch the data from the RHS
	for (idx_t i = 0; i < ht.output_columns.size(); i++) {
		auto &vector = result.data[ht.lhs_output_in_probe.size() + i];
		// set NULL entries for every entry that was not found
		for (idx_t j = 0; j < probe_data.size(); j++) {
			if (!found_match[j]) {
				FlatVector::SetNull(vector, j, true);
			}
		}
		const auto output_col_idx = ht.output_columns[i];
		D_ASSERT(vector.GetType() == ht.layout_ptr->GetTypes()[output_col_idx]);
		GatherResult(vector, result_sel, result_sel, result_count, output_col_idx);
		FlatVector::SetSize(vector, count_t(probe_data.size()));
	}
	result.SetChildCardinality(probe_data.size());

	// like the SEMI, ANTI and MARK join types, the SINGLE join only ever does one pass over the HT per input chunk
	finished = true;

	if (ht.single_join_error_on_multiple_rows && result_count > 0) {
		// we need to throw an error if there are multiple rows per key
		// advance pointers for those rows
		AdvancePointers(result_sel, result_count);

		// now resolve the predicates
		idx_t match_count = ResolvePredicates(keys, probe_data, chain_match_sel_vector, nullptr);
		if (match_count > 0) {
			// we found at least one duplicate row - throw
			throw InvalidInputException(
			    "More than one row returned by a subquery used as an expression - scalar subqueries can only "
			    "return a single row.\n\nUse \"SET scalar_subquery_error_on_multiple_rows=false\" to revert to "
			    "previous behavior of returning a random row.");
		}

		this->count = 0;
	}
}

void ScanStructure::NextUniqueLeftJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result) {
	// Unique left join: RHS has unique keys, so at most one match per LHS row.
	// Single pass - no state machine needed.
	D_ASSERT(!ht.chains_longer_than_one.load(std::memory_order_relaxed));

	// First scan for key matches
	ScanKeyMatches(keys, probe_data);
	// Should always be 0 after scanning key matches once when !ht.chains_longer_than_one
	D_ASSERT(this->count == 0);

	// Build result_sel from found_match so we know which positions to gather RHS data from.
	SelectionVector result_sel(STANDARD_VECTOR_SIZE);
	const idx_t probe_size = probe_data.size();
	auto result_count = SelectionVector::SelectFromBoolean(found_match.get(), nullptr, probe_size,
	                                                       array_ptr<sel_t>(result_sel.data(), result_sel.Capacity()));

	// Reference the columns of the left side from the result (zero-copy).
	D_ASSERT(ht.lhs_output_in_probe.size() > 0);
	for (idx_t i = 0; i < ht.lhs_output_in_probe.size(); i++) {
		idx_t probe_col_idx = ht.lhs_output_in_probe[i];
		result.data[i].Reference(probe_data.data[probe_col_idx]);
	}

	// Fetch RHS data. If all rows matched we can skip NULL-setting and gather directly.
	// Otherwise, mark unmatched rows as NULL before gathering matched rows.
	const bool all_match = result_count == probe_size;
	for (idx_t i = 0; i < ht.output_columns.size(); i++) {
		auto &vector = result.data[ht.lhs_output_in_probe.size() + i];
		if (!all_match) {
			for (idx_t j = 0; j < probe_size; j++) {
				if (!found_match[j]) {
					FlatVector::SetNull(vector, j, true);
				}
			}
		}
		const auto output_col_idx = ht.output_columns[i];
		D_ASSERT(vector.GetType() == ht.layout_ptr->GetTypes()[output_col_idx]);
		GatherResult(vector, result_sel, result_sel, result_count, output_col_idx);
		FlatVector::SetSize(vector, count_t(probe_size));
	}

	// single pass - done
	result.SetChildCardinality(probe_data.size());
	finished = true;
}

void JoinHashTable::ScanFullOuter(JoinHTScanState &state, Vector &addresses, DataChunk &result) const {
	// scan the HT starting from the current position and check which rows from the build side did not find a match
	auto key_locations = FlatVector::GetDataMutable<data_ptr_t>(addresses);
	idx_t found_entries = 0;

	auto &iterator = state.iterator;
	if (iterator.Done()) {
		return;
	}

	// When scanning Full Outer for right semi joins, we only propagate matches that have true
	// Right Semi Joins do not propagate values during the probe phase, since we do not want to
	// duplicate RHS rows.
	bool match_propagation_value = false;
	if (join_type == JoinType::RIGHT_SEMI) {
		match_propagation_value = true;
	}

	const auto row_locations = iterator.GetRowLocations();
	do {
		const auto count = iterator.GetCurrentChunkCount();
		idx_t i = state.offset_in_chunk;
		while (i < count && found_entries < STANDARD_VECTOR_SIZE) {
			// Leave room for a scratch write even when every scanned row matches.
			const auto end = MinValue(count, i + STANDARD_VECTOR_SIZE - found_entries);
			for (; i < end; i++) {
				const auto row = row_locations[i];
				key_locations[found_entries] = row;
				found_entries += Load<bool>(row + tuple_size) == match_propagation_value;
			}
		}
		if (found_entries == STANDARD_VECTOR_SIZE) {
			state.offset_in_chunk = i;
			break;
		}
		state.offset_in_chunk = 0;
		state.chunks_done++;
	} while (iterator.Next());

	// now gather from the found rows
	if (found_entries == 0) {
		return;
	}
	idx_t left_column_count = result.ColumnCount() - output_columns.size();
	if (join_type == JoinType::RIGHT_SEMI || join_type == JoinType::RIGHT_ANTI) {
		left_column_count = 0;
	}
	const auto &sel_vector = *FlatVector::IncrementalSelectionVector();
	// set the left side as a constant NULL
	for (idx_t i = 0; i < left_column_count; i++) {
		Vector &vec = result.data[i];
		ConstantVector::SetNull(vec, count_t(found_entries));
	}

	// gather the values from the RHS
	GatherRHS(addresses, sel_vector, found_entries, result, left_column_count);
	// the result may have no columns, so set the count explicitly
	result.SetChildCardinality(found_entries);
}

idx_t JoinHashTable::FillWithHTOffsets(JoinHTScanState &state, Vector &addresses) {
	// iterate over HT
	auto key_locations = FlatVector::GetDataMutable<data_ptr_t>(addresses);
	idx_t key_count = 0;

	auto &iterator = state.iterator;
	const auto row_locations = iterator.GetRowLocations();
	do {
		const auto count = iterator.GetCurrentChunkCount();
		for (idx_t i = 0; i < count; i++) {
			key_locations[key_count + i] = row_locations[i];
		}
		key_count += count;
	} while (iterator.Next());

	return key_count;
}

idx_t JoinHashTable::ScanKeyColumn(Vector &addresses, Vector &result, idx_t column_index) const {
	// nothing to scan if the build side is empty
	if (data_collection->ChunkCount() == 0) {
		return 0;
	}
	D_ASSERT(result.GetType() == layout_ptr->GetTypes()[column_index]);
	JoinHTScanState join_ht_state(*data_collection, 0, data_collection->ChunkCount(),
	                              TupleDataPinProperties::KEEP_EVERYTHING_PINNED);
	auto key_count = FillWithHTOffsets(join_ht_state, addresses);
	if (key_count == 0) {
		return 0;
	}
	const auto &sel = *FlatVector::IncrementalSelectionVector();
	data_collection->Gather(addresses, sel, key_count, column_index, result, sel, nullptr);
	return key_count;
}

idx_t JoinHashTable::GetTotalSize(const vector<idx_t> &partition_sizes, const vector<idx_t> &partition_counts,
                                  idx_t &max_partition_size, idx_t &max_partition_count) const {
	const auto num_partitions = RadixPartitioning::NumberOfPartitions(radix_bits);
	const auto mark_join_size =
	    mark_join_info.uncorrelated_condition_rows ? mark_join_info.uncorrelated_condition_rows->SizeInBytes() : 0;

	idx_t total_size = 0;
	idx_t total_count = 0;
	idx_t max_partition_ht_size = 0;
	max_partition_size = 0;
	max_partition_count = 0;
	for (idx_t i = 0; i < num_partitions; i++) {
		total_size += partition_sizes[i];
		total_count += partition_counts[i];

		auto partition_size = partition_sizes[i] + PointerTableSize(partition_counts[i]);
		if (partition_size > max_partition_ht_size) {
			max_partition_ht_size = partition_size;
			max_partition_size = partition_sizes[i];
			max_partition_count = partition_counts[i];
		}
	}

	if (total_count == 0) {
		return mark_join_size;
	}

	return total_size + PointerTableSize(total_count) + mark_join_size;
}

idx_t JoinHashTable::GetTotalSize(const vector<reference<JoinHashTable>> &local_hts, idx_t &max_partition_size,
                                  idx_t &max_partition_count) const {
	const auto num_partitions = RadixPartitioning::NumberOfPartitions(radix_bits);
	vector<idx_t> partition_sizes(num_partitions, 0);
	vector<idx_t> partition_counts(num_partitions, 0);
	for (auto &ht : local_hts) {
		ht.get().GetSinkCollection().GetSizesAndCounts(partition_sizes, partition_counts);
	}

	auto total_size = GetTotalSize(partition_sizes, partition_counts, max_partition_size, max_partition_count);
	for (auto &ht : local_hts) {
		auto &condition_rows = ht.get().mark_join_info.uncorrelated_condition_rows;
		if (condition_rows) {
			total_size += condition_rows->SizeInBytes();
		}
	}
	return total_size;
}

idx_t JoinHashTable::GetRemainingSize() const {
	const auto num_partitions = RadixPartitioning::NumberOfPartitions(radix_bits);
	auto &partitions = sink_collection->GetPartitions();

	idx_t count = 0;
	idx_t data_size = 0;
	for (idx_t partition_idx = 0; partition_idx < num_partitions; partition_idx++) {
		if (completed_partitions.RowIsValidUnsafe(partition_idx)) {
			continue;
		}
		count += partitions[partition_idx]->Count();
		data_size += partitions[partition_idx]->SizeInBytes();
	}

	const auto mark_join_size =
	    mark_join_info.uncorrelated_condition_rows ? mark_join_info.uncorrelated_condition_rows->SizeInBytes() : 0;
	return data_size + PointerTableSize(count) + mark_join_size;
}

void JoinHashTable::Unpartition() {
	data_collection = sink_collection->GetUnpartitioned();
}

void JoinHashTable::SetRepartitionRadixBits(const idx_t max_ht_size, const idx_t max_partition_size,
                                            const idx_t max_partition_count) {
	D_ASSERT(max_partition_size + PointerTableSize(max_partition_count) > max_ht_size);

	const auto max_added_bits = RadixPartitioning::MAX_RADIX_BITS - radix_bits;
	idx_t added_bits = 1;
	for (; added_bits < max_added_bits; added_bits++) {
		double partition_multiplier = static_cast<double>(RadixPartitioning::NumberOfPartitions(added_bits));

		auto new_estimated_size = static_cast<double>(max_partition_size) / partition_multiplier;
		auto new_estimated_count = static_cast<double>(max_partition_count) / partition_multiplier;
		auto new_estimated_ht_size =
		    new_estimated_size + static_cast<double>(PointerTableSize(LossyNumericCast<idx_t>(new_estimated_count)));

		if (new_estimated_ht_size <= static_cast<double>(max_ht_size) / 4) {
			// Aim for an estimated partition size of max_ht_size / 4
			break;
		}
	}
	radix_bits += added_bits;
	sink_collection = make_uniq<RadixPartitionedTupleData>(buffer_manager, layout_ptr, MemoryTag::HASH_TABLE,
	                                                       radix_bits, layout_ptr->ColumnCount() - 1, context);

	// Need to initialize again after changing the number of bits
	InitializePartitionMasks();
}

void JoinHashTable::InitializePartitionMasks() {
	const auto num_partitions = RadixPartitioning::NumberOfPartitions(radix_bits);

	current_partitions.Initialize(num_partitions);
	current_partitions.SetAllInvalid(num_partitions);

	completed_partitions.Initialize(num_partitions);
	completed_partitions.SetAllInvalid(num_partitions);
}

idx_t JoinHashTable::CurrentPartitionCount() const {
	const auto num_partitions = RadixPartitioning::NumberOfPartitions(radix_bits);
	D_ASSERT(current_partitions.Capacity() == num_partitions);
	return current_partitions.CountValid(num_partitions);
}

const ValidityMask &JoinHashTable::GetCurrentPartitions() const {
	return current_partitions;
}

idx_t JoinHashTable::FinishedPartitionCount() const {
	const auto num_partitions = RadixPartitioning::NumberOfPartitions(radix_bits);
	D_ASSERT(completed_partitions.Capacity() == num_partitions);
	// We already marked the active partitions as done, so we have to subtract them here
	return completed_partitions.CountValid(num_partitions) - CurrentPartitionCount();
}

void JoinHashTable::Repartition(JoinHashTable &global_ht) {
	auto new_sink_collection =
	    make_uniq<RadixPartitionedTupleData>(buffer_manager, layout_ptr, MemoryTag::HASH_TABLE, global_ht.radix_bits,
	                                         layout_ptr->ColumnCount() - 1, context);
	sink_collection->Repartition(context, *new_sink_collection);
	sink_collection = std::move(new_sink_collection);
	global_ht.Merge(*this);
}

void JoinHashTable::Reset() {
	bucket_shift = 0;
	data_collection->Reset();
	hash_map.Reset();
	current_partitions.SetAllInvalid(RadixPartitioning::NumberOfPartitions(radix_bits));
	finalized = false;
}

static void ResetMarkJoinInfo(JoinHashTable &ht) {
	auto &info = ht.mark_join_info;
	if (!info.correlated_types.empty()) {
		vector<AggregateObject> correlated_aggregates;
		vector<LogicalType> payload_types;
		correlated_aggregates.reserve(info.correlated_aggregates.size());
		payload_types.reserve(info.correlated_aggregates.size());
		for (auto &expr : info.correlated_aggregates) {
			auto &aggr = expr->Cast<BoundAggregateExpression>();
			correlated_aggregates.emplace_back(aggr);
			payload_types.push_back(aggr.GetReturnType());
		}
		auto &allocator = BufferAllocator::Get(ht.context);
		info.correlated_counts = make_uniq<GroupedAggregateHashTable>(ht.context, allocator, info.correlated_types,
		                                                              payload_types, std::move(correlated_aggregates));
		info.group_chunk.Reset();
		info.correlated_payload.Reset();
		info.result_chunk.Reset();
	}
	if (info.uncorrelated_condition_rows) {
		info.uncorrelated_has_null = false;
		info.uncorrelated_condition_rows = make_uniq<ColumnDataCollection>(ht.context, ht.condition_types);
	}
}

void JoinHashTable::ResetForNewIterationSinglePartition() {
	// Always use a single partition (radix_bits=0) to avoid per-iteration overhead of resetting
	// and re-creating many radix partitions when only one thread builds the hash table. This also
	// keeps an unpublished shell consistent with the global radix bits when it is recycled.
	radix_bits = 0;
	InitializePartitionMasks();
	if (!layout_ptr) {
		// layout was never published (no Sink chunk last iteration); the next first chunk will publish
		return;
	}
	// Return to the unpublished state (see FinishInitWithLayout): the next iteration's first chunk publishes the
	// layout afresh. The rerun build is the same operator, but it may deliver a different dictionary (e.g. a
	// nextval() in its build side), a dictionary of another size, or none at all, so a narrowed slot and the
	// dictionary it indexes into cannot be carried over.
	layout_ptr.reset();
	dict_info.reset();
	data_collection.reset();
	sink_collection.reset();
	capacity = DConstants::INVALID_INDEX;
	bitmask = DConstants::INVALID_INDEX;
	bucket_shift = 0;
	finalized = false;
	has_null = false;
	chains_longer_than_one.store(false, std::memory_order_relaxed);
	total_probe_matches = 0;
	load_factor = DEFAULT_LOAD_FACTOR;
	bloom_filter.reset();
	should_build_bloom_filter = false;
	prefix_range_filter.reset();
	should_build_prefix_range_filter = false;
	ResetMarkJoinInfo(*this);
	// The next iteration may rebuild a different small build side, so this iteration's dictionary
	// state is stale. Keep in lock-step with BuildDictionaryArrays, which sets these four fields.
	dict_arrays.clear();
	aux_next_ptrs.Reset();
	aux_next_ptrs_data = nullptr;
	use_dict_emission = false;
}

bool JoinHashTable::PrepareExternalFinalize(const idx_t max_ht_size) {
	if (finalized) {
		Reset();
	}

	const auto num_partitions = RadixPartitioning::NumberOfPartitions(radix_bits);
	D_ASSERT(current_partitions.Capacity() == num_partitions);
	D_ASSERT(completed_partitions.Capacity() == num_partitions);
	D_ASSERT(current_partitions.CheckAllInvalid(num_partitions));

	if (completed_partitions.CheckAllValid(num_partitions)) {
		return false; // All partitions are done
	}

	// Create vector with unfinished partition indices
	auto &partitions = sink_collection->GetPartitions();
	auto min_partition_size = NumericLimits<idx_t>::Maximum();
	vector<idx_t> partition_indices;
	partition_indices.reserve(num_partitions);
	for (idx_t partition_idx = 0; partition_idx < num_partitions; partition_idx++) {
		if (completed_partitions.RowIsValidUnsafe(partition_idx)) {
			continue;
		}
		partition_indices.push_back(partition_idx);
		// Keep track of min partition size
		const auto size =
		    partitions[partition_idx]->SizeInBytes() + PointerTableSize(partitions[partition_idx]->Count());
		min_partition_size = MinValue(min_partition_size, size);
	}

	// Sort partitions by size, from small to large
	std::stable_sort(partition_indices.begin(), partition_indices.end(), [&](const idx_t &lhs, const idx_t &rhs) {
		const auto lhs_size = partitions[lhs]->SizeInBytes() + PointerTableSize(partitions[lhs]->Count());
		const auto rhs_size = partitions[rhs]->SizeInBytes() + PointerTableSize(partitions[rhs]->Count());
		// We divide by min_partition_size, effectively rounding everything down to a multiple of min_partition_size
		// Makes it so minor differences in partition sizes don't mess up the original order
		// Retaining as much of the original order as possible reduces I/O (partition idx determines eviction queue idx)
		return lhs_size / min_partition_size < rhs_size / min_partition_size;
	});

	// Determine which partitions should go next
	idx_t count = 0;
	idx_t data_size = 0;
	for (const auto &partition_idx : partition_indices) {
		D_ASSERT(!completed_partitions.RowIsValidUnsafe(partition_idx));
		const auto incl_count = count + partitions[partition_idx]->Count();
		const auto incl_data_size = data_size + partitions[partition_idx]->SizeInBytes();
		const auto incl_ht_size = incl_data_size + PointerTableSize(incl_count);
		if (count > 0 && incl_ht_size > max_ht_size) {
			break; // Always add at least one partition
		}
		count = incl_count;
		data_size = incl_data_size;
		current_partitions.SetValidUnsafe(partition_idx);     // Mark as currently active
		data_collection->Combine(*partitions[partition_idx]); // Move partition to the main data collection
		completed_partitions.SetValidUnsafe(partition_idx);   // Also already mark as done
	}
	D_ASSERT(Count() == count);

	return true;
}

void JoinHashTable::ProbeAndSpill(ScanStructure &scan_structure, DataChunk &probe_keys, TupleDataChunkState &key_state,
                                  ProbeState &probe_state, DataChunk &probe_chunk, ProbeSpill &probe_spill,
                                  ProbeSpillLocalAppendState &spill_state, DataChunk &spill_chunk) {
	// hash all the keys
	Vector hashes(LogicalType::HASH);
	Hash(probe_keys, *FlatVector::IncrementalSelectionVector(), probe_keys.size(), hashes);

	// find out which keys we can match with the current pinned partitions
	SelectionVector true_sel(STANDARD_VECTOR_SIZE);
	SelectionVector false_sel(STANDARD_VECTOR_SIZE);
	const auto true_count =
	    RadixPartitioning::Select(hashes, FlatVector::IncrementalSelectionVector(), probe_keys.size(), radix_bits,
	                              current_partitions, &true_sel, &false_sel);
	const auto false_count = probe_keys.size() - true_count;

	// can't probe these values right now, append to spill
	spill_chunk.Reset();
	spill_chunk.Reference(probe_chunk);
	spill_chunk.data.back().Reference(hashes);
	spill_chunk.Slice(false_sel, false_count);
	probe_spill.Append(spill_chunk, spill_state);

	// slice the stuff we CAN probe right now
	hashes.Slice(true_sel, true_count);
	probe_keys.Slice(true_sel, true_count);
	probe_chunk.Slice(true_sel, true_count);

	optional_ptr<const SelectionVector> current_sel;
	InitializeScanStructure(scan_structure, probe_keys, key_state, current_sel);
	if (scan_structure.count == 0) {
		return;
	}

	// now initialize the pointers of the scan structure based on the hashes
	GetRowPointers(probe_keys, key_state, probe_state, hashes, current_sel, scan_structure.count,
	               scan_structure.pointers, scan_structure.sel_vector, scan_structure.has_null_value_filter);
}

ProbeSpill::ProbeSpill(JoinHashTable &ht, ClientContext &context, const vector<LogicalType> &probe_types)
    : ht(ht), context(context), probe_types(probe_types) {
	global_partitions =
	    make_uniq<RadixPartitionedColumnData>(context, probe_types, ht.radix_bits, probe_types.size() - 1);
	column_ids.reserve(probe_types.size());
	for (column_t column_id = 0; column_id < probe_types.size(); column_id++) {
		column_ids.emplace_back(column_id);
	}
}

ProbeSpillLocalState ProbeSpill::RegisterThread() {
	ProbeSpillLocalAppendState result;
	lock_guard<mutex> guard(lock);
	local_partitions.emplace_back(global_partitions->CreateShared());
	local_partition_append_states.emplace_back(make_uniq<PartitionedColumnDataAppendState>());
	local_partitions.back()->InitializeAppendState(*local_partition_append_states.back());

	result.local_partition = local_partitions.back().get();
	result.local_partition_append_state = local_partition_append_states.back().get();
	return result;
}

void ProbeSpill::Append(DataChunk &chunk, ProbeSpillLocalAppendState &local_state) {
	local_state.local_partition->Append(*local_state.local_partition_append_state, chunk);
}

void ProbeSpill::Finalize() {
	D_ASSERT(local_partitions.size() == local_partition_append_states.size());
	for (idx_t i = 0; i < local_partition_append_states.size(); i++) {
		local_partitions[i]->FlushAppendState(*local_partition_append_states[i]);
	}
	for (auto &local_partition : local_partitions) {
		global_partitions->Combine(*local_partition);
	}
	local_partitions.clear();
	local_partition_append_states.clear();
}

void ProbeSpill::PrepareNextProbe() {
	global_spill_collection.reset();
	auto &partitions = global_partitions->GetPartitions();
	if (partitions.empty() || ht.current_partitions.CheckAllInvalid(partitions.size())) {
		// Can't probe, just make an empty one
		global_spill_collection =
		    make_uniq<ColumnDataCollection>(BufferManager::GetBufferManager(context), probe_types);
	} else {
		// Move current partitions to the global spill collection
		for (idx_t partition_idx = 0; partition_idx < partitions.size(); partition_idx++) {
			if (!ht.current_partitions.RowIsValidUnsafe(partition_idx)) {
				continue;
			}
			auto &partition = partitions[partition_idx];
			if (!global_spill_collection) {
				global_spill_collection = std::move(partition);
			} else if (partition->Count() != 0) {
				global_spill_collection->Combine(*partition);
			}
			partition.reset();
		}
	}
	consumer = make_uniq<ColumnDataConsumer>(*global_spill_collection, column_ids);
	consumer->InitializeScan();
}

//===--------------------------------------------------------------------===//
// Small Build Side Dictionary Emission
//===--------------------------------------------------------------------===//
//! Threshold constants gating small-build-side dictionary emission
struct DictionaryEmissionActivation {
	//! Caps the dict_arrays allocation; 8 MiB keeps them in L3 and bounds aux_next_ptrs
	static constexpr idx_t MAX_BUILD_PAYLOAD_BYTES = 8ULL * 1024ULL * 1024ULL;
	//! Below this, dictionary-emission setup is not amortized by probe-side savings
	static constexpr idx_t MIN_PROBE_ROWS = 262144;
	//! Below this ratio, per-row savings are unlikely to amortize the setup cost
	static constexpr idx_t PROBE_BUILD_RATIO = 100;
};

idx_t JoinHashTable::ComputeBuildPayloadBytes(const vector<LogicalType> &rhs_output_types) const {
	// the size of variable-size strings isn't counted here because they are already stored in the TupleDataCollection
	idx_t payload_bytes_per_row = 0;
	for (const auto &type : rhs_output_types) {
		payload_bytes_per_row += GetTypeIdSize(type.InternalType());
	}
	return Count() * payload_bytes_per_row;
}

bool JoinHashTable::CanUseDictionaryEmission(const PhysicalHashJoin &op, bool external, idx_t probe_cardinality) const {
	// external joins rebuild partitions, invalidating row pointers
	if (external) {
		return false;
	}
	// mutually exclusive with the dict-surviving path: that path narrows the payload slot to a 1/2/4-byte
	// index, while NEXT_PTR embedding here overwrites a different field and assumes payload bytes are intact.
	if (HasDictSurvivingColumns()) {
		return false;
	}
	// SINGLE joins need FlatVector::SetNull for unmatched rows; dictionary vectors cannot supply it
	if (join_type == JoinType::SINGLE) {
		return false;
	}
	if (op.rhs_output_columns.col_types.empty()) {
		return false;
	}
	if (Count() == 0) {
		return false;
	}
	if (probe_cardinality < DictionaryEmissionActivation::MIN_PROBE_ROWS) {
		return false;
	}
	if (probe_cardinality < DictionaryEmissionActivation::PROBE_BUILD_RATIO * Count()) {
		return false;
	}
	// Vector::Dictionary does not support nested types
	for (const auto &type : op.rhs_output_columns.col_types) {
		switch (type.InternalType()) {
		case PhysicalType::STRUCT:
		case PhysicalType::LIST:
		case PhysicalType::ARRAY:
			return false;
		default:
			break;
		}
	}
	if (ComputeBuildPayloadBytes(op.rhs_output_columns.col_types) >
	    DictionaryEmissionActivation::MAX_BUILD_PAYLOAD_BYTES) {
		return false;
	}
	// Count() fits in the uint32_t dict index stored into NEXT_PTR by BuildDictionaryArrays
	D_ASSERT(Count() <= NumericLimits<uint32_t>::Maximum());
	return true;
}

void JoinHashTable::EmitDictVectors(const data_ptr_t *row_ptrs, const SelectionVector &ptr_sel, idx_t count,
                                    DataChunk &result, idx_t rhs_col_offset) const {
	D_ASSERT(output_columns.size() == dict_arrays.size());
	SelectionVector build_sel_vec(count);
	for (idx_t i = 0; i < count; i++) {
		const auto idx = ptr_sel.get_index(i);
		build_sel_vec.set_index(i, Load<uint32_t>(row_ptrs[idx] + pointer_offset));
	}
	for (idx_t col_idx = 0; col_idx < output_columns.size(); col_idx++) {
		auto &vector = result.data[rhs_col_offset + col_idx];
		vector.Dictionary(dict_arrays[col_idx], build_sel_vec, count);
	}
}

void JoinHashTable::BuildDictionaryArrays(const PhysicalHashJoin &op) {
	auto &collection = *data_collection;
	const auto build_count = collection.Count();
	// preconditions enforced by CanUseDictionaryEmission
	D_ASSERT(build_count > 0);
	// dict index is a uint32_t; assert defensively if MAX_BUILD_PAYLOAD_BYTES is ever relaxed
	D_ASSERT(build_count <= NumericLimits<uint32_t>::Maximum());

	// collect a row pointer per build row; the TDC holds its own BufferHandles for its lifetime,
	// and FinishEvent has already verified everything is pinned before calling us
	Vector row_pointer_vector(LogicalType::POINTER, build_count);
	JoinHTScanState scan_state(collection, 0, collection.ChunkCount(), TupleDataPinProperties::KEEP_EVERYTHING_PINNED);
	const auto collected = FillWithHTOffsets(scan_state, row_pointer_vector);
	D_ASSERT(collected == build_count);
	(void)collected;
	const auto row_ptrs = FlatVector::GetData<data_ptr_t>(row_pointer_vector);

	// LEFT / OUTER joins fill unmatched probe rows with a constant-NULL vector (NextLeftJoin), so they do not
	// wrap this entry on every chunk; it is only a global dictionary when every chunk goes through EmitDictVectors.
	const bool dict_on_every_chunk = join_type != JoinType::LEFT && join_type != JoinType::OUTER;

	// gather RHS output columns into columnar dictionary arrays
	const auto &sel = *FlatVector::IncrementalSelectionVector();
	for (idx_t col_idx = 0; col_idx < op.rhs_output_columns.col_types.size(); col_idx++) {
		const auto &type = op.rhs_output_columns.col_types[col_idx];
		auto dict_entry = dict_on_every_chunk ? DictionaryVector::CreateReusableGlobalDictionary(type, build_count)
		                                      : DictionaryVector::CreateReusableDictionary(type, build_count);
		const auto output_col_idx = output_columns[col_idx];
		collection.Gather(row_pointer_vector, sel, build_count, output_col_idx, dict_entry->data, sel, nullptr);
		dict_arrays.emplace_back(std::move(dict_entry));
	}

	// save chain pointers before overwriting NEXT_PTR; GetNextPointer reads them back.
	// One extra slot is reserved at the end and left null so that `dead_end` has an index
	// that terminates a chain. Without it, `dead_end` is zeroed and therefore resolves to
	// index 0, which belongs to a real row, and following it walks an unrelated chain.
	const auto has_chains = chains_longer_than_one.load(std::memory_order_relaxed);
	if (has_chains) {
		aux_next_ptrs = buffer_manager.GetBufferAllocator().Allocate((build_count + 1) * sizeof(data_ptr_t));
		aux_next_ptrs_data = reinterpret_cast<data_ptr_t *>(aux_next_ptrs.get());
		aux_next_ptrs_data[build_count] = nullptr;
		Store<uint32_t>(static_cast<uint32_t>(build_count), dead_end.get() + pointer_offset);
	}

	// save the original NEXT_PTR into aux_next_ptrs (if chains exist) and embed the dict index
	for (idx_t i = 0; i < build_count; i++) {
		const auto next_ptr_location = row_ptrs[i] + pointer_offset;
		if (has_chains) {
			aux_next_ptrs_data[i] = LoadPointer(next_ptr_location);
		}
		Store<uint32_t>(static_cast<uint32_t>(i), next_ptr_location);
	}

	use_dict_emission = true;
}

} // namespace duckdb
