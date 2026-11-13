//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/sorting/sort_key.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bswap.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/string_type.hpp"

namespace duckdb {

enum class SortKeyType : uint8_t {
	INVALID = 0,
	//! Without payload
	NO_PAYLOAD_FIXED_8 = 1,
	NO_PAYLOAD_FIXED_16 = 2,
	NO_PAYLOAD_FIXED_24 = 3,
	NO_PAYLOAD_FIXED_32 = 4,
	NO_PAYLOAD_VARIABLE_32 = 5,
	//! With payload (requires row pointer in key)
	PAYLOAD_FIXED_16 = 6,
	PAYLOAD_FIXED_24 = 7,
	PAYLOAD_FIXED_32 = 8,
	PAYLOAD_VARIABLE_32 = 9,
};

//! Expands MACRO once for every SortKeyType that TupleDataLayout can produce.
//! The entire sorting stack is templated on SortKeyType, so every entry here multiplies the
//! amount of code generated for sorting, merging, scattering/gathering and the range joins.
//! All nine together are ~623 KB of a Release build, ~70 KB each.
//!
//! The sort_key_layouts feature drops the three layouts that can be replaced by a wider one that
//! is already there: a key without payload rounds up to NO_PAYLOAD_FIXED_32, and a payload key
//! wider than 24 bytes falls back to PAYLOAD_VARIABLE_32. See TupleDataLayout::Initialize.
//!
//! What is left cannot go. NO_PAYLOAD_FIXED_8 and PAYLOAD_FIXED_16 are the BIGINT sort key, and
//! VariableSortKey::Construct(int64_t) throws. PAYLOAD_FIXED_24 is what an index build over an
//! 8-byte column uses (a 9-byte key plus the row pointer) and it is the most memory-sensitive sort
//! we do: widening it fails test_art_mem_limit.test, which builds one under a 10MB memory_limit.
//!
//! This trades real time for code size. Order By over 20M rows, trimmed vs not, against a 1.19x
//! noise band measured by running the same binary in both slots:
//!   ORDER BY 1x INTEGER  NO_PAYLOAD_FIXED_8  kept          1.06x
//!   ORDER BY 1x BIGINT   NO_PAYLOAD_FIXED_16 -> FIXED_32   1.56x
//!   ORDER BY 2x BIGINT   NO_PAYLOAD_FIXED_24 -> FIXED_32   1.26x
//!   ORDER BY 3x BIGINT   NO_PAYLOAD_FIXED_32 kept          1.03x
//! for 297 KB off the dylib, the same whether this feature is trimmed on its own or as part of a
//! full SMALLER_BINARY=1 build. Dropping NO_PAYLOAD_FIXED_32 and PAYLOAD_FIXED_24 as well, so that
//! everything collapses onto the variable-size key, saves ~90 KB more but costs 1.6-2.4x instead,
//! because a variable-size key compares through the heap. Re-measure before trimming further.
#if DUCKDB_SMALLER_BINARY(sort_key_layouts)
#define DUCKDB_FOR_EACH_SORT_KEY_TYPE(MACRO)                                                                           \
	MACRO(NO_PAYLOAD_FIXED_8)                                                                                          \
	MACRO(NO_PAYLOAD_FIXED_32)                                                                                         \
	MACRO(NO_PAYLOAD_VARIABLE_32)                                                                                      \
	MACRO(PAYLOAD_FIXED_16)                                                                                            \
	MACRO(PAYLOAD_FIXED_24)                                                                                            \
	MACRO(PAYLOAD_VARIABLE_32)
#else
#define DUCKDB_FOR_EACH_SORT_KEY_TYPE(MACRO)                                                                           \
	MACRO(NO_PAYLOAD_FIXED_8)                                                                                          \
	MACRO(NO_PAYLOAD_FIXED_16)                                                                                         \
	MACRO(NO_PAYLOAD_FIXED_24)                                                                                         \
	MACRO(NO_PAYLOAD_FIXED_32)                                                                                         \
	MACRO(NO_PAYLOAD_VARIABLE_32)                                                                                      \
	MACRO(PAYLOAD_FIXED_16)                                                                                            \
	MACRO(PAYLOAD_FIXED_24)                                                                                            \
	MACRO(PAYLOAD_FIXED_32)                                                                                            \
	MACRO(PAYLOAD_VARIABLE_32)
#endif

//! Same as above, restricted to the sort key types that carry a payload pointer
#if DUCKDB_SMALLER_BINARY(sort_key_layouts)
#define DUCKDB_FOR_EACH_PAYLOAD_SORT_KEY_TYPE(MACRO)                                                                   \
	MACRO(PAYLOAD_FIXED_16)                                                                                            \
	MACRO(PAYLOAD_FIXED_24)                                                                                            \
	MACRO(PAYLOAD_VARIABLE_32)
#else
#define DUCKDB_FOR_EACH_PAYLOAD_SORT_KEY_TYPE(MACRO)                                                                   \
	MACRO(PAYLOAD_FIXED_16)                                                                                            \
	MACRO(PAYLOAD_FIXED_24)                                                                                            \
	MACRO(PAYLOAD_FIXED_32)                                                                                            \
	MACRO(PAYLOAD_VARIABLE_32)
#endif

//! Same as above, restricted to the sort key types that need reordering when going external,
//! i.e. those that are not constant size, or that carry a payload pointer
#define DUCKDB_FOR_EACH_REORDERABLE_SORT_KEY_TYPE(MACRO)                                                               \
	MACRO(NO_PAYLOAD_VARIABLE_32)                                                                                      \
	DUCKDB_FOR_EACH_PAYLOAD_SORT_KEY_TYPE(MACRO)

//! Forces a pointer size of 8 bytes (even on 32-bit)
struct sort_key_ptr_t { // NOLINT: match stl case
	union {
		data_ptr_t ptr;
		uint64_t pad;
	} u;
};

template <SortKeyType>
struct SortKey;

template <class SORT_KEY>
struct SortKeyNoPayload {
protected:
	SortKeyNoPayload() = default; // NOLINT
	friend SORT_KEY;

public:
	static constexpr bool HAS_PAYLOAD = false;

