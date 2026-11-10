
#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/likely.hpp"
#include "duckdb/common/memory_safety.hpp"
#include "duckdb/original/std/memory.hpp"

#include <type_traits>

namespace duckdb {

template <class DATA_TYPE, class DELETER = std::default_delete<DATA_TYPE>, bool SAFE = true>
class unique_ptr : public duckdb_base_std::unique_ptr<DATA_TYPE, DELETER> { // NOLINT: naming
public:
	using original = duckdb_base_std::unique_ptr<DATA_TYPE, DELETER>;
	using original::original; // NOLINT
	using pointer = typename original::pointer;
#if defined(_LIBCPP_VERSION)
	// libc++ relocates elements of this type with memcpy when the typedef names the class itself; the standard
	// unique_ptr is trivially relocatable when its deleter is, which for the deleters in use means trivially copyable
	using __trivially_relocatable = // NOLINT: libc++ extension point
	    typename std::conditional<std::is_trivially_copyable<DELETER>::value, unique_ptr, void>::type;
#endif

private:
	static inline void AssertNotNull(const bool null) {
#if defined(DUCKDB_DEBUG_NO_SAFETY) || defined(DUCKDB_CLANG_TIDY)
		return;
#else
		if (DUCKDB_UNLIKELY(null)) {
			ThrowNullUniquePtrDereference();
		}
#endif
	}

public:
	typename std::add_lvalue_reference<DATA_TYPE>::type operator*() const { // NOLINT: hiding on purpose
		const auto ptr = original::get();
		if (MemorySafety<SAFE>::ENABLED) {
			AssertNotNull(!ptr);
		}
		return *ptr;
	}

	typename original::pointer operator->() const { // NOLINT: hiding on purpose
		const auto ptr = original::get();
		if (MemorySafety<SAFE>::ENABLED) {
			AssertNotNull(!ptr);
		}
		return ptr;
	}

#ifdef DUCKDB_CLANG_TIDY
	// This is necessary to tell clang-tidy that it reinitializes the variable after a move
	[[clang::reinitializes]]
#endif
	inline void
	reset(typename original::pointer ptr = typename original::pointer()) noexcept { // NOLINT: hiding on purpose
		original::reset(ptr);
	}
};

template <class DATA_TYPE, class DELETER>
class unique_ptr<DATA_TYPE[], DELETER, true> : public duckdb_base_std::unique_ptr<DATA_TYPE[], DELETER> {
public:
	using original = duckdb_base_std::unique_ptr<DATA_TYPE[], DELETER>;
	using original::original;
#if defined(_LIBCPP_VERSION)
	// libc++ relocates elements of this type with memcpy when the typedef names the class itself; the standard
	// unique_ptr is trivially relocatable when its deleter is, which for the deleters in use means trivially copyable
	using __trivially_relocatable = // NOLINT: libc++ extension point
	    typename std::conditional<std::is_trivially_copyable<DELETER>::value, unique_ptr, void>::type;
#endif

private:
	static inline void AssertNotNull(const bool null) {
#if defined(DUCKDB_DEBUG_NO_SAFETY) || defined(DUCKDB_CLANG_TIDY)
		return;
#else
		if (DUCKDB_UNLIKELY(null)) {
			ThrowNullUniquePtrDereference();
		}
#endif
	}

public:
	typename std::add_lvalue_reference<DATA_TYPE>::type operator[](size_t __i) const { // NOLINT: hiding on purpose
		const auto ptr = original::get();
		if (MemorySafety<true>::ENABLED) {
			AssertNotNull(!ptr);
		}
		return ptr[__i];
	}
};

template <class DATA_TYPE, class DELETER, bool SAFE>
class unique_ptr<DATA_TYPE[], DELETER, SAFE> : public duckdb_base_std::unique_ptr<DATA_TYPE[], DELETER> {
public:
	using original = duckdb_base_std::unique_ptr<DATA_TYPE[], DELETER>;
	using original::original;
#if defined(_LIBCPP_VERSION)
	// libc++ relocates elements of this type with memcpy when the typedef names the class itself; the standard
	// unique_ptr is trivially relocatable when its deleter is, which for the deleters in use means trivially copyable
	using __trivially_relocatable = // NOLINT: libc++ extension point
	    typename std::conditional<std::is_trivially_copyable<DELETER>::value, unique_ptr, void>::type;
#endif

private:
	static inline void AssertNotNull(const bool null) {
#if defined(DUCKDB_DEBUG_NO_SAFETY) || defined(DUCKDB_CLANG_TIDY)
		return;
#else
		if (DUCKDB_UNLIKELY(null)) {
			ThrowNullUniquePtrDereference();
		}
#endif
	}

public:
	typename std::add_lvalue_reference<DATA_TYPE>::type operator[](size_t __i) const { // NOLINT: hiding on purpose
		const auto ptr = original::get();
		if (MemorySafety<SAFE>::ENABLED) {
			AssertNotNull(!ptr);
		}
		return ptr[__i];
	}
};

template <typename T>
using unique_array = unique_ptr<T[], std::default_delete<T[]>, true>;

template <typename T>
using unsafe_unique_array = unique_ptr<T[], std::default_delete<T[]>, false>;

template <typename T>
using unsafe_unique_ptr = unique_ptr<T, std::default_delete<T>, false>;

} // namespace duckdb
