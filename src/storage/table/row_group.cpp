#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/async_result.hpp"
#include "duckdb/transaction/commit_state.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/execution/adaptive_filter.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/checkpoint/table_data_writer.hpp"
#include "duckdb/storage/metadata/metadata_reader.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/row_version_manager.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/transaction/delete_info.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/storage/table/row_id_column_data.hpp"
#include "duckdb/storage/table/row_number_column_data.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "duckdb/main/settings.hpp"

#include "duckdb/common/storage_compatibility.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

RowGroup::RowGroup(RowGroupCollection &collection_p, idx_t count)
    : SegmentBase<RowGroup>(count), collection(collection_p), version_info(make_shared_ptr<RowGroupVersionInfo>()),
      allocation_size(0), row_id_is_loaded(false), row_number_is_loaded(false), has_changes(false) {
	Verify();
}

RowGroup::RowGroup(RowGroupCollection &collection_p, RowGroupPointer pointer)
    : SegmentBase<RowGroup>(pointer.tuple_count), collection(collection_p),
      version_info(make_shared_ptr<RowGroupVersionInfo>(std::move(pointer.deletes_pointers))), allocation_size(0),
      row_id_is_loaded(false), row_number_is_loaded(false), has_changes(false) {
	// deserialize the columns
	if (pointer.data_pointers.size() != collection_p.GetTypes().size()) {
		throw DataCorruptionException("Row group column count is unaligned with table column count. Corrupt file?");
	}
	this->column_pointers = std::move(pointer.data_pointers);
	this->columns.resize(column_pointers.size());
	this->is_loaded = unique_ptr<atomic<bool>[]>(new atomic<bool>[columns.size()]);
	for (idx_t c = 0; c < columns.size(); c++) {
		this->is_loaded[c] = false;
	}
	this->has_metadata_blocks = pointer.has_metadata_blocks;
	this->extra_metadata_blocks = std::move(pointer.extra_metadata_blocks);
	this->has_per_column_metadata_blocks = pointer.has_per_column_metadata_blocks;
	this->per_column_metadata_blocks = std::move(pointer.per_column_metadata_blocks);

	Verify();
}

RowGroup::RowGroup(RowGroupCollection &collection_p, PersistentRowGroupData &data)
    : SegmentBase<RowGroup>(data.count), collection(collection_p), version_info(make_shared_ptr<RowGroupVersionInfo>()),
      allocation_size(0), row_id_is_loaded(false), row_number_is_loaded(false), has_changes(false) {
	auto &block_manager = GetBlockManager();
	auto &info = GetTableInfo();
	auto &types = collection.get().GetTypes();
	columns.reserve(types.size());
	for (idx_t c = 0; c < types.size(); c++) {
		auto entry = ColumnData::CreateColumn(block_manager, info, c, types[c]);
		entry->InitializeColumn(data.column_data[c]);
		columns.push_back(std::move(entry));
	}

	Verify();
}

void RowGroup::MoveToCollection(RowGroupCollection &collection_p) {
	lock_guard<mutex> l(row_group_lock);
	// FIXME
	// MoveToCollection causes any_changes to be set to true because we are changing the start position of the row group
	// the start position is ONLY written when targeting old serialization versions - as such, we don't actually
	// need to do this when targeting newer serialization versions
	// not doing this could allow metadata reuse in these situations, which would improve vacuuming performance
	// especially when vacuuming from the beginning of large tables
	has_changes = true;
	this->collection = collection_p;
	for (idx_t c = 0; c < columns.size(); c++) {
		if (!ColumnIsLoaded(c)) {
			// we only need to set the column start position if it is already loaded
			// if it is not loaded - we will set the correct start position upon loading
			continue;
		}
		columns[c]->SetDataType(ColumnDataType::MAIN_TABLE);
	}
}

RowGroup::~RowGroup() {
}

bool RowGroup::ColumnIsLoaded(storage_t c) const {
	if (!is_loaded) {
		return true;
	}
	return is_loaded[c];
}

vector<shared_ptr<ColumnData>> &RowGroup::GetColumns() {
	// ensure all columns are loaded
	for (idx_t c = 0; c < GetColumnCount(); c++) {
		GetColumn(c);
	}
	return columns;
}

idx_t RowGroup::GetColumnCount() const {
	return columns.size();
}

idx_t RowGroup::GetRowGroupSize() const {
	return collection.get().GetRowGroupSize();
}

void RowGroup::LoadRowIdColumnData() const {
	if (row_id_is_loaded) {
		return;
	}
	lock_guard<mutex> l(row_group_lock);
	if (row_id_column_data) {
		return;
	}
	row_id_column_data = make_uniq<RowIdColumnData>(GetBlockManager(), GetTableInfo());
	row_id_column_data->count = count.load();
	row_id_is_loaded = true;
}

void RowGroup::LoadRowNumberColumnData() const {
	if (row_number_is_loaded) {
		return;
	}
	lock_guard<mutex> l(row_group_lock);
	if (row_number_column_data) {
		return;
	}
	row_number_column_data = make_uniq<RowNumberColumnData>(GetBlockManager(), GetTableInfo());
	row_number_column_data->count = count.load();
	row_number_is_loaded = true;
}

ColumnData &RowGroup::GetColumn(const StorageIndex &c) const {
	auto &res = GetColumn(c.GetPrimaryIndex());
	return res;
}

ColumnData &RowGroup::GetColumn(storage_t c) const {
	LoadColumn(c);
	if (c == COLUMN_IDENTIFIER_ROW_ID) {
		return *row_id_column_data;
	}
	if (c == COLUMN_IDENTIFIER_ROW_NUMBER) {
		return *row_number_column_data;
	}
	return *columns[c];
}

ColumnData &RowGroup::GetRawColumnData(const StorageIndex &c) const {
	return GetColumn(c);
}

ColumnData &RowGroup::GetRawColumnData(storage_t c) const {
	return GetColumn(c);
}

void RowGroup::LoadColumn(storage_t c) const {
	if (c == COLUMN_IDENTIFIER_ROW_ID) {
		LoadRowIdColumnData();
		return;
	}
	if (c == COLUMN_IDENTIFIER_ROW_NUMBER) {
		LoadRowNumberColumnData();
		return;
	}
	D_ASSERT(c < columns.size());
	if (!is_loaded) {
		// not being lazy loaded
		D_ASSERT(columns[c]);
		return;
	}
	if (is_loaded[c]) {
		D_ASSERT(columns[c]);
		return;
	}
	lock_guard<mutex> l(row_group_lock);
	if (columns[c]) {
		// another thread loaded the column while we were waiting for the lock
		D_ASSERT(is_loaded[c]);
		return;
	}
	// load the column
	if (column_pointers.size() != columns.size()) {
		throw InternalException("Lazy loading a column but the pointer was not set");
	}
	auto &metadata_manager = GetCollection().GetMetadataManager();
	auto &types = GetCollection().GetTypes();
	auto &block_pointer = column_pointers[c];
	MetadataReader column_data_reader(metadata_manager, block_pointer);
	this->columns[c] = ColumnData::Deserialize(GetBlockManager(), GetTableInfo(), c, column_data_reader, types[c]);
	is_loaded[c] = true;
	if (this->columns[c]->count != this->count) {
		throw DataCorruptionException("Corrupted database - loaded column with index %llu, count %llu did "
		                              "not match count of row group %llu",
		                              c, this->columns[c]->count.load(), this->count.load());
	}
}

void RowGroup::UnloadColumn(storage_t c) {
	if (column_pointers.size() != columns.size()) {
		throw InternalException("Trying to unload a column but column pointers were not set");
	}
	if (!ColumnIsLoaded(c)) {
		throw InternalException("Trying to unload a column that is not loaded");
	}
	lock_guard<mutex> l(row_group_lock);
	if (!is_loaded) {
		// is_loaded is not set - all columns must be loaded
		this->is_loaded = unique_ptr<atomic<bool>[]>(new atomic<bool>[columns.size()]);
		for (idx_t c = 0; c < columns.size(); c++) {
			this->is_loaded[c] = true;
		}
	}
	this->is_loaded[c] = false;
	columns[c].reset();
}

BlockManager &RowGroup::GetBlockManager() const {
	return GetCollection().GetBlockManager();
}
DataTableInfo &RowGroup::GetTableInfo() const {
	return GetCollection().GetTableInfo();
}

void RowGroup::InitializeEmpty(const vector<LogicalType> &types, ColumnDataType data_type) {
	// set up the segment trees for the column segments
	D_ASSERT(columns.empty());
	for (idx_t i = 0; i < types.size(); i++) {
		auto column_data = ColumnData::CreateColumn(GetBlockManager(), GetTableInfo(), i, types[i], data_type);
		columns.push_back(std::move(column_data));
	}
}

void ColumnScanState::PushDownCast(const LogicalType &original_type, const LogicalType &cast_type) {
	D_ASSERT(context.Valid());
	D_ASSERT(!expression_state);
	auto &client_context = *context.GetClientContext();

	auto input = make_uniq<BoundReferenceExpression>(original_type, 0ULL);
	auto cast_expression = BoundCastExpression::AddCastToType(client_context, std::move(input), cast_type);
	expression_state = make_uniq<PushedDownExpressionState>(client_context);
	expression_state->target.Initialize(client_context, {cast_type});
	expression_state->input.Initialize(client_context, {original_type});
	expression_state->executor.AddExpression(*cast_expression);
	expression_state->expression = std::move(cast_expression);
}

void ColumnScanState::Initialize(const QueryContext &context_p, const LogicalType &type, const StorageIndex &column_id,
                                 optional_ptr<TableScanOptions> options) {
	auto &children = column_id.GetChildIndexes();
	// Register the options in the state
	scan_options = options;
	context = context_p;
	storage_index = column_id;

	D_ASSERT(type.id() != LogicalTypeId::INVALID);
	if (type.id() == LogicalTypeId::VALIDITY) {
		// validity - nothing to initialize
		return;
	}

	if (type.id() == LogicalTypeId::VARIANT) {
		// variant - column scan states are created later
		// this is done because the internal shape of the VARIANT is different per rowgroup
		scan_child_column.resize(2, true);
		if (!storage_index.IsPushdownExtract()) {
			return;
		}
		auto &scan_type = storage_index.GetScanType();
		if (scan_type.id() != LogicalTypeId::VARIANT) {
			PushDownCast(type, scan_type);
		}
		return;
	}

	D_ASSERT(child_states.empty());
	if (type.InternalType() == PhysicalType::STRUCT) {
		// validity + struct children
		auto &struct_children = StructType::GetChildTypes(type);
		child_states.reserve(struct_children.size() + 1);
		for (idx_t i = 0; i <= struct_children.size(); i++) {
			child_states.emplace_back(parent);
		}

		if (children.empty()) {
			// scan all struct children
			scan_child_column.resize(struct_children.size(), true);
			for (idx_t i = 0; i < struct_children.size(); i++) {
				child_states[i + 1].Initialize(context, struct_children[i].second, options);
			}
		} else {
			if (storage_index.IsPushdownExtract()) {
				D_ASSERT(context.Valid());
				scan_child_column.resize(1, true);
				D_ASSERT(children.size() == 1);
				auto &child = children[0];
				auto child_index = child.GetPrimaryIndex();
				auto &child_type = StructType::GetChildTypes(type)[child_index].second;
				if (!child.HasChildren() && child_type != child.GetType()) {
					PushDownCast(child_type, child.GetType());
				}
				child_states[1].Initialize(context, struct_children[child_index].second, child, options);
			} else {
				// only scan the specified subset of columns
				scan_child_column.resize(struct_children.size(), false);
				for (idx_t i = 0; i < children.size(); i++) {
					auto &child = children[i];
					auto index = child.GetPrimaryIndex();
					scan_child_column[index] = true;
					child_states[index + 1].Initialize(context, struct_children[index].second, child, options);
				}
			}
		}
		child_states[0].scan_options = options;
	} else if (type.InternalType() == PhysicalType::LIST) {
		// validity + list child
		for (idx_t i = 0; i < 2; i++) {
			child_states.emplace_back(parent);
		}
		child_states[1].Initialize(context, ListType::GetChildType(type), options);
		child_states[0].scan_options = options;
	} else if (type.InternalType() == PhysicalType::ARRAY) {
		// validity + array child
		for (idx_t i = 0; i < 2; i++) {
			child_states.emplace_back(parent);
		}
		child_states[0].scan_options = options;
		child_states[1].Initialize(context, ArrayType::GetChildType(type), options);
	} else {
		// validity
		child_states.emplace_back(parent);
		child_states[0].scan_options = options;
	}
}