	data_ptr_t GetPayload() const {
		throw InternalException("GetPayload() called on a SortKeyNoPayload");
	}

	void SetPayload(const data_ptr_t &payload) {
		throw InternalException("SetPayload() called on a SortKeyNoPayload");
	}
};

template <class SORT_KEY>
struct SortKeyPayload {
protected:
	SortKeyPayload() = default; // NOLINT
	friend SORT_KEY;

public:
	static constexpr bool HAS_PAYLOAD = true;

	data_ptr_t GetPayload() const {
		auto &sort_key = static_cast<const SORT_KEY &>(*this);
		return sort_key.payload.u.ptr;
	}

	void SetPayload(const data_ptr_t &payload) {
		auto &sort_key = static_cast<SORT_KEY &>(*this);
		sort_key.payload.u.ptr = payload;
	}
};

template <idx_t REMAINING>
static bool SortKeyLessThan(const uint64_t *const &lhs, const uint64_t *const &rhs) {
	return (*lhs < *rhs) || ((*lhs == *rhs) && SortKeyLessThan<REMAINING - 1>(lhs + 1, rhs + 1));
}

template <>
inline bool SortKeyLessThan<1>(const uint64_t *const &lhs, const uint64_t *const &rhs) {
	return *lhs < *rhs;
}

template <class SORT_KEY, bool HAS_PAYLOAD>
struct FixedSortKey : std::conditional<HAS_PAYLOAD, SortKeyPayload<SORT_KEY>, SortKeyNoPayload<SORT_KEY>>::type {
protected:
	FixedSortKey() = default; // NOLINT
	friend SORT_KEY;

public:
	static constexpr bool CONSTANT_SIZE = true;

	void ByteSwap() {
		auto &sort_key = static_cast<SORT_KEY &>(*this);
		for (idx_t i = 0; i < SORT_KEY::PARTS; i++) {
			(&sort_key.part0)[i] = BSwapIfLE((&sort_key.part0)[i]);
		}
	}

	void Construct(const string_t &str, data_ptr_t &) {
		D_ASSERT(str.GetSize() <= SORT_KEY::INLINE_LENGTH);
		auto &sort_key = static_cast<SORT_KEY &>(*this);
		for (idx_t i = 0; i < SORT_KEY::PARTS; i++) {
			(&sort_key.part0)[i] = 0;
		}

		if (SORT_KEY::INLINE_LENGTH <= string_t::INLINE_LENGTH) {
			memcpy(&sort_key.part0, str.GetPrefix(), SORT_KEY::INLINE_LENGTH);
		} else if (str.IsInlined()) {
			memcpy(&sort_key.part0, str.GetPrefix(), string_t::INLINE_LENGTH);
		} else {
			memcpy(&sort_key.part0, str.GetPointer(), str.GetSize());
		}

		// IMPORTANT NOTE: We don't actually store the data in byte-comparable order!
		// This allows us to do int64_t comparisons, yielding better performance.
		// This means we have to ByteSwap once more when decoding the keys later.
		ByteSwap();
	}

