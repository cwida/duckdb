#include "duckdb/storage/table/table_index_list.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/storage/checkpoint/table_index_writer.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/common/types/constraint_conflict_info.hpp"
#include "duckdb/common/types/conflict_manager.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/storage/index_storage_info.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/unbound_index.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/main/attached_database.hpp"
#include <utility>

namespace duckdb {

template <class T>
TableIndexIterationHelper<T>::TableIndexIterationHelper(const TableIndexList &index_list)
    : lock(index_list.index_entries_lock), index_entries(index_list.index_entries) {
}

template <class T>
TableIndexIterationHelper<T>::TableIndexIterator::TableIndexIterator(
    optional_ptr<const vector<shared_ptr<IndexEntry>>> index_entries_p)
    : index_entries(index_entries_p) {
	if (index_entries) {
		if (index_entries->empty()) {
			index_entries = nullptr;
		} else {
			index = 0;
		}
	}
}

template <class T>
typename TableIndexIterationHelper<T>::TableIndexIterator &
TableIndexIterationHelper<T>::TableIndexIterator::operator++() {
	if (index_entries) {
		auto next_index = index.GetIndex() + 1;
		if (next_index >= index_entries->size()) {
			// reached the end
			index = optional_idx();
			index_entries = nullptr;
		} else {
			// next index
			index = next_index;
		}
	}
	return *this;
}

template <class T>
bool TableIndexIterationHelper<T>::TableIndexIterator::operator!=(const TableIndexIterator &other) const {
	return index != other.index || index_entries != other.index_entries;
}

TableIndexIterationHelper<shared_ptr<IndexEntry>> TableIndexList::IndexEntries() const {
	return TableIndexIterationHelper<shared_ptr<IndexEntry>>(*this);
}

template <>
shared_ptr<IndexEntry> TableIndexIterationHelper<shared_ptr<IndexEntry>>::TableIndexIterator::operator*() const {
	return index_entries->at(index.GetIndex());
}

template class TableIndexIterationHelper<shared_ptr<IndexEntry>>;

TableIndexList::~TableIndexList() {
	vector<shared_ptr<IndexEntry>> entries;
	{
		annotated_lock_guard lock(index_entries_lock);
		entries = std::move(index_entries);
		unbound_count = 0;
	}
	for (auto &entry : entries) {
		entry->Retire();
	}
}

shared_ptr<IndexEntry> TableIndexList::AddIndex(unique_ptr<Index> index, idx_t index_oid,
                                                const ConstraintCheckMode check_mode) {
	D_ASSERT(index);
	D_ASSERT(index_oid != DConstants::INVALID_INDEX);
	annotated_lock_guard lock(index_entries_lock);
	auto index_entry = make_shared_ptr<IndexEntry>(std::move(index), index_oid, check_mode);
	if (index_entry->GetBindState() != IndexBindState::BOUND) {
		unbound_count++;
	}
	index_entries.push_back(index_entry);
	return index_entry;
}

void TableIndexList::InitializeLocalIndexes(TableIndexList &delete_indexes, TableIndexList &append_indexes) const {
	D_ASSERT(this != &delete_indexes);
	D_ASSERT(this != &append_indexes);
	D_ASSERT(&delete_indexes != &append_indexes);
	D_ASSERT(delete_indexes.Empty());
	D_ASSERT(append_indexes.Empty());

	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		entry->InitializeLocalIndexes(delete_indexes, append_indexes);
	}
}

void TableIndexList::Append(DataChunk &chunk, Vector &row_ids) {
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		entry->Append(chunk, row_ids);
	}
}