void ColumnScanState::Initialize(const QueryContext &context_p, const LogicalType &type,
                                 optional_ptr<TableScanOptions> options) {
	auto column_id = StorageIndex(0);
	Initialize(context_p, type, column_id, options);
}

void CollectionScanState::Initialize(const QueryContext &context_p, const vector<LogicalType> &types) {
	context = context_p;
	auto &column_ids = GetColumnIds();
	D_ASSERT(column_scans.empty());
	column_scans.reserve(column_ids.size());
	for (idx_t i = 0; i < column_ids.size(); i++) {
		column_scans.emplace_back(*this);
	}
	for (idx_t i = 0; i < column_ids.size(); i++) {
		if (column_ids[i].IsRowIdColumn() || column_ids[i].IsRowNumberColumn()) {
			continue;
		}
		auto index = column_ids[i].GetPrimaryIndex();
		auto &type = types[index];
		column_scans[i].Initialize(context, type, column_ids[i], &GetOptions());
	}
}

bool RowGroup::InitializeScanInternal(CollectionScanState &state, SegmentNode<RowGroup> &node, idx_t vector_offset) {
	auto &filters = state.GetFilterInfo();
	if (!CheckZonemap(state.context.GetClientContext(), filters, node.GetRowStart())) {
		return false;
	}
	if (!RefersToSameObject(node.GetNode(), *this)) {
		throw InternalException("RowGroup::InitializeScan segment node mismatch");
	}
	D_ASSERT(state.prepared_vector.prepare_state == VectorPrepareState::NONE);
	state.prepared_vector.Reset();
	state.assignment_io_registered = false;
	state.SetRowGroup(node);
	state.vector_index = vector_offset;
	auto row_start = node.GetRowStart();
	state.max_row_group_row = row_start > state.max_row ? 0 : MinValue<idx_t>(this->count, state.max_row - row_start);
	if (state.max_row_group_row == 0) {
		// exceeded row groups to scan
		return false;
	}
	D_ASSERT(!state.column_scans.empty());
	return true;
}

bool RowGroup::InitializeScanWithOffset(CollectionScanState &state, SegmentNode<RowGroup> &node, idx_t vector_offset,
                                        bool initialize_columns) {
	if (!InitializeScanInternal(state, node, vector_offset)) {
		return false;
	}
	state.column_scans_pending = true;
	if (initialize_columns) {
		InitializeColumnScans(state);
	}
	return true;
}

void RowGroup::InitializeColumnScans(CollectionScanState &state) {
	D_ASSERT(state.column_scans_pending);
	const auto &column_ids = state.GetColumnIds();
	auto row_number = state.vector_index * STANDARD_VECTOR_SIZE;
	for (idx_t i = 0; i < column_ids.size(); i++) {
		auto &column_data = GetColumn(column_ids[i]);
		column_data.InitializeScanWithOffset(state.column_scans[i], row_number);
		state.column_scans[i].scan_options = &state.GetOptions();
	}
	state.column_scans_pending = false;
}

bool RowGroup::InitializeScan(CollectionScanState &state, SegmentNode<RowGroup> &node) {
	if (!InitializeScanInternal(state, node, 0)) {
		return false;
	}
	const auto &column_ids = state.GetColumnIds();
	for (idx_t i = 0; i < column_ids.size(); i++) {
		auto &column_data = GetColumn(column_ids[i]);
		column_data.InitializeScan(state.column_scans[i]);
		state.column_scans[i].scan_options = &state.GetOptions();
	}
	return true;
}

unique_ptr<RowGroup> RowGroup::CreateNewRowGroupCopy(RowGroupCollection &new_collection, idx_t new_column_count) {
	auto row_group = make_uniq<RowGroup>(new_collection, this->count);
	row_group->version_info = version_info;
	row_group->columns.resize(new_column_count);
	if (is_loaded) {
		row_group->is_loaded = unique_ptr<atomic<bool>[]>(new atomic<bool>[new_column_count]);
	}
	if (!column_pointers.empty()) {
		row_group->column_pointers.resize(new_column_count);
	}
	row_group->has_per_column_metadata_blocks = has_per_column_metadata_blocks;
	row_group->has_changes = true;
	return row_group;
}

unique_ptr<RowGroup> RowGroup::AlterType(RowGroupCollection &new_collection, const LogicalType &target_type,
                                         idx_t changed_idx, ExpressionExecutor &executor,
                                         CollectionScanState &scan_state, SegmentNode<RowGroup> &node,
                                         DataChunk &scan_chunk, TransactionData transaction,
                                         ColumnStatistics &changed_stats) {
	Verify();

	// construct a new column data for this type
	auto column_data = ColumnData::CreateColumn(GetBlockManager(), GetTableInfo(), changed_idx, target_type);

	ColumnAppendState append_state;
	column_data->InitializeAppend(append_state);

	// scan the original table, and fill the new column with the transformed value
	InitializeScan(scan_state, node);

	DataChunk append_chunk;
	vector<LogicalType> append_types;
	append_types.push_back(target_type);
	append_chunk.Initialize(Allocator::DefaultAllocator(), append_types);
	auto &append_vector = append_chunk.data[0];
	Vector hashes(LogicalType::HASH);
	ScanOptions options(transaction);
	options.insert_type = InsertedScanType::ALL_ROWS;
	options.delete_type = DeletedScanType::INCLUDE_ALL_DELETED;
	while (true) {
		// scan the table
		scan_chunk.Reset();
		Scan(options, scan_state, scan_chunk);
		if (scan_chunk.size() == 0) {
			break;
		}
		// execute the expression
		append_chunk.Reset();
		executor.ExecuteExpression(scan_chunk, append_vector);
		changed_stats.UpdateDistinctStatistics(append_vector, scan_chunk.size(), hashes);
		column_data->Append(append_state, append_vector, scan_chunk.size());
	}
	column_data->FinalizeAppend(nullptr, append_state);

	auto supports_per_column_writes = new_collection.SupportsPerColumnWrites();
	if (!supports_per_column_writes) {
		// ensure all columns are loaded, as checkpointing will need to load them anyway, and it's better to fail-fast
		// in case these don't fit in memory here
		GetColumns();
	}
	unique_lock<mutex> lock(row_group_lock);
	auto row_group = CreateNewRowGroupCopy(new_collection, columns.size());
	// copy existing columns, but swap out the one at changed_idx
	for (idx_t i = 0; i < columns.size(); i++) {
		if (i == changed_idx) {
			row_group->columns[i] = std::move(column_data);
			if (row_group->is_loaded) {
				row_group->is_loaded[i] = true;
			}
			column_data.reset();
		} else {
			row_group->columns[i] = columns[i];
			if (row_group->is_loaded) {
				row_group->is_loaded[i] = is_loaded[i].load();
			}
			if (!row_group->column_pointers.empty()) {
				row_group->column_pointers[i] = column_pointers[i];
			}
		}
	}
	if (has_per_column_metadata_blocks) {
		row_group->per_column_metadata_blocks = per_column_metadata_blocks;
		row_group->per_column_metadata_blocks.ClearColumn(changed_idx);
	}
	lock.unlock();
	row_group->Verify();
	return row_group;
}

unique_ptr<RowGroup> RowGroup::AddColumn(RowGroupCollection &new_collection, ColumnDefinition &new_column,
                                         ExpressionExecutor &executor, ColumnStatistics &new_column_stats) {
	Verify();

	// construct a new column data for the new column
	auto added_column =
	    ColumnData::CreateColumn(GetBlockManager(), GetTableInfo(), GetColumnCount(), new_column.Type());

	idx_t rows_to_write = this->count;
	if (rows_to_write > 0) {
		DataChunk dummy_chunk;
		DataChunk result_chunk;
		result_chunk.Initialize(Allocator::DefaultAllocator(), {new_column.GetType()});
		auto &result = result_chunk.data[0];
		Vector hashes(LogicalType::HASH);

		ColumnAppendState state;
		added_column->InitializeAppend(state);
		for (idx_t i = 0; i < rows_to_write; i += STANDARD_VECTOR_SIZE) {
			idx_t rows_in_this_vector = MinValue<idx_t>(rows_to_write - i, STANDARD_VECTOR_SIZE);
			dummy_chunk.SetChildCardinality(rows_in_this_vector);
			result_chunk.Reset();
			executor.ExecuteExpression(dummy_chunk, result);
			new_column_stats.UpdateDistinctStatistics(result, rows_in_this_vector, hashes);
			added_column->Append(state, result, rows_in_this_vector);
		}
		added_column->FinalizeAppend(nullptr, state);
	}

	if (!new_collection.SupportsPerColumnWrites()) {
		// ensure all columns are loaded, as checkpointing will need to load them anyway, and it's better to fail-fast
		// in case these don't fit in memory here
		GetColumns();
	}

	unique_lock<mutex> lock(row_group_lock);
	auto row_group = CreateNewRowGroupCopy(new_collection, columns.size() + 1);
	// copy existing columns
	for (idx_t i = 0; i < columns.size(); i++) {
		row_group->columns[i] = columns[i];
		if (row_group->is_loaded) {
			row_group->is_loaded[i] = is_loaded[i].load();
		}
		if (!row_group->column_pointers.empty()) {
			row_group->column_pointers[i] = column_pointers[i];
		}
	}
	if (has_per_column_metadata_blocks) {
		row_group->per_column_metadata_blocks = per_column_metadata_blocks;
	}
	// add the new column
	row_group->columns[columns.size()] = std::move(added_column);
	if (row_group->is_loaded) {
		row_group->is_loaded[columns.size()] = true;
	}
	lock.unlock();
	row_group->Verify();
	return row_group;
}

