//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/serializer/serializer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/serializer/serialization_traits.hpp"
#include "duckdb/common/serializer/serialization_data.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/queue.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optionally_owned_ptr.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_option.hpp"
#include "duckdb/common/insertion_order_preserving_map.hpp"
#include "duckdb/common/storage_compatibility.hpp"
#include "duckdb/storage/table/per_column_metadata_blocks.hpp"

namespace duckdb {
class SerializationOptions {
public:
	SerializationOptions() = default;
	explicit SerializationOptions(AttachedDatabase &db);

	bool serialize_enum_as_string = false;
	bool serialize_default_values = false;
	StorageCompatibility storage_compatibility = StorageCompatibility::Default();
};

class Serializer {
protected:
	SerializationOptions options;
	SerializationData data;

public:
	virtual ~Serializer() {
	}

	bool ShouldSerializeInternal(StorageVersion version_added) const {
		return options.storage_compatibility.Compare(version_added);
	}

	bool ShouldSerialize(StorageVersion version_added) const {
		return ShouldSerializeInternal(version_added);
	}

	class List {
		friend Serializer;

	private:
		Serializer &serializer;
		explicit List(Serializer &serializer) : serializer(serializer) {
		}

	public:
		// Serialize an element
		template <class T>
		void WriteElement(const T &value);

		//! Serialize bytes
		void WriteElement(data_ptr_t ptr, idx_t size);

		// Serialize an object
		template <class FUNC>
		void WriteObject(FUNC f);
	};

public:
	SerializationOptions GetOptions() {
		return options;
	}
	SerializationData &GetSerializationData() {
		return data;
	}

	void SetSerializationData(const SerializationData &other) {
		data = other;
	}

	// Serialize a value
	template <class T>
	void WriteProperty(const field_id_t field_id, const char *tag, const T &value) {
		OnPropertyBegin(field_id, tag);
		WriteValue(value);
		OnPropertyEnd();
	}

	// Default value
	template <class T>
	void WritePropertyWithDefault(const field_id_t field_id, const char *tag, const T &value) {
		// If current value is default, don't write it
		if (!options.serialize_default_values && SerializationDefaultValue::IsDefault<T>(value)) {
			OnOptionalPropertyBegin(field_id, tag, false);
			OnOptionalPropertyEnd(false);
			return;
		}
		OnOptionalPropertyBegin(field_id, tag, true);
		WriteValue(value);
		OnOptionalPropertyEnd(true);
	}

	template <class T>
	void WritePropertyWithDefault(const field_id_t field_id, const char *tag, const T &value, const T &default_value) {
		// If current value is default, don't write it
		if (!options.serialize_default_values && (value == default_value)) {
			OnOptionalPropertyBegin(field_id, tag, false);
			OnOptionalPropertyEnd(false);
			return;
		}
		OnOptionalPropertyBegin(field_id, tag, true);
		WriteValue(value);
		OnOptionalPropertyEnd(true);
	}

	// Specialization for Value (default Value comparison throws when comparing nulls)
	template <class T>
	void WritePropertyWithDefault(const field_id_t field_id, const char *tag, const CSVOption<T> &value,
	                              const T &default_value) {
		// If current value is default, don't write it
		if (!options.serialize_default_values && (value == default_value)) {
			OnOptionalPropertyBegin(field_id, tag, false);
			OnOptionalPropertyEnd(false);
			return;
		}
		OnOptionalPropertyBegin(field_id, tag, true);
		WriteValue(value.GetValue());
		OnOptionalPropertyEnd(true);
	}

	// Special case: data_ptr_T
	void WriteProperty(const field_id_t field_id, const char *tag, const_data_ptr_t ptr, idx_t count) {
		OnPropertyBegin(field_id, tag);
		WriteDataPtr(ptr, count);
		OnPropertyEnd();
	}

	// Manually begin an object
	template <class FUNC>
	void WriteObject(const field_id_t field_id, const char *tag, FUNC f) {
		OnPropertyBegin(field_id, tag);
		OnObjectBegin();
		f(*this);
		OnObjectEnd();
		OnPropertyEnd();
	}

