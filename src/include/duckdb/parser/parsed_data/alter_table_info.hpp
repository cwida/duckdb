//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/alter_table_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/result_modifier.hpp"

#include "duckdb/common/identifier.hpp"
namespace duckdb {

enum class AlterForeignKeyType : uint8_t { AFT_ADD = 0, AFT_DELETE = 1 };

//===--------------------------------------------------------------------===//
// Change Ownership
//===--------------------------------------------------------------------===//
struct ChangeOwnershipInfo : public AlterInfo {
	ChangeOwnershipInfo(CatalogType entry_catalog_type, Identifier entry_catalog, Identifier entry_schema,
	                    Identifier entry_name, Identifier owner_schema, Identifier owner_name,
	                    OnEntryNotFound if_not_found);

	// Catalog type refers to the entry type, since this struct is usually built from an
	// ALTER <TYPE> <schema>.<name> OWNED BY <owner_schema>.<owner_name> statement
	// here it is only possible to know the type of who is to be owned
	CatalogType entry_catalog_type;

	Identifier owner_schema;
	Identifier owner_name;

public:
	CatalogType GetCatalogType() const override;
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

	explicit ChangeOwnershipInfo();
};

//===--------------------------------------------------------------------===//
// Set Comment
//===--------------------------------------------------------------------===//
struct SetCommentInfo : public AlterInfo {
	SetCommentInfo(CatalogType entry_catalog_type, QualifiedName qualified_name, Value new_comment_value_p,
	               OnEntryNotFound if_not_found);
	//! Compatibility overload for callers that build the qualification from separate parts (e.g. the legacy
	//! parser extension); the qualification is stored as a three-component path.
	SetCommentInfo(CatalogType entry_catalog_type, Identifier entry_catalog, Identifier entry_schema,
	               Identifier entry_name, Value new_comment_value_p, OnEntryNotFound if_not_found);

	CatalogType entry_catalog_type;
	Value comment_value;

public:
	CatalogType GetCatalogType() const override;
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

	explicit SetCommentInfo();
};

//===--------------------------------------------------------------------===//
// Alter Table
//===--------------------------------------------------------------------===//
enum class AlterTableType : uint8_t {
	INVALID = 0,
	RENAME_COLUMN = 1,
	RENAME_TABLE = 2,
	ADD_COLUMN = 3,
	REMOVE_COLUMN = 4,
	ALTER_COLUMN_TYPE = 5,
	SET_DEFAULT = 6,
	FOREIGN_KEY_CONSTRAINT = 7,
	SET_NOT_NULL = 8,
	DROP_NOT_NULL = 9,
	SET_COLUMN_COMMENT = 10,
	ADD_CONSTRAINT = 11,
	SET_PARTITIONED_BY = 12,
	SET_SORTED_BY = 13,
	ADD_FIELD = 14,
	REMOVE_FIELD = 15,
	RENAME_FIELD = 16,
	SET_TABLE_OPTIONS = 17,
	RESET_TABLE_OPTIONS = 18,
};

struct AlterTableInfo : public AlterInfo {
	AlterTableInfo(AlterTableType type, const AlterEntryData &data);
	~AlterTableInfo() override;

	AlterTableType alter_table_type;

public:
	CatalogType GetCatalogType() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

protected:
	explicit AlterTableInfo(AlterTableType type);
};

//===--------------------------------------------------------------------===//
// RenameColumnInfo
//===--------------------------------------------------------------------===//
struct RenameColumnInfo : public AlterTableInfo {
	RenameColumnInfo(const AlterEntryData &data, Identifier old_name_p, Identifier new_name_p);
	~RenameColumnInfo() override;

	//! Column old name
	Identifier old_name;
	//! Column new name
	Identifier new_name;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	RenameColumnInfo();
};

//===--------------------------------------------------------------------===//
// RenameFieldInfo
//===--------------------------------------------------------------------===//
struct RenameFieldInfo : public AlterTableInfo {
	RenameFieldInfo(const AlterEntryData &data, vector<Identifier> column_path, Identifier new_name_p);
	~RenameFieldInfo() override;

	//! Path to source field.
	vector<Identifier> column_path;
	//! New name of the column (field).
	Identifier new_name;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	Identifier GetColumnName() const override {
		return column_path[0];
	}

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	RenameFieldInfo();
};

//===--------------------------------------------------------------------===//
// RenameTableInfo
//===--------------------------------------------------------------------===//
struct RenameTableInfo : public AlterTableInfo {
	RenameTableInfo(const AlterEntryData &data, Identifier new_name);
	~RenameTableInfo() override;