unique_ptr<RowGroup> RowGroup::RemoveColumn(RowGroupCollection &new_collection, idx_t removed_column) {
	Verify();

	D_ASSERT(removed_column < columns.size());

	if (!new_collection.SupportsPerColumnWrites()) {
		// ensure all columns are loaded, as checkpointing will need to load them anyway, and it's better to fail-fast
		// in case these don't fit in memory here
		GetColumns();
	}
	unique_lock<mutex> lock(row_group_lock);
	auto row_group = CreateNewRowGroupCopy(new_collection, columns.size() - 1);
	// copy over all columns except for the removed one
	idx_t target_idx = 0;
	for (idx_t i = 0; i < columns.size(); i++) {
		if (i == removed_column) {
			continue;
		}
		row_group->columns[target_idx] = columns[i];
		if (row_group->is_loaded) {
			row_group->is_loaded[target_idx] = is_loaded[i].load();
		}
		if (!row_group->column_pointers.empty()) {
			row_group->column_pointers[target_idx] = column_pointers[i];
		}
		target_idx++;
	}
	if (has_per_column_metadata_blocks) {
		row_group->per_column_metadata_blocks = per_column_metadata_blocks;
		// the columns after the removed one shift down by one position, so their
		// metadata block entries (keyed by column index) must shift down as well
		row_group->per_column_metadata_blocks.RemoveColumn(removed_column);
	}
	lock.unlock();
	row_group->Verify();
	return row_group;
}

void RowGroup::CommitDrop(CommitDropState &drop_state) {
	for (idx_t column_idx = 0; column_idx < GetColumnCount(); column_idx++) {
		CommitDropColumn(column_idx, drop_state);
	}
}

struct BlockIdDropper : public BlockIdVisitor {
	explicit BlockIdDropper(CommitDropState &drop_state) : drop_state(drop_state) {
	}

	void Visit(block_id_t block_id) override {
		drop_state.DropBlock(block_id);
	}

	CommitDropState &drop_state;
};

void RowGroup::CommitDropColumn(const idx_t column_index, CommitDropState &drop_state) {
	auto &column = GetColumn(column_index);
	BlockIdDropper dropper(drop_state);
	column.VisitBlockIds(dropper);
}

void RowGroup::CommitDrop() {
	CommitDropState drop_state(&GetBlockManager());
	CommitDrop(drop_state);
	drop_state.FinalizeCommit();
}

void RowGroup::FinishVector(CollectionScanState &state) {
	state.vector_index++;
	state.prepared_vector.Reset();
}

void RowGroup::NextVector(CollectionScanState &state) {
	FinishVector(state);
	const auto &column_ids = state.GetColumnIds();
	for (idx_t i = 0; i < column_ids.size(); i++) {
		const auto &column = column_ids[i];
		GetColumn(column).Skip(state.column_scans[i]);
	}
}

static idx_t SystemRowsSelection(const ScanSamplingInfo &sampling_info, idx_t start_row, idx_t count,
                                 SelectionVector &sel) {
	auto rate = sampling_info.sample_rate;
	if (rate >= 1) {
		return count;
	}
	idx_t result_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto row_idx = start_row + i;
		auto before = std::floor(LossyNumericCast<double>(row_idx) * rate + sampling_info.sample_phase);
		auto after = std::floor(LossyNumericCast<double>(row_idx + 1) * rate + sampling_info.sample_phase);
		if (after > before) {
			sel.set_index(result_count++, i);
		}
	}
	return result_count;
}

static idx_t IntersectSelections(const SelectionVector &left, idx_t left_count, const SelectionVector &right,
                                 idx_t right_count, SelectionVector &result) {
	idx_t left_idx = 0;
	idx_t right_idx = 0;
	idx_t result_count = 0;
	while (left_idx < left_count && right_idx < right_count) {
		auto left_entry = left.get_index(left_idx);
		auto right_entry = right.get_index(right_idx);
		if (left_entry == right_entry) {
			result.set_index(result_count++, left_entry);
			left_idx++;
			right_idx++;
		} else if (left_entry < right_entry) {
			left_idx++;
		} else {
			right_idx++;
		}
	}
	return result_count;
}

static BaseStatistics CreateRowIdStats(idx_t beg_row, idx_t end_row) {
	D_ASSERT(end_row > beg_row);
	auto result = NumericStats::CreateEmpty(LogicalType::ROW_TYPE);
	result.SetHasNoNullFast();
	NumericStats::SetMin(result, UnsafeNumericCast<row_t>(beg_row));
	NumericStats::SetMax(result, UnsafeNumericCast<row_t>(end_row - 1));
	return result;
}

FilterPropagateResult RowGroup::CheckRowIdFilter(const TableFilter &filter, idx_t beg_row, idx_t end_row) {
	if (end_row <= beg_row) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	// RowId columns dont have a zonemap, but we can trivially create stats to check the filter against.
	auto rowid_stats = CreateRowIdStats(beg_row, end_row);

	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "RowGroup::CheckRowIdFilter");
	return expr_filter.CheckStatistics(rowid_stats);
}

bool RowGroup::CheckZonemap(optional_ptr<ClientContext> context, ScanFilterInfo &filters, idx_t row_start) {
	const auto table_filters = filters.GetTableFilters();
	const auto column_ids = filters.GetColumnIds();
	if (table_filters && column_ids) {
		for (const auto &filter : table_filters->GetMultiColumnFilters()) {
			const auto &expression_filter = ExpressionFilter::GetExpressionFilter(*filter, "RowGroup::CheckZonemap");
			vector<BaseStatistics> input_stats;
			input_stats.reserve(expression_filter.column_indexes.size());
			bool supported = true;
			for (const auto &column_index : expression_filter.column_indexes) {
				if (column_index.GetIndex() >= column_ids->size()) {
					throw InternalException("Multi-column filter column index out of range");
				}
				const auto &storage_index = (*column_ids)[column_index.GetIndex()];
				if (storage_index.IsRowIdColumn()) {
					input_stats.push_back(CreateRowIdStats(row_start, row_start + count));
					continue;
				}
				if (storage_index.IsRowNumberColumn()) {
					supported = false;
					break;
				}
				auto column_stats = GetStatistics(storage_index);
				if (!column_stats) {
					supported = false;
					break;
				}
				input_stats.push_back(column_stats->Copy());
			}
			if (!supported) {
				continue;
			}
			const auto prune_result = ExpressionFilter::CheckExpressionStatistics(
			    context, *expression_filter.expr,
			    array_ptr<const BaseStatistics>(input_stats.data(), input_stats.size()));
			if (prune_result == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
			    prune_result == FilterPropagateResult::FILTER_FALSE_OR_NULL) {
				return false;
			}
		}
	}
	auto &filter_list = filters.GetFilterList();
	// new row group - label all filters as up for grabs again
	filters.CheckAllFilters();
	for (idx_t i = 0; i < filter_list.size(); i++) {
		auto &entry = filter_list[i];
		auto &filter = entry.filter;
		const auto &base_column_index = entry.table_column_index;

		FilterPropagateResult prune_result;
		if (base_column_index.IsRowIdColumn()) {
			// the row ids in this row group span exactly [row_start, row_start + count)
			prune_result = CheckRowIdFilter(filter, row_start, row_start + count);
		} else {
			prune_result = GetColumn(base_column_index).CheckZonemap(context, base_column_index, filter);
		}
		if (prune_result == FilterPropagateResult::FILTER_ALWAYS_FALSE ||
		    prune_result == FilterPropagateResult::FILTER_FALSE_OR_NULL) {
			return false;
		}
		if (ExpressionFilter::IsRootNonSelectivityOptionalFilter(filter)) {
			// these are only for row group checking, set as always true so we don't check it
			filters.SetFilterAlwaysTrue(i);
		} else if (prune_result == FilterPropagateResult::FILTER_ALWAYS_TRUE) {
			// filter is always true - no need to check it
			// label the filter as always true so we don't need to check it anymore
			filters.SetFilterAlwaysTrue(i);
		}
	}
	return true;
}

bool RowGroup::CheckZonemapSegments(CollectionScanState &state) {
	auto &filters = state.GetFilterInfo();
	optional_idx target_vector_index_max;
	for (auto &entry : filters.GetFilterList()) {
		if (entry.IsAlwaysTrue()) {
			// filter is always true - avoid checking
			continue;
		}
		auto column_idx = entry.scan_column_index;
		auto base_column_idx = entry.table_column_index;
		auto &filter = entry.filter;
		auto &column_data = GetColumn(base_column_idx);

		optional_ptr<SegmentNode<ColumnSegment>> current_segment;
		auto prune_result = column_data.CheckZonemap(state.column_scans[column_idx], filter, current_segment);
		if (prune_result != FilterPropagateResult::FILTER_ALWAYS_FALSE &&
		    prune_result != FilterPropagateResult::FILTER_FALSE_OR_NULL) {
			continue;
		}

		// check zone map segment.
		if (!current_segment) {
			// no segment to skip
			continue;
		}
		auto row_start = current_segment->GetRowStart();
		idx_t target_row = row_start + current_segment->GetNode().count;
		if (target_row >= state.max_row) {
			target_row = state.max_row;
		}
		D_ASSERT(target_row >= row_start);
		D_ASSERT(target_row <= row_start + this->count);
		// current_segment->GetRowStart() is already row-group-relative, and state.vector_index uses the same
		// coordinate space. Subtracting row_start here incorrectly makes the target segment-local.
		idx_t target_vector_index = target_row / STANDARD_VECTOR_SIZE;

		if (!target_vector_index_max.IsValid() || target_vector_index_max.GetIndex() < target_vector_index) {
			target_vector_index_max = target_vector_index;
		}
	}
	if (target_vector_index_max.IsValid()) {
		if (state.vector_index == target_vector_index_max.GetIndex()) {
			// we can't skip any full vectors because this segment contains less than a full vector
			// for now we just bail-out
			// FIXME: we could check if we can ALSO skip the next segments, in which case skipping a full vector
			// might be possible
			// we don't care that much though, since a single segment that fits less than a full vector is
			// exceedingly rare
			return true;
		}
		while (state.vector_index < target_vector_index_max.GetIndex()) {
			NextVector(state);
		}
		return false;
	} else {
		return true;
	}
}

bool RowGroup::RegisterScanIO(CollectionScanState &state, idx_t row_count, PrefetchState &prefetch_state) const {
	if (!GetBlockManager().Prefetch()) {
		return false;
	}
	const auto &column_ids = state.GetColumnIds();
	for (idx_t i = 0; i < column_ids.size(); i++) {
		GetColumn(column_ids[i]).InitializePrefetch(prefetch_state, state.column_scans[i], row_count);
	}
	return true;
}

void RowGroup::PrefetchScanIO(CollectionScanState &state, idx_t row_count) const {
	PrefetchState prefetch_state;
	if (!RegisterScanIO(state, row_count, prefetch_state)) {
		return;
	}
	GetBlockManager().buffer_manager.Prefetch(state.context, prefetch_state.blocks);
}

vector<unique_ptr<AsyncTask>> RowGroup::CollectScanIOTasks(CollectionScanState &state, idx_t row_count) const {
	PrefetchState prefetch_state;
	if (!RegisterScanIO(state, row_count, prefetch_state)) {
		return vector<unique_ptr<AsyncTask>>();
	}
	return GetBlockManager().buffer_manager.CreatePrefetchTasks(state.context, prefetch_state.blocks);
}

idx_t RowGroup::PrefetchRowCount(CollectionScanState &state) {
	const idx_t start_row = state.vector_index * STANDARD_VECTOR_SIZE;
	idx_t end_row = state.max_row_group_row;
	auto context = state.context.GetClientContext();
	for (auto &entry : state.GetFilterInfo().GetFilterList()) {
		if (entry.IsAlwaysTrue() || entry.table_column_index.IsPushdownExtract()) {
			continue;
		}
		auto &column_data = GetColumn(entry.table_column_index);
		end_row = MinValue<idx_t>(end_row, column_data.ZonemapScanEnd(context, start_row, end_row, entry.filter));
	}
	return end_row > start_row ? end_row - start_row : 0;
}

