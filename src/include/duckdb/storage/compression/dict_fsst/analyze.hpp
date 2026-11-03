#pragma once

#include "duckdb/common/bitpacking.hpp"
#include "duckdb/common/primitive_dictionary.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/compression/dict_fsst/common.hpp"

namespace duckdb {

namespace dict_fsst {

//===--------------------------------------------------------------------===//
// Analyze
//===--------------------------------------------------------------------===//
struct DictFSSTAnalyzeState : public AnalyzeState {
public:
	//! Distinct values the set of a simulated block starts out with, and the factor it grows by when it fills up
	static constexpr idx_t INITIAL_SET_SIZE = 1024;
	static constexpr idx_t SET_GROWTH_FACTOR = 4;
	//! Fixed point scale of 'fsst_ratio'
	static constexpr idx_t FSST_RATIO_SCALE = 1024;
	//! Bytes beyond twice the input length that FSST needs in its output buffer, as specified in fsst.h
	static constexpr idx_t FSST_OUTPUT_SLACK = 7;
	//! Amount of the dictionary of a block that is encoded to measure the ratio, which is what FSST trains on
	static constexpr idx_t FSST_MEASURE_SIZE = 16384;
	//! Number of times a full block considers encoding its dictionary between two measurements
	static constexpr idx_t FSST_MEASURE_INTERVAL = 4;

public:
	explicit DictFSSTAnalyzeState(BlockManager &block_manager);

public:
	bool Analyze(const Vector &input);
	idx_t FinalAnalyze();

	//! Space one block with an unencoded dictionary needs to hold the given amount of tuples and dictionary entries
	static idx_t RequiredSpace(idx_t tuple_count, idx_t unique_count, idx_t dict_size, idx_t max_string_length);
	//! Same for the layout of the given mode, where an encoded dictionary also stores the symbol table
	static idx_t RequiredSpace(DictionaryAppendState mode, idx_t tuple_count, idx_t unique_count, idx_t dict_size,
	                           idx_t max_string_length, idx_t symbol_table_size);

private:
	//! Account for a tuple that reuses a dictionary index (NULL or a repeated value); false if the block is full, or
	//! if it decided to store every value, in which case the tuple has to be stored like a new value
	bool TryAddIndexOnlyTuple();
	//! Account for a value the current block has not seen, flushing the block first if it does not fit. The hash is
	//! only passed when the caller had to compute it, a block that stores every value does not need one.
	void AddNewString(const string_t &str, optional_idx hash);
	void FlushSimulatedBlock();
	//! Space a flushed segment of the given size takes on disk: the partial block manager packs small segments
	//! together, but a segment above its limit gets a block of its own
	idx_t SegmentCharge(idx_t required_space) const;
	unique_ptr<PrimitiveDictionary<string_t>> MakeSet(idx_t size) const;
	void GrowSet();

	//! Space the current block can use: it reserves room for a symbol table until it is known to not need one
	idx_t AvailableSpace() const;
	//! Space the current block needs for the given contents, in the mode and with the symbol table it has
	idx_t BlockRequiredSpace(idx_t tuple_count, idx_t unique_count, idx_t dict_size, idx_t max_string_length) const;
	//! Length of a value of the given length once FSST encoded, according to the measured ratio
	idx_t EncodedLength(idx_t length) const;
	//! Length a value takes in the dictionary of the current block
	idx_t StoredLength(idx_t length) const;
	//! The block is full for an unencoded dictionary: decide how to continue, the way the writer does when it is
	//! full (DictFSSTCompressionState::TryEncode)
	void SwitchFromRegular();
	DictionaryAppendState ChooseEncoding();
	//! FSST encode the dictionary of the current block, to learn how well values compress and how big the table is
	void MeasureFSST();

public:
	idx_t max_string_length = 0;
	bool contains_nulls = false;
	//! Effective exclusive size limit for plain dictionary encoding.
	idx_t string_size_limit = 0;
	//! Effective exclusive input size limit for worst-case FSST encoding.
	idx_t fsst_string_size_limit = 0;
	//! Flag which disables the usage of FSST if worst-case encoding blowup does not fit the block size.
	bool disable_fsst = false;
	idx_t total_count = 0;

	//! The dictionary is block-local, so the size estimate has to simulate how values pack into blocks:
	//! duplicates that end up in different blocks are stored once per block, not once in total.
	//! Space charged for the simulated blocks flushed so far, see SegmentCharge
	idx_t simulated_space = 0;
	//! How the current simulated block stores its values, which changes when it fills up (see SwitchFromRegular)
	DictionaryAppendState current_mode = DictionaryAppendState::REGULAR;
	idx_t current_tuple_count = 0;
	idx_t current_unique_count = 0;
	//! Size of the dictionary and of its longest entry, as stored in the mode of the block
	idx_t current_dict_size = 0;
	idx_t current_max_string_length = 0;
	//! Size of the symbol table of the block, 0 as long as its dictionary is not encoded
	idx_t current_symbol_table_size = 0;
	//! Bit width of the dictionary indices and the space the block needs in its current state, cached so a tuple
	//! that reuses an index only has to add a packing group now and then instead of recomputing the layout
	bitpacking_width_t current_indices_width = 0;
	idx_t current_required_space = 0;
	//! Set of the distinct values in the current simulated block, grown when it fills up
	unique_ptr<PrimitiveDictionary<string_t>> current_set;
	//! Number of values 'current_set' was created for
	idx_t current_set_size = 0;
	//! Most distinct strings a block can hold, where 'current_set' stops growing
	idx_t max_set_size = 0;
	//! Owns the memory of the strings in 'current_set'; reset per simulated block to bound memory usage
	StringHeap heap;
	//! The first values the current block stored, to measure how well they compress if the block fills up
	vector<string_t> measure_sample;
	idx_t measure_sample_size = 0;

	//! What encoding a dictionary with FSST achieves, measured on the dictionary of a block that filled up
	//! Encoded over unencoded size, in FSST_RATIO_SCALE
	idx_t fsst_ratio = FSST_RATIO_SCALE;
	idx_t fsst_symbol_table_size = 0;
	//! Number of times a full block considered encoding its dictionary, and the number at which to measure next
	idx_t encode_decisions = 0;
	idx_t next_measurement = 1;
};

} // namespace dict_fsst

} // namespace duckdb
