//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/stl_allocator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/helper.hpp"

#include <limits>

namespace duckdb {

//! An STL-compatible allocator that forwards to a DuckDB Allocator, e.g. the BufferAllocator so that the memory of a
//! standard container is tracked by the buffer manager.
template <class T>
class stl_allocator { // NOLINT: match stl case
public:
	//! Typedefs
	typedef T value_type;
	typedef std::size_t size_type;
	typedef std::ptrdiff_t difference_type;
	typedef value_type &reference;
	typedef value_type const &const_reference;
	typedef value_type *pointer;
	typedef value_type const *const_pointer;

	//! Propagation traits
	using propagate_on_container_copy_assignment = std::true_type;
	using propagate_on_container_move_assignment = std::true_type;
	using propagate_on_container_swap = std::true_type;
	using is_always_equal = std::false_type;

	//! Rebind
	template <class U>
	struct rebind {
		using other = stl_allocator<U>;
	};

public:
	stl_allocator(Allocator &allocator_p) noexcept // NOLINT: allow implicit conversion
	    : allocator(allocator_p) {
	}
	template <class U>
	stl_allocator(const stl_allocator<U> &other) noexcept // NOLINT: allow implicit conversion
	    : allocator(other.GetAllocator()) {
	}

public:
	pointer allocate(size_type n) { // NOLINT: match stl case
		return reinterpret_cast<pointer>(allocator.get().AllocateData(n * sizeof(T)));
	}

	void deallocate(pointer p, size_type n) noexcept { // NOLINT: match stl case
		allocator.get().FreeData(reinterpret_cast<data_ptr_t>(p), n * sizeof(T));
	}

	template <class U, class... Args>
	void construct(U *p, Args &&...args) { // NOLINT: match stl case
		::new (p) U(std::forward<Args>(args)...);
	}

	template <class U>
	void destroy(U *p) noexcept { // NOLINT: match stl case
		p->~U();
	}

	size_type max_size() const noexcept { // NOLINT: match stl case
		return std::numeric_limits<size_type>::max() / sizeof(T);
	}

public:
	Allocator &GetAllocator() const noexcept {
		return allocator.get();
	}

	bool operator==(const stl_allocator &other) const noexcept {
		return RefersToSameObject(allocator.get(), other.allocator.get());
	}
	bool operator!=(const stl_allocator &other) const noexcept {
		return !(*this == other);
	}

private:
	//! Need to use std::reference_wrapper because "reference" is already a typedef
	std::reference_wrapper<Allocator> allocator;
};

} // namespace duckdb