bool RowGroup::PrepareScan(ScanOptions options, CollectionScanState &state) {
	auto &prepared = state.prepared_vector;
	if (prepared.prepare_state != VectorPrepareState::NONE) {
		return true;
	}
	while (true) {
		const idx_t remaining_rows = state.RemainingAssignmentRows();
		if (remaining_rows == 0) {
			// exceeded the amount of rows to scan
			return false;
		}
		idx_t current_row = state.vector_index * STANDARD_VECTOR_SIZE;
		idx_t max_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, remaining_rows);
		bool has_sample_selection = false;
		idx_t sample_count = max_count;
		auto &sample_sel = prepared.sample_sel;

		// check the sampling info if we have to sample this chunk
		if (state.GetSamplingInfo().do_system_sample) {
			auto &sampling_info = state.GetSamplingInfo();
			if (!sampling_info.is_percentage) {
				double rate = sampling_info.sample_rate;
				if (rate <= 0) {
					NextVector(state);
					continue;
				}
				if (rate < 1) {
					auto row_group_start = state.GetRowGroup()->GetRowStart();
					sample_count =
					    SystemRowsSelection(sampling_info, row_group_start + current_row, max_count, sample_sel);
					if (sample_count == 0) {
						NextVector(state);
						continue;
					}
					has_sample_selection = true;
				}
			} else {
				// percentage based system sampling
				if (state.GetRandomEngine().NextRandom() > sampling_info.sample_rate) {
					NextVector(state);
					continue;
				}
			}
		}

		//! first check the zonemap if we have to scan this partition
		if (!CheckZonemapSegments(state)) {
			continue;
		}
		// second, scan the version chunk manager to figure out which tuples to load for this transaction
		idx_t count = GetSelVector(options, state.vector_index, state.valid_sel, max_count);
		if (count == 0) {
			// nothing to scan for this vector, skip the entire vector
			NextVector(state);
			continue;
		}
		state.rows_scanned += count;

		prepared.prepare_state = VectorPrepareState::PREPARED;
		prepared.max_count = max_count;
		prepared.visible_count = count;
		prepared.has_sample_selection = has_sample_selection;
		prepared.sample_count = sample_count;
		return true;
	}
}

void RowGroup::ProcessPreparedScan(ScanOptions options, CollectionScanState &state, DataChunk &result) {
	const auto &column_ids = state.GetColumnIds();
	auto &filter_info = state.GetFilterInfo();
	auto &transaction = options.transaction;
	auto &prepared = state.prepared_vector;
	D_ASSERT(prepared.prepare_state != VectorPrepareState::NONE);
	idx_t max_count = prepared.max_count;
	idx_t count = prepared.visible_count;
	bool has_sample_selection = prepared.has_sample_selection;
	idx_t sample_count = prepared.sample_count;
	auto &sample_sel = prepared.sample_sel;

	bool has_filters = filter_info.HasFilters();
	if (count == max_count && !has_filters) {
		// scan all vectors completely: full scan without deletions or table filters
		for (idx_t i = 0; i < column_ids.size(); i++) {
			const auto &column = column_ids[i];
			auto &col_data = GetColumn(column);
			state.column_scans[i].update_scan_type = options.update_type;
			// pass max_count explicitly, concurrent inserts can grow the column past the count captured at scan init
			col_data.Scan(transaction, state.vector_index, state.column_scans[i], result.data[i], max_count);
			if (has_sample_selection) {
				result.data[i].Slice(sample_sel, sample_count);
			}
		}
		if (has_sample_selection) {
			count = sample_count;
		}
	} else {
		// partial scan: we have deletions or table filters
		idx_t approved_tuple_count = count;
		SelectionVector sel;
		SelectionVector intersect_sel(STANDARD_VECTOR_SIZE);
		if (has_sample_selection && count != max_count) {
			approved_tuple_count = IntersectSelections(state.valid_sel, count, sample_sel, sample_count, intersect_sel);
			if (approved_tuple_count == 0) {
				NextVector(state);
				return;
			}
			sel.Initialize(intersect_sel);
		} else if (has_sample_selection) {
			approved_tuple_count = sample_count;
			sel.Initialize(sample_sel);
		} else if (count != max_count) {
			sel.Initialize(state.valid_sel);
		} else {
			sel.Initialize(nullptr);
		}
		//! first, we scan the columns with filters, fetch their data and generate a selection vector.
		auto adaptive_filter = filter_info.GetAdaptiveFilter();
		auto filter_state = filter_info.BeginFilter();
		if (has_filters) {
			auto &filter_list = filter_info.GetFilterList();
			const auto &permutation = adaptive_filter->GetPermutation();
			for (idx_t i = 0; i < filter_list.size(); i++) {
				auto filter_idx = permutation[i];
				auto &filter = filter_list[filter_idx];
				if (filter.IsAlwaysTrue()) {
					continue;
				}
				auto &table_filter_state = *filter.filter_state;

				const auto scan_idx = filter.scan_column_index;
				const auto column_idx = filter.table_column_index;

				auto &result_vector = result.data[scan_idx];
				if (approved_tuple_count == 0) {
					auto &col_data = GetColumn(column_idx);
					col_data.Skip(state.column_scans[scan_idx]);
					continue;
				}
				auto &col_data = GetColumn(column_idx);
				col_data.Filter(transaction, state.vector_index, state.column_scans[scan_idx], result_vector, sel,
				                approved_tuple_count, filter.filter, table_filter_state);
			}
			for (auto &table_filter : filter_list) {
				if (table_filter.IsAlwaysTrue()) {
					continue;
				}
				result.data[table_filter.scan_column_index].Slice(sel, approved_tuple_count);
			}
		}
		if (approved_tuple_count == 0) {
			// all rows were filtered out by the table filters
			D_ASSERT(has_filters);
			result.Reset();
			// skip this vector in all the scans that were not scanned yet
			for (idx_t i = 0; i < column_ids.size(); i++) {
				auto &col_idx = column_ids[i];
				if (has_filters && filter_info.ColumnHasFilters(i)) {
					continue;
				}
				auto &col_data = GetColumn(col_idx);
				col_data.Skip(state.column_scans[i]);
			}
			filter_info.EndFilter(filter_state);
			FinishVector(state);
			return;
		}
		//! Now we use the selection vector to fetch data for the other columns.
		for (idx_t i = 0; i < column_ids.size(); i++) {
			if (has_filters && filter_info.ColumnHasFilters(i)) {
				// column has already been scanned as part of the filtering process
				continue;
			}
			auto &column = column_ids[i];
			auto &col_data = GetColumn(column);
			state.column_scans[i].update_scan_type = options.update_type;
			col_data.Select(transaction, state.vector_index, state.column_scans[i], result.data[i], sel,
			                approved_tuple_count);
		}
		filter_info.EndFilter(filter_state);

		D_ASSERT(approved_tuple_count > 0);
		count = approved_tuple_count;
	}
	result.SetChildCardinality(count);
	FinishVector(state);
}

void RowGroup::Scan(ScanOptions options, CollectionScanState &state, DataChunk &result) {
	while (PrepareScan(options, state)) {
		PrefetchScanIO(state, state.prepared_vector.max_count);
		ProcessPreparedScan(options, state, result);
		if (result.size() > 0) {
			return;
		}
	}
}

ScanOptions::ScanOptions(TransactionData transaction) : transaction(transaction) {
}

void RowGroup::Scan(CollectionScanState &state, DataChunk &result, TableScanType type) {
	auto &transaction_manager = DuckTransactionManager::Get(GetCollection().GetAttached());

	VisibilityBound visibility_bound;
	if (type == TableScanType::TABLE_SCAN_COMMITTED_ROWS) {
		visibility_bound = VisibilityBound::Through(transaction_manager.GetLastCommit());
	} else {
		visibility_bound = transaction_manager.LowestVisibilityBound();
	}
	// a scan on behalf of no transaction: there are no writes of its own to see
	TransactionData transaction(MAX_TRANSACTION_ID, visibility_bound);

	ScanOptions options(transaction);
	options.insert_type = InsertedScanType::ALL_ROWS;
	switch (type) {
	case TableScanType::TABLE_SCAN_ALL_ROWS:
		options.delete_type = DeletedScanType::INCLUDE_ALL_DELETED;
		break;
	case TableScanType::TABLE_SCAN_OMIT_PERMANENTLY_DELETED:
	case TableScanType::TABLE_SCAN_COMMITTED_ROWS:
		options.delete_type = DeletedScanType::OMIT_COMMITTED_DELETES;
		options.update_type = UpdateScanType::DISALLOW_UPDATES;
		break;
	default:
		throw InternalException("Unrecognized table scan type");
	}
	Scan(options, state, result);
}

//===--------------------------------------------------------------------===//
// RowGroupVersionInfo
//===--------------------------------------------------------------------===//
RowGroupVersionInfo::RowGroupVersionInfo(vector<MetaBlockPointer> deletes_pointers_p)
    : deletes_pointers(std::move(deletes_pointers_p)), deletes_is_loaded(false), version_info(nullptr) {
}

bool RowGroupVersionInfo::HasUnloadedDeletes() const {
	if (deletes_pointers.empty()) {
		// no stored deletes at all
		return false;
	}
	// return whether or not the deletes have been loaded
	return !deletes_is_loaded;
}

optional_ptr<RowVersionManager> RowGroupVersionInfo::Get(BlockManager &block_manager) {
	if (!HasUnloadedDeletes()) {
		// deletes are loaded - return the version info
		return version_info.load();
	}
	lock_guard<mutex> guard(lock);
	// double-check after obtaining the lock whether or not deletes are still not loaded to avoid double load
	if (!HasUnloadedDeletes()) {
		return version_info.load();
	}
	D_ASSERT(!deletes_pointers.empty());
	auto root_delete = deletes_pointers[0];
	owned_version_info = RowVersionManager::Deserialize(root_delete, block_manager.GetMetadataManager());
	version_info = owned_version_info.get();
	deletes_is_loaded = true;
	return version_info.load();
}

shared_ptr<RowVersionManager> RowGroupVersionInfo::GetOrCreate(BlockManager &block_manager) {
	// load the persisted deletes first, so that a new manager does not shadow them
	Get(block_manager);
	lock_guard<mutex> guard(lock);
	if (!owned_version_info) {
		owned_version_info = make_shared_ptr<RowVersionManager>(block_manager.GetBufferManager());
		version_info = owned_version_info.get();
	}
	return owned_version_info;
}

optional_ptr<RowVersionManager> RowGroup::GetVersionInfo() {
	return version_info->Get(GetBlockManager());
}

shared_ptr<RowVersionManager> RowGroup::GetOrCreateVersionInfoPtr() {
	return version_info->GetOrCreate(GetBlockManager());
}

RowVersionManager &RowGroup::GetOrCreateVersionInfo() {
	return *GetOrCreateVersionInfoPtr();
}

optional_ptr<RowVersionManager> RowGroup::GetVersionInfoIfLoaded() const {
	return version_info->GetIfLoaded();
}