ErrorData TableIndexList::Append(optional_ptr<TableIndexList> delete_indexes, DataChunk &chunk, row_t row_start,
                                 IndexAppendMode append_mode, optional_idx active_checkpoint) {
	Vector row_ids(LogicalType::ROW_TYPE);
	VectorOperations::GenerateSequence(row_ids, chunk.size(), row_start, 1);

	annotated_lock_guard lock(index_entries_lock);
	vector<shared_ptr<IndexEntry>> already_appended;

	ErrorData error;
	for (const auto &entry : index_entries) {
		shared_ptr<IndexEntry> delete_entry;
		if (delete_indexes && entry->IsUnique()) {
			delete_entry = delete_indexes->FindEntry(*entry);
		}
		error = entry->Append(chunk, row_ids, delete_entry, append_mode, active_checkpoint);
		if (error.HasError()) {
			break;
		}
		already_appended.push_back(entry);
	}

	if (error.HasError()) {
		for (auto &entry : already_appended) {
			entry->RevertAppend(chunk, row_ids);
		}
	}
	return error;
}

void TableIndexList::RevertAppend(DataChunk &chunk, Vector &row_ids) {
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		entry->RevertAppend(chunk, row_ids);
	}
}

void TableIndexList::RevertAppend(DataChunk &chunk, row_t row_start) {
	Vector row_ids(LogicalType::ROW_TYPE);
	VectorOperations::GenerateSequence(row_ids, chunk.size(), row_start, 1);
	RevertAppend(chunk, row_ids);
}

void TableIndexList::AppendToDeleteIndexes(DataChunk &chunk, Vector &row_ids) {
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		entry->AppendToDeleteIndexes(chunk, row_ids);
	}
}

void TableIndexList::RemoveFromIndexes(DataChunk &chunk, Vector &row_ids, const IndexRemovalType removal_type,
                                       const optional_idx active_checkpoint) {
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		entry->RemoveFromIndex(chunk, row_ids, removal_type, active_checkpoint);
	}
}

void TableIndexList::RemoveIndex(idx_t index_oid) {
	shared_ptr<IndexEntry> removed_entry;
	{
		annotated_lock_guard lock(index_entries_lock);
		for (idx_t i = 0; i < index_entries.size(); i++) {
			auto &entry = index_entries[i];
			if (entry->GetIndexOid() != index_oid) {
				continue;
			}
			if (entry->GetBindState() != IndexBindState::BOUND) {
				unbound_count--;
			}
			removed_entry = std::move(entry);
			index_entries.erase_at(i);
			break;
		}
	}
	if (removed_entry) {
		removed_entry->Retire();
	}
}

bool TableIndexList::HasUniqueIndexes() const {
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		if (entry->IsUnique()) {
			return true;
		}
	}
	return false;
}

void TableIndexList::VerifyUniqueIndexes(optional_ptr<const TableIndexList> delete_indexes, DataChunk &chunk,
                                         optional_ptr<ConflictManager> manager) const {
	annotated_lock_guard lock(index_entries_lock);
	if (!manager) {
		for (const auto &entry : index_entries) {
			if (!entry->IsUnique() || entry->IsDeferred() || entry->GetIndexType() != ART::TYPE_NAME) {
				continue;
			}
			auto delete_entry = delete_indexes ? delete_indexes->FindEntry(*entry) : nullptr;
			entry->VerifyAppend(delete_entry, chunk, nullptr);
		}
		return;
	}

	// Find all indexes matching the conflict target.
	const auto &conflict_info = manager->GetConflictInfo();
	for (const auto &entry : index_entries) {
		auto index_info = entry->GetStorageInfo();
		if (!index_info.is_unique || index_info.check_mode == ConstraintCheckMode::DEFERRED ||
		    entry->GetIndexType() != ART::TYPE_NAME ||
		    !conflict_info.ConflictTargetMatches(index_info.is_unique, index_info.column_set)) {
			continue;
		}
		auto delete_entry = delete_indexes ? delete_indexes->FindEntry(*entry) : nullptr;
		manager->AddIndex(entry, std::move(delete_entry));
	}

	// Verify indexes matching the conflict target.
	manager->SetMode(ConflictManagerMode::SCAN);
	const auto &matching_indexes = manager->MatchingIndexes();
	const auto &matching_delete_indexes = manager->MatchingDeleteIndexes();
	for (idx_t i = 0; i < matching_indexes.size(); i++) {
		matching_indexes[i]->VerifyAppend(matching_delete_indexes[i], chunk, manager);
	}

	// Scan the other indexes and throw if there are any conflicts.
	manager->SetMode(ConflictManagerMode::THROW);
	for (const auto &entry : index_entries) {
		if (!entry->IsUnique() || entry->IsDeferred() || entry->GetIndexType() != ART::TYPE_NAME ||
		    manager->IndexMatches(entry)) {
			continue;
		}
		auto delete_entry = delete_indexes ? delete_indexes->FindEntry(*entry) : nullptr;
		entry->VerifyAppend(delete_entry, chunk, manager);
	}
}

