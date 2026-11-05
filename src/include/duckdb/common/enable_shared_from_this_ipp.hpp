#pragma once

#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/weak_ptr_ipp.hpp"

namespace duckdb {

template <class T>
class enable_shared_from_this { // NOLINT: invalid case style
public:
	template <class U, bool SAFE>
	friend class shared_ptr;

private:
	mutable weak_ptr<T> __weak_this_; // NOLINT: __weak_this_ is reserved

protected:
	constexpr enable_shared_from_this() noexcept { // NOLINT(bugprone-crtp-constructor-accessibility)
	}
	enable_shared_from_this(enable_shared_from_this const &) noexcept { // NOLINT: not marked as explicit
	}
	enable_shared_from_this &operator=(enable_shared_from_this const &) noexcept {
		return *this;
	}
	~enable_shared_from_this() {
	}

public:
	shared_ptr<T> shared_from_this() { // NOLINT: invalid case style
		return shared_ptr<T>(__weak_this_);
	}
	shared_ptr<T const> shared_from_this() const { // NOLINT: invalid case style
		return shared_ptr<const T>(__weak_this_);
	}

#if _LIBCPP_STD_VER >= 17
	weak_ptr<T> weak_from_this() noexcept { // NOLINT: invalid case style
		return __weak_this_;
	}

	weak_ptr<const T> weak_from_this() const noexcept { // NOLINT: invalid case style
		return __weak_this_;
	}
#endif // _LIBCPP_STD_VER >= 17
};

} // namespace duckdb
