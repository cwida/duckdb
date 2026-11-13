#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/execution/reservoir_sample.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include <unordered_set>

namespace duckdb {

std::pair<double, idx_t> BlockingSample::PopFromWeightQueue() {
	D_ASSERT(base_reservoir_sample && !base_reservoir_sample->reservoir_weights.empty());
	auto ret = base_reservoir_sample->reservoir_weights.top();
	base_reservoir_sample->reservoir_weights.pop();

	base_reservoir_sample->UpdateMinWeightThreshold();
	D_ASSERT(base_reservoir_sample->min_weight_threshold > 0);
	return ret;
}

double BlockingSample::GetMinWeightThreshold() {
	return base_reservoir_sample->min_weight_threshold;
}

idx_t BlockingSample::GetPriorityQueueSize() {
	return base_reservoir_sample->reservoir_weights.size();
}

void BlockingSample::Destroy() {
	destroyed = true;
}

void ReservoirChunk::Serialize(Serializer &serializer) const {
	chunk.Serialize(serializer);
}

unique_ptr<ReservoirChunk> ReservoirChunk::Deserialize(Deserializer &deserializer) {
	auto result = make_uniq<ReservoirChunk>();
	result->chunk.Deserialize(deserializer);
	return result;
}

unique_ptr<ReservoirChunk> ReservoirChunk::Copy() const {
	auto copy = make_uniq<ReservoirChunk>();
	copy->chunk.Initialize(Allocator::DefaultAllocator(), chunk.GetTypes());

	chunk.Copy(copy->chunk);
	return copy;
}

ReservoirSample::ReservoirSample(idx_t sample_count, unique_ptr<ReservoirChunk> reservoir_chunk)
    : ReservoirSample(Allocator::DefaultAllocator(), sample_count, 1) {
	if (reservoir_chunk) {
		this->reservoir_chunk = std::move(reservoir_chunk);
		sel_size = this->reservoir_chunk->chunk.size();
		sel = SelectionVector(FIXED_SAMPLE_SIZE);
		for (idx_t i = 0; i < sel_size; i++) {
			sel.set_index(i, i);
		}
		ExpandSerializedSample();
	}
	stats_sample = true;
}

ReservoirSample::ReservoirSample(Allocator &allocator, idx_t sample_count, int64_t seed)
    : BlockingSample(seed), sample_count(sample_count), allocator(allocator) {
	base_reservoir_sample = make_uniq<BaseReservoirSampling>(seed);
	type = SampleType::RESERVOIR_SAMPLE;
	reservoir_chunk = nullptr;
	stats_sample = false;
	sel = SelectionVector(sample_count);
	sel_size = 0;
}

idx_t ReservoirSample::GetSampleCount() {
	return sample_count;
}

idx_t ReservoirSample::NumSamplesCollected() const {
	if (!reservoir_chunk) {
		return 0;
	}
	return reservoir_chunk->chunk.size();
}

SamplingState ReservoirSample::GetSamplingState() const {
	if (base_reservoir_sample->reservoir_weights.empty()) {
		return SamplingState::RANDOM;
	}
	return SamplingState::RESERVOIR;
}

idx_t ReservoirSample::GetActiveSampleCount() const {
	switch (GetSamplingState()) {
	case SamplingState::RANDOM:
		return sel_size;
	case SamplingState::RESERVOIR:
		return base_reservoir_sample->reservoir_weights.size();
	default:
		throw InternalException("Sampling State is INVALID");
	}
}

idx_t ReservoirSample::GetTuplesSeen() const {
	return base_reservoir_sample->num_entries_seen_total;
}

DataChunk &ReservoirSample::Chunk() {
	D_ASSERT(reservoir_chunk);
	return reservoir_chunk->chunk;
}

unique_ptr<DataChunk> ReservoirSample::GetChunk() {
	if (destroyed || !reservoir_chunk || Chunk().size() == 0) {
		return nullptr;
	}
	// cannot destroy internal samples.
	auto ret = make_uniq<DataChunk>();

	SelectionVector ret_sel(STANDARD_VECTOR_SIZE);
	idx_t collected_samples = GetActiveSampleCount();

	if (collected_samples == 0) {
		return nullptr;
	}

	idx_t samples_remaining;
	idx_t return_chunk_size;
	if (collected_samples > STANDARD_VECTOR_SIZE) {
		samples_remaining = collected_samples - STANDARD_VECTOR_SIZE;
		return_chunk_size = STANDARD_VECTOR_SIZE;
	} else {
		samples_remaining = 0;
		return_chunk_size = collected_samples;
	}

	for (idx_t i = samples_remaining; i < collected_samples; i++) {
		// pop samples and reduce size of selection vector.
		if (GetSamplingState() == SamplingState::RESERVOIR) {
			auto top = PopFromWeightQueue();
			ret_sel.set_index(i - samples_remaining, sel.get_index(top.second));
		} else {
			ret_sel.set_index(i - samples_remaining, sel.get_index(i));
		}
		sel_size -= 1;
	}

	auto reservoir_types = Chunk().GetTypes();

	ret->Initialize(allocator, reservoir_types, STANDARD_VECTOR_SIZE);
	ret->Slice(Chunk(), ret_sel, return_chunk_size);
	return ret;
}

unique_ptr<ReservoirChunk> ReservoirSample::CreateNewSampleChunk(vector<LogicalType> &types, idx_t size) const {
	auto new_sample_chunk = make_uniq<ReservoirChunk>();
	new_sample_chunk->chunk.Initialize(allocator, types, size);

	// set the NULL columns correctly
	for (idx_t col_idx = 0; col_idx < types.size(); col_idx++) {
		if (!ValidSampleType(types[col_idx]) && stats_sample) {
			ConstantVector::SetNull(new_sample_chunk->chunk.data[col_idx], count_t(size));
		}
	}
	return new_sample_chunk;
}

void ReservoirSample::Vacuum() {
	Verify();
	bool do_vacuum = false;
	// when it's not a stats sample, sometimes we neverr collect more than FIXED_SAMPLE_SIZE tuples
	// but we still need to vacuum, so the rules are a little bit different.
	if (!stats_sample && GetActiveSampleCount() <= static_cast<idx_t>(GetReservoirChunkCapacity<double>() * 0.8)) {
		do_vacuum = true;
	}
	if (!do_vacuum && (NumSamplesCollected() <= FIXED_SAMPLE_SIZE || !reservoir_chunk || destroyed)) {
		// sample is destroyed or too small to shrink
		// sample does not need to be vacuumed.
		return;
	}

	auto ret = Copy();
	auto ret_reservoir = duckdb::unique_ptr_cast<BlockingSample, ReservoirSample>(std::move(ret));
	reservoir_chunk = std::move(ret_reservoir->reservoir_chunk);
	sel = std::move(ret_reservoir->sel);
	sel_size = ret_reservoir->sel_size;

	Verify();
	// We should only have one sample chunk now.
	D_ASSERT(Chunk().size() > 0 && Chunk().size() <= sample_count);
}

unique_ptr<BlockingSample> ReservoirSample::Copy() const {
	auto ret = make_uniq<ReservoirSample>(allocator, sample_count);
	ret->stats_sample = stats_sample;

	ret->base_reservoir_sample = base_reservoir_sample->Copy();
	ret->destroyed = destroyed;

	if (!reservoir_chunk || destroyed) {
		return unique_ptr_cast<ReservoirSample, BlockingSample>(std::move(ret));
	}

	D_ASSERT(reservoir_chunk);

	// create a new sample chunk to store new samples
	auto types = reservoir_chunk->chunk.GetTypes();
	// how many values should be copied
	idx_t values_to_copy = MinValue<idx_t>(GetActiveSampleCount(), sample_count);

	auto new_sample_chunk = CreateNewSampleChunk(types, GetReservoirChunkCapacity<idx_t>());

	SelectionVector sel_copy(sel);

	ret->reservoir_chunk = std::move(new_sample_chunk);
	ret->UpdateSampleAppend(ret->reservoir_chunk->chunk, reservoir_chunk->chunk, sel_copy, values_to_copy);
	ret->sel = SelectionVector(values_to_copy);
	for (idx_t i = 0; i < values_to_copy; i++) {
		ret->sel.set_index(i, i);
	}
	ret->sel_size = sel_size;
	D_ASSERT(ret->reservoir_chunk->chunk.size() <= sample_count);
	ret->Verify();
	return unique_ptr_cast<ReservoirSample, BlockingSample>(std::move(ret));
}

void ReservoirSample::ConvertToReservoirSample() {
	D_ASSERT(sel_size <= sample_count);
	base_reservoir_sample->FillWeights(sel, sel_size);
}

vector<uint32_t> ReservoirSample::GetRandomizedVector(uint32_t range, uint32_t size) const {
	vector<uint32_t> ret;
	ret.reserve(range);
	for (uint32_t i = 0; i < range; i++) {
		ret.push_back(i);
	}
	for (uint32_t i = 0; i < size; i++) {
		uint32_t random_shuffle = base_reservoir_sample->random.NextRandomInteger32(i, range);
		if (random_shuffle == i) {
			// leave the value where it is
			continue;
		}
		uint32_t tmp = ret[random_shuffle];
		// basically replacing the tuple that was at index actual_sample_indexes[random_shuffle]
		ret[random_shuffle] = ret[i];
		ret[i] = tmp;
	}
	return ret;
}

void ReservoirSample::SimpleMerge(ReservoirSample &other) {
	D_ASSERT(GetPriorityQueueSize() == 0);
	D_ASSERT(other.GetPriorityQueueSize() == 0);
	D_ASSERT(GetSamplingState() == SamplingState::RANDOM);
	D_ASSERT(other.GetSamplingState() == SamplingState::RANDOM);

	if (other.GetActiveSampleCount() == 0 && other.GetTuplesSeen() == 0) {
		return;
	}

	if (GetActiveSampleCount() == 0 && GetTuplesSeen() == 0) {
		sel = SelectionVector(other.sel);
		sel_size = other.sel_size;
		base_reservoir_sample->num_entries_seen_total = other.GetTuplesSeen();
		return;
	}

	idx_t total_seen = GetTuplesSeen() + other.GetTuplesSeen();

	auto weight_tuples_this = static_cast<double>(GetTuplesSeen()) / static_cast<double>(total_seen);
	auto weight_tuples_other = static_cast<double>(other.GetTuplesSeen()) / static_cast<double>(total_seen);

	// If weights don't add up to 1, most likely a simple merge occurred and no new samples were added.
	// if that is the case, add the missing weight to the lower weighted sample to adjust.
	// this is to avoid cases where if you have a 20k row table and add another 20k rows row by row
	// then eventually the missing weights will add up, and get you a more even distribution
	if (weight_tuples_this + weight_tuples_other < 1) {
		weight_tuples_other += 1 - (weight_tuples_other + weight_tuples_this);
	}

	idx_t keep_from_this = 0;
	idx_t keep_from_other = 0;
	D_ASSERT(stats_sample);
	D_ASSERT(sample_count == FIXED_SAMPLE_SIZE);
	D_ASSERT(sample_count == other.sample_count);
	auto sample_count_double = static_cast<double>(sample_count);

	if (weight_tuples_this > weight_tuples_other) {
		keep_from_this = MinValue<idx_t>(static_cast<idx_t>(round(sample_count_double * weight_tuples_this)),
		                                 GetActiveSampleCount());
		keep_from_other = MinValue<idx_t>(sample_count - keep_from_this, other.GetActiveSampleCount());
	} else {
		keep_from_other = MinValue<idx_t>(static_cast<idx_t>(round(sample_count_double * weight_tuples_other)),
		                                  other.GetActiveSampleCount());
		keep_from_this = MinValue<idx_t>(sample_count - keep_from_other, GetActiveSampleCount());
	}

	D_ASSERT(keep_from_this <= GetActiveSampleCount());
	D_ASSERT(keep_from_other <= other.GetActiveSampleCount());
	D_ASSERT(keep_from_other + keep_from_this <= FIXED_SAMPLE_SIZE);
	idx_t size_after_merge = MinValue<idx_t>(keep_from_other + keep_from_this, FIXED_SAMPLE_SIZE);

	// Check if appending the other samples to this will go over the sample chunk size
	if (reservoir_chunk->chunk.size() + keep_from_other > GetReservoirChunkCapacity<idx_t>()) {
		Vacuum();
	}

	D_ASSERT(size_after_merge <= other.GetActiveSampleCount() + GetActiveSampleCount());
	SelectionVector chunk_sel(keep_from_other);
	auto offset = reservoir_chunk->chunk.size();
	for (idx_t i = keep_from_this; i < size_after_merge; i++) {
		if (i >= GetActiveSampleCount()) {
			D_ASSERT(sel_size >= GetActiveSampleCount());
			sel.set_index(GetActiveSampleCount(), offset);
			sel_size += 1;
		} else {
			sel.set_index(i, offset);
		}
		chunk_sel.set_index(i - keep_from_this, other.sel.get_index(i - keep_from_this));
		offset += 1;
	}

	D_ASSERT(GetActiveSampleCount() == size_after_merge);

	// Copy the rows that make it to the sample from other and put them into this.
	UpdateSampleAppend(reservoir_chunk->chunk, other.reservoir_chunk->chunk, chunk_sel, keep_from_other);
	base_reservoir_sample->num_entries_seen_total += other.GetTuplesSeen();

	// if THIS has too many samples now, we convert it to a slower sample.
	if (GetTuplesSeen() >= FIXED_SAMPLE_SIZE * FAST_TO_SLOW_THRESHOLD) {
		ConvertToReservoirSample();
	}
	Verify();
}

void ReservoirSample::WeightedMerge(ReservoirSample &other_sample) {
	D_ASSERT(GetSamplingState() == SamplingState::RESERVOIR);
	D_ASSERT(other_sample.GetSamplingState() == SamplingState::RESERVOIR);

	// Find out how many samples we want to keep.
	idx_t total_samples = GetActiveSampleCount() + other_sample.GetActiveSampleCount();
	idx_t total_samples_seen =
	    base_reservoir_sample->num_entries_seen_total + other_sample.base_reservoir_sample->num_entries_seen_total;
	idx_t num_samples_to_keep = MinValue<idx_t>(total_samples, MinValue<idx_t>(sample_count, total_samples_seen));

	D_ASSERT(GetActiveSampleCount() <= num_samples_to_keep);
	D_ASSERT(total_samples <= FIXED_SAMPLE_SIZE * 2);

	// pop from base base_reservoir weights until there are num_samples_to_keep left.
	vector<idx_t> this_indexes_to_replace;
	for (idx_t i = num_samples_to_keep; i < total_samples; i++) {
		auto min_weight_this = base_reservoir_sample->min_weight_threshold;
		auto min_weight_other = other_sample.base_reservoir_sample->min_weight_threshold;
		// min weight threshold is always positive
		if (min_weight_this > min_weight_other) {
			// pop from other
			other_sample.base_reservoir_sample->reservoir_weights.pop();
			other_sample.base_reservoir_sample->UpdateMinWeightThreshold();
		} else {
			auto top_this = PopFromWeightQueue();
			this_indexes_to_replace.push_back(top_this.second);
			base_reservoir_sample->UpdateMinWeightThreshold();
		}
	}

	D_ASSERT(other_sample.GetPriorityQueueSize() + GetPriorityQueueSize() <= FIXED_SAMPLE_SIZE);
	D_ASSERT(other_sample.GetPriorityQueueSize() + GetPriorityQueueSize() == num_samples_to_keep);
	D_ASSERT(other_sample.reservoir_chunk->chunk.GetTypes() == reservoir_chunk->chunk.GetTypes());

	// Prepare a selection vector to copy data from the other sample chunk to this sample chunk
	SelectionVector sel_other(other_sample.GetPriorityQueueSize());
	D_ASSERT(GetPriorityQueueSize() <= num_samples_to_keep);
	D_ASSERT(other_sample.GetPriorityQueueSize() >= this_indexes_to_replace.size());
	idx_t chunk_offset = 0;

	// Now push weights from other.base_reservoir_sample to this
	// Depending on how many sample values "this" has, we either need to add to the selection vector
	// Or replace values in "this'" selection vector
	idx_t i = 0;
	while (other_sample.GetPriorityQueueSize() > 0) {
		auto other_top = other_sample.PopFromWeightQueue();
		idx_t index_for_new_pair = chunk_offset + reservoir_chunk->chunk.size();

		// update the sel used to copy values from other to this
		sel_other.set_index(chunk_offset, other_top.second);
		if (i < this_indexes_to_replace.size()) {
			auto replacement_index = this_indexes_to_replace[i];
			sel.set_index(replacement_index, index_for_new_pair);
			other_top.second = replacement_index;
		} else {
			sel.set_index(sel_size, index_for_new_pair);
			other_top.second = sel_size;
			sel_size += 1;
		}

		// make sure that the sample indexes are (this.sample_chunk.size() + chunk_offset)
		base_reservoir_sample->reservoir_weights.push(other_top);
		chunk_offset += 1;
		i += 1;
	}

	D_ASSERT(GetPriorityQueueSize() == num_samples_to_keep);

	base_reservoir_sample->UpdateMinWeightThreshold();
	D_ASSERT(base_reservoir_sample->min_weight_threshold > 0);
	base_reservoir_sample->num_entries_seen_total = GetTuplesSeen() + other_sample.GetTuplesSeen();

	UpdateSampleAppend(reservoir_chunk->chunk, other_sample.reservoir_chunk->chunk, sel_other, chunk_offset);
	if (reservoir_chunk->chunk.size() > FIXED_SAMPLE_SIZE * (FIXED_SAMPLE_SIZE_MULTIPLIER - 3)) {
		Vacuum();
	}

	Verify();
}

void ReservoirSample::Merge(unique_ptr<BlockingSample> other) {
	if (destroyed || other->destroyed) {
		Destroy();
		return;
	}

	D_ASSERT(other->type == SampleType::RESERVOIR_SAMPLE);
	auto &other_sample = other->Cast<ReservoirSample>();

	// if the other sample has not collected anything yet return
	if (!other_sample.reservoir_chunk || other_sample.reservoir_chunk->chunk.size() == 0) {
		return;
	}

	// this has not collected samples, take over the other
	if (!reservoir_chunk || reservoir_chunk->chunk.size() == 0) {
		base_reservoir_sample = std::move(other->base_reservoir_sample);
		reservoir_chunk = std::move(other_sample.reservoir_chunk);
		sel = SelectionVector(other_sample.sel);
		sel_size = other_sample.sel_size;
		Verify();
		return;
	}
	//! Both samples are still in "fast sampling" method
	if (GetSamplingState() == SamplingState::RANDOM && other_sample.GetSamplingState() == SamplingState::RANDOM) {
		SimpleMerge(other_sample);
		return;
	}

	// One or none of the samples are in "Fast Sampling" method.
	// When this is the case, switch both to slow sampling
	ConvertToReservoirSample();
	other_sample.ConvertToReservoirSample();
	WeightedMerge(other_sample);
}

void ReservoirSample::ShuffleSel(SelectionVector &sel, idx_t range, idx_t size) const {
	auto randomized = GetRandomizedVector(static_cast<uint32_t>(range), static_cast<uint32_t>(size));
	SelectionVector original_sel(range);
	for (idx_t i = 0; i < range; i++) {
		original_sel.set_index(i, sel.get_index(i));
	}
	for (idx_t i = 0; i < size; i++) {
		sel.set_index(i, original_sel.get_index(randomized[i]));
	}
}

void ReservoirSample::NormalizeWeights() {
	vector<std::pair<double, idx_t>> tmp_weights;
	while (!base_reservoir_sample->reservoir_weights.empty()) {
		auto top = base_reservoir_sample->reservoir_weights.top();
		tmp_weights.push_back(std::move(top));
		base_reservoir_sample->reservoir_weights.pop();
	}
	std::sort(tmp_weights.begin(), tmp_weights.end(),
	          [&](std::pair<double, idx_t> a, std::pair<double, idx_t> b) { return a.second < b.second; });
	for (idx_t i = 0; i < tmp_weights.size(); i++) {
		base_reservoir_sample->reservoir_weights.emplace(tmp_weights.at(i).first, i);
	}
	base_reservoir_sample->SetNextEntry();
}

void ReservoirSample::EvictOverBudgetSamples() {
	Verify();
	if (!reservoir_chunk || destroyed) {
		return;
	}

	// since this is for serialization, we really need to make sure keep a
	// minimum of 1% of the rows or 2048 rows
	idx_t num_samples_to_keep =
	    MinValue<idx_t>(FIXED_SAMPLE_SIZE, static_cast<idx_t>(SAVE_PERCENTAGE * static_cast<double>(GetTuplesSeen())));

	if (num_samples_to_keep <= 0) {
		reservoir_chunk->chunk.SetChildCardinality(0);
		return;
	}

	if (num_samples_to_keep == sample_count) {
		return;
	}

	// if we over sampled, make sure we only keep the highest percentage samples
	std::unordered_set<idx_t> selections_to_delete;

	while (num_samples_to_keep < GetPriorityQueueSize()) {
		auto top = PopFromWeightQueue();
		D_ASSERT(top.second < sel_size);
		selections_to_delete.emplace(top.second);
	}

	// set up reservoir chunk for the reservoir sample
	D_ASSERT(reservoir_chunk->chunk.size() <= sample_count);
	// create a new sample chunk to store new samples
	auto types = reservoir_chunk->chunk.GetTypes();
	D_ASSERT(num_samples_to_keep <= sample_count);
	D_ASSERT(stats_sample);
	D_ASSERT(sample_count == FIXED_SAMPLE_SIZE);
	auto new_reservoir_chunk = CreateNewSampleChunk(types, sample_count);

	// The current selection vector can potentially have 2048 valid mappings.
	// If we need to save a sample with less rows than that, we need to do the following
	// 1. Create a new selection vector that doesn't point to the rows we are evicting
	SelectionVector new_sel(num_samples_to_keep);
	idx_t offset = 0;
	for (idx_t i = 0; i < num_samples_to_keep + selections_to_delete.size(); i++) {
		if (selections_to_delete.find(i) == selections_to_delete.end()) {
			D_ASSERT(i - offset < num_samples_to_keep);
			new_sel.set_index(i - offset, sel.get_index(i));
		} else {
			offset += 1;
		}
	}
	// 2. Update row_ids in our weights so that they don't store rows ids to
	//    indexes in the selection vector that have been evicted.
	if (!selections_to_delete.empty()) {
		NormalizeWeights();
	}

	D_ASSERT(reservoir_chunk->chunk.GetTypes() == new_reservoir_chunk->chunk.GetTypes());

	UpdateSampleAppend(new_reservoir_chunk->chunk, reservoir_chunk->chunk, new_sel, num_samples_to_keep);
	// set the cardinality
	reservoir_chunk = std::move(new_reservoir_chunk);
	sel_size = num_samples_to_keep;
	base_reservoir_sample->UpdateMinWeightThreshold();
}

void ReservoirSample::ExpandSerializedSample() {
	if (!reservoir_chunk) {
		return;
	}

	auto types = reservoir_chunk->chunk.GetTypes();
	auto new_res_chunk = CreateNewSampleChunk(types, GetReservoirChunkCapacity<idx_t>());
	auto copy_count = reservoir_chunk->chunk.size();
	SelectionVector tmp_sel = SelectionVector(static_cast<idx_t>(0), copy_count);
	UpdateSampleAppend(new_res_chunk->chunk, reservoir_chunk->chunk, tmp_sel, copy_count);
	std::swap(reservoir_chunk, new_res_chunk);
}

template <typename T>
T ReservoirSample::GetReservoirChunkCapacity() const {
	return static_cast<T>(sample_count +
	                      (FIXED_SAMPLE_SIZE_MULTIPLIER * MinValue<idx_t>(sample_count, FIXED_SAMPLE_SIZE)));
}

idx_t ReservoirSample::FillReservoir(DataChunk &chunk) {
	idx_t ingested_count = 0;
	if (!reservoir_chunk) {
		if (chunk.size() > FIXED_SAMPLE_SIZE) {
			throw InternalException("Creating sample with DataChunk that is larger than the fixed sample size");
		}
		auto types = chunk.GetTypes();
		// create a new sample chunk to store new samples
		reservoir_chunk = CreateNewSampleChunk(types, GetReservoirChunkCapacity<idx_t>());
	}

	idx_t actual_sample_index_start = GetActiveSampleCount();
	D_ASSERT(reservoir_chunk->chunk.ColumnCount() == chunk.ColumnCount());

	if (reservoir_chunk->chunk.size() < sample_count) {
		ingested_count = MinValue<idx_t>(sample_count - reservoir_chunk->chunk.size(), chunk.size());
		auto random_other_sel =
		    GetRandomizedVector(static_cast<uint32_t>(ingested_count), static_cast<uint32_t>(ingested_count));
		SelectionVector sel_for_input_chunk(ingested_count);
		for (idx_t i = 0; i < ingested_count; i++) {
			sel.set_index(actual_sample_index_start + i, actual_sample_index_start + i);
			sel_for_input_chunk.set_index(i, random_other_sel[i]);
		}
		UpdateSampleAppend(reservoir_chunk->chunk, chunk, sel_for_input_chunk, ingested_count);
		sel_size += ingested_count;
	}
	D_ASSERT(GetActiveSampleCount() <= sample_count);
	D_ASSERT(GetActiveSampleCount() >= ingested_count);
	// always return how many tuples were ingested
	return ingested_count;
}

void ReservoirSample::Destroy() {
	destroyed = true;
}

SelectionVectorHelper ReservoirSample::GetReplacementIndexes(idx_t sample_chunk_offset,
                                                             idx_t theoretical_chunk_length) {
	if (GetSamplingState() == SamplingState::RANDOM) {
		return GetReplacementIndexesFast(sample_chunk_offset, theoretical_chunk_length);
	}
	return GetReplacementIndexesSlow(sample_chunk_offset, theoretical_chunk_length);
}

SelectionVectorHelper ReservoirSample::GetReplacementIndexesFast(idx_t sample_chunk_offset, idx_t chunk_length) {
	// how much weight to the other tuples have compared to the ones in this chunk?
	auto weight_tuples_other = static_cast<double>(chunk_length) / static_cast<double>(GetTuplesSeen() + chunk_length);
	auto num_to_pop = static_cast<uint32_t>(round(weight_tuples_other * static_cast<double>(sample_count)));
	D_ASSERT(num_to_pop <= sample_count);
	D_ASSERT(num_to_pop <= sel_size);
	SelectionVectorHelper ret;

	if (num_to_pop == 0) {
		ret.sel = SelectionVector(num_to_pop);
		ret.size = 0;
		return ret;
	}
	std::unordered_map<idx_t, idx_t> replacement_indexes;
	SelectionVector chunk_sel(num_to_pop);

	auto random_indexes_chunk = GetRandomizedVector(static_cast<uint32_t>(chunk_length), num_to_pop);
	auto random_sel_indexes = GetRandomizedVector(static_cast<uint32_t>(sel_size), num_to_pop);
	for (idx_t i = 0; i < num_to_pop; i++) {
		// update the selection vector for the reservoir sample
		chunk_sel.set_index(i, random_indexes_chunk[i]);
		// sel is not guaratneed to be random, so we update the indexes according to our
		// random sel indexes.
		sel.set_index(random_sel_indexes[i], sample_chunk_offset + i);
	}

	D_ASSERT(sel_size == sample_count);

	ret.sel = SelectionVector(chunk_sel);
	ret.size = num_to_pop;
	return ret;
}

SelectionVectorHelper ReservoirSample::GetReplacementIndexesSlow(const idx_t sample_chunk_offset,
                                                                 const idx_t chunk_length) {
	idx_t remaining = chunk_length;
	std::unordered_map<idx_t, idx_t> ret_map;
	idx_t sample_chunk_index = 0;

	idx_t base_offset = 0;

	while (true) {
		idx_t offset =
		    base_reservoir_sample->next_index_to_sample - base_reservoir_sample->num_entries_to_skip_b4_next_sample;
		if (offset >= remaining) {
			// not in this chunk! increment current count and go to the next chunk
			base_reservoir_sample->num_entries_to_skip_b4_next_sample += remaining;
			break;
		}
		// in this chunk! replace the element
		// ret[index_in_new_chunk] = index_in_sample_chunk (the sample chunk offset will be applied later)
		// D_ASSERT(sample_chunk_index == ret.size());
		ret_map[base_offset + offset] = sample_chunk_index;
		double r2 = base_reservoir_sample->random.NextRandom32(base_reservoir_sample->min_weight_threshold, 1);
		// replace element in our max_heap
		// first get the top most pair
		const auto top = PopFromWeightQueue();
		const auto index = top.second;
		const auto index_in_sample_chunk = sample_chunk_offset + sample_chunk_index;
		sel.set_index(index, index_in_sample_chunk);
		base_reservoir_sample->ReplaceElementWithIndex(index, r2, false);

		sample_chunk_index += 1;
		// shift the chunk forward
		remaining -= offset;
		base_offset += offset;
	}

	// create selection vector to return
	SelectionVector ret_sel(ret_map.size());
	D_ASSERT(sel_size == sample_count);
	for (auto &kv : ret_map) {
		ret_sel.set_index(kv.second, kv.first);
	}
	SelectionVectorHelper ret;
	ret.sel = SelectionVector(ret_sel);
	ret.size = static_cast<uint32_t>(ret_map.size());
	return ret;
}

void ReservoirSample::Finalize() {
}

bool ReservoirSample::ValidSampleType(const LogicalType &type) {
	return type.IsNumeric();
}

void ReservoirSample::UpdateSampleAppend(DataChunk &this_, DataChunk &other, SelectionVector &other_sel,
                                         idx_t append_count) const {
	idx_t new_size = this_.size() + append_count;
	if (other.size() == 0) {
		return;
	}
	D_ASSERT(this_.GetTypes() == other.GetTypes());
	auto types = reservoir_chunk->chunk.GetTypes();

	for (idx_t i = 0; i < reservoir_chunk->chunk.ColumnCount(); i++) {
		const auto &col_type = types[i];
		if (ValidSampleType(col_type) || !stats_sample) {
			D_ASSERT(this_.data[i].GetVectorType() == VectorType::FLAT_VECTOR);
			VectorOperations::Copy(other.data[i], this_.data[i], other_sel, append_count, 0, this_.size());
		}
	}
	this_.SetChildCardinality(new_size);
}

void ReservoirSample::AddToReservoir(DataChunk &chunk) {
	if (destroyed || chunk.size() == 0) {
		return;
	}

	if (!reservoir_chunk && GetReservoirChunkCapacity<idx_t>() == 0) {
		return;
	}
	idx_t tuples_consumed = FillReservoir(chunk);
	base_reservoir_sample->num_entries_seen_total += tuples_consumed;
	D_ASSERT(sample_count == 0 || reservoir_chunk->chunk.size() >= 1);

	if (tuples_consumed == chunk.size()) {
		return;
	}

	// the chunk filled the first FIXED_SAMPLE_SIZE chunk but still has tuples remaining
	// slice the chunk and call AddToReservoir again.
	if (tuples_consumed != chunk.size() && tuples_consumed != 0) {
		// Fill reservoir consumed some of the chunk to reach FIXED_SAMPLE_SIZE
		// now we need to
		// So we slice it and call AddToReservoir
		auto slice = make_uniq<DataChunk>();
		auto samples_remaining = chunk.size() - tuples_consumed;
		auto types = chunk.GetTypes();
		SelectionVector input_sel(samples_remaining);
		for (idx_t i = 0; i < samples_remaining; i++) {
			input_sel.set_index(i, tuples_consumed + i);
		}
		slice->Initialize(allocator, types, samples_remaining);
		slice->Slice(chunk, input_sel, samples_remaining);
		AddToReservoir(*slice);
		return;
	}

	// at this point we should have collected at least sample count samples
	D_ASSERT(GetActiveSampleCount() >= sample_count);

	auto chunk_sel = GetReplacementIndexes(reservoir_chunk->chunk.size(), chunk.size());

	if (chunk_sel.size == 0) {
		// not adding any samples
		base_reservoir_sample->num_entries_seen_total += chunk.size();
		return;
	}

	idx_t size = chunk_sel.size;
	D_ASSERT(size <= chunk.size());
	D_ASSERT(reservoir_chunk->chunk.size() < GetReservoirChunkCapacity<idx_t>());

	UpdateSampleAppend(reservoir_chunk->chunk, chunk, chunk_sel.sel, size);

	base_reservoir_sample->num_entries_seen_total += chunk.size();
	D_ASSERT(base_reservoir_sample->reservoir_weights.size() == 0 ||
	         base_reservoir_sample->reservoir_weights.size() == sample_count);

	Verify();

	// if we are over the threshold, we need to switch to slow sampling.
	if (GetSamplingState() == SamplingState::RANDOM && GetTuplesSeen() >= FIXED_SAMPLE_SIZE * FAST_TO_SLOW_THRESHOLD) {
		ConvertToReservoirSample();
	}
	if (static_cast<int64_t>(reservoir_chunk->chunk.size()) >=
	    GetReservoirChunkCapacity<int64_t>() - (static_cast<int64_t>(FIXED_SAMPLE_SIZE) * 3)) {
		Vacuum();
	}
}

void ReservoirSample::Verify() {
#ifdef DEBUG
	if (destroyed) {
		return;
	}
	if (GetPriorityQueueSize() == 0) {
		D_ASSERT(GetActiveSampleCount() <= sample_count);
		D_ASSERT(GetTuplesSeen() >= GetActiveSampleCount());
		return;
	}
	if (NumSamplesCollected() > sample_count) {
		D_ASSERT(GetPriorityQueueSize() == sample_count);
	} else if (NumSamplesCollected() <= sample_count && GetPriorityQueueSize() > 0) {
		// it's possible to collect more samples than your priority queue size.
		// see sample_converts_to_reservoir_sample.test
		D_ASSERT(NumSamplesCollected() >= GetPriorityQueueSize());
	}
	auto base_reservoir_copy = base_reservoir_sample->Copy();
	std::unordered_map<idx_t, idx_t> index_count;
	while (!base_reservoir_copy->reservoir_weights.empty()) {
		auto &pair = base_reservoir_copy->reservoir_weights.top();
		if (index_count.find(pair.second) == index_count.end()) {
			index_count[pair.second] = 1;
			base_reservoir_copy->reservoir_weights.pop();
		} else {
			index_count[pair.second] += 1;
			base_reservoir_copy->reservoir_weights.pop();
			throw InternalException("Duplicate selection index in reservoir weights");
		}
	}
	// TODO: Verify the Sel as well. No duplicate indices.

	if (reservoir_chunk) {
		reservoir_chunk->chunk.Verify();
	}
#endif
}

ReservoirSamplePercentage::ReservoirSamplePercentage(double percentage, int64_t seed, idx_t reservoir_sample_size)
    : BlockingSample(seed), allocator(Allocator::DefaultAllocator()), sample_percentage(percentage / 100.0),
      reservoir_sample_size(reservoir_sample_size), current_count(0), is_finalized(false) {
	current_sample = make_uniq<ReservoirSample>(allocator, reservoir_sample_size, base_reservoir_sample->random());
	type = SampleType::RESERVOIR_PERCENTAGE_SAMPLE;
}

ReservoirSamplePercentage::ReservoirSamplePercentage(Allocator &allocator, double percentage, int64_t seed)
    : BlockingSample(seed), allocator(allocator), sample_percentage(percentage / 100.0), current_count(0),
      is_finalized(false) {
	reservoir_sample_size = (idx_t)(sample_percentage * RESERVOIR_THRESHOLD);
	current_sample = make_uniq<ReservoirSample>(allocator, reservoir_sample_size, base_reservoir_sample->random());
	type = SampleType::RESERVOIR_PERCENTAGE_SAMPLE;
}

ReservoirSamplePercentage::ReservoirSamplePercentage(double percentage, int64_t seed)
    : ReservoirSamplePercentage(Allocator::DefaultAllocator(), percentage, seed) {
}

void ReservoirSamplePercentage::AddToReservoir(DataChunk &input) {
	base_reservoir_sample->num_entries_seen_total += input.size();
	if (current_count + input.size() > RESERVOIR_THRESHOLD) {
		// we don't have enough space in our current reservoir
		// first check what we still need to append to the current sample
		idx_t append_to_current_sample_count = RESERVOIR_THRESHOLD - current_count;
		idx_t append_to_next_sample = input.size() - append_to_current_sample_count;
		if (append_to_current_sample_count > 0) {
			// we have elements remaining, first add them to the current sample
			if (append_to_next_sample > 0) {
				// we need to also add to the next sample
				DataChunk new_chunk;
				new_chunk.InitializeEmpty(input.GetTypes());
				new_chunk.Slice(input, *FlatVector::IncrementalSelectionVector(), append_to_current_sample_count);
				new_chunk.Flatten();
				current_sample->AddToReservoir(new_chunk);
			} else {
				input.Flatten();
				input.SetChildCardinality(append_to_current_sample_count);
				current_sample->AddToReservoir(input);
			}
		}
		if (append_to_next_sample > 0) {
			// slice the input for the remainder
			SelectionVector sel(append_to_next_sample);
			for (idx_t i = append_to_current_sample_count; i < append_to_next_sample + append_to_current_sample_count;
			     i++) {
				sel.set_index(i - append_to_current_sample_count, i);
			}
			input.Slice(sel, append_to_next_sample);
		}
		// now our first sample is filled: append it to the set of finished samples
		finished_samples.push_back(std::move(current_sample));

		// allocate a new sample, and potentially add the remainder of the current input to that sample
		current_sample = make_uniq<ReservoirSample>(allocator, reservoir_sample_size, base_reservoir_sample->random());
		if (append_to_next_sample > 0) {
			current_sample->AddToReservoir(input);
		}
		current_count = append_to_next_sample;
	} else {
		// we can just append to the current sample
		current_count += input.size();
		current_sample->AddToReservoir(input);
	}
}

unique_ptr<DataChunk> ReservoirSamplePercentage::GetChunk() {
	// reservoir sample percentage should never stay
	if (!is_finalized) {
		Finalize();
	}
	while (!finished_samples.empty()) {
		auto &front = finished_samples.front();
		auto chunk = front->GetChunk();
		if (chunk && chunk->size() > 0) {
			return chunk;
		}
		// move to the next sample
		finished_samples.erase(finished_samples.begin());
	}
	return nullptr;
}

idx_t ReservoirSamplePercentage::GetActiveSampleCount() const {
	D_ASSERT(is_finalized);
	idx_t count = 0;
	for (auto &sample : finished_samples) {
		count += sample->GetActiveSampleCount();
	}
	return count;
}

unique_ptr<BlockingSample> ReservoirSamplePercentage::Copy() const {
	throw InternalException("Cannot call Copy on ReservoirSample Percentage");
}

void ReservoirSamplePercentage::Finalize() {
	// need to finalize the current sample, if any
	// we are finalizing, so we are starting to return chunks. Our last chunk has
	// sample_percentage * RESERVOIR_THRESHOLD entries that hold samples.
	// if our current count is less than the sample_percentage * RESERVOIR_THRESHOLD
	// then we have sampled too much for the current_sample and we need to redo the sample
	// otherwise we can just push the current sample back
	// Imagine sampling 70% of 100 rows (so 70 rows). We allocate sample_percentage * RESERVOIR_THRESHOLD
	// -----------------------------------------
	auto sampled_more_than_required =
	    static_cast<double>(current_count) > sample_percentage * RESERVOIR_THRESHOLD || finished_samples.empty();
	if (current_count > 0 && sampled_more_than_required) {
		// create a new sample
		auto new_sample_size = static_cast<idx_t>(round(sample_percentage * static_cast<double>(current_count)));
		auto new_sample = make_uniq<ReservoirSample>(allocator, new_sample_size, base_reservoir_sample->random());
		while (true) {
			auto chunk = current_sample->GetChunk();
			if (!chunk || chunk->size() == 0) {
				break;
			}
			new_sample->AddToReservoir(*chunk);
		}
		finished_samples.push_back(std::move(new_sample));
	} else {
		finished_samples.push_back(std::move(current_sample));
	}
	// when finalizing, current_sample is null. All samples are now in finished samples.
	current_sample = nullptr;
	is_finalized = true;
}

} // namespace duckdb
