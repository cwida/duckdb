#include "duckdb/common/types/variant/parquet_variant_iterator.hpp"
#include "reader/variant_column_reader.hpp"

#include "duckdb/common/types/variant/variant_builder.hpp"
#include "duckdb/function/variant/variant_shredding.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

#include <cstring>

namespace duckdb {

namespace {

//! A shredded node is either a "picked off" bare primitive (fully convertible - referenced directly), or a
//! wrapper STRUCT("typed_value" <T>, "untyped_value_index" UINTEGER) (see SetShreddedType in
//! variant_shredding.cpp). The (row-local, 1-based) untyped_value_index points at the leftover value in the
//! row's unshredded pool; NULL means "no leftover" (VARIANT_NULL / missing -> see is_object_field).
constexpr idx_t SHRED_TYPED_VALUE = 0;
constexpr idx_t SHRED_UNTYPED_INDEX = 1;

//===--------------------------------------------------------------------===//
// Analysis: which subtrees can be "picked off" (fully shredded -> flattened + referenced)
//===--------------------------------------------------------------------===//
//! Whether the group's binary 'value' column is entirely NULL (no leftover anywhere in the chunk)
bool ValueAllNull(const ShreddedGroupView &view, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		if ((*view.value)[i].IsValid()) {
			return false;
		}
	}
	return true;
}

bool LeafAllValid(const ShreddedGroupView &view, idx_t count) {
	for (idx_t idx = 0; idx < count; idx++) {
		if (!view.leaf_format.validity.RowIsValid(view.leaf_format.sel->get_index(idx))) {
			return false;
		}
	}
	return true;
}

//! A plan mirroring the group tree: 'flat' marks a primitive leaf that is fully convertible and is emitted
//! as a bare (referenced) column instead of a wrapper.
struct ShredPlan {
	bool flat = false;
	vector<ShredPlan> fields; //! OBJECT
	unique_ptr<ShredPlan> element;
};

ShredPlan AnalyzeShred(const ShreddedGroupView &view, idx_t count, bool is_object_field) {
	ShredPlan plan;
	if (!view.has_typed_value) {
		return plan; //! everything is leftover -> wrapper
	}
	switch (view.kind) {
	case ParquetGroupKind::LEAF: {
		//! A leaf is fully convertible if there is no binary leftover; an OBJECT field additionally must never
		//! be missing (a NULL typed_value of a flat field would read as VARIANT_NULL, not "missing")
		bool no_leftover = ValueAllNull(view, count);
		bool no_missing = !is_object_field || LeafAllValid(view, count);
		plan.flat = no_leftover && no_missing;
		break;
	}
	case ParquetGroupKind::OBJECT: {
		plan.fields.reserve(view.fields.size());
		for (auto &field : view.fields) {
			plan.fields.push_back(AnalyzeShred(*field, count, true));
		}
		break;
	}
	default: {
		D_ASSERT(view.kind == ParquetGroupKind::ARRAY);
		auto child_count = ListVector::GetListSize(*view.typed_value_vec);
		plan.element = make_uniq<ShredPlan>(AnalyzeShred(*view.element, child_count, false));
		break;
	}
	}
	return plan;
}

//===--------------------------------------------------------------------===//
// Stage 1: derive the shred type
//===--------------------------------------------------------------------===//
LogicalType DeriveShredNodeType(const ShreddedGroupView &view, const ShredPlan &plan);

//! The 'typed_value' (<T>) of a node: a primitive, a STRUCT of field nodes, or a LIST of an element node.
LogicalType DeriveTypedValueType(const ShreddedGroupView &view, const ShredPlan &plan) {
	if (!view.has_typed_value) {
		//! Nothing is shredded here - a placeholder all-NULL column, everything goes to the unshredded pool
		return LogicalType::INTEGER;
	}
	switch (view.kind) {
	case ParquetGroupKind::LEAF:
		return view.typed_type;
	case ParquetGroupKind::OBJECT: {
		child_list_t<LogicalType> fields;
		for (idx_t i = 0; i < view.fields.size(); i++) {
			fields.emplace_back(view.field_names[i], DeriveShredNodeType(*view.fields[i], plan.fields[i]));
		}
		return LogicalType::STRUCT(std::move(fields));
	}
	default:
		D_ASSERT(view.kind == ParquetGroupKind::ARRAY);
		return LogicalType::LIST(DeriveShredNodeType(*view.element, *plan.element));
	}
}

LogicalType DeriveShredNodeType(const ShreddedGroupView &view, const ShredPlan &plan) {
	if (plan.flat) {
		//! picked off: a bare primitive column (no untyped_value_index needed)
		return DeriveTypedValueType(view, plan);
	}
	child_list_t<LogicalType> children;
	children.emplace_back("typed_value", DeriveTypedValueType(view, plan));
	children.emplace_back("untyped_value_index", LogicalType::UINTEGER);
	return LogicalType::STRUCT(std::move(children));
}

//===--------------------------------------------------------------------===//
// Stage 2a: fill the typed_value tree (referencing Parquet leaves where possible)
//===--------------------------------------------------------------------===//
//! Reference the Parquet leaf into 'out' (BLOB stays raw to preserve the type; base64 happens at JSON time)
void FillLeafTypedValue(const ShreddedGroupView &view, Vector &out, idx_t count) {
	out.Reference(*view.typed_value_vec);
}

//! Captures the (per-node) untyped_value_index target so the leftover pass can wire it up. A "flat" node has
//! no untyped_value_index (untyped_index_data stays null) and never contributes a leftover.
struct ShredNodeWriter {
	uint32_t *untyped_index_data = nullptr;
	ValidityMask *untyped_index_validity = nullptr;
	vector<unique_ptr<ShredNodeWriter>> fields; //! OBJECT
	unique_ptr<ShredNodeWriter> element;        //! ARRAY
	//! Whether this node OR any descendant carries a binary 'value' leftover anywhere in the chunk. When
	//! false the whole subtree is fully shredded and the per-row leftover pass can skip it entirely.
	bool subtree_has_leftover = false;
};

//! Recursively NULL a shred node (and all its descendants) at 'row' - a child of a NULL struct must itself be
//! NULL. Only the synthesized parts are written: the wrapper struct validity, its untyped_value_index, and the
//! validity of any nested STRUCT/LIST typed_value. A picked-off (flat) leaf and a LEAF's referenced Parquet
//! leaf are skipped: they already read as NULL wherever their object is absent.
void SetShredNodeNull(const ShreddedGroupView &view, const ShredPlan &plan, Vector &node, idx_t row) {
	if (plan.flat) {
		return;
	}
	auto &entries = StructVector::GetEntries(node);
	auto &typed_value = entries[SHRED_TYPED_VALUE];
	auto &untyped_index = entries[SHRED_UNTYPED_INDEX];
	FlatVector::ValidityMutable(node).SetInvalid(row);
	FlatVector::ValidityMutable(untyped_index).SetInvalid(row);
	if (!view.has_typed_value) {
		//! constant-NULL placeholder typed_value
		return;
	}
	switch (view.kind) {
	case ParquetGroupKind::LEAF:
		//! typed_value references the Parquet leaf, already NULL where its object is absent
		break;
	case ParquetGroupKind::OBJECT: {
		FlatVector::ValidityMutable(typed_value).SetInvalid(row);
		auto &field_entries = StructVector::GetEntries(typed_value);
		for (idx_t f = 0; f < view.fields.size(); f++) {
			SetShredNodeNull(*view.fields[f], plan.fields[f], field_entries[f], row);
		}
		break;
	}
	default:
		D_ASSERT(view.kind == ParquetGroupKind::ARRAY);
		//! typed_value is a LIST; the entry at this row is an empty/NULL list (no element rows to recurse into)
		FlatVector::ValidityMutable(typed_value).SetInvalid(row);
		break;
	}
}

//! 'is_object_field': whether this node is a field of an OBJECT. For object fields the "no leftover" default
//! is 0 (== a missing field), since a NULL untyped_value_index means a present VARIANT_NULL value. For the
//! root / array elements (no notion of "missing") the default is NULL (== VARIANT_NULL).
void FillShredNode(const ShreddedGroupView &view, const ShredPlan &plan, Vector &node, idx_t count,
                   ShredNodeWriter &writer, bool is_object_field) {
	if (plan.flat) {
		//! picked off: 'node' is the bare primitive column - reference it, no untyped_value_index
		FillLeafTypedValue(view, node, count);
		return;
	}

	auto &entries = StructVector::GetEntries(node);
	auto &typed_value = entries[SHRED_TYPED_VALUE];
	auto &untyped_index = entries[SHRED_UNTYPED_INDEX];

	writer.untyped_index_data = FlatVector::GetDataMutable<uint32_t>(untyped_index);
	writer.untyped_index_validity = &FlatVector::ValidityMutable(untyped_index);
	if (is_object_field) {
		memset(writer.untyped_index_data, 0, count * sizeof(uint32_t));
	} else {
		writer.untyped_index_validity->SetAllInvalid(count);
	}

	//! This node has a leftover if its own binary 'value' is present for any row; OR'd below with its children
	bool has_leftover = !ValueAllNull(view, count);

	if (!view.has_typed_value) {
		//! Nothing is shredded here - the typed_value is a never-read all-NULL placeholder, so make it constant
		ConstantVector::SetNull(typed_value, count_t(count));
		writer.subtree_has_leftover = has_leftover;
		return;
	}

	switch (view.kind) {
	case ParquetGroupKind::LEAF:
		FillLeafTypedValue(view, typed_value, count);
		break;
	case ParquetGroupKind::OBJECT: {
		//! typed_value is a STRUCT of field nodes; its validity is the object's shredded-ness
		auto &dst_validity = FlatVector::ValidityMutable(typed_value);
		auto &field_entries = StructVector::GetEntries(typed_value);
		writer.fields.resize(view.fields.size());
		for (idx_t i = 0; i < view.fields.size(); i++) {
			writer.fields[i] = make_uniq<ShredNodeWriter>();
			FillShredNode(*view.fields[i], plan.fields[i], field_entries[i], count, *writer.fields[i], true);
			has_leftover |= writer.fields[i]->subtree_has_leftover;
		}
		//! Where the object is not shredded, the typed_value struct is NULL - recursively NULL the field nodes
		//! too, since a child of a NULL struct must itself be NULL
		for (idx_t i = 0; i < count; i++) {
			if (view.typed_validity->IsValid(i)) {
				continue;
			}
			dst_validity.SetInvalid(i);
			for (idx_t f = 0; f < view.fields.size(); f++) {
				SetShredNodeNull(*view.fields[f], plan.fields[f], field_entries[f], i);
			}
		}
		break;
	}
	default: {
		D_ASSERT(view.kind == ParquetGroupKind::ARRAY);
		//! typed_value is a LIST of element nodes; copy the Parquet list entries (the element child is built
		//! 1:1, so the offsets align) and recurse into the element node over the list child
		auto out_data = FlatVector::GetDataMutable<list_entry_t>(typed_value);
		auto &out_validity = FlatVector::ValidityMutable(typed_value);
		for (idx_t i = 0; i < count; i++) {
			auto entry = (*view.list)[i];
			if (!entry.IsValid()) {
				out_validity.SetInvalid(i);
				out_data[i] = list_entry_t(0, 0);
			} else {
				out_data[i] = entry.GetValueUnsafe();
			}
		}
		auto child_count = ListVector::GetListSize(*view.typed_value_vec);
		ListVector::Reserve(typed_value, child_count);
		ListVector::SetListSize(typed_value, child_count);
		writer.element = make_uniq<ShredNodeWriter>();
		FillShredNode(*view.element, *plan.element, ListVector::GetChildMutable(typed_value), child_count,
		              *writer.element, false);
		has_leftover |= writer.element->subtree_has_leftover;
		break;
	}
	}
	writer.subtree_has_leftover = has_leftover;
}

//===--------------------------------------------------------------------===//
// Stage 2b: build the unshredded pool + wire untyped_value_index (per row)
//===--------------------------------------------------------------------===//
bool TypedValid(const ShreddedGroupView &view, idx_t index) {
	switch (view.kind) {
	case ParquetGroupKind::LEAF:
		return view.leaf_format.validity.RowIsValid(view.leaf_format.sel->get_index(index));
	case ParquetGroupKind::ARRAY:
		return (*view.list)[index].IsValid();
	default:
		D_ASSERT(view.kind == ParquetGroupKind::OBJECT);
		return view.typed_validity->IsValid(index);
	}
}

//! A root that resolves to a (variant) NULL / missing is a genuine SQL NULL row (matches the unshredded path)
bool IsRowNull(const ParquetVariantNode &root) {
	return root.IsNull() || root.GetTypeId() == VariantLogicalType::VARIANT_NULL;
}

//! Set the shredded struct validity for the SQL NULL rows (used when there is no leftover to build, so the
//! per-row null marking otherwise done by ShreddedLeftoverSource still has to happen)
void MarkShreddedNullRows(ParquetVariantIterator &iterator, idx_t count, ValidityMask &shredded_validity) {
	for (idx_t row = 0; row < count; row++) {
		iterator.BeginRow(row);
		if (IsRowNull(iterator.Root(row))) {
			shredded_validity.SetInvalid(row);
		}
	}
}

struct ShreddedLeftoverSource {
	ParquetVariantIterator &iterator;
	ShredNodeWriter &root_writer;
	ValidityMask &shredded_validity;

