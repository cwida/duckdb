//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/arrow/arrow_buffer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

struct ArrowSchema;

namespace duckdb {

struct ArrowBuffer {
	static constexpr const idx_t MINIMUM_SHRINK_SIZE = 4096;

	ArrowBuffer() : dataptr(nullptr), count(0), capacity(0) {
	}
	~ArrowBuffer() {
		if (!dataptr) {
			return;
		}
		free(dataptr);
		dataptr = nullptr;
		count = 0;
		capacity = 0;
	}
	// disable copy constructors
	ArrowBuffer(const ArrowBuffer &other) = delete;
	ArrowBuffer &operator=(const ArrowBuffer &) = delete;
	//! enable move constructors
	ArrowBuffer(ArrowBuffer &&other) noexcept : count(0), capacity(0) {
		std::swap(dataptr, other.dataptr);
		std::swap(count, other.count);
		std::swap(capacity, other.capacity);
	}
	ArrowBuffer &operator=(ArrowBuffer &&other) noexcept {
		std::swap(dataptr, other.dataptr);
		std::swap(count, other.count);
		std::swap(capacity, other.capacity);
		return *this;
	}

	void reserve(idx_t bytes) { // NOLINT
		auto new_capacity = NextPowerOfTwo(bytes);
		if (new_capacity <= capacity) {
			return;
		}
		ReserveInternal(new_capacity);
	}

	void resize(idx_t bytes) { // NOLINT
		reserve(bytes);
		count = bytes;
	}

	void resize(idx_t bytes, data_t value) { // NOLINT
		reserve(bytes);
		for (idx_t i = count; i < bytes; i++) {
			dataptr[i] = value;
		}
		count = bytes;
	}

	template <class T>
	void push_back(T value) {
		reserve(sizeof(T) * (count + 1));
		reinterpret_cast<T *>(dataptr)[count] = value;
		count++;
	}

	idx_t size() const { // NOLINT
		return count;
	}

	data_ptr_t data() { // NOLINT
		return dataptr;
	}

	template <class T>
	T *GetData() {
		return reinterpret_cast<T *>(data());
	}

private:
	void ReserveInternal(idx_t bytes) {
		data_ptr_t new_ptr;
		if (dataptr) {
			new_ptr = data_ptr_cast(realloc(dataptr, bytes));
		} else {
			new_ptr = data_ptr_cast(malloc(bytes));
		}
		if (!new_ptr) {
			throw OutOfMemoryException("ArrowBuffer: failed to allocate %llu bytes", bytes);
		}
		dataptr = new_ptr;
		capacity = bytes;
	}

private:
	data_ptr_t dataptr = nullptr;
	idx_t count = 0;
	idx_t capacity = 0;
};

} // namespace duckdb
