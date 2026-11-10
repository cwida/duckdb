//===----------------------------------------------------------------------===//
//                         DuckDB
//
// resizable_buffer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/allocator.hpp"

#include <exception>

namespace duckdb {

class ByteBuffer { // on to the 10 thousandth impl
public:
	ByteBuffer() {};
	ByteBuffer(data_ptr_t ptr, uint64_t len) : ptr(ptr), len(len) {};

	data_ptr_t ptr = nullptr;
	uint64_t len = 0;

public:
	void inc(const uint64_t increment) {
		available(increment);
		unsafe_inc(increment);
	}

	void unsafe_inc(const uint64_t increment) {
		len -= increment;
		ptr += increment;
	}

	template <class T>
	T read() {
		available(sizeof(T));
		return unsafe_read<T>();
	}

	template <class T>
	T unsafe_read() {
		T val = unsafe_get<T>();
		unsafe_inc(sizeof(T));
		return val;
	}

	template <class T>
	T get() {
		available(sizeof(T));
		return unsafe_get<T>();
	}

	template <class T>
	T unsafe_get() {
		return Load<T>(ptr);
	}

	void copy_to(char *dest, const uint64_t len) const {
		available(len);
		unsafe_copy_to(dest, len);
	}

	void unsafe_copy_to(char *dest, const uint64_t len) const {
		std::memcpy(dest, ptr, len);
	}

	void zero() const {
		std::memset(ptr, 0, len);
	}

	void available(const uint64_t req_len) const {
		if (!check_available(req_len)) {
			throw std::runtime_error("Out of buffer");
		}
	}

	bool check_available(const uint64_t req_len) const {
		return req_len <= len;
	}
};

class ResizeableBuffer : public ByteBuffer {
public:
	ResizeableBuffer() {
	}
	ResizeableBuffer(Allocator &allocator, const uint64_t new_size) {
		resize(allocator, new_size);
	}
	void resize(Allocator &allocator, const uint64_t new_size) {
		len = new_size;
		if (new_size == 0) {
			return;
		}
		if (new_size > alloc_len) {
			alloc_len = NextPowerOfTwo(new_size);
			allocated_data.Reset(); // Have to reset before allocating new buffer (otherwise we use ~2x the memory)
			allocated_data = allocator.Allocate(alloc_len);
			ptr = allocated_data.get();
		}
	}
	void reset() {
		ptr = allocated_data.get();
		len = alloc_len;
	}
	idx_t GetCapacity() const {
		return alloc_len;
	}

private:
	AllocatedData allocated_data;
	idx_t alloc_len = 0;
};

} // namespace duckdb
