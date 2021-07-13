#include "duckdb/execution/operator/join/perfect_hash_join_executor.hpp"

#include "duckdb/common/types/row_layout.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp"

namespace duckdb {

PerfectHashJoinExecutor::PerfectHashJoinExecutor(PerfectHashJoinStats pjoin_stats_) : pjoin_stats(pjoin_stats_) {
}

bool PerfectHashJoinExecutor::CanDoPerfectHashJoin() {
	return pjoin_stats.is_build_small;
}

void PerfectHashJoinExecutor::BuildPerfectHashTable(JoinHashTable *hash_table_ptr, JoinHTScanState &join_ht_state,
                                                    LogicalType key_type) {
	// First, allocate memory for each build column
	auto build_size = pjoin_stats.build_range + 1;
	for (auto type : hash_table_ptr->build_types) {
		perfect_hash_table.emplace_back(type, build_size);
	}
	// and for duplicate_checking
	bitmap_build_idx = unique_ptr<bool[]>(new bool[build_size]);
	memset(bitmap_build_idx.get(), 0, sizeof(bool) * build_size); // set false

	// Now fill columns with build data
	FullScanHashTable(join_ht_state, key_type, hash_table_ptr);
}

void PerfectHashJoinExecutor::FullScanHashTable(JoinHTScanState &state, LogicalType key_type,
                                                JoinHashTable *hash_table) {
	Vector tuples_addresses(LogicalType::POINTER, hash_table->size());      // allocate space for all the tuples
	auto key_locations = FlatVector::GetData<data_ptr_t>(tuples_addresses); // get a pointer to vector data
	// TODO: In a parallel finalize: One should exclusivly lock and each thread should do one part of the code below.
	// Go through all the blocks and fill the keys addresses
	auto keys_count = hash_table->FillWithHTOffsets(key_locations, state);
	// Scan the build keys in the hash table
	Vector build_vector(key_type, keys_count);
	RowOperations::FullScanColumn(hash_table->layout, tuples_addresses, build_vector, keys_count, 0);
	// Now fill the selection vector using the build keys and create a sequential vector
	// todo: add check for fast pass when probe is part of build domain
	SelectionVector sel_build(keys_count + 1);
	SelectionVector sel_tuples(keys_count + 1);
	FillSelectionVectorSwitchBuild(build_vector, sel_build, sel_tuples, keys_count);
	// early out
	if (has_duplicates)
		return;
	if (unique_keys == pjoin_stats.build_range + 1 && !hash_table->has_null) {
		pjoin_stats.is_build_dense = true;
	}
	keys_count = unique_keys; // do not condider keys out of the range
	// Full scan the remaining build columns and fill the perfect hash table
	for (idx_t i = 0; i < hash_table->build_types.size(); i++) {
		auto &vector = perfect_hash_table[i];
		D_ASSERT(vector.GetType() == hash_table->build_types[i]);
		const auto col_no = hash_table->condition_types.size() + i;
		const auto col_offset = hash_table->layout.GetOffsets()[col_no];
		RowOperations::Gather(tuples_addresses, sel_tuples, vector, sel_build, keys_count, col_offset, col_no);
	}
}

void PerfectHashJoinExecutor::FillSelectionVectorSwitchBuild(Vector &source, SelectionVector &sel_vec,
                                                             SelectionVector &seq_sel_vec, idx_t count) {
	switch (source.GetType().id()) {
	case LogicalTypeId::TINYINT:
		TemplatedFillSelectionVectorBuild<int8_t>(source, sel_vec, seq_sel_vec, count);
		break;
	case LogicalTypeId::SMALLINT:
		TemplatedFillSelectionVectorBuild<int16_t>(source, sel_vec, seq_sel_vec, count);
		break;
	case LogicalTypeId::INTEGER:
		TemplatedFillSelectionVectorBuild<int32_t>(source, sel_vec, seq_sel_vec, count);
		break;
	case LogicalTypeId::BIGINT:
		TemplatedFillSelectionVectorBuild<int64_t>(source, sel_vec, seq_sel_vec, count);
		break;
	case LogicalTypeId::UTINYINT:
		TemplatedFillSelectionVectorBuild<uint8_t>(source, sel_vec, seq_sel_vec, count);
		break;
	case LogicalTypeId::USMALLINT:
		TemplatedFillSelectionVectorBuild<uint16_t>(source, sel_vec, seq_sel_vec, count);
		break;
	case LogicalTypeId::UINTEGER:
		TemplatedFillSelectionVectorBuild<uint32_t>(source, sel_vec, seq_sel_vec, count);
		break;
	case LogicalTypeId::UBIGINT:
		TemplatedFillSelectionVectorBuild<uint64_t>(source, sel_vec, seq_sel_vec, count);
		break;
	default:
		throw NotImplementedException("Type not supported");
		break;
	}
}

template <typename T>
void PerfectHashJoinExecutor::TemplatedFillSelectionVectorBuild(Vector &source, SelectionVector &sel_vec,
                                                                SelectionVector &seq_sel_vec, idx_t count) {
	auto min_value = pjoin_stats.build_min.GetValue<T>();
	auto max_value = pjoin_stats.build_max.GetValue<T>();
	VectorData vector_data;
	source.Orrify(count, vector_data);
	auto data = reinterpret_cast<T *>(vector_data.data);
	// generate the selection vector
	for (idx_t i = 0, sel_idx = 0; i != count; ++i) {
		auto data_idx = vector_data.sel->get_index(i);
		auto input_value = data[data_idx];
		// add index to selection vector if value in the range
		if (min_value <= input_value && input_value <= max_value) {
			auto idx = (idx_t)(input_value - min_value); // subtract min value to get the idx position
			sel_vec.set_index(sel_idx++, idx);
			if (bitmap_build_idx[idx]) {
				has_duplicates = true;
				break;
			} else {
				bitmap_build_idx[idx] = true;
				unique_keys++;
			}
		}
		seq_sel_vec.set_index(i, i);
	}
}

bool PerfectHashJoinExecutor::ProbePerfectHashTable(ExecutionContext &context, DataChunk &result,
                                                    PhysicalHashJoinState *physical_state, JoinHashTable *ht_ptr,
                                                    PhysicalOperator *operator_child) {
	do {
		// fetch the chunk to join
		operator_child->GetChunk(context, physical_state->child_chunk, physical_state->child_state.get());
		if (physical_state->child_chunk.size() == 0) {
			// no more keys to probe
			return true;
		}
		// fetch the join keys from the chunk
		physical_state->probe_executor.Execute(physical_state->child_chunk, physical_state->join_keys);
		// select the keys that are in the min-max range
		auto &keys_vec = physical_state->join_keys.data[0];
		auto keys_count = physical_state->join_keys.size();
		// todo: add check for fast pass when probe is part of build domain
		FillSelectionVectorSwitchProbe(keys_vec, physical_state->build_sel_vec, physical_state->probe_sel_vec,
		                               keys_count);
		// If build is dense and probe is in build's domain, just reference probe
		if (pjoin_stats.is_build_dense && keys_count == probe_sel_count) {
			result.Reference(physical_state->child_chunk);
		} else {
			// otherwise, filter it out the values that do not match
			result.Slice(physical_state->child_chunk, physical_state->probe_sel_vec, probe_sel_count, 0);
		}
		// on the build side, we need to fetch the data and build dictionary vectors with the sel_vec
		for (idx_t i = 0; i < ht_ptr->build_types.size(); i++) {
			auto &result_vector = result.data[physical_state->child_chunk.ColumnCount() + i];
			D_ASSERT(result_vector.GetType() == ht_ptr->build_types[i]);
			auto &build_vec = perfect_hash_table[i];
			result_vector.Reference(build_vec); //
			result_vector.Slice(physical_state->build_sel_vec, probe_sel_count);
		}
		probe_sel_count = 0;
	} while (result.size() == 0);
	return true;
}

void PerfectHashJoinExecutor::FillSelectionVectorSwitchProbe(Vector &source, SelectionVector &build_sel_vec,
                                                             SelectionVector &probe_sel_vec, idx_t count) {
	switch (source.GetType().id()) {
	case LogicalTypeId::TINYINT:
		TemplatedFillSelectionVectorProbe<int8_t>(source, build_sel_vec, probe_sel_vec, count);
		break;
	case LogicalTypeId::SMALLINT:
		TemplatedFillSelectionVectorProbe<int16_t>(source, build_sel_vec, probe_sel_vec, count);
		break;
	case LogicalTypeId::INTEGER:
		TemplatedFillSelectionVectorProbe<int32_t>(source, build_sel_vec, probe_sel_vec, count);
		break;
	case LogicalTypeId::BIGINT:
		TemplatedFillSelectionVectorProbe<int64_t>(source, build_sel_vec, probe_sel_vec, count);
		break;
	case LogicalTypeId::UTINYINT:
		TemplatedFillSelectionVectorProbe<uint8_t>(source, build_sel_vec, probe_sel_vec, count);
		break;
	case LogicalTypeId::USMALLINT:
		TemplatedFillSelectionVectorProbe<uint16_t>(source, build_sel_vec, probe_sel_vec, count);
		break;
	case LogicalTypeId::UINTEGER:
		TemplatedFillSelectionVectorProbe<uint32_t>(source, build_sel_vec, probe_sel_vec, count);
		break;
	case LogicalTypeId::UBIGINT:
		TemplatedFillSelectionVectorProbe<uint64_t>(source, build_sel_vec, probe_sel_vec, count);
		break;
	default:
		throw NotImplementedException("Type not supported");
		break;
	}
}

template <typename T>
void PerfectHashJoinExecutor::TemplatedFillSelectionVectorProbe(Vector &source, SelectionVector &build_sel_vec,
                                                                SelectionVector &probe_sel_vec, idx_t count) {
	auto min_value = pjoin_stats.build_min.GetValue<T>();
	auto max_value = pjoin_stats.build_max.GetValue<T>();
	VectorData vector_data;
	source.Orrify(count, vector_data);
	auto data = reinterpret_cast<T *>(vector_data.data);

	// build selection vector for non-dense build
	for (idx_t i = 0, sel_idx = 0; i != count; ++i) {
		// retrieve value from vector
		auto data_idx = vector_data.sel->get_index(i);
		auto input_value = data[data_idx];
		// add index to selection vector if value in the range
		if (min_value <= input_value && input_value <= max_value) {
			auto idx = (idx_t)(input_value - min_value); // subtract min value to get the idx position
			                                             // check for matches in the build
			if (bitmap_build_idx[idx]) {
				build_sel_vec.set_index(sel_idx, idx);
				probe_sel_vec.set_index(sel_idx++, i);
				probe_sel_count++;
			}
		}
	}
}

} // namespace duckdb