void TableIndexList::Vacuum() {
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		entry->Vacuum();
	}
}

void TableIndexList::Rebuild(const IndexRebuildScan &scan) {
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		entry->Rebuild(scan);
	}
}

void TableIndexList::VerifyBuffers() const {
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		entry->VerifyBuffers();
	}
}

void TableIndexList::VerifyUpdate(const vector<PhysicalIndex> &column_ids) const {
#ifdef DEBUG
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		entry->VerifyUpdate(column_ids);
	}
#else
	(void)column_ids;
#endif
}

vector<IndexInfo> TableIndexList::GetStorageInfo() const {
	annotated_lock_guard lock(index_entries_lock);
	vector<IndexInfo> result;
	result.reserve(index_entries.size());
	for (const auto &entry : index_entries) {
		result.push_back(entry->GetStorageInfo());
	}
	return result;
}

idx_t TableIndexList::GetInMemorySize() const {
	annotated_lock_guard lock(index_entries_lock);
	idx_t result = 0;
	for (const auto &entry : index_entries) {
		result += entry->GetInMemorySize();
	}
	return result;
}

unordered_set<string> TableIndexList::DistinctIndexTypes() const {
	annotated_lock_guard lock(index_entries_lock);
	unordered_set<string> result;
	for (const auto &entry : index_entries) {
		result.insert(entry->GetIndexType());
	}
	return result;
}

bool TableIndexList::AllIndexesBoundOfType(const string &index_type) const {
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		if (entry->GetBindState() != IndexBindState::BOUND || entry->GetIndexType() != index_type) {
			return false;
		}
	}
	return true;
}

bool TableIndexList::HasBufferedReplays() const {
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		if (entry->HasBufferedReplays()) {
			return true;
		}
	}
	return false;
}

shared_ptr<IndexEntry> TableIndexList::FindEntry(const IndexEntry &index) const {
	return FindEntry(index.GetIndexOid());
}

shared_ptr<IndexEntry> TableIndexList::FindEntry(idx_t index_oid) const {
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		if (entry->GetIndexOid() != index_oid) {
			continue;
		}
		if (entry->GetBindState() != IndexBindState::BOUND) {
			throw InternalException("TableIndexList::FindEntry cannot return an unbound index");
		}
		return entry;
	}
	return nullptr;
}

