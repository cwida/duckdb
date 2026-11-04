#pragma once

#include "duckdb/common/compatible_with_ipp.hpp"

namespace duckdb {

template <typename T, bool SAFE = true>
class weak_ptr;

template <class T>
class enable_shared_from_this;

template <typename T, bool SAFE = true>
class shared_ptr { // NOLINT: invalid case style
public:
	using original = std::shared_ptr<T>;
	using element_type = typename original::element_type;
	using weak_type = weak_ptr<T, SAFE>;
#if defined(_LIBCPP_VERSION)
	// libc++ relocates elements of this type with memcpy when the typedef names the class itself
	using __trivially_relocatable = shared_ptr; // NOLINT: libc++ extension point
#endif

private:
	static inline void AssertNotNull(const bool null) {
#if defined(DUCKDB_DEBUG_NO_SAFETY) || defined(DUCKDB_CLANG_TIDY)
		return;
#else
		if (DUCKDB_UNLIKELY(null)) {
			ThrowNullSharedPtrDereference();
		}
#endif
	}

private:
	template <class U, bool SAFE_P>
	friend class weak_ptr;

	template <class U, bool SAFE_P>
	friend class shared_ptr;

	template <typename U, typename S>
	friend shared_ptr<S> shared_ptr_cast(shared_ptr<U> src); // NOLINT: invalid case style

private:
	original internal;

public:
	// Constructors
	shared_ptr() : internal() {
	}
	shared_ptr(std::nullptr_t) : internal(nullptr) { // NOLINT: not marked as explicit
	}

	// From raw pointer of type U convertible to T
	template <class U, typename std::enable_if<compatible_with_t<U, T>::value, int>::type = 0>
	explicit shared_ptr(U *ptr) : internal(ptr) {
		__enable_weak_this(internal.get(), internal.get());
	}
	// From raw pointer of type T with custom DELETER
	template <typename DELETER>
	shared_ptr(T *ptr, DELETER deleter) : internal(ptr, std::move(deleter)) {
		__enable_weak_this(internal.get(), internal.get());
	}
	// Aliasing constructor: shares ownership information with ref but contains ptr instead
	// When the created shared_ptr goes out of scope, it will call the DELETER of ref, will not delete ptr
	template <class U>
	shared_ptr(const shared_ptr<U> &ref, T *ptr) noexcept : internal(ref.internal, ptr) {
	}
#if _LIBCPP_STD_VER >= 20
	template <class U>
	shared_ptr(shared_ptr<U> &&ref, T *ptr) noexcept : internal(std::move(ref.internal), ptr) {
	}
#endif

	// Copy constructor, share ownership with ref
	template <class U, typename std::enable_if<compatible_with_t<U, T>::value, int>::type = 0>
	shared_ptr(const shared_ptr<U> &ref) noexcept : internal(ref.internal) { // NOLINT: not marked as explicit
	}
	shared_ptr(const shared_ptr &other) : internal(other.internal) { // NOLINT: not marked as explicit
	}
	// Move constructor, share ownership with ref
	template <class U, typename std::enable_if<compatible_with_t<U, T>::value, int>::type = 0>
#ifdef DUCKDB_CLANG_TIDY
	[[clang::reinitializes]]
#endif
	shared_ptr(shared_ptr<U> &&ref) noexcept // NOLINT: not marked as explicit
	    : internal(std::move(ref.internal)) {
	}
	// move constructor
#ifdef DUCKDB_CLANG_TIDY
	[[clang::reinitializes]]
#endif
	shared_ptr(shared_ptr<T, SAFE> &&other) noexcept
	    : internal(std::move(other.internal)) {
	}

	// Construct from std::shared_ptr
	explicit shared_ptr(std::shared_ptr<T> other) : internal(std::move(other)) {
		// FIXME: should we __enable_weak_this here?
		// *our* enable_shared_from_this hasn't initialized yet, so I think so?
		__enable_weak_this(internal.get(), internal.get());
	}

	// Construct from weak_ptr
	template <class U>
	explicit shared_ptr(const weak_ptr<U> &other) : internal(other.internal) {
	}

	// Construct from unique_ptr, takes over ownership of the unique_ptr
	template <class U, class DELETER, bool SAFE_P,
	          typename std::enable_if<compatible_with_t<U, T>::value &&
	                                      std::is_convertible<typename unique_ptr<U, DELETER>::pointer, T *>::value,
	                                  int>::type = 0>
#ifdef DUCKDB_CLANG_TIDY
	[[clang::reinitializes]]
#endif
	shared_ptr(unique_ptr<U, DELETER, SAFE_P> &&other) // NOLINT: not marked as explicit
	    : internal(std::move(other)) {
		__enable_weak_this(internal.get(), internal.get());
	}

	// Destructor
	~shared_ptr() = default;

	// Assign from shared_ptr copy
	shared_ptr<T, SAFE> &operator=(const shared_ptr<T, SAFE> &other) noexcept {
		if (this == &other) {
			return *this;
		}
		// Create a new shared_ptr using the copy constructor, then swap out the ownership to *this
		shared_ptr(other).swap(*this);
		return *this;
	}
	template <class U, typename std::enable_if<compatible_with_t<U, T>::value, int>::type = 0>
	shared_ptr<T> &operator=(const shared_ptr<U> &other) {
		shared_ptr(other).swap(*this);
		return *this;
	}

