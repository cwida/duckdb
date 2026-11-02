//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/variant/parquet_variant_iterator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/unified_vector_format.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/types/variant/variant_binary_decoder.hpp"

namespace duckdb {

struct VariantBuilder;

//! A "group" in the Parquet shredded VARIANT layout: STRUCT(value BLOB, [typed_value <T>]).
//! The recursive view builds the (type-specific) VectorIterators of the group tree once, so each layer
//! can be navigated by index during the (per-row) single-pass conversion.
enum class ParquetGroupKind : uint8_t { LEAF, OBJECT, ARRAY };

struct ShreddedGroupView {
	//! 'value' (the binary-encoded fallback / overlay)
	unique_ptr<VectorIterator<string_t>> value;

	//! Whether the group has a 'typed_value' (a shredded representation)
	bool has_typed_value = false;
	ParquetGroupKind kind = ParquetGroupKind::LEAF;
	LogicalType typed_type;
	//! The raw 'typed_value' Vector (LEAF primitive / OBJECT struct / ARRAY list) - used to Reference it
	//! directly into a shredded output (see ParquetVariantConversion::ConvertToShredded)
	optional_ptr<Vector> typed_value_vec;

	//! LEAF: the typed primitive values (type-erased; read typed via GetData<T> where T is known)
	UnifiedVectorFormat leaf_format;

	//! ARRAY: the list entries + the element (group) sub-view
	unique_ptr<VectorIterator<list_entry_t>> list;
	unique_ptr<ShreddedGroupView> element;

	//! OBJECT: the struct's validity + the field names + their (group) sub-views
	unique_ptr<VectorValidityIterator> typed_validity;
	vector<string> field_names;
	vector<unique_ptr<ShreddedGroupView>> fields;

	//! Build the view recursively from a group Vector
	DUCKDB_API void Build(Vector &group);
};

class ParquetVariantIterator;
class ParquetObjectIterator;
class ParquetArrayIterator;

//! A lightweight cursor pointing at a single logical Parquet VARIANT value. A value is either SHREDDED
//! (a position in a typed vector tree) or BINARY (a Spark variant-encoded value at a byte offset in a
//! 'value' blob). Both expose the same node concept, so the shared EmitIterator traverses them uniformly
//! and emits the correct VariantLogicalType.
class ParquetVariantNode {
public:
	enum class Kind : uint8_t { NULL_VALUE, MISSING, SHREDDED, BINARY };

public:
	ParquetVariantNode() : kind(Kind::NULL_VALUE) {
	}

public:
	static ParquetVariantNode MakeNull() {
		return ParquetVariantNode(Kind::NULL_VALUE);
	}
	static ParquetVariantNode MakeMissing() {
		return ParquetVariantNode(Kind::MISSING);
	}
	//! A position in the shredded (typed) tree. 'overlay' is the binary OBJECT holding the leftover fields
	//! of a partially-shredded object (or null when fully shredded / not an object); 'overlay_end' is one
	//! past the end of the overlay's 'value' blob (for bounds-checking the binary reads).
	static ParquetVariantNode MakeShredded(const ParquetVariantIterator &state, const ShreddedGroupView &view,
	                                       idx_t index, const_data_ptr_t overlay = nullptr,
	                                       const_data_ptr_t overlay_end = nullptr) {
		ParquetVariantNode result(Kind::SHREDDED);
		result.state = &state;
		result.view = &view;
		result.index = index;
		result.binary = overlay;
		result.binary_end = overlay_end;
		return result;
	}
	//! A Spark variant-encoded value starting at 'data' (its header byte); 'end' is one past the end of the
	//! 'value' blob the value lives in (for bounds-checking the binary reads).
	static ParquetVariantNode MakeBinary(const ParquetVariantIterator &state, const_data_ptr_t data,
	                                     const_data_ptr_t end) {
		ParquetVariantNode result(Kind::BINARY);
		result.state = &state;
		result.binary = data;
		result.binary_end = end;
		return result;
	}

public:
	bool IsNull() const {
		return kind == Kind::NULL_VALUE;
	}
	bool IsMissing() const {
		return kind == Kind::MISSING;
	}

