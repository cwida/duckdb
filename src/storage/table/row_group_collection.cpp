#include "duckdb/storage/table/row_group_collection.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/transaction/commit_state.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/profiler/profiling_utils.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/task_executor.hpp"
#include "duckdb/planner/constraints/bound_not_null_constraint.hpp"
#include "duckdb/storage/checkpoint/table_data_writer.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/metadata/metadata_reader.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/persistent_table_data.hpp"
#include "duckdb/storage/table/row_group_segment_tree.hpp"
#include "duckdb/storage/table/row_version_manager.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/common/storage_compatibility.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

static bool CanRebuildExistingIndexesAfterVacuum(DataTableInfo &info, AttachedDatabase &attached, idx_t total_rows) {
	auto vacuum_rebuild_threshold = attached.GetVacuumRebuildIndexThreshold();
	if (vacuum_rebuild_threshold == 0 || total_rows > vacuum_rebuild_threshold) {
		return false;
	}
	// Rebuild only applies when there are indexes to rebuild: indexless tables use the NO_INDEXES strategy.
	auto &indexes = info.GetIndexes();
	return !indexes.Empty() && indexes.AllIndexesBoundOfType(ART::TYPE_NAME);
}

//! Returns true if checkpoint vacuum can incrementally remap every index: rowid gaps are persistable and
//! every index is a bound ART without legacy-encoded geometry keys. If set, remap_indexes is filled with the indexes.
static bool CanVacuumRemap(DataTableInfo &table_info, AttachedDatabase &attached,
                           optional_ptr<vector<shared_ptr<IndexEntry>>> remap_indexes) {
	if (remap_indexes) {
		remap_indexes->clear();
	}
	if (!StorageCompatibility::FromDatabase(attached).CanPersistRowIdGaps()) {
		return false;
	}
	bool any_index = false;
	for (auto entry : table_info.GetIndexes().IndexEntries()) {
		any_index = true;
		if (entry->GetBindState() != IndexBindState::BOUND || entry->GetIndexType() != ART::TYPE_NAME) {
			if (remap_indexes) {
				remap_indexes->clear();
			}
			return false;
		}
		const auto art = entry->GetReadHandle<ART>();
		// Remap regenerates keys from column values, which cannot reproduce legacy-encoded geometry keys.
		if (art->HasLegacyGeometryKeys()) {
			if (remap_indexes) {
				remap_indexes->clear();
			}
			return false;
		}
		if (remap_indexes) {
			remap_indexes->push_back(entry);
		}
	}
	return any_index;
}

VacuumIndexStrategy
RowGroupCollection::GetVacuumIndexStrategy(AttachedDatabase &attached,
                                           optional_ptr<vector<shared_ptr<IndexEntry>>> remap_indexes) const {
	if (CanVacuumRemap(*info, attached, remap_indexes)) {
		return VacuumIndexStrategy::REMAP;
	}
	if (CanRebuildExistingIndexesAfterVacuum(*info, attached, GetTotalRows())) {
		return VacuumIndexStrategy::REBUILD;
	}
	if (info->GetIndexes().Empty()) {
		return VacuumIndexStrategy::NO_INDEXES;
	}
	return VacuumIndexStrategy::KEEP_ROW_IDS;
}

//===--------------------------------------------------------------------===//
// Row Group Segment Tree
//===--------------------------------------------------------------------===//
RowGroupSegmentTree::RowGroupSegmentTree(RowGroupCollection &collection, idx_t base_row_id)
    : SegmentTree<RowGroup, true>(base_row_id), collection(collection), current_row_group(0), max_row_group(0) {
}
RowGroupSegmentTree::~RowGroupSegmentTree() {
}

void RowGroupSegmentTree::Initialize(PersistentTableData &data, optional_ptr<vector<MetaBlockPointer>> read_pointers) {
	D_ASSERT(data.row_group_count > 0);
	current_row_group = 0;
	max_row_group = data.row_group_count;
	finished_loading = false;
	reader = make_uniq<MetadataReader>(collection.GetMetadataManager(), data.block_pointer, read_pointers);
	root_pointer = data.block_pointer;
}

optional<LoadedSegment<RowGroup>> RowGroupSegmentTree::LoadSegment() const {
	if (current_row_group >= max_row_group) {
		reader.reset();
		finished_loading = true;
		return nullopt;
	}
	BinaryDeserializer deserializer(*reader);
	deserializer.Begin();
	auto row_group_pointer = RowGroup::Deserialize(deserializer);
	deserializer.End();
	current_row_group++;
	auto row_start = row_group_pointer.row_start;
	return LoadedSegment(make_shared_ptr<RowGroup>(collection, std::move(row_group_pointer)), row_start);
}

//===--------------------------------------------------------------------===//
// Row Group Collection
//===--------------------------------------------------------------------===//
RowGroupCollection::RowGroupCollection(shared_ptr<DataTableInfo> info_p, TableIOManager &io_manager,
                                       vector<LogicalType> types_p, idx_t row_start, idx_t total_rows)
    : RowGroupCollection(std::move(info_p), io_manager.GetBlockManagerForRowData(), std::move(types_p), row_start,
                         total_rows, io_manager.GetRowGroupSize()) {
}

RowGroupCollection::RowGroupCollection(shared_ptr<DataTableInfo> info_p, BlockManager &block_manager,
                                       vector<LogicalType> types_p, idx_t row_start, idx_t total_rows_p,
                                       idx_t row_group_size_p)
    : block_manager(block_manager), row_group_size(row_group_size_p), total_rows(total_rows_p),
      next_row_id(total_rows_p), info(std::move(info_p)), types(std::move(types_p)),
      owned_row_groups(make_shared_ptr<RowGroupSegmentTree>(*this, row_start)), allocation_size(0),
      row_group_append_mode(RowGroupAppendMode::APPEND_TO_EXISTING) {
	// If the table contains shredded types (variant / geometry) then we can't append to an existing row group
	for (auto &type : types) {
		if (TypeVisitor::Contains(type, LogicalTypeId::VARIANT) ||
		    TypeVisitor::Contains(type, LogicalTypeId::GEOMETRY) ||
		    TypeVisitor::Contains(type, LogicalTypeId::GEOGRAPHY)) {
			row_group_append_mode = RowGroupAppendMode::REQUIRE_NEW;
			can_append_to_checkpointed_row_group = false;
			break;
		}
	}
}

idx_t RowGroupCollection::GetTotalRows() const {
	return total_rows.load();
}

idx_t RowGroupCollection::GetNextRowId() const {
	return next_row_id.load();
}

idx_t RowGroupCollection::GetRowGroupCount() const {
	auto row_groups = GetRowGroups();
	return row_groups->GetSegmentCount();
}

const vector<LogicalType> &RowGroupCollection::GetTypes() const {
	return types;
}

Allocator &RowGroupCollection::GetAllocator() const {
	return Allocator::Get(info->GetDB());
}

AttachedDatabase &RowGroupCollection::GetAttached() const {
	return GetTableInfo().GetDB();
}

DatabaseInstance &RowGroupCollection::GetDatabase() const {
	return GetAttached().GetDatabase();
}

MetadataManager &RowGroupCollection::GetMetadataManager() {
	return GetBlockManager().GetMetadataManager();
}

shared_ptr<RowGroupSegmentTree> RowGroupCollection::GetRowGroups() const {
	lock_guard<mutex> guard(row_group_pointer_lock);
	return owned_row_groups;
}

void RowGroupCollection::SetRowGroups(shared_ptr<RowGroupSegmentTree> new_row_groups) {
	lock_guard<mutex> guard(row_group_pointer_lock);
	owned_row_groups = std::move(new_row_groups);
}

//===--------------------------------------------------------------------===//
// Initialize
//===--------------------------------------------------------------------===//
void RowGroupCollection::Initialize(PersistentTableData &data) {
	D_ASSERT(owned_row_groups->GetBaseRowId() == 0);
	auto l = owned_row_groups->Lock();
	this->total_rows = data.total_rows;
	this->next_row_id = data.next_row_id;
	D_ASSERT(this->next_row_id >= this->total_rows);
	metadata_pointer = data.base_table_pointer;
	metadata_pointers = data.read_metadata_pointers;
	owned_row_groups->Initialize(data, metadata_pointers);
	stats.Initialize(types, data);
}

void RowGroupCollection::FinalizeCheckpoint(MetaBlockPointer pointer,
                                            const vector<MetaBlockPointer> &existing_pointers) {
	metadata_pointer = pointer;
	metadata_pointers = existing_pointers;
}

void RowGroupCollection::Initialize(PersistentCollectionData &data) {
	stats.InitializeEmpty(types);
	auto l = owned_row_groups->Lock();
	for (auto &row_group_data : data.row_group_data) {
		D_ASSERT(row_group_data.start == owned_row_groups->GetBaseRowId() + total_rows.load());
		auto row_group = make_uniq<RowGroup>(*this, row_group_data);
		row_group->MergeIntoStatistics(stats);
		total_rows += row_group->count;
		owned_row_groups->AppendSegment(l, std::move(row_group), row_group_data.start);
	}
	next_row_id = total_rows.load();
}

void RowGroupCollection::SetRowGroupAppendMode(RowGroupAppendMode mode) {
	if (mode == RowGroupAppendMode::SUGGEST_NEW && !can_append_to_checkpointed_row_group) {
		// if we cannot append to existing (checkpointed) row groups we need to promote SUGGEST_NEW to REQUIRE_NEW
		mode = RowGroupAppendMode::REQUIRE_NEW;
	}
	if (mode > row_group_append_mode) {
		// We never downgrade the mode, i.e. if REQUIRE_NEW was already set then we do not set it back to SUGGEST_NEW
		row_group_append_mode = mode;
	}
}

void RowGroupCollection::InitializeEmpty() {
	stats.InitializeEmpty(types);
}

ColumnDataType GetColumnDataType(idx_t row_start) {
	if (row_start == UnsafeNumericCast<idx_t>(MAX_ROW_ID)) {
		return ColumnDataType::INITIAL_TRANSACTION_LOCAL;
	}
	if (row_start > UnsafeNumericCast<idx_t>(MAX_ROW_ID)) {
		return ColumnDataType::TRANSACTION_LOCAL;
	}
	return ColumnDataType::MAIN_TABLE;
}

void RowGroupCollection::AppendRowGroup(SegmentLock &l, idx_t start_row) {
	auto new_row_group = make_uniq<RowGroup>(*this, 0U);
	new_row_group->InitializeEmpty(types, GetColumnDataType(start_row));
	owned_row_groups->AppendSegment(l, std::move(new_row_group), start_row);
	row_group_append_mode = RowGroupAppendMode::APPEND_TO_EXISTING;
}

optional_ptr<RowGroup> RowGroupCollection::GetRowGroup(int64_t index) {
	auto result = owned_row_groups->GetSegmentByIndex(index);
	if (!result) {
		return nullptr;
	}
	return result->GetNode();
}

idx_t RowGroupCollection::GetSegmentCount() {
	auto row_groups = GetRowGroups();
	return row_groups->GetSegmentCount();
}

void RowGroupCollection::SetRowGroup(int64_t index, shared_ptr<RowGroup> new_row_group) {
	auto result = owned_row_groups->GetSegmentByIndex(index);
	if (!result) {
		throw InternalException("RowGroupCollection::SetRowGroup - Segment is out of range");
	}
	result->SetNode(std::move(new_row_group));
}

