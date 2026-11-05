#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/arena_containers/arena_ptr.hpp"

#include <cstring>

namespace duckdb {

template <class T>
struct TransformResultTypeIdentifier {
	static const char *GetName() {
		static_assert(AlwaysFalse<T>::VALUE,
		              "Transform result types must be registered with DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE");
		return nullptr;
	}
};

//! Registers a stable name for a transform result type. Invoke this macro from namespace duckdb.
#define DUCKDB_REGISTER_TRANSFORM_RESULT_TYPE(NAME, ...)                                                               \
	template <>                                                                                                        \
	struct TransformResultTypeIdentifier<__VA_ARGS__> {                                                                \
		static constexpr const char *GetName() {                                                                       \
			return NAME;                                                                                               \
		}                                                                                                              \
	};

//! A stable per-type name, used to identify transform results across loadable extension boundaries without RTTI.
template <class T>
constexpr const char *TransformResultTypeName() {
	return TransformResultTypeIdentifier<T>::GetName();
}

//! FNV-1a hash of a type name, computed at compile time
constexpr uint64_t TransformResultTypeNameHash(const char *name) {
	uint64_t hash = 14695981039346656037ULL;
	for (; *name; name++) {
		hash = (hash ^ static_cast<uint64_t>(static_cast<unsigned char>(*name))) * 1099511628211ULL;
	}
	return hash;
}

template <class T>
constexpr uint64_t TransformResultTypeHash() {
	return TransformResultTypeNameHash(TransformResultTypeName<T>());
}

struct DUCKDB_API TransformResultValue {
	explicit TransformResultValue(uint64_t type_hash_p) : type_hash(type_hash_p) {
	}
	virtual ~TransformResultValue() = default;

	//! Returns a pointer to the value if its type matches, without relying on RTTI
	virtual void *GetValuePointer(const char *type_name) = 0;

	//! Hash of the registered type name, so mismatching types are rejected without a virtual call or string compare
	uint64_t type_hash;
};

template <class T>
struct DUCKDB_API TypedTransformResult : public TransformResultValue {
	explicit TypedTransformResult(T value_p) : TransformResultValue(TYPE_HASH), value(std::move(value_p)) {
	}
	TypedTransformResult(const TypedTransformResult &) = delete;
	TypedTransformResult &operator=(const TypedTransformResult &) = delete;

	static constexpr uint64_t TYPE_HASH = TransformResultTypeHash<T>();

	void *GetValuePointer(const char *type_name) override {
		auto expected = TransformResultTypeName<T>();
		return type_name == expected || std::strcmp(type_name, expected) == 0 ? &value : nullptr;
	}

	T value;
};

//! Returns a pointer to the contained value if the result holds exactly T, and nullptr otherwise
template <class T>
T *TryGetTransformResult(TransformResultValue &result) {
	if (result.type_hash != TransformResultTypeHash<T>()) {
		return nullptr;
	}
	return reinterpret_cast<T *>(result.GetValuePointer(TransformResultTypeName<T>()));
}

} // namespace duckdb
