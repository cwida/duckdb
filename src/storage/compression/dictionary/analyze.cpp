#include "duckdb/storage/compression/dictionary/analyze.hpp"

namespace duckdb {

DictionaryAnalyzeState::DictionaryAnalyzeState(BlockManager &block_manager)
    : AnalyzeState(block_manager), segment_count(0), current_tuple_count(0), current_unique_count(0),
      current_dict_size(0), heap(block_manager.buffer_manager.GetBufferAllocator()), current_width(0), next_width(0) {
}

bool DictionaryAnalyzeState::LookupString(string_t str) {
	return current_set.count(str);
}

void DictionaryAnalyzeState::AddNewString(string_t str) {
	current_tuple_count++;
	current_unique_count++;
	current_dict_size += str.GetSize();
	current_set.insert(heap.AddBlob(str));
	current_width = next_width;
}

void DictionaryAnalyzeState::AddLastLookup(string_t str) {
	current_tuple_count++;
}

void DictionaryAnalyzeState::AddNull() {
	current_tuple_count++;
}

bool DictionaryAnalyzeState::CalculateSpaceRequirements(bool new_string, idx_t string_size) {
	if (!new_string) {
		return DictionaryCompression::HasEnoughSpace(current_tuple_count + 1, current_unique_count, current_dict_size,
		                                             current_width, info.GetBlockSize());
	}
	next_width = BitpackingPrimitives::MinimumBitWidth(current_unique_count + 2); // 1 for null, one for new string
	return DictionaryCompression::HasEnoughSpace(current_tuple_count + 1, current_unique_count + 1,
	                                             current_dict_size + string_size, next_width, info.GetBlockSize());
}

void DictionaryAnalyzeState::Flush(bool final) {
	segment_count++;
	current_tuple_count = 0;
	max_unique_count_across_segments = MaxValue(max_unique_count_across_segments, current_unique_count);
	current_unique_count = 0;
	current_dict_size = 0;
	current_set.clear();
}
void DictionaryAnalyzeState::UpdateMaxUniqueCount() {
	max_unique_count_across_segments = MaxValue(max_unique_count_across_segments, current_unique_count);
}
void DictionaryAnalyzeState::Verify() {
}

} // namespace duckdb
