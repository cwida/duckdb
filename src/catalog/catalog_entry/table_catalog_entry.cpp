#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/constraints/bound_check_constraint.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

#include <sstream>

namespace duckdb {

constexpr const char *TableCatalogEntry::Name;

TableCatalogEntry::TableCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info)
    : StandardEntry(CatalogType::TABLE_ENTRY, schema, catalog, info.GetTableName()),
      constraints(std::move(info.constraints)) {
	this->temporary = info.temporary;
	this->dependencies = info.dependencies;
	this->comment = info.comment;
	this->tags = info.tags;
}

bool TableCatalogEntry::HasGeneratedColumns() const {
	auto &columns = GetColumns();
	return columns.LogicalColumnCount() != columns.PhysicalColumnCount();
}

StorageIndex TableCatalogEntry::GetStorageIndex(const ColumnIndex &column_id) const {
	if (column_id.IsRowIdColumn()) {
		return StorageIndex(COLUMN_IDENTIFIER_ROW_ID);
	}
	if (column_id.IsRowNumberColumn()) {
		return StorageIndex(COLUMN_IDENTIFIER_ROW_NUMBER);
	}

	// The index of the base ColumnIndex is equal to the physical column index in the table
	// for any child indices because the indices are already the physical indices.
	// Only the top-level can have generated columns.
	auto &col = GetColumn(column_id.ToLogical());
	auto result = StorageIndex::FromColumnIndex(column_id);
	result.SetIndex(col.StorageOid());
	return result;
}

LogicalIndex TableCatalogEntry::GetColumnIndex(Identifier &column_name, bool if_exists) const {
	auto &columns = GetColumns();
	auto entry = columns.GetColumnIndex(column_name);
	if (!entry.IsValid()) {
		if (if_exists) {
			return entry;
		}
		vector<string> column_names;
		for (auto &col : columns.Logical()) {
			column_names.emplace_back(col.Name());
		}
		auto candidates =
		    StringUtil::CandidatesErrorMessage(column_names, column_name.GetIdentifierName(), "Did you mean");
		throw BinderException("Table %s does not have a column with name %s\n%s", name, column_name, candidates);
	}
	return entry;
}

unique_ptr<BlockingSample> TableCatalogEntry::GetSample() {
	return nullptr;
}

bool TableCatalogEntry::ColumnExists(const Identifier &name) const {
	return GetColumns().ColumnExists(name);
}

const ColumnDefinition &TableCatalogEntry::GetColumn(const Identifier &name) const {
	return GetColumns().GetColumn(name);
}

vector<LogicalType> TableCatalogEntry::GetTypes() const {
	vector<LogicalType> types;
	for (auto &col : GetColumns().Physical()) {
		types.push_back(col.Type());
	}
	return types;
}

unique_ptr<CreateInfo> TableCatalogEntry::GetInfo() const {
	auto result = make_uniq<CreateTableInfo>();
	// carry the full (possibly nested) schema path: [catalog, schema_path..., name]
	result->SetQualifiedName(schema.GetQualifiedName(name));
	result->columns = GetColumns().Copy();
	result->constraints.reserve(constraints.size());
	result->dependencies = dependencies;
	std::for_each(constraints.begin(), constraints.end(),
	              [&result](const unique_ptr<Constraint> &c) { result->constraints.emplace_back(c->Copy()); });
	result->temporary = temporary;
	result->internal = internal;
	result->comment = comment;
	result->tags = tags;
	return std::move(result);
}

