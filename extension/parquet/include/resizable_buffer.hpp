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

#include "duckdb/common/enums/memory_tag.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

class ByteBuffer { // on to the 10 thousandth impl
public:
	ByteBuffer() {};
	ByteBuffer(const data_ptr_t ptr, const idx_t len) : ptr(ptr), len(len) {};

public:
	data_ptr_t GetCurrentLoc() const {
		return ptr + offset;
	}

	idx_t GetOffset() const {
		return offset;
	}

	idx_t GetLength() const {
		return len;
	}

	idx_t GetRemaining() const {
		return len - offset;
	}

	data_ptr_t ConsumeRemaining(idx_t &length_out) {
		length_out = GetRemaining();
		auto loc = GetCurrentLoc();
		UnsafeInc(length_out);
		return loc;
	}

	data_ptr_t GetPtrAt(const idx_t rel_offset) const {
		return ptr + rel_offset;
	}

	void Inc(const idx_t increment) {
		Available(increment);
		UnsafeInc(increment);
	}

	void UnsafeInc(const idx_t increment) {
		offset += increment;
	}

	template <class T>
	T Read() {
		Available(sizeof(T));
		return UnsafeRead<T>();
	}

	template <class T>
	T UnsafeRead() {
		T val = UnsafeGet<T>();
		UnsafeInc(sizeof(T));
		return val;
	}

	template <class T>
	T Get() {
		Available(sizeof(T));
		return UnsafeGet<T>();
	}

	template <class T>
	T UnsafeGet() {
		return Load<T>(ptr + offset);
	}

	void CopyTo(char *dest, const idx_t copy_len) const {
		Available(copy_len);
		UnsafeCopyTo(dest, copy_len);
	}

	void UnsafeCopyTo(char *dest, const idx_t copy_len) const {
		std::memcpy(dest, ptr + offset, copy_len);
	}

	void Zero() const {
		std::memset(ptr + offset, 0, len - offset);
	}

	void Available(const idx_t req_len) const {
		if (!CheckAvailable(req_len)) {
			throw std::runtime_error("Out of buffer");
		}
	}

	bool CheckAvailable(const idx_t req_len) const {
		return req_len <= len - offset;
	}

	void Rebase(const data_ptr_t new_ptr) {
		ptr = new_ptr;
	}

	//! Shrink the readable length without touching the allocation.
	void Truncate(const idx_t new_len) {
		D_ASSERT(new_len <= len);
		len = new_len;
	}

protected:
	data_ptr_t ptr = nullptr;

	idx_t offset = 0;
	idx_t len = 0;
};

class ResizeableBuffer : public ByteBuffer {
public:
	ResizeableBuffer() {
	}

	ResizeableBuffer(Allocator &allocator, const idx_t new_size) {
		Resize(allocator, new_size);
	}

	ResizeableBuffer(BufferManager &buffer_manager, const idx_t new_size) {
		Resize(buffer_manager, new_size);
	}

	void Resize(Allocator &allocator, const idx_t new_size) {
		if (PrepareResize(new_size)) {
			allocated_data.Reset(); // Have to reset before allocating new buffer (otherwise we use ~2x the memory)
			allocated_data = allocator.Allocate(alloc_len);
			ptr = allocated_data.get();
		}
	}

	void Resize(BufferManager &buffer_manager, const idx_t new_size) {
		// Unpinned reuse is overwritten anyway; allocate fresh instead of reloading a spilled block.
		if (PrepareResize(new_size) || (new_size > 0 && block_handle && !handle.IsValid())) {
			handle.Destroy();
			block_handle.reset(); // release before allocating, so old and new aren't counted at once
			handle = buffer_manager.Allocate(MemoryTag::PARQUET_READER, alloc_len, false);
			block_handle = handle.GetBlockHandle();
			ptr = handle.GetDataMutable();
		}
	}

	void Pin(BufferManager &buffer_manager) {
		if (block_handle) {
			handle = buffer_manager.Pin(block_handle);
			ptr = handle.GetDataMutable();
		}
	}

	void Unpin() {
		if (block_handle) {
			handle.Destroy();
			ptr = nullptr;
		}
	}

	//! RAII pin, like std::lock_guard: construct it as a named local to keep the buffer pinned for that scope.
	class PinGuard {
	public:
		PinGuard(ResizeableBuffer &buffer, BufferManager &buffer_manager) : buffer(buffer) {
			buffer.Pin(buffer_manager);
		}
		~PinGuard() {
			buffer.Unpin();
		}
		PinGuard(const PinGuard &) = delete;
		PinGuard &operator=(const PinGuard &) = delete;

	private:
		ResizeableBuffer &buffer;
	};

	shared_ptr<BlockHandle> &GetBlockHandle() {
		return block_handle;
	}

private:
	//! Sets the new length and reports whether a larger allocation is needed.
	bool PrepareResize(const idx_t new_size) {
		len = new_size;
		offset = 0;
		if (new_size <= alloc_len || new_size == 0) {
			return false;
		}
		alloc_len = NextPowerOfTwo(new_size);
		return true;
	}

	AllocatedData allocated_data;
	shared_ptr<BlockHandle> block_handle;
	BufferHandle handle;

	idx_t alloc_len = 0;
};

} // namespace duckdb