void TableIndexList::Bind(ClientContext &context, DataTableInfo &table_info, const optional<string> &index_type) {
	{
		// Early-out, if we have no unbound indexes.
		annotated_lock_guard lock(index_entries_lock);
		if (unbound_count == 0) {
			return;
		}
	}

	// Get the table from the catalog, so we can add it to the binder.
	auto &catalog = table_info.GetDB().GetCatalog();
	auto &table_entry = catalog.GetEntry<TableCatalogEntry>(
	    context,
	    QualifiedName::FromCatalogSchema(catalog.GetName(), table_info.GetSchemaPath(), table_info.GetTableName()));
	auto &table = table_entry.Cast<DuckTableEntry>();

	vector<LogicalType> column_types;
	vector<string> column_names;
	for (auto &col : table.GetColumns().Logical()) {
		column_types.push_back(col.Type());
		column_names.emplace_back(col.Name());
	}

	annotated_unique_lock lock(index_entries_lock);
	// Busy-spin trying to bind all indexes.
	while (true) {
		shared_ptr<IndexEntry> index_entry;
		for (auto &entry : index_entries) {
			if (entry->GetBindState() != IndexBindState::BOUND &&
			    (!index_type || entry->GetIndexType() == *index_type)) {
				index_entry = entry;
				break;
			}
		}
		if (!index_entry) {
			// We bound all indexes. (of this type)
			break;
		}
		if (index_entry->GetBindState() == IndexBindState::BINDING) {
			// Another thread is binding the index.
			// Lock and unlock the index entries so that the other thread can commit its changes.
			lock.unlock();
			lock.lock();
			continue;

		} else if (index_entry->GetBindState() == IndexBindState::UNBOUND) {
			// We are the thread that'll bind the index.
			index_entry->SetBindState(IndexBindState::BINDING);
			lock.unlock();

		} else {
			throw InternalException("index entry bind state cannot be BOUND here");
		}

		// Create a binder to bind this index.
		auto binder = Binder::CreateBinder(context);

		// Add the table to the binder.
		vector<ColumnIndex> dummy_column_ids;
		binder->bind_context.AddBaseTable(TableIndex(0), Identifier(), StringsToIdentifiers(column_names), column_types,
		                                  dummy_column_ids, table);

		// Create an IndexBinder to bind the index
		IndexBinder idx_binder(*binder, context);

		// Apply any outstanding buffered replays and replace the unbound index with a bound index.
		unique_ptr<BoundIndex> bound_idx;
		{
			vector<LogicalType> physical_column_types;
			for (auto &col : table.GetColumns().Physical()) {
				physical_column_types.push_back(col.Type());
			}
			bound_idx = index_entry->Bind(idx_binder, physical_column_types);
		}

		// Commit the bound index to the index entry.
		lock.lock();
		auto current_entry = std::find(index_entries.begin(), index_entries.end(), index_entry);
		if (current_entry == index_entries.end()) {
			continue;
		}
		index_entry->CommitBind(std::move(bound_idx));
		unbound_count--;
	}
}

shared_ptr<IndexEntry> TableIndexList::FindForeignKeyIndex(const vector<PhysicalIndex> &fk_keys,
                                                           const ForeignKeyType fk_type) {
	annotated_lock_guard<annotated_mutex> lock(index_entries_lock);
	for (auto &entry : index_entries) {
		if (entry->IsForeignKeyIndex(fk_keys, fk_type)) {
			return entry;
		}
	}
	return nullptr;
}

void TableIndexList::VerifyForeignKey(optional_ptr<const TableIndexList> delete_indexes,
                                      const vector<PhysicalIndex> &fk_keys, DataChunk &chunk,
                                      ConflictManager &conflict_manager) {
	const auto fk_type = conflict_manager.GetVerifyExistenceType() == VerifyExistenceType::APPEND_FK
	                         ? ForeignKeyType::FK_TYPE_PRIMARY_KEY_TABLE
	                         : ForeignKeyType::FK_TYPE_FOREIGN_KEY_TABLE;

	// Check whether the chunk can be inserted in or deleted from the referenced table storage.
	annotated_lock_guard lock(index_entries_lock);
	shared_ptr<IndexEntry> entry;
	for (const auto &candidate : index_entries) {
		if (candidate->IsForeignKeyIndex(fk_keys, fk_type)) {
			entry = candidate;
			break;
		}
	}
	if (!entry) {
		throw InternalException("TableIndexList::VerifyForeignKey failed to find foreign key index");
	}

	auto delete_entry = delete_indexes ? delete_indexes->FindEntry(*entry) : nullptr;
	entry->VerifyForeignKey(delete_entry, chunk, conflict_manager);
}

unordered_set<column_t> TableIndexList::GetIndexedColumns() const {
	annotated_lock_guard lock(index_entries_lock);
	unordered_set<column_t> column_ids;
	for (const auto &entry : index_entries) {
		auto index_info = entry->GetStorageInfo();
		column_ids.insert(index_info.column_set.begin(), index_info.column_set.end());
	}
	return column_ids;
}