idx_t RowGroup::GetSelVector(ScanOptions options, idx_t vector_idx, SelectionVector &sel_vector, idx_t max_count) {
	if (options.insert_type == InsertedScanType::ALL_ROWS &&
	    options.delete_type == DeletedScanType::INCLUDE_ALL_DELETED) {
		return max_count;
	}
	auto vinfo = GetVersionInfo();
	if (!vinfo) {
		return max_count;
	}
	return vinfo->GetSelVector(options, vector_idx, sel_vector, max_count);
}

idx_t RowGroup::Fetch(TransactionData transaction, const idx_t *offsets, idx_t fetch_count,
                      SelectionVector &visible_sel) {
	if (fetch_count == 0) {
		return 0;
	}
	auto vinfo = GetVersionInfo();
	if (!vinfo) {
		// No version info at all, which means every row is visible.
		return fetch_count;
	}
	return vinfo->GetVisibleRows(transaction, offsets, fetch_count, visible_sel);
}

void RowGroup::FetchRows(TransactionData transaction, ColumnFetchState &state, const vector<StorageIndex> &column_ids,
                         const idx_t *offsets, const SelectionVector &visible_sel, idx_t visible_count,
                         DataChunk &result, idx_t result_offset) {
	if (visible_count == 0) {
		return;
	}

	for (idx_t col_idx = 0; col_idx < column_ids.size(); col_idx++) {
		auto &column = column_ids[col_idx];
		auto &result_vector = result.data[col_idx];
		D_ASSERT(result_vector.GetVectorType() == VectorType::FLAT_VECTOR);
		auto &col_data = GetColumn(column);
		col_data.FetchRows(transaction, state, column, offsets, visible_sel, visible_count, result_vector,
		                   result_offset);
	}
}

void RowGroup::SetCount(idx_t count) {
	this->count = count;
	lock_guard<mutex> guard(row_group_lock);
	if (row_id_is_loaded) {
		row_id_column_data->count = count;
	}
	if (row_number_is_loaded) {
		row_number_column_data->count = count;
	}
}

void RowGroup::AppendVersionInfo(TransactionData transaction, idx_t count) {
	const idx_t row_group_size = GetRowGroupSize();
	idx_t row_group_start = this->count.load();
	idx_t row_group_end = row_group_start + count;
	if (row_group_end > row_group_size) {
		row_group_end = row_group_size;
	}
	// create the version_info if it doesn't exist yet
	auto &vinfo = GetOrCreateVersionInfo();
	vinfo.AppendVersionInfo(transaction, count, row_group_start, row_group_end);
	SetCount(row_group_end);
}

void RowGroup::CommitAppend(transaction_t commit_id, idx_t row_group_start, idx_t count) {
	auto &vinfo = GetOrCreateVersionInfo();
	vinfo.CommitAppend(commit_id, row_group_start, count);
}

void RowGroup::RevertAppend(idx_t new_count) {
	if (new_count > this->count) {
		throw InternalException("RowGroup::RevertAppend new_count out of range");
	}
	// version info or columns that are not loaded hold no appended rows
	auto vinfo = GetVersionInfoIfLoaded();
	if (vinfo) {
		vinfo->RevertAppend(new_count);
	}
	for (idx_t c = 0; c < GetColumnCount(); c++) {
		if (ColumnIsLoaded(c)) {
			columns[c]->RevertAppend(UnsafeNumericCast<row_t>(new_count));
		}
	}
	if (transient) {
		*transient = SuballocationBlock();
	}
	SetCount(new_count);
	Verify();
}

RowGroupAppendState::RowGroupAppendState(TableAppendState &parent_p)
    : parent(parent_p), row_group(nullptr), offset_in_row_group(0) {
}

RowGroupAppendState::~RowGroupAppendState() {
}

void RowGroup::InitializeAppend(SegmentNode<RowGroup> &row_group, RowGroupAppendState &append_state) {
	append_state.row_group = row_group;
	row_group.GetNode().InitializeAppendInternal(append_state);
}

void RowGroup::InitializeNextAppend(SegmentNode<RowGroup> &row_group, RowGroupAppendState &append_state) {
	D_ASSERT(append_state.row_group);
	auto &previous = append_state.row_group->GetNode();
	auto &next = row_group.GetNode();
	D_ASSERT(!RefersToSameObject(previous, next));
	D_ASSERT(RefersToSameObject(previous.GetCollection(), next.GetCollection()));
	D_ASSERT(!next.transient);
	next.transient = std::move(previous.transient);
	InitializeAppend(row_group, append_state);
}

void RowGroup::InitializeAppendInternal(RowGroupAppendState &append_state) {
	if (!RefersToSameObject(append_state.row_group->GetNode(), *this)) {
		throw InternalException("RowGroup::InitializeAppend mismatch - call RowGroupAppendState::InitializeAppend");
	}
	append_state.offset_in_row_group = this->count;
	if (!transient) {
		transient = make_uniq<SuballocationBlock>();
	}
	// for each column, initialize the append state
	append_state.states = make_unsafe_uniq_array<ColumnAppendState>(GetColumnCount());
	for (idx_t i = 0; i < GetColumnCount(); i++) {
		auto &col_data = GetColumn(i);
		auto &state = append_state.states[i];
		state.transient = transient.get();
		col_data.InitializeAppend(state);
	}
}

void RowGroup::Append(RowGroupAppendState &state, DataChunk &chunk, idx_t append_count) {
	// append to the current row_group
	D_ASSERT(chunk.ColumnCount() == GetColumnCount());
	for (idx_t i = 0; i < GetColumnCount(); i++) {
		auto &col_data = GetColumn(i);
		auto prev_allocation_size = col_data.GetAllocationSize();
		col_data.Append(state.states[i], chunk.data[i], append_count);
		allocation_size += col_data.GetAllocationSize() - prev_allocation_size;
	}
	state.offset_in_row_group += append_count;
}

void RowGroup::FinalizeAppend(RowGroupAppendState &state) {
	auto &parent_stats = state.parent.stats;
	if (parent_stats.Empty()) {
		for (idx_t c = 0; c < GetColumnCount(); c++) {
			auto &col_data = GetColumn(c);
			col_data.FinalizeAppend(nullptr, state.states[c]);
		}
	} else {
		auto stats_lock = parent_stats.GetLock();
		for (idx_t c = 0; c < GetColumnCount(); c++) {
			auto &col_data = GetColumn(c);
			col_data.FinalizeAppend(parent_stats.GetStats(*stats_lock, c).Statistics(), state.states[c]);
		}
	}
}

void RowGroup::CleanupAppend(VisibilityBound lowest_visibility_bound, idx_t start, idx_t count) {
	auto &vinfo = GetOrCreateVersionInfo();
	vinfo.CleanupAppend(lowest_visibility_bound, start, count);
}

void RowGroup::Update(TransactionData transaction, DuckTableEntry &table_entry, DataChunk &update_chunk, row_t *ids,
                      idx_t offset, idx_t count, const vector<PhysicalIndex> &column_ids, idx_t row_group_start) {
#ifdef DEBUG
	for (size_t i = offset; i < offset + count; i++) {
		D_ASSERT(ids[i] >= row_t(row_group_start) && ids[i] < row_t(row_group_start + this->count));
	}
#endif
	if (transaction.transaction) {
		// mark the rows before updating them: an update conflicts with any write of a row the transaction does not see
		auto vector_idx = (UnsafeNumericCast<idx_t>(ids[offset]) - row_group_start) / STANDARD_VECTOR_SIZE;
		auto vector_start = row_group_start + vector_idx * STANDARD_VECTOR_SIZE;
		WrittenRows written;
		WriteRows(transaction, table_entry, vector_idx, vector_start, ids + offset, count,
		          UnsafeNumericCast<row_t>(vector_start), true, written);
	}
	for (idx_t i = 0; i < column_ids.size(); i++) {
		auto column = column_ids[i];
		auto &col_data = GetColumn(column.index);
		D_ASSERT(col_data.type.id() == update_chunk.data[i].GetType().id());
		if (offset > 0) {
			Vector sliced_vector(update_chunk.data[i], offset, offset + count);
			sliced_vector.Flatten();
			col_data.Update(transaction, table_entry, column.index, sliced_vector, ids + offset, count,
			                row_group_start);
		} else {
			col_data.Update(transaction, table_entry, column.index, update_chunk.data[i], ids, count, row_group_start);
		}
		MergeStatistics(column.index, *col_data.GetUpdateStatistics());
	}
}

void RowGroup::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry, DataChunk &updates,
                            Vector &row_ids, idx_t offset, idx_t count, const vector<column_t> &column_path,
                            idx_t row_group_start) {
	D_ASSERT(updates.ColumnCount() == 1);
	auto ids = FlatVector::GetDataMutable<row_t>(row_ids);

	auto primary_column_idx = column_path[0];
	D_ASSERT(primary_column_idx < columns.size());
	auto &col_data = GetColumn(primary_column_idx);
	idx_t depth = 1;
	if (offset > 0) {
		Vector sliced_vector(updates.data[0], offset, offset + count);
		sliced_vector.Flatten();
		col_data.UpdateColumn(transaction, table_entry, column_path, sliced_vector, ids + offset, count, depth,
		                      row_group_start);
	} else {
		col_data.UpdateColumn(transaction, table_entry, column_path, updates.data[0], ids, count, depth,
		                      row_group_start);
	}
	MergeStatistics(primary_column_idx, *col_data.GetUpdateStatistics());
}

unique_ptr<BaseStatistics> RowGroup::GetStatistics(idx_t column_idx) const {
	StorageIndex storage_index(column_idx);
	return GetStatistics(storage_index);
}

unique_ptr<BaseStatistics> RowGroup::GetStatistics(const StorageIndex &column_idx) const {
	auto &col_data = GetColumn(column_idx);
	auto column_stats = col_data.GetStatistics();
	if (!column_idx.IsPushdownExtract()) {
		return column_stats;
	}
	return column_stats->PushdownExtract(column_idx.GetChildIndex(0));
}

void RowGroup::MergeStatistics(idx_t column_idx, const BaseStatistics &other) {
	auto &col_data = GetColumn(column_idx);
	col_data.MergeStatistics(other);
}

void RowGroup::MergeIntoStatistics(idx_t column_idx, BaseStatistics &other) {
	auto &col_data = GetColumn(column_idx);
	col_data.MergeIntoStatistics(other);
}

void RowGroup::MergeIntoStatistics(TableStatistics &other) {
	auto stats_lock = other.GetLock();
	for (idx_t i = 0; i < columns.size(); i++) {
		MergeIntoStatistics(i, other.GetStats(*stats_lock, i).Statistics());
	}
}

ColumnCheckpointInfo::ColumnCheckpointInfo(RowGroupWriteInfo &info, idx_t column_idx)
    : column_idx(column_idx), info(info) {
}

RowGroupWriteInfo::RowGroupWriteInfo(PartialBlockManager &manager, const vector<CompressionType> &compression_types,
                                     CheckpointOptions options_p)
    : manager(manager), compression_types(compression_types), options(options_p) {
}

RowGroupWriteInfo::RowGroupWriteInfo(PartialBlockManager &manager, const vector<CompressionType> &compression_types,
                                     vector<unique_ptr<PartialBlockManager>> &column_partial_block_managers_p)
    : manager(manager), compression_types(compression_types), options(),
      column_partial_block_managers(column_partial_block_managers_p) {
}

