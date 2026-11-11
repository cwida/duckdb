//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/reservoir_sample.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/windows_undefs.hpp"

#include "duckdb/common/queue.hpp"

// Originally intended to be the vector size, but in order to run on
// vector size = 2, we had to change it.
#define FIXED_SAMPLE_SIZE 2048

namespace duckdb {

enum class SampleType : uint8_t { BLOCKING_SAMPLE = 0, RESERVOIR_SAMPLE = 1, RESERVOIR_PERCENTAGE_SAMPLE = 2 };

enum class SamplingState : uint8_t { RANDOM = 0, RESERVOIR = 1 };

class ReservoirRNG : public RandomEngine {
public:
	// return type must be called result type to be a valid URNG
	typedef uint32_t result_type;

	explicit ReservoirRNG(int64_t seed) : RandomEngine(seed) {};

	result_type operator()() {
		return NextRandomInteger();
	};

	static constexpr result_type min() {
		return NumericLimits<result_type>::Minimum();
	};
	static constexpr result_type max() {
		return NumericLimits<result_type>::Maximum();
	};
};

//! Reservoir sampling is based on the 2005 paper "Weighted Random Sampling" by Efraimidis and Spirakis
class BaseReservoirSampling {
public:
	explicit BaseReservoirSampling(int64_t seed);
	BaseReservoirSampling();

	void InitializeReservoirWeights(idx_t cur_size, idx_t sample_size);

	void SetNextEntry();

	void ReplaceElementWithIndex(idx_t entry_index, double with_weight, bool pop = true);
	void ReplaceElement(double with_weight = -1);

	void UpdateMinWeightThreshold();

	//! Go from the naive sampling to the reservoir sampling
	//! Naive sampling will not collect weights, but when we serialize
	//! we need to serialize weights again.
	void FillWeights(SelectionVector &sel, idx_t &sel_size);

	unique_ptr<BaseReservoirSampling> Copy();

	//! The random generator
	ReservoirRNG random;

	//! The next element to sample
	idx_t next_index_to_sample;
	//! The reservoir threshold of the current min entry
	double min_weight_threshold;
	//! The reservoir index of the current min entry
	idx_t min_weighted_entry_index;
	//! The current count towards next index (i.e. we will replace an entry in next_index - current_count tuples)
	//! The number of entries "seen" before choosing one that will go in our reservoir sample.
	idx_t num_entries_to_skip_b4_next_sample;
	//! when collecting a sample in parallel, we want to know how many values each thread has seen
	//! so we can collect the samples from the thread local states in a uniform manner
	idx_t num_entries_seen_total;
	//! Priority queue of [random element, index] for each of the elements in the sample
	std::priority_queue<std::pair<double, idx_t>> reservoir_weights;

	void Serialize(Serializer &serializer) const;
	static unique_ptr<BaseReservoirSampling> Deserialize(Deserializer &deserializer);

	// static unordered_map<idx_t, double> tuples_to_min_weight_map;
	// Blocking sample is a virtual class. It should be allowed to see the weights and
	// of tuples in the sample. The blocking sample can then easily maintain statistical properties
	// from the sample point of view.
	friend class BlockingSample;
};

class BlockingSample {
public:
	static constexpr const SampleType TYPE = SampleType::BLOCKING_SAMPLE;

	unique_ptr<BaseReservoirSampling> base_reservoir_sample;
	//! The sample type
	SampleType type;
	//! has the sample been destroyed due to updates to the referenced table
	bool destroyed;

public:
	explicit BlockingSample(int64_t seed = -1)
	    : base_reservoir_sample(make_uniq<BaseReservoirSampling>(seed)), type(SampleType::BLOCKING_SAMPLE),
	      destroyed(false) {
	}
	virtual ~BlockingSample() {
	}

	//! Add a chunk of data to the sample
	virtual void AddToReservoir(DataChunk &input) = 0;
	virtual unique_ptr<BlockingSample> Copy() const = 0;
	virtual void Finalize() = 0;
	virtual void Destroy();

	//! Fetches a chunk from the sample. destroy = true should only be used when
	//! querying from a sample defined in a query and not a duckdb_table_sample.
	virtual unique_ptr<DataChunk> GetChunk() = 0;

