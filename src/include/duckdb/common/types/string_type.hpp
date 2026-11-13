//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/string_type.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/assert.hpp"
#include "duckdb/common/bswap.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/hash.hpp"

#include <cstring>
#include <algorithm>

namespace duckdb {

struct string_t {
	friend struct StringComparisonOperators;

public:
	static constexpr idx_t PREFIX_BYTES = 4 * sizeof(char);
	static constexpr idx_t INLINE_BYTES = 12 * sizeof(char);
	static constexpr idx_t HEADER_SIZE = sizeof(uint32_t) + PREFIX_BYTES;
	static constexpr idx_t MAX_STRING_SIZE = NumericLimits<uint32_t>::Maximum();
#ifndef DUCKDB_DEBUG_NO_INLINE
	static constexpr idx_t PREFIX_LENGTH = PREFIX_BYTES;
	static constexpr idx_t INLINE_LENGTH = INLINE_BYTES;
#else
	static constexpr idx_t PREFIX_LENGTH = 0;
	static constexpr idx_t INLINE_LENGTH = 0;
#endif

	string_t() = default;
	explicit string_t(uint32_t len) {
		value.inlined.length = len;
		memset(value.inlined.inlined, 0, INLINE_BYTES);
	}
	string_t(const char *data, uint32_t len) {
		value.inlined.length = len;
		D_ASSERT(data || GetSize() == 0);
		if (IsInlined()) {
			// zero initialize the prefix first
			// this makes sure that strings with length smaller than 4 still have an equal prefix
			memset(value.inlined.inlined, 0, INLINE_BYTES);
			if (GetSize() == 0) {
				return;
			}
			// small string: inlined
			memcpy(value.inlined.inlined, data, GetSize());
		} else {
			// large string: store pointer
#ifndef DUCKDB_DEBUG_NO_INLINE
			memcpy(value.pointer.prefix, data, PREFIX_LENGTH);
#else
			memset(value.pointer.prefix, 0, PREFIX_BYTES);
#endif
			value.pointer.ptr = (char *)data; // NOLINT
		}
	}

	string_t(const char *data) // NOLINT: Allow implicit conversion from `const char*`
	    : string_t(data, UnsafeNumericCast<uint32_t>(strlen(data))) {
	}
	string_t(const string &value) // NOLINT: Allow implicit conversion from `const char*`
	    : string_t(value.c_str(), UnsafeNumericCast<uint32_t>(value.size())) {
	}

	bool IsInlined() const {
		return GetSize() <= INLINE_LENGTH;
	}

	const char *GetData() const {
		return IsInlined() ? const_char_ptr_cast(value.inlined.inlined) : value.pointer.ptr;
	}
	const char *GetDataUnsafe() const {
		return GetData();
	}

	char *GetDataWriteable() const {
		return IsInlined() ? (char *)value.inlined.inlined : value.pointer.ptr; // NOLINT
	}

	const char *GetPrefix() const {
		return value.inlined.inlined;
	}

	char *GetPrefixWriteable() {
		return value.inlined.inlined;
	}

	uint32_t GetPrefixIntegerComparable() const {
#ifdef DUCKDB_DEBUG_NO_INLINE
		return 0;
#else
		return BSwapIfLE(Load<uint32_t>(const_data_ptr_cast(GetPrefix())));
#endif
	}

	idx_t GetSize() const {
		return value.inlined.length;
	}

	void SetSizeAndFinalize(uint32_t size, idx_t allocated_size) {
		value.inlined.length = size;
		if (allocated_size > INLINE_LENGTH && IsInlined()) {
			//! Data was written to the 'value.pointer.ptr', has to be copied to the inlined bytes
			D_ASSERT(value.pointer.ptr);
			auto ptr = value.pointer.ptr;
			memcpy(GetDataWriteable(), ptr, size);
		}
		Finalize();
	}

	bool Empty() const {
		return value.inlined.length == 0;
	}

	string GetString() const {
		return string(GetData(), GetSize());
	}

	explicit operator string() const {
		return GetString();
	}

	char *GetPointer() const {
		D_ASSERT(!IsInlined());
		return value.pointer.ptr;
	}

	void SetPointer(char *new_ptr) {
		D_ASSERT(!IsInlined());
		value.pointer.ptr = new_ptr;
	}

	void Finalize() {
		// set trailing NULL byte
		if (GetSize() <= INLINE_LENGTH) {
			// fill prefix with zeros if the length is smaller than the prefix length
			memset(value.inlined.inlined + GetSize(), 0, INLINE_BYTES - GetSize());
		} else {
			// copy the data into the prefix
#ifndef DUCKDB_DEBUG_NO_INLINE
			auto dataptr = GetData();
			memcpy(value.pointer.prefix, dataptr, PREFIX_LENGTH);
#else
			memset(value.pointer.prefix, 0, PREFIX_BYTES);
#endif
		}
	}

	void Verify() const;
	//! Verify also in release mode
	void ForceVerify() const;
	void VerifyUTF8() const;
	void VerifyCharacters() const;

