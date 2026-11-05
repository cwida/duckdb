#include "duckdb/main/retained_result_collection.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/types/batched_data_collection.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

RetainedResultCollection::~RetainedResultCollection() {
}

//===--------------------------------------------------------------------===//
// ChunkRetainedCollection
//===--------------------------------------------------------------------===//
ChunkRetainedCollection::ChunkRetainedCollection(ClientContext &context, const vector<LogicalType> &types,
                                                 QueryResultMemoryType memory_type, bool batch_ordered_p)
    : batch_ordered(batch_ordered_p) {
	switch (memory_type) {
	case QueryResultMemoryType::IN_MEMORY:
		if (batch_ordered) {
			batched = make_uniq<BatchedDataCollection>(context, types);
		} else {
			collection = make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator(), types);
		}
		break;
	case QueryResultMemoryType::BUFFER_MANAGED:
		// The database's buffer manager, because the result can outlive the ClientContext
		if (batch_ordered) {
			batched =
			    make_uniq<BatchedDataCollection>(context, types, ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR,
			                                     ColumnDataCollectionLifetime::THROW_ERROR_AFTER_DATABASE_CLOSES);
		} else {
			collection =
			    make_uniq<ColumnDataCollection>(BufferManager::GetBufferManager(*context.db), types,
			                                    ColumnDataCollectionLifetime::THROW_ERROR_AFTER_DATABASE_CLOSES);
		}
		break;
	default:
		throw NotImplementedException("ChunkRetainedCollection for %s", EnumUtil::ToString(memory_type));
	}
}

ChunkRetainedCollection::ChunkRetainedCollection(unique_ptr<ColumnDataCollection> collection_p)
    : batch_ordered(false), collection(std::move(collection_p)) {
}

ChunkRetainedCollection::~ChunkRetainedCollection() {
}

void ChunkRetainedCollection::Append(DataChunk &chunk, idx_t batch_index) {
	if (batch_ordered) {
		batched->Append(chunk, batch_index);
		return;
	}
	if (!append_initialized) {
		// InitializeAppend adds an empty chunk, which a scan of a combined-only collection would read as its end
		collection->InitializeAppend(append_state);
		append_initialized = true;
	}
	collection->Append(append_state, chunk);
}

void ChunkRetainedCollection::Combine(RetainedResultCollection &local_p) {
	auto &local = local_p.Cast<ChunkRetainedCollection>();
	if (batch_ordered) {
		batched->Merge(*local.batched);
		return;
	}
	collection->Combine(*local.collection);
}

void ChunkRetainedCollection::Finalize() {
	if (batch_ordered) {
		collection = batched->FetchCollection();
		batched.reset();
	}
}

idx_t ChunkRetainedCollection::Count() const {
	if (batch_ordered) {
		return batched ? batched->Count() : collection->Count();
	}
	return collection->Count();
}

ColumnDataCollection &ChunkRetainedCollection::Get() {
	D_ASSERT(collection);
	return *collection;
}

unique_ptr<ColumnDataCollection> ChunkRetainedCollection::Take() {
	D_ASSERT(collection);
	return std::move(collection);
}

unique_ptr<DataChunk> ChunkRetainedCollection::FetchRaw() {
	D_ASSERT(collection);
	if (scan_exhausted) {
		return nullptr;
	}
	auto result = make_uniq<DataChunk>();
	collection->InitializeScanChunk(*result);
	if (!scan_initialized) {
		// Disallow zero copy so the chunk is independently usable even after the collection is destroyed
		collection->InitializeScan(scan_state, ColumnDataScanProperties::DISALLOW_ZERO_COPY);
		scan_initialized = true;
	}
	collection->Scan(scan_state, *result);
	if (result->size() == 0) {
		scan_exhausted = true;
		return nullptr;
	}
	return result;
}

} // namespace duckdb
