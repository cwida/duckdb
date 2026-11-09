//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/variant_iterator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/vector/unified_vector_format.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

class Vector;
struct UnifiedVectorFormat;

//! The order in which the children of an OBJECT are iterated
enum class VariantIterationOrder {
	//! The order in which the children are physically stored (no sorting)
	INTERNAL,
	//! Sorted by object key
	LEXICOGRAPHIC
};

//===--------------------------------------------------------------------===//
// VariantIterator
//===--------------------------------------------------------------------===//
// VariantIterator iterates over the logical values of a VARIANT vector *without* unshredding it,
// handing out a VariantNode cursor per row (each cursor points at a single logical value/node).
//
// A VARIANT vector is either stored in its canonical "unshredded" layout:
//     STRUCT(
//         keys VARCHAR[],
//         children STRUCT(key_id UINTEGER, value_id UINTEGER)[],
//         values STRUCT(type_id UTINYINT, byte_offset UINTEGER)[],
//         data BLOB
//     )
// or as a SHREDDED_VECTOR which combines a typed ("shredded") representation with the leftover
// "unshredded" component for everything that did not fit the shredded schema.
//
// The unshredded component is read through plain VectorIterator<>s over the layout above (see
// UnshreddedVariantLayout) - no RecursiveUnifiedVectorFormat / UnifiedVariantVectorData is involved.
// For a shredded vector the iterator descends into the typed shredded tree directly and only falls
// back to these unshredded iterators for the leftover values, merging the two on the fly.

//! The VectorIterator<> layout of the canonical (unshredded) VARIANT representation

//! Reads the canonical (unshredded) VARIANT layout (see UnshreddedVariantLayout) through iterators
class UnshreddedVariantIterator {
	using UnshreddedVariantLayout =
	    VectorStructType<VectorListType<string_t>, VectorListType<VectorStructType<uint32_t, uint32_t>>,
	                     VectorListType<VectorStructType<uint8_t, uint32_t>>, string_t>;

public:
	explicit UnshreddedVariantIterator(const Vector &unshredded);

public:
	//! Whether the top-level row is valid (non-NULL)
	bool RowIsValid(idx_t row) const;
	//! The type_id of values[value_index]
	VariantLogicalType GetTypeId(idx_t row, idx_t value_index) const;
	//! The byte_offset of values[value_index]
	uint32_t GetByteOffset(idx_t row, idx_t value_index) const;
	//! The data BLOB of the row. Returned by reference: the inlined bytes of a returned string_t copy
	//! would otherwise dangle once the temporary is destroyed.
	const string_t &GetBlob(idx_t row) const;
	//! The key string of keys[key_index]
	string_t GetKey(idx_t row, idx_t key_index) const;
	//! The key_id of children[child_index]
	uint32_t GetKeysIndex(idx_t row, idx_t child_index) const;
	//! The value_id of children[child_index]
	uint32_t GetValuesIndex(idx_t row, idx_t child_index) const;

private:
	VectorIterator<UnshreddedVariantLayout> data;
};

//! A recursive, self-similar view over the typed ("shredded") component of a variant
struct ShreddedVariantIterator {
public:
	//! Recursively populates this node (and its children) from a shredded variant Vector
	void Build(const Vector &vec);

public:
	//! The unified format of this layer
	UnifiedVectorFormat unified;
	//! The children layers (list/array element, or struct fields)
	vector<ShreddedVariantIterator> children;
	//! The logical type of this layer
	LogicalType logical_type;
	//! The type_id shared by all values of this layer, valid only if has_fixed_type_id is set
	VariantLogicalType type_id = VariantLogicalType::VARIANT_NULL;
	//! Whether type_id holds for every value of this layer (BOOLEAN resolves per value)
	bool has_fixed_type_id = false;
	//! The size of the physical payload of this layer
	idx_t type_size = 0;
};

class VariantNode;

//! Shared state required to iterate a single VARIANT vector. Owns the vector iterators / flattened
//! vectors that the individual VariantNode cursors point into - so it must outlive any cursor.
class VariantIterator {
public:
	explicit VariantIterator(const Vector &variant);
	//! Build directly from the separate unshredded + shredded components (the STRUCT(unshredded, shredded)
	//! intermediate produced during unshredding, which is not itself a SHREDDED_VECTOR)
	VariantIterator(const Vector &unshredded, const Vector &shredded);

public:
	//! Whether the row is a (SQL) NULL variant
	bool RowIsValid(idx_t row) const;
	//! Returns a cursor pointing at the root value of the given row
	VariantNode Root(idx_t row) const;

private:
	//! The "core": the unshredded component reader (plain vector iterators)
	UnshreddedVariantIterator unshredded;

	//! Whether the variant vector is shredded
	bool is_shredded = false;
	//! The shredded component - the (recursive) view of the root of the shredded tree
	ShreddedVariantIterator shredded_format;