void RowGroupCollection::Verify() {
#ifdef DEBUG
	idx_t current_total_rows = 0;
	auto row_groups = GetRowGroups();
	row_groups->Verify(SegmentTreeVerifyMode::NON_OVERLAPPING);
	idx_t current_rowid_end = row_groups->GetBaseRowId();
	for (auto &entry : row_groups->SegmentNodes()) {
		auto &row_group = entry.GetNode();
		row_group.Verify();
		D_ASSERT(&row_group.GetCollection() == this);
		D_ASSERT(entry.GetRowStart() >= current_rowid_end);
		current_total_rows += row_group.count;
		current_rowid_end = entry.GetRowStart() + row_group.count;
	}
	D_ASSERT(current_total_rows == total_rows.load());
	D_ASSERT(row_groups->GetBaseRowId() + next_row_id.load() == current_rowid_end);
#endif
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
void RowGroupCollection::InitializeScan(const QueryContext &context, CollectionScanState &state,
                                        const vector<StorageIndex> &column_ids,
                                        optional_ptr<TableFilterSet> table_filters) {
	state.row_groups = GetRowGroups();
	auto row_group = state.GetRootSegment();
	state.max_row = state.row_groups->GetBaseRowId() + next_row_id.load();
	state.Initialize(context, GetTypes());
	while (row_group && !row_group->GetNode().InitializeScan(state, *row_group)) {
		row_group = state.GetNextRowGroup(*row_group);
	}
}

void RowGroupCollection::InitializeCreateIndexScan(CreateIndexScanState &state) {
	state.row_groups = GetRowGroups();
	state.segment_lock = state.row_groups->Lock();
}

void RowGroupCollection::InitializeScanWithOffset(const QueryContext &context, CollectionScanState &state,
                                                  const vector<StorageIndex> &column_ids, idx_t start_row,
                                                  idx_t end_row) {
	state.row_groups = GetRowGroups();
	auto row_group = state.row_groups->GetSegment(start_row);
	D_ASSERT(row_group);
	state.max_row = end_row;
	state.Initialize(context, GetTypes());
	idx_t start_vector = (start_row - row_group->GetRowStart()) / STANDARD_VECTOR_SIZE;
	if (!row_group->GetNode().InitializeScanWithOffset(state, *row_group, start_vector)) {
		throw InternalException("Failed to initialize row group scan with offset");
	}
}

bool RowGroupCollection::InitializeScanInRowGroup(ClientContext &context, CollectionScanState &state,
                                                  RowGroupCollection &collection, SegmentNode<RowGroup> &row_group,
                                                  idx_t vector_index, idx_t max_row, bool initialize_columns) {
	state.max_row = max_row;
	if (state.column_scans.empty()) {
		// initialize the scan state
		state.Initialize(context, collection.GetTypes());
	}
	return row_group.GetNode().InitializeScanWithOffset(state, row_group, vector_index, initialize_columns);
}

void RowGroupCollection::InitializeParallelScan(ParallelCollectionScanState &state) {
	state.collection = this;
	state.row_groups = GetRowGroups();
	state.AssignRowGroup(state.GetRootSegment(*state.row_groups));
	state.vector_index = 0;
	state.max_row = state.row_groups->GetBaseRowId() + next_row_id.load();
	state.batch_index = 0;
	state.skipped_rows = 0;
	if (state.reorderer) {
		// row groups pruned by the reorderer are never scanned
		idx_t total_rows = 0;
		for (auto &row_group : state.row_groups->SegmentNodes()) {
			total_rows += row_group.GetNode().count;
		}
		auto scan_rows = state.reorderer->ScanRowCount();
		state.skipped_rows = total_rows > scan_rows ? total_rows - scan_rows : 0;
	}
}

optional_idx RowGroupCollection::NextParallelScan(ClientContext &context, ParallelCollectionScanState &state,
                                                  CollectionScanState &scan_state, bool initialize_columns) {
	AssignSharedPointer(scan_state.row_groups, state.row_groups);
	while (true) {
		idx_t vector_index;
		idx_t max_row;
		idx_t assignment_rows;
		optional_ptr<RowGroupCollection> collection;
		optional_ptr<SegmentNode<RowGroup>> row_group;
		{
			// select the next row group to scan from the parallel state
			lock_guard<mutex> l(state.lock);
			if (!state.current_row_group) {
				// no more data left to scan
				break;
			}
			auto &current_row_group = state.current_row_group->GetNode();
			if (current_row_group.count == 0) {
				break;
			}
			auto row_start = state.current_row_group->GetRowStart();
			collection = state.collection;
			row_group = state.current_row_group;
			if (ClientConfig::GetConfig(context).verify_parallelism) {
				vector_index = state.vector_index;
				max_row = row_start + MinValue<idx_t>(current_row_group.count,
				                                      STANDARD_VECTOR_SIZE * state.vector_index + STANDARD_VECTOR_SIZE);
				D_ASSERT(vector_index * STANDARD_VECTOR_SIZE < current_row_group.count);
				state.vector_index++;
				if (state.vector_index * STANDARD_VECTOR_SIZE >= current_row_group.count) {
					state.AssignRowGroup(state.GetNextRowGroup(*state.row_groups, *row_group).get());
					state.vector_index = 0;
				}
			} else {
				vector_index = 0;
				max_row = row_start + current_row_group.count;
				state.AssignRowGroup(state.GetNextRowGroup(*state.row_groups, *row_group).get());
			}
			max_row = MinValue<idx_t>(max_row, state.max_row);
			const idx_t assignment_start = row_start + vector_index * STANDARD_VECTOR_SIZE;
			assignment_rows = max_row > assignment_start ? max_row - assignment_start : 0;
			scan_state.batch_index = ++state.batch_index;
			if (!state.row_number_base.IsValid() && scan_state.row_number_base.IsValid()) {
				state.row_number_base = scan_state.row_number_base.GetIndex();
			}
			if (state.row_number_base.IsValid()) {
				// Reserve numbers only for visible rows in this assignment, which may be part of a row group.
				scan_state.row_number_base = state.row_number_base.GetIndex();
				auto &tx = DuckTransaction::Get(context, GetAttached());
				state.row_number_base = state.row_number_base.GetIndex() +
				                        current_row_group.GetVisibleRowCount(tx, vector_index, assignment_rows);
			}
		}
		D_ASSERT(collection);
		D_ASSERT(row_group);

		// initialize the scan for this row group
		bool need_to_scan = InitializeScanInRowGroup(context, scan_state, *collection, *row_group, vector_index,
		                                             max_row, initialize_columns);
		if (!need_to_scan) {
			// skip this row group
			state.skipped_rows.fetch_add(assignment_rows, std::memory_order_relaxed);
			continue;
		}
		return assignment_rows;
	}
	lock_guard<mutex> l(state.lock);
	scan_state.batch_index = state.batch_index;
	return optional_idx();
}

//===--------------------------------------------------------------------===//
// Iterator
//===--------------------------------------------------------------------===//
RowGroupIterationHelper::RowGroupIterationHelper(RowGroupCollection &collection, DuckTransaction &transaction,
                                                 vector<StorageIndex> column_ids_p)
    : collection(collection), transaction(transaction), column_ids(std::move(column_ids_p)) {
}

RowGroupIterationHelper::RowGroupIterator RowGroupIterationHelper::begin() { // NOLINT: match stl API
	return RowGroupIterator(collection, &transaction, column_ids);
}
RowGroupIterationHelper::RowGroupIterator RowGroupIterationHelper::end() { // NOLINT: match stl API
	return RowGroupIterator(nullptr, nullptr, column_ids);
}

RowGroupIterationHelper::RowGroupIterator::RowGroupIterator(optional_ptr<RowGroupCollection> collection,
                                                            optional_ptr<DuckTransaction> transaction,
                                                            const vector<StorageIndex> &column_ids)
    : collection(collection), transaction(transaction) {
	if (collection) {
		vector<LogicalType> scan_types;
		auto &types = collection->GetTypes();
		for (idx_t i = 0; i < column_ids.size(); i++) {
			scan_types.push_back(types[column_ids[i].GetPrimaryIndex()]);
		}
		chunk = make_uniq<DataChunk>();
		chunk->Initialize(collection->GetAllocator(), scan_types);

		// initialize the scan
		state = make_uniq<TableScanState>();
		state->Initialize(column_ids, nullptr);
		collection->InitializeScan(QueryContext(), state->local_state, column_ids, nullptr);
		// scan the first chunk
		this->operator++();
	}
}
RowGroupIterationHelper::RowGroupIterator::~RowGroupIterator() {
}

RowGroupIterationHelper::RowGroupIterator::RowGroupIterator(RowGroupIterator &&other) noexcept {
	std::swap(collection, other.collection);
	std::swap(transaction, other.transaction);
	std::swap(chunk, other.chunk);
	std::swap(state, other.state);
}

RowGroupIterationHelper::RowGroupIterator &RowGroupIterationHelper::RowGroupIterator::operator++() {
	// scan the next chunk
	chunk->Reset();
	state->local_state.Scan(*transaction, *chunk);
	if (chunk->size() == 0) {
		// done
		collection = nullptr;
		transaction = nullptr;
		chunk.reset();
		state.reset();
	}
	return *this;
}

bool RowGroupIterationHelper::RowGroupIterator::operator!=(const RowGroupIterator &other) const {
	return collection != other.collection || transaction != other.transaction;
}

DataChunk &RowGroupIterationHelper::RowGroupIterator::operator*() const {
	return *chunk;
}

RowGroupIterationHelper RowGroupCollection::Chunks(DuckTransaction &transaction) {
	vector<StorageIndex> column_ids;
	column_ids.reserve(types.size());
	for (idx_t i = 0; i < types.size(); i++) {
		column_ids.emplace_back(i);
	}
	return Chunks(transaction, column_ids);
}

RowGroupIterationHelper RowGroupCollection::Chunks(DuckTransaction &transaction,
                                                   const vector<StorageIndex> &column_ids) {
	return RowGroupIterationHelper(*this, transaction, column_ids);
}

//===--------------------------------------------------------------------===//
// Fetch
//===--------------------------------------------------------------------===//
void RowGroupCollection::Fetch(TransactionData transaction, DataChunk &result, const vector<StorageIndex> &column_ids,
                               const Vector &row_identifiers, idx_t fetch_count, ColumnFetchState &state) {
	if (fetch_count == 0) {
		result.SetChildCardinality(0);
		return;
	}
	ALWAYS_ASSERT(fetch_count <= STANDARD_VECTOR_SIZE);

	auto row_ids = FlatVector::GetData<row_t>(row_identifiers);
	auto row_groups = GetRowGroups();
	idx_t count = 0;

	// Stack-allocated scratch buffers reused across runs/iterations within this call.
	//
	// Row positions in the row group.
	idx_t offsets[STANDARD_VECTOR_SIZE];
	// Visible row positions in the row group.
	sel_t visible_sel_buffer[STANDARD_VECTOR_SIZE];
	// Filter selection vector.
	SelectionVector filter_sel(visible_sel_buffer, STANDARD_VECTOR_SIZE);

	idx_t pos = 0;
	while (pos < fetch_count) {
		// 1. resolve the row group containing row_ids[pos]
		optional_ptr<SegmentNode<RowGroup>> row_group;
		{
			idx_t segment_index;
			auto l = row_groups->Lock();
			if (!row_groups->TryGetSegmentIndex(l, NumericCast<idx_t>(row_ids[pos]), segment_index)) {
				// row not yet visible, skip
				pos++;
				continue;
			}
			row_group = row_groups->GetSegmentByIndex(l, UnsafeNumericCast<int64_t>(segment_index));
		}
		auto &current_row_group = row_group->GetNode();
		const idx_t row_start = row_group->GetRowStart();
		const idx_t row_end = row_start + current_row_group.count;

		// 2. extend the run while consecutive row-ids stay in [row_start, row_end)
		const idx_t run_start = pos;
		offsets[0] = NumericCast<idx_t>(row_ids[pos]) - row_start;
		pos++;
		while (pos < fetch_count) {
			const idx_t rid = NumericCast<idx_t>(row_ids[pos]);
			if (rid < row_start || rid >= row_end) {
				break;
			}
			offsets[pos - run_start] = rid - row_start;
			pos++;
		}
		const idx_t run_count = pos - run_start;

		// 3. bulk visibility check for the whole run.
		idx_t visible_count = 0;
		const_reference<SelectionVector> sel_for_fetch(*FlatVector::IncrementalSelectionVector());
		if (state.fetch_type == FetchType::FORCE_FETCH) {
			visible_count = run_count;
		} else {
			visible_count = current_row_group.Fetch(transaction, offsets, run_count, filter_sel);
			if (visible_count != run_count) {
				sel_for_fetch = filter_sel;
			}
		}

		if (visible_count == 0) {
			continue;
		}

		// 4. bulk per-column fetch
		state.row_group = row_group;
		current_row_group.FetchRows(transaction, state, column_ids, offsets, sel_for_fetch.get(), visible_count, result,
		                            count);
		count += visible_count;
	}
	result.SetChildCardinality(count);
}

bool RowGroupCollection::CanFetch(TransactionData transaction, const row_t row_id) {
	auto row_groups = GetRowGroups();
	optional_ptr<SegmentNode<RowGroup>> row_group;
	{
		idx_t segment_index;
		auto l = row_groups->Lock();
		if (!row_groups->TryGetSegmentIndex(l, UnsafeNumericCast<idx_t>(row_id), segment_index)) {
			return false;
		}
		row_group = row_groups->GetSegmentByIndex(l, UnsafeNumericCast<int64_t>(segment_index));
	}
	auto &current_row_group = row_group->GetNode();
	auto offset_in_row_group = UnsafeNumericCast<idx_t>(row_id) - row_group->GetRowStart();
	SelectionVector visible_sel(1);
	return current_row_group.Fetch(transaction, &offset_in_row_group, /*count=*/1, visible_sel) == 1;
}

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//
TableAppendState::TableAppendState()
    : row_group_append_state(*this), total_append_count(0), start_row_group(nullptr),
      transaction(TransactionData::Unversioned()), hashes(LogicalType::HASH) {
}

TableAppendState::~TableAppendState() {
}

bool RowGroupCollection::IsEmpty() const {
	auto row_groups = GetRowGroups();
	auto l = row_groups->Lock();
	return row_groups->IsEmpty(l);
}

void RowGroupCollection::InitializeAppend(TransactionData transaction, TableAppendState &state) {
	auto next_row_id = this->next_row_id.load();
	D_ASSERT(next_row_id >= total_rows.load());
	state.row_start = UnsafeNumericCast<row_t>(next_row_id);
	state.current_row = state.row_start;
	state.total_append_count = 0;

	// start writing to the row_groups
	state.row_groups = GetRowGroups();
	auto l = state.row_groups->Lock();
	// We need a new row group if there are none yet or the append mode forces us to create a new row group
	bool needs_new_row_group = state.row_groups->IsEmpty(l) || row_group_append_mode == RowGroupAppendMode::REQUIRE_NEW;
	// Otherwise we evaluate the row_group_append_mode
	if (!needs_new_row_group) {
		auto last_row_group = state.row_groups->GetLastSegment(l);
		D_ASSERT(last_row_group->GetRowEnd() == state.row_groups->GetBaseRowId() + next_row_id);
		if (info->GetIndexes().Empty() || GetVacuumIndexStrategy(GetAttached()) != VacuumIndexStrategy::KEEP_ROW_IDS) {
			// Honor SUGGEST_NEW if vacuum can compact the table later, either because there are no indexes or because
			// the existing indexes can be rebuilt or remapped after vacuuming.
			needs_new_row_group = row_group_append_mode == RowGroupAppendMode::SUGGEST_NEW;
		} else {
			// If the table has indexes that vacuum cannot rebuild, ignore row_group_append_mode and try to append,
			// unless the last row group is full already.
			needs_new_row_group = row_group_size < last_row_group->GetNode().count;
		}
	}
	if (needs_new_row_group) {
		AppendRowGroup(l, state.row_groups->GetBaseRowId() + next_row_id);
	}
	state.start_row_group = state.row_groups->GetLastSegment(l);
	D_ASSERT(state.row_groups->GetBaseRowId() + next_row_id ==
	         state.start_row_group->GetRowStart() + state.start_row_group->GetNode().count);
	RowGroup::InitializeAppend(*state.start_row_group, state.row_group_append_state);
	state.transaction = transaction;
	state.row_group_start = state.start_row_group->GetRowStart();

	// initialize thread-local stats so we have less lock contention when updating distinct statistics
	state.stats = TableStatistics();
	state.stats.InitializeEmpty(types);
}

void RowGroupCollection::InitializeAppend(TableAppendState &state) {
	auto tdata = TransactionData::Unversioned();
	InitializeAppend(tdata, state);
}

optional_idx RowGroupCollection::Append(DataChunk &chunk, TableAppendState &state) {
	const idx_t row_group_size = GetRowGroupSize();
	D_ASSERT(chunk.ColumnCount() == types.size());
	chunk.Verify(GetDatabase());

	optional_idx flushed_row_group_idx;
	idx_t total_append_count = chunk.size();
	idx_t remaining = chunk.size();
	state.total_append_count += total_append_count;
	while (true) {
		auto &current_row_group = state.row_group_append_state.row_group->GetNode();
		// check how much we can fit into the current row_group
		idx_t append_count =
		    MinValue<idx_t>(remaining, row_group_size - state.row_group_append_state.offset_in_row_group);
		if (append_count > 0) {
			auto previous_allocation_size = current_row_group.GetAllocationSize();
			current_row_group.Append(state.row_group_append_state, chunk, append_count);
			allocation_size += current_row_group.GetAllocationSize() - previous_allocation_size;
		}
		remaining -= append_count;
		if (remaining == 0) {
			break;
		}
		// finalize the append state for the current row group
		current_row_group.FinalizeAppend(state.row_group_append_state);
		// we expect max 1 iteration of this loop (i.e. a single chunk should never overflow more than one
		// row_group)
		D_ASSERT(chunk.size() == remaining + append_count);
		// slice the input chunk
		if (remaining < chunk.size()) {
			chunk.Slice(append_count, remaining);
		}
		// append a new row_group
		flushed_row_group_idx = state.row_group_append_state.row_group->GetIndex();
		auto next_start = state.row_group_start + state.row_group_append_state.offset_in_row_group;

		auto l = state.row_groups->Lock();
		AppendRowGroup(l, next_start);
		// set up the append state for this row_group
		auto last_row_group = state.row_groups->GetLastSegment(l);
		RowGroup::InitializeNextAppend(*last_row_group, state.row_group_append_state);
		state.row_group_start = next_start;
	}
	state.current_row += row_t(total_append_count);

	auto local_stats_lock = state.stats.GetLock();

	for (idx_t col_idx = 0; col_idx < types.size(); col_idx++) {
		auto &column_stats = state.stats.GetStats(*local_stats_lock, col_idx);
		column_stats.UpdateDistinctStatistics(chunk.data[col_idx], chunk.size(), state.hashes);
	}

	return flushed_row_group_idx;
}

void RowGroupCollection::FinalizeAppend(TransactionData transaction, TableAppendState &state) {
	// first finalize the append of the final row group we appended to
	auto &last_row_group = state.row_group_append_state.row_group->GetNode();
	last_row_group.FinalizeAppend(state.row_group_append_state);

	// now push version info into all row groups
	auto remaining = state.total_append_count;
	auto row_group = state.start_row_group;
	while (remaining > 0) {
		auto &current_row_group = row_group->GetNode();
		auto append_count = MinValue<idx_t>(remaining, row_group_size - current_row_group.count);
		current_row_group.AppendVersionInfo(transaction, append_count);
		remaining -= append_count;
		row_group = state.row_groups->GetNextSegment(*row_group);
	}
	total_rows += state.total_append_count;
	next_row_id += state.total_append_count;
	D_ASSERT(next_row_id.load() >= total_rows.load());

	state.total_append_count = 0;
	state.start_row_group = nullptr;

	auto local_stats_lock = state.stats.GetLock();
	auto global_stats_lock = stats.GetLock();
	for (idx_t col_idx = 0; col_idx < types.size(); col_idx++) {
		auto &global_stats = stats.GetStats(*global_stats_lock, col_idx);
		auto &local_stats = state.stats.GetStats(*local_stats_lock, col_idx);
		global_stats.Merge(local_stats);
	}

	Verify();
}

void RowGroupCollection::CommitAppend(transaction_t commit_id, idx_t row_start, idx_t count) {
	auto row_groups = GetRowGroups();
	auto row_group = row_groups->GetSegment(row_start);
	D_ASSERT(row_group);
	idx_t current_row = row_start;
	idx_t remaining = count;
	while (true) {
		auto &current_row_group = row_group->GetNode();
		idx_t start_in_row_group = current_row - row_group->GetRowStart();
		idx_t append_count = MinValue<idx_t>(current_row_group.count - start_in_row_group, remaining);

		current_row_group.CommitAppend(commit_id, start_in_row_group, append_count);

		current_row += append_count;
		remaining -= append_count;
		if (remaining == 0) {
			break;
		}
		row_group = row_groups->GetNextSegment(*row_group);
	}
}

void RowGroupCollection::RevertAppendInternal(idx_t new_end_idx) {
	auto row_groups = GetRowGroups();

	auto l = row_groups->Lock();
	idx_t segment_count = row_groups->GetSegmentCount(l);
	if (segment_count == 0) {
		// we have no segments to revert
		return;
	}
	auto last_segment = row_groups->GetLastSegment(l);
	if (last_segment->GetRowEnd() <= new_end_idx && last_segment->GetRowStart() < new_end_idx) {
		// nothing to revert, unless a row group starts at the row (a failed append can leave it empty)
		return;
	}
	D_ASSERT(new_end_idx >= row_groups->GetBaseRowId());
	auto reverted_row_groups = make_shared_ptr<RowGroupSegmentTree>(*this, row_groups->GetBaseRowId());
	auto rlock = reverted_row_groups->Lock();
	idx_t new_total_rows = 0;
	for (auto &entry : row_groups->SegmentNodes(l)) {
		idx_t row_start = entry.GetRowStart();
		idx_t row_end = row_start + entry.GetCount();
		if (row_start >= new_end_idx) {
			// this row group does not belong to the new row group set
			break;
		}
		// this row group - at least partially - belongs to the new set
		if (row_end > new_end_idx) {
			// this is the last row group - have to revert WITHIN it
			entry.GetNode().RevertAppend(new_end_idx - row_start);
		}
		new_total_rows += entry.GetNode().count;
		reverted_row_groups->AppendSegment(rlock, entry.ReferenceNode(), row_start);
	}
	if (reverted_row_groups->GetSegmentCount(rlock) < segment_count) {
		// the reverted append created a row group, so the row group that is last again might not take appends
		SetRowGroupAppendMode(RowGroupAppendMode::SUGGEST_NEW);
	}
	SetRowGroups(std::move(reverted_row_groups));
	total_rows = new_total_rows;
	next_row_id = new_end_idx - row_groups->GetBaseRowId();
	D_ASSERT(next_row_id.load() >= total_rows.load());
}

void RowGroupCollection::RevertFailedAppend(TableAppendState &state) {
	auto start_row = NumericCast<idx_t>(state.row_start);
	auto start_row_group = state.start_row_group;
	if (start_row_group && start_row_group->GetRowStart() < start_row) {
		// the columns of the row group that the append started in hold rows past its count
		auto &row_group = start_row_group->GetNode();
		row_group.RevertAppend(row_group.count);
	}
	RevertAppendInternal(start_row);
}

void RowGroupCollection::CleanupAppend(VisibilityBound lowest_visibility_bound, idx_t start, idx_t count) {
	auto row_groups = GetRowGroups();
	auto row_group = row_groups->GetSegment(start);
	D_ASSERT(row_group);
	idx_t current_row = start;
	idx_t remaining = count;
	while (true) {
		auto &current_row_group = row_group->GetNode();
		idx_t start_in_row_group = current_row - row_group->GetRowStart();
		idx_t append_count = MinValue<idx_t>(current_row_group.count - start_in_row_group, remaining);

		current_row_group.CleanupAppend(lowest_visibility_bound, start_in_row_group, append_count);

		current_row += append_count;
		remaining -= append_count;
		if (remaining == 0) {
			break;
		}
		row_group = row_groups->GetNextSegment(*row_group);
	}
}

bool RowGroupCollection::IsPersistent() const {
	auto row_groups = GetRowGroups();
	for (auto &row_group : row_groups->Segments()) {
		if (!row_group.IsPersistent()) {
			return false;
		}
	}
	return true;
}

void RowGroupCollection::MergeStorage(RowGroupCollection &data, optional_ptr<DataTable> table,
                                      optional_ptr<StorageCommitState> commit_state) {
	D_ASSERT(data.types == types);
	auto source_row_groups = data.GetRowGroups();
	auto segments = source_row_groups->MoveSegments();
	auto row_groups = GetRowGroups();
	D_ASSERT(next_row_id.load() >= total_rows.load());
	auto target_base_row_id = row_groups->GetBaseRowId();
	auto start_index = target_base_row_id + next_row_id.load();

	// check if the row groups we are merging are optimistically written
	// if all row groups are optimistically written we keep around the block pointers
	unique_ptr<PersistentCollectionData> row_group_data;
	idx_t optimistically_written_count = 0;
	if (commit_state) {
		for (auto &entry : segments) {
			auto &row_group = entry->GetNode();
			if (!row_group.IsPersistent()) {
				if (optimistically_written_count > 0) {
#ifdef DEBUG
					throw InternalException("Partially optimistically written data at position %d (row start %d)",
					                        entry->GetIndex(), entry->GetRowStart());
#endif
				}
				break;
			}
			optimistically_written_count += row_group.count;
		}
		if (optimistically_written_count > 0) {
			row_group_data = make_uniq<PersistentCollectionData>();
		}
	}
	bool is_persistent = segments.back()->GetNode().IsPersistent();
	idx_t merged_count = 0;
#ifdef D_ASSERT_IS_ENABLED
	idx_t source_offset = 0;
#endif
	idx_t target_row_start = start_index;
	for (auto &entry : segments) {
#ifdef D_ASSERT_IS_ENABLED
		D_ASSERT(entry->GetRowStart() == source_row_groups->GetBaseRowId() + source_offset);
#endif
		auto row_group = entry->MoveNode();
		row_group->MoveToCollection(*this);
		idx_t row_group_count = row_group->count;

		if (commit_state && merged_count < optimistically_written_count) {
			// serialize the block pointers of this row group
			auto persistent_data = row_group->SerializeRowGroupInfo(target_row_start);
			persistent_data.types = types;
			row_group_data->row_group_data.push_back(std::move(persistent_data));
		}
		merged_count += row_group_count;
#ifdef D_ASSERT_IS_ENABLED
		source_offset += row_group_count;
#endif
		row_groups->AppendSegment(std::move(row_group), target_row_start);
		target_row_start += row_group_count;
	}
	if (commit_state && optimistically_written_count > 0) {
		// if we have serialized the row groups - push the serialized block pointers into the commit state
		commit_state->AddRowGroupData(*table, start_index, optimistically_written_count, std::move(row_group_data));
	}
	stats.MergeStats(data.stats);
	D_ASSERT(source_offset == data.total_rows.load());
	D_ASSERT(data.next_row_id.load() == data.total_rows.load());
	total_rows += data.total_rows.load();
	next_row_id = target_row_start - target_base_row_id;
	if (is_persistent) {
		SetRowGroupAppendMode(RowGroupAppendMode::SUGGEST_NEW);
	}
}

//===--------------------------------------------------------------------===//
// Delete
//===--------------------------------------------------------------------===//
idx_t RowGroupCollection::Delete(TransactionData transaction, DuckTableEntry &table_entry, row_t *ids, idx_t count,
                                 optional_ptr<DeletedRowIds> deleted_ids) {
	idx_t delete_count = 0;
	// delete is in the row groups
	// we need to figure out for each id to which row group it belongs
	// usually all (or many) ids belong to the same row group
	// we iterate over the ids and check for every id if it belongs to the same row group as their predecessor
	idx_t pos = 0;
	auto row_groups = GetRowGroups();
	do {
		idx_t start = pos;
		auto row_group = row_groups->GetSegment(UnsafeNumericCast<idx_t>(ids[start]));

		auto &current_row_group = row_group->GetNode();
		auto row_start = row_group->GetRowStart();
		auto row_end = row_start + current_row_group.count;
		for (pos++; pos < count; pos++) {
			D_ASSERT(ids[pos] >= 0);
			// check if this id still belongs to this row group
			if (idx_t(ids[pos]) < row_start) {
				// id is before row_group start -> it does not
				break;
			}
			if (idx_t(ids[pos]) >= row_end) {
				// id is after row group end -> it does not
				break;
			}
		}
		delete_count +=
		    current_row_group.Delete(transaction, table_entry, ids + start, pos - start, row_start, deleted_ids, start);
	} while (pos < count);

	return delete_count;
}

//===--------------------------------------------------------------------===//
// Update
//===--------------------------------------------------------------------===//
optional_ptr<SegmentNode<RowGroup>> RowGroupCollection::NextUpdateRowGroup(RowGroupSegmentTree &row_groups, row_t *ids,
                                                                           idx_t &pos, idx_t count) const {
	auto row_group = row_groups.GetSegment(UnsafeNumericCast<idx_t>(ids[pos]));

	auto &current_row_group = row_group->GetNode();
	auto row_start = row_group->GetRowStart();
	row_t base_id = UnsafeNumericCast<row_t>(
	    row_start + ((UnsafeNumericCast<idx_t>(ids[pos]) - row_start) / STANDARD_VECTOR_SIZE * STANDARD_VECTOR_SIZE));
	auto max_id =
	    MinValue<row_t>(base_id + STANDARD_VECTOR_SIZE, UnsafeNumericCast<row_t>(row_start + current_row_group.count));
	for (pos++; pos < count; pos++) {
		D_ASSERT(ids[pos] >= 0);
		// check if this id still belongs to this vector in this row group
		if (ids[pos] < base_id) {
			// id is before vector start -> it does not
			break;
		}
		if (ids[pos] >= max_id) {
			// id is after the maximum id in this vector -> it does not
			break;
		}
	}
	return row_group;
}

void RowGroupCollection::Update(TransactionData transaction, DuckTableEntry &table_entry, row_t *ids,
                                const vector<PhysicalIndex> &column_ids, DataChunk &updates) {
	D_ASSERT(updates.size() >= 1);
	idx_t pos = 0;
	auto row_groups = GetRowGroups();
	do {
		idx_t start = pos;
		auto row_group = NextUpdateRowGroup(*row_groups, ids, pos, updates.size());

		auto &current_row_group = row_group->GetNode();
		current_row_group.Update(transaction, table_entry, updates, ids, start, pos - start, column_ids,
		                         row_group->GetRowStart());

		auto l = stats.GetLock();
		for (idx_t i = 0; i < column_ids.size(); i++) {
			auto column_id = column_ids[i];
			// Use EXPAND_BOUNDS here: the row group stats include original data already counted in collection stats,
			// so additive stats (like total_string_length) cannot be maintained correctly here. EXPAND_BOUNDS
			// correctly expands min/max bounds while invalidating total_string_length until the next checkpoint.
			stats.MergeStats(*l, column_id.index, *current_row_group.GetStatistics(column_id.index),
			                 StatsMergeType::EXPAND_BOUNDS);
		}
	} while (pos < updates.size());
}

void RowGroupCollection::RemoveFromIndexes(const QueryContext &context, TableIndexList &indexes,
                                           Vector &row_identifiers, idx_t count, IndexRemovalType removal_type,
                                           optional_idx active_checkpoint) {
	// Collect all Indexed columns on the table.
	auto indexed_column_id_set = indexes.GetIndexedColumns();

	// Sorted so that the fetched columns align with the ascending physical order used when
	// referencing them into result_chunk below.
	vector<StorageIndex> column_ids {indexed_column_id_set.begin(), indexed_column_id_set.end()};
	sort(column_ids.begin(), column_ids.end());

	vector<LogicalType> column_types;
	column_types.reserve(column_ids.size());
	for (auto &col : column_ids) {
		column_types.push_back(types[col.GetPrimaryIndex()]);
	}

	DataChunk fetch_chunk;
	fetch_chunk.Initialize(GetAllocator(), column_types);

	ColumnFetchState state;
	state.fetch_type = FetchType::FORCE_FETCH;
	TransactionData commit_transaction(MAX_TRANSACTION_ID, VisibilityBound::Before(MAX_COMMIT_ID));
	Fetch(commit_transaction, fetch_chunk, column_ids, row_identifiers, count, state);

	// Used for index value removal.
	// Contains all columns but only initializes indexed ones.
	DataChunk result_chunk;
	auto fetched_columns = vector<bool>(types.size(), false);
	result_chunk.Initialize(GetAllocator(), types, fetched_columns);
	// Now set all to-be-fetched columns.
	for (auto &col : indexed_column_id_set) {
		fetched_columns[col] = true;
	}

	// Reference the necessary columns of the fetch_chunk.
	idx_t fetch_idx = 0;
	for (idx_t j = 0; j < types.size(); j++) {
		if (fetched_columns[j]) {
			result_chunk.data[j].Reference(fetch_chunk.data[fetch_idx++]);
			continue;
		}
		result_chunk.data[j].Reference(Value(types[j]), count_t(fetch_chunk.size()));
	}

	indexes.RemoveFromIndexes(result_chunk, row_identifiers, removal_type, active_checkpoint);
}

void RowGroupCollection::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry, Vector &row_ids,
                                      const vector<column_t> &column_path, DataChunk &updates) {
	D_ASSERT(updates.size() >= 1);
	auto ids = FlatVector::GetDataMutable<row_t>(row_ids);
	idx_t pos = 0;
	auto row_groups = GetRowGroups();
	do {
		idx_t start = pos;
		auto row_group = NextUpdateRowGroup(*row_groups, ids, pos, updates.size());
		auto &current_row_group = row_group->GetNode();
		current_row_group.UpdateColumn(transaction, table_entry, updates, row_ids, start, pos - start, column_path,
		                               row_group->GetRowStart());

		auto lock = stats.GetLock();
		auto primary_column_idx = column_path[0];
		current_row_group.MergeIntoStatistics(primary_column_idx,
		                                      stats.GetStats(*lock, primary_column_idx).Statistics());
	} while (pos < updates.size());
}