string TableCatalogEntry::ColumnsToSQL(const ColumnList &columns, const vector<unique_ptr<Constraint>> &constraints) {
	duckdb::stringstream ss;

	ss << "(";

	// find all columns that have NOT NULL specified, but are NOT primary key columns
	logical_index_set_t not_null_columns;
	logical_index_map_t<vector<ConstraintCheckMode>> unique_columns;
	logical_index_map_t<vector<ConstraintCheckMode>> pk_columns;
	identifier_set_t multi_key_pks;
	vector<string> extra_constraints;
	for (auto &constraint : constraints) {
		if (constraint->type == ConstraintType::NOT_NULL) {
			auto &not_null = constraint->Cast<NotNullConstraint>();
			not_null_columns.insert(not_null.index);
		} else if (constraint->type == ConstraintType::UNIQUE) {
			auto &pk = constraint->Cast<UniqueConstraint>();
			if (pk.HasIndex()) {
				// no columns specified: single column constraint
				if (pk.IsPrimaryKey()) {
					pk_columns[pk.GetIndex()].push_back(pk.check_mode);
				} else {
					unique_columns[pk.GetIndex()].push_back(pk.check_mode);
				}
			} else {
				// multi-column constraint, this constraint needs to go at the end after all columns
				if (pk.IsPrimaryKey()) {
					// multi key pk column: insert set of columns into multi_key_pks
					for (auto &col : pk.GetColumnNames()) {
						multi_key_pks.insert(col);
					}
				}
				extra_constraints.push_back(constraint->ToString());
			}
		} else if (constraint->type == ConstraintType::FOREIGN_KEY) {
			auto &fk = constraint->Cast<ForeignKeyConstraint>();
			if (fk.info.type == ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE ||
			    fk.info.type == ForeignKeyType::FK_TYPE_SELF_REFERENCE_TABLE) {
				extra_constraints.push_back(constraint->ToString());
			}
		} else {
			extra_constraints.push_back(constraint->ToString());
		}
	}

	for (auto &column : columns.Logical()) {
		if (column.Oid() > 0) {
			ss << ", ";
		}
		ss << column.ToSQLString();
		bool not_null = not_null_columns.find(column.Logical()) != not_null_columns.end();
		bool is_single_key_pk = pk_columns.find(column.Logical()) != pk_columns.end();
		bool is_multi_key_pk = multi_key_pks.find(Identifier(column.Name().GetIdentifierName())) != multi_key_pks.end();
		bool is_unique = unique_columns.find(column.Logical()) != unique_columns.end();
		if (not_null && !is_single_key_pk && !is_multi_key_pk) {
			// NOT NULL but not a primary key column
			ss << " NOT NULL";
		}
		if (is_single_key_pk) {
			// single column pk: insert constraint here
			for (auto check_mode : pk_columns.at(column.Logical())) {
				ss << " PRIMARY KEY";
				if (check_mode != ConstraintCheckMode::DEFAULT) {
					ss << " " << EnumUtil::ToString(check_mode);
				}
			}
		}
		if (is_unique) {
			// single column unique: insert constraint here
			for (auto check_mode : unique_columns.at(column.Logical())) {
				ss << " UNIQUE";
				if (check_mode != ConstraintCheckMode::DEFAULT) {
					ss << " " << EnumUtil::ToString(check_mode);
				}
			}
		}
	}
	// print any extra constraints that still need to be printed
	for (auto &extra_constraint : extra_constraints) {
		ss << ", ";
		ss << extra_constraint;
	}

	ss << ")";
	return ss.str();
}

string TableCatalogEntry::ColumnNamesToSQL(const ColumnList &columns) {
	if (columns.empty()) {
		return "";
	}

	duckdb::stringstream ss;
	ss << "(";

	for (auto &column : columns.Logical()) {
		if (column.Oid() > 0) {
			ss << ", ";
		}
		ss << SQLIdentifier(column.Name()) << " ";
	}
	ss << ")";
	return ss.str();
}

string TableCatalogEntry::ToSQL() const {
	auto create_info = GetInfo();
	create_info->StripCatalogQualification();
	return create_info->ToString();
}

TableFunction TableCatalogEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data,
                                                 const EntryLookupInfo &lookup_info) {
	return GetScanFunction(context, bind_data);
}

const ColumnDefinition &TableCatalogEntry::GetColumn(LogicalIndex idx) const {
	return GetColumns().GetColumn(idx);
}

const vector<unique_ptr<Constraint>> &TableCatalogEntry::GetConstraints() const {
	return constraints;
}

// LCOV_EXCL_START
DataTable &TableCatalogEntry::GetStorage() {
	throw InternalException("Calling GetStorage on a TableCatalogEntry that is not a DuckTableEntry");
}
// LCOV_EXCL_STOP