	friend class VariantNode;
	friend class VariantArrayIterator;
	friend class VariantObjectIterator;
};

class VariantArrayIterator;
class VariantObjectIterator;

//! A lightweight cursor pointing at a single logical VARIANT value.
class VariantNode {
public:
	enum class Kind {
		NULL_VALUE, //! a (SQL/variant) NULL value
		MISSING,    //! an absent value (e.g. a missing object field)
		UNSHREDDED, //! a value living in the unshredded component
		SHREDDED    //! a value living in the shredded (typed) component
	};

public:
	VariantNode() : state(nullptr), kind(Kind::NULL_VALUE) {
	}

public:
	bool IsNull() const {
		return kind == Kind::NULL_VALUE;
	}
	bool IsMissing() const {
		return kind == Kind::MISSING;
	}

	//! The logical type of the value the cursor points at
	VariantLogicalType GetTypeId() const;

	//! Returns the fixed-width primitive payload loaded as T (e.g. GetData<int32_t>()). For a DECIMAL the
	//! payload follows the (width, scale) prefix, so T must match the physical type implied by the width.
	template <class T>
	T GetData() const {
		if (GetTypeId() == VariantLogicalType::DECIMAL) {
			return Load<T>(GetDecimal().value_ptr);
		}
		return Load<T>(GetDataPointer());
	}
	//! Returns the (variable-length) string payload of a VARCHAR/BLOB/BIGNUM/GEOMETRY/BITSTRING value
	string_t GetString() const;
	//! Returns the (width, scale) of a DECIMAL value
	VariantDecimalProperties GetDecimalProperties() const {
		auto decimal = GetDecimal();
		return VariantDecimalProperties(decimal.width, decimal.scale);
	}

	//! Returns a pointer to the raw payload of a fixed-width primitive value
	const_data_ptr_t GetDataPointer() const;
	//! Returns the decimal payload of a DECIMAL value
	VariantDecimalData GetDecimal() const;

	//! Iterates the (key, value) children of an OBJECT value (merging shredded + unshredded) in the
	//! requested order. LEXICOGRAPHIC sorts by key (currently materialized + sorted up-front).
	VariantObjectIterator GetObjectChildren(VariantIterationOrder order = VariantIterationOrder::INTERNAL) const;
	//! Lazily iterates the element values of an ARRAY value
	VariantArrayIterator GetArrayChildren() const;

private:
	//! Resolve the shredded node (a "STRUCT(typed_value, [untyped_value_index])" wrapper, or a
	//! flattened primitive) at the given index into a concrete cursor
	static VariantNode ResolveShredded(const VariantIterator &state, const ShreddedVariantIterator &node, idx_t index,
	                                   idx_t row);

	static VariantNode MakeUnshredded(const VariantIterator &state, idx_t row, uint32_t value_index);
	static VariantNode MakeShredded(const VariantIterator &state, const ShreddedVariantIterator &content, idx_t index,
	                                idx_t row, uint32_t overlay_value_index);
	static VariantNode MakeNull(const VariantIterator &state);
	static VariantNode MakeMissing(const VariantIterator &state);

private:
	//! The owning iterator this value lives in (null only for a default-constructed cursor)
	optional_ptr<const VariantIterator> state;
	Kind kind;

	//! The row this value belongs to (used for the unshredded component / overlay lookups)
	idx_t row = 0;
	//! UNSHREDDED: 0-based index into the 'values' array of the unshredded component
	uint32_t value_index = 0;

	//! SHREDDED: the current layer in the shredded format tree (the resolved 'typed_value', i.e. the
	//! object struct, the array list, or the primitive)
	optional_ptr<const ShreddedVariantIterator> shredded_format;
	//! SHREDDED: the (logical) index into shredded_format
	idx_t shredded_index = 0;
	//! SHREDDED OBJECT: 1-based index into the unshredded component holding the leftover fields
	//! (0 means there is no leftover object to merge)
	uint32_t overlay_value_index = 0;

	friend class VariantIterator;
	friend class VariantArrayIterator;
	friend class VariantObjectIterator;
};

//! Lazily iterates the element values of an ARRAY VariantNode. Random-access: no child cursor is
//! materialized until it is dereferenced.
class VariantArrayIterator {
public:
	explicit VariantArrayIterator(const VariantNode &array);

public:
	idx_t size() const {
		return length;
	}
	VariantNode operator[](idx_t i) const;

	class Iterator {
	public:
		Iterator(const VariantArrayIterator &parent, idx_t pos) : parent(parent), pos(pos) {
		}
		VariantNode operator*() const {
			return parent[pos];
		}
		Iterator &operator++() { // NOLINT: match stl API
			++pos;
			return *this;
		}
		bool operator!=(const Iterator &other) const {
			return pos != other.pos;
		}

