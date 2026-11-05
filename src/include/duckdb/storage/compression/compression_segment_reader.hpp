//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/compression/compression_segment_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array_ptr.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/likely.hpp"

#include <type_traits>

namespace duckdb {

class BufferHandle;
class ColumnSegment;

//! A non-owning, bounds-checked view over [0, size) of a block or page, with a byte position.
//! It validates physical byte ranges, not row or codec invariants.
class CompressionSegmentReader {
public:
	CompressionSegmentReader(const_data_ptr_t data, idx_t size, const char *context)
	    : data(data), size(size), position(0), context(context) {
	}

	//! Returns a reader over the recorded segment bytes, or the remaining block bytes for legacy segments.
	static CompressionSegmentReader FromSegment(const BufferHandle &handle, const ColumnSegment &segment,
	                                            const char *context);

public:
	idx_t Size() const {
		return size;
	}
	idx_t Position() const {
		return position;
	}
	idx_t Remaining() const {
		return size - position;
	}
	bool Finished() const {
		return position == size;
	}

	//! Loads one element at the current position and advances it.
	template <class T>
	T Read() {
		static_assert(std::is_trivially_copyable_v<T>, "Read element must be a trivially copyable data type");
		CheckForwardRead(sizeof(T));
		T result = Load<T>(data + position);
		position += sizeof(T);
		return result;
	}

	//! Loads one element before the current position and moves it backward.
	template <class T>
	T ReadBackward() {
		static_assert(std::is_trivially_copyable_v<T>, "ReadBackward element must be a trivially copyable data type");
		CheckBackwardRead(sizeof(T));
		position -= sizeof(T);
		return Load<T>(data + position);
	}

	//! Loads one element at offset without changing the position.
	template <class T>
	T Get(idx_t offset) const {
		static_assert(std::is_trivially_copyable_v<T>, "Get element must be a trivially copyable data type");
		CheckRange(offset, sizeof(T));
		return Load<T>(data + offset);
	}

	//! Returns a view over the next length bytes and advances the position.
	unsafe_array_ptr<const uint8_t> ReadBytes(idx_t length) {
		CheckForwardRead(length);
		unsafe_array_ptr<const uint8_t> result(data + position, length);
		position += length;
		return result;
	}

	//! Returns a view over the preceding length bytes and moves the position backward.
	unsafe_array_ptr<const uint8_t> ReadBytesBackward(idx_t length) {
		CheckBackwardRead(length);
		position -= length;
		return unsafe_array_ptr<const uint8_t>(data + position, length);
	}

	//! Returns a view over the next count aligned elements and advances the position.
	template <class T>
	unsafe_array_ptr<const T> ReadArray(idx_t count) {
		static_assert(std::is_trivially_copyable_v<T>, "ReadArray element must be a trivially copyable data type");
		auto result = GetArray<T>(position, count);
		position += count * sizeof(T);
		return result;
	}

	//! Returns a view over the preceding count aligned elements and moves the position backward.
	template <class T>
	unsafe_array_ptr<const T> ReadArrayBackward(idx_t count) {
		static_assert(std::is_trivially_copyable_v<T>,
		              "ReadArrayBackward element must be a trivially copyable data type");
		if (DUCKDB_UNLIKELY(count > position / sizeof(T))) {
			ThrowBackwardReadOutOfBounds();
		}
		auto offset = position - count * sizeof(T);
		auto result = GetArray<T>(offset, count);
		position = offset;
		return result;
	}

	//! Copies the next length bytes into destination and advances the position.
	//! The caller must ensure destination has capacity for length bytes.
	void ReadBytesInto(data_ptr_t destination, idx_t length) {
		auto source = ReadBytes(length);
		memcpy(destination, source.data(), length);
	}

	//! Copies the next length bytes into a fixed-size destination and advances the position.
	template <class T, idx_t N>
	void ReadBytesIntoArray(T (&destination)[N], idx_t length) {
		static_assert(std::is_trivially_copyable_v<T>,
		              "ReadBytesIntoArray element must be a trivially copyable data type");
		if (DUCKDB_UNLIKELY(length > sizeof(destination))) {
			ThrowDestinationTooSmall();
		}
		ReadBytesInto(data_ptr_cast(destination), length);
	}

	//! Copies the next count elements into a fixed-size destination and advances the position.
	template <class T, idx_t N>
	void ReadIntoArray(T (&destination)[N], idx_t count) {
		static_assert(std::is_trivially_copyable_v<T>, "ReadIntoArray element must be a trivially copyable data type");
		if (DUCKDB_UNLIKELY(count > N)) {
			ThrowDestinationTooSmall();
		}
		ReadBytesInto(data_ptr_cast(destination), count * sizeof(T));
	}

	//! Returns a view over the next length bytes aligned for T and advances the position.
	template <class T>
	unsafe_array_ptr<const uint8_t> ReadBytesAligned(idx_t length) {
		CheckForwardRead(length);
		CheckAlignment<T>(position);
		unsafe_array_ptr<const uint8_t> result(data + position, length);
		position += length;
		return result;
	}

	//! Returns a view over length bytes at offset without changing the position.
	unsafe_array_ptr<const uint8_t> GetBytes(idx_t offset, idx_t length) const {
		CheckRange(offset, length);
		return unsafe_array_ptr<const uint8_t>(data + offset, length);
	}

