//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/hash.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/projection_index.hpp"
#include "duckdb/common/table_index.hpp"

namespace duckdb {

struct string_t;
struct interval_t; // NOLINT

//! Combine two hashes by XORing them
inline hash_t CombineHash(hash_t left, hash_t right) {
	return left ^ right;
}

#ifdef DUCKDB_HASH_SKEW
//! Collapse target: an arbitrary constant with well-spread bits, so the collapsed group lands in an
//! ordinary bucket with an ordinary salt rather than a special one.
static constexpr hash_t HASH_SKEW_COLLAPSED = 0x9e3779b97f4a7c15ULL;
//! Decorrelates the collapse decision from the hash value being tested.
static constexpr hash_t HASH_SKEW_SELECTOR_SEED = 0xc3a5c85c97cb3127ULL;
//! DUCKDB_HASH_SKEW is a percentage of distinct values to collapse. The collapsed chain holds
//! `percent * distinct_values` entries, so the useful setting depends on data size.
static constexpr double HASH_SKEW_PERCENT = DUCKDB_HASH_SKEW;
static constexpr hash_t HASH_SKEW_THRESHOLD =
    HASH_SKEW_PERCENT >= 100.0
        ? ~hash_t(0)
        : (HASH_SKEW_PERCENT <= 0.0 ? hash_t(0)
                                    : static_cast<hash_t>(HASH_SKEW_PERCENT * 0.01 * 18446744073709551616.0));

//! A private mixer. Deliberately NOT MurmurHash64: the skew is applied inside that function, so
//! calling it here would recurse.
inline hash_t SkewSelectorMix(uint64_t x) {
	x ^= x >> 32;
	x *= 0xd6e8feb86659fd93U;
	x ^= x >> 32;
	x *= 0xd6e8feb86659fd93U;
	x ^= x >> 32;
	return x;
}
#endif

//! Debug setting: collapse a fraction of hash values onto one constant, leaving the rest properly
//! distributed - one pathological bucket alongside healthy ones. Selecting on a separately mixed
//! value rather than on `hash` itself matters: a direct `hash < threshold` would leave survivors
//! biased towards high hashes, shrinking the salt and bucket space. Idempotent, so applying it more
//! than once along a hash path is harmless.
inline hash_t ApplyHashSkew(hash_t hash) {
#ifdef DUCKDB_HASH_SKEW
	return SkewSelectorMix(hash ^ HASH_SKEW_SELECTOR_SEED) < HASH_SKEW_THRESHOLD ? HASH_SKEW_COLLAPSED : hash;
#else
	return hash;
#endif
}

//! Efficient hash function that maximizes the avalanche effect and minimizes bias
//! See: https://nullprogram.com/blog/2018/07/31/
inline hash_t MurmurHash64(uint64_t x) {
	x ^= x >> 32;
	x *= 0xd6e8feb86659fd93U;
	x ^= x >> 32;
	x *= 0xd6e8feb86659fd93U;
	x ^= x >> 32;
	return ApplyHashSkew(x);
}

#ifdef DUCKDB_HASH_ZERO
template <class T>
hash_t Hash(T value) {
	return 0;
}

DUCKDB_API hash_t Hash(const char *val, size_t size);
DUCKDB_API hash_t Hash(uint8_t *val, size_t size);
#else

inline hash_t MurmurHash32(uint32_t x) {
	return MurmurHash64(x);
}

template <class T>
hash_t Hash(T value) {
	return MurmurHash32(static_cast<uint32_t>(value));
}

template <>
DUCKDB_API inline hash_t Hash(uint64_t val) {
	return MurmurHash64(val);
}
template <>
DUCKDB_API inline hash_t Hash(int64_t val) {
	return MurmurHash64(static_cast<uint64_t>(val));
}
template <>
DUCKDB_API inline hash_t Hash(TableIndex val) {
	return MurmurHash64(val.index);
}
template <>
DUCKDB_API inline hash_t Hash(ProjectionIndex val) {
	return MurmurHash64(val.GetIndexUnsafe());
}
template <>
DUCKDB_API hash_t Hash(hugeint_t val);
template <>
DUCKDB_API hash_t Hash(uhugeint_t val);
template <>
DUCKDB_API hash_t Hash(float val);
template <>
DUCKDB_API hash_t Hash(double val);
template <>
DUCKDB_API hash_t Hash(const char *val);
template <>
DUCKDB_API hash_t Hash(char *val);
template <>
DUCKDB_API hash_t Hash(string_t val);
template <>
DUCKDB_API hash_t Hash(interval_t val);
template <>
DUCKDB_API hash_t Hash(dtime_tz_t val);
DUCKDB_API hash_t Hash(const char *val, size_t size);
DUCKDB_API hash_t Hash(uint8_t *val, size_t size);
#endif

} // namespace duckdb