//===--------------------------------------------------------------------===//
// Checkpoint State
//===--------------------------------------------------------------------===//
struct VacuumState {
	bool can_vacuum_deletes = true;
	bool can_change_row_ids = false;
	//! How vacuum handles the table's indexes when it changes rowids.
	VacuumIndexStrategy index_strategy = VacuumIndexStrategy::KEEP_ROW_IDS;
	//! The indexes to remap, populated only when index_strategy == REMAP.
	vector<shared_ptr<IndexEntry>> remap_indexes;
	idx_t row_start = 0;
	idx_t next_vacuum_idx = 0;
	vector<optional_idx> row_group_counts;
};

struct CollectionCheckpointState {
	CollectionCheckpointState(RowGroupCollection &collection, TableDataWriter &writer, TableStatistics &global_stats,
	                          RowGroupSegmentTree &row_groups, idx_t segment_count)
	    : collection(collection), writer(writer), executor(writer.CreateTaskExecutor()), global_stats(global_stats),
	      row_groups(row_groups) {
		writers.resize(segment_count);
		write_data.resize(segment_count);
		dropped_segments = make_uniq_array<bool>(segment_count);
		overridden_segments.resize(segment_count);
	}

	~CollectionCheckpointState() {
		// the tasks reference this state, so join here rather than relying on the executor's own destructor
		// doing it after the members declared below it are already gone
		executor->CancelAndDrain();
	}

