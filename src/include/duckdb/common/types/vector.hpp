//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector/unified_vector_format.hpp"
#include "duckdb/common/types/vector_buffer.hpp"
#include "duckdb/common/vector_size.hpp"

namespace duckdb {

class VectorCache;
class DictionaryBuffer;
class DictionaryEntry;
class VectorStringBuffer;
class VectorStructBuffer;
class VectorListBuffer;
struct SelCache;
enum class VectorConstructorAction;

template <class T>
class VectorIterator;
template <class T>
class VectorValidValueIterator;
class VectorValidityIterator;

enum class VectorDataInitialization { UNINITIALIZED, ZERO_INITIALIZE };

//! Vector of values of a specified PhysicalType.
class Vector {
public:
	//! Create a vector that slices another vector
	DUCKDB_API explicit Vector(const Vector &other, const SelectionVector &sel, idx_t count);
	//! Create a vector that slices another vector between a pair of offsets
	DUCKDB_API explicit Vector(const Vector &other, idx_t offset, idx_t end);
	//! Create a vector of size one holding the passed on value
	DUCKDB_API explicit Vector(const Value &value, count_t count);
	//! Create a vector of size capacity (non-standard)
	DUCKDB_API explicit Vector(LogicalType type, idx_t capacity = STANDARD_VECTOR_SIZE,
	                           VectorDataInitialization initialize = VectorDataInitialization::UNINITIALIZED);
	//! Instantiate a vector from a vector cache
	DUCKDB_API explicit Vector(const VectorCache &cache);
	//! Create a non-owning vector that references the specified data
	DUCKDB_API Vector(LogicalType type, data_ptr_t dataptr, idx_t count);
	//! Create a vector with an explicitly created vector buffer
	DUCKDB_API Vector(LogicalType type, buffer_ptr<VectorBuffer> buffer);
	// Copying of vectors is not
	Vector(const Vector &other) = delete;
	// but moving of vectors is allowed
	DUCKDB_API Vector(Vector &&other) noexcept;

public:
	idx_t size() const; // NOLINT
	//! Checks if a vector has enough space for the given count - throws an internal error otherwise
	DUCKDB_API void CheckCapacity(idx_t capacity) const;

	//! Create a new vector that references the other vector
	DUCKDB_API static Vector Ref(const Vector &other);

	//! Create a vector that references the specified value.
	DUCKDB_API void Reference(const Value &value, count_t count);
	//! Causes this vector to reference the data held by the other vector.
	//! The type of the "other" vector should match the type of this vector
	DUCKDB_API void Reference(const Vector &other);
	//! Reinterpret the data of the other vector as the type of this vector
	//! Note that this takes the data of the other vector as-is and places it in this vector
	//! Without changing the type of this vector
	DUCKDB_API void Reinterpret(const Vector &other);

	//! Causes this vector to reference the data held by the other vector, changes the type if required.
	DUCKDB_API void ReferenceAndSetType(const Vector &other);

	//! Resets a vector from a vector cache.
	//! Returns the cached flat buffer, empty and without allocating its payload.
	DUCKDB_API void ResetFromCache(const VectorCache &cache);

	//! Creates a reference to a slice of the other vector
	DUCKDB_API void Slice(const Vector &other, idx_t offset, idx_t end);
	//! Creates a reference to a slice of the other vector
	DUCKDB_API void Slice(const Vector &other, const SelectionVector &sel, idx_t count);
	//! Turns the vector into a dictionary vector with the specified dictionary
	DUCKDB_API void Slice(const SelectionVector &sel, idx_t count);
	//! Slice the vector, keeping the result around in a cache or potentially using the cache instead of slicing
	DUCKDB_API void Slice(const SelectionVector &sel, idx_t count, SelCache &cache);
	//! Turn this vector into a dictionary vector
	DUCKDB_API void Dictionary(idx_t dictionary_size, const SelectionVector &sel, idx_t count);
	//! Creates a reference to a dictionary of the other vector
	DUCKDB_API void Dictionary(const Vector &dict, idx_t dictionary_size, const SelectionVector &sel, idx_t count);
	//! Creates a dictionary on the reusable dict
	DUCKDB_API void Dictionary(buffer_ptr<DictionaryEntry> reusable_dict, const SelectionVector &sel, idx_t sel_count);

	//! Creates the data of this vector with the specified type. Any data that
	//! is currently in the vector is destroyed.
	DUCKDB_API void Initialize(VectorDataInitialization data_initialize = VectorDataInitialization::UNINITIALIZED,
	                           idx_t capacity = STANDARD_VECTOR_SIZE);

	//! Converts this Vector to a printable string representation
	[[deprecated("ToString no longer requires a count - use ToString() instead")]] DUCKDB_API string
	ToString(idx_t count) const;
	[[deprecated("Print no longer requires a count - use Print() instead")]] DUCKDB_API void Print(idx_t count) const;

	DUCKDB_API string ToString() const;
	DUCKDB_API void Print() const;

	[[deprecated("Flatten no longer requires a count - use Flatten() instead")]] DUCKDB_API void
	Flatten(idx_t count) const;
	//! Flatten the vector, removing any compression and turning it into a FLAT_VECTOR
	//! While Flatten mutates the buffers / vector type, it does not change the *logical* representation of a vector
	//! As such, it can be used on constant vectors.
	DUCKDB_API void Flatten() const;
	DUCKDB_API void Flatten(const SelectionVector &sel, idx_t count) const;

