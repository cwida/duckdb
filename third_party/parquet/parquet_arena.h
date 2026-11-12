#pragma once

#include "duckdb/storage/arena_allocator.hpp"
#include "duckdb/common/vector.hpp"
#include "thrift/protocol/TProtocol.h"

#include <functional>
#include <scoped_allocator>
#include <string>

namespace duckdb_parquet {

//! STL allocator that allocates from an ArenaAllocator. Memory is released when the arena is destroyed.
//! Containers never propagate the allocator on assignment/swap, so assigning between two objects that live
//! in different arenas copies the data into the arena of the assignment target.
template <class T>
class ParquetArenaAllocator {
public:
	using value_type = T;
	using propagate_on_container_copy_assignment = std::false_type;
	using propagate_on_container_move_assignment = std::false_type;
	using propagate_on_container_swap = std::false_type;
	using is_always_equal = std::false_type;

	template <class U>
	struct rebind {
		using other = ParquetArenaAllocator<U>;
	};

public:
	ParquetArenaAllocator(duckdb::ArenaAllocator &arena_p) noexcept : arena(arena_p) { // NOLINT: implicit conversion
	}
	template <class U>
	ParquetArenaAllocator(const ParquetArenaAllocator<U> &other) noexcept // NOLINT: implicit conversion
	    : arena(other.GetArena()) {
	}

	T *allocate(std::size_t n) { // NOLINT: match stl case
		if (n > static_cast<std::size_t>(-1) / sizeof(T)) {
			throw std::bad_array_new_length();
		}
		arena.get().AlignNext();
		return reinterpret_cast<T *>(arena.get().Allocate(n * sizeof(T)));
	}
	void deallocate(T *, std::size_t) noexcept { // NOLINT: match stl case
	}

	duckdb::ArenaAllocator &GetArena() const {
		return arena.get();
	}

	template <class U>
	bool operator==(const ParquetArenaAllocator<U> &other) const noexcept {
		return &arena.get() == &other.GetArena();
	}
	template <class U>
	bool operator!=(const ParquetArenaAllocator<U> &other) const noexcept {
		return !(*this == other);
	}

private:
	std::reference_wrapper<duckdb::ArenaAllocator> arena;
};

//! Nested containers/strings/structs are constructed with the arena of their parent
template <class T>
using ParquetAllocator = std::scoped_allocator_adaptor<ParquetArenaAllocator<T>>;

//! std::string that allocates from the arena - implicitly converts to std::string to keep the call sites simple
class ParquetString : public std::basic_string<char, std::char_traits<char>, ParquetAllocator<char>> {
public:
	using base = std::basic_string<char, std::char_traits<char>, ParquetAllocator<char>>;
	using base::base;
	using base::operator=;

	ParquetString &operator=(const std::string &value) {
		assign(value.data(), value.size());
		return *this;
	}
	operator std::string() const { // NOLINT: allow implicit conversion
		return std::string(data(), size());
	}
};

template <class T>
using ParquetVector = duckdb::vector<T, true, ParquetAllocator<T>>;

class ColumnChunk;
class SchemaElement;
using ColumnChunkList = ParquetVector<ColumnChunk>;
using SchemaElementList = ParquetVector<SchemaElement>;

//! Thrift protocols read/write std::string - these helpers bridge to the arena strings
inline uint32_t ReadParquetString(duckdb_apache::thrift::protocol::TProtocol *iprot, ParquetString &result) {
	std::string tmp;
	auto xfer = iprot->readString(tmp);
	result.assign(tmp.data(), tmp.size());
	return xfer;
}

inline uint32_t ReadParquetBinary(duckdb_apache::thrift::protocol::TProtocol *iprot, ParquetString &result) {
	std::string tmp;
	auto xfer = iprot->readBinary(tmp);
	result.assign(tmp.data(), tmp.size());
	return xfer;
}

inline uint32_t WriteParquetString(duckdb_apache::thrift::protocol::TProtocol *oprot, const ParquetString &value) {
	return oprot->writeString(std::string(value.data(), value.size()));
}

inline uint32_t WriteParquetBinary(duckdb_apache::thrift::protocol::TProtocol *oprot, const ParquetString &value) {
	return oprot->writeBinary(std::string(value.data(), value.size()));
}

} // namespace duckdb_parquet