	// Assign from moved shared_ptr
	shared_ptr<T, SAFE> &operator=(shared_ptr &&other) noexcept {
		// Create a new shared_ptr using the move constructor, then swap out the ownership to *this
		shared_ptr(std::move(other)).swap(*this);
		return *this;
	}
	template <class U, typename std::enable_if<compatible_with_t<U, T>::value, int>::type = 0>
	shared_ptr<T, SAFE> &operator=(shared_ptr<U> &&other) {
		shared_ptr(std::move(other)).swap(*this);
		return *this;
	}

	// Assign from moved unique_ptr
	template <class U, class DELETER, bool SAFE_P,
	          typename std::enable_if<compatible_with_t<U, T>::value &&
	                                      std::is_convertible<typename unique_ptr<U, DELETER>::pointer, T *>::value,
	                                  int>::type = 0>
	shared_ptr<T, SAFE> &operator=(unique_ptr<U, DELETER, SAFE_P> &&ref) {
		shared_ptr(std::move(ref)).swap(*this);
		return *this;
	}

#ifdef DUCKDB_CLANG_TIDY
	[[clang::reinitializes]]
#endif
	void
	reset() { // NOLINT: invalid case style
		internal.reset();
	}
	template <typename U>
#ifdef DUCKDB_CLANG_TIDY
	[[clang::reinitializes]]
#endif
	void
	reset(U *ptr) { // NOLINT: invalid case style
		internal.reset(ptr);
	}
	template <typename U, typename DELETER>
#ifdef DUCKDB_CLANG_TIDY
	[[clang::reinitializes]]
#endif
	void
	reset(U *ptr, DELETER deleter) { // NOLINT: invalid case style
		internal.reset(ptr, deleter);
	}

	void swap(shared_ptr &r) noexcept { // NOLINT: invalid case style
		internal.swap(r.internal);
	}

	T *get() const { // NOLINT: invalid case style
		return internal.get();
	}

	long use_count() const { // NOLINT: invalid case style
		return internal.use_count();
	}

	explicit operator bool() const noexcept {
		return internal.operator bool();
	}

	typename std::add_lvalue_reference<T>::type operator*() const {
		if (MemorySafety<SAFE>::ENABLED) {
			const auto ptr = internal.get();
			AssertNotNull(!ptr);
			return *ptr;
		} else {
			return *internal;
		}
	}

	T *operator->() const {
		if (MemorySafety<SAFE>::ENABLED) {
			const auto ptr = internal.get();
			AssertNotNull(!ptr);
			return ptr;
		} else {
			return internal.operator->();
		}
	}

	// Relational operators
	template <typename U>
	bool operator==(const shared_ptr<U> &other) const noexcept {
		return internal == other.internal;
	}
	template <typename U>
	bool operator!=(const shared_ptr<U> &other) const noexcept {
		return internal != other.internal;
	}

	bool operator==(std::nullptr_t) const noexcept {
		return internal == nullptr;
	}
	bool operator!=(std::nullptr_t) const noexcept {
		return internal != nullptr;
	}

	template <typename U>
	bool operator<(const shared_ptr<U> &other) const noexcept {
		return internal < other.internal;
	}
	template <typename U>
	bool operator<=(const shared_ptr<U> &other) const noexcept {
		return internal <= other.internal;
	}
	template <typename U>
	bool operator>(const shared_ptr<U> &other) const noexcept {
		return internal > other.internal;
	}
	template <typename U>
	bool operator>=(const shared_ptr<U> &other) const noexcept {
		return internal >= other.internal;
	}

	shared_ptr<T, SAFE> atomic_load() const {
		return shared_ptr<T, SAFE>(std::atomic_load(&internal));
	}

	shared_ptr<T, SAFE> atomic_load(std::memory_order order) const {
		return shared_ptr<T, SAFE>(std::atomic_load_explicit(&internal, order));
	}

	void atomic_store(const shared_ptr<T, SAFE> &new_ptr) {
		std::atomic_store(&internal, new_ptr.internal);
	}

private:
	// This overload is used when the class inherits from 'enable_shared_from_this<U>'
	template <class U, class V,
	          typename std::enable_if<std::is_convertible<V *, const enable_shared_from_this<U> *>::value,
	                                  int>::type = 0>
	void __enable_weak_this(const enable_shared_from_this<U> *object, // NOLINT: invalid case style
	                        V *ptr) noexcept {
		typedef typename std::remove_cv<U>::type non_const_u_t;
		if (object && object->__weak_this_.expired()) {
			// __weak_this__ is the mutable variable returned by 'shared_from_this'
			// it is initialized here
			auto non_const = const_cast<non_const_u_t *>(static_cast<const U *>(ptr)); // NOLINT: const cast
			object->__weak_this_ = shared_ptr<non_const_u_t>(*this, non_const);
		}
	}

	void __enable_weak_this(...) noexcept { // NOLINT: invalid case style
	}
};

template <typename T>
using unsafe_shared_ptr = shared_ptr<T, false>;

} // namespace duckdb