	virtual void Serialize(Serializer &serializer) const;
	static unique_ptr<BlockingSample> Deserialize(Deserializer &deserializer);

	//! Helper functions needed to merge two reservoirs while respecting weights of sampled rows
	std::pair<double, idx_t> PopFromWeightQueue();
	double GetMinWeightThreshold();
	idx_t GetPriorityQueueSize();

public:
	template <class TARGET>
	TARGET &Cast() {
		if (type != TARGET::TYPE && TARGET::TYPE != SampleType::BLOCKING_SAMPLE) {
			InternalException::Throw("Failed to cast sample to type - sample type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (type != TARGET::TYPE && TARGET::TYPE != SampleType::BLOCKING_SAMPLE) {
			InternalException::Throw("Failed to cast sample to type - sample type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}
};

class ReservoirChunk {
public:
	ReservoirChunk() {
	}

	DataChunk chunk;
	void Serialize(Serializer &serializer) const;
	static unique_ptr<ReservoirChunk> Deserialize(Deserializer &deserializer);

	unique_ptr<ReservoirChunk> Copy() const;
};

struct SelectionVectorHelper {
	SelectionVector sel;
	uint32_t size;
};

class ReservoirSample : public BlockingSample {
public:
	static constexpr const SampleType TYPE = SampleType::RESERVOIR_SAMPLE;

	constexpr static idx_t FIXED_SAMPLE_SIZE_MULTIPLIER = 10;
	// size is small enough, then the threshold to switch
	// MinValue between std vec size and fixed sample size.
	// During 'fast' sampling, we want every new vector to have the potential
	// to add to the sample. If the threshold is too far below the standard vector size, then
	// samples in the sample have a higher weight than new samples coming in.
	// i.e during vector_size=2, 2 new samples will not be significant compared 2048 samples from 204800 tuples.
	constexpr static idx_t FAST_TO_SLOW_THRESHOLD = MinValue<idx_t>(STANDARD_VECTOR_SIZE, 60);

	// If the table has less than 204800 rows, this is the percentage
	// of values we save when serializing/returning a sample.
	constexpr static double SAVE_PERCENTAGE = 0.01;

	ReservoirSample(Allocator &allocator, idx_t sample_count, int64_t seed = 1);
	explicit ReservoirSample(idx_t sample_count, unique_ptr<ReservoirChunk> = nullptr);

	//! methods used to help with serializing and deserializing
	void EvictOverBudgetSamples();
	void ExpandSerializedSample();

	SamplingState GetSamplingState() const;

	//! Vacuum the Reservoir Sample so it throws away tuples that are not in the
	//! reservoir weights or in the selection vector
	void Vacuum();

	//! Transform To sample based on reservoir sampling paper
	void ConvertToReservoirSample();

	//! Get the capactiy of the data chunk reserved for storing samples
	template <typename T>
	T GetReservoirChunkCapacity() const;

	//! If for_serialization=true then the sample_chunk is not padded with extra spaces for
	//! future sampling values
	unique_ptr<BlockingSample> Copy() const override;

	//! create the first chunk called by AddToReservoir()
	idx_t FillReservoir(DataChunk &chunk);
	//! Add a chunk of data to the sample
	void AddToReservoir(DataChunk &input) override;
	//! Merge two Reservoir Samples. Other must be a reservoir sample
	void Merge(unique_ptr<BlockingSample> other);

	void ShuffleSel(SelectionVector &sel, idx_t range, idx_t size) const;

	//! Update the sample by pushing new sample rows to the end of the sample_chunk.
	//! The new sample rows are the tuples rows resulting from applying sel to other
	void UpdateSampleAppend(DataChunk &this_, DataChunk &other, SelectionVector &other_sel, idx_t append_count) const;

	idx_t GetTuplesSeen() const;
	idx_t NumSamplesCollected() const;
	idx_t GetActiveSampleCount() const;
	static bool ValidSampleType(const LogicalType &type);

	// get the chunk from Reservoir chunk
	DataChunk &Chunk();

	//! Fetches a chunk from the sample. Note that this method is destructive and should only be used after the
	//! sample is completely built.
	// unique_ptr<DataChunk> GetChunkAndDestroy() override;
	unique_ptr<DataChunk> GetChunk() override;
	void Destroy() override;
	void Finalize() override;
	void Verify();

	idx_t GetSampleCount();

	// map is [index in input chunk] -> [index in sample chunk]. Both are zero-based
	// [index in sample chunk] is incremented by 1
	// index in input chunk have random values, however, they are increasing.
	// The base_reservoir_sampling gets updated however, so the indexes point to (sample_chunk_offset +
	// index_in_sample_chunk) this data is used to make a selection vector to copy samples from the input chunk to the
	// sample chunk
	//! Get indexes from current sample that can be replaced.
	SelectionVectorHelper GetReplacementIndexes(idx_t sample_chunk_offset, idx_t theoretical_chunk_length);

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<BlockingSample> Deserialize(Deserializer &deserializer);

private:
	// when we serialize, we may have collected too many samples since we fill a standard vector size, then
	// truncate if the table is still <=204800 values. The problem is, in our weights, we store indexes into
	// the selection vector. If throw away values at selection vector index i = 5 , we need to update all indexes
	// i > 5. Otherwise we will have indexes in the weights that are greater than the length of our sample.
	void NormalizeWeights();

	SelectionVectorHelper GetReplacementIndexesSlow(const idx_t sample_chunk_offset, const idx_t chunk_length);
	SelectionVectorHelper GetReplacementIndexesFast(const idx_t sample_chunk_offset, const idx_t chunk_length);
	void SimpleMerge(ReservoirSample &other);
	void WeightedMerge(ReservoirSample &other_sample);

	// Helper methods for Shrink().
	// Shrink has different logic depending on if the Reservoir sample is still in
	// "Random" mode or in "reservoir" mode. This function creates a new sample chunk
	// to copy the old sample chunk into
	unique_ptr<ReservoirChunk> CreateNewSampleChunk(vector<LogicalType> &types, idx_t size) const;

	// Get a vector where each index is a random int in the range 0, size.
	// This is used to shuffle selection vector indexes
	vector<uint32_t> GetRandomizedVector(uint32_t range, uint32_t size) const;

	idx_t sample_count;
	Allocator &allocator;
	unique_ptr<ReservoirChunk> reservoir_chunk;
	bool stats_sample;
	SelectionVector sel;
	idx_t sel_size;
};

//! The reservoir sample sample_size class maintains a streaming sample of variable size
class ReservoirSamplePercentage : public BlockingSample {
	constexpr static idx_t RESERVOIR_THRESHOLD = 100000;

public:
	static constexpr const SampleType TYPE = SampleType::RESERVOIR_PERCENTAGE_SAMPLE;

	ReservoirSamplePercentage(Allocator &allocator, double percentage, int64_t seed = -1);
	ReservoirSamplePercentage(double percentage, int64_t seed, idx_t reservoir_sample_size);
	explicit ReservoirSamplePercentage(double percentage, int64_t seed = -1);

	//! Add a chunk of data to the sample
	void AddToReservoir(DataChunk &input) override;

	unique_ptr<BlockingSample> Copy() const override;

	//! Fetches a chunk from the sample. If destroy = true this method is descructive
	unique_ptr<DataChunk> GetChunk() override;
	void Finalize() override;
	idx_t GetActiveSampleCount() const;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<BlockingSample> Deserialize(Deserializer &deserializer);

private:
	Allocator &allocator;
	//! The sample_size to sample
	double sample_percentage;
	//! The fixed sample size of the sub-reservoirs
	idx_t reservoir_sample_size;

	//! The current sample
	unique_ptr<ReservoirSample> current_sample;

	//! The set of finished samples of the reservoir sample
	vector<unique_ptr<ReservoirSample>> finished_samples;

	//! The amount of tuples that have been processed so far (not put in the reservoir, just processed)
	idx_t current_count = 0;
	//! Whether or not the stream is finalized. The stream is automatically finalized on the first call to
	//! GetChunkAndShrink();
	bool is_finalized;
};

} // namespace duckdb
