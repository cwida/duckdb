//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/alp/alp_analyze.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/compression/alp/algorithm/alp.hpp"
#include "duckdb/storage/compression/alp/alp_constants.hpp"
#include "duckdb/storage/compression/alp/alp_utils.hpp"
#include "duckdb/storage/compression/patas/patas.hpp"
#include "duckdb/storage/table/column_data.hpp"

namespace duckdb {

template <class T>
struct AlpAnalyzeState : public AnalyzeState {
public:
	using EXACT_TYPE = typename FloatingToExact<T>::TYPE;

	AlpAnalyzeState(BlockManager &block_manager, bool has_fallback)
	    : AnalyzeState(block_manager), has_fallback(has_fallback) {
	}

	//! Flag indicating support for uncompressed storage in case of many exceptions.
	const bool has_fallback;
	idx_t vectors_sampled_count = 0;
	idx_t total_values_count = 0;
	idx_t vectors_count = 0;
	vector<vector<T>> rowgroup_sample;
	vector<vector<T>> complete_vectors_sampled;
	alp::AlpCompressionData<T, true> compression_data;
};

template <class T>
unique_ptr<AnalyzeState> AlpInitAnalyze(ColumnData &col_data, PhysicalType type) {
	auto &block_manager = col_data.GetBlockManager();
	const auto storage_version = col_data.GetStorageManager().GetStorageVersion();
	const bool has_fallback = !StorageManager::IsPriorToVersion(StorageVersion::V1_5_0, storage_version);

	if (!has_fallback &&
	    block_manager.GetBlockSize() + block_manager.GetBlockHeaderSize() < AlpConstants::MIN_BLOCK_SIZE) {
		return nullptr;
	}

	return make_uniq<AlpAnalyzeState<T>>(block_manager, has_fallback);
}

/*
 * ALP Analyze step only pushes the needed samples to estimate the compression size in the finalize step
 */
template <class T>
bool AlpAnalyze(AnalyzeState &state, const Vector &input) {
	auto &analyze_state = state.Cast<AlpAnalyzeState<T>>();

	const auto count = input.size();
	bool must_skip_current_vector = alp::AlpUtils::MustSkipSamplingFromCurrentVector(
	    analyze_state.vectors_count, analyze_state.vectors_sampled_count, count);
	analyze_state.vectors_count += 1;
	analyze_state.total_values_count += count;
	if (must_skip_current_vector) {
		return true;
	}

	UnifiedVectorFormat vdata;
	input.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<T>(vdata);

	alp::AlpSamplingParameters sampling_params = alp::AlpUtils::GetSamplingParameters(count);

	vector<uint16_t> current_vector_null_positions(sampling_params.n_lookup_values, 0);
	vector<T> current_vector_values(sampling_params.n_lookup_values, 0);
	vector<T> current_vector_sample(sampling_params.n_sampled_values, 0);

	// Storing the entire sampled vector
	//! We need to store the entire sampled vector to perform the 'analyze' compression in it
	idx_t nulls_idx = 0;
	// We optimize by doing a different loop when there are no nulls
	if (vdata.validity.CannotHaveNull()) {
		for (idx_t i = 0; i < sampling_params.n_lookup_values; i++) {
			auto idx = vdata.sel->get_index(i);
			T value = data[idx];
			current_vector_values[i] = value;
		}
	} else {
		for (idx_t i = 0; i < sampling_params.n_lookup_values; i++) {
			auto idx = vdata.sel->get_index(i);
			T value = data[idx];
			//! We resolve null values with a predicated comparison
			bool is_null = !vdata.validity.RowIsValid(idx);
			current_vector_null_positions[nulls_idx] = UnsafeNumericCast<uint16_t>(i);
			nulls_idx += is_null;
			current_vector_values[i] = value;
		}
		alp::AlpUtils::FindAndReplaceNullsInVector<T>(current_vector_values.data(),
		                                              current_vector_null_positions.data(),
		                                              sampling_params.n_lookup_values, nulls_idx);
	}

	// Storing the sample of that vector
	idx_t sample_idx = 0;
	for (idx_t i = 0; i < sampling_params.n_lookup_values; i += sampling_params.n_sampled_increments) {
		current_vector_sample[sample_idx] = current_vector_values[i];
		sample_idx++;
	}
	D_ASSERT(sample_idx == sampling_params.n_sampled_values);

	//! A std::move is needed to avoid a copy of the pushed vector
	analyze_state.complete_vectors_sampled.push_back(std::move(current_vector_values));
	analyze_state.rowgroup_sample.push_back(std::move(current_vector_sample));
	analyze_state.vectors_sampled_count++;
	return true;
}

/*
 * Estimate the compression size of ALP using the taken samples
 */
template <class T>
idx_t AlpFinalAnalyze(AnalyzeState &state) {
	auto &analyze_state = state.Cast<AlpAnalyzeState<T>>();
	auto &compression_data = analyze_state.compression_data;

	// Find the top combinations of exponent and factor.
	alp::AlpCompression<T, true>::FindTopKCombinations(analyze_state.rowgroup_sample, compression_data);

	// Encode the sampled vectors to estimate their storage size.
	double sampled_values = 0;
	double sampled_bytes = 0;
	for (auto &sample : analyze_state.complete_vectors_sampled) {
		alp::AlpCompression<T, true>::Compress(sample.data(), sample.size(), compression_data);
		auto vector_size = compression_data.RequiredSpace();
		if (analyze_state.has_fallback) {
			const idx_t uncompressed_size = AlpConstants::EXPONENT_SIZE + sizeof(T) * sample.size();
			vector_size = MinValue(vector_size, uncompressed_size);
		}

		sampled_bytes += vector_size;
		sampled_values += sample.size();
		compression_data.Reset();
	}

	if (sampled_values == 0) {
		return DConstants::INVALID_INDEX;
	}

	// Extrapolate sampled set to total count
	const auto total_values = static_cast<double>(analyze_state.total_values_count);
	const auto sampling_factor = total_values / sampled_values;
	auto estimated_size = sampled_bytes * sampling_factor;

	const auto vector_count = std::ceil(total_values / AlpConstants::ALP_VECTOR_SIZE);
	estimated_size += vector_count * AlpConstants::METADATA_POINTER_SIZE;

	const auto usable_segment_size = static_cast<double>(state.info.GetBlockSize() - AlpConstants::HEADER_SIZE);
	const auto segment_count = std::ceil(estimated_size / usable_segment_size);
	estimated_size = std::ceil(estimated_size + segment_count * AlpConstants::HEADER_SIZE);
	if (estimated_size >= static_cast<double>(DConstants::INVALID_INDEX)) {
		return DConstants::INVALID_INDEX;
	}
	return AlignValue(LossyNumericCast<idx_t>(estimated_size));
}

} // namespace duckdb