	bool Emit(idx_t row, VariantBuilder &builder) {
		iterator.BeginRow(row);
		auto root = iterator.Root(row);
		if (IsRowNull(root)) {
			shredded_validity.SetInvalid(row);
			return true;
		}
		EmitNode(iterator.GetRootView(), root_writer, row, builder);
		return false;
	}

	//! Emit the binary value (one subtree) into the unshredded pool, recording its 1-based row-local index in
	//! this node's untyped_value_index
	void EmitLeftover(const string_t &value, ShredNodeWriter &writer, idx_t index, VariantBuilder &builder) {
		auto data = const_data_ptr_cast(value.GetData());
		auto local = builder.LocalValue();
		iterator.EmitBinary(data, data + value.GetSize(), builder);
		writer.untyped_index_data[index] = local + 1;
		writer.untyped_index_validity->SetValid(index);
	}

	void EmitNode(const ShreddedGroupView &view, ShredNodeWriter &writer, idx_t index, VariantBuilder &builder) {
		auto value_entry = (*view.value)[index];
		bool value_present = value_entry.IsValid();
		bool typed_valid = view.has_typed_value && TypedValid(view, index);

		if (typed_valid) {
			if (view.kind == ParquetGroupKind::OBJECT) {
				//! A partially-shredded object: the 'value' blob holds the leftover (overlay) fields
				if (value_present) {
					EmitLeftover(value_entry.GetValueUnsafe(), writer, index, builder);
				}
				//! Descend only into fields whose subtree carries a leftover somewhere in the chunk; a
				//! fully-shredded field contributes nothing and its untyped_value_index keeps its default
				for (idx_t i = 0; i < view.fields.size(); i++) {
					if (writer.fields[i]->subtree_has_leftover) {
						EmitNode(*view.fields[i], *writer.fields[i], index, builder);
					}
				}
			} else if (view.kind == ParquetGroupKind::ARRAY) {
				//! Same pruning for arrays: a fully-shredded element type means no element can have a leftover
				if (writer.element->subtree_has_leftover) {
					auto entry = (*view.list)[index].GetValueUnsafe();
					for (idx_t j = 0; j < entry.length; j++) {
						EmitNode(*view.element, *writer.element, entry.offset + j, builder);
					}
				}
			}
			//! LEAF (incl. picked-off): fully shredded, no leftover
		} else if (value_present) {
			//! Not shredded - the whole value is a leftover (do not recurse: the subtree lives in this value)
			EmitLeftover(value_entry.GetValueUnsafe(), writer, index, builder);
		}
		//! else: missing / VARIANT_NULL - leave untyped_value_index at its default
	}
};

} // namespace