	RowGroupCollection &collection;
	TableDataWriter &writer;
	unique_ptr<TaskExecutor> executor;
	vector<unique_ptr<RowGroupWriter>> writers;
	vector<RowGroupWriteData> write_data;
	//! Owned here so that no task can outlive it, the destructor above joins before anything is torn down
	VacuumState vacuum_state;
	TableStatistics &global_stats;
	RowGroupSegmentTree &row_groups;

	idx_t SegmentCount() const {
		return writers.size();
	}
	optional_ptr<SegmentNode<RowGroup>> GetSegment(idx_t index) {
		if (overridden_segments[index]) {
			return *overridden_segments[index];
		}
		if (dropped_segments[index]) {
			// segment was dropped
			return nullptr;
		}
		return row_groups.GetSegmentByIndex(NumericCast<int64_t>(index));
	}

	void DropSegment(idx_t index) {
		dropped_segments[index] = true;
	}

	bool SegmentIsDropped(idx_t index) const {
		return !overridden_segments[index] && dropped_segments[index];
	}

	void SetSegment(idx_t row_start, idx_t index, shared_ptr<RowGroup> row_group) {
		overridden_segments[index] = make_uniq<SegmentNode<RowGroup>>(row_start, std::move(row_group), index);
	}

private:
	vector<unique_ptr<SegmentNode<RowGroup>>> overridden_segments;
	unique_array<bool> dropped_segments;
};

class BaseCheckpointTask : public BaseExecutorTask {
public:
	explicit BaseCheckpointTask(CollectionCheckpointState &checkpoint_state)
	    : BaseExecutorTask(*checkpoint_state.executor), checkpoint_state(checkpoint_state) {
	}

protected:
	CollectionCheckpointState &checkpoint_state;
};

class CheckpointTask : public BaseCheckpointTask {
public:
	CheckpointTask(CollectionCheckpointState &checkpoint_state, idx_t index)
	    : BaseCheckpointTask(checkpoint_state), index(index) {
	}

	void ExecuteTask() override {
		auto entry = checkpoint_state.GetSegment(index);
		auto &row_group = entry->GetNode();
		checkpoint_state.writers[index] = checkpoint_state.writer.GetRowGroupWriter(row_group);
		checkpoint_state.write_data[index] = row_group.WriteToDisk(*checkpoint_state.writers[index]);
	}

	string TaskType() const override {
		return "CheckpointTask";
	}

private:
	idx_t index;
};

//===--------------------------------------------------------------------===//
// Vacuum
//===--------------------------------------------------------------------===//

//! Per-task remap execution: buffers the shifted rows of a vacuum merge, then applies them to each index.
//! The buffer holds the indexed key columns in the sorted canonical order of
//! TableIndexList::InitializeIndexChunk, followed by the old and new rowid.
class VacuumIndexRemapper {
public:
	VacuumIndexRemapper(const vector<shared_ptr<IndexEntry>> &entries, RowGroupCollection &collection)
	    : index_entries(entries), table_types(collection.GetTypes()) {
		DataChunk index_chunk;
		TableIndexList::InitializeIndexChunk(index_chunk, table_types, mapped_column_ids, collection.GetTableInfo());
		old_rowid_idx = mapped_column_ids.size();
		new_rowid_idx = old_rowid_idx + 1;

		auto buffer_types = index_chunk.GetTypes();
		buffer_types.push_back(LogicalType::ROW_TYPE);
		buffer_types.push_back(LogicalType::ROW_TYPE);
		buffer =
		    make_uniq<ColumnDataCollection>(BufferManager::GetBufferManager(collection.GetAttached()), buffer_types);
		buffer->InitializeAppend(buffer_append_state);
		buffer_chunk.InitializeEmpty(buffer_types);
		for (idx_t c = 0; c < table_types.size(); c++) {
			table_column_ids.emplace_back(c);
		}
		append_chunk.InitializeEmpty(table_types);
	}

	//! Initialize the merge scan chunk with the table columns plus a trailing rowid column.
	void InitializeMergeScan(DataChunk &scan_chunk, vector<StorageIndex> &column_ids) const {
		column_ids.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
		auto scan_types = table_types;
		scan_types.push_back(LogicalType::ROW_TYPE);
		scan_chunk.Initialize(Allocator::DefaultAllocator(), scan_types);
	}

	//! Buffer the chunk's shifted rows and return the table columns to append (stripping the rowid column).
	DataChunk &ProcessScanChunk(DataChunk &scan_chunk, idx_t new_rowid_start) {
		BufferShiftedRows(scan_chunk, new_rowid_start);
		append_chunk.ReferenceColumns(scan_chunk, table_column_ids);
		return append_chunk;
	}