	[[deprecated("ToUnifiedFormat no longer requires a count - use ToUnifiedFormat(data) instead")]] DUCKDB_API void
	ToUnifiedFormat(idx_t count, UnifiedVectorFormat &data) const;
	//! Creates a UnifiedVectorFormat of a vector
	//! The UnifiedVectorFormat allows efficient reading of vectors regardless of their vector type
	//! It contains (1) a data pointer, (2) a validity mask, and (3) a selection vector
	//! Access to the individual vector elements can be performed through data_pointer[sel_idx[i]]/validity[sel_idx[i]]
	//! The most common vector types (flat, constant & dictionary) can be converted to the canonical format "for free"
	//! ToUnifiedFormat was originally called Orrify, as a tribute to Orri Erling who came up with it
	DUCKDB_API void ToUnifiedFormat(UnifiedVectorFormat &data) const;

	[[deprecated("RecursiveToUnifiedFormat no longer requires a count - use RecursiveToUnifiedFormat(input, data) "
	             "instead")]] static void
	RecursiveToUnifiedFormat(const Vector &input, idx_t count, RecursiveUnifiedVectorFormat &data);
	//! Recursively calls UnifiedVectorFormat on a vector and its child vectors (for nested types)
	static void RecursiveToUnifiedFormat(const Vector &input, RecursiveUnifiedVectorFormat &data);

	//! Turn the vector into a sequence vector
	DUCKDB_API void Sequence(int64_t start, int64_t increment, idx_t count);

	//! Turn the vector into a shredded variant vector
	DUCKDB_API void Shred(Vector &shredded_data, idx_t capacity);

	[[deprecated("Verify no longer requires a count - use Verify() without count instead")]] DUCKDB_API void
	Verify(idx_t count) const;
	//! Verify that the Vector is in a consistent, not corrupt state. DEBUG
	//! FUNCTION ONLY!
	DUCKDB_API void Verify() const;
	DUCKDB_API void Verify(const SelectionVector &sel, idx_t count) const;

	//! Returns the [index] element of the Vector as a Value.
	DUCKDB_API Value GetValue(idx_t index) const;
	//! Sets the [index] element of the Vector to the specified Value.
	DUCKDB_API void SetValue(idx_t index, const Value &val);

	void AddAuxiliaryData(unique_ptr<AuxiliaryDataHolder> data);
	void AddHeapReference(const Vector &other);

	DUCKDB_API void Append(const Value &value, VectorAppendMode append_mode = VectorAppendMode::ERROR_ON_NO_SPACE);
	DUCKDB_API void Append(const Vector &source, idx_t count,
	                       VectorAppendMode append_mode = VectorAppendMode::ERROR_ON_NO_SPACE);
	DUCKDB_API void Append(const Vector &source, const SelectionVector &sel, idx_t count,
	                       VectorAppendMode append_mode = VectorAppendMode::ERROR_ON_NO_SPACE);
	DUCKDB_API void Copy(const Vector &source, const SelectionVector &source_sel, idx_t source_count,
	                     idx_t source_offset, idx_t target_offset, idx_t copy_count);

	//! Reserve space for at least "to_reserve" elements
	DUCKDB_API void Reserve(idx_t to_reserve);
	[[deprecated("Resize has been replaced by Reserve - use Reserve(to_reserve) instead")]] DUCKDB_API void
	Resize(idx_t current_size, idx_t to_reserve);

	DUCKDB_API void Serialize(Serializer &serializer, bool compressed_serialization = true);
	DUCKDB_API void Deserialize(Deserializer &deserializer, idx_t count);

	//! Returns the uncompressed size of the data stored within this vector
	idx_t GetDataSize() const;
	idx_t GetDataSize(idx_t count) const;
	[[deprecated("This method is an alias for GetDataSize() for legacy reasons - use either that or "
	             "GetAllocationSize() instead")]] idx_t
	GetAllocationSize(idx_t cardinality) const;
	//! Returns the data allocated by the vector buffers
	idx_t GetAllocationSize() const;

	// Getters
	inline VectorType GetVectorType() const {
		auto &buffer_ref = GetBufferRef();
		if (!buffer_ref) {
			return VectorType::FLAT_VECTOR;
		}
		return buffer_ref->GetVectorType();
	}
	inline const LogicalType &GetType() const {
		return type;
	}
	inline VectorBuffer &BufferMutable() {
		return *buffer;
	}
	inline const VectorBuffer &Buffer() const {
		return *buffer;
	}
	inline const buffer_ptr<VectorBuffer> &GetBufferRef() const {
		return buffer;
	}
	void SetBuffer(buffer_ptr<VectorBuffer> buffer_p) {
		buffer = std::move(buffer_p);
	}

	// Setters
	DUCKDB_API void SetVectorType(VectorType vector_type);
	DUCKDB_API void FlattenAndSetConstant();

	// Transform vector to an equivalent dictionary vector
	static void DebugTransformToDictionary(Vector &vector);
	// Transform vector to an equivalent nested vector
	static void DebugShuffleNestedVector(Vector &vector);

	template <class T>
	VectorIterator<T> Values() const;

	template <class T>
	VectorValidValueIterator<T> ValidValues() const;

	VectorValidityIterator Validity() const;

	//! This allows a vector to reference another vector while const
	//! This is only used internally in `Flatten` - since referencing
	// an arbitrary other vector could change the logical data contained in the vector (and not be const)
	void ConstReference(const Vector &other) const;

private:
	//! Returns the [index] element of the Vector as a Value.
	static Value GetValue(const Vector &v, idx_t index);
	//! Returns the [index] element of the Vector as a Value.
	static Value GetValueInternal(const Vector &v, idx_t index);

	//! Create a vector that references the other vector
	Vector(const Vector &other, VectorConstructorAction action);

protected:
	//! The type of the elements stored in the vector (e.g. integer, float)
	LogicalType type;
	//! The main buffer holding the data of the vector
	mutable buffer_ptr<VectorBuffer> buffer;
};

} // namespace duckdb

#include "duckdb/common/vector/vector_iterator.hpp"