	private:
		const VariantArrayIterator &parent;
		idx_t pos;
	};
	Iterator begin() const { // NOLINT: match stl API
		return Iterator(*this, 0);
	}
	Iterator end() const { // NOLINT: match stl API
		return Iterator(*this, length);
	}

private:
	//! The owning iterator this array's elements live in (always non-null for a real ARRAY node)
	reference<const VariantIterator> state;
	idx_t row;
	idx_t length;
	bool shredded;
	//! UNSHREDDED: the 'children' base index; SHREDDED: the list offset of the array's elements
	idx_t base;
	//! SHREDDED: the element layer of the array
	optional_ptr<const ShreddedVariantIterator> element_node;
};

//! A single (key, value) entry of an OBJECT
struct VariantObjectEntry {
	string_t key;
	VariantNode value;
};

//! Iterates the (key, value) children of an OBJECT VariantNode, merging the shredded (typed)
//! fields with the leftover unshredded fields. There are two backing modes:
//!   - INTERNAL order: lazy forward iteration over the raw entries (skipping missing fields)
//!   - LEXICOGRAPHIC order: iterates the materialized + sorted 'ordered_entries'
class VariantObjectIterator {
public:
	VariantObjectIterator(const VariantNode &object, VariantIterationOrder order);

public:
	//! Forward iterator over the object entries. All modes are position-based, so the only difference is
	//! how an entry at a position is produced (see Load).
	class Iterator {
	public:
		Iterator(const VariantObjectIterator &parent, idx_t pos) : parent(parent), pos(pos) {
			Load();
		}
		const VariantObjectEntry &operator*() const {
			return current;
		}
		const VariantObjectEntry *operator->() const {
			return &current;
		}
		Iterator &operator++() { // NOLINT: match stl API
			++pos;
			Load();
			return *this;
		}
		bool operator!=(const Iterator &other) const {
			return pos != other.pos;
		}

	private:
		//! Loads the entry at the current position, skipping missing fields
		void Load();

	private:
		const VariantObjectIterator &parent;
		idx_t pos;
		VariantObjectEntry current;
	};
	Iterator begin() const { // NOLINT: match stl API
		return Iterator(*this, 0);
	}
	Iterator end() const { // NOLINT: match stl API
		return Iterator(*this, EndPos());
	}

private:
	//! Materializes the entry at the given raw position (before missing-field skipping)
	VariantObjectEntry RawEntry(idx_t raw_pos) const;
	//! The end position in the iteration space
	idx_t EndPos() const {
		return order == VariantIterationOrder::LEXICOGRAPHIC ? ordered_entries.size() : raw_count;
	}

private:
	//! The owning iterator this object's values live in (always non-null for a real OBJECT node)
	reference<const VariantIterator> state;
	idx_t row;
	VariantIterationOrder order;
	bool shredded;
	//! Total number of raw entries (typed fields + leftover fields), before missing-field skipping
	idx_t raw_count;

	//! UNSHREDDED: the 'children' base index of the object
	idx_t base;

	//! SHREDDED: the object's typed struct layer + its index, the number of typed fields, and the
	//! 'children' base index of the leftover (overlay) object
	optional_ptr<const ShreddedVariantIterator> content;
	idx_t shredded_index;
	idx_t typed_field_count;
	idx_t overlay_base;

	//! LEXICOGRAPHIC: the entries materialized and sorted by key up-front
	vector<VariantObjectEntry> ordered_entries;

	friend class Iterator;
};

//! Specialization of VectorIterator for VectorVariantType.
//! Iterates over a VARIANT vector, handing out a VariantNode cursor per row via operator[].
//! The cursors point back into the owned VariantIterator, so this iterator must outlive them.
template <>
class VectorIterator<VectorVariantType> {
public:
	explicit VectorIterator(const Vector &vector) : state(vector), count(vector.size()) {
	}

public:
	//! Returns a cursor pointing at the root VARIANT value of the given row
	VariantNode operator[](idx_t row) const {
		return state.Root(row);
	}
	//! Whether the row is a valid (non-NULL) variant
	bool RowIsValid(idx_t row) const {
		return state.RowIsValid(row);
	}
	idx_t size() const {
		return count;
	}

	class Iterator {
	public:
		Iterator(const VectorIterator &parent, idx_t index) : parent(parent), index(index) {
		}
		VariantNode operator*() const {
			return parent[index];
		}
		Iterator &operator++() { // NOLINT: match stl API
			++index;
			return *this;
		}
		bool operator!=(const Iterator &other) const {
			return index != other.index;
		}

	private:
		const VectorIterator &parent;
		idx_t index;
	};
	Iterator begin() const { // NOLINT: match stl API
		return Iterator(*this, 0);
	}
	Iterator end() const { // NOLINT: match stl API
		return Iterator(*this, count);
	}

private:
	VariantIterator state;
	idx_t count;
};

} // namespace duckdb
