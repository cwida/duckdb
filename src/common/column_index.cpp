#include "duckdb/common/column_index.hpp"

namespace duckdb {

void ColumnIndex::SetPushdownExtractType(const LogicalType &type_information,
                                         optional_ptr<const LogicalType> cast_type) {
	//! We can upgrade the optional prune hint to a PUSHDOWN_EXTRACT, which is no longer optional
	index_type = ColumnIndexType::PUSHDOWN_EXTRACT;
	type = type_information;
	D_ASSERT(child_indexes.size() == 1);

	auto &child = child_indexes[0];
	if (child.HasPrimaryIndex()) {
		auto &child_types = StructType::GetChildTypes(type);
		auto &child_type = child_types[child.GetPrimaryIndex()].second;
		if (child.child_indexes.empty()) {
			if (cast_type) {
				child.SetType(*cast_type);
			} else {
				child.SetType(child_type);
			}
		} else {
			child.SetPushdownExtractType(child_type, cast_type);
		}
	} else {
		D_ASSERT(type_information.id() == LogicalTypeId::VARIANT);
		if (child.child_indexes.empty()) {
			if (cast_type) {
				child.SetType(*cast_type);
			} else {
				//! Without a cast, the child will always be VARIANT
				child.SetType(type_information);
			}
		} else {
			child.SetPushdownExtractType(type_information, cast_type);
		}
	}
}
const LogicalType &ColumnIndex::GetScanType() const {
	D_ASSERT(HasType());
	if (IsPushdownExtract()) {
		return child_indexes[0].GetScanType();
	}
	return GetType();
}
const LogicalType &ColumnIndex::GetType() const {
	D_ASSERT(type.id() != LogicalTypeId::INVALID);
	return type;
}
void ColumnIndex::AddChildIndex(ColumnIndex new_index) {
	this->child_indexes.push_back(std::move(new_index));
}
bool ColumnIndex::IsRowIdColumn() const {
	if (!has_index) {
		return false;
	}
	return index == COLUMN_IDENTIFIER_ROW_ID;
}
bool ColumnIndex::IsRowNumberColumn() const {
	if (!has_index) {
		return false;
	}
	return index == COLUMN_IDENTIFIER_ROW_NUMBER;
}
bool ColumnIndex::IsEmptyColumn() const {
	if (!has_index) {
		return false;
	}
	return index == COLUMN_IDENTIFIER_EMPTY;
}
bool ColumnIndex::IsVirtualColumn() const {
	if (!has_index) {
		return false;
	}
	return index >= VIRTUAL_COLUMN_START;
}
void ColumnIndex::VerifySinglePath() const {
	if (child_indexes.empty()) {
		return;
	}
	if (child_indexes.size() != 1) {
		throw InternalException(
		    "We were expecting to find a single path in the index, meaning 0 or 1 children, found: %d",
		    child_indexes.size());
	}
	child_indexes[0].VerifySinglePath();
}
bool ColumnIndex::IsChildPathOf(const ColumnIndex &path) const {
	VerifySinglePath();
	path.VerifySinglePath();
	reference<const ColumnIndex> a(*this);
	reference<const ColumnIndex> b(path);

	while (true) {
		if (a.get().HasPrimaryIndex()) {
			if (!b.get().HasPrimaryIndex()) {
				return false;
			}
			if (a.get().GetPrimaryIndex() != b.get().GetPrimaryIndex()) {
				return false;
			}
		} else {
			if (b.get().HasPrimaryIndex()) {
				return false;
			}
			if (a.get().GetFieldName() != b.get().GetFieldName()) {
				return false;
			}
		}
		if (a.get().type != b.get().type) {
			// A cast parent cannot replace a path through the uncast value.
			return false;
		}
		const bool a_has_children = a.get().HasChildren();
		const bool b_has_children = b.get().HasChildren();
		if (!a_has_children && !b_has_children) {
			return false;
		}
		if (!a_has_children) {
			//! a's path has stopped short of b's path
			return false;
		}
		if (!b_has_children) {
			//! b's path is a subset of a's path, so it's a parent path
			return true;
		}
		a = a.get().GetChildIndexes()[0];
		b = b.get().GetChildIndexes()[0];
	}
	return true;
}

string ColumnIndex::GetName(const string &column_name) const {
	if (index_type != ColumnIndexType::PUSHDOWN_EXTRACT) {
		return column_name;
	}
	D_ASSERT(child_indexes.size() == 1);
	auto &child = child_indexes[0];
	string current_name = column_name + ".";
	if (child.HasPrimaryIndex()) {
		//! 'type' is the parent struct type at this level; child.GetPrimaryIndex() is the field's index within it
		current_name += StructType::GetChildName(type, child.GetPrimaryIndex());
	} else {
		current_name += child.GetFieldName();
	}
	return child.GetName(current_name);
}

} // namespace duckdb