	//! Apply the buffered remaps to every index.
	void Apply() {
		if (buffer->Count() == 0) {
			return;
		}
		for (const auto &entry : index_entries) {
			// IndexEntry invokes this scan once to delete old rowids and once to append new rowids.
			entry->RemapRowIds([&](const IndexRemapApply &apply) {
				DataChunk scan_chunk;
				buffer->InitializeScanChunk(scan_chunk);
				DataChunk table_chunk;
				table_chunk.InitializeEmpty(table_types);

				ColumnDataScanState scan_state;
				buffer->InitializeScan(scan_state);
				while (buffer->Scan(scan_state, scan_chunk)) {
					// Reference the buffered key columns into the table-shaped chunk. Non-indexed columns stay
					// unreferenced: index expressions only read the index's own columns (see ApplyBufferedReplays).
					for (idx_t col_idx = 0; col_idx < mapped_column_ids.size(); col_idx++) {
						const auto col_id = mapped_column_ids[col_idx].GetPrimaryIndex();
						table_chunk.data[col_id].Reference(scan_chunk.data[col_idx]);
					}
					apply(table_chunk, scan_chunk.data[old_rowid_idx], scan_chunk.data[new_rowid_idx]);
				}
			});
		}
	}

private:
	void BufferShiftedRows(DataChunk &scan_chunk, idx_t new_rowid_start) {
		const auto count = scan_chunk.size();
		const auto rowid_column_index = table_types.size();

		SelectionVector shifted_sel(STANDARD_VECTOR_SIZE);
		idx_t shifted_count = 0;
		for (auto entry : scan_chunk.data[rowid_column_index].Values<row_t>()) {
			auto new_rowid = UnsafeNumericCast<row_t>(new_rowid_start + entry.GetIndex());
			if (entry.GetValue() == new_rowid) {
				continue;
			}
			shifted_sel.set_index(shifted_count++, entry.GetIndex());
		}
		if (shifted_count == 0) {
			return;
		}

		Vector new_rowids(LogicalType::ROW_TYPE);
		new_rowids.Sequence(UnsafeNumericCast<int64_t>(new_rowid_start), 1, count);

		buffer_chunk.Reset();
		TableIndexList::ReferenceIndexChunk(scan_chunk, buffer_chunk, mapped_column_ids);
		buffer_chunk.data[old_rowid_idx].Reference(scan_chunk.data[rowid_column_index]);
		buffer_chunk.data[new_rowid_idx].Reference(new_rowids);
		buffer_chunk.Slice(shifted_sel, shifted_count);
		buffer->Append(buffer_append_state, buffer_chunk);
	}

private:
	//! The stable index entries selected for row ID remapping.
	vector<shared_ptr<IndexEntry>> index_entries;
	const vector<LogicalType> &table_types;
	//! Buffer slot -> table column id for the indexed key columns.
	vector<StorageIndex> mapped_column_ids;
	//! Buffer column indexes of the old and new rowid.
	idx_t old_rowid_idx;
	idx_t new_rowid_idx;
	unique_ptr<ColumnDataCollection> buffer;
	ColumnDataAppendState buffer_append_state;
	//! Buffer-layout chunk used to append shifted rows.
	DataChunk buffer_chunk;
	//! The scan chunk's table columns (excludes the trailing rowid column).
	vector<column_t> table_column_ids;
	//! Table-column view of the scan chunk handed back to the merge append.
	DataChunk append_chunk;
};

class VacuumTask : public BaseCheckpointTask {
public:
	VacuumTask(CollectionCheckpointState &checkpoint_state, VacuumState &vacuum_state, idx_t segment_idx,
	           idx_t merge_count, idx_t target_count, idx_t merge_rows)
	    : BaseCheckpointTask(checkpoint_state), vacuum_state(vacuum_state), segment_idx(segment_idx),
	      merge_count(merge_count), target_count(target_count), merge_rows(merge_rows) {
	}

	void ExecuteTask() override {
		MetricsTimer timer;
		auto context = checkpoint_state.writer.TryGetClientContext();
		if (context) {
			timer = QueryProfiler::Get(*context).StartTimer<MetricStorageTotalVacuumTime>();
		}

		auto &collection = checkpoint_state.collection;
		const idx_t row_group_size = collection.GetRowGroupSize();
		auto &types = collection.GetTypes();
		unique_ptr<VacuumIndexRemapper> remapper;
		if (vacuum_state.index_strategy == VacuumIndexStrategy::REMAP && !vacuum_state.remap_indexes.empty()) {
			remapper = make_uniq<VacuumIndexRemapper>(vacuum_state.remap_indexes, collection);
		}

		// create the new set of target row groups (initially empty)
		vector<unique_ptr<SegmentNode<RowGroup>>> new_row_groups;
		vector<idx_t> append_counts;
		idx_t row_group_rows = merge_rows;
		for (idx_t target_idx = 0; target_idx < target_count; target_idx++) {
			idx_t current_row_group_rows = MinValue<idx_t>(row_group_rows, row_group_size);
			auto new_row_group = make_shared_ptr<RowGroup>(collection, current_row_group_rows);
			new_row_group->InitializeEmpty(types, ColumnDataType::MAIN_TABLE);
			new_row_groups.push_back(make_uniq<SegmentNode<RowGroup>>(0ULL, std::move(new_row_group), target_idx));
			append_counts.push_back(0);
			row_group_rows -= current_row_group_rows;
		}

		vector<StorageIndex> column_ids;
		for (idx_t c = 0; c < types.size(); c++) {
			column_ids.emplace_back(c);
		}
		DataChunk scan_chunk;
		if (remapper) {
			remapper->InitializeMergeScan(scan_chunk, column_ids);
		} else {
			scan_chunk.Initialize(Allocator::DefaultAllocator(), types);
		}

		idx_t current_append_idx = 0;

		// fill the new row group with the merged rows
		TableAppendState append_state;
		auto &initial_append_row_group = *new_row_groups[current_append_idx];
		RowGroup::InitializeAppend(initial_append_row_group, append_state.row_group_append_state);

		TableScanState scan_state;
		scan_state.Initialize(column_ids);
		scan_state.table_state.Initialize(QueryContext(), types);
		scan_state.table_state.max_row = idx_t(-1);
		idx_t merged_groups = 0;
		idx_t total_row_groups = vacuum_state.row_group_counts.size();
		optional_idx row_start;
		idx_t live_row_offset = 0;
		for (idx_t c_idx = segment_idx; merged_groups < merge_count && c_idx < total_row_groups; c_idx++) {
			if (vacuum_state.row_group_counts[c_idx] == 0) {
				continue;
			}
			merged_groups++;

			auto current_segment = checkpoint_state.GetSegment(c_idx);
			if (!row_start.IsValid()) {
				row_start = current_segment->GetRowStart();
			}
			auto &current_row_group = current_segment->GetNode();

			current_row_group.InitializeScan(scan_state.table_state, *current_segment);
			while (true) {
				scan_chunk.Reset();

				current_row_group.Scan(scan_state.table_state, scan_chunk, TableScanType::TABLE_SCAN_COMMITTED_ROWS);
				if (scan_chunk.size() == 0) {
					break;
				}
				scan_chunk.Flatten();
				const auto chunk_count = scan_chunk.size();
				// Buffer shifted rows and strip the scanned rowid column before appending to the new row groups.
				auto &append_chunk =
				    remapper ? remapper->ProcessScanChunk(scan_chunk, row_start.GetIndex() + live_row_offset)
				             : scan_chunk;
				idx_t remaining = append_chunk.size();
				while (remaining > 0) {
					auto &current_append_row_group = new_row_groups[current_append_idx]->GetNode();
					idx_t append_count = MinValue<idx_t>(remaining, row_group_size - append_counts[current_append_idx]);
					current_append_row_group.Append(append_state.row_group_append_state, append_chunk, append_count);
					append_counts[current_append_idx] += append_count;
					remaining -= append_count;
					const bool row_group_full = append_counts[current_append_idx] == row_group_size;
					const bool last_row_group = current_append_idx + 1 >= new_row_groups.size();
					if (remaining > 0 || (row_group_full && !last_row_group)) {
						// finalize the last append
						new_row_groups[current_append_idx]->GetNode().FinalizeAppend(
						    append_state.row_group_append_state);

						// move to the next row group
						current_append_idx++;
						RowGroup::InitializeNextAppend(*new_row_groups[current_append_idx],
						                               append_state.row_group_append_state);
						// slice chunk for the next append
						append_chunk.Slice(append_count, remaining);
					}
				}
				live_row_offset += chunk_count;
			}
			// drop the row group after merging
			current_row_group.CommitDrop();
			checkpoint_state.DropSegment(c_idx);
		}
		// finalize the final append
		new_row_groups[current_append_idx]->GetNode().FinalizeAppend(append_state.row_group_append_state);

		idx_t total_append_count = 0;
		for (idx_t target_idx = 0; target_idx < target_count; target_idx++) {
			auto row_group = new_row_groups[target_idx]->MoveNode();
			row_group->Verify();

			// assign the new row group to the current segment
			checkpoint_state.SetSegment(row_start.GetIndex() + total_append_count, segment_idx + target_idx,
			                            std::move(row_group));
			total_append_count += append_counts[target_idx];
		}
		if (total_append_count != merge_rows) {
			throw InternalException(
			    "Mismatch in row group count %d vs verify count %d in RowGroupCollection::Checkpoint", merge_rows,
			    total_append_count);
		}
		if (remapper) {
			remapper->Apply();
		}

		// Explicitly end the timer for the vacuum tasks here.
		timer.EndTimer();

		// merging is complete - execute checkpoint tasks of the target row groups
		for (idx_t i = 0; i < target_count; i++) {
			auto checkpoint_task = collection.GetCheckpointTask(checkpoint_state, segment_idx + i);
			if (!checkpoint_task) {
				throw InternalException("Failed to get checkpoint task for row group we just merged");
			}
			checkpoint_task->ExecuteTask();
		}
	}

	string TaskType() const override {
		return "VacuumTask";
	}

private:
	VacuumState &vacuum_state;
	idx_t segment_idx;
	idx_t merge_count;
	idx_t target_count;
	idx_t merge_rows;
};

void RowGroupCollection::InitializeVacuumState(CollectionCheckpointState &checkpoint_state, VacuumState &state,
                                               optional_idx checkpoint_row_group_count) {
	auto options = checkpoint_state.writer.GetCheckpointOptions();
	// currently we can only vacuum deletes if we are doing a full checkpoint
	state.can_vacuum_deletes = options.type != CheckpointType::CONCURRENT_CHECKPOINT;
	if (!state.can_vacuum_deletes) {
		return;
	}

	// Index state decides whether vacuum may change surviving rowids.
	auto &attached = checkpoint_state.writer.GetAttached();
	state.index_strategy = GetVacuumIndexStrategy(attached, &state.remap_indexes);

	// can_change_row_ids only answers whether index state allows changing row_ids.
	state.can_change_row_ids = state.index_strategy != VacuumIndexStrategy::KEEP_ROW_IDS;

	// obtain the set of committed row counts for each row group
	auto num_row_groups = checkpoint_state.SegmentCount();
	state.row_group_counts.reserve(num_row_groups);

	if (checkpoint_row_group_count.IsValid() && checkpoint_row_group_count.GetIndex() > num_row_groups) {
		// we have row groups that were concurrently appended to this collection
		// don't vacuum - otherwise we can move row groups which could cause committed row-ids to be moved around
		// while transactions are still processing / depending on them being stable (during e.g. commit)
		state.can_vacuum_deletes = false;
		return;
	}
	bool legacy_vacuum_with_stable_row_ids =
	    !checkpoint_state.writer.CanPersistRowIdGaps() && !state.can_change_row_ids;
	// RowIdsChanged condition: legacy storage cannot persist rowid gaps. If we are allowed to change rowids,
	// we record a gap as seen; if we later see a row group with live rows, then we know rowids have to be shifted.
	bool rowid_gap_seen = false;
	vector<idx_t> committed_counts;
	idx_t visited = 0;
	for (auto &entry : checkpoint_state.row_groups.SegmentNodes()) {
		if (visited++ >= num_row_groups) {
			// appended after the snapshot
			break;
		}
		auto &row_group = entry.GetNode();
		auto row_group_num_rows = row_group.GetCommittedRowCount();
		if (legacy_vacuum_with_stable_row_ids) {
			// In this legacy path, empty row groups in the middle must be kept because they would create a rowid gap
			// that cannot be persisted or densely rewritten. Keep the committed counts so the pass below can
			// still remove trailing deleted row groups.
			committed_counts.emplace_back(row_group_num_rows);
		}
		if (row_group_num_rows == 0) {
			if (!checkpoint_state.writer.CanPersistRowIdGaps()) {
				// Older storage versions cannot represent rowid gaps. Dropping a row group in the middle of the
				// table therefore requires dense rowid rewriting.
				if (!state.can_change_row_ids) {
					// Indexes prevent rowid changes, so keep the row group for now. If this is part of a trailing
					// deleted suffix, we can still drop it in the legacy_vacuum_with_stable_row_ids pass below.
					state.row_group_counts.emplace_back();
					continue;
				}
				// track this gap until we know whether it is followed by live rows.
				rowid_gap_seen = true;
			}
			// Drop the empty row group. Newer storage versions persist the resulting rowid gap; older storage reaches
			// this path only when rowids may be densely rewritten.
			row_group.CommitDrop();
			checkpoint_state.DropSegment(entry.GetIndex());
			state.row_group_counts.push_back(row_group_num_rows);
			continue;
		}
		if (!state.can_change_row_ids) {
			idx_t total_count = row_group.count;
			if (total_count != row_group_num_rows) {
				// We have partial deletes and cannot change rowid's, so skip it.
				state.row_group_counts.emplace_back();
				continue;
			}
			// Otherwise, the row group is fully live. We can still consider fully live row groups for merging as
			// that does not change rowids.
		}
		if (rowid_gap_seen) {
			// checkpointing will later re-number rowids densely.
			checkpoint_state.writer.SetRowIdsChanged();
		}
		state.row_group_counts.push_back(row_group_num_rows);
	}
	if (legacy_vacuum_with_stable_row_ids) {
		// With older storage and stable rowids, gap-forming drops were skipped above. Removing a suffix of fully
		// deleted row groups is still safe because no surviving rowid is shifted and no persisted rowid gap remains
		// before live data.
		auto segment_count = state.row_group_counts.size();
		for (idx_t i = segment_count; i > 0; i--) {
			auto segment_idx = i - 1;
			if (committed_counts[segment_idx] != 0) {
				// multiple rows found here - skip
				break;
			}
			auto &entry = *checkpoint_state.row_groups.GetSegmentByIndex(NumericCast<int64_t>(segment_idx));
			auto &row_group = entry.GetNode();
			D_ASSERT(entry.GetIndex() == segment_idx);
			row_group.CommitDrop();
			checkpoint_state.DropSegment(segment_idx);
		}
	}
}