void LogicalUpdate::BindExtraColumns(TableCatalogEntry &table, LogicalGet &get, LogicalProjection &proj,
                                     LogicalUpdate &update, physical_index_set_t &bound_columns,
                                     bool reuse_projected_columns) {
	if (bound_columns.size() <= 1) {
		return;
	}
	idx_t found_column_count = 0;
	physical_index_set_t found_columns;
	for (idx_t i = 0; i < update.columns.size(); i++) {
		if (bound_columns.find(update.columns[i]) != bound_columns.end()) {
			// this column is referenced in the CHECK constraint
			found_column_count++;
			found_columns.insert(update.columns[i]);
		}
	}
	if (found_column_count != bound_columns.size()) {
		// columns that were required are not all part of the UPDATE
		// add them to the scan and update set
		for (auto &physical_id : bound_columns) {
			if (found_columns.find(physical_id) != found_columns.end()) {
				// column is already projected
				continue;
			}
			// column is not projected yet: project it by adding the clause "i=i" to the set of updated columns
			auto &column = table.GetColumns().GetColumn(physical_id);
			auto column_id = column.Logical().index;
			auto get_index = reuse_projected_columns ? get.TryGetProjectionIndex(column_id) : ProjectionIndex();
			if (!get_index.IsValid()) {
				get_index = get.AddColumnId(column_id);
			}
			auto proj_ref =
			    make_uniq<BoundColumnRefExpression>(column.Type(), ColumnBinding(get.table_index, get_index));
			auto proj_index = ColumnBinding::PushExpression(proj.expressions, std::move(proj_ref));
			update.expressions.push_back(
			    make_uniq<BoundColumnRefExpression>(column.Type(), ColumnBinding(proj.table_index, proj_index)));
			update.columns.push_back(physical_id);
		}
	}
}

void LogicalUpdate::BindAllColumns(TableCatalogEntry &table, LogicalGet &get, LogicalProjection &proj,
                                   LogicalUpdate &update, bool reuse_projected_columns) {
	physical_index_set_t all_columns;
	for (auto &column : table.GetColumns().Physical()) {
		all_columns.insert(column.Physical());
	}
	BindExtraColumns(table, get, proj, update, all_columns, reuse_projected_columns);
}

vector<ColumnSegmentInfo> TableCatalogEntry::GetColumnSegmentInfo(const QueryContext &context,
                                                                  const ColumnSegmentInfoScanOptions &options) {
	return {};
}

void TableCatalogEntry::InitializeColumnSegmentInfoScan(ColumnSegmentInfoScanState &state) {
}

bool TableCatalogEntry::ScanColumnSegmentInfo(const QueryContext &context, ColumnSegmentInfoScanState &state,
                                              vector<ColumnSegmentInfo> &result) {
	return false;
}

void TableCatalogEntry::BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj,
                                              LogicalUpdate &update, ClientContext &context) {
	// check the constraints and indexes of the table to see if we need to project any additional columns
	// we do this for indexes with multiple columns and CHECK constraints in the UPDATE clause
	// suppose we have a constraint CHECK(i + j < 10); now we need both i and j to check the constraint
	// if we are only updating one of the two columns we add the other one to the UPDATE set
	// with a "useless" update (i.e. i=i) so we can verify that the CHECK constraint is not violated
	auto bound_constraints = binder.BindConstraints(constraints, name, GetColumns());
	for (auto &constraint : bound_constraints) {
		if (constraint->type == ConstraintType::CHECK) {
			auto &check = constraint->Cast<BoundCheckConstraint>();
			// check constraint! check if we need to add any extra columns to the UPDATE clause
			LogicalUpdate::BindExtraColumns(*this, get, proj, update, check.bound_columns);
		}
	}
	if (update.return_chunk) {
		LogicalUpdate::BindAllColumns(*this, get, proj, update);
	}
	// for index updates we always turn any update into an insert and a delete
	// we thus need all the columns to be available, hence we check if the update touches any index columns
	// If the returning keyword is used, we need access to the whole row in case the user requests it.
	// Therefore switch the update to a delete and insert.
	update.update_is_del_and_insert = Settings::Get<ForceUpdateToDelAndInsertSetting>(context);
	TableStorageInfo table_storage_info = GetStorageInfo(context);
	for (auto index : table_storage_info.index_info) {
		for (auto &column : update.columns) {
			if (index.column_set.find(column.index) != index.column_set.end()) {
				update.update_is_del_and_insert = true;
				break;
			}
		}
	}

	// we also convert any updates on LIST columns into delete + insert
	for (auto &col_index : update.columns) {
		auto &column = GetColumns().GetColumn(col_index);
		if (!column.Type().SupportsRegularUpdate()) {
			update.update_is_del_and_insert = true;
			break;
		}
	}

	if (update.update_is_del_and_insert) {
		// the update updates a column required by an index or requires returning the updated rows,
		// push projections for all columns
		LogicalUpdate::BindAllColumns(*this, get, proj, update);
	}
}

