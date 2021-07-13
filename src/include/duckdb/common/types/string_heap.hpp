//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/string_heap.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {
//! A string heap is the owner of a set of strings, strings can be inserted into
//! it On every insert, a pointer to the inserted string is returned The
//! returned pointer will remain valid until the StringHeap is destroyed
class StringHeap {
public:
	StringHeap();

	void Destroy() {
		tail = nullptr;
		chunk = nullptr;
	}

	void Move(StringHeap &other) {
		D_ASSERT(!other.chunk);
		other.tail = tail;
		other.chunk = move(chunk);
		tail = nullptr;
	}

	//! Add a string to the string heap, returns a pointer to the string
	string_t AddString(const char *data, idx_t len);
	//! Add a string to the string heap, returns a pointer to the string
	string_t AddString(const char *data);
	//! Add a string to the string heap, returns a pointer to the string
	string_t AddString(const string &data);
	//! Add a string to the string heap, returns a pointer to the string
	string_t AddString(const string_t &data);
	//! Add a blob to the string heap; blobs can be non-valid UTF8
	string_t AddBlob(const char *data, idx_t len);
	//! Allocates space for an empty string of size "len" on the heap
	string_t EmptyString(idx_t len);

private:
	struct StringChunk {
		explicit StringChunk(idx_t size) : current_position(0), maximum_size(size) {
			data = unique_ptr<char[]>(new char[maximum_size]);
		}
		~StringChunk() {
			if (prev) {
				auto current_prev = move(prev);
				while (current_prev) {
					current_prev = move(current_prev->prev);
				}
			}
		}

		unique_ptr<char[]> data;
		idx_t current_position;
		idx_t maximum_size;
		unique_ptr<StringChunk> prev;
	};
	StringChunk *tail;
	unique_ptr<StringChunk> chunk;
};

} // namespace duckdb