	struct StringComparisonOperators {
		static inline bool Equals(const string_t &a, const string_t &b) {
#ifdef DUCKDB_DEBUG_NO_INLINE
			if (a.GetSize() != b.GetSize()) {
				return false;
			}
			return (memcmp(a.GetData(), b.GetData(), a.GetSize()) == 0);
#endif
			uint64_t a_bulk_comp = Load<uint64_t>(const_data_ptr_cast(&a));
			uint64_t b_bulk_comp = Load<uint64_t>(const_data_ptr_cast(&b));
			if (a_bulk_comp != b_bulk_comp) {
				// Either length or prefix are different -> not equal
				return false;
			}
			// they have the same length and same prefix!
			a_bulk_comp = Load<uint64_t>(const_data_ptr_cast(&a) + 8u);
			b_bulk_comp = Load<uint64_t>(const_data_ptr_cast(&b) + 8u);
			if (a_bulk_comp == b_bulk_comp) {
				// either they are both inlined (so compare equal) or point to the same string (so compare equal)
				return true;
			}
			if (!a.IsInlined()) {
				// 'long' strings of the same length -> compare pointed value
				if (memcmp(a.value.pointer.ptr, b.value.pointer.ptr, a.GetSize()) == 0) {
					return true;
				}
			}
			// either they are short string of same length but different content
			//     or they point to string with different content
			//     either way, they can't represent the same underlying string
			return false;
		}
		// compare up to shared length. if still the same, compare lengths
		static bool GreaterThan(const string_t &left, const string_t &right) {
#ifndef DUCKDB_DEBUG_NO_INLINE
			// length and prefix share the first 8 bytes of a string_t: one load gives both
			const uint64_t a_head = Load<uint64_t>(const_data_ptr_cast(&left));
			const uint64_t b_head = Load<uint64_t>(const_data_ptr_cast(&right));
#if DUCKDB_IS_BIG_ENDIAN
			const uint32_t a_prefix = static_cast<uint32_t>(a_head);
			const uint32_t b_prefix = static_cast<uint32_t>(b_head);
			const uint32_t a_len = static_cast<uint32_t>(a_head >> 32);
			const uint32_t b_len = static_cast<uint32_t>(b_head >> 32);
#else
			const uint32_t a_prefix = BSwap(static_cast<uint32_t>(a_head >> 32));
			const uint32_t b_prefix = BSwap(static_cast<uint32_t>(b_head >> 32));
			const uint32_t a_len = static_cast<uint32_t>(a_head);
			const uint32_t b_len = static_cast<uint32_t>(b_head);
#endif
			if (a_prefix != b_prefix) {
				return a_prefix > b_prefix;
			}
			const uint32_t min_length = std::min<uint32_t>(a_len, b_len);
			if (min_length <= PREFIX_LENGTH) {
				// the shorter string lies entirely within the (equal) prefix
				return a_len > b_len;
			}
			// the second 8 bytes: the rest of an inlined string, or the pointer of a long one
			const uint64_t a_rest = Load<uint64_t>(const_data_ptr_cast(left.value.inlined.inlined) + PREFIX_BYTES);
			const uint64_t b_rest = Load<uint64_t>(const_data_ptr_cast(right.value.inlined.inlined) + PREFIX_BYTES);
			if (std::max<uint32_t>(a_len, b_len) <= INLINE_LENGTH) {
				if (a_rest != b_rest) {
					return BSwapIfLE(a_rest) > BSwapIfLE(b_rest);
				}
				return a_len > b_len;
			} else {
				// for a pointer string the loaded rest word is the pointer itself; no second load, no IsInlined()
				auto a_data = a_len > INLINE_LENGTH ? reinterpret_cast<const char *>(static_cast<uintptr_t>(a_rest))
				                                    : left.value.inlined.inlined;
				auto b_data = b_len > INLINE_LENGTH ? reinterpret_cast<const char *>(static_cast<uintptr_t>(b_rest))
				                                    : right.value.inlined.inlined;
				auto memcmp_res = memcmp(a_data + PREFIX_LENGTH, b_data + PREFIX_LENGTH, min_length - PREFIX_LENGTH);
				return memcmp_res > 0 || (memcmp_res == 0 && a_len > b_len);
			}
#else
			const uint32_t left_length = UnsafeNumericCast<uint32_t>(left.GetSize());
			const uint32_t right_length = UnsafeNumericCast<uint32_t>(right.GetSize());
			const uint32_t min_length = std::min<uint32_t>(left_length, right_length);
			auto memcmp_res = memcmp(left.GetData(), right.GetData(), min_length);
			return memcmp_res > 0 || (memcmp_res == 0 && left_length > right_length);
#endif
		}
	};

	bool operator==(const string_t &r) const {
		return StringComparisonOperators::Equals(*this, r);
	}

	bool operator!=(const string_t &r) const {
		return !(*this == r);
	}

	bool operator>(const string_t &r) const {
		return StringComparisonOperators::GreaterThan(*this, r);
	}
	bool operator<(const string_t &r) const {
		return r > *this;
	}
	bool operator<=(const string_t &r) const {
		return !(r < *this);
	}
	bool operator>=(const string_t &r) const {
		return !(*this < r);
	}

private:
	union {
		struct {
			uint32_t length;
			char prefix[4];
			char *ptr;
		} pointer;
		struct {
			uint32_t length;
			char inlined[12];
		} inlined;
	} value;
};

} // namespace duckdb

namespace std {
template <>
struct hash<duckdb::string_t> {
	size_t operator()(const duckdb::string_t &val) const {
		return Hash(val);
	}
};
} // namespace std