	template <class FUNC>
	void WriteList(const field_id_t field_id, const char *tag, idx_t count, FUNC func) {
		OnPropertyBegin(field_id, tag);
		OnListBegin(count);
		List list {*this};
		for (idx_t i = 0; i < count; i++) {
			func(list, i);
		}
		OnListEnd();
		OnPropertyEnd();
	}

protected:
	template <typename T>
	typename std::enable_if<std::is_enum<T>::value, void>::type WriteValue(const T value) {
		if (options.serialize_enum_as_string) {
			// Use the enum serializer to lookup tostring function
			auto str = EnumUtil::ToChars(value);
			WriteValue(str);
		} else {
			// Use the underlying type
			WriteValue(static_cast<typename std::underlying_type<T>::type>(value));
		}
	}

	// Optionally Owned Pointer Ref
	template <typename T>
	void WriteValue(const optionally_owned_ptr<T> &ptr) {
		WriteValue(ptr.get());
	}

	// Unique Pointer Ref
	template <typename T>
	void WriteValue(const unique_ptr<T> &ptr) {
		WriteValue(ptr.get());
	}

	// Shared Pointer Ref
	template <typename T>
	void WriteValue(const shared_ptr<T> &ptr) {
		WriteValue(ptr.get());
	}

	// Optional Pointer Ref
	template <typename T>
	void WriteValue(const optional_ptr<T> &ptr) {
		WriteValue(ptr.get());
	}

	// Pointer
	template <typename T>
	void WriteValue(const T *ptr) {
		if (ptr == nullptr) {
			OnNullableBegin(false);
			OnNullableEnd();
		} else {
			OnNullableBegin(true);
			WriteValue(*ptr);
			OnNullableEnd();
		}
	}

	// DuckDB Optional
	template <typename T>
	void WriteValue(const optional<T> &opt) {
		if (!opt) {
			OnNullableBegin(false);
			OnNullableEnd();
		} else {
			OnNullableBegin(true);
			WriteValue(opt.value());
			OnNullableEnd();
		}
	}

	// Pair
	template <class K, class V>
	void WriteValue(const std::pair<K, V> &pair) {
		OnObjectBegin();
		WriteProperty(0, "first", pair.first);
		WriteProperty(1, "second", pair.second);
		OnObjectEnd();
	}

	// Reference Wrapper
	template <class T>
	void WriteValue(const reference<T> ref) {
		WriteValue(ref.get());
	}

	// Vector
	template <class T>
	void WriteValue(const vector<T> &vec) {
		auto count = vec.size();
		OnListBegin(count);
		for (auto &item : vec) {
			WriteValue(item);
		}
		OnListEnd();
	}

	template <class T>
	void WriteValue(const unsafe_vector<T> &vec) {
		auto count = vec.size();
		OnListBegin(count);
		for (auto &item : vec) {
			WriteValue(item);
		}
		OnListEnd();
	}

	// UnorderedSet
	// Serialized the same way as a list/vector
	template <class T, class HASH, class CMP>
	void WriteValue(const duckdb::unordered_set<T, HASH, CMP> &set) {
		auto count = set.size();
		OnListBegin(count);
		for (auto &item : set) {
			WriteValue(item);
		}
		OnListEnd();
	}

	// Set
	// Serialized the same way as a list/vector
	template <class T, class HASH, class CMP>
	void WriteValue(const duckdb::set<T, HASH, CMP> &set) {
		auto count = set.size();
		OnListBegin(count);
		for (auto &item : set) {
			WriteValue(item);
		}
		OnListEnd();
	}

	// Map
	// serialized as a list of pairs
	template <class K, class V, class HASH, class CMP>
	void WriteValue(const duckdb::unordered_map<K, V, HASH, CMP> &map) {
		auto count = map.size();
		OnListBegin(count);
		for (auto &item : map) {
			OnObjectBegin();
			WriteProperty(0, "key", item.first);
			WriteProperty(1, "value", item.second);
			OnObjectEnd();
		}
		OnListEnd();
	}

	// Map
	// serialized as a list of pairs
	template <class K, class V, class HASH, class CMP>
	void WriteValue(const duckdb::map<K, V, HASH, CMP> &map) {
		auto count = map.size();
		OnListBegin(count);
		for (auto &item : map) {
			OnObjectBegin();
			WriteProperty(0, "key", item.first);
			WriteProperty(1, "value", item.second);
			OnObjectEnd();
		}
		OnListEnd();
	}

	// Insertion Order Preserving Map
	// serialized as a list of pairs
	template <class V, class KEY, class INDEX_MAP>
	void WriteValue(const duckdb::InsertionOrderPreservingMap<V, KEY, INDEX_MAP> &map) {
		auto count = map.size();
		OnListBegin(count);
		for (auto &entry : map) {
			OnObjectBegin();
			WriteProperty(0, "key", entry.first);
			WriteProperty(1, "value", entry.second);
			OnObjectEnd();
		}
		OnListEnd();
	}