	void Construct(const int64_t &val, data_ptr_t &) {
		auto &sort_key = static_cast<SORT_KEY &>(*this);
		sort_key.part0 = static_cast<uint64_t>(val); // NOLINT: unsafe cast on purpose
	}

	void Deconstruct(string_t &val) {
		auto &sort_key = static_cast<SORT_KEY &>(*this);
		ByteSwap();
		val = string_t(const_char_ptr_cast(&sort_key.part0), UnsafeNumericCast<uint32_t>(SORT_KEY::INLINE_LENGTH));
	}

	void Deconstruct(int64_t &val) {
		auto &sort_key = static_cast<SORT_KEY &>(*this);
		val = static_cast<int64_t>(sort_key.part0); // NOLINT: unsafe cast on purpose
	}

	data_ptr_t GetData() const {
		throw InternalException("GetData() called on a FixedSortKey");
	}

	idx_t GetSize() const {
		throw InternalException("GetSize() called on a FixedSortKey");
	}

	idx_t GetHeapSize() const {
		throw InternalException("GetHeapSize() called on a FixedSortKey");
	}

	friend bool operator<(const SORT_KEY &lhs, const SORT_KEY &rhs) {
		return SortKeyLessThan<SORT_KEY::PARTS>(&lhs.part0, &rhs.part0);
	}
};

template <class SORT_KEY, bool HAS_PAYLOAD>
struct VariableSortKey : std::conditional<HAS_PAYLOAD, SortKeyPayload<SORT_KEY>, SortKeyNoPayload<SORT_KEY>>::type {
protected:
	VariableSortKey() = default; // NOLINT
	friend SORT_KEY;

public:
	static constexpr bool CONSTANT_SIZE = false;

	void ByteSwap() {
		auto &sort_key = static_cast<SORT_KEY &>(*this);
		for (idx_t i = 0; i < SORT_KEY::PARTS; i++) {
			(&sort_key.part0)[i] = BSwapIfLE((&sort_key.part0)[i]);
		}
	}

	void Construct(const string_t &val, data_ptr_t &heap_ptr) {
		auto &sort_key = static_cast<SORT_KEY &>(*this);
		// Initialize to 0's (including size by doing + 1)
		for (idx_t i = 0; i < SORT_KEY::PARTS + 1; i++) {
			(&sort_key.part0)[i] = 0;
		}

		// Deal with inlined part first
		memcpy(&sort_key.part0, val.GetData(), MinValue(val.GetSize(), SORT_KEY::INLINE_LENGTH));
		// Same as FixedSortKey, we do not store the data in byte-comparable order
		ByteSwap();

		// Deal with non-inlined part (if necessary)
		if (val.GetSize() > SORT_KEY::INLINE_CAPACITY) {
			sort_key.size = val.GetSize();
			sort_key.data.u.ptr = heap_ptr;
			memcpy(sort_key.data.u.ptr, val.GetData(), sort_key.size);
			heap_ptr += sort_key.size;
		} else {
			// The unused heap pointer holds a zero-padded tail. Keep the heap size at zero.
			sort_key.data.u.pad = 0;
			if (val.GetSize() > SORT_KEY::INLINE_LENGTH) {
				memcpy(&sort_key.data.u.pad, val.GetData() + SORT_KEY::INLINE_LENGTH,
				       val.GetSize() - SORT_KEY::INLINE_LENGTH);
			}
		}
	}

	void Construct(const int64_t &, data_ptr_t &) {
		throw InternalException("VariableSortKey::Construct() called with an int64_t");
	}

	void Deconstruct(string_t &val) {
		auto &sort_key = static_cast<SORT_KEY &>(*this);
		if (!IsInlined()) {
			val = string_t(const_char_ptr_cast(sort_key.data.u.ptr), UnsafeNumericCast<uint32_t>(sort_key.size));
		} else {
			ByteSwap();
			// Deconstruction already consumes a temporary key copy. Pack its tail over the now-unused size field.
			memcpy(data_ptr_cast(&sort_key.part0) + SORT_KEY::INLINE_LENGTH, &sort_key.data.u.pad, sizeof(uint64_t));
			val =
			    string_t(const_char_ptr_cast(&sort_key.part0), UnsafeNumericCast<uint32_t>(SORT_KEY::INLINE_CAPACITY));
		}
	}

