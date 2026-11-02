#include "duckdb/storage/compression/dict_fsst/compression.hpp"

#include "duckdb/common/fsst.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/compression/dict_fsst/analyze.hpp"
#include "fsst.h"

#if defined(__MVS__) && !defined(alloca)
#define alloca __builtin_alloca
#endif

namespace duckdb {
namespace dict_fsst {

DictFSSTCompressionState::DictFSSTCompressionState(ColumnDataCheckpointData &checkpoint_data_p,
                                                   unique_ptr<DictFSSTAnalyzeState> &&analyze_p)
    : StandardCompressionState(checkpoint_data_p, CompressionType::COMPRESSION_DICT_FSST), stats_writer(GetType()),
      current_string_map(
          info.GetBlockManager().buffer_manager.GetBufferAllocator(),
          MinValue(analyze_p.get()->total_count, info.GetBlockSize()) / 2, // maximum_size_p (amount of elements)
          1                                                                // maximum_target_capacity_p (byte capacity)
          ),
      analyze(std::move(analyze_p)),
      verify_compression(DBConfigOptions::global_verification_mode == DebugVerificationMode::VERIFY_COMPRESSION) {
	CreateEmptySegment();
}

DictFSSTCompressionState::~DictFSSTCompressionState() {
	if (encoder) {
		auto fsst_encoder = reinterpret_cast<duckdb_fsst_encoder_t *>(encoder);
		duckdb_fsst_destroy(fsst_encoder);
	}
}

static constexpr idx_t DICTIONARY_ENCODE_THRESHOLD = 4096;

static inline bool IsEncoded(DictionaryAppendState state) {
	return state == DictionaryAppendState::ENCODED || state == DictionaryAppendState::ENCODED_ALL_UNIQUE;
}

static DictFSSTMode ConvertToMode(DictionaryAppendState &state) {
	switch (state) {
	case DictionaryAppendState::ENCODED:
		return DictFSSTMode::DICT_FSST;
	case DictionaryAppendState::REGULAR:
	case DictionaryAppendState::NOT_ENCODED:
		return DictFSSTMode::DICTIONARY;
	case DictionaryAppendState::ENCODED_ALL_UNIQUE:
		return DictFSSTMode::FSST_ONLY;
	default:
		throw InternalException("DictFSSTMode not handled!");
	}
}

idx_t DictFSSTCompressionState::Finalize() {
	const bool is_fsst_encoded = IsEncoded(append_state);
	if (is_fsst_encoded) {
		D_ASSERT(analyze->disable_fsst == false);
	}

// calculate sizes
#ifdef DEBUG
	{
		auto dictionary_indices_space = BitpackingPrimitives::GetRequiredSize(tuple_count, dictionary_indices_width);
		D_ASSERT(dictionary_indices_space == this->dictionary_indices_space);

		auto string_lengths_space = BitpackingPrimitives::GetRequiredSize(dict_count, string_lengths_width);
		D_ASSERT(string_lengths_space == this->string_lengths_space);

		if (append_state == DictionaryAppendState::ENCODED_ALL_UNIQUE) {
			D_ASSERT(dictionary_indices_space == 0);
		}
	}
#endif

	if (!is_fsst_encoded) {
		symbol_table_size = 0;
	}

	D_ASSERT(to_encode_string_sum == 0);
	D_ASSERT(symbol_table_size != DConstants::INVALID_INDEX);

	idx_t required_space = 0;
	required_space += sizeof(dict_fsst_compression_header_t);
	required_space = AlignValue<idx_t>(required_space);
	required_space += dictionary_offset;
	required_space = AlignValue<idx_t>(required_space);
	if (is_fsst_encoded) {
		required_space += symbol_table_size;
		required_space = AlignValue<idx_t>(required_space);
	}
	required_space += string_lengths_space;
	required_space = AlignValue<idx_t>(required_space);
	required_space += dictionary_indices_space;

	D_ASSERT(info.GetBlockSize() >= required_space);

	// calculate ptr and offsets
	auto base_ptr = handle.GetDataMutable();
	auto header_ptr = reinterpret_cast<dict_fsst_compression_header_t *>(base_ptr);
	auto dictionary_dest = AlignValue<idx_t>(DictFSSTCompression::DICTIONARY_HEADER_SIZE);
	auto symbol_table_dest = AlignValue<idx_t>(dictionary_dest + dictionary_offset);
	auto string_lengths_dest = AlignValue<idx_t>(symbol_table_dest + symbol_table_size);
	auto dictionary_indices_dest = AlignValue<idx_t>(string_lengths_dest + string_lengths_space);

	header_ptr->mode = ConvertToMode(append_state);
	header_ptr->symbol_table_size = NumericCast<uint32_t>(symbol_table_size);
	header_ptr->dict_size = NumericCast<uint32_t>(dictionary_offset);
	header_ptr->dict_count = NumericCast<uint32_t>(dict_count);
	header_ptr->dictionary_indices_width = dictionary_indices_width;
	header_ptr->string_lengths_width = string_lengths_width;

	// Write the symbol table
	if (is_fsst_encoded) {
		D_ASSERT(symbol_table_size != DConstants::INVALID_INDEX);
		memcpy(base_ptr + symbol_table_dest, fsst_serialized_symbol_table.get(), symbol_table_size);
	}

	// Write the string lengths of the dictionary
	BitpackingPrimitives::PackBuffer<uint32_t, false>(base_ptr + string_lengths_dest, string_lengths.data(), dict_count,
	                                                  string_lengths_width);
	// Write the dictionary indices (selection vector)
	BitpackingPrimitives::PackBuffer<sel_t, false>(base_ptr + dictionary_indices_dest,
	                                               (sel_t *)(dictionary_indices.data()), tuple_count,
	                                               dictionary_indices_width);

#ifdef DEBUG
	if (append_state != DictionaryAppendState::ENCODED_ALL_UNIQUE) {
		auto expected_bitwidth = BitpackingPrimitives::MinimumBitWidth(dict_count - 1);
		D_ASSERT(dictionary_indices_width == expected_bitwidth);
	}
#endif
	D_ASSERT(base_ptr + required_space == base_ptr + dictionary_indices_dest + dictionary_indices_space);
	D_ASSERT((uint64_t)*max_element(std::begin(dictionary_indices), std::end(dictionary_indices)) == dict_count - 1);
	return required_space;
}

idx_t DictFSSTCompressionState::CalculateRequiredSpace() const {
	idx_t required_space = 0;
	required_space += sizeof(dict_fsst_compression_header_t);
	required_space = AlignValue<idx_t>(required_space);
	required_space += dictionary_offset + to_encode_string_sum;
	required_space = AlignValue<idx_t>(required_space);
	if (IsEncoded(append_state)) {
		required_space += symbol_table_size;
		required_space = AlignValue<idx_t>(required_space);
	}
	required_space += string_lengths_space;
	required_space = AlignValue<idx_t>(required_space);
	required_space += dictionary_indices_space;

	return required_space;
}

void DictFSSTCompressionState::FlushEncodingBuffer() {
	if (dictionary_encoding_buffer.empty()) {
		D_ASSERT(to_encode_string_sum == 0);
		return;
	}

	vector<size_t> fsst_string_sizes;
	vector<unsigned char *> fsst_string_ptrs;

	data_ptr_t dictionary_start =
	    AlignPointer<sizeof(void *)>(handle.GetDataMutable() + sizeof(dict_fsst_compression_header_t));
	D_ASSERT(dictionary_encoding_buffer.size() == dict_count - string_lengths.size());
	auto string_count = dictionary_encoding_buffer.size();
	idx_t sum = 0;
	for (auto &to_encode : dictionary_encoding_buffer) {
		auto str_len = to_encode.GetSize();
		sum += str_len * 2;
		fsst_string_sizes.push_back(str_len);
		fsst_string_ptrs.push_back((unsigned char *)to_encode.GetData()); // NOLINT
	}
	(void)sum;
	D_ASSERT(sum + 7 == to_encode_string_sum);

	auto compressed_ptrs = vector<unsigned char *>(string_count, nullptr);
	auto compressed_sizes = vector<size_t>(string_count, 0);

	// Compress the dictionary, straight to the segment
	idx_t taken_space = 0;
	taken_space += AlignValue<idx_t>(sizeof(dict_fsst_compression_header_t));
	taken_space += symbol_table_size;
	taken_space = AlignValue<idx_t>(taken_space);
	taken_space += string_lengths_space;
	taken_space = AlignValue<idx_t>(taken_space);
	taken_space += dictionary_indices_space;
	taken_space = AlignValue<idx_t>(taken_space);
	taken_space += dictionary_offset;
	D_ASSERT(taken_space < info.GetBlockSize());

	auto fsst_encoder = reinterpret_cast<duckdb_fsst_encoder_t *>(encoder);
	auto res =
	    duckdb_fsst_compress(fsst_encoder, string_count, fsst_string_sizes.data(), fsst_string_ptrs.data(),
	                         info.GetBlockSize() - taken_space, (unsigned char *)dictionary_start + dictionary_offset,
	                         compressed_sizes.data(), compressed_ptrs.data());
	if (res != string_count) {
		throw FatalException("Somehow we did not have enough room in the segment to store the encoded strings");
	}
	string_lengths_width = real_string_lengths_width;
	//! The highest value that still requires the current string lengths width
	uint32_t biggest_strlen = (1 << string_lengths_width) - 1;
	D_ASSERT(BitpackingPrimitives::MinimumBitWidth(biggest_strlen) == string_lengths_width);
	idx_t compressed_sum = 0;
	for (idx_t i = 0; i < string_count; i++) {
		auto str_len = UnsafeNumericCast<uint32_t>(compressed_sizes[i]);
		if (str_len >= biggest_strlen) {
			biggest_strlen = str_len;
		}
		compressed_sum += str_len;
		string_lengths.push_back(str_len);
		dictionary_offset += str_len;
	}
	(void)compressed_sum;
	D_ASSERT(compressed_sum <= to_encode_string_sum);
	if (biggest_strlen >= NumericCast<uint32_t>(1 << string_lengths_width)) {
		string_lengths_width = BitpackingPrimitives::MinimumBitWidth(biggest_strlen);
	}
	real_string_lengths_width = string_lengths_width;
	auto new_string_lengths_space = BitpackingPrimitives::GetRequiredSize(dict_count, string_lengths_width);
	D_ASSERT(new_string_lengths_space <= string_lengths_space);
	string_lengths_space = new_string_lengths_space;

	idx_t required_space = 0;
	required_space += sizeof(dict_fsst_compression_header_t);
	required_space = AlignValue<idx_t>(required_space);
	required_space += dictionary_offset;
	required_space = AlignValue<idx_t>(required_space);
	required_space += symbol_table_size;
	required_space = AlignValue<idx_t>(required_space);
	required_space += string_lengths_space;
	required_space = AlignValue<idx_t>(required_space);
	required_space += dictionary_indices_space;

	D_ASSERT(required_space <= info.GetBlockSize());

	D_ASSERT(string_lengths_space != 0);
	to_encode_string_sum = 0;
	dictionary_encoding_buffer.clear();
}

void DictFSSTCompressionState::CreateEmptySegment() {
	CreateAndPinNewSegment();

	// If analysis determined that FSST cannot be used, skip the decision phase.
	append_state = analyze->disable_fsst ? DictionaryAppendState::NOT_ENCODED : DictionaryAppendState::REGULAR;
	string_lengths_width = 0;
	real_string_lengths_width = 0;
	dictionary_indices_width = 0;
	string_lengths_space = 0;
	D_ASSERT(dictionary_indices.empty());
	dictionary_indices_space = 0;
	tuple_count = 0;
	D_ASSERT(string_lengths.empty());
	string_lengths.push_back(0);
	dict_count = 1;
	D_ASSERT(current_string_map.GetSize() == 0);
	symbol_table_size = DConstants::INVALID_INDEX;

	dictionary_offset = 0;
}

void DictFSSTCompressionState::Flush(bool final) {
	if (final) {
		FlushEncodingBuffer();
	}

	if (!tuple_count) {
		return;
	}

	current_segment->count = tuple_count;

	auto segment_size = Finalize();
	FlushCurrentSegment(stats_writer, segment_size);

	// Reset the state
	uncompressed_dictionary_copy.Destroy();
	//! This should already be empty at this point, otherwise that means that strings are not encoded / not added to the
	//! dictionary
	D_ASSERT(dictionary_encoding_buffer.empty());
	D_ASSERT(to_encode_string_sum == 0);

	current_string_map.Clear();
	string_lengths.clear();
	dictionary_indices.clear();
	if (encoder) {
		auto fsst_encoder = reinterpret_cast<duckdb_fsst_encoder_t *>(encoder);
		duckdb_fsst_destroy(fsst_encoder);
		encoder = nullptr;
		symbol_table_size = DConstants::INVALID_INDEX;
	}
	total_tuple_count += tuple_count;

	if (!final) {
		CreateEmptySegment();
	}
}

static inline bool RequiresHigherBitWidth(bitpacking_width_t bitwidth, uint32_t other) {
	return other >= NumericCast<uint32_t>(1 << bitwidth);
}

template <DictionaryAppendState APPEND_STATE>
static inline bool AddLookup(DictFSSTCompressionState &state, idx_t lookup, const bool recalculate_indices_space,
                             bool fail_on_no_space) {
	D_ASSERT(lookup != DConstants::INVALID_INDEX);

	//! This string exists in the dictionary
	idx_t new_dictionary_indices_space = state.dictionary_indices_space;
	auto get_bitpacking_size = APPEND_STATE != DictionaryAppendState::ENCODED_ALL_UNIQUE && recalculate_indices_space;
	if (get_bitpacking_size) {
		new_dictionary_indices_space =
		    BitpackingPrimitives::GetRequiredSize(state.tuple_count + 1, state.dictionary_indices_width);
	}

	idx_t required_space = 0;
	required_space += sizeof(dict_fsst_compression_header_t);
	required_space = AlignValue<idx_t>(required_space);
	required_space += state.dictionary_offset;
	if (APPEND_STATE == DictionaryAppendState::ENCODED) {
		required_space += state.to_encode_string_sum;
	}
	required_space = AlignValue<idx_t>(required_space);
	if (IsEncoded(APPEND_STATE)) {
		required_space += state.symbol_table_size;
		required_space = AlignValue<idx_t>(required_space);
	}
	required_space += state.string_lengths_space;
	required_space = AlignValue<idx_t>(required_space);
	required_space += new_dictionary_indices_space;

	idx_t available_space = state.info.GetBlockSize();
	if (APPEND_STATE == DictionaryAppendState::REGULAR) {
		available_space -= DictFSSTCompression::FSST_SYMBOL_TABLE_SIZE;
	}
	if (required_space > available_space) {
		if (fail_on_no_space) {
			throw FatalException("AddLookup in DictFSST failed: required: %d, available: %d, indices: %d, bitpacking: "
			                     "%b, dict offset: %d, str length: %d",
			                     required_space, available_space, new_dictionary_indices_space, get_bitpacking_size,
			                     state.dictionary_offset, state.string_lengths_space);
		}
		return false;
	}

	if (recalculate_indices_space) {
		state.dictionary_indices_space = new_dictionary_indices_space;
	}
	// Exists in the dictionary, add it
	state.dictionary_indices.push_back(UnsafeNumericCast<uint32_t>(lookup));
	return true;
}

template <DictionaryAppendState APPEND_STATE>
static inline bool AddToDictionary(DictFSSTCompressionState &state, const string_t &str,
                                   const bool recalculate_indices_space, bool fail_on_no_space) {
	uint32_t str_len = UnsafeNumericCast<uint32_t>(str.GetSize());
	if (APPEND_STATE == DictionaryAppendState::ENCODED) {
		//! We delay encoding of new entries.
		//  Encoding can increase the size of the string by 2x max, so we prepare for this worst case scenario.
		str_len *= 2;
	}

	const bool requires_higher_strlen_bitwidth = RequiresHigherBitWidth(state.string_lengths_width, str_len);
	const bool requires_higher_indices_bitwidth =
	    RequiresHigherBitWidth(state.dictionary_indices_width, state.dict_count);
	// We round the required size up to bitpacking group sizes anyways, so we only have to recalculate every 32 values
	const bool recalculate_strlen_space =
	    (state.dict_count % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE) == 0;

	//! String Lengths
	bitpacking_width_t new_string_lengths_width = state.string_lengths_width;
	idx_t new_string_lengths_space = state.string_lengths_space;
	if (requires_higher_strlen_bitwidth) {
		new_string_lengths_width = BitpackingPrimitives::MinimumBitWidth(str_len);
	}
	if (requires_higher_strlen_bitwidth || recalculate_strlen_space) {
		new_string_lengths_space =
		    BitpackingPrimitives::GetRequiredSize(state.dict_count + 1, new_string_lengths_width);
	}

	//! Dictionary Indices
	bitpacking_width_t new_dictionary_indices_width = state.dictionary_indices_width;
	idx_t new_dictionary_indices_space = state.dictionary_indices_space;
	if (APPEND_STATE != DictionaryAppendState::ENCODED_ALL_UNIQUE) {
		if (requires_higher_indices_bitwidth) {
			new_dictionary_indices_width = BitpackingPrimitives::MinimumBitWidth(state.dict_count);
		}
		if (requires_higher_indices_bitwidth || recalculate_indices_space) {
			new_dictionary_indices_space =
			    BitpackingPrimitives::GetRequiredSize(state.tuple_count + 1, new_dictionary_indices_width);
		}
	}

	idx_t required_space = 0;
	required_space += sizeof(dict_fsst_compression_header_t);
	required_space = AlignValue<idx_t>(required_space);
	required_space += state.dictionary_offset + str_len;
	if (APPEND_STATE == DictionaryAppendState::ENCODED) {
		if (!state.to_encode_string_sum) {
			//! As specified in fsst.h
			required_space += 7;
		}
		required_space += state.to_encode_string_sum;
	}
	required_space = AlignValue<idx_t>(required_space);
	if (IsEncoded(APPEND_STATE)) {
		required_space += state.symbol_table_size;
		required_space = AlignValue<idx_t>(required_space);
	}
	required_space += new_string_lengths_space;
	required_space = AlignValue<idx_t>(required_space);
	required_space += new_dictionary_indices_space;

	idx_t available_space = state.info.GetBlockSize();
	if (APPEND_STATE == DictionaryAppendState::REGULAR) {
		available_space -= DictFSSTCompression::FSST_SYMBOL_TABLE_SIZE;
	}
	if (required_space > available_space) {
		if (fail_on_no_space) {
			throw FatalException("AddToDictionary in DictFSST failed: required: %d, available: %d, dict offset + "
			                     "str_len: %d, new str length: %d, new dict indices: %d",
			                     required_space, available_space, state.dictionary_offset + str_len,
			                     new_string_lengths_space, new_dictionary_indices_space);
		}
		return false;
	}

	// Add it to the dictionary
	state.dictionary_indices.push_back(state.dict_count);
	if (APPEND_STATE == DictionaryAppendState::ENCODED) {
		state.dictionary_encoding_buffer.push_back(state.uncompressed_dictionary_copy.AddBlob(str));
		if (!state.to_encode_string_sum) {
			//! As specified in fsst.h
			state.to_encode_string_sum = 7;
		}
		state.to_encode_string_sum += str_len;
		auto &uncompressed_string = state.dictionary_encoding_buffer.back();
		state.current_string_map.Insert(uncompressed_string);
	} else {
		state.string_lengths.push_back(str_len);
		auto baseptr =
		    AlignPointer<sizeof(data_ptr_t)>(state.handle.GetDataMutable() + sizeof(dict_fsst_compression_header_t));
		memcpy(baseptr + state.dictionary_offset, str.GetData(), str_len);
		string_t dictionary_string((const char *)(baseptr + state.dictionary_offset), str_len); // NOLINT
		state.dictionary_offset += str_len;
		state.current_string_map.Insert(dictionary_string);
	}
	state.dict_count++;

	//! Update the state for serializing the dictionary_indices + string_lengths
	if (requires_higher_strlen_bitwidth) {
		state.string_lengths_width = new_string_lengths_width;
	}
	if (requires_higher_strlen_bitwidth || recalculate_strlen_space) {
		state.string_lengths_space = new_string_lengths_space;
		D_ASSERT(state.string_lengths_space != 0);
	}

	if (requires_higher_indices_bitwidth) {
		state.dictionary_indices_width = new_dictionary_indices_width;
	}
	if (requires_higher_indices_bitwidth || recalculate_indices_space) {
		state.dictionary_indices_space = new_dictionary_indices_space;
	}
	return true;
}

static DictFSSTCompressResult GetCompressResult(bool success, bool new_string) {
	if (!success) {
		return DictFSSTCompressResult::FAILED;
	}
	return new_string ? DictFSSTCompressResult::NEW_STRING : DictFSSTCompressResult::REPEATED_STRING;
}

static void VerifyFSSTCompressedString(void *decoder, const string_t &compressed_string,
                                       const string_t &uncompressed_string, vector<unsigned char> &decompress_buffer) {
	decompress_buffer.resize(uncompressed_string.GetSize() + 1 + 100);
	auto decoded_std_string = FSSTPrimitives::DecompressValue(decoder, compressed_string.GetData(),
	                                                          compressed_string.GetSize(), decompress_buffer);

	if (decoded_std_string.size() != uncompressed_string.GetSize()) {
		throw InternalException("FSST compression verification failed: decompressed string length mismatch");
	}
	string_t decompressed_string(reinterpret_cast<const char *>(decompress_buffer.data()),
	                             UnsafeNumericCast<uint32_t>(uncompressed_string.GetSize()));
	if (decompressed_string != uncompressed_string) {
		throw InternalException("FSST compression verification failed: decompressed string mismatch");
	}
}

DictFSSTCompressResult DictFSSTCompressionState::CompressInternal(UnifiedVectorFormat &vector_format,
                                                                  const string_t &str, bool is_null,
                                                                  EncodedInput &encoded_input, const idx_t i,
                                                                  idx_t count, bool fail_on_no_space) {
	auto strings = UnifiedVectorFormat::GetData<string_t>(vector_format);
	idx_t lookup = DConstants::INVALID_INDEX;

	//! In GetRequiredSize we will round up to ALGORITHM_GROUP_SIZE anyways
	//  so we can avoid recalculating for every tuple
	const bool recalculate_indices_space =
	    append_state == DictionaryAppendState::ENCODED_ALL_UNIQUE
	        ? false
	        : ((tuple_count % BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE) == 0);

	if (append_state == DictionaryAppendState::ENCODED_ALL_UNIQUE || is_null) {
		lookup = 0;
	} else {
		auto it = current_string_map.Lookup(str);
		lookup = it.IsEmpty() ? DConstants::INVALID_INDEX : it.index + 1;
	}

	switch (append_state) {
	case DictionaryAppendState::NOT_ENCODED:
	case DictionaryAppendState::REGULAR: {
		if (append_state == DictionaryAppendState::REGULAR) {
			if (lookup != DConstants::INVALID_INDEX) {
				return GetCompressResult(AddLookup<DictionaryAppendState::REGULAR>(
				                             *this, lookup, recalculate_indices_space, fail_on_no_space),
				                         false);
			} else {
				// This string does not exist in the dictionary, add it
				return GetCompressResult(AddToDictionary<DictionaryAppendState::REGULAR>(
				                             *this, str, recalculate_indices_space, fail_on_no_space),
				                         true);
			}
		} else {
			if (lookup != DConstants::INVALID_INDEX) {
				return GetCompressResult(AddLookup<DictionaryAppendState::NOT_ENCODED>(
				                             *this, lookup, recalculate_indices_space, fail_on_no_space),
				                         false);
			} else {
				// This string does not exist in the dictionary, add it
				return GetCompressResult(AddToDictionary<DictionaryAppendState::NOT_ENCODED>(
				                             *this, str, recalculate_indices_space, fail_on_no_space),
				                         true);
			}
		}
	}
	case DictionaryAppendState::ENCODED: {
		// Don't encode the input, the 'current_string_map' is not encoded.
		// encoding of the dictionary is done lazily
		// we optimize for the case where the strings are *already* in the dictionary

		bool fits;
		if (lookup != DConstants::INVALID_INDEX) {
			fits =
			    AddLookup<DictionaryAppendState::ENCODED>(*this, lookup, recalculate_indices_space, fail_on_no_space);
		} else {
			//! Not in the dictionary, add it
			fits = AddToDictionary<DictionaryAppendState::ENCODED>(*this, str, recalculate_indices_space,
			                                                       fail_on_no_space);
		}
		if (fits) {
			return GetCompressResult(true, lookup == DConstants::INVALID_INDEX);
		}
		if (dictionary_encoding_buffer.empty()) {
			//! The string doesn't fit, there are no strings left in the encoding buffer that could potentially
			//  reduce the space used enough to store this string.
			return DictFSSTCompressResult::FAILED;
		}

		// We lazily encode the new entries, if we're full but have entries in the buffer
		// we flush these and try again to see if the size went down enough
		FlushEncodingBuffer();
		if (lookup != DConstants::INVALID_INDEX) {
			return GetCompressResult(
			    AddLookup<DictionaryAppendState::ENCODED>(*this, lookup, recalculate_indices_space, fail_on_no_space),
			    false);
		} else {
			//! Not in the dictionary, add it
			return GetCompressResult(AddToDictionary<DictionaryAppendState::ENCODED>(
			                             *this, str, recalculate_indices_space, fail_on_no_space),
			                         true);
		}
	}
	case DictionaryAppendState::ENCODED_ALL_UNIQUE: {
		// Encode the input upfront, the 'current_string_map' is also encoded.
		// no lookups are performed, everything is added.

		duckdb_fsst_decoder_t temp_decoder_storage;
		void *temp_decoder = nullptr;
		vector<unsigned char> decompress_buffer;
		if (verify_compression) {
			temp_decoder = &temp_decoder_storage;
			duckdb_fsst_import(&temp_decoder_storage, fsst_serialized_symbol_table.get(),
			                   sizeof(duckdb_fsst_decoder_t));
		}

		if (encoded_input.data.empty()) {
			encoded_input.offset = i;
			vector<unsigned char *> input_string_ptrs;
			vector<size_t> input_string_lengths;
			idx_t total_size = 0;
			for (idx_t j = i; j < count; j++) {
				auto index = vector_format.sel->get_index(j);
#ifdef DEBUG
				//! We only choose FSST_ONLY if the rowgroup doesn't contain any nulls
				D_ASSERT(vector_format.validity.RowIsValid(index));
#endif
				auto &to_encode = strings[index];
				input_string_ptrs.push_back((unsigned char *)to_encode.GetData()); // NOLINT
				input_string_lengths.push_back(to_encode.GetSize());
				total_size += to_encode.GetSize();
			}

			size_t output_buffer_size = 7 + (2 * total_size); // size as specified in fsst.h
			auto compressed_ptrs = vector<unsigned char *>(input_string_lengths.size(), nullptr);
			auto compressed_sizes = vector<size_t>(input_string_lengths.size(), 0);
			if (output_buffer_size > encoding_buffer_size) {
				encoding_buffer = make_unsafe_uniq_array_uninitialized<unsigned char>(output_buffer_size);
				encoding_buffer_size = output_buffer_size;
			}

			// FIXME: can we compress directly to the segment? that would save a copy
			// I think yes?
			// We can give the segment as destination, and limit the size
			// it will tell us when it can't fit everything
			// worst case we can just check if the rest of the metadata fits when we remove the last string that it was
			// able to encode I believe 'duckdb_fsst_compress' tells us how many of the input strings it was able to
			// compress We can work backwards from there to see how many strings actually fit (probably worst case ret-1
			// ??)
			auto fsst_encoder = reinterpret_cast<duckdb_fsst_encoder_t *>(encoder);
			auto res = duckdb_fsst_compress(fsst_encoder, input_string_lengths.size(), input_string_lengths.data(),
			                                input_string_ptrs.data(), output_buffer_size, encoding_buffer.get(),
			                                compressed_sizes.data(), compressed_ptrs.data());
			if (res != input_string_lengths.size()) {
				throw FatalException("FSST compression failed to compress all input strings");
			}

			for (idx_t j = 0; j < input_string_lengths.size(); j++) {
				uint32_t size = UnsafeNumericCast<uint32_t>(compressed_sizes[j]);
				string_t encoded_string((const char *)compressed_ptrs[j], size); // NOLINT;

				if (verify_compression) {
					VerifyFSSTCompressedString(temp_decoder, encoded_string, strings[encoded_input.offset + j],
					                           decompress_buffer);
				}

				encoded_input.data.push_back(encoded_string);
			}
		}

		if (verify_compression) {
			for (idx_t j = encoded_input.offset; j < count; j++) {
				VerifyFSSTCompressedString(temp_decoder, encoded_input.data[j - encoded_input.offset], strings[j],
				                           decompress_buffer);
			}
		}
		auto &string = encoded_input.data[i - encoded_input.offset];
		return GetCompressResult(AddToDictionary<DictionaryAppendState::ENCODED_ALL_UNIQUE>(
		                             *this, string, recalculate_indices_space, fail_on_no_space),
		                         true);
	}
	};
	throw InternalException("Unreachable");
}

bool DictFSSTCompressionState::AllUnique() const {
	//! 1 is added for NULL always
	return string_lengths.size() - 1 == tuple_count;
}

DictionaryAppendState DictFSSTCompressionState::TryEncode() {
	//! We were appending normally, the segment is full

	if (dictionary_offset < DICTIONARY_ENCODE_THRESHOLD) {
		return DictionaryAppendState::NOT_ENCODED;
	}

	DictionaryAppendState new_state;
	if (!analyze->contains_nulls && AllUnique()) {
		new_state = DictionaryAppendState::ENCODED_ALL_UNIQUE;
	} else {
		new_state = DictionaryAppendState::ENCODED;
	}

	vector<size_t> fsst_string_sizes;
	vector<unsigned char *> fsst_string_ptrs;

	uint32_t offset = 0;
	data_ptr_t dictionary_start =
	    AlignPointer<sizeof(void *)>(handle.GetDataMutable() + sizeof(dict_fsst_compression_header_t));
	D_ASSERT(dictionary_offset > string_t::INLINE_BYTES && dictionary_offset <= string_t::MAX_STRING_SIZE);
	auto dict_copy = uncompressed_dictionary_copy.EmptyString(dictionary_offset);
	memcpy((void *)dict_copy.GetData(), (void *)dictionary_start, dictionary_offset);
	auto uncompressed_start = dict_copy.GetData();
	// Skip index 0, that's reserved for NULL
	for (idx_t i = 1; i < string_lengths.size(); i++) {
		auto length = string_lengths[i];
		auto start = uncompressed_start + offset;
		fsst_string_sizes.push_back(length);
		fsst_string_ptrs.push_back((unsigned char *)start); // NOLINT
		offset += length;
	}
	D_ASSERT(offset == dictionary_offset);

	// Create the encoder
	auto string_count = string_lengths.size() - 1;
	encoder = reinterpret_cast<void *>(
	    duckdb_fsst_create(string_count, fsst_string_sizes.data(), fsst_string_ptrs.data(), 0));
	auto fsst_encoder = reinterpret_cast<duckdb_fsst_encoder_t *>(encoder);

	auto compressed_ptrs = vector<unsigned char *>(string_count, nullptr);
	auto compressed_sizes = vector<size_t>(string_count, 0);

	// Compress the dictionary, straight to the segment
	auto res = duckdb_fsst_compress(fsst_encoder, string_count, fsst_string_sizes.data(), fsst_string_ptrs.data(),
	                                dictionary_offset, (unsigned char *)dictionary_start, compressed_sizes.data(),
	                                compressed_ptrs.data());

	bool can_use_encoding = true;
	idx_t new_size = 0;
	bitpacking_width_t new_string_lengths_width;
	idx_t new_string_lengths_space;
	//! Check that the compressed dictionary can fit on the block
	do {
		if (res != string_count) {
			can_use_encoding = false;
			break;
		}
		uint32_t max_length = 0;
		for (idx_t i = 0; i < string_count; i++) {
			auto str_len = UnsafeNumericCast<uint32_t>(compressed_sizes[i]);
			new_size += str_len;
			if (str_len > max_length) {
				max_length = str_len;
			}
		}
		if (new_size + DICTIONARY_ENCODE_THRESHOLD > dictionary_offset) {
			can_use_encoding = false;
			break;
		}

		// Export the symbol table, so we get an accurate measurement of the size
		if (!fsst_serialized_symbol_table) {
			fsst_serialized_symbol_table =
			    make_unsafe_uniq_array_uninitialized<unsigned char>(sizeof(duckdb_fsst_decoder_t));
		}
		symbol_table_size = duckdb_fsst_export(fsst_encoder, fsst_serialized_symbol_table.get());

		new_string_lengths_width = BitpackingPrimitives::MinimumBitWidth(max_length);
		new_string_lengths_space = BitpackingPrimitives::GetRequiredSize(dict_count, new_string_lengths_width);

		idx_t required_space = 0;
		required_space += sizeof(dict_fsst_compression_header_t);
		required_space = AlignValue<idx_t>(required_space);
		required_space += new_size;
		required_space = AlignValue<idx_t>(required_space);
		required_space += symbol_table_size;
		required_space = AlignValue<idx_t>(required_space);
		required_space += new_string_lengths_space;
		required_space = AlignValue<idx_t>(required_space);
		required_space += dictionary_indices_space;

		if (required_space > info.GetBlockSize()) {
			can_use_encoding = false;
			break;
		}

	} while (false);

	if (!can_use_encoding) {
		// The dictionary does not compress well enough to use FSST
		// continue filling the remaining bytes without encoding

		// We compressed directly to the segment, in the hopes this would fit and decrease in size
		// which it sadly didn't, so now we need to undo a bunch of things

		memcpy(dictionary_start, dict_copy.GetData(), dictionary_offset);
		uncompressed_dictionary_copy.Destroy();
		duckdb_fsst_destroy(fsst_encoder);
		encoder = nullptr;
		symbol_table_size = DConstants::INVALID_INDEX;
		return DictionaryAppendState::NOT_ENCODED;
	}

	if (new_state == DictionaryAppendState::ENCODED_ALL_UNIQUE) {
		//! We omit the selection buffer in this mode, setting the width to 0 makes the RequiredSpace result not include
		//! the selection buffer space.
		dictionary_indices_width = 0;
		dictionary_indices_space = 0;
	}

#ifdef DEBUG
	auto temp_decoder = alloca(sizeof(duckdb_fsst_decoder_t));
	duckdb_fsst_import((duckdb_fsst_decoder_t *)temp_decoder, fsst_serialized_symbol_table.get(),
	                   sizeof(duckdb_fsst_decoder_t));

	vector<unsigned char> decompress_buffer;
#endif

	// Rewrite the dictionary
	current_string_map.Clear();
	if (new_state == DictionaryAppendState::ENCODED) {
		offset = 0;
		auto uncompressed_dictionary_ptr = dict_copy.GetData();
		for (idx_t i = 0; i < string_count; i++) {
			auto size = UnsafeNumericCast<uint32_t>(compressed_sizes[i]);
			// Skip index 0, reserved for NULL
			uint32_t dictionary_index = UnsafeNumericCast<uint32_t>(i + 1);
			auto uncompressed_str_len = string_lengths[dictionary_index];

			string_t dictionary_string(uncompressed_dictionary_ptr + offset, uncompressed_str_len);
			current_string_map.Insert(dictionary_string);

#ifdef DEBUG
			//! Verify that we can decompress the string
			decompress_buffer.resize(uncompressed_str_len + 1 + 100);
			FSSTPrimitives::DecompressValue((void *)temp_decoder, (const char *)compressed_ptrs[i],
			                                (idx_t)compressed_sizes[i], decompress_buffer);

			string_t decompressed_string((const char *)decompress_buffer.data(), uncompressed_str_len);
			D_ASSERT(decompressed_string == dictionary_string);
#endif

			string_lengths[dictionary_index] = size;
			offset += uncompressed_str_len;
		}
	} else {
		D_ASSERT(new_state == DictionaryAppendState::ENCODED_ALL_UNIQUE);
		for (idx_t i = 0; i < string_count; i++) {
			auto &start = compressed_ptrs[i];
			auto size = UnsafeNumericCast<uint32_t>(compressed_sizes[i]);
			// Skip index 0, reserved for NULL
			uint32_t dictionary_index = UnsafeNumericCast<uint32_t>(i + 1);
			string_lengths[dictionary_index] = size;
			string_t dictionary_string((const char *)start, UnsafeNumericCast<uint32_t>(size)); // NOLINT

			current_string_map.Insert(dictionary_string);
		}
	}
	dictionary_offset = new_size;
	string_lengths_width = new_string_lengths_width;
	string_lengths_space = new_string_lengths_space;
	real_string_lengths_width = string_lengths_width;
	return new_state;
}

void DictFSSTCompressionState::Compress(const Vector &scan_vector) {
	UnifiedVectorFormat vector_format;
	scan_vector.ToUnifiedFormat(vector_format);
	auto strings = UnifiedVectorFormat::GetData<string_t>(vector_format);

	const auto count = scan_vector.size();
	EncodedInput encoded_input;
	for (idx_t i = 0; i < count; i++) {
		auto idx = vector_format.sel->get_index(i);
		auto &str = strings[idx];
		auto is_null = !vector_format.validity.RowIsValid(idx);
		DictFSSTCompressResult compress_result;
		do {
			compress_result = CompressInternal(vector_format, str, is_null, encoded_input, i, count, false);
			if (compress_result != DictFSSTCompressResult::FAILED) {
				break;
			}

			if (append_state == DictionaryAppendState::REGULAR) {
				append_state = TryEncode();
				D_ASSERT(append_state != DictionaryAppendState::REGULAR);
				compress_result = CompressInternal(vector_format, str, is_null, encoded_input, i, count, false);
				if (compress_result != DictFSSTCompressResult::FAILED) {
					break;
				}
			}
			Flush(false);
			encoded_input.data.clear();
			encoded_input.offset = 0;
			compress_result = CompressInternal(vector_format, str, is_null, encoded_input, i, count, true);
			if (compress_result == DictFSSTCompressResult::FAILED) {
				throw FatalException("Compressing directly after Flush doesn't fit - expected to throw earlier!");
			}
		} while (false);
		if (!is_null) {
			if (compress_result == DictFSSTCompressResult::REPEATED_STRING) {
				stats_writer.UpdateRepeated(str);
			} else {
				stats_writer.Update(str);
			}
		} else {
			stats_writer.SetHasNull();
		}
		tuple_count++;
	}
}

void DictFSSTCompressionState::FinalizeCompress() {
	Flush(true);
}

} // namespace dict_fsst
} // namespace duckdb
