#include "duckdb/parser/parsed_data/alter_table_info.hpp"

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/parser/constraint.hpp"
#include "duckdb/parser/keyword_helper.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// ChangeOwnershipInfo
//===--------------------------------------------------------------------===//
ChangeOwnershipInfo::ChangeOwnershipInfo(CatalogType entry_catalog_type, Identifier entry_catalog_p,
                                         Identifier entry_schema_p, Identifier entry_name_p, Identifier owner_schema_p,
                                         Identifier owner_name_p, OnEntryNotFound if_not_found)
    : AlterInfo(AlterType::CHANGE_OWNERSHIP,
                QualifiedName(std::move(entry_catalog_p), std::move(entry_schema_p), std::move(entry_name_p)),
                if_not_found),
      entry_catalog_type(entry_catalog_type), owner_schema(std::move(owner_schema_p)),
      owner_name(std::move(owner_name_p)) {
}

ChangeOwnershipInfo::ChangeOwnershipInfo() : AlterInfo(AlterType::CHANGE_OWNERSHIP) {
}

CatalogType ChangeOwnershipInfo::GetCatalogType() const {
	return entry_catalog_type;
}

unique_ptr<AlterInfo> ChangeOwnershipInfo::Copy() const {
	return make_uniq_base<AlterInfo, ChangeOwnershipInfo>(entry_catalog_type, GetQualifiedName().Catalog(),
	                                                      GetQualifiedName().Schema(), GetQualifiedName().Name(),
	                                                      owner_schema, owner_name, if_not_found);
}

