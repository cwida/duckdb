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
#include "duckdb/storage/arena_allocator.hpp"

namespace duckdb {
//! A string heap is the owner of a set of strings, strings can be inserted into
//! it On every insert, a pointer to the inserted string is returned The
//! returned pointer will remain valid until the StringHeap is destroyed
class StringHeap {
public:
	DUCKDB_API explicit StringHeap(Allocator &allocator = Allocator::DefaultAllocator());

	DUCKDB_API void Destroy();
	DUCKDB_API void Move(StringHeap &other);

	inline string_t AddString(const char *data, idx_t len) {
		D_ASSERT(Value::StringIsValid(data, len));
		return AddBlob(data, len);
	}

	inline string_t AddString(const char *data) {
		return AddString(data, strlen(data));
	}

	inline string_t AddString(const string &data) {
		return AddString(data.c_str(), data.size());
	}

	inline string_t AddString(const string_t &data) {
		D_ASSERT(Value::StringIsValid(data.GetData(), data.GetSize()));
		return AddBlob(data);
	}

	inline string_t AddBlob(const char *data, idx_t len) {
		if (len <= string_t::INLINE_LENGTH) {
			return string_t(data, UnsafeNumericCast<uint32_t>(len));
		}
		return AddBlobToHeap(data, len);
	}

	inline string_t AddBlob(const string_t &data) {
		auto len = data.GetSize();
		if (len <= string_t::INLINE_LENGTH) {
			return data;
		}
		return AddBlobToHeap(data.GetData(), len);
	}

	inline string_t EmptyString(idx_t len) {
		if (len <= string_t::INLINE_LENGTH) {
			return string_t(UnsafeNumericCast<uint32_t>(len));
		}
		return CreateEmptyStringInHeap(len);
	}

	inline string_t CreateEmptyStringInHeap(idx_t len) {
		D_ASSERT(len > string_t::INLINE_LENGTH);
		if (len > string_t::MAX_STRING_SIZE) {
			throw OutOfRangeException(
			    "Cannot create a string of size: '%d', the maximum supported string size is: '%d'", len,
			    string_t::MAX_STRING_SIZE);
		}
		// The caller fills the bytes and finalizes the prefix.
		string_t result(UnsafeNumericCast<uint32_t>(len));
		result.SetPointer(char_ptr_cast(allocator.Allocate(len)));
		return result;
	}

	inline string_t AddBlobToHeap(const char *data, idx_t len) {
		D_ASSERT(len > string_t::INLINE_LENGTH);
		auto insert_string = CreateEmptyStringInHeap(len);
		auto insert_pos = insert_string.GetDataWriteable();
		memcpy(insert_pos, data, len);
		insert_string.Finalize();
		return insert_string;
	}

	//! Size of strings
	DUCKDB_API idx_t SizeInBytes() const;
	//! Total allocation size (cached)
	DUCKDB_API idx_t AllocationSize() const;

	DUCKDB_API ArenaAllocator &GetAllocator() {
		return allocator;
	}

private:
private:
	ArenaAllocator allocator;
};

} // namespace duckdb