PartialBlockManager &RowGroupWriteInfo::GetPartialBlockManager(idx_t column_idx) {
	if (column_partial_block_managers && !column_partial_block_managers->empty()) {
		return *column_partial_block_managers->at(column_idx);
	}
	return manager;
}

PartialBlockManager &ColumnCheckpointInfo::GetPartialBlockManager() {
	return info.GetPartialBlockManager(column_idx);
}

CompressionType ColumnCheckpointInfo::GetCompressionType() {
	return info.compression_types[column_idx];
}

shared_ptr<ColumnData> RowGroup::CheckpointColumn(const RowGroup &row_group, idx_t column_idx, RowGroupWriteInfo &info,
                                                  RowGroupWriteData &write_data) {
	auto &column = row_group.GetColumn(column_idx);
	ColumnCheckpointInfo checkpoint_info(info, column_idx);
	auto checkpoint_state = column.Checkpoint(row_group, checkpoint_info);

	auto result_col = checkpoint_state->GetFinalResult();
	// FIXME: we should get rid of the checkpoint state statistics - and instead use the stats in the ColumnData
	// directly
	auto stats = checkpoint_state->GetStatistics();
	result_col->MergeStatistics(*stats);

	write_data.statistics.push_back(stats->Copy());
	write_data.states.push_back(std::move(checkpoint_state));
	return result_col;
}

vector<RowGroupWriteData> RowGroup::WriteToDisk(RowGroupWriteInfo &info,
                                                const vector<const_reference<RowGroup>> &row_groups) {
	vector<RowGroupWriteData> result;
	if (row_groups.empty()) {
		return result;
	}

	idx_t column_count = row_groups[0].get().GetColumnCount();
	for (idx_t row_group_idx = 0; row_group_idx < row_groups.size(); row_group_idx++) {
		D_ASSERT(column_count == row_groups[row_group_idx].get().GetColumnCount());
		RowGroupWriteData write_data;
		write_data.states.reserve(column_count);
		write_data.statistics.reserve(column_count);
		result.push_back(std::move(write_data));
	}

	// Checkpoint the row groups

	// In order to co-locate columns across different row groups, we write column-at-a-time
	// i.e. we first write column #0 of all row groups, then column #1, ...

	// Each column can have multiple segments.
	// (Some columns will be wider than others, and require different numbers
	// of blocks to encode.) Segments cannot span blocks.
	//
	// Some of these columns are composite (list, struct). The data is written
	// first sequentially, and the pointers are written later, so that the
	// pointers all end up densely packed, and thus more cache-friendly.
	vector<vector<shared_ptr<ColumnData>>> result_columns;
	result_columns.resize(row_groups.size());
	for (idx_t column_idx = 0; column_idx < column_count; column_idx++) {
		for (idx_t row_group_idx = 0; row_group_idx < row_groups.size(); row_group_idx++) {
			auto &row_group = row_groups[row_group_idx].get();
			result_columns[row_group_idx].emplace_back(
			    CheckpointColumn(row_group, column_idx, info, result[row_group_idx]));
		}
	}

	// create the row groups
	for (idx_t row_group_idx = 0; row_group_idx < row_groups.size(); row_group_idx++) {
		auto &row_group_write_data = result[row_group_idx];
		auto &row_group = row_groups[row_group_idx].get();
		auto result_row_group = make_shared_ptr<RowGroup>(row_group.GetCollection(), row_group.count);
		result_row_group->columns = std::move(result_columns[row_group_idx]);
		result_row_group->version_info = row_group.version_info;

		row_group_write_data.result_row_group = std::move(result_row_group);
	}

	return result;
}

RowGroupWriteData RowGroup::WriteToDisk(RowGroupWriteInfo &info) const {
	vector<const_reference<RowGroup>> row_groups;
	row_groups.push_back(*this);
	auto result = WriteToDisk(info, row_groups);
	return std::move(result[0]);
}

idx_t RowGroup::GetCommittedRowCount() {
	auto vinfo = GetVersionInfo();
	if (!vinfo) {
		return count;
	}
	ScanOptions options(TransactionData(0, VisibilityBound::AllCommitted()));
	options.insert_type = InsertedScanType::ALL_ROWS;
	options.delete_type = DeletedScanType::OMIT_COMMITTED_DELETES;
	return vinfo->GetRowCount(options, count);
}

idx_t RowGroup::GetVisibleRowCount(TransactionData transaction) {
	return GetVisibleRowCount(transaction, 0, count);
}

idx_t RowGroup::GetVisibleRowCount(TransactionData transaction, idx_t start_vector, idx_t scan_count) {
	D_ASSERT(start_vector * STANDARD_VECTOR_SIZE <= count);
	D_ASSERT(scan_count <= count - start_vector * STANDARD_VECTOR_SIZE);
	auto vinfo = GetVersionInfo();
	if (!vinfo) {
		return scan_count;
	}
	return vinfo->GetRowCount(transaction, start_vector, scan_count);
}

bool RowGroup::HasUnloadedDeletes() const {
	return version_info->HasUnloadedDeletes();
}

PerColumnMetadataBlocks RowGroup::ComputePerColumnMetadataBlocks() const {
	PerColumnMetadataBlocks result;
	if (column_pointers.empty()) {
		return result;
	}

	auto &metadata_manager = GetCollection().GetMetadataManager();
	auto &types = GetCollection().GetTypes();

	for (idx_t i = 0; i < column_pointers.size(); i++) {
		auto &start = column_pointers[i];
		vector<MetaBlockPointer> col_read_pointers;
		MetadataReader col_reader(metadata_manager, start, &col_read_pointers);
		ColumnData::Deserialize(GetBlockManager(), GetTableInfo(), i, col_reader, types[i]);
		vector<idx_t> extra_blocks;
		for (auto &ptr : col_read_pointers) {
			if (ptr.block_pointer != start.block_pointer) {
				extra_blocks.emplace_back(ptr.block_pointer);
			}
		}
		result.AddColumn(i, extra_blocks);
	}
	return result;
}

const vector<MetaBlockPointer> &RowGroup::GetColumnStartPointers() const {
	return column_pointers;
}

vector<MetaBlockPointer> RowGroup::GetExtraMetadataBlockPointers() const {
	vector<MetaBlockPointer> extra_metadata_block_pointers;
	if (has_per_column_metadata_blocks) {
		per_column_metadata_blocks.ForEachBlock(
		    [&](idx_t, idx_t block_id) { extra_metadata_block_pointers.emplace_back(block_id, 0); });
	} else {
		D_ASSERT(has_metadata_blocks);
		extra_metadata_block_pointers.reserve(extra_metadata_blocks.size());
		for (auto &block_pointer : extra_metadata_blocks) {
			extra_metadata_block_pointers.emplace_back(block_pointer, 0);
		}
	}
	return extra_metadata_block_pointers;
}

bool RowGroup::CanReuseMetadata(RowGroupWriter &writer) const {
	if (!Settings::Get<ExperimentalMetadataReuseSetting>(writer.GetDatabase())) {
		// disabled by configuration
		return false;
	}
	if (column_pointers.empty()) {
		// no existing metadata on disk - cannot re-use
		return false;
	}
	auto &table_writer = writer.GetTableWriter();
	if (table_writer.RequireLegacyStartRow() && table_writer.RowIdsChanged()) {
		// row-ids changed and we are targeting an old storage version that requires "start_row" - cannot re-use
		return false;
	}
	return true;
}

bool RowGroup::HasUnchangedColumns() const {
	for (idx_t c = 0; c < columns.size(); c++) {
		if (!ColumnIsLoaded(c)) {
			return true;
		}
		if (!columns[c]->HasAnyChanges()) {
			return true;
		}
	}
	return false;
}

RowGroupWriteData RowGroup::WriteToDisk(RowGroupWriter &writer) {
	bool can_reuse_metadata = CanReuseMetadata(writer);
	if (can_reuse_metadata && !HasChanges(writer.GetCheckpointOptions().visibility_bound)) {
		RowGroupWriteData result;
		result.write_action = RowGroupWriteAction::REUSE_EXISTING_ROW_GROUP_METADATA;
		if (GetCollection().SupportsPerColumnWrites()) {
			if (has_per_column_metadata_blocks) {
				return result;
			}
			per_column_metadata_blocks = ComputePerColumnMetadataBlocks();
			has_per_column_metadata_blocks = true;
			return result;
		}

		if (has_metadata_blocks) {
			return result;
		}

		if (!has_per_column_metadata_blocks) {
			auto meta_blocks = ComputePerColumnMetadataBlocks();

			vector<idx_t> computed_extra_metadata_blocks;
			meta_blocks.ForEachBlock(
			    [&](idx_t, idx_t block_id) { computed_extra_metadata_blocks.emplace_back(block_id); });
			extra_metadata_blocks = computed_extra_metadata_blocks;
			has_metadata_blocks = true;
			return result;
		}

		D_ASSERT(has_per_column_metadata_blocks && !GetCollection().SupportsPerColumnWrites());
		// we loaded column-level metadata from disk, but don't support writing it anymore, so we need to fall back to a
		// full checkpoint as we need to write out the metadata in a single go
	}

	// determine which columns can be reused
	bool partial_reuse =
	    can_reuse_metadata && has_per_column_metadata_blocks && GetCollection().SupportsPerColumnWrites();
	auto &compression_types = writer.GetCompressionTypes();
	RowGroupWriteData result;
	if (partial_reuse) {
		result.write_action = RowGroupWriteAction::PARTIALLY_REUSE_COLUMN_METADATA;
	} else {
		result.write_action = RowGroupWriteAction::FULLY_CHECKPOINT_ROW_GROUP;
	}

	auto result_row_group = make_shared_ptr<RowGroup>(GetCollection(), this->count);
	result_row_group->columns.resize(GetColumnCount());
	result_row_group->column_pointers.resize(GetColumnCount());
	// the rewritten row group shares the version info: deletes stay visible through both
	result_row_group->version_info = version_info;
	if (is_loaded) {
		result_row_group->is_loaded = unique_ptr<atomic<bool>[]>(new atomic<bool>[GetColumnCount()]);
		for (idx_t c = 0; c < GetColumnCount(); c++) {
			result_row_group->is_loaded[c] = true;
		}
	}

	RowGroupWriteInfo info(writer.GetPartialBlockManager(), compression_types, writer.GetCheckpointOptions());

	vector<idx_t> reused_columns;
	for (idx_t column_idx = 0; column_idx < GetColumnCount(); column_idx++) {
		bool column_has_changes = true;
		if (partial_reuse) {
			if (!ColumnIsLoaded(column_idx)) {
				column_has_changes = false;
			} else if (!columns[column_idx]->HasAnyChanges()) {
				column_has_changes = false;
			}
		}

		if (!column_has_changes) {
			// reuse this column's metadata
			result.states.push_back(nullptr);
			reused_columns.emplace_back(column_idx);
			result_row_group->column_pointers[column_idx] = column_pointers[column_idx];
			// carry forward existing column data and statistics
			if (!ColumnIsLoaded(column_idx)) {
				result_row_group->columns[column_idx] = nullptr;
				result_row_group->is_loaded[column_idx] = false;
				// column not loaded - stats will be merged from previous table stats during Checkpoint
				result.statistics.push_back(BaseStatistics::CreateEmpty(GetCollection().GetTypes()[column_idx]));
			} else {
				result_row_group->columns[column_idx] = columns[column_idx];
				auto col_stats = columns[column_idx]->GetStatistics();
				result.statistics.push_back(col_stats
				                                ? std::move(*col_stats)
				                                : BaseStatistics::CreateEmpty(GetCollection().GetTypes()[column_idx]));
			}
		} else {
			// checkpoint this column
			auto &column = GetColumn(column_idx);
			if (column.count != this->count) {
				throw InternalException("Corrupted in-memory column - column with index %llu has misaligned count "
				                        "(row group has %llu rows, column has %llu)",
				                        column_idx, this->count.load(), column.count.load());
			}
			result_row_group->columns[column_idx] = CheckpointColumn(*this, column_idx, info, result);
		}
	}

	if (partial_reuse) {
		// carry forward the extras for reused columns onto the new row group, so RowGroup::Checkpoint
		// can look them up via this->per_column_metadata_blocks
		auto extras = per_column_metadata_blocks.GetBlocksForColumns(reused_columns);
		for (idx_t i = 0; i < reused_columns.size(); i++) {
			result_row_group->per_column_metadata_blocks.AddColumn(reused_columns[i], extras[i]);
		}
		result_row_group->has_per_column_metadata_blocks = true;
	}

	result.result_row_group = std::move(result_row_group);
	return result;
}