bool RowGroupCollection::ScheduleVacuumTasks(CollectionCheckpointState &checkpoint_state, VacuumState &state,
                                             idx_t segment_idx, bool schedule_vacuum) {
	static constexpr const idx_t MAX_MERGE_COUNT = 3;

	if (!state.can_vacuum_deletes) {
		// we cannot vacuum deletes - cannot vacuum
		return false;
	}
	if (segment_idx < state.next_vacuum_idx) {
		// this segment is being vacuumed by a previously scheduled task
		return true;
	}
	if (state.row_group_counts[segment_idx].IsValid() && state.row_group_counts[segment_idx].GetIndex() == 0) {
		// segment was already dropped - skip
		D_ASSERT(checkpoint_state.SegmentIsDropped(segment_idx));
		return false;
	}
	if (!schedule_vacuum) {
		return false;
	}
	idx_t merge_rows;
	idx_t next_idx = 0;
	idx_t merge_count = 0;
	idx_t target_count = 0;
	bool perform_merge = false;
	// check if we can merge row groups adjacent to the current segment_idx
	// we try merging row groups into batches of 1-3 row groups
	// our goal is to reduce the amount of row groups
	// hence we target_count should be less than merge_count for a merge to be worth it
	// we greedily prefer to merge to the lowest target_count
	// i.e. we prefer to merge 2 row groups into 1, than 3 row groups into 2
	//
	// RowIdsChanged conditions for scheduled vacuum merges (over-approximation, not exact):
	// 0) rowids are allowed to change in the first place.
	// 1) the selected merge crosses an existing rowid gap.
	// 2) the selected merge includes a partially deleted row group.
	const idx_t target_row_group_size = GetRowGroupSize();
	for (target_count = 1; target_count <= MAX_MERGE_COUNT; target_count++) {
		auto total_target_size = target_count * target_row_group_size;
		merge_count = 0;
		merge_rows = 0;
		optional_idx expected_row_start;
		// Conditions 1 and 2 are evaluated for each candidate merge window. The flag is only applied if this
		// candidate is selected below.
		bool candidate_changes_row_ids = false;
		for (next_idx = segment_idx; next_idx < checkpoint_state.SegmentCount(); next_idx++) {
			if (!state.row_group_counts[next_idx].IsValid()) {
				// cannot vacuum this row group - break
				break;
			}
			auto next_row_count = state.row_group_counts[next_idx].GetIndex();
			if (next_row_count == 0) {
				if (!state.can_change_row_ids) {
					// This row group was dropped under a storage format that can persist rowid gaps, but we are not
					// allowed to change rowids. Do not schedule vacuuming across this row group.
					break;
				}
				continue;
			}
			auto next_segment = checkpoint_state.GetSegment(next_idx);
			if (!next_segment) {
				break;
			}
			auto &next_row_group = next_segment->GetNode();
			auto next_total_count = next_row_group.count.load();
			if (merge_rows + next_row_count > total_target_size) {
				// does not fit
				break;
			}
			if (!expected_row_start.IsValid()) {
				expected_row_start = next_segment->GetRowStart();
			} else if (next_segment->GetRowStart() != expected_row_start.GetIndex()) {
				if (!state.can_change_row_ids) {
					break;
				}
				// Condition 1 candidate: this candidate vacuum task will compact across a rowid gap, which will shift
				// rowids within the selected row groups.
				candidate_changes_row_ids = true;
			}
			if (next_row_count != next_total_count) {
				if (!state.can_change_row_ids) {
					break;
				}
				// Condition 2 candidate: if this merge is selected, it includes a partially deleted row group, which
				// may mean shifting row-ids (over-approximation).
				candidate_changes_row_ids = true;
			}
			expected_row_start = next_segment->GetRowStart() + next_total_count;
			// we can merge this row group together with the other row group
			merge_rows += next_row_count;
			merge_count++;
		}
		if (next_idx == checkpoint_state.SegmentCount()) {
			// in order to prevent poor performance when performing small appends, we only merge row groups at the end
			// if we can reach a "target" size of twice the current size, or the max row group size
			// this is to prevent repeated expensive checkpoints where:
			// we have a row group with 100K rows
			// merge it with a row group with 1 row, creating a row group with 100K+1 rows
			// merge it with a row group with 1 row, creating a row group with 100K+2 rows
			// etc. This leads to constant rewriting of the original 100K rows.
			idx_t minimum_target =
			    MinValue<idx_t>(state.row_group_counts[segment_idx].GetIndex() * 2, target_row_group_size) *
			    target_count;
			if (merge_rows >= STANDARD_VECTOR_SIZE && merge_rows < minimum_target) {
				// we haven't reached the minimum target - don't do this vacuum
				next_idx = segment_idx + 1;
				continue;
			}
		}
		if (target_count < merge_count) {
			// we can reduce "merge_count" row groups to "target_count"
			// perform the merge at this level
			perform_merge = true;
			if (candidate_changes_row_ids) {
				// Apply Condition 1 or 2 for the selected merge window.
				checkpoint_state.writer.SetRowIdsChanged();
			}
			break;
		}
	}
	if (!perform_merge) {
		return false;
	}
	// schedule the vacuum task
	DUCKDB_LOG(checkpoint_state.writer.GetDatabase(), CheckpointLogType, GetAttached(), *info, segment_idx, merge_count,
	           target_count, merge_rows, state.row_start);
	auto vacuum_task =
	    make_uniq<VacuumTask>(checkpoint_state, state, segment_idx, merge_count, target_count, merge_rows);
	checkpoint_state.executor->ScheduleTask(std::move(vacuum_task));
	// skip vacuuming by the row groups we have merged
	state.next_vacuum_idx = next_idx;
	state.row_start += merge_rows;
	return true;
}

//===--------------------------------------------------------------------===//
// Checkpoint
//===--------------------------------------------------------------------===//
unique_ptr<CheckpointTask> RowGroupCollection::GetCheckpointTask(CollectionCheckpointState &checkpoint_state,
                                                                 idx_t segment_idx) {
	auto row_group_count = checkpoint_state.writer.GetRowGroupCount();
	if (row_group_count.IsValid() && segment_idx >= row_group_count.GetIndex()) {
		// this row group was appended AFTER the checkpoint was started - we should not be checkpointing it
		return nullptr;
	}
	return make_uniq<CheckpointTask>(checkpoint_state, segment_idx);
}

CollectionCheckpointSnapshot RowGroupCollection::SnapshotForCheckpoint(TableDataWriter &writer) const {
	// appends resume once the append lock is released: record the number of row groups now
	CollectionCheckpointSnapshot snapshot;
	snapshot.row_groups = GetRowGroups();
	snapshot.segment_count = snapshot.row_groups->GetSegmentCount();
	snapshot.row_group_count = info->CheckpointRowGroupCount(writer.GetCheckpointOptions());
	writer.SetRowGroupCount(snapshot.row_group_count);
	return snapshot;
}