	void Deconstruct(int64_t &) {
		throw InternalException("VariableSortKey::Deconstruct() called with an int64_t");
	}

	bool IsInlined() const {
		auto &sort_key = static_cast<const SORT_KEY &>(*this);
		return sort_key.size == 0;
	}

	data_ptr_t GetData() const {
		auto &sort_key = static_cast<const SORT_KEY &>(*this);
		return IsInlined() ? nullptr : sort_key.data.u.ptr;
	}

	const_data_ptr_t GetSuffixData() const {
		auto &sort_key = static_cast<const SORT_KEY &>(*this);
		return IsInlined() ? const_data_ptr_cast(&sort_key.data.u.pad) : sort_key.data.u.ptr + SORT_KEY::INLINE_LENGTH;
	}

	void RelocateHeapPointer(const data_ptr_t &old_heap_ptr, const data_ptr_t &new_heap_ptr) {
		// Inline rows need not have initialized heap locations.
		if (IsInlined()) {
			return;
		}
		auto &sort_key = static_cast<SORT_KEY &>(*this);
		sort_key.data.u.ptr = new_heap_ptr + (sort_key.data.u.ptr - old_heap_ptr);
	}

	idx_t GetSize() const {
		auto &sort_key = static_cast<const SORT_KEY &>(*this);
		return MaxValue(sort_key.size, SORT_KEY::INLINE_CAPACITY);
	}

	idx_t GetHeapSize() const {
		auto &sort_key = static_cast<const SORT_KEY &>(*this);
		return sort_key.size;
	}

	//! Shared prefix safe for heap-only comparisons between sorted endpoints, or zero if unavailable.
	idx_t GetHeapPrefixLength(const SORT_KEY &other, const idx_t max_prefix) const {
		auto &sort_key = static_cast<const SORT_KEY &>(*this);
		if (IsInlined() || other.IsInlined() || memcmp(&sort_key.part0, &other.part0, SORT_KEY::INLINE_LENGTH) != 0) {
			return 0;
		}
		const auto limit = MinValue(max_prefix, MinValue(sort_key.size, other.size));
		idx_t prefix = SORT_KEY::INLINE_LENGTH;
		while (prefix < limit && sort_key.data.u.ptr[prefix] == other.data.u.ptr[prefix]) {
			prefix++;
		}
		// Matching all inline bytes keeps inline keys before these heap-backed endpoints.
		return prefix < SORT_KEY::INLINE_CAPACITY ? 0 : prefix;
	}

	static int32_t TernaryCompare(const uint64_t &lhs, const uint64_t &rhs) {
		return (lhs > rhs) - (lhs < rhs);
	}

	static int32_t LessThan(const uint64_t *const &lhs, const uint64_t *const &rhs) {
		switch (SORT_KEY::PARTS) {
		case 1:
			return TernaryCompare(lhs[0], rhs[0]);
		case 2:
			return 2 * TernaryCompare(lhs[0], rhs[0]) + TernaryCompare(lhs[1], rhs[1]);
		default:
			throw NotImplementedException("VariableSortKey::LessThan for %llu parts", SORT_KEY::PARTS);
		}
	}

