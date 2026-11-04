#include "duckdb/storage/data_table.hpp"
#include "duckdb/planner/constraints/bound_not_null_constraint.hpp"
#include "duckdb/planner/constraints/bound_unique_constraint.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/transaction/commit_state.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/conflict_manager.hpp"
#include "duckdb/common/types/constraint_conflict_info.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/profiler/profiling_utils.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/planner/constraints/list.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_binder/check_binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/checkpoint/table_data_writer.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/delete_state.hpp"
#include "duckdb/storage/table/persistent_table_data.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/update_state.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

DataTableInfo::DataTableInfo(AttachedDatabase &db, shared_ptr<TableIOManager> table_io_manager_p,
                             vector<Identifier> schema_path, Identifier table)
    : db(db), table_io_manager(std::move(table_io_manager_p)), schema_path(std::move(schema_path)),
      table(std::move(table)) {
	D_ASSERT(!this->schema_path.empty());
}

void DataTableInfo::BindIndexes(ClientContext &context, const optional<string> &index_type) {
	indexes.Bind(context, *this, index_type);
}

bool DataTableInfo::IsTemporary() const {
	return db.IsTemporary();
}

IndexStorageInfo DataTableInfo::ExtractIndexStorageInfo(const Identifier &name) {
	for (idx_t i = 0; i < index_storage_infos.size(); i++) {
		if (index_storage_infos[i].name == name) {
			auto result = std::move(index_storage_infos[i]);
			index_storage_infos.erase_at(i);
			return result;
		}
	}
	throw InternalException("ExtractIndexStorageInfo: index storage info with name %s not found", name);
}

DataTable::DataTable(AttachedDatabase &db, shared_ptr<TableIOManager> table_io_manager_p,
                     vector<Identifier> schema_path, Identifier table, vector<ColumnDefinition> column_definitions_p,
                     unique_ptr<PersistentTableData> data)
    : db(db),
      info(make_shared_ptr<DataTableInfo>(db, std::move(table_io_manager_p), std::move(schema_path), std::move(table))),
      column_definitions(std::move(column_definitions_p)), version(DataTableVersion::MAIN_TABLE) {
	// initialize the table with the existing data from disk, if any
	auto types = GetTypes();
	auto &io_manager = TableIOManager::Get(*this);
	this->row_groups = make_shared_ptr<RowGroupCollection>(info, io_manager, types, 0);
	if (data && data->row_group_count > 0) {
		this->row_groups->Initialize(*data);
		row_groups->SetRowGroupAppendMode(RowGroupAppendMode::SUGGEST_NEW);
	} else {
		this->row_groups->InitializeEmpty();
		D_ASSERT(row_groups->GetTotalRows() == 0);
	}
	row_groups->Verify();
}

DataTable::DataTable(ClientContext &context, DataTable &parent, ColumnDefinition &new_column, Expression &default_value)
    : db(parent.db), info(parent.info), version(DataTableVersion::MAIN_TABLE) {
	// add the column definitions from this DataTable
	for (auto &column_def : parent.column_definitions) {
		column_definitions.emplace_back(column_def.Copy());
	}
	column_definitions.emplace_back(new_column.Copy());

	auto &local_storage = LocalStorage::Get(context, db);

	ExpressionExecutor default_executor(context);
	default_executor.AddExpression(default_value);

	// prevent any new tuples from being added to the parent
	lock_guard<mutex> parent_lock(parent.append_lock);

	this->row_groups = parent.row_groups->AddColumn(context, new_column, default_executor);

	// also add this column to client local storage
	local_storage.AddColumn(parent, *this, new_column, default_executor);

	// this table replaces the previous table, hence the parent is no longer the root DataTable
	parent.version = DataTableVersion::ALTERED;
}

DataTable::DataTable(ClientContext &context, DataTable &parent, idx_t removed_column)
    : db(parent.db), info(parent.info), version(DataTableVersion::MAIN_TABLE) {
	// prevent any new tuples from being added to the parent
	auto &local_storage = LocalStorage::Get(context, db);
	lock_guard<mutex> parent_lock(parent.append_lock);

	for (auto &column_def : parent.column_definitions) {
		column_definitions.emplace_back(column_def.Copy());
	}

	// Bind all indexes.
	info->BindIndexes(context);

	// first check if there are any indexes that exist that point to the removed column
	for (const auto column_id : info->indexes.GetIndexedColumns()) {
		if (column_id == removed_column) {
			throw CatalogException("Cannot drop this column: an index depends on it!");
		} else if (column_id > removed_column) {
			throw CatalogException("Cannot drop this column: an index depends on a column after it!");
		}
	}

	// erase the column definitions from this DataTable
	D_ASSERT(removed_column < column_definitions.size());
	column_definitions.erase_at(removed_column);

	storage_t storage_idx = 0;
	for (idx_t i = 0; i < column_definitions.size(); i++) {
		auto &col = column_definitions[i];
		col.SetOid(i);
		if (col.Generated()) {
			continue;
		}
		col.SetStorageOid(storage_idx++);
	}

	// alter the row_groups and remove the column from each of them
	this->row_groups = parent.row_groups->RemoveColumn(removed_column);

	// scan the original table, and fill the new column with the transformed value
	local_storage.DropColumn(parent, *this, removed_column);

	// this table replaces the previous table, hence the parent is no longer the root DataTable
	parent.version = DataTableVersion::ALTERED;
}

DataTable::DataTable(ClientContext &context, DataTable &parent, BoundConstraint &constraint)
    : db(parent.db), info(parent.info), row_groups(parent.row_groups), version(DataTableVersion::MAIN_TABLE) {
	// ALTER COLUMN to add a new constraint.

	// Bind all indexes.
	info->BindIndexes(context);

	auto &local_storage = LocalStorage::Get(context, db);
	lock_guard<mutex> parent_lock(parent.append_lock);
	for (auto &column_def : parent.column_definitions) {
		column_definitions.emplace_back(column_def.Copy());
	}

	if (constraint.type != ConstraintType::UNIQUE) {
		VerifyNewConstraint(local_storage, parent, constraint);
	} else if (constraint.Cast<BoundUniqueConstraint>().is_primary_key) {
		// the key columns become NOT NULL: verify them under the append lock, including rows committed concurrently
		for (auto &key : constraint.Cast<BoundUniqueConstraint>().keys) {
			BoundNotNullConstraint not_null(key);
			VerifyNewConstraint(local_storage, parent, not_null);
		}
	}
	local_storage.MoveStorage(parent, *this);
	parent.version = DataTableVersion::ALTERED;
}

DataTable::DataTable(ClientContext &context, DataTable &parent, idx_t changed_idx, const LogicalType &target_type,
                     const vector<StorageIndex> &bound_columns, Expression &cast_expr,
                     optional_ptr<BoundConstraint> constraint_to_verify)
    : db(parent.db), info(parent.info), version(DataTableVersion::MAIN_TABLE) {
	auto &transaction = DuckTransaction::Get(context, db);
	auto &local_storage = LocalStorage::Get(transaction);
	// prevent any tuples from being added to the parent
	lock_guard<mutex> parent_lock(parent.append_lock);
	for (auto &column_def : parent.column_definitions) {
		column_definitions.emplace_back(column_def.Copy());
	}

	// Bind all indexes.
	info->BindIndexes(context);

	// first check if there are any indexes that exist that point to the changed column
	for (const auto column_id : info->indexes.GetIndexedColumns()) {
		if (column_id == changed_idx) {
			throw CatalogException("Cannot change the type of this column: an index depends on it!");
		}
	}

	// change the type in this DataTable
	column_definitions[changed_idx].SetType(target_type);

	// set up the statistics for the table
	// the column that had its type changed will have the new statistics computed during conversion
	// mark the parent altered before reading, so a concurrent modification conflicts instead of
	// being dropped by the rewrite
	auto previous_version = parent.version.load();
	parent.version = DataTableVersion::ALTERED;
	try {
		// read at the commits seen so far, not at this transaction's older snapshot
		auto &transaction_manager = DuckTransactionManager::Get(db);
		TransactionData rewrite_visibility(transaction.GetTransactionId(),
		                                   VisibilityBound::Through(transaction_manager.GetLastCommit()));
		row_groups = parent.row_groups->AlterType(context, changed_idx, target_type, bound_columns, cast_expr,
		                                          rewrite_visibility);

		// scan the original table, and fill the new column with the transformed value
		local_storage.ChangeType(parent, *this, changed_idx, target_type, bound_columns, cast_expr);

		// re-verify any column constraint that the rewrite could have violated (e.g. NOT NULL)
		if (constraint_to_verify) {
			VerifyNewConstraint(local_storage, *this, *constraint_to_verify);
		}
	} catch (...) {
		// nothing reached the catalog, so no undo entry will restore the parent
		parent.version = previous_version;
		throw;
	}
}

vector<LogicalType> DataTable::GetTypes() {
	vector<LogicalType> types;
	for (auto &it : column_definitions) {
		types.push_back(it.Type());
	}
	return types;
}