void IncrementSegmentStart(PersistentColumnData &data, idx_t start_increment) {
	for (auto &pointer : data.pointers) {
		pointer.row_start += start_increment;
	}
	for (auto &child_column : data.child_columns) {
		IncrementSegmentStart(child_column, start_increment);
	}
}

RowGroupPointer RowGroup::Checkpoint(RowGroupWriteData write_data, RowGroupWriter &writer,
                                     TableStatistics &global_stats, idx_t row_group_start) {
	RowGroupPointer row_group_pointer;

	auto metadata_manager = writer.GetMetadataManager();
	// construct the row group pointer and write the column meta data to disk
	row_group_pointer.row_start = row_group_start;
	row_group_pointer.tuple_count = count;
	if (write_data.write_action == RowGroupWriteAction::REUSE_EXISTING_ROW_GROUP_METADATA) {
		// we are re-using the previous metadata
		row_group_pointer.data_pointers = column_pointers;
		row_group_pointer.has_metadata_blocks = has_metadata_blocks;
		row_group_pointer.extra_metadata_blocks = extra_metadata_blocks;
		row_group_pointer.has_per_column_metadata_blocks = has_per_column_metadata_blocks;
		row_group_pointer.per_column_metadata_blocks = per_column_metadata_blocks;
		if (metadata_manager) {
			row_group_pointer.deletes_pointers = CheckpointDeletes(writer);

			vector<MetaBlockPointer> metadata_block_pointers_to_be_cleared;
			if (has_per_column_metadata_blocks) {
				per_column_metadata_blocks.ForEachBlock(
				    [&](idx_t, idx_t block_id) { metadata_block_pointers_to_be_cleared.emplace_back(block_id, 0); });
			} else {
				metadata_block_pointers_to_be_cleared.reserve(extra_metadata_blocks.size());
				for (auto &block_pointer : extra_metadata_blocks) {
					metadata_block_pointers_to_be_cleared.emplace_back(block_pointer, 0);
				}
			}
			metadata_manager->ClearModifiedBlocks(column_pointers);
			metadata_manager->ClearModifiedBlocks(metadata_block_pointers_to_be_cleared);
		}
		// merge row group stats into the global stats
		auto lock = global_stats.GetLock();
		for (idx_t column_idx = 0; column_idx < GetColumnCount(); column_idx++) {
			if (!ColumnIsLoaded(column_idx) && collection.get().GetTypes()[column_idx].id() != LogicalTypeId::VARIANT) {
				// column is not loaded from disk - don't load just to update stats
				writer.SetHasUnloadedColumn(column_idx);
				continue;
			}
			GetColumn(column_idx).MergeIntoStatistics(global_stats.GetStats(*lock, column_idx).Statistics());
		}
		return row_group_pointer;
	}
	// write path: write column metadata to disk (with optional per-column reuse)
	D_ASSERT(write_data.states.size() == GetColumnCount());

	// merge stats
	{
		auto lock = global_stats.GetLock();
		for (idx_t column_idx = 0; column_idx < GetColumnCount(); column_idx++) {
			bool is_reused = !write_data.states[column_idx];
			if (is_reused) {
				if (!ColumnIsLoaded(column_idx) &&
				    collection.get().GetTypes()[column_idx].id() != LogicalTypeId::VARIANT) {
					writer.SetHasUnloadedColumn(column_idx);
					continue;
				}
				GetColumn(column_idx).MergeIntoStatistics(global_stats.GetStats(*lock, column_idx).Statistics());
			} else {
				global_stats.GetStats(*lock, column_idx).Statistics().Merge(write_data.statistics[column_idx]);
			}
		}
	}

	auto serialization_options = SerializationOptions(writer.GetAttachedDatabase());

	// collect blocks that need to be preserved for reused columns
	vector<MetaBlockPointer> reused_column_blocks;
	// per-column extras for newly written columns, collected separately and merged with the
	// partial map of reused-column extras (carried over onto this row group by WriteToDisk).
	PerColumnMetadataBlocks written_column_blocks;

	for (idx_t column_idx = 0; column_idx < GetColumnCount(); column_idx++) {
		bool is_reused = !write_data.states[column_idx];
		if (is_reused) {
			// reuse existing column pointer (extras are already carried on this->per_column_metadata_blocks)
			auto col_ptr = column_pointers[column_idx];
			row_group_pointer.data_pointers.push_back(col_ptr);
			reused_column_blocks.push_back(col_ptr);
			continue;
		}
		// write new metadata for this column
		auto &state = write_data.states[column_idx];
		D_ASSERT(state);

		// track blocks written for this column
		vector<MetaBlockPointer> col_written_blocks;
		writer.StartWritingColumns(col_written_blocks);

		auto &data_writer = writer.GetPayloadWriter();
		auto pointer = writer.GetMetaBlockPointer();

		// store the stats and the data pointers in the row group pointers
		row_group_pointer.data_pointers.push_back(pointer);

		// Write pointers to the column segments.
		//
		// Just as above, the state can refer to many other states, so this
		// can cascade recursively into more pointer writes.
		auto persistent_data = state->ToPersistentData();
		// increment the "start" in all data pointers by the row group start
		// FIXME: this is only necessary when targeting old serialization
		IncrementSegmentStart(persistent_data, row_group_start);

		BinarySerializer serializer(data_writer, serialization_options);
		serializer.Begin();
		persistent_data.Serialize(serializer);
		serializer.End();

		writer.FinishWritingColumns();

		// collect per-column extra blocks (excluding the start block)
		vector<idx_t> col_extra_blocks;
		for (auto &written_ptr : col_written_blocks) {
			if (written_ptr.block_pointer != pointer.block_pointer) {
				col_extra_blocks.push_back(written_ptr.block_pointer);
			}
		}
		written_column_blocks.AddColumn(column_idx, col_extra_blocks);
	}

	if (GetCollection().SupportsPerColumnWrites()) {
		row_group_pointer.has_per_column_metadata_blocks = true;
		if (write_data.write_action == RowGroupWriteAction::PARTIALLY_REUSE_COLUMN_METADATA) {
			// merge reused-column extras (on this) with newly-written-column extras
			D_ASSERT(has_per_column_metadata_blocks);
			row_group_pointer.per_column_metadata_blocks =
			    PerColumnMetadataBlocks::Merge(per_column_metadata_blocks, written_column_blocks);

			// reused column blocks must be preserved by ClearModifiedBlocks
			per_column_metadata_blocks.ForEachBlock(
			    [&](idx_t, idx_t block_id) { reused_column_blocks.emplace_back(block_id, 0); });
		} else {
			row_group_pointer.per_column_metadata_blocks = written_column_blocks;
		}
		row_group_pointer.has_metadata_blocks = false;
		row_group_pointer.extra_metadata_blocks.clear();
	} else {
		// Per-column reuse is not supported, so instead flatten the newly-written per-column extras into
		// extra_metadata_blocks to still allow reusing the full row-group metadata on future checkpoints.
		D_ASSERT(write_data.write_action != RowGroupWriteAction::PARTIALLY_REUSE_COLUMN_METADATA);
		row_group_pointer.has_per_column_metadata_blocks = false;
		row_group_pointer.per_column_metadata_blocks = {};
		row_group_pointer.has_metadata_blocks = true;
		row_group_pointer.extra_metadata_blocks.clear();
		written_column_blocks.ForEachBlock(
		    [&](idx_t, idx_t block_id) { row_group_pointer.extra_metadata_blocks.push_back(block_id); });
	}

	if (metadata_manager) {
		row_group_pointer.deletes_pointers = CheckpointDeletes(writer);
		metadata_manager->ClearModifiedBlocks(reused_column_blocks);
	}

	// cache metadata pointers for future checkpoint reuse
	column_pointers = row_group_pointer.data_pointers;
	has_metadata_blocks = row_group_pointer.has_metadata_blocks;
	extra_metadata_blocks = row_group_pointer.extra_metadata_blocks;
	has_per_column_metadata_blocks = row_group_pointer.has_per_column_metadata_blocks;
	per_column_metadata_blocks = row_group_pointer.per_column_metadata_blocks;

	Verify();
	return row_group_pointer;
}

bool RowGroup::HasChanges(VisibilityBound bound) const {
	if (has_changes) {
		return true;
	}
	auto version_info_loaded = version_info->GetIfLoaded();
	if (version_info_loaded && version_info_loaded->HasUnserializedChanges(bound)) {
		// we have deletes
		return true;
	}
	// check if any of the columns have changes
	// avoid loading unloaded columns - unloaded columns can never have changes
	for (idx_t c = 0; c < columns.size(); c++) {
		if (!ColumnIsLoaded(c)) {
			continue;
		}
		if (columns[c]->HasAnyChanges()) {
			return true;
		}
	}
	return false;
}

bool RowGroup::IsPersistent() const {
	for (auto &column : columns) {
		if (!column->IsPersistent()) {
			// column is not persistent
			return false;
		}
	}
	return true;
}

PersistentRowGroupData RowGroup::SerializeRowGroupInfo(idx_t row_group_start) const {
	// all columns are persistent - serialize
	PersistentRowGroupData result;
	for (auto &col : columns) {
		result.column_data.push_back(col->Serialize());
	}
	result.start = row_group_start;
	result.count = count;
	return result;
}

void RowGroup::CompressVersionInfo(VisibilityBound lowest_visibility_bound) {
	if (HasUnloadedDeletes()) {
		// deletes were not loaded - they are still stored in their compact serialized form
		return;
	}
	auto vinfo = GetVersionInfo();
	if (!vinfo) {
		return;
	}
	vinfo->CompressVersionIds(lowest_visibility_bound);
}

vector<MetaBlockPointer> RowGroup::CheckpointDeletes(RowGroupWriter &writer) {
	if (HasUnloadedDeletes()) {
		// deletes were not loaded so they cannot be changed
		// re-use them as-is
		auto &manager = *writer.GetMetadataManager();
		auto &deletes_pointers = version_info->GetDeletesPointers();
		manager.ClearModifiedBlocks(deletes_pointers);
		return deletes_pointers;
	}
	auto vinfo = GetVersionInfo();
	if (!vinfo) {
		// no version information: write nothing
		return vector<MetaBlockPointer>();
	}
	return vinfo->Checkpoint(writer);
}