	friend bool operator<(const SORT_KEY &lhs, const SORT_KEY &rhs) {
		auto comp_res = LessThan(&lhs.part0, &rhs.part0);
		// If inlined is not equal, we can return already
		if (comp_res != 0) {
			return comp_res < 0;
		}

		// Inline tails are zero-padded; heap-backed keys have at least a full suffix word.
		if (lhs.IsInlined() || rhs.IsInlined()) {
			const auto lhs_tail = BSwapIfLE(Load<uint64_t>(lhs.GetSuffixData()));
			const auto rhs_tail = BSwapIfLE(Load<uint64_t>(rhs.GetSuffixData()));
			return lhs_tail < rhs_tail || (lhs_tail == rhs_tail && lhs.size < rhs.size);
		}

		const auto size = MinValue(lhs.size, rhs.size);
		const auto suffix_size = size - SORT_KEY::INLINE_LENGTH;
		constexpr idx_t WORD_COMPARE_LIMIT = 80;
		if (suffix_size > WORD_COMPARE_LIMIT) {
			comp_res =
			    memcmp(lhs.data.u.ptr + SORT_KEY::INLINE_LENGTH, rhs.data.u.ptr + SORT_KEY::INLINE_LENGTH, suffix_size);
			return comp_res < 0 || (comp_res == 0 && lhs.size < rhs.size);
		}

		for (idx_t offset = SORT_KEY::INLINE_LENGTH; offset + sizeof(uint64_t) < size; offset += sizeof(uint64_t)) {
			const auto lhs_word = Load<uint64_t>(lhs.data.u.ptr + offset);
			const auto rhs_word = Load<uint64_t>(rhs.data.u.ptr + offset);
			if (lhs_word != rhs_word) {
				return BSwapIfLE(lhs_word) < BSwapIfLE(rhs_word);
			}
		}

		// Overlap already-compared bytes to keep the final word inside the logical key.
		const auto lhs_word = BSwapIfLE(Load<uint64_t>(lhs.data.u.ptr + size - sizeof(uint64_t)));
		const auto rhs_word = BSwapIfLE(Load<uint64_t>(rhs.data.u.ptr + size - sizeof(uint64_t)));
		return lhs_word < rhs_word || (lhs_word == rhs_word && lhs.size < rhs.size);
	}
};

template <>
struct SortKey<SortKeyType::NO_PAYLOAD_FIXED_8> : FixedSortKey<SortKey<SortKeyType::NO_PAYLOAD_FIXED_8>, false> {
	static constexpr idx_t PARTS = 1;
	static constexpr idx_t INLINE_LENGTH = 8;
	uint64_t part0;
};

template <>
struct SortKey<SortKeyType::NO_PAYLOAD_FIXED_16> : FixedSortKey<SortKey<SortKeyType::NO_PAYLOAD_FIXED_16>, false> {
	static constexpr idx_t PARTS = 2;
	static constexpr idx_t INLINE_LENGTH = 16;
	uint64_t part0;
	uint64_t part1;
};

template <>
struct SortKey<SortKeyType::NO_PAYLOAD_FIXED_24> : FixedSortKey<SortKey<SortKeyType::NO_PAYLOAD_FIXED_24>, false> {
	static constexpr idx_t PARTS = 3;
	static constexpr idx_t INLINE_LENGTH = 24;
	uint64_t part0;
	uint64_t part1;
	uint64_t part2;
};

template <>
struct SortKey<SortKeyType::NO_PAYLOAD_FIXED_32> : FixedSortKey<SortKey<SortKeyType::NO_PAYLOAD_FIXED_32>, false> {
	static constexpr idx_t PARTS = 4;
	static constexpr idx_t INLINE_LENGTH = 32;
	uint64_t part0;
	uint64_t part1;
	uint64_t part2;
	uint64_t part3;
};

template <>
struct SortKey<SortKeyType::NO_PAYLOAD_VARIABLE_32>
    : VariableSortKey<SortKey<SortKeyType::NO_PAYLOAD_VARIABLE_32>, false> {
	static constexpr idx_t PARTS = 2;
	static constexpr idx_t INLINE_LENGTH = 16;
	static constexpr idx_t INLINE_CAPACITY = INLINE_LENGTH + sizeof(uint64_t);
	static constexpr idx_t HEAP_SIZE_OFFSET = 16;
	uint64_t part0;
	uint64_t part1;
	uint64_t size;
	sort_key_ptr_t data;
};

template <>
struct SortKey<SortKeyType::PAYLOAD_FIXED_16> : FixedSortKey<SortKey<SortKeyType::PAYLOAD_FIXED_16>, true> {
	static constexpr idx_t PARTS = 1;
	static constexpr idx_t INLINE_LENGTH = 8;
	uint64_t part0;
	sort_key_ptr_t payload;
};

template <>
struct SortKey<SortKeyType::PAYLOAD_FIXED_24> : FixedSortKey<SortKey<SortKeyType::PAYLOAD_FIXED_24>, true> {
	static constexpr idx_t PARTS = 2;
	static constexpr idx_t INLINE_LENGTH = 16;
	uint64_t part0;
	uint64_t part1;
	sort_key_ptr_t payload;
};

template <>
struct SortKey<SortKeyType::PAYLOAD_FIXED_32> : FixedSortKey<SortKey<SortKeyType::PAYLOAD_FIXED_32>, true> {
	static constexpr idx_t PARTS = 3;
	static constexpr idx_t INLINE_LENGTH = 24;
	uint64_t part0;
	uint64_t part1;
	uint64_t part2;
	sort_key_ptr_t payload;
};

template <>
struct SortKey<SortKeyType::PAYLOAD_VARIABLE_32> : VariableSortKey<SortKey<SortKeyType::PAYLOAD_VARIABLE_32>, true> {
	static constexpr idx_t PARTS = 1;
	static constexpr idx_t INLINE_LENGTH = 8;
	static constexpr idx_t INLINE_CAPACITY = INLINE_LENGTH + sizeof(uint64_t);
	static constexpr idx_t HEAP_SIZE_OFFSET = 8;
	uint64_t part0;
	uint64_t size;
	sort_key_ptr_t data;
	sort_key_ptr_t payload;
};

static_assert(sizeof(SortKey<SortKeyType::NO_PAYLOAD_FIXED_8>) == 8, "NO_PAYLOAD_FIXED_8 must be 8 wide");
static_assert(sizeof(SortKey<SortKeyType::NO_PAYLOAD_FIXED_16>) == 16, "NO_PAYLOAD_FIXED_16 must be 16 wide");
static_assert(sizeof(SortKey<SortKeyType::NO_PAYLOAD_FIXED_24>) == 24, "NO_PAYLOAD_FIXED_24 must be 24 wide");
static_assert(sizeof(SortKey<SortKeyType::NO_PAYLOAD_FIXED_32>) == 32, "NO_PAYLOAD_FIXED_32 must be 32 wide");
static_assert(sizeof(SortKey<SortKeyType::NO_PAYLOAD_VARIABLE_32>) == 32, "NO_PAYLOAD_VARIABLE_32 must be 32 wide");
static_assert(sizeof(SortKey<SortKeyType::PAYLOAD_FIXED_16>) == 16, "PAYLOAD_FIXED_16 must be 16 wide");
static_assert(sizeof(SortKey<SortKeyType::PAYLOAD_FIXED_24>) == 24, "PAYLOAD_FIXED_24 must be 24 wide");
static_assert(sizeof(SortKey<SortKeyType::PAYLOAD_FIXED_32>) == 32, "PAYLOAD_FIXED_32 must be 32 wide");
static_assert(sizeof(SortKey<SortKeyType::PAYLOAD_VARIABLE_32>) == 32, "PAYLOAD_VARIABLE_32 must be 32 wide");

struct SortKeyUtils {
	static idx_t GetInlineLength(const SortKeyType sort_key_type) {
		switch (sort_key_type) {
		case SortKeyType::NO_PAYLOAD_FIXED_8:
			return SortKey<SortKeyType::NO_PAYLOAD_FIXED_8>::INLINE_LENGTH;
		case SortKeyType::NO_PAYLOAD_FIXED_16:
			return SortKey<SortKeyType::NO_PAYLOAD_FIXED_16>::INLINE_LENGTH;
		case SortKeyType::NO_PAYLOAD_FIXED_24:
			return SortKey<SortKeyType::NO_PAYLOAD_FIXED_24>::INLINE_LENGTH;
		case SortKeyType::NO_PAYLOAD_FIXED_32:
			return SortKey<SortKeyType::NO_PAYLOAD_FIXED_32>::INLINE_LENGTH;
		case SortKeyType::NO_PAYLOAD_VARIABLE_32:
			return SortKey<SortKeyType::NO_PAYLOAD_VARIABLE_32>::INLINE_LENGTH;
		case SortKeyType::PAYLOAD_FIXED_16:
			return SortKey<SortKeyType::PAYLOAD_FIXED_16>::INLINE_LENGTH;
		case SortKeyType::PAYLOAD_FIXED_24:
			return SortKey<SortKeyType::PAYLOAD_FIXED_24>::INLINE_LENGTH;
		case SortKeyType::PAYLOAD_FIXED_32:
			return SortKey<SortKeyType::PAYLOAD_FIXED_32>::INLINE_LENGTH;
		case SortKeyType::PAYLOAD_VARIABLE_32:
			return SortKey<SortKeyType::PAYLOAD_VARIABLE_32>::INLINE_LENGTH;
		default:
			throw NotImplementedException("SortKeyUtils::GetInlineLength for %s", EnumUtil::ToString(sort_key_type));
		}
	}