	DUCKDB_API VariantLogicalType GetTypeId() const;
	//! Returns the fixed-width primitive payload (loaded / re-encoded as T)
	template <class T>
	T GetData() const;
	DUCKDB_API string_t GetString() const;
	DUCKDB_API VariantDecimalProperties GetDecimalProperties() const;
	DUCKDB_API ParquetObjectIterator GetObjectChildren(VariantIterationOrder order) const;
	DUCKDB_API ParquetArrayIterator GetArrayChildren() const;

private:
	explicit ParquetVariantNode(Kind kind) : kind(kind) {
	}

private:
	Kind kind;
	optional_ptr<const ParquetVariantIterator> state;
	//! SHREDDED: the shredded position
	optional_ptr<const ShreddedGroupView> view;
	idx_t index = 0;
	//! SHREDDED OBJECT: the (binary) overlay object holding leftover fields, or null.
	//! BINARY: the value's start (header byte).
	const_data_ptr_t binary = nullptr;
	//! One past the end of the 'value' blob 'binary' points into (for bounds-checking the binary reads)
	const_data_ptr_t binary_end = nullptr;
};

//! A single (key, value) entry of an OBJECT
struct ParquetObjectEntry {
	string_t key;
	ParquetVariantNode value;
};

//! Iterates the (key, value) children of an OBJECT: for a shredded object, the typed struct fields merged
//! with the leftover (binary overlay) fields - deduplicated (typed fields win) and sorted lexicographically;
//! for a binary object, the encoded fields.
class ParquetObjectIterator {
public:
	//! Shredded object (typed fields + optional binary overlay)
	DUCKDB_API ParquetObjectIterator(const ParquetVariantIterator &state, const ShreddedGroupView &view, idx_t index,
	                                 const_data_ptr_t overlay, const_data_ptr_t overlay_end);
	//! Binary object
	DUCKDB_API ParquetObjectIterator(const ParquetVariantIterator &state, const VariantMetadata &metadata,
	                                 const_data_ptr_t data, const_data_ptr_t end);

public:
	const ParquetObjectEntry *begin() const { // NOLINT: match stl API
		return ordered_entries.data();
	}
	const ParquetObjectEntry *end() const { // NOLINT: match stl API
		return ordered_entries.data() + ordered_entries.size();
	}

private:
	void Finalize();

private:
	vector<ParquetObjectEntry> ordered_entries;
};

//! Iterates the element values of an ARRAY (shredded list or binary array). Random-access.
class ParquetArrayIterator {
public:
	//! Shredded array
	DUCKDB_API ParquetArrayIterator(const ParquetVariantIterator &state, const ShreddedGroupView &view, idx_t index);
	//! Binary array
	DUCKDB_API ParquetArrayIterator(const ParquetVariantIterator &state, const VariantMetadata &metadata,
	                                const_data_ptr_t data, const_data_ptr_t end);

public:
	idx_t size() const {
		return length;
	}
	DUCKDB_API ParquetVariantNode operator[](idx_t i) const;

private:
	reference<const ParquetVariantIterator> state;
	bool shredded;
	idx_t length;

	//! SHREDDED: the element (group) sub-view + the list offset of the elements
	optional_ptr<const ShreddedGroupView> element;
	idx_t base = 0;

	//! BINARY: where to read each element's value offset, and the values base
	const_data_ptr_t field_offsets = nullptr;
	const_data_ptr_t values = nullptr;
	//! BINARY: one past the end of the 'value' blob (for bounds-checking the binary reads)
	const_data_ptr_t binary_end = nullptr;
	uint32_t field_offset_size = 0;
};

//! Iterates a shredded Parquet VARIANT column (metadata + group), handing out ParquetVariantNode cursors
//! per row. Binary (Spark-encoded) values are read directly from the 'value' blobs; fixed-width payloads
//! are fetched by value (no materialization).
class ParquetVariantIterator {
public:
	DUCKDB_API ParquetVariantIterator(Vector &metadata, Vector &group);
	//! Binary-only: each row is a full Spark variant-encoded value (the metadata blob immediately followed
	//! by the value blob). There is no shredded group - the value is read right after the metadata.
	DUCKDB_API explicit ParquetVariantIterator(Vector &metadata);

public:
	//! Reset the per-row state (lazily-decoded metadata) for a new row
	DUCKDB_API void BeginRow(idx_t row);
	//! Resolve the root value of 'row' (a missing root is promoted to a SQL NULL)
	DUCKDB_API ParquetVariantNode Root(idx_t row) const;
	//! Resolve the root of a binary-only row: the value blob starts right after the metadata
	DUCKDB_API ParquetVariantNode BinaryRoot() const;
	//! Resolve the value of the group 'view' at logical position 'index'
	DUCKDB_API ParquetVariantNode ResolveGroup(const ShreddedGroupView &view, idx_t index) const;

	//! The (lazily-decoded) Variant metadata of the current row
	DUCKDB_API const VariantMetadata &GetMetadata() const;

	//! The recursive view of the Parquet group tree (used by the shredded-conversion path)
	const ShreddedGroupView &GetRootView() const {
		return root_view;
	}
	//! Emit the binary value in ['data', 'end') of the current row into the builder (BeginRow must precede)
	DUCKDB_API void EmitBinary(const_data_ptr_t data, const_data_ptr_t end, VariantBuilder &builder) const;

private:
	ShreddedGroupView root_view;

	VectorIterator<string_t> metadata;

	idx_t current_row = 0;
	mutable unique_ptr<VariantMetadata> current_metadata;
};

//! Convert a Parquet VARIANT (metadata + group) into DuckDB's SHREDDED VARIANT format: the Parquet
//! typed_value columns are referenced directly where they map exactly, and leftover/binary 'value' data
//! (including the entire value when there is no 'typed_value' at all) goes into the unshredded component.
class ParquetVariantConversion {
public:
	//! Convert binary Variant values (each row being the metadata blob followed by the value blob) into the
	//! canonical VARIANT 'result' in a single pass
	DUCKDB_API static void ConvertBinary(Vector &metadata_and_value, Vector &result, idx_t count);

	//! Convert VARIANT values into the Parquet Variant layout declared by 'result's type —
	//! struct<metadata BLOB, value BLOB[, typed_value ...]>. This is 'variant_to_parquet_variant's
	//! conversion, and also the storage of the arrow.parquet.variant canonical extension type.
	DUCKDB_API static void ToParquetVariant(const Vector &variant, idx_t count, Vector &result);
};

} // namespace duckdb