	//! Relation new name
	Identifier new_table_name;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	RenameTableInfo();
};

//===--------------------------------------------------------------------===//
// AddColumnInfo
//===--------------------------------------------------------------------===//

struct AddColumnConstraints {
	bool add_not_null = false;
	bool add_unique = false;
};

struct AddColumnInfo : public AlterTableInfo {
	AddColumnInfo(const AlterEntryData &data, ColumnDefinition new_column_p, bool if_column_not_exists_p,
	              AddColumnConstraints add_column_constraints_p);
	~AddColumnInfo() override;

	//! New column
	ColumnDefinition new_column;
	//! Whether or not an error should be thrown if the column exist
	bool if_column_not_exists;
	//! Constraints applied via extra ALTER statements after the column is added
	AddColumnConstraints add_column_constraints;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	explicit AddColumnInfo(ColumnDefinition new_column);
};

//===--------------------------------------------------------------------===//
// AddFieldInfo
//===--------------------------------------------------------------------===//
struct AddFieldInfo : public AlterTableInfo {
	AddFieldInfo(const AlterEntryData &data, vector<Identifier> column_path, ColumnDefinition new_field,
	             bool if_field_not_exists);
	~AddFieldInfo() override;

	//! Path to source field.
	vector<Identifier> column_path;
	//! New field to add.
	ColumnDefinition new_field;
	//! Whether or not an error should be thrown if the field does not exist.
	bool if_field_not_exists;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	Identifier GetColumnName() const override {
		return column_path[0];
	}

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	explicit AddFieldInfo(ColumnDefinition new_column);
};

//===--------------------------------------------------------------------===//
// RemoveColumnInfo
//===--------------------------------------------------------------------===//
struct RemoveColumnInfo : public AlterTableInfo {
	RemoveColumnInfo(const AlterEntryData &data, string removed_column, bool if_column_exists, bool cascade);
	~RemoveColumnInfo() override;

	//! The column to remove
	Identifier removed_column;
	//! Whether or not an error should be thrown if the column does not exist
	bool if_column_exists;
	//! Whether or not the column should be removed if a dependency conflict arises (used by GENERATED columns)
	bool cascade;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);
	Identifier GetColumnName() const override {
		return removed_column;
	}

private:
	RemoveColumnInfo();
};

//===--------------------------------------------------------------------===//
// RemoveFieldInfo
//===--------------------------------------------------------------------===//
struct RemoveFieldInfo : public AlterTableInfo {
	RemoveFieldInfo(const AlterEntryData &data, vector<Identifier> column_path, bool if_column_exists, bool cascade);
	~RemoveFieldInfo() override;

	//! Path to source field.
	vector<Identifier> column_path;
	//! Whether or not an error should be thrown if the column does not exist.
	bool if_column_exists;
	//! Whether or not the column should be removed if a dependency conflict arises (used by GENERATED columns).
	bool cascade;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	Identifier GetColumnName() const override {
		return column_path[0];
	}

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	RemoveFieldInfo();
};
//===--------------------------------------------------------------------===//
// ChangeColumnTypeInfo
//===--------------------------------------------------------------------===//
struct ChangeColumnTypeInfo : public AlterTableInfo {
	ChangeColumnTypeInfo(const AlterEntryData &data, vector<Identifier> column_path, LogicalType target_type,
	                     unique_ptr<ParsedExpression> expression);
	~ChangeColumnTypeInfo() override;

	//! Path to the column to alter, e.g. ["s", "a"] for "s.a"
	vector<Identifier> column_path;
	//! The target type of the column
	LogicalType target_type;
	//! The expression used for data conversion
	unique_ptr<ParsedExpression> expression;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);
	Identifier GetColumnName() const override {
		return column_path[0];
	}

private:
	ChangeColumnTypeInfo();
};

//===--------------------------------------------------------------------===//
// SetDefaultInfo
//===--------------------------------------------------------------------===//
struct SetDefaultInfo : public AlterTableInfo {
	SetDefaultInfo(const AlterEntryData &data, vector<Identifier> column_path,
	               unique_ptr<ParsedExpression> new_default);
	~SetDefaultInfo() override;

	//! Path to the column to alter, e.g. ["s", "a"] for "s.a"
	vector<Identifier> column_path;
	//! The expression used for data conversion
	unique_ptr<ParsedExpression> expression;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	SetDefaultInfo();
};

//===--------------------------------------------------------------------===//
// AlterForeignKeyInfo
//===--------------------------------------------------------------------===//
struct AlterForeignKeyInfo : public AlterTableInfo {
	AlterForeignKeyInfo(const AlterEntryData &data, Identifier fk_table, vector<Identifier> pk_columns,
	                    vector<Identifier> fk_columns, vector<PhysicalIndex> pk_keys, vector<PhysicalIndex> fk_keys,
	                    AlterForeignKeyType type);
	~AlterForeignKeyInfo() override;