	static idx_t GetInlineCapacity(const SortKeyType sort_key_type) {
		switch (sort_key_type) {
		case SortKeyType::NO_PAYLOAD_VARIABLE_32:
			return SortKey<SortKeyType::NO_PAYLOAD_VARIABLE_32>::INLINE_CAPACITY;
		case SortKeyType::PAYLOAD_VARIABLE_32:
			return SortKey<SortKeyType::PAYLOAD_VARIABLE_32>::INLINE_CAPACITY;
		default:
			return GetInlineLength(sort_key_type);
		}
	}

	static bool IsConstantSize(const SortKeyType sort_key_type) {
		switch (sort_key_type) {
		case SortKeyType::NO_PAYLOAD_FIXED_8:
			return SortKey<SortKeyType::NO_PAYLOAD_FIXED_8>::CONSTANT_SIZE;
		case SortKeyType::NO_PAYLOAD_FIXED_16:
			return SortKey<SortKeyType::NO_PAYLOAD_FIXED_16>::CONSTANT_SIZE;
		case SortKeyType::NO_PAYLOAD_FIXED_24:
			return SortKey<SortKeyType::NO_PAYLOAD_FIXED_24>::CONSTANT_SIZE;
		case SortKeyType::NO_PAYLOAD_FIXED_32:
			return SortKey<SortKeyType::NO_PAYLOAD_FIXED_32>::CONSTANT_SIZE;
		case SortKeyType::NO_PAYLOAD_VARIABLE_32:
			return SortKey<SortKeyType::NO_PAYLOAD_VARIABLE_32>::CONSTANT_SIZE;
		case SortKeyType::PAYLOAD_FIXED_16:
			return SortKey<SortKeyType::PAYLOAD_FIXED_16>::CONSTANT_SIZE;
		case SortKeyType::PAYLOAD_FIXED_24:
			return SortKey<SortKeyType::PAYLOAD_FIXED_24>::CONSTANT_SIZE;
		case SortKeyType::PAYLOAD_FIXED_32:
			return SortKey<SortKeyType::PAYLOAD_FIXED_32>::CONSTANT_SIZE;
		case SortKeyType::PAYLOAD_VARIABLE_32:
			return SortKey<SortKeyType::PAYLOAD_VARIABLE_32>::CONSTANT_SIZE;
		default:
			throw NotImplementedException("SortKeyUtils::IsConstantSize for %s", EnumUtil::ToString(sort_key_type));
		}
	}