bool DataTable::IsTemporary() const {
	return info->IsTemporary();
}

AttachedDatabase &DataTable::GetAttached() {
	D_ASSERT(RefersToSameObject(db, info->db));
	return db;
}

const vector<ColumnDefinition> &DataTable::Columns() const {
	return column_definitions;
}

TableIOManager &DataTable::GetTableIOManager() {
	return *info->table_io_manager;
}

TableIOManager &TableIOManager::Get(DataTable &table) {
	return table.GetTableIOManager();
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
void DataTable::InitializeScan(ClientContext &context, DuckTransaction &transaction, TableScanState &state,
                               const vector<StorageIndex> &column_ids, optional_ptr<TableFilterSet> table_filters) {
	auto &local_storage = LocalStorage::Get(transaction);
	state.Initialize(column_ids, context, table_filters);
	row_groups->InitializeScan(context, state.table_state, column_ids, table_filters);
	local_storage.InitializeScan(*this, state.local_state, table_filters);
}

void DataTable::InitializeScanWithOffset(DuckTransaction &transaction, TableScanState &state,
                                         const vector<StorageIndex> &column_ids, idx_t start_row, idx_t end_row) {
	state.Initialize(column_ids);
	row_groups->InitializeScanWithOffset(QueryContext(), state.table_state, column_ids, start_row, end_row);
}

idx_t DataTable::GetRowGroupSize() const {
	return row_groups->GetRowGroupSize();
}

vector<PartitionStatistics> DataTable::GetPartitionStats(ClientContext &context) {
	auto &transaction = DuckTransaction::Get(context, db);
	auto result = row_groups->GetPartitionStats(transaction);
	auto &local_storage = LocalStorage::Get(context, db);
	auto local_partitions = local_storage.GetPartitionStats(*this, transaction);
	result.insert(result.end(), local_partitions.begin(), local_partitions.end());
	return result;
}

idx_t DataTable::MaxThreads(ClientContext &context) const {
	idx_t row_group_size = GetRowGroupSize();
	idx_t parallel_scan_vector_count = row_group_size / STANDARD_VECTOR_SIZE;
	if (ClientConfig::GetConfig(context).verify_parallelism) {
		parallel_scan_vector_count = 1;
	}
	idx_t parallel_scan_tuple_count = STANDARD_VECTOR_SIZE * parallel_scan_vector_count;
	idx_t total_rows = GetTotalRows();
	auto local_storage = LocalStorage::Get(context, db).GetStorage(*this);
	if (local_storage) {
		// transaction-local rows are scanned in parallel as well
		total_rows += local_storage->GetCollection().GetTotalRows();
	}
	return total_rows / parallel_scan_tuple_count + 1;
}

void DataTable::InitializeParallelScan(ClientContext &context, ParallelTableScanState &state,
                                       const vector<ColumnIndex> &column_indexes) {
	auto &local_storage = LocalStorage::Get(context, db);
	row_groups->InitializeParallelScan(state.scan_state);

	local_storage.InitializeParallelScan(*this, state.local_state);
}

optional_idx DataTable::NextParallelScan(ClientContext &context, ParallelTableScanState &state,
                                         TableScanState &scan_state, bool initialize_columns) {
	const auto rows =
	    row_groups->NextParallelScan(context, state.scan_state, scan_state.table_state, initialize_columns);
	if (rows.IsValid()) {
		return rows;
	}
	if (state.scan_state.row_number_base.IsValid()) {
		// start the row number for transaction-local rows from the final row count in the base table
		scan_state.local_state.row_number_base = state.scan_state.row_number_base.GetIndex();
	}
	auto &local_storage = LocalStorage::Get(context, db);
	return local_storage.NextParallelScan(context, *this, state.local_state, scan_state.local_state,
	                                      initialize_columns);
}

void DataTable::Scan(DuckTransaction &transaction, DataChunk &result, TableScanState &state) {
	// scan the persistent segments
	if (state.table_state.Scan(transaction, result)) {
		D_ASSERT(result.size() > 0);
		return;
	}

	// scan the transaction-local segments
	auto &local_storage = LocalStorage::Get(transaction);
	local_storage.Scan(state.local_state, state.GetColumnIds(), result);
}

bool DataTable::CreateIndexScan(TableScanState &state, DataChunk &result) {
	return state.table_state.Scan(result, TableScanType::TABLE_SCAN_OMIT_PERMANENTLY_DELETED);
}

//===--------------------------------------------------------------------===//
// Index Methods
//===--------------------------------------------------------------------===//
shared_ptr<DataTableInfo> &DataTable::GetDataTableInfo() {
	return info;
}

void DataTable::BindIndexes(ClientContext &context) {
	info->BindIndexes(context);
}

bool DataTable::HasIndexes() const {
	return !info->indexes.Empty();
}

bool DataTable::HasUniqueIndexes() const {
	return info->indexes.HasUniqueIndexes();
}

void DataTable::AddIndex(unique_ptr<Index> index, idx_t index_oid) {
	info->indexes.AddIndex(std::move(index), index_oid, ConstraintCheckMode::DEFAULT);
}

bool DataTable::HasForeignKeyIndex(const vector<PhysicalIndex> &keys, ForeignKeyType type) {
	auto index = info->indexes.FindForeignKeyIndex(keys, type);
	return index != nullptr;
}

void DataTable::SetIndexStorageInfo(vector<IndexStorageInfo> index_storage_info) {
	info->index_storage_infos = std::move(index_storage_info);
}

void DataTable::VacuumIndexes() {
	info->indexes.Vacuum();
}

void DataTable::RebuildIndexes() {
	auto &types = row_groups->GetTypes();
	info->indexes.Rebuild([&](const vector<column_t> &col_ids, const IndexRebuildAppend &append) {
		vector<StorageIndex> scan_column_ids;
		vector<LogicalType> scan_types;
		for (auto col_id : col_ids) {
			scan_column_ids.emplace_back(col_id);
			scan_types.push_back(types[col_id]);
		}
		scan_column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
		scan_types.push_back(LogicalType::ROW_TYPE);

		DataChunk scan_chunk;
		scan_chunk.Initialize(Allocator::Get(db), scan_types);

		CreateIndexScanState state;
		auto scan_type = TableScanType::TABLE_SCAN_COMMITTED_ROWS;
		state.Initialize(scan_column_ids, nullptr);
		QueryContext context;
		row_groups->InitializeScan(context, state.table_state, scan_column_ids, nullptr);
		row_groups->InitializeCreateIndexScan(state);

		DataChunk table_chunk;
		table_chunk.InitializeEmpty(types);

		while (true) {
			scan_chunk.Reset();
			state.table_state.Scan(scan_chunk, scan_type, state.segment_lock);
			if (scan_chunk.size() == 0) {
				break;
			}
			for (idx_t i = 0; i < col_ids.size(); i++) {
				table_chunk.data[col_ids[i]].Reference(scan_chunk.data[i]);
			}
			Vector &row_ids = scan_chunk.data[col_ids.size()];
			append(table_chunk, row_ids);
		}
	});
}

void DataTable::VerifyIndexBuffers() const {
	info->VerifyIndexBuffers();
}

void DataTableInfo::VerifyIndexBuffers() const {
	indexes.VerifyBuffers();
}

void DataTable::CleanupAppend(VisibilityBound lowest_visibility_bound, idx_t start, idx_t count) {
	row_groups->CleanupAppend(lowest_visibility_bound, start, count);
}

Identifier DataTableInfo::GetSchemaName() {
	return schema_path.back();
}

const vector<Identifier> &DataTableInfo::GetSchemaPath() const {
	return schema_path;
}

Identifier DataTableInfo::GetTableName() {
	lock_guard<mutex> l(name_lock);
	return table;
}

void DataTableInfo::SetTableName(Identifier name) {
	lock_guard<mutex> l(name_lock);
	table = std::move(name);
}

Identifier DataTable::GetTableName() const {
	return info->GetTableName();
}

void DataTable::SetTableName(Identifier new_name) {
	info->SetTableName(std::move(new_name));
}

TableStorageInfo DataTable::GetStorageInfo() const {
	TableStorageInfo result;
	result.cardinality = GetTotalRows();
	result.index_info = info->indexes.GetStorageInfo();
	return result;
}

//===--------------------------------------------------------------------===//
// Fetch
//===--------------------------------------------------------------------===//
void DataTable::Fetch(DuckTransaction &transaction, DataChunk &result, const vector<StorageIndex> &column_ids,
                      const Vector &row_identifiers, idx_t fetch_count, ColumnFetchState &state) {
	D_ASSERT(row_identifiers.GetVectorType() == VectorType::FLAT_VECTOR);
	auto row_ids = FlatVector::GetData<row_t>(row_identifiers);

	// Quick scan to check for transaction-local row IDs (>= MAX_ROW_ID).
	bool has_local = false;
	for (idx_t i = 0; i < fetch_count; i++) {
		if (row_ids[i] >= MAX_ROW_ID) {
			has_local = true;
			break;
		}
	}

	if (!has_local) {
		// All committed rows — fast path (common case).
		row_groups->Fetch(transaction, result, column_ids, row_identifiers, fetch_count, state);
		return;
	}

	// There are local rows. Classify all row IDs to determine the split.
	auto &local_storage = transaction.GetLocalStorage();
	SelectionVector committed_sel(fetch_count);
	SelectionVector local_sel(fetch_count);
	idx_t committed_count = 0;
	idx_t local_count = 0;
	for (idx_t i = 0; i < fetch_count; i++) {
		if (row_ids[i] >= MAX_ROW_ID) {
			local_sel.set_index(local_count++, i);
		} else {
			committed_sel.set_index(committed_count++, i);
		}
	}

	if (committed_count == 0) {
		// All local rows.
		local_storage.FetchChunk(*this, row_identifiers, fetch_count, column_ids, result, state);
		return;
	}

	// Mixed: some rows are committed, some are local.
	// row_groups->Fetch silently skips local row IDs, packing committed rows at 0..committed_count-1.
	row_groups->Fetch(transaction, result, column_ids, row_identifiers, fetch_count, state);
	D_ASSERT(result.size() == committed_count);

	// Fetch local rows into a separate chunk.
	auto &allocator = Allocator::Get(local_storage.GetClientContext());
	DataChunk local_chunk;
	local_chunk.Initialize(allocator, result.GetTypes());
	Vector local_row_ids(row_identifiers, local_sel, local_count);
	local_row_ids.Flatten();
	ColumnFetchState local_fetch_state;
	local_storage.FetchChunk(*this, local_row_ids, local_count, column_ids, local_chunk, local_fetch_state);

	// Append local rows after committed rows in the result.
	for (idx_t col = 0; col < result.ColumnCount(); col++) {
		VectorOperations::Copy(local_chunk.data[col], result.data[col], local_count, 0, committed_count);
	}
	result.SetChildCardinality(committed_count + local_count);

	// Build inverse permutation to restore original row order.
	// Current layout: [committed rows in relative order | local rows in relative order].
	SelectionVector inv_perm(fetch_count);
	for (idx_t i = 0; i < committed_count; i++) {
		inv_perm.set_index(committed_sel.get_index(i), i);
	}
	for (idx_t j = 0; j < local_count; j++) {
		inv_perm.set_index(local_sel.get_index(j), committed_count + j);
	}
	for (idx_t col = 0; col < result.ColumnCount(); col++) {
		result.data[col].Slice(inv_perm, fetch_count);
	}
}

void DataTable::FetchCommitted(DataChunk &result, const vector<StorageIndex> &column_ids, const Vector &row_identifiers,
                               idx_t fetch_count, ColumnFetchState &state) {
	TransactionData commit_transaction(MAX_TRANSACTION_ID, VisibilityBound::Before(MAX_COMMIT_ID));
	row_groups->Fetch(commit_transaction, result, column_ids, row_identifiers, fetch_count, state);
}

bool DataTable::CanFetch(DuckTransaction &transaction, const row_t row_id) {
	return row_groups->CanFetch(transaction, row_id);
}

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//
static void VerifyNotNullConstraint(TableCatalogEntry &table, const Vector &vector, const Identifier &col_name) {
	if (!VectorOperations::HasNull(vector)) {
		return;
	}

	throw ConstraintException("NOT NULL constraint failed: %s.%s", SQLIdentifier(table.name), SQLIdentifier(col_name));
}

// To avoid throwing an error at SELECT, instead this moves the error detection to INSERT
static void VerifyGeneratedExpressionSuccess(ClientContext &context, TableCatalogEntry &table, DataChunk &chunk,
                                             Expression &expr, column_t index) {
	auto &col = table.GetColumn(LogicalIndex(index));
	D_ASSERT(col.Generated());
	ExpressionExecutor executor(context, expr);
	Vector result(col.Type());
	try {
		executor.ExecuteExpression(chunk, result);
	} catch (InternalException &ex) {
		throw;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		throw ConstraintException("Incorrect value for generated column '%s %s AS (%s)' : %s",
		                          SQLIdentifier(col.Name()), col.Type().ToString(),
		                          col.GeneratedExpression().ToString(), error.RawMessage());
	}
}

static void VerifyCheckConstraint(ClientContext &context, TableCatalogEntry &table, Expression &expr, DataChunk &chunk,
                                  CheckConstraint &check) {
	ExpressionExecutor executor(context, expr);
	Vector result(LogicalType::INTEGER);
	try {
		executor.ExecuteExpression(chunk, result);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		throw ConstraintException("CHECK constraint failed on table %s with expression %s (Error: %s)", table.name,
		                          check.ToString(), error.RawMessage());
	} catch (...) {
		// LCOV_EXCL_START
		throw ConstraintException("CHECK constraint failed on table %s with expression %s (Unknown Error)", table.name,
		                          check.ToString());
	} // LCOV_EXCL_STOP
	for (auto entry : result.Values<int32_t>()) {
		if (entry.IsValid() && entry.GetValue() == 0) {
			throw ConstraintException("CHECK constraint failed on table %s with expression %s", table.name,
			                          check.ToString());
		}
	}
}

static idx_t FirstMissingMatch(ConflictManager &manager, const idx_t count) {
	// Find the first non-NULL index without a match.
	if (!manager.HasConflicts()) {
		return 0;
	}
	auto conflict = manager.GetFirstInvalidIndex(count, true);
	if (conflict.IsValid()) {
		return conflict.GetIndex();
	}
	throw InternalException("expected a missing match for the index");
}

idx_t LocateErrorIndex(ConflictManager &manager, const bool is_append, const idx_t count) {
	// We expected to find nothing, so the first error is the first match.
	if (!is_append) {
		return manager.GetFirstIndex();
	}
	// We expected to find matches for each row.
	// Thus, the first missing match is the first error.
	return FirstMissingMatch(manager, count);
}

static string ConstructForeignKeyError(optional_idx conflict, bool is_append, const shared_ptr<IndexEntry> &entry,
                                       DataChunk &input) {
	const auto verify_type = is_append ? VerifyExistenceType::APPEND_FK : VerifyExistenceType::DELETE_FK;
	return entry->GetConstraintViolationMessage(verify_type, conflict.GetIndex(), input);
}

bool IsForeignKeyConstraintError(const ConflictManager &manager, const bool is_append, const idx_t input_count) {
	if (is_append) {
		// We need to find a match for all values.
		return manager.ConflictCount() != input_count;
	}
	// Nothing should match.
	return manager.ConflictCount() != 0;
}

static bool IsAppend(VerifyExistenceType verify_type) {
	return verify_type == VerifyExistenceType::APPEND_FK;
}

void DataTable::VerifyForeignKeyConstraint(optional_ptr<LocalTableStorage> storage,
                                           const BoundForeignKeyConstraint &bound_foreign_key, ClientContext &context,
                                           DataChunk &chunk, VerifyExistenceType verify_type) {
	reference<const vector<PhysicalIndex>> src_keys_ptr = bound_foreign_key.info.fk_keys;
	reference<const vector<PhysicalIndex>> dst_keys_ptr = bound_foreign_key.info.pk_keys;

	auto is_append = IsAppend(verify_type);
	if (!is_append) {
		src_keys_ptr = bound_foreign_key.info.pk_keys;
		dst_keys_ptr = bound_foreign_key.info.fk_keys;
	}

	// Get the column types in their physical order. A foreign key always references a table in the same (possibly
	// nested) schema, so we qualify it with this table's schema path.
	auto &table_entry = Catalog::GetEntry<TableCatalogEntry>(
	    context, QualifiedName::FromCatalogSchema(db.GetName(), info->GetSchemaPath(), bound_foreign_key.info.table));
	vector<LogicalType> types;
	for (auto &col : table_entry.GetColumns().Physical()) {
		types.emplace_back(col.Type());
	}

	// Create the data chunk that has to be verified.
	DataChunk dst_chunk;
	dst_chunk.InitializeEmpty(types);
	for (idx_t i = 0; i < src_keys_ptr.get().size(); i++) {
		const auto &src_chunk = chunk.data[src_keys_ptr.get()[i].index];
		dst_chunk.data[dst_keys_ptr.get()[i].index].Reference(src_chunk);
	}

	auto count = chunk.size();
	if (count <= 0) {
		return;
	}

	// Record conflicts instead of throwing immediately.
	unordered_set<column_t> empty_column_list;
	ConflictInfo empty_conflict_info(empty_column_list, false);

	ConflictManager global_conflict_manager(verify_type, count, &empty_conflict_info);
	global_conflict_manager.SetMode(ConflictManagerMode::SCAN);
	ConflictManager local_conflict_manager(verify_type, count, &empty_conflict_info);
	local_conflict_manager.SetMode(ConflictManagerMode::SCAN);

	// Global constraint verification.
	auto &data_table = table_entry.GetStorage();
	auto &local_storage = LocalStorage::Get(context, db);
	auto sibling_storage = local_storage.GetStorage(data_table);
	auto sibling_delete_indexes = sibling_storage ? &sibling_storage->delete_indexes : nullptr;

	data_table.info->indexes.VerifyForeignKey(sibling_delete_indexes, dst_keys_ptr, dst_chunk, global_conflict_manager);

	// Check if we can insert the chunk into the local storage.
	bool local_error = false;
	auto local_verification = sibling_storage != nullptr;

	// Local constraint verification.
	if (local_verification) {
		auto &local_indexes = local_storage.GetIndexes(context, data_table);
		local_indexes.VerifyForeignKey(sibling_delete_indexes, dst_keys_ptr, dst_chunk, local_conflict_manager);
		local_error = IsForeignKeyConstraintError(local_conflict_manager, is_append, count);
	}
	// Global constraint verification.
	auto global_error = IsForeignKeyConstraintError(global_conflict_manager, is_append, count);

	// No constraint violation.
	if (!global_error && !local_error) {
		return;
	}

	// Either a global or local error occurred.
	// We construct the error message and throw.
	shared_ptr<IndexEntry> index_entry;
	shared_ptr<IndexEntry> transaction_index_entry;
	auto fk_type = is_append ? ForeignKeyType::FK_TYPE_PRIMARY_KEY_TABLE : ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE;

	// Check whether we can insert into the foreign key table, or delete from the reference table.
	index_entry = data_table.info->indexes.FindForeignKeyIndex(dst_keys_ptr, fk_type);
	if (!local_verification) {
		auto conflict = LocateErrorIndex(global_conflict_manager, is_append, count);
		auto message = ConstructForeignKeyError(conflict, is_append, index_entry, dst_chunk);
		throw ConstraintException(message);
	}

	auto &transact_index = local_storage.GetIndexes(context, data_table);
	transaction_index_entry = transact_index.FindForeignKeyIndex(dst_keys_ptr, fk_type);

	if (local_error && global_error && is_append) {
		// For appends, we throw if the foreign key neither exists in the transaction nor the local storage.
		optional_idx conflict;
		auto global_conflicts = global_conflict_manager.HasConflicts();
		auto local_conflicts = local_conflict_manager.HasConflicts();
		if (!global_conflicts && !local_conflicts) {
			conflict = 0;
		} else if (!global_conflicts && local_conflicts) {
			conflict = local_conflict_manager.GetFirstInvalidIndex(count, /*negate=*/true);
		} else if (global_conflicts && !local_conflicts) {
			conflict = global_conflict_manager.GetFirstInvalidIndex(count, /*negate=*/true);
		} else {
			auto &global_validity = global_conflict_manager.GetFirstValidity();
			auto &local_validity = local_conflict_manager.GetFirstValidity();
			for (idx_t i = 0; i < count; i++) {
				auto global_match = global_validity.RowIsValid(i);
				auto local_match = local_validity.RowIsValid(i);
				if (!global_match && !local_match) {
					conflict = i;
					break;
				}
			}
		}

		if (!conflict.IsValid()) {
			// We don't throw, every value was present in either regular or transaction storage
			return;
		}
		auto message = ConstructForeignKeyError(conflict, true, index_entry, dst_chunk);
		throw ConstraintException(message);
	}

	if (!is_append) {
		if (global_error) {
			auto conflict = LocateErrorIndex(global_conflict_manager, false, count);
			auto message = ConstructForeignKeyError(conflict, false, index_entry, dst_chunk);
			throw ConstraintException(message);
		}

		D_ASSERT(local_conflict_manager.HasConflicts());
		auto conflict = LocateErrorIndex(local_conflict_manager, false, count);
		auto message = ConstructForeignKeyError(conflict, false, transaction_index_entry, dst_chunk);
		throw ConstraintException(message);
	}
}

void DataTable::VerifyAppendForeignKeyConstraint(optional_ptr<LocalTableStorage> storage,
                                                 const BoundForeignKeyConstraint &bound_foreign_key,
                                                 ClientContext &context, DataChunk &chunk) {
	VerifyForeignKeyConstraint(storage, bound_foreign_key, context, chunk, VerifyExistenceType::APPEND_FK);
}

void DataTable::VerifyAppendedForeignKeys(ClientContext &context, idx_t start, idx_t end,
                                          const vector<unique_ptr<BoundConstraint>> &bound_constraints) {
	if (start >= end) {
		return;
	}
	vector<reference<BoundForeignKeyConstraint>> append_constraints;
	for (auto &constraint : bound_constraints) {
		if (constraint->type != ConstraintType::FOREIGN_KEY) {
			continue;
		}
		auto &bound_foreign_key = constraint->Cast<BoundForeignKeyConstraint>();
		if (bound_foreign_key.info.IsAppendConstraint()) {
			append_constraints.push_back(bound_foreign_key);
		}
	}
	if (append_constraints.empty()) {
		return;
	}
	auto &transaction = DuckTransaction::Get(context, db);
	auto &local_storage = LocalStorage::Get(context, db);
	auto local_table_storage = local_storage.GetStorage(*this);
	if (!local_table_storage) {
		return;
	}
	auto &collection = local_table_storage->GetCollection();
	auto types = GetTypes();
	for (auto &bound_foreign_key_ref : append_constraints) {
		auto &bound_foreign_key = bound_foreign_key_ref.get();
		auto &fk_keys = bound_foreign_key.info.fk_keys;
		vector<StorageIndex> column_ids;
		vector<LogicalType> fk_types;
		for (auto &fk_key : fk_keys) {
			column_ids.emplace_back(fk_key.index);
			fk_types.push_back(types[fk_key.index]);
		}
		// Read the key columns with a vectorized range scan instead of a per-row fetch; the scan starts at the
		// enclosing vector boundary, so a few rows appended by earlier statements may be verified again
		TableScanState scan_state;
		scan_state.Initialize(column_ids);
		auto local_start = NumericCast<idx_t>(MAX_ROW_ID) + start;
		auto local_end = NumericCast<idx_t>(MAX_ROW_ID) + end;
		collection.InitializeScanWithOffset(QueryContext(context), scan_state.table_state, column_ids, local_start,
		                                    local_end);
		DataChunk fk_chunk;
		fk_chunk.Initialize(context, fk_types);
		while (true) {
			context.InterruptCheck();
			fk_chunk.Reset();
			if (!scan_state.table_state.Scan(transaction, fk_chunk)) {
				break;
			}
			// Build a shell chunk holding the key columns at their physical positions
			DataChunk shell_chunk;
			shell_chunk.InitializeEmpty(types);
			for (idx_t i = 0; i < fk_keys.size(); i++) {
				shell_chunk.data[fk_keys[i].index].Reference(fk_chunk.data[i]);
			}
			shell_chunk.SetChildCardinality(fk_chunk.size());
			VerifyAppendForeignKeyConstraint(nullptr, bound_foreign_key, context, shell_chunk);
		}
	}
}

void DataTable::VerifyDeleteForeignKeyConstraint(optional_ptr<LocalTableStorage> storage,
                                                 const BoundForeignKeyConstraint &bound_foreign_key,
                                                 ClientContext &context, DataChunk &chunk) {
	VerifyForeignKeyConstraint(storage, bound_foreign_key, context, chunk, VerifyExistenceType::DELETE_FK);
}

void DataTable::VerifyNewConstraint(LocalStorage &local_storage, DataTable &parent, const BoundConstraint &constraint) {
	if (constraint.type != ConstraintType::NOT_NULL) {
		throw NotImplementedException("FIXME: ALTER COLUMN with such constraint is not supported yet");
	}

	parent.row_groups->VerifyNewConstraint(local_storage.GetClientContext(), local_storage.GetTransaction(), parent,
	                                       constraint);
	local_storage.VerifyNewConstraint(parent, constraint);
}

void DataTable::VerifyAppendConstraints(ConstraintState &constraint_state, ClientContext &context, DataChunk &chunk,
                                        optional_ptr<LocalTableStorage> storage,
                                        optional_ptr<ConflictManager> manager) {
	auto &table = constraint_state.table;
	if (table.HasGeneratedColumns()) {
		// Verify the generated columns against the inserted values.
		auto binder = Binder::CreateBinder(context);
		physical_index_set_t bound_columns;
		CheckBinder generated_check_binder(*binder, context, table.name, table.GetColumns(), bound_columns);
		for (auto &col : table.GetColumns().Logical()) {
			if (!col.Generated()) {
				continue;
			}
			D_ASSERT(col.Type().id() != LogicalTypeId::ANY);
			generated_check_binder.target_type = col.Type();
			auto to_be_bound_expression = col.GeneratedExpression().Copy();
			auto bound_expression = generated_check_binder.Bind(to_be_bound_expression);
			VerifyGeneratedExpressionSuccess(context, table, chunk, *bound_expression, col.Oid());
		}
	}

	if (HasUniqueIndexes()) {
		info->indexes.VerifyUniqueIndexes(storage ? &storage->delete_indexes : nullptr, chunk, manager);
	}

	auto &constraints = table.GetConstraints();
	for (idx_t i = 0; i < constraint_state.bound_constraints.size(); i++) {
		auto &base_constraint = constraints[i];
		auto &constraint = constraint_state.bound_constraints[i];
		switch (base_constraint->type) {
		case ConstraintType::NOT_NULL: {
			auto &bound_not_null = constraint->Cast<BoundNotNullConstraint>();
			auto &not_null = base_constraint->Cast<NotNullConstraint>();
			auto &col = table.GetColumns().GetColumn(LogicalIndex(not_null.index));
			VerifyNotNullConstraint(table, chunk.data[bound_not_null.index.index], col.Name());
			break;
		}
		case ConstraintType::CHECK: {
			auto &check = base_constraint->Cast<CheckConstraint>();
			auto &bound_check = constraint->Cast<BoundCheckConstraint>();
			VerifyCheckConstraint(context, table, *bound_check.expression, chunk, check);
			break;
		}
		case ConstraintType::UNIQUE: {
			// These were handled earlier.
			break;
		}
		case ConstraintType::FOREIGN_KEY: {
			// Foreign keys are verified after the statement has finished (ClientContext::VerifyDeferredForeignKeys)
			// so that all rows written by the statement are visible to the check
			if (!context.transaction.HasActiveTransaction() ||
			    context.transaction.GetActiveQuery() == MAXIMUM_QUERY_ID) {
				// appends outside of a statement (e.g. the internal appender) are not verified later
				auto &bound_foreign_key = constraint->Cast<BoundForeignKeyConstraint>();
				if (bound_foreign_key.info.IsAppendConstraint()) {
					VerifyAppendForeignKeyConstraint(storage, bound_foreign_key, context, chunk);
				}
			}
			break;
		}
		default:
			throw InternalException("invalid constraint type");
		}
	}
}

unique_ptr<ConstraintState>
DataTable::InitializeConstraintState(TableCatalogEntry &table,
                                     const vector<unique_ptr<BoundConstraint>> &bound_constraints) {
	return make_uniq<ConstraintState>(table, bound_constraints);
}

string DataTable::TableModification() const {
	switch (version.load()) {
	case DataTableVersion::MAIN_TABLE:
		return "no changes";
	case DataTableVersion::ALTERED:
		return "altered";
	case DataTableVersion::DROPPED:
		return "dropped";
	default:
		throw InternalException("Unrecognized table version");
	}
}

void DataTable::InitializeLocalAppend(LocalAppendState &state, DuckTableEntry &table, ClientContext &context,
                                      const vector<unique_ptr<BoundConstraint>> &bound_constraints) {
	if (!IsMainTable()) {
		throw TransactionException("Transaction conflict: attempting to insert into table %s but it has been %s by "
		                           "a different transaction",
		                           GetTableName(), TableModification());
	}
	auto &local_storage = LocalStorage::Get(context, db);
	local_storage.InitializeAppend(state, *this, table);
	state.constraint_state = InitializeConstraintState(table, bound_constraints);
}

void DataTable::InitializeLocalStorage(LocalAppendState &state, DuckTableEntry &table, ClientContext &context,
                                       const vector<unique_ptr<BoundConstraint>> &bound_constraints) {
	if (!IsMainTable()) {
		throw TransactionException("Transaction conflict: attempting to insert into table %s but it has been %s by "
		                           "a different transaction",
		                           GetTableName(), TableModification());
	}

	// The transaction-local indexes are created with the local storage, and only for indexes that are bound
	info->BindIndexes(context);
	auto &local_storage = LocalStorage::Get(context, db);
	local_storage.InitializeStorage(state, *this, table);
	state.constraint_state = InitializeConstraintState(table, bound_constraints);
}

void DataTable::LocalAppend(LocalAppendState &state, DuckTableEntry &table_entry, ClientContext &context,
                            DataChunk &chunk, bool unsafe) {
	if (chunk.size() == 0) {
		return;
	}
	if (!IsMainTable()) {
		throw TransactionException("Transaction conflict: attempting to insert into table %s but it has been %s by "
		                           "a different transaction",
		                           GetTableName(), TableModification());
	}
	chunk.Verify(context.db);

	// Insert any row ids into the DELETE ART and verify constraints afterward.
	// This happens only for the global indexes.
	if (!unsafe) {
		auto &constraint_state = *state.constraint_state;
		VerifyAppendConstraints(constraint_state, context, chunk, *state.storage, nullptr);
	}

	// Append to the transaction-local data.
	LocalStorage::Append(state, table_entry, chunk);
}

void DataTable::LocalAppend(DuckTableEntry &table, ClientContext &context, DataChunk &chunk,
                            const vector<unique_ptr<BoundConstraint>> &bound_constraints, bool unsafe) {
	LocalAppendState append_state;
	InitializeLocalAppend(append_state, table, context, bound_constraints);
	LocalAppend(append_state, table, context, chunk, unsafe);
	FinalizeLocalAppend(append_state);
}

void DataTable::FinalizeLocalAppend(LocalAppendState &state) {
	LocalStorage::FinalizeAppend(state);
}

PhysicalIndex DataTable::CreateOptimisticCollection(ClientContext &context,
                                                    unique_ptr<OptimisticWriteCollection> collection) {
	auto &local_storage = LocalStorage::Get(context, db);
	return local_storage.CreateOptimisticCollection(*this, std::move(collection));
}

OptimisticWriteCollection &DataTable::GetOptimisticCollection(ClientContext &context,
                                                              const PhysicalIndex collection_index) {
	auto &local_storage = LocalStorage::Get(context, db);
	return local_storage.GetOptimisticCollection(*this, collection_index);
}

void DataTable::ResetOptimisticCollection(ClientContext &context, const PhysicalIndex collection_index) {
	auto &local_storage = LocalStorage::Get(context, db);
	local_storage.ResetOptimisticCollection(*this, collection_index);
}

OptimisticDataWriter &DataTable::GetOptimisticWriter(ClientContext &context) {
	auto &local_storage = LocalStorage::Get(context, db);
	return local_storage.GetOptimisticWriter(*this);
}

void DataTable::LocalMerge(ClientContext &context, DuckTableEntry &table_entry, OptimisticWriteCollection &collection) {
	auto &local_storage = LocalStorage::Get(context, db);
	local_storage.LocalMerge(*this, table_entry, collection);
}

void DataTable::LocalWALAppend(DuckTableEntry &table, ClientContext &context, DataChunk &chunk,
                               const vector<unique_ptr<BoundConstraint>> &bound_constraints) {
	LocalAppendState append_state;
	auto &storage = table.GetStorage();
	storage.InitializeLocalAppend(append_state, table, context, bound_constraints);

	storage.LocalAppend(append_state, table, context, chunk, true);
	append_state.storage->index_append_mode = IndexAppendMode::INSERT_DUPLICATES;
	storage.FinalizeLocalAppend(append_state);
}

void DataTable::LocalAppend(DuckTableEntry &table, ClientContext &context, DataChunk &chunk,
                            const vector<unique_ptr<BoundConstraint>> &bound_constraints, Vector &row_ids,
                            DataChunk &delete_chunk) {
	LocalAppendState append_state;
	auto &storage = table.GetStorage();
	storage.InitializeLocalAppend(append_state, table, context, bound_constraints);
	append_state.storage->AppendToDeleteIndexes(row_ids, delete_chunk);

	storage.LocalAppend(append_state, table, context, chunk, false);
	storage.FinalizeLocalAppend(append_state);
}

void DataTable::LocalAppend(DuckTableEntry &table, ClientContext &context, ColumnDataCollection &collection,
                            const vector<unique_ptr<BoundConstraint>> &bound_constraints,
                            optional_ptr<const vector<LogicalIndex>> column_ids) {
	LocalAppendState append_state;
	auto &storage = table.GetStorage();
	storage.InitializeLocalAppend(append_state, table, context, bound_constraints);

	if (!column_ids || column_ids->empty()) {
		for (auto &chunk : collection.Chunks()) {
			storage.LocalAppend(append_state, table, context, chunk, false);
		}
		storage.FinalizeLocalAppend(append_state);
		return;
	}

	auto &column_list = table.GetColumns();
	map<PhysicalIndex, unique_ptr<Expression>> active_expressions;
	for (idx_t i = 0; i < column_ids->size(); i++) {
		auto &col = column_list.GetColumn((*column_ids)[i]);
		auto expr = make_uniq<BoundReferenceExpression>(col.Name(), col.Type(), i);
		active_expressions[col.Physical()] = std::move(expr);
	}

	auto binder = Binder::CreateBinder(context);
	ConstantBinder default_binder(*binder, context, "DEFAULT value");
	vector<unique_ptr<Expression>> expressions;
	for (idx_t i = 0; i < column_list.PhysicalColumnCount(); i++) {
		auto expr = active_expressions.find(PhysicalIndex(i));
		if (expr != active_expressions.end()) {
			expressions.push_back(std::move(expr->second));
			continue;
		}

		auto &col = column_list.GetColumn(PhysicalIndex(i));
		if (!col.HasDefaultValue()) {
			auto null_expr = make_uniq<BoundConstantExpression>(Value(col.Type()));
			expressions.push_back(std::move(null_expr));
			continue;
		}

		auto default_copy = col.DefaultValue().Copy();
		default_binder.target_type = col.Type();
		auto bound_default = default_binder.Bind(default_copy);
		expressions.push_back(std::move(bound_default));
	}

	ExpressionExecutor expression_executor(context, expressions);
	DataChunk result;
	result.Initialize(context, table.GetTypes());

	for (auto &chunk : collection.Chunks()) {
		expression_executor.Execute(chunk, result);
		storage.LocalAppend(append_state, table, context, result, false);
		result.Reset();
	}
	storage.FinalizeLocalAppend(append_state);
}

void DataTable::AppendLock(DuckTransaction &transaction, TableAppendState &state) {
	state.append_lock = unique_lock<mutex>(append_lock);
	if (!IsMainTable()) {
		throw TransactionException("Transaction conflict: attempting to insert into table %s but it has been %s by "
		                           "a different transaction",
		                           GetTableName(), TableModification());
	}
	state.row_start = NumericCast<row_t>(row_groups->GetNextRowId());
	state.current_row = state.row_start;
	auto &transaction_manager = transaction.GetTransactionManager();
	auto active_checkpoint = transaction_manager.GetActiveCheckpoint();
	if (info->AppendRequiresNewRowGroup(*row_groups, active_checkpoint)) {
		// there is a checkpoint active while we are appending
		// in this case we cannot just blindly append to the last row group, because we need to checkpoint that
		// always start a new row group in this case
		row_groups->SetRowGroupAppendMode(RowGroupAppendMode::REQUIRE_NEW);
	}
}

bool DataTableInfo::AppendRequiresNewRowGroup(RowGroupCollection &collection, optional_idx checkpoint_id) {
	if (!checkpoint_id.IsValid()) {
		// no active checkpoint
		return false;
	}
	auto current_segment_count = collection.GetSegmentCount();
	if (last_seen_checkpoint.IsValid() && last_seen_checkpoint.GetIndex() == checkpoint_id.GetIndex()) {
		// we have already seen this checkpoint
		// however, we might still need to append a new row group if a previous append was reverted
		return current_segment_count <= checkpoint_row_group_count.GetIndex();
	}
	// we have not yet seen this checkpoint - we need to append a new row group
	// store the id and the segment count
	last_seen_checkpoint = checkpoint_id;
	checkpoint_row_group_count = current_segment_count;
	return true;
}

optional_idx DataTableInfo::CheckpointRowGroupCount(const CheckpointOptions &options) const {
	if (!last_seen_checkpoint.IsValid() || !options.checkpoint_id.IsValid() ||
	    last_seen_checkpoint.GetIndex() != options.checkpoint_id.GetIndex()) {
		return optional_idx();
	}
	return checkpoint_row_group_count;
}

void DataTable::InitializeAppend(DuckTransaction &transaction, TableAppendState &state) {
	// obtain the append lock for this table
	if (!state.append_lock) {
		throw InternalException("DataTable::AppendLock should be called before DataTable::InitializeAppend");
	}
	row_groups->InitializeAppend(transaction, state);
}

void DataTable::Append(DataChunk &chunk, TableAppendState &state) {
	D_ASSERT(IsMainTable());
	row_groups->Append(chunk, state);
}

void DataTable::FinalizeAppend(DuckTransaction &transaction, TableAppendState &state) {
	row_groups->FinalizeAppend(transaction, state);
}

void DataTable::ScanTableSegment(DuckTransaction &transaction, idx_t row_start, idx_t count,
                                 const std::function<void(DataChunk &chunk)> &function) {
	if (count == 0) {
		return;
	}
	idx_t end = row_start + count;

	vector<StorageIndex> column_ids;
	vector<LogicalType> types;
	for (idx_t i = 0; i < this->column_definitions.size(); i++) {
		auto &col = this->column_definitions[i];
		column_ids.emplace_back(i);
		types.push_back(col.Type());
	}
	DataChunk chunk;
	chunk.Initialize(Allocator::Get(db), types);

	CreateIndexScanState state;

	InitializeScanWithOffset(transaction, state, column_ids, row_start, row_start + count);
	auto row_start_aligned =
	    state.table_state.GetRowGroup()->GetRowStart() + state.table_state.vector_index * STANDARD_VECTOR_SIZE;

	idx_t current_row = row_start_aligned;
	while (current_row < end) {
		state.table_state.Scan(chunk, TableScanType::TABLE_SCAN_ALL_ROWS);
		if (chunk.size() == 0) {
			break;
		}
		idx_t end_row = current_row + chunk.size();
		// start of chunk is current_row
		// end of chunk is end_row
		// figure out if we need to write the entire chunk or just part of it
		idx_t chunk_start = MaxValue<idx_t>(current_row, row_start);
		idx_t chunk_end = MinValue<idx_t>(end_row, end);
		D_ASSERT(chunk_start < chunk_end);
		idx_t chunk_count = chunk_end - chunk_start;
		if (chunk_count != chunk.size()) {
			D_ASSERT(chunk_count <= chunk.size());
			// need to slice the chunk before insert
			idx_t start_in_chunk;
			if (current_row >= row_start) {
				start_in_chunk = 0;
			} else {
				start_in_chunk = row_start - current_row;
			}
			SelectionVector sel(start_in_chunk, chunk_count);
			chunk.Slice(sel, chunk_count);
			chunk.Verify(GetAttached().GetDatabase());
		}
		function(chunk);
		chunk.Reset();
		current_row = end_row;
	}
}

void DataTable::MergeStorage(RowGroupCollection &data, optional_ptr<StorageCommitState> commit_state) {
	row_groups->MergeStorage(data, this, commit_state);
	row_groups->Verify();
}

void DataTable::WriteToLog(DuckTransaction &transaction, WriteAheadLog &log, idx_t row_start, idx_t count,
                           optional_ptr<StorageCommitState> commit_state) {
	log.WriteSetTable(QualifiedName(info->GetSchemaPath(), info->GetTableName()));
	if (!commit_state) {
		ScanTableSegment(transaction, row_start, count, [&](DataChunk &chunk) { log.WriteInsert(chunk); });
		return;
	}

	idx_t optimistic_count = 0;
	auto entry = commit_state->GetRowGroupData(*this, row_start, optimistic_count);
	if (!entry) {
		ScanTableSegment(transaction, row_start, count, [&](DataChunk &chunk) { log.WriteInsert(chunk); });
		return;
	}

	// Optimistically write the entry.
	D_ASSERT(optimistic_count > 0);
	log.WriteRowGroupData(*entry);
	if (optimistic_count > count) {
		throw InternalException(
		    "Optimistically written count cannot exceed actual count (got %llu, but expected count is %llu)",
		    optimistic_count, count);
	}

	// Write any remaining (non-optimistically written) rows to the WAL.
	row_start += optimistic_count;
	count -= optimistic_count;
	if (count == 0) {
		return;
	}

	ScanTableSegment(transaction, row_start, count, [&](DataChunk &chunk) { log.WriteInsert(chunk); });
}

void DataTable::CommitAppend(transaction_t commit_id, idx_t row_start, idx_t count) {
	lock_guard<mutex> lock(append_lock);
	row_groups->CommitAppend(commit_id, row_start, count);
}

void DataTable::RevertAppendInternal(idx_t start_row) {
	D_ASSERT(IsMainTable());
	// revert appends made to row_groups
	row_groups->RevertAppendInternal(start_row);
}

void DataTable::RevertFailedAppend(TableAppendState &state) {
	D_ASSERT(IsMainTable());
	row_groups->RevertFailedAppend(state);
}

void DataTable::RevertAppend(DuckTransaction &transaction, idx_t start_row, idx_t count) {
	lock_guard<mutex> lock(append_lock);

	// revert any appends to indexes
	if (!info->indexes.Empty()) {
		idx_t current_row_base = start_row;
		row_t row_data[STANDARD_VECTOR_SIZE];
		Vector row_identifiers(LogicalType::ROW_TYPE, data_ptr_cast(row_data), STANDARD_VECTOR_SIZE);
		auto next_row_id = row_groups->GetNextRowId();
		D_ASSERT(start_row <= next_row_id);
		idx_t scan_count = MinValue<idx_t>(count, next_row_id - start_row);
		ScanTableSegment(transaction, start_row, scan_count, [&](DataChunk &chunk) {
			auto row_id_writer = FlatVector::Writer<row_t>(row_identifiers, chunk.size());
			for (idx_t i = 0; i < chunk.size(); i++) {
				row_id_writer.WriteValue(NumericCast<row_t>(current_row_base + i));
			}
			info->indexes.RevertAppend(chunk, row_identifiers);
			current_row_base += chunk.size();
		});
	}

#ifdef DEBUG
	// Verify that our index memory is stable.
	info->indexes.VerifyBuffers();
#endif

	if (!IsMainTable()) {
		//! The table was altered by another transaction since we staged our commit, revert it
		//! Causing the commit of the other transaction to fail
		version = DataTableVersion::MAIN_TABLE;
	}
	// revert the data table append
	RevertAppendInternal(start_row);
}

//===--------------------------------------------------------------------===//
// Indexes
//===--------------------------------------------------------------------===//
void DataTable::RemoveFromIndexes(const QueryContext &context, Vector &row_identifiers, idx_t count,
                                  IndexRemovalType removal_type, optional_idx active_checkpoint) {
	row_groups->RemoveFromIndexes(context, info->indexes, row_identifiers, count, removal_type, active_checkpoint);
}

//===--------------------------------------------------------------------===//
// Delete
//===--------------------------------------------------------------------===//
static bool TableHasDeleteConstraints(TableCatalogEntry &table) {
	for (auto &constraint : table.GetConstraints()) {
		switch (constraint->type) {
		case ConstraintType::NOT_NULL:
		case ConstraintType::CHECK:
		case ConstraintType::UNIQUE:
			break;
		case ConstraintType::FOREIGN_KEY: {
			auto &foreign_key = constraint->Cast<ForeignKeyConstraint>();
			if (foreign_key.info.IsDeleteConstraint()) {
				return true;
			}
			break;
		}
		default:
			throw NotImplementedException("Constraint type not implemented!");
		}
	}
	return false;
}

void DataTable::VerifyDeleteConstraints(optional_ptr<LocalTableStorage> storage, TableDeleteState &state,
                                        ClientContext &context, DataChunk &chunk) {
	for (auto &constraint : state.constraint_state->bound_constraints) {
		switch (constraint->type) {
		case ConstraintType::NOT_NULL:
		case ConstraintType::CHECK:
		case ConstraintType::UNIQUE:
			break;
		case ConstraintType::FOREIGN_KEY: {
			auto &bound_foreign_key = constraint->Cast<BoundForeignKeyConstraint>();
			if (bound_foreign_key.info.IsDeleteConstraint()) {
				VerifyDeleteForeignKeyConstraint(storage, bound_foreign_key, context, chunk);
			}
			break;
		}
		default:
			throw NotImplementedException("Constraint type not implemented!");
		}
	}
}

unique_ptr<TableDeleteState> DataTable::InitializeDelete(TableCatalogEntry &table, ClientContext &context,
                                                         const vector<unique_ptr<BoundConstraint>> &bound_constraints) {
	// Bind all indexes.
	info->BindIndexes(context);

	auto binder = Binder::CreateBinder(context);
	vector<LogicalType> types;
	auto result = make_uniq<TableDeleteState>();
	result->has_delete_constraints = TableHasDeleteConstraints(table);
	if (result->has_delete_constraints) {
		// initialize the chunk if there are any constraints to verify
		for (idx_t i = 0; i < column_definitions.size(); i++) {
			result->col_ids.emplace_back(column_definitions[i].StorageOid());
			types.emplace_back(column_definitions[i].Type());
		}
		result->verify_chunk.Initialize(Allocator::Get(context), types);
		result->constraint_state = make_uniq<ConstraintState>(table, bound_constraints);
	}
	return result;
}

idx_t DataTable::Delete(TableDeleteState &state, ClientContext &context, DuckTableEntry &table_entry,
                        Vector &row_identifiers, idx_t count) {
	D_ASSERT(row_identifiers.GetType().InternalType() == ROW_TYPE);
	if (count == 0) {
		return 0;
	}

	auto &transaction = DuckTransaction::Get(context, db);
	auto &local_storage = LocalStorage::Get(transaction);
	auto storage = local_storage.GetStorage(*this);

	row_identifiers.Flatten();
	auto ids = FlatVector::GetDataMutable<row_t>(row_identifiers);

	idx_t pos = 0;
	idx_t delete_count = 0;
	while (pos < count) {
		idx_t start = pos;
		bool is_transaction_delete = ids[pos] >= MAX_ROW_ID;
		// figure out which batch of rows to delete now
		for (pos++; pos < count; pos++) {
			bool row_is_transaction_delete = ids[pos] >= MAX_ROW_ID;
			if (row_is_transaction_delete != is_transaction_delete) {
				break;
			}
		}
		idx_t current_offset = start;
		idx_t current_count = pos - start;

		Vector offset_ids(row_identifiers, current_offset, pos);

		// This is a transaction-local DELETE.
		if (is_transaction_delete) {
			if (state.has_delete_constraints) {
				// Verify any delete constraints.
				ColumnFetchState fetch_state;
				local_storage.FetchChunk(*this, offset_ids, current_count, state.col_ids, state.verify_chunk,
				                         fetch_state);
				VerifyDeleteConstraints(storage, state, context, state.verify_chunk);
			}
			delete_count += local_storage.Delete(*this, table_entry, offset_ids, current_count);
			continue;
		}

		// This is a regular DELETE.
		if (state.has_delete_constraints) {
			// Verify any delete constraints.
			ColumnFetchState fetch_state;
			Fetch(transaction, state.verify_chunk, state.col_ids, offset_ids, current_count, fetch_state);
			VerifyDeleteConstraints(storage, state, context, state.verify_chunk);
		}
		delete_count += row_groups->Delete(transaction, table_entry, ids + current_offset, current_count);
	}
	return delete_count;
}

//===--------------------------------------------------------------------===//
// Update
//===--------------------------------------------------------------------===//
static void CreateMockChunk(vector<LogicalType> &types, const vector<PhysicalIndex> &column_ids, DataChunk &chunk,
                            DataChunk &mock_chunk) {
	// construct a mock DataChunk
	mock_chunk.InitializeEmpty(types);
	for (column_t i = 0; i < column_ids.size(); i++) {
		mock_chunk.data[column_ids[i].index].Reference(chunk.data[i]);
	}
}

static bool CreateMockChunk(TableCatalogEntry &table, const vector<PhysicalIndex> &column_ids,
                            physical_index_set_t &desired_column_ids, DataChunk &chunk, DataChunk &mock_chunk) {
	idx_t found_columns = 0;
	// check whether the desired columns are present in the UPDATE clause
	for (column_t i = 0; i < column_ids.size(); i++) {
		if (desired_column_ids.find(column_ids[i]) != desired_column_ids.end()) {
			found_columns++;
		}
	}
	if (found_columns == 0) {
		// no columns were found: no need to check the constraint again
		return false;
	}
	if (found_columns != desired_column_ids.size()) {
		// not all columns in UPDATE clause are present!
		// this should not be triggered at all as the binder should add these columns
		throw InternalException("Not all columns required for the CHECK constraint are present in the UPDATED chunk!");
	}
	// construct a mock DataChunk
	auto types = table.GetTypes();
	CreateMockChunk(types, column_ids, chunk, mock_chunk);
	return true;
}

void DataTable::VerifyUpdateConstraints(ConstraintState &state, ClientContext &context, DataChunk &chunk,
                                        const vector<PhysicalIndex> &column_ids) {
	auto &table = state.table;
	auto &constraints = table.GetConstraints();
	auto &bound_constraints = state.bound_constraints;
	for (idx_t constr_idx = 0; constr_idx < bound_constraints.size(); constr_idx++) {
		auto &base_constraint = constraints[constr_idx];
		auto &constraint = bound_constraints[constr_idx];
		switch (constraint->type) {
		case ConstraintType::NOT_NULL: {
			auto &bound_not_null = constraint->Cast<BoundNotNullConstraint>();
			auto &not_null = base_constraint->Cast<NotNullConstraint>();
			// check if the constraint is in the list of column_ids
			for (idx_t col_idx = 0; col_idx < column_ids.size(); col_idx++) {
				if (column_ids[col_idx] == bound_not_null.index) {
					// found the column id: check the data in
					auto &col = table.GetColumn(LogicalIndex(not_null.index));
					VerifyNotNullConstraint(table, chunk.data[col_idx], col.Name());
					break;
				}
			}
			break;
		}
		case ConstraintType::CHECK: {
			auto &check = base_constraint->Cast<CheckConstraint>();
			auto &bound_check = constraint->Cast<BoundCheckConstraint>();

			DataChunk mock_chunk;
			if (CreateMockChunk(table, column_ids, bound_check.bound_columns, chunk, mock_chunk)) {
				VerifyCheckConstraint(context, table, *bound_check.expression, mock_chunk, check);
			}
			break;
		}
		case ConstraintType::UNIQUE:
		case ConstraintType::FOREIGN_KEY:
			break;
		default:
			throw NotImplementedException("Constraint type not implemented!");
		}
	}
	// Ensure that we never call UPDATE for indexed columns.
	// Instead, we must rewrite these updates into DELETE + INSERT.
	info->indexes.VerifyUpdate(column_ids);
}

unique_ptr<TableUpdateState> DataTable::InitializeUpdate(TableCatalogEntry &table, ClientContext &context,
                                                         const vector<unique_ptr<BoundConstraint>> &bound_constraints) {
	// Bind all indexes.
	info->BindIndexes(context);
	auto result = make_uniq<TableUpdateState>();
	result->constraint_state = InitializeConstraintState(table, bound_constraints);
	return result;
}

void DataTable::Update(TableUpdateState &state, ClientContext &context, DuckTableEntry &table_entry, Vector &row_ids,
                       const vector<PhysicalIndex> &column_ids, DataChunk &updates) {
	D_ASSERT(row_ids.GetType().InternalType() == ROW_TYPE);
	D_ASSERT(column_ids.size() == updates.ColumnCount());
	updates.Verify(context.db);

	auto count = updates.size();
	if (count == 0) {
		return;
	}

	if (!IsMainTable()) {
		throw TransactionException(
		    "Transaction conflict: attempting to update table %s but it has been %s by a different transaction",
		    GetTableName(), TableModification());
	}

	// first verify that no constraints are violated
	VerifyUpdateConstraints(*state.constraint_state, context, updates, column_ids);

	// now perform the actual update
	Vector max_row_id_vec(Value::BIGINT(MAX_ROW_ID), count_t(count));
	Vector row_ids_slice(LogicalType::BIGINT);
	DataChunk updates_slice;
	updates_slice.InitializeEmpty(updates.GetTypes());

	SelectionVector sel_local_update(count), sel_global_update(count);
	auto n_local_update = VectorOperations::GreaterThanEquals(row_ids, max_row_id_vec, nullptr, count,
	                                                          &sel_local_update, &sel_global_update);
	auto n_global_update = count - n_local_update;

	// row id > MAX_ROW_ID? transaction-local storage
	if (n_local_update > 0) {
		updates_slice.Slice(updates, sel_local_update, n_local_update);
		updates_slice.Flatten();
		row_ids_slice.Slice(row_ids, sel_local_update, n_local_update);
		row_ids_slice.Flatten();

		LocalStorage::Get(context, db).Update(*this, table_entry, row_ids_slice, column_ids, updates_slice);
	}

	// otherwise global storage
	if (n_global_update > 0) {
		auto &transaction = DuckTransaction::Get(context, db);
		updates_slice.Slice(updates, sel_global_update, n_global_update);
		updates_slice.Flatten();
		row_ids_slice.Slice(row_ids, sel_global_update, n_global_update);
		row_ids_slice.Flatten();

		row_groups->Update(transaction, table_entry, FlatVector::GetDataMutable<row_t>(row_ids_slice), column_ids,
		                   updates_slice);
	}
}

void DataTable::UpdateColumn(DuckTableEntry &table, ClientContext &context, Vector &row_ids,
                             const vector<column_t> &column_path, DataChunk &updates) {
	D_ASSERT(row_ids.GetType().InternalType() == ROW_TYPE);
	D_ASSERT(updates.ColumnCount() == 1);
	updates.Verify(context.db);
	if (updates.size() == 0) {
		return;
	}

	if (!IsMainTable()) {
		throw TransactionException(
		    "Transaction conflict: attempting to update table %s but it has been %s by a different transaction",
		    GetTableName(), TableModification());
	}

	// now perform the actual update
	auto &transaction = DuckTransaction::Get(context, db);

	updates.Flatten();
	row_ids.Flatten();
	row_groups->UpdateColumn(transaction, table, row_ids, column_path, updates);
}

//===--------------------------------------------------------------------===//
// Statistics
//===--------------------------------------------------------------------===//
unique_ptr<BaseStatistics> DataTable::GetStatistics(ClientContext &context, const StorageIndex &column_id) {
	if (column_id.IsRowIdColumn()) {
		return nullptr;
	}
	return row_groups->CopyStats(column_id);
}

void DataTable::SetDistinct(column_t column_id, unique_ptr<DistinctStatistics> distinct_stats) {
	D_ASSERT(column_id != COLUMN_IDENTIFIER_ROW_ID);
	row_groups->SetDistinct(column_id, std::move(distinct_stats));
}

unique_ptr<BlockingSample> DataTable::GetSample() {
	return row_groups->GetSample();
}

//===--------------------------------------------------------------------===//
// Checkpoint
//===--------------------------------------------------------------------===//
void DataTable::Checkpoint(TableDataWriter &writer, Serializer &serializer) {
	TableStatistics global_stats;
	CollectionCheckpointSnapshot snapshot;
	{
		lock_guard<mutex> lock(append_lock);
		snapshot = row_groups->SnapshotForCheckpoint(writer);
	}
	// checkpoint each individual row group
	auto result = row_groups->Checkpoint(writer, global_stats, snapshot);
	{
		lock_guard<mutex> lock(append_lock);
		row_groups->InstallCheckpoint(std::move(result), global_stats);
		row_groups->SetRowGroupAppendMode(RowGroupAppendMode::SUGGEST_NEW);
	}
	if (writer.GetRebuildIndexes()) {
		MetricsTimer timer;
		auto context = writer.TryGetClientContext();
		if (context) {
			timer = QueryProfiler::Get(*context).StartTimer<MetricStorageTotalVacuumTime>();
		}
		RebuildIndexes();
		timer.EndTimer();
	}

	const auto storage_version = serializer.GetOptions().storage_compatibility.storage_version;
	const auto index_writer = writer.GetTableIndexWriter(storage_version);
	vector<shared_ptr<const IndexStorageInfo>> index_infos;
	if (index_writer) {
		// Only checkpoint indexes when we write to disk
		index_infos = info->GetIndexes().Checkpoint(*index_writer);
	}

	writer.FinalizeTable(global_stats, *info, *row_groups, index_infos, serializer);
}

void DataTable::CommitDropColumn(const idx_t column_index, CommitDropState &drop_state) {
	row_groups->CommitDropColumn(column_index, drop_state);
}

void DataTable::Destroy() {
	row_groups->Destroy();
}

idx_t DataTable::ColumnCount() const {
	return column_definitions.size();
}

idx_t DataTable::GetTotalRows() const {
	return row_groups->GetTotalRows();
}

idx_t DataTable::GetNextRowId() const {
	return row_groups->GetNextRowId();
}

void DataTable::CommitDropTable(CommitDropState &drop_state) {
	row_groups->CommitDropTable(drop_state);
}

idx_t DataTable::GetRowGroupCount() const {
	return row_groups->GetRowGroupCount();
}

idx_t DataTable::GetRowGroupCountWithLocalStorage(ClientContext &context) {
	auto &local_storage = LocalStorage::Get(context, db);
	auto storage = local_storage.GetStorage(*this);
	if (!storage) {
		return GetRowGroupCount();
	}
	return GetRowGroupCount() + storage->GetCollection().GetRowGroupCount();
}

//===--------------------------------------------------------------------===//
// Column Segment Info
//===--------------------------------------------------------------------===//
vector<ColumnSegmentInfo> DataTable::GetColumnSegmentInfo(const QueryContext &context,
                                                          const ColumnSegmentInfoScanOptions &options) {
	return row_groups->GetColumnSegmentInfo(context, options);
}

void DataTable::InitializeColumnSegmentInfoScan(ColumnSegmentInfoScanState &state) {
	row_groups->InitializeColumnSegmentInfoScan(state);
}

bool DataTable::ScanColumnSegmentInfo(const QueryContext &context, ColumnSegmentInfoScanState &state,
                                      vector<ColumnSegmentInfo> &result) {
	return row_groups->ScanColumnSegmentInfo(context, state, result);
}

//===--------------------------------------------------------------------===//
// Index Constraint Creation
//===--------------------------------------------------------------------===//
void DataTable::AddIndex(const ColumnList &columns, const vector<LogicalIndex> &column_indexes,
                         const IndexConstraintType type, IndexStorageInfo index_info, idx_t index_oid,
                         const ConstraintCheckMode check_mode) {
	if (!IsMainTable()) {
		throw TransactionException("Transaction conflict: attempting to add an index to table %s but it has been "
		                           "%s by a different transaction",
		                           GetTableName(), TableModification());
	}

	// Fetch the column types and create bound column reference expressions.
	vector<column_t> physical_ids;
	vector<unique_ptr<Expression>> expressions;

	for (const auto column_index : column_indexes) {
		auto binding = ColumnBinding(TableIndex(0), ProjectionIndex(physical_ids.size()));
		auto &col = columns.GetColumn(column_index);
		auto ref = make_uniq<BoundColumnRefExpression>(col.Name(), col.Type(), binding);
		expressions.push_back(std::move(ref));
		physical_ids.push_back(col.Physical().index);
	}

	// Create an ART around the expressions.
	auto &io_manager = TableIOManager::Get(*this);
	auto art = make_uniq<ART>(index_info.name, type, physical_ids, io_manager, std::move(expressions), db, nullptr,
	                          index_info);
	info->indexes.AddIndex(std::move(art), index_oid, check_mode);
}

} // namespace duckdb