optional_ptr<Constraint> TableCatalogEntry::GetPrimaryKey() const {
	for (const auto &constraint : GetConstraints()) {
		if (constraint->type == ConstraintType::UNIQUE) {
			auto &unique = constraint->Cast<UniqueConstraint>();
			if (unique.IsPrimaryKey()) {
				return &unique;
			}
		}
	}
	return nullptr;
}

bool TableCatalogEntry::HasPrimaryKey() const {
	return GetPrimaryKey() != nullptr;
}

LogicalType TableCatalogEntry::GetExpectedTypeForInsert(const ColumnDefinition &column) const {
	return column.Type();
}

unique_ptr<Expression> TableCatalogEntry::GetDefaultExpressionForColumn(ClientContext &context,
                                                                        const LogicalType &input_type,
                                                                        const LogicalType &result_type,
                                                                        ColumnBinding binding,
                                                                        const Expression &constant_value) const {
	(void)context;
	(void)constant_value;
	return BoundCastExpression::AddCastToType(context, make_uniq<BoundColumnRefExpression>(input_type, binding),
	                                          result_type);
}

virtual_column_map_t TableCatalogEntry::GetVirtualColumns() const {
	virtual_column_map_t virtual_columns;
	virtual_columns.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", LogicalType::ROW_TYPE)));
	return virtual_columns;
}

vector<column_t> TableCatalogEntry::GetRowIdColumns() const {
	vector<column_t> result;
	result.push_back(COLUMN_IDENTIFIER_ROW_ID);
	return result;
}

optional_ptr<CatalogEntry> TableCatalogEntry::CreateTrigger(CatalogTransaction transaction, CreateTriggerInfo &info) {
	throw NotImplementedException("Triggers are not supported for this table type");
}

void TableCatalogEntry::ScanTriggers(CatalogTransaction transaction,
                                     const std::function<void(CatalogEntry &)> &callback) const {
	// Default: no triggers (non-DuckDB tables do not support triggers)
}

optional_ptr<CatalogEntry> TableCatalogEntry::GetTrigger(CatalogTransaction transaction, const Identifier &name) const {
	// Default: no triggers (non-DuckDB tables do not support triggers)
	return nullptr;
}

bool TableCatalogEntry::DropTrigger(CatalogTransaction transaction, const Identifier &name, bool cascade) {
	// Default: no triggers, so there is nothing to drop. Returning false instead of throwing lets DROP TRIGGER IF
	// EXISTS be a no-op.
	return false;
}

vector<const_reference<TriggerCatalogEntry>> TableCatalogEntry::GetTriggersForEvent(CatalogTransaction transaction,
                                                                                    TriggerEventType event_type,
                                                                                    TriggerForEach for_each) const {
	vector<const_reference<TriggerCatalogEntry>> result;
	// CatalogSet is backed by case_insensitive_tree_t (a map with case-insensitive comparator),
	// so ScanTriggers yields entries in alphabetical order by name
	ScanTriggers(transaction, [&](CatalogEntry &entry) {
		auto &trigger = entry.Cast<TriggerCatalogEntry>();
		if (trigger.event_type == event_type && trigger.for_each == for_each) {
			result.emplace_back(trigger);
		}
	});
	return result;
}

vector<const_reference<TriggerCatalogEntry>> TableCatalogEntry::GetTriggersForEvent(CatalogTransaction transaction,
                                                                                    TriggerTiming timing,
                                                                                    TriggerEventType event_type,
                                                                                    TriggerForEach for_each) const {
	vector<const_reference<TriggerCatalogEntry>> result;
	ScanTriggers(transaction, [&](CatalogEntry &entry) {
		auto &trigger = entry.Cast<TriggerCatalogEntry>();
		if (trigger.event_type == event_type && trigger.for_each == for_each && trigger.timing == timing) {
			result.emplace_back(trigger);
		}
	});
	return result;
}

} // namespace duckdb