	static bool HasPayload(const SortKeyType sort_key_type) {
		switch (sort_key_type) {
		case SortKeyType::NO_PAYLOAD_FIXED_8:
			return SortKey<SortKeyType::NO_PAYLOAD_FIXED_8>::HAS_PAYLOAD;
		case SortKeyType::NO_PAYLOAD_FIXED_16:
			return SortKey<SortKeyType::NO_PAYLOAD_FIXED_16>::HAS_PAYLOAD;
		case SortKeyType::NO_PAYLOAD_FIXED_24:
			return SortKey<SortKeyType::NO_PAYLOAD_FIXED_24>::HAS_PAYLOAD;
		case SortKeyType::NO_PAYLOAD_FIXED_32:
			return SortKey<SortKeyType::NO_PAYLOAD_FIXED_32>::HAS_PAYLOAD;
		case SortKeyType::NO_PAYLOAD_VARIABLE_32:
			return SortKey<SortKeyType::NO_PAYLOAD_VARIABLE_32>::HAS_PAYLOAD;
		case SortKeyType::PAYLOAD_FIXED_16:
			return SortKey<SortKeyType::PAYLOAD_FIXED_16>::HAS_PAYLOAD;
		case SortKeyType::PAYLOAD_FIXED_24:
			return SortKey<SortKeyType::PAYLOAD_FIXED_24>::HAS_PAYLOAD;
		case SortKeyType::PAYLOAD_FIXED_32:
			return SortKey<SortKeyType::PAYLOAD_FIXED_32>::HAS_PAYLOAD;
		case SortKeyType::PAYLOAD_VARIABLE_32:
			return SortKey<SortKeyType::PAYLOAD_VARIABLE_32>::HAS_PAYLOAD;
		default:
			throw NotImplementedException("SortKeyUtils::HasPayload for %s", EnumUtil::ToString(sort_key_type));
		}
	}
};

} // namespace duckdb