vector<shared_ptr<const IndexStorageInfo>> TableIndexList::Checkpoint(TableIndexWriter &writer) {
	annotated_lock_guard lock(index_entries_lock);

	vector<unique_ptr<BoundIndex>> shadows;
	vector<shared_ptr<const IndexStorageInfo>> infos;
	shadows.reserve(index_entries.size());
	infos.reserve(index_entries.size());

	for (const auto &entry : index_entries) {
		auto checkpoint = entry->Checkpoint(writer);
		shadows.push_back(std::move(checkpoint.shadow_index));
		infos.push_back(std::move(checkpoint.storage_info));
	}

	writer.Flush();

	for (idx_t i = 0; i < index_entries.size(); i++) {
		index_entries[i]->InstallShadow(std::move(shadows[i]));
	}

	return infos;
}

vector<unordered_set<column_t>> TableIndexList::GetConflictTargetColumns(const ConflictInfo &conflict_info) const {
	annotated_lock_guard lock(index_entries_lock);
	vector<unordered_set<column_t>> result;
	for (const auto &entry : index_entries) {
		auto index_info = entry->GetStorageInfo();
		if (!index_info.is_unique || index_info.check_mode == ConstraintCheckMode::DEFERRED ||
		    !conflict_info.ConflictTargetMatches(index_info.is_unique, index_info.column_set)) {
			continue;
		}
		D_ASSERT(entry->GetBindState() == IndexBindState::BOUND);
		result.push_back(std::move(index_info.column_set));
	}
	return result;
}

unordered_set<column_t> TableIndexList::GetUniqueIndexColumns() const {
	annotated_lock_guard lock(index_entries_lock);
	unordered_set<column_t> result;
	for (const auto &entry : index_entries) {
		auto index_info = entry->GetStorageInfo();
		if (!index_info.is_unique) {
			continue;
		}
		result.insert(index_info.column_set.begin(), index_info.column_set.end());
	}
	return result;
}

unique_ptr<IndexStorageInfo> TableIndexList::SerializeToWAL(idx_t index_oid, const StorageVersion version) {
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		if (entry->GetIndexOid() == index_oid) {
			return make_uniq<IndexStorageInfo>(entry->SerializeToWAL(version));
		}
	}
	return nullptr;
}

void TableIndexList::MergeCheckpointDeltas(const optional_idx checkpoint_id) const {
	annotated_lock_guard lock(index_entries_lock);
	for (const auto &entry : index_entries) {
		entry->MergeCheckpointDeltas(checkpoint_id);
	}
}

void TableIndexList::InitializeIndexChunk(DataChunk &index_chunk, const vector<LogicalType> &table_types,
                                          vector<StorageIndex> &mapped_column_ids, DataTableInfo &data_table_info) {
	// table_chunk contains all table columns.
	// We only reference the index columns in the index chunk.
	auto &index_list = data_table_info.GetIndexes();
	auto indexed_columns = index_list.GetIndexedColumns();

	// Store the mapped_column_ids and index_types in sorted canonical form.
	// First sort mapped_column_ids, then populate index_types according to the sorted order.
	for (auto &col : indexed_columns) {
		mapped_column_ids.emplace_back(col);
	}
	std::sort(mapped_column_ids.begin(), mapped_column_ids.end());

	vector<LogicalType> index_types;
	for (auto &col : mapped_column_ids) {
		index_types.push_back(table_types[col.GetPrimaryIndex()]);
	}

	index_chunk.InitializeEmpty(index_types);
}

void TableIndexList::ReferenceIndexChunk(DataChunk &table_chunk, DataChunk &index_chunk,
                                         vector<StorageIndex> &mapped_column_ids) {
	for (idx_t i = 0; i < mapped_column_ids.size(); i++) {
		auto col_id = mapped_column_ids[i].GetPrimaryIndex();
		index_chunk.data[i].Reference(table_chunk.data[col_id]);
	}
}

} // namespace duckdb