	Identifier fk_table;
	vector<Identifier> pk_columns;
	vector<Identifier> fk_columns;
	vector<PhysicalIndex> pk_keys;
	vector<PhysicalIndex> fk_keys;
	AlterForeignKeyType type;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	AlterForeignKeyInfo();
};

//===--------------------------------------------------------------------===//
// SetNotNullInfo
//===--------------------------------------------------------------------===//
struct SetNotNullInfo : public AlterTableInfo {
	SetNotNullInfo(const AlterEntryData &data, vector<Identifier> column_path);
	~SetNotNullInfo() override;

	//! Path to the column to alter, e.g. ["s", "a"] for "s.a"
	vector<Identifier> column_path;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	SetNotNullInfo();
};

//===--------------------------------------------------------------------===//
// DropNotNullInfo
//===--------------------------------------------------------------------===//
struct DropNotNullInfo : public AlterTableInfo {
	DropNotNullInfo(const AlterEntryData &data, vector<Identifier> column_path);
	~DropNotNullInfo() override;

	//! Path to the column to alter, e.g. ["s", "a"] for "s.a"
	vector<Identifier> column_path;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	DropNotNullInfo();
};

//===--------------------------------------------------------------------===//
// Alter View
//===--------------------------------------------------------------------===//
enum class AlterViewType : uint8_t { INVALID = 0, RENAME_VIEW = 1 };

struct AlterViewInfo : public AlterInfo {
	AlterViewInfo(AlterViewType type, const AlterEntryData &data);
	~AlterViewInfo() override;

	AlterViewType alter_view_type;

public:
	CatalogType GetCatalogType() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterInfo> Deserialize(Deserializer &deserializer);

protected:
	explicit AlterViewInfo(AlterViewType type);
};

//===--------------------------------------------------------------------===//
// RenameViewInfo
//===--------------------------------------------------------------------===//
struct RenameViewInfo : public AlterViewInfo {
	RenameViewInfo(const AlterEntryData &data, Identifier new_name);
	~RenameViewInfo() override;

	//! Relation new name
	Identifier new_view_name;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterViewInfo> Deserialize(Deserializer &deserializer);

private:
	RenameViewInfo();
};

//===--------------------------------------------------------------------===//
// AddConstraintInfo
//===--------------------------------------------------------------------===//
struct AddConstraintInfo : public AlterTableInfo {
	AddConstraintInfo(const AlterEntryData &data, unique_ptr<Constraint> constraint);
	~AddConstraintInfo() override;

	//! The constraint to add.
	unique_ptr<Constraint> constraint;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	AddConstraintInfo();
};

//===--------------------------------------------------------------------===//
// SetPartitionedByInfo
//===--------------------------------------------------------------------===//
struct SetPartitionedByInfo : public AlterTableInfo {
	SetPartitionedByInfo(const AlterEntryData &data, vector<unique_ptr<ParsedExpression>> partition_keys);
	~SetPartitionedByInfo() override;

	//! The partition keys
	vector<unique_ptr<ParsedExpression>> partition_keys;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	SetPartitionedByInfo();
};

//===--------------------------------------------------------------------===//
// SetSortedByInfo
//===--------------------------------------------------------------------===//
struct SetSortedByInfo : public AlterTableInfo {
	SetSortedByInfo(const AlterEntryData &data, vector<OrderByNode> orders);
	~SetSortedByInfo() override;

	//! The sort keys
	vector<OrderByNode> orders;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	SetSortedByInfo();
};

//===--------------------------------------------------------------------===//
// SetOptionsInfo
//===--------------------------------------------------------------------===//
struct SetTableOptionsInfo : public AlterTableInfo {
	SetTableOptionsInfo(const AlterEntryData &data, case_insensitive_map_t<unique_ptr<ParsedExpression>> table_options);
	~SetTableOptionsInfo() override;

	case_insensitive_map_t<unique_ptr<ParsedExpression>> table_options;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	SetTableOptionsInfo();
};

//===--------------------------------------------------------------------===//
// ResetOptionsInfo
//===--------------------------------------------------------------------===//
struct ResetTableOptionsInfo : public AlterTableInfo {
	ResetTableOptionsInfo(const AlterEntryData &data, identifier_set_t table_options);
	~ResetTableOptionsInfo() override;

	identifier_set_t table_options;

public:
	unique_ptr<AlterInfo> Copy() const override;
	string ToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<AlterTableInfo> Deserialize(Deserializer &deserializer);

private:
	ResetTableOptionsInfo();
};

} // namespace duckdb