void VariantColumnReader::Convert(Vector &metadata, Vector &group, Vector &result, idx_t count) {
	ParquetVariantIterator iterator(metadata, group);
	auto &root_view = iterator.GetRootView();

	//! Analysis + Stage 1: pick off fully-convertible subtrees, derive STRUCT("unshredded", "shredded")
	auto root_plan = AnalyzeShred(root_view, count, false);
	child_list_t<LogicalType> shredded_data_children;
	shredded_data_children.emplace_back("unshredded", VariantShredding::GetUnshreddedType());
	shredded_data_children.emplace_back("shredded", DeriveShredNodeType(root_view, root_plan));
	auto shredded_data_type = LogicalType::STRUCT(std::move(shredded_data_children));

	PrepareChunk(shredded_chunk, shredded_capacity, {shredded_data_type}, count);
	auto &shredded_data = shredded_chunk.data[0];

	auto &shredded_data_entries = StructVector::GetEntries(shredded_data);
	auto &unshredded = shredded_data_entries[0];
	auto &shredded = shredded_data_entries[1];

	if (root_plan.flat) {
		//! The whole column is a fully-shredded primitive: reference it; the unshredded pool is unused
		FillLeafTypedValue(root_view, shredded, count);
		BuildEmptyVariant(count, unshredded);
	} else {
		//! Stage 2a: fill the typed_value tree (referencing leaves that map exactly); this also computes, per
		//! node, whether its subtree carries any leftover (root_writer.subtree_has_leftover for the whole vector)
		ShredNodeWriter root_writer;
		FillShredNode(root_view, root_plan, shredded, count, root_writer, false);
		if (root_writer.subtree_has_leftover) {
			//! Stage 2b: build the unshredded pool from leftovers, wire untyped_value_index, set row validity
			ShreddedLeftoverSource source {iterator, root_writer, FlatVector::ValidityMutable(shredded)};
			BuildVariant(source, count, unshredded);
		} else {
			//! No leftover anywhere in this vector: skip building the (never-consulted) unshredded pool, only
			//! mark the SQL NULL rows on the shredded component
			MarkShreddedNullRows(iterator, count, FlatVector::ValidityMutable(shredded));
			BuildEmptyVariant(count, unshredded);
		}
	}

	FlatVector::SetSize(shredded_data, count_t(count));
	result.Shred(shredded_data, count);
}

} // namespace duckdb
