#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/types/string_type.hpp"

#include "duckdb/common/exception.hpp"

#include <functional>

namespace duckdb {

template <>
hash_t Hash(uint64_t val) {
	return murmurhash64(val);
}

template <>
hash_t Hash(int64_t val) {
	return murmurhash64((uint64_t)val);
}

template <>
hash_t Hash(hugeint_t val) {
	return murmurhash64(val.lower) ^ murmurhash64(val.upper);
}

template <>
hash_t Hash(float val) {
	return std::hash<float> {}(val);
}

template <>
hash_t Hash(double val) {
	return std::hash<double> {}(val);
}

template <>
hash_t Hash(interval_t val) {
	return Hash(val.days) ^ Hash(val.months) ^ Hash(val.micros);
}

template <>
hash_t Hash(const char *str) {
	return Hash(str, strlen(str));
}

template <>
hash_t Hash(string_t val) {
	return Hash(val.GetDataUnsafe(), val.GetSize());
}

template <>
hash_t Hash(char *val) {
	return Hash<const char *>(val);
}

// Jenkins hash function: https://en.wikipedia.org/wiki/Jenkins_hash_function
uint32_t JenkinsOneAtATimeHash(const char *key, size_t length) {
	size_t i = 0;
	uint32_t hash = 0;
	while (i != length) {
		hash += key[i++];
		hash += hash << 10;
		hash ^= hash >> 6;
	}
	hash += hash << 3;
	hash ^= hash >> 11;
	hash += hash << 15;
	return hash;
}

hash_t Hash(const char *val, size_t size) {
	auto hash_val = JenkinsOneAtATimeHash(val, size);
	return Hash<uint32_t>(hash_val);
}

hash_t Hash(uint8_t *val, size_t size) {
	return Hash((const char *)val, size);
}

} // namespace duckdb