void RowGroup::Serialize(RowGroupPointer &pointer, Serializer &serializer, bool supports_per_column_writes) {
	serializer.WriteProperty(100, "row_start", pointer.row_start);
	serializer.WriteProperty(101, "tuple_count", pointer.tuple_count);
	serializer.WriteProperty(102, "data_pointers", pointer.data_pointers);
	serializer.WriteProperty(103, "delete_pointers", pointer.deletes_pointers);
	if (serializer.ShouldSerialize(StorageVersion::V1_4_0) && !supports_per_column_writes) {
		serializer.WriteProperty(104, "has_metadata_blocks", pointer.has_metadata_blocks);
		serializer.WritePropertyWithDefault(105, "extra_metadata_blocks", pointer.extra_metadata_blocks);
	}
	if (supports_per_column_writes) {
		D_ASSERT(serializer.ShouldSerialize(StorageVersion::V1_4_0));
		// also write legacy metadata blocks for v1.4 and v1.5
		serializer.WriteProperty(104, "has_metadata_blocks", pointer.has_per_column_metadata_blocks);
		vector<idx_t> extra_metadata_block_ids;
		pointer.per_column_metadata_blocks.ForEachBlock(
		    [&](idx_t, idx_t block_id) { extra_metadata_block_ids.push_back(block_id); });
		serializer.WritePropertyWithDefault(105, "extra_metadata_blocks", extra_metadata_block_ids);
		serializer.WriteProperty(106, "has_per_column_metadata_blocks", pointer.has_per_column_metadata_blocks);
		serializer.WritePropertyWithDefault(107, "per_column_metadata_blocks", pointer.per_column_metadata_blocks.data);
	}
}

RowGroupPointer RowGroup::Deserialize(Deserializer &deserializer) {
	RowGroupPointer result;
	result.row_start = deserializer.ReadProperty<uint64_t>(100, "row_start");
	result.tuple_count = deserializer.ReadProperty<uint64_t>(101, "tuple_count");
	result.data_pointers = deserializer.ReadProperty<vector<MetaBlockPointer>>(102, "data_pointers");
	result.deletes_pointers = deserializer.ReadProperty<vector<MetaBlockPointer>>(103, "delete_pointers");
	result.has_metadata_blocks = deserializer.ReadPropertyWithExplicitDefault<bool>(104, "has_metadata_blocks", false);
	result.extra_metadata_blocks = deserializer.ReadPropertyWithDefault<vector<idx_t>>(105, "extra_metadata_blocks");
	result.has_per_column_metadata_blocks =
	    deserializer.ReadPropertyWithExplicitDefault<bool>(106, "has_per_column_metadata_blocks", false);
	result.per_column_metadata_blocks = {
	    deserializer.ReadPropertyWithDefault<vector<PerColumnMetadataBlock>>(107, "per_column_metadata_blocks")};
	if (result.has_per_column_metadata_blocks) {
		// per-column metadata supersedes legacy extra_metadata_blocks
		result.has_metadata_blocks = false;
		result.extra_metadata_blocks.clear();
	}
	return result;
}

//===--------------------------------------------------------------------===//
// GetPartitionStats
//===--------------------------------------------------------------------===//
struct DuckDBPartitionRowGroup : public PartitionRowGroup {
	explicit DuckDBPartitionRowGroup(shared_ptr<RowGroup> row_group_p, bool is_exact_p)
	    : row_group(std::move(row_group_p)), is_exact(is_exact_p) {
	}

	shared_ptr<RowGroup> row_group;
	const bool is_exact;

	unique_ptr<BaseStatistics> GetColumnStatistics(const StorageIndex &storage_index) override {
		return row_group->GetStatistics(storage_index);
	}

	bool MinMaxIsExact(const StorageIndex &) override {
		return is_exact;
	}

	bool HasPendingWrites() override {
		return row_group->HasChanges(VisibilityBound::AllCommitted());
	}
};

PartitionStatistics RowGroup::GetPartitionStats(SegmentNode<RowGroup> &row_group, TransactionData transaction) {
	auto &row_group_ref = row_group.GetNode();

	PartitionStatistics result;
	result.row_start = row_group.GetRowStart();
	if (row_group_ref.HasUnloadedDeletes()) {
		result.count = row_group_ref.count;
		result.count_type = CountType::COUNT_APPROXIMATE;
		result.partition_row_group =
		    make_shared_ptr<DuckDBPartitionRowGroup>(row_group.ReferenceNode(), /*is_exact_p=*/false);
		return result;
	}

	auto vinfo = row_group_ref.GetVersionInfoIfLoaded();
	bool has_uncommitted_changes = false;
	if (vinfo) {
		result.count = row_group_ref.GetVisibleRowCount(transaction);
		has_uncommitted_changes = vinfo->HasUncommittedChanges();
	} else {
		result.count = row_group_ref.count;
	}
	result.count_type = has_uncommitted_changes ? CountType::COUNT_APPROXIMATE : CountType::COUNT_EXACT;
	const bool is_exact = result.count == row_group_ref.count && (!vinfo || !vinfo->HasDeletes());
	result.partition_row_group = make_shared_ptr<DuckDBPartitionRowGroup>(row_group.ReferenceNode(), is_exact);

	return result;
}

//===--------------------------------------------------------------------===//
// GetColumnSegmentInfo
//===--------------------------------------------------------------------===//
void RowGroup::GetColumnSegmentInfo(const QueryContext &context, idx_t row_group_index,
                                    vector<ColumnSegmentInfo> &result, const ColumnSegmentInfoScanOptions &options) {
	for (idx_t col_idx = 0; col_idx < GetColumnCount(); col_idx++) {
		if (options.loaded_segments_only && !ColumnIsLoaded(col_idx)) {
			// column is not loaded - skip it
			continue;
		}
		auto &col_data = GetColumn(col_idx);
		col_data.GetColumnSegmentInfo(context, row_group_index, {col_idx}, result, options);
	}
}

//===--------------------------------------------------------------------===//
// Version Delete Information
//===--------------------------------------------------------------------===//
class VersionDeleteState {
public:
	VersionDeleteState(RowGroup &info, TransactionData transaction, DuckTableEntry &table_entry, idx_t base_row,
	                   optional_ptr<DeletedRowIds> deleted_ids, idx_t input_offset)
	    : info(info), transaction(transaction), table_entry(table_entry), current_chunk(DConstants::INVALID_INDEX),
	      count(0), base_row(base_row), delete_count(0), deleted_ids(deleted_ids), input_offset(input_offset) {
	}

	RowGroup &info;
	TransactionData transaction;
	DuckTableEntry &table_entry;
	idx_t current_chunk;
	row_t rows[STANDARD_VECTOR_SIZE];
	idx_t count;
	idx_t base_row;
	idx_t chunk_row;
	idx_t delete_count;
	optional_ptr<DeletedRowIds> deleted_ids;
	//! The position of the next row to flush in the input row ids of the delete
	idx_t input_offset;

public:
	void Delete(row_t row_id);
	void Flush();
};

idx_t RowGroup::Delete(TransactionData transaction, DuckTableEntry &table_entry, row_t *ids, idx_t count,
                       idx_t row_group_start, optional_ptr<DeletedRowIds> deleted_ids, idx_t input_offset) {
	VersionDeleteState del_state(*this, transaction, table_entry, row_group_start, deleted_ids, input_offset);

	// obtain a write lock
	for (idx_t i = 0; i < count; i++) {
		D_ASSERT(ids[i] >= 0);
		D_ASSERT(idx_t(ids[i]) >= row_group_start && idx_t(ids[i]) < row_group_start + this->count);
		del_state.Delete(ids[i] - UnsafeNumericCast<row_t>(row_group_start));
	}
	del_state.Flush();
	return del_state.delete_count;
}

void RowGroup::Verify() {
#ifdef DEBUG
	for (idx_t c = 0; c < columns.size(); c++) {
		if (!ColumnIsLoaded(c)) {
			continue;
		}
		if (columns[c]) {
			columns[c]->Verify(*this);
		}
	}
	lock_guard<mutex> guard(row_group_lock);
	if (row_id_is_loaded) {
		D_ASSERT(row_id_column_data->count == count);
	}
	if (row_number_is_loaded) {
		D_ASSERT(row_number_column_data->count == count);
	}
#endif
}

void RowGroup::WriteRows(TransactionData transaction, DuckTableEntry &table_entry, idx_t vector_idx, idx_t base_row,
                         const row_t ids[], idx_t count, row_t offset, bool is_update, WrittenRows &written) {
	auto &version_info = GetOrCreateVersionInfo();
	version_info.WriteRows(vector_idx, transaction, ids, count, offset, is_update, written);
	if (transaction.transaction && written.count > 0) {
		try {
			transaction.transaction->PushWrite(is_update ? UndoFlags::UPDATE_ROWS : UndoFlags::DELETE_TUPLE,
			                                   table_entry, version_info, vector_idx, base_row, written);
		} catch (...) {
			try {
				// without an undo entry, nothing would restore the ids - restore them as the entry would have
				version_info.RollbackWrite(vector_idx, written.GetStoredRows(), written.count, written.previous_ids,
				                           written.previous_id_count);
			} catch (std::exception &ex) {
				throw FatalException("Failed to restore the version info after failing to push its undo entry: %s",
				                     ErrorData(ex).Message());
			}
			throw;
		}
	}
}

void VersionDeleteState::Delete(row_t row_id) {
	D_ASSERT(row_id >= 0);
	idx_t vector_idx = UnsafeNumericCast<idx_t>(row_id) / STANDARD_VECTOR_SIZE;
	idx_t idx_in_vector = UnsafeNumericCast<idx_t>(row_id) - vector_idx * STANDARD_VECTOR_SIZE;
	if (current_chunk != vector_idx) {
		Flush();

		current_chunk = vector_idx;
		chunk_row = vector_idx * STANDARD_VECTOR_SIZE;
	}
	rows[count++] = UnsafeNumericCast<row_t>(idx_in_vector);
}

void VersionDeleteState::Flush() {
	if (count == 0) {
		return;
	}
	// it is possible for delete statements to delete the same tuple multiple times when combined with a USING clause
	// WriteRows skips the tuples that are deleted already, and outputs the tuples it actually deleted
	WrittenRows deleted;
	info.WriteRows(transaction, table_entry, current_chunk, base_row + chunk_row, rows, count, 0, false, deleted);
	auto actual_delete_count = deleted.count;
	delete_count += actual_delete_count;
	if (deleted_ids) {
		if (!deleted_ids->has_skipped_rows && actual_delete_count != count) {
			// the first skipped rows - all rows flushed before were deleted, they are the preceding input rows
			deleted_ids->has_skipped_rows = true;
			deleted_ids->row_ids.assign(deleted_ids->input_ids, deleted_ids->input_ids + input_offset);
		}
		if (deleted_ids->has_skipped_rows) {
			for (idx_t i = 0; i < actual_delete_count; i++) {
				deleted_ids->row_ids.push_back(UnsafeNumericCast<row_t>(base_row + chunk_row + deleted.rows[i]));
			}
		}
		input_offset += count;
	}
	count = 0;
}

} // namespace duckdb
