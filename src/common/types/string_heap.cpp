#include "duckdb/common/types/string_heap.hpp"

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/exception.hpp"
#include "utf8proc_wrapper.hpp"

#include <cstring>

namespace duckdb {

#define MINIMUM_HEAP_SIZE 4096

StringHeap::StringHeap() : tail(nullptr) {
}

string_t StringHeap::AddString(const char *data, idx_t len) {
	D_ASSERT(Utf8Proc::Analyze(data, len) != UnicodeType::INVALID);
	return AddBlob(data, len);
}

string_t StringHeap::AddString(const char *data) {
	return AddString(data, strlen(data));
}

string_t StringHeap::AddString(const string &data) {
	return AddString(data.c_str(), data.size());
}

string_t StringHeap::AddString(const string_t &data) {
	return AddString(data.GetDataUnsafe(), data.GetSize());
}

string_t StringHeap::AddBlob(const char *data, idx_t len) {
	auto insert_string = EmptyString(len);
	auto insert_pos = insert_string.GetDataWriteable();
	memcpy(insert_pos, data, len);
	insert_string.Finalize();
	return insert_string;
}

string_t StringHeap::EmptyString(idx_t len) {
	D_ASSERT(len >= string_t::INLINE_LENGTH);
	if (!chunk || chunk->current_position + len >= chunk->maximum_size) {
		// have to make a new entry
		auto new_chunk = make_unique<StringChunk>(MaxValue<idx_t>(len, MINIMUM_HEAP_SIZE));
		new_chunk->prev = move(chunk);
		chunk = move(new_chunk);
		if (!tail) {
			tail = chunk.get();
		}
	}
	auto insert_pos = chunk->data.get() + chunk->current_position;
	chunk->current_position += len;
	return string_t(insert_pos, len);
}

} // namespace duckdb
