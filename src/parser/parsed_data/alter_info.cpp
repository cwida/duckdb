#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/catalog/dependency_list.hpp"

#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"

namespace duckdb {

AlterInfo::AlterInfo(AlterType type, QualifiedName name_p, OnEntryNotFound if_not_found)
    : ParseInfo(TYPE), type(type), if_not_found(if_not_found), allow_internal(false),
      qualified_name(std::move(name_p)) {
}

AlterInfo::AlterInfo(AlterType type) : ParseInfo(TYPE), type(type) {
}

AlterInfo::~AlterInfo() {
}

AlterEntryData AlterInfo::GetAlterEntryData() const {
	return AlterEntryData(GetQualifiedName(), if_not_found);
}

DatabaseModificationType AlterInfo::GetModifications() const {
	if (type != AlterType::ALTER_TABLE) {
		return DatabaseModificationType();
	}
	switch (Cast<AlterTableInfo>().alter_table_type) {
	case AlterTableType::RENAME_COLUMN:
	case AlterTableType::RENAME_TABLE:
	case AlterTableType::SET_DEFAULT:
	case AlterTableType::DROP_NOT_NULL:
	case AlterTableType::SET_COLUMN_COMMENT:
		// the new version of the table keeps the storage of the old one
		return DatabaseModificationType();
	default:
		return DatabaseModificationType::ALTER_TABLE;
	}
}

bool AlterInfo::IsAddUniqueConstraint() const {
	if (type != AlterType::ALTER_TABLE) {
		return false;
	}

	auto &table_info = Cast<AlterTableInfo>();
	if (table_info.alter_table_type != AlterTableType::ADD_CONSTRAINT) {
		return false;
	}

	auto &constraint_info = table_info.Cast<AddConstraintInfo>();
	return constraint_info.constraint->type == ConstraintType::UNIQUE;
}

bool AlterInfo::IsAddPrimaryKey() const {
	if (!IsAddUniqueConstraint()) {
		return false;
	}

	auto &table_info = Cast<AlterTableInfo>();
	auto &constraint_info = table_info.Cast<AddConstraintInfo>();
	return constraint_info.constraint->Cast<UniqueConstraint>().IsPrimaryKey();
}

} // namespace duckdb
