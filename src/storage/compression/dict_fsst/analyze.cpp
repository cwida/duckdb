#include "duckdb/storage/compression/dict_fsst/analyze.hpp"

#include "duckdb/common/bitpacking.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/compression/dict_fsst/common.hpp"

namespace duckdb {
namespace dict_fsst {

//! Determine the size requirements for the worst case, which is when a single string fills an
//! entire segment on its own.
static idx_t GetStringSizeLimit(const idx_t available_space, const bool fsst_encoded) {
	idx_t max_str_len = DictFSSTCompression::STRING_SIZE_LIMIT - 1;
	if (fsst_encoded) {
		// In the worst case FSST may double the string length by prepending every byte with an exception
		max_str_len *= 2;
	}

	// Dictionary contains NULL and current string
	const bitpacking_width_t string_lengths_width = BitpackingPrimitives::MinimumBitWidth(max_str_len);
	const idx_t string_lengths_space = BitpackingPrimitives::GetRequiredSize(2, string_lengths_width);

	// Dictionary stores only one valid string
	const bitpacking_width_t dict_indices_width = BitpackingPrimitives::MinimumBitWidth(1);
	const idx_t dict_indices_space = BitpackingPrimitives::GetRequiredSize(1, dict_indices_width);

	idx_t metadata_size = 0;
	metadata_size += AlignValue<idx_t>(sizeof(dict_fsst_compression_header_t));
	if (fsst_encoded) {
		// As denoted in fsst.h
		metadata_size += 7;
	}
	// Reserve maximum alignment padding for variable length string
	metadata_size += sizeof(idx_t) - 1;
	if (fsst_encoded) {
		metadata_size += AlignValue<idx_t>(DictFSSTCompression::FSST_SYMBOL_TABLE_SIZE);
	}
	metadata_size += AlignValue<idx_t>(string_lengths_space);
	metadata_size += dict_indices_space;

	D_ASSERT(metadata_size < available_space);
	idx_t max_string_size = available_space - metadata_size;

	if (fsst_encoded) {
		max_string_size = max_string_size / 2;
	}

	return MinValue(DictFSSTCompression::STRING_SIZE_LIMIT, max_string_size + 1);
}

DictFSSTAnalyzeState::DictFSSTAnalyzeState(BlockManager &block_manager) : AnalyzeState(block_manager) {
	const auto block_size = info.GetBlockSize();

	string_size_limit = GetStringSizeLimit(block_size, false);
	fsst_string_size_limit = GetStringSizeLimit(block_size, true);

	// a block cannot hold more distinct strings than half its size in bytes, matching the compression state
	max_set_size = block_size / 2;
	// most blocks hold few distinct values, so start small and grow the set only when it runs out of room
	current_set_size = MinValue(INITIAL_SET_SIZE, max_set_size);
	current_set = MakeSet(current_set_size);
	current_required_space = BlockRequiredSpace(0, 0, 0, 0);
}

unique_ptr<PrimitiveDictionary<string_t>> DictFSSTAnalyzeState::MakeSet(idx_t size) const {
	auto &allocator = info.GetBlockManager().buffer_manager.GetBufferAllocator();
	return make_uniq<PrimitiveDictionary<string_t>>(allocator, size, 1);
}

void DictFSSTAnalyzeState::GrowSet() {
	D_ASSERT(current_set_size < max_set_size);
	current_set_size = MinValue(current_set_size * SET_GROWTH_FACTOR, max_set_size);
	auto grown = MakeSet(current_set_size);
	// the strings live in 'heap', so the entries carry over as they are
	current_set->IterateValues([&](const string_t &value, const string_t &) { grown->Insert(value); });
	current_set = std::move(grown);
}

idx_t DictFSSTAnalyzeState::RequiredSpace(idx_t tuple_count, idx_t unique_count, idx_t dict_size,
                                          idx_t max_string_length) {
	return RequiredSpace(DictionaryAppendState::NOT_ENCODED, tuple_count, unique_count, dict_size, max_string_length,
	                     0);
}

idx_t DictFSSTAnalyzeState::RequiredSpace(DictionaryAppendState mode, idx_t tuple_count, idx_t unique_count,
                                          idx_t dict_size, idx_t max_string_length, idx_t symbol_table_size) {
	// index 0 of the dictionary is reserved for NULL
	const idx_t dict_count = unique_count + 1;

	const auto string_lengths_width = BitpackingPrimitives::MinimumBitWidth(max_string_length);
	const auto string_lengths_space = BitpackingPrimitives::GetRequiredSize(dict_count, string_lengths_width);

	idx_t required_space = AlignValue<idx_t>(sizeof(dict_fsst_compression_header_t));
	required_space += dict_size;
	required_space = AlignValue<idx_t>(required_space);
	if (IsEncoded(mode)) {
		required_space += symbol_table_size;
		required_space = AlignValue<idx_t>(required_space);
	}
	required_space += string_lengths_space;
	required_space = AlignValue<idx_t>(required_space);
	if (mode != DictionaryAppendState::ENCODED_ALL_UNIQUE) {
		// when every value is stored there is no selection buffer
		const auto dictionary_indices_width = BitpackingPrimitives::MinimumBitWidth(dict_count - 1);
		required_space += BitpackingPrimitives::GetRequiredSize(tuple_count, dictionary_indices_width);
	}
	return required_space;
}

idx_t DictFSSTAnalyzeState::BlockRequiredSpace(idx_t tuple_count, idx_t unique_count, idx_t dict_size,
                                               idx_t max_string_length) const {
	return RequiredSpace(current_mode, tuple_count, unique_count, dict_size, max_string_length,
	                     current_symbol_table_size);
}

idx_t DictFSSTAnalyzeState::AvailableSpace() const {
	if (current_mode == DictionaryAppendState::REGULAR) {
		return info.GetBlockSize() - DictFSSTCompression::FSST_SYMBOL_TABLE_SIZE;
	}
	return info.GetBlockSize();
}

idx_t DictFSSTAnalyzeState::EncodedLength(idx_t length) const {
	if (!length) {
		return 0;
	}
	return MaxValue<idx_t>((length * fsst_ratio + FSST_RATIO_SCALE - 1) / FSST_RATIO_SCALE, 1);
}

idx_t DictFSSTAnalyzeState::StoredLength(idx_t length) const {
	return IsEncoded(current_mode) ? EncodedLength(length) : length;
}

idx_t DictFSSTAnalyzeState::SegmentCharge(idx_t required_space) const {
	// a segment above the limit is written to a block of its own whose remainder is lost, a smaller one is packed
	// with other segments, see PartialBlockManager::RetainOrFlush
	if (required_space > info.GetCompactionFlushLimit()) {
		return info.GetBlockSize();
	}
	return AlignValue<idx_t>(required_space);
}

void DictFSSTAnalyzeState::MeasureFSST() {
	// The writer trains the symbol table on the dictionary of the block. FSST only looks at a sample of what it is
	// given, so the first values of the block are enough to tell how well the dictionary compresses.
	vector<unsigned char> dictionary;
	vector<size_t> lengths;
	dictionary.reserve(measure_sample_size);
	lengths.reserve(measure_sample.size());
	for (auto &value : measure_sample) {
		const auto data = const_data_ptr_cast(value.GetData());
		dictionary.insert(dictionary.end(), data, data + value.GetSize());
		lengths.push_back(value.GetSize());
	}
	if (dictionary.empty()) {
		return;
	}
	vector<unsigned char *> pointers;
	pointers.reserve(lengths.size());
	idx_t offset = 0;
	for (auto length : lengths) {
		pointers.push_back(dictionary.data() + offset);
		offset += length;
	}

	auto encoder = duckdb_fsst_create(lengths.size(), lengths.data(), pointers.data(), 0);
	// as specified in fsst.h, the output buffer has to hold the worst case
	vector<unsigned char> output(7 + 2 * dictionary.size());
	vector<size_t> encoded_lengths(lengths.size());
	vector<unsigned char *> encoded_pointers(lengths.size());
	const auto encoded_count =
	    duckdb_fsst_compress(encoder, lengths.size(), lengths.data(), pointers.data(), output.size(), output.data(),
	                         encoded_lengths.data(), encoded_pointers.data());
	vector<unsigned char> symbol_table(sizeof(duckdb_fsst_decoder_t));
	const idx_t symbol_table_size = duckdb_fsst_export(encoder, symbol_table.data());
	duckdb_fsst_destroy(encoder);

	idx_t encoded_size = 0;
	for (auto length : encoded_lengths) {
		encoded_size += length;
	}
	if (encoded_count != lengths.size()) {
		// cannot happen with a worst case sized output, but it must not make encoding look attractive
		encoded_size = dictionary.size();
	}
	// round up, so the estimate is not too optimistic
	fsst_ratio = MinValue<idx_t>((encoded_size * FSST_RATIO_SCALE + dictionary.size() - 1) / dictionary.size(),
	                             2 * FSST_RATIO_SCALE);
	fsst_symbol_table_size = symbol_table_size;
}

DictionaryAppendState DictFSSTAnalyzeState::ChooseEncoding() {
	if (disable_fsst || current_dict_size < DictFSSTCompression::DICTIONARY_ENCODE_THRESHOLD) {
		return DictionaryAppendState::NOT_ENCODED;
	}
	// the writer trains a symbol table for every block that gets here, which is too expensive to do while
	// analyzing. The ratio is measured on the first block and again after every few more.
	encode_decisions++;
	if (encode_decisions == next_measurement) {
		MeasureFSST();
		next_measurement *= FSST_MEASURE_INTERVAL;
	}

	const idx_t encoded_dict_size = EncodedLength(current_dict_size);
	if (encoded_dict_size + DictFSSTCompression::DICTIONARY_ENCODE_THRESHOLD > current_dict_size) {
		// the dictionary does not compress well enough to be worth it
		return DictionaryAppendState::NOT_ENCODED;
	}
	// The writer encodes into a buffer as big as the dictionary, and FSST only encodes a value while the space left
	// holds twice its length and 7 more bytes. A dictionary of a few large values can therefore not be encoded.
	// The longest value is assumed to come last, which is the hardest position.
	const idx_t space_before_longest = current_dict_size - EncodedLength(current_dict_size - current_max_string_length);
	if (space_before_longest < 2 * current_max_string_length + FSST_OUTPUT_SLACK) {
		return DictionaryAppendState::NOT_ENCODED;
	}
	// without NULLs and duplicates every value can be stored as is, which saves the selection buffer
	const bool all_unique = !contains_nulls && current_unique_count == current_tuple_count;
	const auto mode = all_unique ? DictionaryAppendState::ENCODED_ALL_UNIQUE : DictionaryAppendState::ENCODED;
	const idx_t required_space = RequiredSpace(mode, current_tuple_count, current_unique_count, encoded_dict_size,
	                                           EncodedLength(current_max_string_length), fsst_symbol_table_size);
	if (required_space > info.GetBlockSize()) {
		return DictionaryAppendState::NOT_ENCODED;
	}
	return mode;
}

void DictFSSTAnalyzeState::SwitchFromRegular() {
	D_ASSERT(current_mode == DictionaryAppendState::REGULAR);
	current_mode = ChooseEncoding();
	if (!IsEncoded(current_mode)) {
		// the layout is the same, the block only stops reserving space for a symbol table
		return;
	}
	current_symbol_table_size = fsst_symbol_table_size;
	current_dict_size = EncodedLength(current_dict_size);
	current_max_string_length = EncodedLength(current_max_string_length);
	current_required_space =
	    BlockRequiredSpace(current_tuple_count, current_unique_count, current_dict_size, current_max_string_length);
	D_ASSERT(current_required_space <= info.GetBlockSize());
}

bool DictFSSTAnalyzeState::TryAddIndexOnlyTuple() {
	while (true) {
		// the dictionary does not change, so only the index buffer can grow, and only when a new packing group starts
		idx_t growth = 0;
		if (current_mode != DictionaryAppendState::ENCODED_ALL_UNIQUE &&
		    current_tuple_count % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE == 0) {
			growth = BitpackingPrimitives::GetRequiredSize(BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE,
			                                               current_indices_width);
		}
		if (current_required_space + growth <= AvailableSpace()) {
			current_tuple_count++;
			current_required_space += growth;
			D_ASSERT(current_required_space == BlockRequiredSpace(current_tuple_count, current_unique_count,
			                                                      current_dict_size, current_max_string_length));
			return true;
		}
		if (current_mode != DictionaryAppendState::REGULAR) {
			return false;
		}
		SwitchFromRegular();
		if (current_mode == DictionaryAppendState::ENCODED_ALL_UNIQUE) {
			// no lookups are performed in this mode, the value is stored again
			return false;
		}
	}
}

void DictFSSTAnalyzeState::AddNewString(const string_t &str, optional_idx hash) {
	const idx_t str_len = str.GetSize();
	idx_t stored_length;
	idx_t required_space;
	while (true) {
		stored_length = StoredLength(str_len);
		// a new value changes the dictionary size and with it the alignment of the sections after it: recompute
		required_space =
		    BlockRequiredSpace(current_tuple_count + 1, current_unique_count + 1, current_dict_size + stored_length,
		                       MaxValue(current_max_string_length, stored_length));
		idx_t admission_space = required_space;
		if (current_mode == DictionaryAppendState::ENCODED) {
			// The writer encodes the values it adds later, and reserves twice their length until then. It only
			// accepts a value when that fits next to everything stored before it.
			admission_space = BlockRequiredSpace(current_tuple_count + 1, current_unique_count + 1,
			                                     current_dict_size + 2 * str_len + FSST_OUTPUT_SLACK,
			                                     MaxValue(current_max_string_length, 2 * str_len));
		}
		// the set that finds repeated values holds a limited number of them
		const bool set_is_full =
		    current_mode != DictionaryAppendState::ENCODED_ALL_UNIQUE && current_unique_count >= max_set_size;
		if (admission_space <= AvailableSpace() && !set_is_full) {
			break;
		}
		if (current_mode == DictionaryAppendState::REGULAR) {
			SwitchFromRegular();
			continue;
		}
		if (!current_tuple_count) {
			throw InternalException("A string does not fit in an empty block while analyzing DICT_FSST");
		}
		FlushSimulatedBlock();
	}

	current_tuple_count++;
	current_unique_count++;
	current_dict_size += stored_length;
	current_max_string_length = MaxValue(current_max_string_length, stored_length);
	current_indices_width = BitpackingPrimitives::MinimumBitWidth(current_unique_count);
	current_required_space = required_space;

	if (current_mode == DictionaryAppendState::ENCODED_ALL_UNIQUE) {
		// nothing is looked up in this mode, so the value does not have to be remembered
		return;
	}
	if (current_set->GetSize() == current_set_size) {
		GrowSet();
	}
	// the block may have been flushed into one that looks values up, while the value was added without a hash
	const auto stored = heap.AddBlob(str);
	current_set->Insert(stored, hash.IsValid() ? hash.GetIndex() : Hash(str));
	D_ASSERT(!current_set->IsFull());
	if (current_mode == DictionaryAppendState::REGULAR && measure_sample_size < FSST_MEASURE_SIZE) {
		measure_sample.push_back(stored);
		measure_sample_size += str_len;
	}
}

void DictFSSTAnalyzeState::FlushSimulatedBlock() {
	simulated_space += SegmentCharge(current_required_space);
	current_mode = disable_fsst ? DictionaryAppendState::NOT_ENCODED : DictionaryAppendState::REGULAR;
	current_tuple_count = 0;
	current_unique_count = 0;
	current_dict_size = 0;
	current_max_string_length = 0;
	current_symbol_table_size = 0;
	current_indices_width = 0;
	current_required_space = BlockRequiredSpace(0, 0, 0, 0);
	current_set->Clear();
	measure_sample.clear();
	measure_sample_size = 0;
	// the strings are only referenced by 'current_set' and 'measure_sample', which we just cleared; keep the current
	// chunk for the next block
	heap.GetAllocator().Reset();
}

bool DictFSSTAnalyzeState::Analyze(const Vector &input) {
	for (auto entry : input.Values<string_t>()) {
		if (!entry.IsValid()) {
			contains_nulls = true;
			// NULL is dictionary index 0, it does not occupy a dictionary entry of its own
			if (!TryAddIndexOnlyTuple()) {
				FlushSimulatedBlock();
				// an empty block always has room for one more index
				const bool added = TryAddIndexOnlyTuple();
				D_ASSERT(added);
				(void)added;
			}
			continue;
		}
		auto &str = entry.GetValue();
		auto str_len = str.GetSize();
		if (str_len > max_string_length) {
			max_string_length = str_len;
		}
		if (str_len >= string_size_limit) {
			// A segment cannot be spread out over multiple blocks, so if a string cannot fit in an empty segment
			// the encoding will fail
			return false;
		}
		if (str_len >= fsst_string_size_limit) {
			// FSST strings may be up to two times larger than their plain equivalent
			disable_fsst = true;
		}

		if (current_mode == DictionaryAppendState::ENCODED_ALL_UNIQUE) {
			// the writer does not look values up in this mode, every one is stored
			AddNewString(str, optional_idx());
			continue;
		}
		const auto hash = Hash(str);
		if (!current_set->Lookup(str, hash).IsEmpty()) {
			if (TryAddIndexOnlyTuple()) {
				continue;
			}
			if (current_mode != DictionaryAppendState::ENCODED_ALL_UNIQUE) {
				// the next block starts with an empty dictionary, so this value has to be stored again
				FlushSimulatedBlock();
			}
		}
		AddNewString(str, hash);
	}
	total_count += input.size();
	return true;
}

idx_t DictFSSTAnalyzeState::FinalAnalyze() {
	// the simulation is complete, free its set before the compression state allocates a dictionary of its own
	current_set.reset();
	measure_sample.clear();
	heap.Destroy();

	if (!total_count) {
		return 0;
	}

	// Every block carries its own dictionary, so a value repeated across blocks is stored once per block.
	// Charging the simulated segments the way they are stored also charges for the space wasted in blocks that a
	// large dictionary left mostly empty - which a ratio of the raw size cannot express.
	idx_t estimate = simulated_space;
	if (current_tuple_count) {
		estimate += SegmentCharge(current_required_space);
	}
	return estimate;
}

} // namespace dict_fsst
} // namespace duckdb