CollectionCheckpointResult RowGroupCollection::Checkpoint(TableDataWriter &writer, TableStatistics &global_stats,
                                                          const CollectionCheckpointSnapshot &snapshot) {
	auto &row_groups = snapshot.row_groups;
	// row groups past this count were appended after the checkpoint started and are taken over at install time
	idx_t checkpointed_row_group_count = snapshot.segment_count;
	if (snapshot.row_group_count.IsValid()) {
		// appends that saw this checkpoint before the snapshot already started new row groups: those stay in the WAL
		checkpointed_row_group_count =
		    MinValue<idx_t>(checkpointed_row_group_count, snapshot.row_group_count.GetIndex());
	}
	CollectionCheckpointState checkpoint_state(*this, writer, global_stats, *row_groups, checkpointed_row_group_count);

	auto &vacuum_state = checkpoint_state.vacuum_state;
	InitializeVacuumState(checkpoint_state, vacuum_state, writer.GetRowGroupCount());

	auto &transaction_manager = DuckTransactionManager::Get(GetAttached());
	auto lowest_visibility_bound = transaction_manager.LowestVisibilityBound();
	{
		// schedule tasks
		idx_t total_vacuum_tasks = 0;
		auto max_vacuum_tasks = Settings::Get<MaxVacuumTasksSetting>(writer.GetDatabase());
		for (idx_t segment_idx = 0; segment_idx < checkpointed_row_group_count; segment_idx++) {
			auto vacuum_tasks =
			    ScheduleVacuumTasks(checkpoint_state, vacuum_state, segment_idx, total_vacuum_tasks < max_vacuum_tasks);
			if (vacuum_tasks) {
				// vacuum tasks were scheduled - don't schedule a checkpoint task yet
				total_vacuum_tasks++;
				continue;
			}
			if (checkpoint_state.SegmentIsDropped(segment_idx)) {
				// row group was vacuumed/dropped - skip
				continue;
			}
			// schedule a checkpoint task for this row group
			auto entry = checkpoint_state.GetSegment(segment_idx);
			auto &row_group = entry->GetNode();
			if (!RefersToSameObject(row_group.GetCollection(), *this)) {
				throw InternalException("RowGroup Vacuum - row group collection of row group changed");
			}
			// the row group is kept as-is: try to compress its version information
			row_group.CompressVersionInfo(lowest_visibility_bound);
			if (writer.GetCheckpointOptions().type != CheckpointType::VACUUM_ONLY) {
				DUCKDB_LOG(checkpoint_state.writer.GetDatabase(), CheckpointLogType, GetAttached(), *info, segment_idx,
				           row_group, vacuum_state.row_start);
				auto checkpoint_task = GetCheckpointTask(checkpoint_state, segment_idx);
				if (checkpoint_task) {
					checkpoint_state.executor->ScheduleTask(std::move(checkpoint_task));
				}
			}
			vacuum_state.row_start += row_group.count;
		}
	}
	// all tasks have been successfully scheduled - execute tasks until we are done
	checkpoint_state.executor->WorkOnTasks();

	auto scan_sleep_ms = Settings::Get<DebugCheckpointScanSleepMsSetting>(writer.GetDatabase());
	if (scan_sleep_ms > 0) {
		ThreadUtil::SleepMs(scan_sleep_ms);
	}

	// no errors - finalize the row groups
	// if the table already exists on disk - check if all row groups have stayed the same
	if (Settings::Get<ExperimentalMetadataReuseSetting>(writer.GetDatabase()) && metadata_pointer.IsValid()) {
		bool table_has_changes = false;
		for (idx_t segment_idx = 0; segment_idx < checkpointed_row_group_count; segment_idx++) {
			if (checkpoint_state.SegmentIsDropped(segment_idx)) {
				table_has_changes = true;
				break;
			}
			auto &write_state = checkpoint_state.write_data[segment_idx];
			if (write_state.write_action != RowGroupWriteAction::REUSE_EXISTING_ROW_GROUP_METADATA) {
				table_has_changes = true;
				break;
			}
		}
		if (!table_has_changes) {
			// table is unmodified and already exists on disk
			// we can directly re-use the metadata pointer
			// mark all blocks associated with row groups as still being in-use
			auto &metadata_manager = writer.GetMetadataManager();
			for (idx_t segment_idx = 0; segment_idx < checkpointed_row_group_count; segment_idx++) {
				auto entry = checkpoint_state.GetSegment(segment_idx);
				auto &row_group = entry->GetNode();
				metadata_manager.ClearModifiedBlocks(row_group.GetColumnStartPointers());
				D_ASSERT(checkpoint_state.write_data[segment_idx].write_action ==
				         RowGroupWriteAction::REUSE_EXISTING_ROW_GROUP_METADATA);
				vector<MetaBlockPointer> extra_metadata_block_pointers = row_group.GetExtraMetadataBlockPointers();
				metadata_manager.ClearModifiedBlocks(extra_metadata_block_pointers);
				auto row_group_writer = checkpoint_state.writer.GetRowGroupWriter(row_group);
				row_group.CheckpointDeletes(*row_group_writer);
			}
			// the totals as of the snapshot: rows appended since stay in the WAL
			idx_t checkpoint_total_rows = 0;
			idx_t checkpoint_next_row_id = 0;
			auto base_row_id = row_groups->GetBaseRowId();
			for (idx_t segment_idx = 0; segment_idx < checkpointed_row_group_count; segment_idx++) {
				auto entry = checkpoint_state.GetSegment(segment_idx);
				idx_t count = entry->GetNode().count;
				checkpoint_total_rows += count;
				checkpoint_next_row_id =
				    MaxValue<idx_t>(checkpoint_next_row_id, entry->GetRowStart() + count - base_row_id);
			}
			writer.WriteUnchangedTable(metadata_pointer, metadata_pointers, checkpoint_total_rows,
			                           checkpoint_next_row_id);
			return CollectionCheckpointResult();
		}
	}

	// not all segments have stayed the same - we need to make a new segment tree with the new set of segments
	// initialize new empty stats - before locking the new segment tree, as this takes the stats lock
	global_stats.InitializeEmpty(stats);

	auto new_row_groups = make_shared_ptr<RowGroupSegmentTree>(*this, row_groups->GetBaseRowId());
	auto l = new_row_groups->Lock();

	idx_t new_total_rows = 0;
	idx_t new_next_row_id = 0;
	auto base_row_id = row_groups->GetBaseRowId();
	auto can_persist_rowid_gaps = writer.CanPersistRowIdGaps();
	unordered_set<idx_t> columns_with_incomplete_stats;
	for (idx_t segment_idx = 0; segment_idx < checkpointed_row_group_count; segment_idx++) {
		auto entry = checkpoint_state.GetSegment(segment_idx);
		if (!entry) {
			// row group was vacuumed/dropped - skip
			D_ASSERT(checkpoint_state.SegmentIsDropped(segment_idx));
			continue;
		}
		auto &existing_row_group = entry->GetNode();
		auto &row_group_writer = checkpoint_state.writers[segment_idx];
		auto source_row_start = entry->GetRowStart();
		auto row_start = source_row_start;
		if (!can_persist_rowid_gaps) {
			// Older storage versions do not serialize next_row_id, so the checkpointed row groups must have
			// contiguous numbering for rowids.
			row_start = base_row_id + new_total_rows;
			// If rowids must remain stable, dense old-storage output must not move this surviving row group.
			D_ASSERT(vacuum_state.can_change_row_ids || row_start == source_row_start);
		}
		if (!row_group_writer) {
			// row group was not checkpointed - this can happen if compressing is disabled for in-memory tables
			D_ASSERT(row_start == source_row_start);
			new_row_groups->AppendSegment(l, entry->ReferenceNode(), row_start);
			new_total_rows += existing_row_group.count;
			new_next_row_id = MaxValue<idx_t>(new_next_row_id, row_start + existing_row_group.count - base_row_id);

			auto lock = global_stats.GetLock();
			for (idx_t column_idx = 0; column_idx < existing_row_group.GetColumnCount(); column_idx++) {
				global_stats.GetStats(*lock, column_idx)
				    .Statistics()
				    .Merge(*existing_row_group.GetStatistics(column_idx));
			}
			continue;
		}
		auto &row_group_write_data = checkpoint_state.write_data[segment_idx];
		auto write_action = row_group_write_data.write_action;
		auto debug_verify_blocks = Settings::Get<DebugVerifyBlocksSetting>(GetAttached().GetDatabase()) &&
		                           checkpoint_state.writer.IsSingleFileWriter();
		vector<bool> reuse_column;
		if (debug_verify_blocks) {
			if (write_action == RowGroupWriteAction::REUSE_EXISTING_ROW_GROUP_METADATA) {
				auto existing_column_count = entry->ReferenceNode()->GetColumnCount();
				reuse_column.resize(existing_column_count, true);
			} else {
				reuse_column.resize(row_group_write_data.states.size());
				for (idx_t column_idx = 0; column_idx < row_group_write_data.states.size(); column_idx++) {
					reuse_column[column_idx] = !row_group_write_data.states[column_idx];
				}
			}
		}
		auto new_row_group = std::move(row_group_write_data.result_row_group);
		if (!new_row_group) {
			// row group was unchanged - emit previous row group
			new_row_group = entry->ReferenceNode();
		}
		auto &row_group = *new_row_group;
		RowGroupPointer pointer_copy;
		// check if we should write this row group to the persistent storage
		// don't write it if it only has uncommitted transaction-local changes made AFTER this checkpoint was started
		auto pointer =
		    row_group.Checkpoint(std::move(row_group_write_data), *row_group_writer, global_stats, row_start);
		if (debug_verify_blocks) {
			pointer_copy = pointer;
		}
		// for any columns that are not yet loaded add them to the list of columns with incomplete stats
		auto unloaded_columns = row_group_writer->GetUnloadedColumns();
		for (auto &column_idx : unloaded_columns) {
			columns_with_incomplete_stats.insert(column_idx);
		}

		writer.AddRowGroup(std::move(pointer), std::move(row_group_writer));
		new_total_rows += row_group.count;
		new_next_row_id = MaxValue<idx_t>(new_next_row_id, row_start + row_group.count - base_row_id);
		new_row_groups->AppendSegment(l, std::move(new_row_group), row_start);

		if (debug_verify_blocks) {
			if (!pointer_copy.has_metadata_blocks && !pointer_copy.has_per_column_metadata_blocks) {
				throw InternalException("Checkpointing should always remember metadata blocks");
			}
			if (SupportsPerColumnWrites() != pointer_copy.has_per_column_metadata_blocks) {
				throw InternalException(
				    "Checkpointing should always remember per-column metadata blocks when supporting it");
			}
			if (write_action == RowGroupWriteAction::PARTIALLY_REUSE_COLUMN_METADATA && !SupportsPerColumnWrites()) {
				throw InternalException("Partially reusing column metadata should only be done when supporting it");
			}
			if (write_action == RowGroupWriteAction::REUSE_EXISTING_ROW_GROUP_METADATA &&
			    pointer_copy.data_pointers != row_group.GetColumnStartPointers()) {
				throw InternalException("Column start pointers changed during full metadata reuse");
			}

			// Verify per_column_metadata_blocks matches full deserialization
			if (pointer_copy.has_per_column_metadata_blocks) {
				const auto &column_start_ptrs = row_group.GetColumnStartPointers();
				auto &col_types = row_group.GetCollection().GetTypes();
				auto &mm = row_group.GetCollection().GetMetadataManager();
				vector<idx_t> columns;
				vector<vector<idx_t>> deserialized_extras;
				deserialized_extras.reserve(column_start_ptrs.size());
				for (idx_t i = 0; i < column_start_ptrs.size(); i++) {
					columns.emplace_back(i);
					deserialized_extras.emplace_back();
					vector<MetaBlockPointer> col_read_pointers;
					MetadataReader reader(mm, column_start_ptrs[i], &col_read_pointers);
					ColumnData::Deserialize(GetBlockManager(), GetTableInfo(), i, reader, col_types[i]);
					// collect extra blocks from deserialization (excluding start block)
					for (auto &ptr : col_read_pointers) {
						if (ptr.block_pointer != column_start_ptrs[i].block_pointer) {
							deserialized_extras[i].emplace_back(ptr.block_pointer);
						}
					}
				}
				auto blocks_for_columns = pointer_copy.per_column_metadata_blocks.GetBlocksForColumns(columns);
				if (deserialized_extras != blocks_for_columns) {
					throw InternalException("per_column_metadata_blocks mismatch");
				}
			}

			// Verify blocks are cleared for partial column reuse
			for (idx_t col_idx = 0; col_idx < pointer_copy.data_pointers.size(); col_idx++) {
				if (!reuse_column[col_idx]) {
					continue;
				}
				// reused column: its start block should be cleared
				if (!block_manager.GetMetadataManager().BlockHasBeenCleared(pointer_copy.data_pointers[col_idx])) {
					throw InternalException("Partial reuse: column %llu start block was not cleared", col_idx);
				}
			}
			pointer_copy.per_column_metadata_blocks.ForEachBlock([&](idx_t col_idx, idx_t block_id) {
				if (!reuse_column[col_idx]) {
					return;
				}
				// reused column: extra blocks should be cleared
				auto block_ptr = MetaBlockPointer(block_id, 0);
				if (!block_manager.GetMetadataManager().BlockHasBeenCleared(block_ptr)) {
					throw InternalException("Partial reuse: column extra block %llu was not cleared", block_id);
				}
			});

			// Capture blocks that have been written
			vector<MetaBlockPointer> all_written_blocks = pointer_copy.data_pointers;
			if (pointer_copy.has_metadata_blocks) {
				for (auto &block : pointer_copy.extra_metadata_blocks) {
					all_written_blocks.emplace_back(block, 0);
				}
			} else {
				if (!pointer_copy.has_per_column_metadata_blocks) {
					throw InternalException("Checkpointing should always remember metadata blocks");
				}
				pointer_copy.per_column_metadata_blocks.ForEachBlock(
				    [&](idx_t, idx_t block) { all_written_blocks.emplace_back(block, 0); });
			}

			// Deserialize all columns to check if what's on disk matches our metadata
			vector<MetaBlockPointer> all_full_read_blocks;
			const auto &column_start_pointers = row_group.GetColumnStartPointers();
			auto &column_types = row_group.GetCollection().GetTypes();
			auto &metadata_manager = row_group.GetCollection().GetMetadataManager();
			for (idx_t i = 0; i < column_start_pointers.size(); i++) {
				MetadataReader reader(metadata_manager, column_start_pointers[i], &all_full_read_blocks);
				ColumnData::Deserialize(GetBlockManager(), GetTableInfo(), i, reader, column_types[i]);
			}

			// Derive sets of blocks to compare
			set<idx_t> all_written_block_ids;
			for (auto &ptr : all_written_blocks) {
				all_written_block_ids.insert(ptr.block_pointer);
			}
			set<idx_t> all_full_read_block_ids;
			for (auto &ptr : all_full_read_blocks) {
				all_full_read_block_ids.insert(ptr.block_pointer);
			}
			if (all_written_block_ids != all_full_read_block_ids) {
				std::stringstream oss;
				oss << "\nWritten: ";
				for (auto &block : all_written_blocks) {
					oss << block << ", ";
				}
				oss << "\nFull read: ";
				for (auto &block : all_full_read_blocks) {
					oss << block << ", ";
				}

				throw InternalException("Reloading blocks just written does not yield same blocks: " + oss.str());
			}

			vector<MetaBlockPointer> read_deletes_pointers;
			if (!pointer_copy.deletes_pointers.empty()) {
				auto root_delete = pointer_copy.deletes_pointers[0];
				auto vm = RowVersionManager::Deserialize(root_delete, GetBlockManager().GetMetadataManager());
				read_deletes_pointers = vm->GetStoragePointers();
			}

			set<idx_t> all_written_deletes_block_ids;
			for (auto &ptr : pointer_copy.deletes_pointers) {
				all_written_deletes_block_ids.insert(ptr.block_pointer);
				// delete ptr should be cleared (unless it's been newly written)
				if (block_manager.GetMetadataManager().BlockIsModified(ptr) &&
				    !block_manager.GetMetadataManager().BlockHasBeenCleared(ptr)) {
					throw InternalException("Delete ptr %llu was not cleared", ptr.block_pointer);
				}
			}
			set<idx_t> all_read_deletes_block_ids;
			for (auto &ptr : read_deletes_pointers) {
				all_read_deletes_block_ids.insert(ptr.block_pointer);
			}

			if (all_written_deletes_block_ids != all_read_deletes_block_ids) {
				std::stringstream oss;
				oss << "Written: ";
				for (auto &block : all_written_deletes_block_ids) {
					oss << block << ", ";
				}
				oss << "\n";
				oss << "Read: ";
				for (auto &block : all_read_deletes_block_ids) {
					oss << block << ", ";
				}
				oss << "\n";

				throw InternalException("Reloading deletes blocks just written does not yield same blocks: " +
				                        oss.str());
			}
		}
	}
	l.Release();
	if (!columns_with_incomplete_stats.empty()) {
		// for any columns that have incomplete stats we need to merge in the previous global stats to ensure the stats
		// are correct — use EXPAND_BOUNDS so additive stats (e.g. total_string_length) are invalidated rather than
		// double-counted (the collection stats include contributions from all row groups, including those already
		// merged)
		// the two statistics locks are never nested (see InstallCheckpoint)
		vector<BaseStatistics> live_column_stats;
		{
			auto stats_lock = stats.GetLock();
			for (auto &column_idx : columns_with_incomplete_stats) {
				live_column_stats.push_back(stats.GetStats(*stats_lock, column_idx).Statistics().Copy());
			}
		}
		auto lock = global_stats.GetLock();
		idx_t stats_idx = 0;
		for (auto &column_idx : columns_with_incomplete_stats) {
			global_stats.MergeStats(*lock, column_idx, live_column_stats[stats_idx++], StatsMergeType::EXPAND_BOUNDS);
		}
	}

	// flush any partial blocks BEFORE updating the row group pointer
	// flushing partial blocks updates where data lives
	// this cannot be done after other threads start scanning the row groups
	// so this HAS to happen before we call "SetRowGroups" to update the row groups
	writer.FlushPartialBlocks();
	// Rebuild indexes if the REBUILD strategy was chosen (legacy vacuum_rebuild_indexes path) and rowids changed.
	if (vacuum_state.index_strategy == VacuumIndexStrategy::REBUILD && writer.RowIdsChanged()) {
		writer.SetRebuildIndexes();
	}

	CollectionCheckpointResult result;
	result.row_groups = std::move(new_row_groups);
	result.total_rows = new_total_rows;
	result.next_row_id = new_next_row_id;
	result.checkpointed_row_group_count = checkpointed_row_group_count;
	return result;
}

