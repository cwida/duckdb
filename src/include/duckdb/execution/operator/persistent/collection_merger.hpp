//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/persistent/collection_merger.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/optimistic_data_writer.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/transaction/duck_transaction.hpp"

namespace duckdb {

enum class RowGroupBatchType : uint8_t { FLUSHED, NOT_FLUSHED };

class CollectionMerger {
public:
	explicit CollectionMerger(ClientContext &context, DataTable &data_table)
	    : context(context), data_table(data_table), batch_type(RowGroupBatchType::NOT_FLUSHED) {
	}

	//! The transaction context.
	ClientContext &context;
	//! The data table.
	DataTable &data_table;
	//! Indexes to the optimistic row group collection vector of the local table storage for this transaction.
	vector<PhysicalIndex> collection_indexes;
	//! The batch type for merging collections.
	RowGroupBatchType batch_type;

public:
	void AddCollection(const PhysicalIndex collection_index, RowGroupBatchType type) {
		collection_indexes.push_back(collection_index);
		if (type == RowGroupBatchType::FLUSHED) {
			batch_type = RowGroupBatchType::FLUSHED;
			if (collection_indexes.size() > 1) {
				throw InternalException("Cannot merge flushed collections");
			}
		}
	}

	bool Empty() {
		return collection_indexes.empty();
	}

	PhysicalIndex Flush(OptimisticDataWriter &writer) {
		if (Empty()) {
			return PhysicalIndex(DConstants::INVALID_INDEX);
		}

		auto result_collection_index = collection_indexes[0];
		auto &optimistic_collection = data_table.GetOptimisticCollection(context, result_collection_index);
		auto &result_collection = *optimistic_collection.collection;

		if (collection_indexes.size() > 1) {
			// Merge all collections into one result collection.
			auto &types = result_collection.GetTypes();
			TableAppendState append_state;
			result_collection.InitializeAppend(append_state);

			DataChunk scan_chunk;
			scan_chunk.Initialize(context, types);

			vector<StorageIndex> column_ids;
			for (idx_t i = 0; i < types.size(); i++) {
				column_ids.emplace_back(i);
			}
			for (idx_t i = 1; i < collection_indexes.size(); i++) {
				auto &collection = data_table.GetOptimisticCollection(context, collection_indexes[i]);
				TableScanState scan_state;
				scan_state.Initialize(column_ids);
				collection.collection->InitializeScan(context, scan_state.local_state, column_ids, nullptr);

				while (true) {
					scan_chunk.Reset();
					scan_state.local_state.Scan(scan_chunk, TableScanType::TABLE_SCAN_ALL_ROWS);
					if (scan_chunk.size() == 0) {
						break;
					}
					auto flushed_row_group_idx = result_collection.Append(scan_chunk, append_state);
					if (flushed_row_group_idx.IsValid()) {
						writer.WriteNewRowGroup(optimistic_collection, flushed_row_group_idx.GetIndex());
					}
				}
				data_table.ResetOptimisticCollection(context, collection_indexes[i]);
			}
			result_collection.FinalizeAppend(DuckTransaction::Get(context, data_table.db), append_state);
			writer.WriteUnflushedRowGroups(optimistic_collection);
		} else if (batch_type == RowGroupBatchType::NOT_FLUSHED) {
			writer.WriteUnflushedRowGroups(optimistic_collection);
		}

		collection_indexes.clear();
		return result_collection_index;
	}
};

} // namespace duckdb