	//! Checks alignment at a byte offset, without validating its range or moving the position.
	template <class T>
	void CheckAlignment(idx_t offset) const {
		// Use integer arithmetic to avoid forming a pointer outside the buffer.
		if (DUCKDB_UNLIKELY((reinterpret_cast<uintptr_t>(data) + offset) % alignof(T) != 0)) { // NOLINT
			ThrowArrayMisaligned();
		}
	}

	//! Validates that count aligned elements fit at byte offset.
	template <class T>
	void CheckArray(idx_t offset, idx_t count) const {
		static_assert(std::is_trivially_copyable_v<T>, "CheckArray element must be a trivially copyable data type");
		CheckRange(offset, 0);
		CheckAlignment<T>(offset);
		// Use division so an untrusted count cannot lead to overflow.
		if (DUCKDB_UNLIKELY(count > (size - offset) / sizeof(T))) {
			ThrowArrayOutOfBounds();
		}
	}

	//! Returns a view over count elements at byte offset.
	template <class T>
	unsafe_array_ptr<const T> GetArray(idx_t offset, idx_t count) const {
		return GetArraySlice<T>(offset, 0, count);
	}

	//! Returns a view over elements [start, start + count) in the array at byte offset.
	template <class T>
	unsafe_array_ptr<const T> GetArraySlice(idx_t offset, idx_t start, idx_t count) const {
		CheckArray<T>(offset, start);
		auto capacity = (size - offset) / sizeof(T);
		if (DUCKDB_UNLIKELY(count > capacity - start)) {
			ThrowArrayOutOfBounds();
		}
		return unsafe_array_ptr<const T>(reinterpret_cast<const T *>(data + offset) + start, count); // NOLINT
	}

	//! Loads the element at index after validating array_count elements at byte offset.
	template <class T>
	T GetArrayElement(idx_t offset, idx_t array_count, idx_t index) const {
		CheckArray<T>(offset, array_count);
		if (DUCKDB_UNLIKELY(index >= array_count)) {
			ThrowArrayOutOfBounds();
		}
		return Load<T>(data + offset + index * sizeof(T));
	}

	//! Returns a reader over the next length bytes and advances this reader's position.
	CompressionSegmentReader ReadSubReader(idx_t length, const char *sub_context) {
		CheckForwardRead(length);
		CompressionSegmentReader sub(data + position, length, sub_context);
		position += length;
		return sub;
	}

	//! Returns a reader over [offset, offset + length) without advancing this reader's position.
	CompressionSegmentReader GetSubReader(idx_t offset, idx_t length, const char *sub_context) const {
		CheckRange(offset, length);
		return CompressionSegmentReader(data + offset, length, sub_context);
	}

	//! Returns a reader over count elements at byte offset.
	template <class T>
	CompressionSegmentReader GetArraySubReader(idx_t offset, idx_t count, const char *sub_context) const {
		CheckArray<T>(offset, count);
		return GetSubReader(offset, count * sizeof(T), sub_context);
	}

	//! Advances the position by length bytes.
	void Skip(idx_t length) {
		CheckForwardRead(length);
		position += length;
	}

	void SetPosition(idx_t new_position) {
		CheckRange(new_position, 0);
		position = new_position;
	}

	//! Aligns the position relative to this reader's origin.
	//! Throws if alignment is zero or the padding exceeds the range.
	void Align(idx_t alignment);

	//! Aligns the position backward relative to this reader's origin.
	//! Throws if alignment is zero.
	void AlignBackward(idx_t alignment);

	//! Validates that length bytes fit at the current position without advancing it.
	void CheckForwardRead(idx_t length) const {
		if (DUCKDB_UNLIKELY(length > size - position)) {
			ThrowForwardReadOutOfBounds();
		}
	}

	//! Validates that length bytes fit before the current position without moving it.
	void CheckBackwardRead(idx_t length) const {
		if (DUCKDB_UNLIKELY(length > position)) {
			ThrowBackwardReadOutOfBounds();
		}
	}

	//! Validates that length bytes fit at offset without changing the position.
	void CheckRange(idx_t offset, idx_t length) const {
		if (DUCKDB_UNLIKELY(offset > size)) {
			ThrowOffsetOutOfBounds();
		}
		if (DUCKDB_UNLIKELY(length > size - offset)) {
			ThrowRangeOutOfBounds();
		}
	}

private:
	//! Keep exception construction out of bounds-checking paths.
	[[noreturn]] static void ThrowOffsetExceedsBlockSize(const char *context);
	[[noreturn]] static void ThrowByteSizeExceedsBlockSize(const char *context);
	[[noreturn]] void ThrowForwardReadOutOfBounds() const;
	[[noreturn]] void ThrowBackwardReadOutOfBounds() const;
	[[noreturn]] void ThrowOffsetOutOfBounds() const;
	[[noreturn]] void ThrowRangeOutOfBounds() const;
	[[noreturn]] void ThrowArrayMisaligned() const;
	[[noreturn]] void ThrowArrayOutOfBounds() const;
	[[noreturn]] void ThrowDestinationTooSmall() const;

private:
	const_data_ptr_t data;
	idx_t size;
	idx_t position;
	//! Short compressor/region label used in corruption exception messages, e.g. "RLE value stream".
	const char *context;
};

} // namespace duckdb