void RowGroupCollection::InstallCheckpoint(CollectionCheckpointResult result, TableStatistics &checkpoint_stats) {
	if (!result.row_groups) {
		// the table was unchanged: the live statistics already describe it
		return;
	}
	auto &new_row_groups = result.row_groups;
	vector<reference<RowGroup>> appended_row_groups;
	{
		// take over the row groups appended while the checkpoint was running
		auto live_row_groups = GetRowGroups();
		// lock order as in Checkpoint: the new tree, then the one it was written from
		auto new_lock = new_row_groups->Lock();
		auto live_lock = live_row_groups->Lock();
		idx_t live_count = live_row_groups->GetSegmentCount(live_lock);
		for (idx_t segment_idx = result.checkpointed_row_group_count; segment_idx < live_count; segment_idx++) {
			auto entry = live_row_groups->GetSegmentByIndex(live_lock, UnsafeNumericCast<int64_t>(segment_idx));
			auto row_start = entry->GetRowStart();
			// appended row groups start past the checkpointed ones
			D_ASSERT(row_start >= new_row_groups->GetBaseRowId() + result.next_row_id);
			result.total_rows += entry->GetNode().count;
			new_row_groups->AppendSegment(new_lock, entry->ReferenceNode(), row_start);
			appended_row_groups.push_back(entry->GetNode());
		}
		if (live_count > result.checkpointed_row_group_count) {
			// the appends and reverts that ran meanwhile kept next_row_id up to date
			result.next_row_id = next_row_id.load();
		}
	}
	{
		// the live statistics become those of the checkpoint plus those of the row groups appended meanwhile
		// the two statistics locks are never nested: ALTER takes the live one before a tree lock
		vector<BaseStatistics> checkpoint_column_stats;
		{
			auto checkpoint_lock = checkpoint_stats.GetLock();
			for (idx_t column_idx = 0; column_idx < types.size(); column_idx++) {
				checkpoint_column_stats.push_back(
				    checkpoint_stats.GetStats(*checkpoint_lock, column_idx).Statistics().Copy());
			}
		}
		auto lock = stats.GetLock();
		for (idx_t column_idx = 0; column_idx < types.size(); column_idx++) {
			auto &column_stats = stats.GetStats(*lock, column_idx).Statistics();
			column_stats = std::move(checkpoint_column_stats[column_idx]);
			for (auto &row_group : appended_row_groups) {
				column_stats.Merge(*row_group.get().GetStatistics(column_idx));
			}
		}
	}
	total_rows = result.total_rows;
	next_row_id = result.next_row_id;
	D_ASSERT(next_row_id.load() >= total_rows.load());
	SetRowGroups(std::move(new_row_groups));
	Verify();
}

//===--------------------------------------------------------------------===//
// Destroy
//===--------------------------------------------------------------------===//

class DestroyTask : public BaseExecutorTask {
public:
	DestroyTask(TaskExecutor &executor, shared_ptr<RowGroup> row_group_p)
	    : BaseExecutorTask(executor), row_group(std::move(row_group_p)) {
	}

	void ExecuteTask() override {
		row_group.reset();
	}

private:
	shared_ptr<RowGroup> row_group;
};

void RowGroupCollection::Destroy() {
	auto l = owned_row_groups->Lock();
	auto &segments = owned_row_groups->ReferenceLoadedSegmentsMutable(l);

	TaskExecutor executor(TaskScheduler::GetScheduler(GetAttached().GetDatabase()));
	for (auto &segment : segments) {
		auto destroy_task = make_uniq<DestroyTask>(executor, segment->MoveNode());
		executor.ScheduleTask(std::move(destroy_task));
	}
	executor.WorkOnTasks();
}

//===--------------------------------------------------------------------===//
// CommitDrop
//===--------------------------------------------------------------------===//
void RowGroupCollection::CommitDropColumn(const idx_t column_index, CommitDropState &drop_state) {
	auto row_groups = GetRowGroups();
	for (auto &row_group : row_groups->Segments()) {
		row_group.CommitDropColumn(column_index, drop_state);
	}
}

void RowGroupCollection::CommitDropTable(CommitDropState &drop_state) {
	auto row_groups = GetRowGroups();
	for (auto &row_group : row_groups->Segments()) {
		row_group.CommitDrop(drop_state);
	}
}

void RowGroupCollection::CommitDropColumn(const idx_t column_index) {
	CommitDropState drop_state(&GetBlockManager());
	CommitDropColumn(column_index, drop_state);
	drop_state.FinalizeCommit();
}

void RowGroupCollection::CommitDropTable() {
	CommitDropState drop_state(&GetBlockManager());
	CommitDropTable(drop_state);
	drop_state.FinalizeCommit();
}

//===--------------------------------------------------------------------===//
// GetPartitionStats
//===--------------------------------------------------------------------===//
vector<PartitionStatistics> RowGroupCollection::GetPartitionStats(TransactionData transaction) const {
	vector<PartitionStatistics> result;
	auto row_groups = GetRowGroups();
	for (auto &entry : row_groups->SegmentNodes()) {
		result.push_back(RowGroup::GetPartitionStats(entry, transaction));
	}
	return result;
}

//===--------------------------------------------------------------------===//
// GetColumnSegmentInfo
//===--------------------------------------------------------------------===//
vector<ColumnSegmentInfo> RowGroupCollection::GetColumnSegmentInfo(const QueryContext &context,
                                                                   const ColumnSegmentInfoScanOptions &options) const {
	vector<ColumnSegmentInfo> result;
	ColumnSegmentInfoScanState state;
	state.options = options;
	InitializeColumnSegmentInfoScan(state);
	while (ScanColumnSegmentInfo(context, state, result)) {
	}
	return result;
}

void RowGroupCollection::InitializeColumnSegmentInfoScan(ColumnSegmentInfoScanState &state) const {
	// Pin a consistent snapshot of the row groups. Holding the shared_ptr keeps the
	// segment tree alive even if a concurrent operation (alter, vacuum) installs a
	// new collection. The segment-tree node lock is acquired only briefly here to
	// fetch the first node (and may lazily load it from disk).
	state.row_groups = GetRowGroups();
	state.current_row_group = state.row_groups->GetRootSegment();
}

bool RowGroupCollection::ScanColumnSegmentInfo(const QueryContext &context, ColumnSegmentInfoScanState &state,
                                               vector<ColumnSegmentInfo> &result) const {
	if (!state.current_row_group) {
		return false;
	}
	auto &node = *state.current_row_group;
	node.GetNode().GetColumnSegmentInfo(context, node.GetIndex(), result, state.options);
	// Advance to the next row group. For lazy-loading segment trees this acquires
	// the node lock briefly only while more segments still need to be loaded.
	state.current_row_group = state.row_groups->GetNextSegment(node);
	return true;
}

bool RowGroupCollection::SupportsPerColumnWrites() {
	auto version = StorageCompatibility::FromDatabase(GetAttached());
	if (version.storage_version >= StorageCompatibility::FromString("v2.0.0").storage_version) {
		return true;
	}
	if (version.storage_version >= StorageCompatibility::FromString("v1.4.0").storage_version) {
		return Settings::Get<ForceColumnMetadataReuseSetting>(GetAttached().GetDatabase());
	}
	return false;
}

//===--------------------------------------------------------------------===//
// Alter
//===--------------------------------------------------------------------===//
shared_ptr<RowGroupCollection> RowGroupCollection::AddColumn(ClientContext &context, ColumnDefinition &new_column,
                                                             ExpressionExecutor &default_executor) {
	idx_t new_column_idx = types.size();
	auto new_types = types;
	auto row_groups = GetRowGroups();
	new_types.push_back(new_column.GetType());
	auto result = make_shared_ptr<RowGroupCollection>(info, block_manager, std::move(new_types),
	                                                  row_groups->GetBaseRowId(), total_rows.load(), row_group_size);
	result->next_row_id = next_row_id.load();

	result->stats.InitializeAddColumn(stats, new_column.GetType());
	auto lock = result->stats.GetLock();
	auto &new_column_stats = result->stats.GetStats(*lock, new_column_idx);

	// fill the column with its DEFAULT value, or NULL if none is specified
	auto new_stats = make_uniq<SegmentStatistics>(new_column.GetType());
	auto result_row_groups = result->GetRowGroups();
	for (auto &node : row_groups->SegmentNodes()) {
		auto &current_row_group = node.GetNode();
		auto new_row_group = current_row_group.AddColumn(*result, new_column, default_executor, new_column_stats);
		// merge in the statistics
		new_row_group->MergeIntoStatistics(new_column_idx, new_column_stats.Statistics());

		result_row_groups->AppendSegment(std::move(new_row_group), node.GetRowStart());
	}

	return result;
}

shared_ptr<RowGroupCollection> RowGroupCollection::RemoveColumn(idx_t col_idx) {
	D_ASSERT(col_idx < types.size());
	auto new_types = types;
	auto row_groups = GetRowGroups();
	new_types.erase_at(col_idx);

	auto result = make_shared_ptr<RowGroupCollection>(info, block_manager, std::move(new_types),
	                                                  row_groups->GetBaseRowId(), total_rows.load(), row_group_size);
	result->next_row_id = next_row_id.load();
	result->stats.InitializeRemoveColumn(stats, col_idx);

	auto result_lock = result->stats.GetLock();
	result->stats.DestroyTableSample(*result_lock);

	auto result_row_groups = result->GetRowGroups();
	for (auto &node : row_groups->SegmentNodes()) {
		auto &current_row_group = node.GetNode();
		auto new_row_group = current_row_group.RemoveColumn(*result, col_idx);
		result_row_groups->AppendSegment(std::move(new_row_group), node.GetRowStart());
	}
	return result;
}

shared_ptr<RowGroupCollection> RowGroupCollection::AlterType(ClientContext &context, idx_t changed_idx,
                                                             const LogicalType &target_type,
                                                             vector<StorageIndex> bound_columns, Expression &cast_expr,
                                                             TransactionData transaction) {
	D_ASSERT(changed_idx < types.size());
	auto new_types = types;
	auto row_groups = GetRowGroups();
	new_types[changed_idx] = target_type;

	auto result = make_shared_ptr<RowGroupCollection>(info, block_manager, std::move(new_types),
	                                                  row_groups->GetBaseRowId(), total_rows.load(), row_group_size);
	result->next_row_id = next_row_id.load();
	result->stats.InitializeAlterType(stats, changed_idx, target_type);

	vector<LogicalType> scan_types;
	for (idx_t i = 0; i < bound_columns.size(); i++) {
		if (bound_columns[i].IsRowIdColumn()) {
			scan_types.emplace_back(LogicalType::ROW_TYPE);
		} else {
			scan_types.push_back(types[bound_columns[i].GetPrimaryIndex()]);
		}
	}
	DataChunk scan_chunk;
	scan_chunk.Initialize(GetAllocator(), scan_types);

	ExpressionExecutor executor(context);
	executor.AddExpression(cast_expr);

	TableScanState scan_state;
	scan_state.Initialize(bound_columns);
	scan_state.table_state.Initialize(context, GetTypes());
	scan_state.table_state.max_row = row_groups->GetBaseRowId() + next_row_id.load();

	// now alter the type of the column within all of the row_groups individually
	auto lock = result->stats.GetLock();
	auto &changed_stats = result->stats.GetStats(*lock, changed_idx);
	auto result_row_groups = result->GetRowGroups();

	for (auto &node : row_groups->SegmentNodes()) {
		auto &current_row_group = node.GetNode();
		auto new_row_group =
		    current_row_group.AlterType(*result, target_type, changed_idx, executor, scan_state.table_state, node,
		                                scan_chunk, transaction, changed_stats);
		new_row_group->MergeIntoStatistics(changed_idx, changed_stats.Statistics());
		result_row_groups->AppendSegment(std::move(new_row_group), node.GetRowStart());
	}
	return result;
}

void RowGroupCollection::VerifyNewConstraint(const QueryContext &context, DuckTransaction &transaction,
                                             DataTable &parent, const BoundConstraint &constraint) {
	if (total_rows == 0) {
		return;
	}

	// Scan the original table for NULL values.
	auto &not_null_constraint = constraint.Cast<BoundNotNullConstraint>();
	vector<LogicalType> scan_types;
	auto physical_index = not_null_constraint.index.index;
	D_ASSERT(physical_index < types.size());

	scan_types.push_back(types[physical_index]);
	DataChunk scan_chunk;
	scan_chunk.Initialize(GetAllocator(), scan_types);

	vector<StorageIndex> column_ids;
	column_ids.emplace_back(physical_index);

	CreateIndexScanState state;
	state.Initialize(column_ids, nullptr);
	InitializeScan(context, state.table_state, column_ids, nullptr);

	InitializeCreateIndexScan(state);

	auto &transaction_manager = DuckTransactionManager::Get(parent.db);
	TransactionData constraint_visibility(transaction.GetTransactionId(),
	                                      VisibilityBound::Through(transaction_manager.GetLastCommit()));
	ScanOptions scan_options(constraint_visibility);
	scan_options.insert_type = InsertedScanType::ALL_ROWS;
	scan_options.update_type = UpdateScanType::DISALLOW_UPDATES;
	while (true) {
		scan_chunk.Reset();
		state.table_state.Scan(scan_options, scan_chunk, state.segment_lock);
		if (scan_chunk.size() == 0) {
			break;
		}

		// Verify the NOT NULL constraint.
		if (VectorOperations::HasNull(scan_chunk.data[0])) {
			auto name = parent.Columns()[physical_index].GetName();
			throw ConstraintException("NOT NULL constraint failed: %s.%s", SQLIdentifier(info->GetTableName()),
			                          SQLIdentifier(name));
		}
	}
}

//===--------------------------------------------------------------------===//
// Statistics
//===---------------------------------------------------------------r-----===//

void RowGroupCollection::CopyStats(TableStatistics &other_stats) {
	stats.CopyStats(other_stats);
}

unique_ptr<BaseStatistics> RowGroupCollection::CopyStats(const StorageIndex &column_id) {
	return stats.CopyStats(column_id);
}

unique_ptr<BlockingSample> RowGroupCollection::GetSample() {
	return nullptr;
}

void RowGroupCollection::SetDistinct(column_t column_id, unique_ptr<DistinctStatistics> distinct_stats) {
	D_ASSERT(column_id != COLUMN_IDENTIFIER_ROW_ID);
	auto stats_lock = stats.GetLock();
	stats.GetStats(*stats_lock, column_id).SetDistinct(std::move(distinct_stats));
}

} // namespace duckdb