string ChangeOwnershipInfo::ToString() const {
	string result = "";

	result += "ALTER ";
	result += TypeToString(entry_catalog_type);
	result += " ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " OWNED BY ";
	result += QualifiedName(GetQualifiedName().Catalog(), owner_schema, owner_name)
	              .ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// SetCommentInfo
//===--------------------------------------------------------------------===//
SetCommentInfo::SetCommentInfo(CatalogType entry_catalog_type, QualifiedName qualified_name, Value new_comment_value_p,
                               OnEntryNotFound if_not_found)
    : AlterInfo(AlterType::SET_COMMENT, std::move(qualified_name), if_not_found),
      entry_catalog_type(entry_catalog_type), comment_value(std::move(new_comment_value_p)) {
}

SetCommentInfo::SetCommentInfo(CatalogType entry_catalog_type, Identifier entry_catalog, Identifier entry_schema,
                               Identifier entry_name, Value new_comment_value_p, OnEntryNotFound if_not_found)
    : SetCommentInfo(entry_catalog_type,
                     QualifiedName(std::move(entry_catalog), std::move(entry_schema), std::move(entry_name)),
                     std::move(new_comment_value_p), if_not_found) {
}

CatalogType SetCommentInfo::GetCatalogType() const {
	return entry_catalog_type;
}

unique_ptr<AlterInfo> SetCommentInfo::Copy() const {
	return make_uniq_base<AlterInfo, SetCommentInfo>(entry_catalog_type, GetQualifiedName(), comment_value,
	                                                 if_not_found);
}

string SetCommentInfo::ToString() const {
	string result = "";

	result += "COMMENT ON ";
	result += ParseInfo::TypeToString(entry_catalog_type);
	result += " ";
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " IS ";
	result += comment_value.ToSQLString();

	result += ";";
	return result;
}

SetCommentInfo::SetCommentInfo() : AlterInfo(AlterType::SET_COMMENT) {
}

//===--------------------------------------------------------------------===//
// AlterTableInfo
//===--------------------------------------------------------------------===//
AlterTableInfo::AlterTableInfo(AlterTableType type) : AlterInfo(AlterType::ALTER_TABLE), alter_table_type(type) {
}

AlterTableInfo::AlterTableInfo(AlterTableType type, const AlterEntryData &data)
    : AlterInfo(AlterType::ALTER_TABLE, data.GetQualifiedName(), data.if_not_found), alter_table_type(type) {
}
AlterTableInfo::~AlterTableInfo() {
}

CatalogType AlterTableInfo::GetCatalogType() const {
	return CatalogType::TABLE_ENTRY;
}
//===--------------------------------------------------------------------===//
// RenameColumnInfo
//===--------------------------------------------------------------------===//
RenameColumnInfo::RenameColumnInfo(const AlterEntryData &data, Identifier old_name_p, Identifier new_name_p)
    : AlterTableInfo(AlterTableType::RENAME_COLUMN, data), old_name(std::move(old_name_p)),
      new_name(std::move(new_name_p)) {
}

RenameColumnInfo::RenameColumnInfo() : AlterTableInfo(AlterTableType::RENAME_COLUMN) {
}

RenameColumnInfo::~RenameColumnInfo() {
}

unique_ptr<AlterInfo> RenameColumnInfo::Copy() const {
	return make_uniq_base<AlterInfo, RenameColumnInfo>(GetAlterEntryData(), old_name, new_name);
}

string RenameColumnInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " RENAME COLUMN ";
	result += SQLIdentifier(old_name);
	result += " TO ";
	result += SQLIdentifier(new_name);
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// RenameFieldInfo
//===--------------------------------------------------------------------===//
RenameFieldInfo::RenameFieldInfo(const AlterEntryData &data, vector<Identifier> column_path_p, Identifier new_name_p)
    : AlterTableInfo(AlterTableType::RENAME_FIELD, data), column_path(std::move(column_path_p)),
      new_name(std::move(new_name_p)) {
}

RenameFieldInfo::RenameFieldInfo() : AlterTableInfo(AlterTableType::RENAME_FIELD) {
}

RenameFieldInfo::~RenameFieldInfo() {
}

unique_ptr<AlterInfo> RenameFieldInfo::Copy() const {
	return make_uniq_base<AlterInfo, RenameFieldInfo>(GetAlterEntryData(), column_path, new_name);
}

string RenameFieldInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " RENAME COLUMN ";
	for (idx_t i = 0; i < column_path.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += SQLIdentifier(column_path[i]);
	}
	result += " TO ";
	result += SQLIdentifier(new_name);
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// RenameTableInfo
//===--------------------------------------------------------------------===//
RenameTableInfo::RenameTableInfo() : AlterTableInfo(AlterTableType::RENAME_TABLE) {
}

RenameTableInfo::RenameTableInfo(const AlterEntryData &data, Identifier new_name_p)
    : AlterTableInfo(AlterTableType::RENAME_TABLE, data), new_table_name(std::move(new_name_p)) {
}

RenameTableInfo::~RenameTableInfo() {
}

unique_ptr<AlterInfo> RenameTableInfo::Copy() const {
	return make_uniq_base<AlterInfo, RenameTableInfo>(GetAlterEntryData(), new_table_name);
}

string RenameTableInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " RENAME TO ";
	result += SQLIdentifier(new_table_name);
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// AddColumnInfo
//===--------------------------------------------------------------------===//
AddColumnInfo::AddColumnInfo(ColumnDefinition new_column_p)
    : AlterTableInfo(AlterTableType::ADD_COLUMN), new_column(std::move(new_column_p)) {
}

AddColumnInfo::AddColumnInfo(const AlterEntryData &data, ColumnDefinition new_column_p, bool if_column_not_exists_p,
                             AddColumnConstraints add_column_constraints_p)
    : AlterTableInfo(AlterTableType::ADD_COLUMN, data), new_column(std::move(new_column_p)),
      if_column_not_exists(if_column_not_exists_p), add_column_constraints(add_column_constraints_p) {
}

AddColumnInfo::~AddColumnInfo() {
}

unique_ptr<AlterInfo> AddColumnInfo::Copy() const {
	return make_uniq_base<AlterInfo, AddColumnInfo>(GetAlterEntryData(), new_column.Copy(), if_column_not_exists,
	                                                add_column_constraints);
}

string AddColumnInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ADD COLUMN";
	if (if_column_not_exists) {
		result += " IF NOT EXISTS";
	}
	result += " " + SQLIdentifier(this->new_column.GetName());
	result += " " + this->new_column.GetType().ToString();
	if (add_column_constraints.add_not_null) {
		result += " NOT NULL";
	}
	if (add_column_constraints.add_unique) {
		result += " UNIQUE";
	}
	if (this->new_column.HasDefaultValue()) {
		result += " DEFAULT ";
		result += this->new_column.DefaultValue().ToString();
	}
	if (this->new_column.CompressionType() != CompressionType::COMPRESSION_AUTO) {
		result += " USING COMPRESSION " + CompressionTypeToString(this->new_column.CompressionType());
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// AddFieldInfo
//===--------------------------------------------------------------------===//
AddFieldInfo::AddFieldInfo(ColumnDefinition new_field_p)
    : AlterTableInfo(AlterTableType::ADD_FIELD), new_field(std::move(new_field_p)) {
}

AddFieldInfo::AddFieldInfo(const AlterEntryData &data, vector<Identifier> column_path_p, ColumnDefinition new_field_p,
                           bool if_field_not_exists)
    : AlterTableInfo(AlterTableType::ADD_FIELD, data), column_path(std::move(column_path_p)),
      new_field(std::move(new_field_p)), if_field_not_exists(if_field_not_exists) {
}

AddFieldInfo::~AddFieldInfo() {
}

unique_ptr<AlterInfo> AddFieldInfo::Copy() const {
	return make_uniq_base<AlterInfo, AddFieldInfo>(GetAlterEntryData(), column_path, new_field.Copy(),
	                                               if_field_not_exists);
}

string AddFieldInfo::ToString() const {
	string result = "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ADD COLUMN ";
	if (if_field_not_exists) {
		result += "IF NOT EXISTS ";
	}
	for (auto &path : column_path) {
		result += SQLIdentifier(path);
		result += ".";
	}
	result += new_field.ToSQLString();
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// RemoveColumnInfo
//===--------------------------------------------------------------------===//
RemoveColumnInfo::RemoveColumnInfo() : AlterTableInfo(AlterTableType::REMOVE_COLUMN) {
}

RemoveColumnInfo::RemoveColumnInfo(const AlterEntryData &data, string removed_column, bool if_column_exists,
                                   bool cascade)
    : AlterTableInfo(AlterTableType::REMOVE_COLUMN, data), removed_column(std::move(removed_column)),
      if_column_exists(if_column_exists), cascade(cascade) {
}
RemoveColumnInfo::~RemoveColumnInfo() {
}

unique_ptr<AlterInfo> RemoveColumnInfo::Copy() const {
	return make_uniq_base<AlterInfo, RemoveColumnInfo>(GetAlterEntryData(), removed_column.GetIdentifierName(),
	                                                   if_column_exists, cascade);
}

string RemoveColumnInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " DROP COLUMN ";
	if (if_column_exists) {
		result += "IF EXISTS ";
	}
	result += SQLIdentifier(removed_column);
	if (cascade) {
		result += " CASCADE";
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// RemoveFieldInfo
//===--------------------------------------------------------------------===//
RemoveFieldInfo::RemoveFieldInfo() : AlterTableInfo(AlterTableType::REMOVE_FIELD) {
}

RemoveFieldInfo::RemoveFieldInfo(const AlterEntryData &data, vector<Identifier> column_path_p, bool if_column_exists,
                                 bool cascade)
    : AlterTableInfo(AlterTableType::REMOVE_FIELD, data), column_path(std::move(column_path_p)),
      if_column_exists(if_column_exists), cascade(cascade) {
}
RemoveFieldInfo::~RemoveFieldInfo() {
}

unique_ptr<AlterInfo> RemoveFieldInfo::Copy() const {
	return make_uniq_base<AlterInfo, RemoveFieldInfo>(GetAlterEntryData(), column_path, if_column_exists, cascade);
}

string RemoveFieldInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " DROP COLUMN ";
	if (if_column_exists) {
		result += "IF EXISTS ";
	}
	for (idx_t i = 0; i < column_path.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += SQLIdentifier(column_path[i]);
	}
	if (cascade) {
		result += " CASCADE";
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// ChangeColumnTypeInfo
//===--------------------------------------------------------------------===//
ChangeColumnTypeInfo::ChangeColumnTypeInfo() : AlterTableInfo(AlterTableType::ALTER_COLUMN_TYPE) {
}

ChangeColumnTypeInfo::ChangeColumnTypeInfo(const AlterEntryData &data, vector<Identifier> column_path,
                                           LogicalType target_type, unique_ptr<ParsedExpression> expression)
    : AlterTableInfo(AlterTableType::ALTER_COLUMN_TYPE, data), column_path(std::move(column_path)),
      target_type(std::move(target_type)), expression(std::move(expression)) {
}
ChangeColumnTypeInfo::~ChangeColumnTypeInfo() {
}

unique_ptr<AlterInfo> ChangeColumnTypeInfo::Copy() const {
	return make_uniq_base<AlterInfo, ChangeColumnTypeInfo>(GetAlterEntryData(), column_path, target_type,
	                                                       expression ? expression->Copy() : nullptr);
}

string ChangeColumnTypeInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ALTER COLUMN ";
	for (idx_t i = 0; i < column_path.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += SQLIdentifier(column_path[i]);
	}
	result += " TYPE ";
	if (target_type.IsValid()) {
		result += target_type.ToString();
	}
	if (expression) {
		result += " USING ";
		result += expression->ToString();
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// SetDefaultInfo
//===--------------------------------------------------------------------===//
SetDefaultInfo::SetDefaultInfo() : AlterTableInfo(AlterTableType::SET_DEFAULT) {
}

SetDefaultInfo::SetDefaultInfo(const AlterEntryData &data, vector<Identifier> column_path,
                               unique_ptr<ParsedExpression> new_default)
    : AlterTableInfo(AlterTableType::SET_DEFAULT, data), column_path(std::move(column_path)),
      expression(std::move(new_default)) {
}
SetDefaultInfo::~SetDefaultInfo() {
}

unique_ptr<AlterInfo> SetDefaultInfo::Copy() const {
	return make_uniq_base<AlterInfo, SetDefaultInfo>(GetAlterEntryData(), column_path,
	                                                 expression ? expression->Copy() : nullptr);
}

string SetDefaultInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ALTER COLUMN ";
	for (idx_t i = 0; i < column_path.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += SQLIdentifier(column_path[i]);
	}
	if (expression) {
		result += " SET DEFAULT ";
		result += expression->ToString();
	} else {
		result += " DROP DEFAULT";
	}
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// SetNotNullInfo
//===--------------------------------------------------------------------===//
SetNotNullInfo::SetNotNullInfo() : AlterTableInfo(AlterTableType::SET_NOT_NULL) {
}

SetNotNullInfo::SetNotNullInfo(const AlterEntryData &data, vector<Identifier> column_path)
    : AlterTableInfo(AlterTableType::SET_NOT_NULL, data), column_path(std::move(column_path)) {
}
SetNotNullInfo::~SetNotNullInfo() {
}

unique_ptr<AlterInfo> SetNotNullInfo::Copy() const {
	return make_uniq_base<AlterInfo, SetNotNullInfo>(GetAlterEntryData(), column_path);
}

string SetNotNullInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ALTER COLUMN ";
	for (idx_t i = 0; i < column_path.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += SQLIdentifier(column_path[i]);
	}
	result += " SET NOT NULL";
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// DropNotNullInfo
//===--------------------------------------------------------------------===//
DropNotNullInfo::DropNotNullInfo() : AlterTableInfo(AlterTableType::DROP_NOT_NULL) {
}

DropNotNullInfo::DropNotNullInfo(const AlterEntryData &data, vector<Identifier> column_path)
    : AlterTableInfo(AlterTableType::DROP_NOT_NULL, data), column_path(std::move(column_path)) {
}
DropNotNullInfo::~DropNotNullInfo() {
}

unique_ptr<AlterInfo> DropNotNullInfo::Copy() const {
	return make_uniq_base<AlterInfo, DropNotNullInfo>(GetAlterEntryData(), column_path);
}

string DropNotNullInfo::ToString() const {
	string result = "";
	result += "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ALTER COLUMN ";
	for (idx_t i = 0; i < column_path.size(); i++) {
		if (i > 0) {
			result += ".";
		}
		result += SQLIdentifier(column_path[i]);
	}
	result += " DROP NOT NULL";
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// AlterForeignKeyInfo
//===--------------------------------------------------------------------===//
AlterForeignKeyInfo::AlterForeignKeyInfo() : AlterTableInfo(AlterTableType::FOREIGN_KEY_CONSTRAINT) {
}

AlterForeignKeyInfo::AlterForeignKeyInfo(const AlterEntryData &data, Identifier fk_table, vector<Identifier> pk_columns,
                                         vector<Identifier> fk_columns, vector<PhysicalIndex> pk_keys,
                                         vector<PhysicalIndex> fk_keys, AlterForeignKeyType type_p)
    : AlterTableInfo(AlterTableType::FOREIGN_KEY_CONSTRAINT, data), fk_table(std::move(fk_table)),
      pk_columns(std::move(pk_columns)), fk_columns(std::move(fk_columns)), pk_keys(std::move(pk_keys)),
      fk_keys(std::move(fk_keys)), type(type_p) {
}
AlterForeignKeyInfo::~AlterForeignKeyInfo() {
}

unique_ptr<AlterInfo> AlterForeignKeyInfo::Copy() const {
	return make_uniq_base<AlterInfo, AlterForeignKeyInfo>(GetAlterEntryData(), fk_table, pk_columns, fk_columns,
	                                                      pk_keys, fk_keys, type);
}

string AlterForeignKeyInfo::ToString() const {
	throw NotImplementedException("NOT PARSABLE CURRENTLY");
}

//===--------------------------------------------------------------------===//
// Alter View
//===--------------------------------------------------------------------===//
AlterViewInfo::AlterViewInfo(AlterViewType type) : AlterInfo(AlterType::ALTER_VIEW), alter_view_type(type) {
}

AlterViewInfo::AlterViewInfo(AlterViewType type, const AlterEntryData &data)
    : AlterInfo(AlterType::ALTER_VIEW, data.GetQualifiedName(), data.if_not_found), alter_view_type(type) {
}
AlterViewInfo::~AlterViewInfo() {
}

CatalogType AlterViewInfo::GetCatalogType() const {
	return CatalogType::VIEW_ENTRY;
}

//===--------------------------------------------------------------------===//
// RenameViewInfo
//===--------------------------------------------------------------------===//
RenameViewInfo::RenameViewInfo() : AlterViewInfo(AlterViewType::RENAME_VIEW) {
}
RenameViewInfo::RenameViewInfo(const AlterEntryData &data, Identifier new_name_p)
    : AlterViewInfo(AlterViewType::RENAME_VIEW, data), new_view_name(std::move(new_name_p)) {
}
RenameViewInfo::~RenameViewInfo() {
}

unique_ptr<AlterInfo> RenameViewInfo::Copy() const {
	return make_uniq_base<AlterInfo, RenameViewInfo>(GetAlterEntryData(), new_view_name);
}

string RenameViewInfo::ToString() const {
	string result = "";
	result += "ALTER VIEW ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " RENAME TO ";
	result += SQLIdentifier(new_view_name);
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// AddConstraintInfo
//===--------------------------------------------------------------------===//
AddConstraintInfo::AddConstraintInfo() : AlterTableInfo(AlterTableType::ADD_CONSTRAINT) {
}

AddConstraintInfo::AddConstraintInfo(const AlterEntryData &data, unique_ptr<Constraint> constraint_p)
    : AlterTableInfo(AlterTableType::ADD_CONSTRAINT, data), constraint(std::move(constraint_p)) {
}

AddConstraintInfo::~AddConstraintInfo() {
}

unique_ptr<AlterInfo> AddConstraintInfo::Copy() const {
	return make_uniq_base<AlterInfo, AddConstraintInfo>(GetAlterEntryData(), constraint->Copy());
}

string AddConstraintInfo::ToString() const {
	string result = "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " ADD ";
	result += constraint->ToString();
	result += ";";
	return result;
}

//===--------------------------------------------------------------------===//
// SetPartitionedByInfo
//===--------------------------------------------------------------------===//
SetPartitionedByInfo::SetPartitionedByInfo() : AlterTableInfo(AlterTableType::SET_PARTITIONED_BY) {
}

SetPartitionedByInfo::SetPartitionedByInfo(const AlterEntryData &data,
                                           vector<unique_ptr<ParsedExpression>> partition_keys_p)
    : AlterTableInfo(AlterTableType::SET_PARTITIONED_BY, data), partition_keys(std::move(partition_keys_p)) {
}

SetPartitionedByInfo::~SetPartitionedByInfo() {
}

unique_ptr<AlterInfo> SetPartitionedByInfo::Copy() const {
	vector<unique_ptr<ParsedExpression>> copied_partition_keys;
	for (auto &partition_key : partition_keys) {
		copied_partition_keys.push_back(partition_key->Copy());
	}
	return make_uniq_base<AlterInfo, SetPartitionedByInfo>(GetAlterEntryData(), std::move(copied_partition_keys));
}

string SetPartitionedByInfo::ToString() const {
	string result = "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	if (partition_keys.empty()) {
		result += " RESET PARTITIONED BY";
	} else {
		result += " SET PARTITIONED BY (";
		for (idx_t i = 0; i < partition_keys.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += partition_keys[i]->ToString();
		}
		result += ")";
	}
	return result;
}

//===--------------------------------------------------------------------===//
// SetSortedByInfo
//===--------------------------------------------------------------------===//
SetSortedByInfo::SetSortedByInfo() : AlterTableInfo(AlterTableType::SET_SORTED_BY) {
}

SetSortedByInfo::SetSortedByInfo(const AlterEntryData &data, vector<OrderByNode> orders_p)
    : AlterTableInfo(AlterTableType::SET_SORTED_BY, data), orders(std::move(orders_p)) {
}

SetSortedByInfo::~SetSortedByInfo() {
}

unique_ptr<AlterInfo> SetSortedByInfo::Copy() const {
	vector<OrderByNode> copied_orders;
	for (auto &order_key : orders) {
		copied_orders.emplace_back(order_key.type, order_key.null_order, order_key.expression->Copy());
	}
	return make_uniq_base<AlterInfo, SetSortedByInfo>(GetAlterEntryData(), std::move(copied_orders));
}

string SetSortedByInfo::ToString() const {
	string result = "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	if (orders.empty()) {
		result += " RESET SORTED BY";
	} else {
		result += " SET SORTED BY (";
		for (idx_t i = 0; i < orders.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += orders[i].ToString();
		}
		result += ")";
	}
	return result;
}

//===--------------------------------------------------------------------===//
// SetTblPropertiesInfo
//===--------------------------------------------------------------------===//
SetTableOptionsInfo::SetTableOptionsInfo() : AlterTableInfo(AlterTableType::SET_TABLE_OPTIONS) {
}

SetTableOptionsInfo::SetTableOptionsInfo(const AlterEntryData &data,
                                         case_insensitive_map_t<unique_ptr<ParsedExpression>> table_options)
    : AlterTableInfo(AlterTableType::SET_TABLE_OPTIONS, data), table_options(std::move(table_options)) {
}

SetTableOptionsInfo::~SetTableOptionsInfo() {
}

unique_ptr<AlterInfo> SetTableOptionsInfo::Copy() const {
	case_insensitive_map_t<unique_ptr<ParsedExpression>> table_options_copy;
	for (auto &option : table_options) {
		table_options_copy.emplace(option.first, option.second->Copy());
	}
	return make_uniq<SetTableOptionsInfo>(GetAlterEntryData(), std::move(table_options_copy));
}

string SetTableOptionsInfo::ToString() const {
	string result = "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " SET (";
	idx_t i = 0;
	for (auto &entry : table_options) {
		if (i > 0) {
			result += ", ";
		}
		result += SQLString(entry.first) + "=" + entry.second->ToString();
		i++;
	}
	result += ")";
	return result;
}

//===--------------------------------------------------------------------===//
// SetTblPropertiesInfo
//===--------------------------------------------------------------------===//
ResetTableOptionsInfo::ResetTableOptionsInfo() : AlterTableInfo(AlterTableType::RESET_TABLE_OPTIONS) {
}

ResetTableOptionsInfo::ResetTableOptionsInfo(const AlterEntryData &data, identifier_set_t table_options)
    : AlterTableInfo(AlterTableType::RESET_TABLE_OPTIONS, data), table_options(std::move(table_options)) {
}

ResetTableOptionsInfo::~ResetTableOptionsInfo() {
}

unique_ptr<AlterInfo> ResetTableOptionsInfo::Copy() const {
	identifier_set_t table_options_copy;
	for (auto &option : table_options) {
		table_options_copy.emplace(option);
	}
	return make_uniq<ResetTableOptionsInfo>(GetAlterEntryData(), table_options_copy);
}

string ResetTableOptionsInfo::ToString() const {
	string result = "ALTER TABLE ";
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		result += "IF EXISTS ";
	}
	result += GetQualifiedName().ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result += " RESET (";
	idx_t i = 0;
	for (auto &entry : table_options) {
		if (i > 0) {
			result += ", ";
		}
		result += SQLString(entry.GetIdentifierName());
		i++;
	}
	result += ")";
	return result;
}

} // namespace duckdb