	// priority queue
	template <typename T>
	void WriteValue(const std::priority_queue<T> &queue) {
		vector<T> placeholder;
		auto queue_copy = std::priority_queue<T>(queue);
		while (queue_copy.size() > 0) {
			placeholder.emplace_back(queue_copy.top());
			queue_copy.pop();
		}
		WriteValue(placeholder);
	}

	// class or struct implementing `Serialize(Serializer& Serializer)`;
	template <typename T>
	typename std::enable_if<has_serialize<T>::value>::type WriteValue(const T &value) {
		OnObjectBegin();
		value.Serialize(*this);
		OnObjectEnd();
	}

protected:
	// Hooks for subclasses to override to implement custom behavior
	virtual void OnPropertyBegin(const field_id_t field_id, const char *tag) = 0;
	virtual void OnPropertyEnd() = 0;
	virtual void OnOptionalPropertyBegin(const field_id_t field_id, const char *tag, bool present) = 0;
	virtual void OnOptionalPropertyEnd(bool present) = 0;
	virtual void OnObjectBegin() = 0;
	virtual void OnObjectEnd() = 0;
	virtual void OnListBegin(idx_t count) = 0;
	virtual void OnListEnd() = 0;
	virtual void OnNullableBegin(bool present) = 0;
	virtual void OnNullableEnd() = 0;

	// Handle primitive types, a serializer needs to implement these.
	virtual void WriteNull() = 0;
	virtual void WriteValue(char value) {
		throw NotImplementedException("Write char value not implemented");
	}
	virtual void WriteValue(bool value) = 0;
	virtual void WriteValue(uint8_t value) = 0;
	virtual void WriteValue(int8_t value) = 0;
	virtual void WriteValue(uint16_t value) = 0;
	virtual void WriteValue(int16_t value) = 0;
	virtual void WriteValue(uint32_t value) = 0;
	virtual void WriteValue(int32_t value) = 0;
	virtual void WriteValue(uint64_t value) = 0;
	virtual void WriteValue(int64_t value) = 0;
	virtual void WriteValue(hugeint_t value) = 0;
	virtual void WriteValue(uhugeint_t value) = 0;
	virtual void WriteValue(float value) = 0;
	virtual void WriteValue(double value) = 0;
	virtual void WriteValue(const string_t value) = 0;
	virtual void WriteValue(const string &value) = 0;
	virtual void WriteValue(const char *str) = 0;
	virtual void WriteDataPtr(const_data_ptr_t ptr, idx_t count) = 0;
	//! Identifiers are serialized identically to a plain string (preserving the original casing)
	void WriteValue(const Identifier &value) {
		WriteValue(value.GetIdentifierName());
	}
	void WriteValue(LogicalIndex value) {
		WriteValue(value.index);
	}
	void WriteValue(PhysicalIndex value) {
		WriteValue(value.index);
	}
	void WriteValue(TableIndex value) {
		WriteValue(value.index);
	}
	void WriteValue(ProjectionIndex value) {
		WriteValue(value.GetIndexUnsafe());
	}
	void WriteValue(optional_idx value) {
		if (!value.IsValid()) {
			WriteValue(DConstants::INVALID_INDEX);
			return;
		}
		// INVALID_INDEX marks an unset optional_idx in the serialized format
		if (value.GetIndex() == DConstants::INVALID_INDEX) {
			throw InternalException("Cannot serialize an optional_idx that is set to INVALID_INDEX");
		}
		WriteValue(value.GetIndex());
	}
	void WriteValue(PerColumnMetadataBlock value) {
		WriteValue(value.GetPacked());
	}
};

// We need to special case vector<bool> because elements of vector<bool> cannot be referenced
template <>
void Serializer::WriteValue(const vector<bool> &vec);

// Specialization for Value (default Value comparison throws when comparing nulls)
template <>
void Serializer::WritePropertyWithDefault<Value>(const field_id_t field_id, const char *tag, const Value &value,
                                                 const Value &default_value);

// List Impl
template <class FUNC>
void Serializer::List::WriteObject(FUNC f) {
	serializer.OnObjectBegin();
	f(serializer);
	serializer.OnObjectEnd();
}

template <class T>
void Serializer::List::WriteElement(const T &value) {
	serializer.WriteValue(value);
}

} // namespace duckdb
