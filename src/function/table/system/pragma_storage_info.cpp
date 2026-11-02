#include "duckdb/function/table/system_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/qualified_name.hpp"

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/execution/partition_info.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "duckdb/storage/table/row_group_segment_tree.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/common/enums/column_segment_info_scan_type.hpp"

namespace duckdb {

struct PragmaStorageFunctionData : public TableFunctionData {
	PragmaStorageFunctionData(TableCatalogEntry &table_entry, ColumnSegmentInfoScanOptions options)
	    : table_entry(table_entry), options(options) {
	}

	TableCatalogEntry &table_entry;
	ColumnSegmentInfoScanOptions options;
};

struct PragmaStorageGlobalState : public GlobalTableFunctionState {
	explicit PragmaStorageGlobalState(idx_t max_threads_p) : max_threads(max_threads_p) {
	}

	mutex lock;
	idx_t max_threads;
	ColumnSegmentInfoScanState scan_state;
	//! The total number of row groups and the number of row groups that were scanned (for progress)
	idx_t total_row_groups = 0;
	atomic<idx_t> scanned_row_groups {0};

	idx_t MaxThreads() const override {
		return max_threads;
	}
};

struct PragmaStorageLocalState : public LocalTableFunctionState {
	//! Buffered column segment info for the row group this thread is currently emitting.
	vector<ColumnSegmentInfo> buffer;
	idx_t buffer_offset = 0;
	//! Index of the row group currently being emitted; used as the batch index for partitioning.
	idx_t batch_index = 0;
};

static unique_ptr<FunctionData> PragmaStorageInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("row_group_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("column_name");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("column_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("column_path");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("segment_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("segment_type");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("start");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("count");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("compression");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("stats");
	return_types.emplace_back(LogicalType::VARIANT());

	names.emplace_back("has_updates");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("persistent");
	return_types.emplace_back(LogicalType::BOOLEAN);

	names.emplace_back("block_id");
	return_types.emplace_back(LogicalType::BIGINT);

	names.emplace_back("block_offset");
	return_types.emplace_back(LogicalType::BIGINT);

	ColumnSegmentInfoScanOptions options;
	auto include_entry = input.named_parameters.find("include_segment_info");
	if (include_entry != input.named_parameters.end()) {
		if (include_entry->second.IsNull()) {
			throw InvalidInputException("Cannot use NULL as argument for include_segment_info");
		}
		options.include_segment_info = include_entry->second.GetValue<bool>();
	}
	auto loaded_only_entry = input.named_parameters.find("loaded_segments_only");
	if (loaded_only_entry != input.named_parameters.end()) {
		if (loaded_only_entry->second.IsNull()) {
			throw InvalidInputException("Cannot use NULL as argument for loaded_segments_only");
		}
		options.loaded_segments_only = loaded_only_entry->second.GetValue<bool>();
	}

	// segment_info is only emitted when the (expensive) per-segment info was actually collected.
	if (options.include_segment_info) {
		names.emplace_back("segment_info");
		return_types.emplace_back(LogicalType::VARCHAR);
	}

	names.emplace_back("additional_block_ids");
	return_types.emplace_back(LogicalType::LIST(LogicalTypeId::BIGINT));

	QualifiedName qname;
	string error;
	if (!QualifiedName::TryParse(input.inputs[0].GetValue<string>(), qname, error)) {
		throw ParserException(error);
	}

	// look up the table name in the catalog
	CatalogEntryRetriever retriever(context);
	qname = Binder::BindTableName(retriever, qname);
	auto &table_entry = Catalog::GetEntry<TableCatalogEntry>(context, qname);
	Binder::RegisterEntryRead(input.binder, context, table_entry);
	return make_uniq<PragmaStorageFunctionData>(table_entry, options);
}

unique_ptr<GlobalTableFunctionState> PragmaStorageInfoInitGlobal(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<PragmaStorageFunctionData>();
	auto max_threads = TaskScheduler::GetScheduler(context).NumberOfThreads();
	auto gstate = make_uniq<PragmaStorageGlobalState>(max_threads);
	gstate->scan_state.options = bind_data.options;
	bind_data.table_entry.InitializeColumnSegmentInfoScan(gstate->scan_state);
	if (gstate->scan_state.row_groups) {
		for (auto &row_group : gstate->scan_state.row_groups->SegmentNodes()) {
			(void)row_group;
			gstate->total_row_groups++;
		}
	}
	return std::move(gstate);
}

unique_ptr<LocalTableFunctionState> PragmaStorageInfoInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                               GlobalTableFunctionState *global_state) {
	return make_uniq<PragmaStorageLocalState>();
}

static Value ValueFromBlockIdList(const vector<block_id_t> &block_ids) {
	vector<Value> blocks;
	for (auto &block_id : block_ids) {
		blocks.push_back(Value::BIGINT(block_id));
	}
	return Value::LIST(LogicalTypeId::BIGINT, blocks);
}

static void PragmaStorageInfoFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<PragmaStorageFunctionData>();
	auto &gstate = data_p.global_state->Cast<PragmaStorageGlobalState>();
	auto &lstate = data_p.local_state->Cast<PragmaStorageLocalState>();
	auto &columns = bind_data.table_entry.GetColumns();
	QueryContext query_context(context);

	idx_t count = 0;

	idx_t col_idx = 0;
	auto &row_group_id = output.data[col_idx++];
	auto &column_name = output.data[col_idx++];
	auto &column_id = output.data[col_idx++];
	auto &column_path = output.data[col_idx++];
	auto &segment_id = output.data[col_idx++];
	auto &segment_type = output.data[col_idx++];
	auto &start = output.data[col_idx++];
	auto &count_col = output.data[col_idx++];
	auto &compression = output.data[col_idx++];
	auto &stats = output.data[col_idx++];
	auto &has_updates = output.data[col_idx++];
	auto &persistent = output.data[col_idx++];
	auto &block_id = output.data[col_idx++];
	auto &block_offset = output.data[col_idx++];
	optional_ptr<Vector> segment_info = bind_data.options.include_segment_info ? &output.data[col_idx++] : nullptr;
	auto &additional_block_ids = output.data[col_idx++];

	while (count < STANDARD_VECTOR_SIZE) {
		if (lstate.buffer_offset >= lstate.buffer.size()) {
			// drained the current buffer; pull the next row group under the global lock
			lstate.buffer.clear();
			lstate.buffer_offset = 0;
			bool has_more;
			{
				lock_guard<mutex> guard(gstate.lock);
				has_more = bind_data.table_entry.ScanColumnSegmentInfo(query_context, gstate.scan_state, lstate.buffer);
				if (has_more) {
					gstate.scanned_row_groups.fetch_add(1, std::memory_order_relaxed);
				}
			}
			if (!has_more) {
				break;
			}
			if (lstate.buffer.empty()) {
				continue;
			}
		}

		auto &entry = lstate.buffer[lstate.buffer_offset];
		// keep each output chunk to a single row group so batch_index identifies its contents.
		// merging is allowed when the next entry happens to belong to the same row group.
		if (count > 0 && entry.row_group_index != lstate.batch_index) {
			break;
		}
		lstate.buffer_offset++;
		lstate.batch_index = entry.row_group_index;

		row_group_id.Append(Value::BIGINT(NumericCast<int64_t>(entry.row_group_index)));
		auto &col = columns.GetColumn(PhysicalIndex(entry.column_id));
		column_name.Append(Value(col.Name()));
		column_id.Append(Value::BIGINT(NumericCast<int64_t>(entry.column_id)));
		column_path.Append(Value(entry.column_path));
		segment_id.Append(Value::BIGINT(NumericCast<int64_t>(entry.segment_idx)));
		segment_type.Append(Value(entry.segment_type));
		start.Append(Value::BIGINT(NumericCast<int64_t>(entry.segment_start)));
		count_col.Append(Value::BIGINT(NumericCast<int64_t>(entry.segment_count)));
		compression.Append(Value(entry.compression_type));
		stats.Append(entry.segment_stats);
		has_updates.Append(Value::BOOLEAN(entry.has_updates));
		persistent.Append(Value::BOOLEAN(entry.persistent));
		if (entry.persistent) {
			block_id.Append(Value::BIGINT(entry.block_id));
			block_offset.Append(Value::BIGINT(NumericCast<int64_t>(entry.block_offset)));
		} else {
			block_id.Append(Value());
			block_offset.Append(Value());
		}
		if (segment_info) {
			segment_info->Append(Value(entry.segment_info));
		}
		if (entry.persistent) {
			additional_block_ids.Append(ValueFromBlockIdList(entry.additional_blocks));
		} else {
			additional_block_ids.Append(Value());
		}

		count++;
	}
}

static double PragmaStorageInfoProgress(ClientContext &context, const FunctionData *bind_data_p,
                                        const GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<PragmaStorageGlobalState>();
	if (gstate.total_row_groups == 0) {
		return 100.0;
	}
	auto scanned = MinValue<idx_t>(gstate.scanned_row_groups, gstate.total_row_groups);
	return 100.0 * static_cast<double>(scanned) / static_cast<double>(gstate.total_row_groups);
}

static OperatorPartitionData PragmaStorageInfoGetPartitionData(ClientContext &context,
                                                               TableFunctionGetPartitionInput &input) {
	auto &lstate = input.local_state->Cast<PragmaStorageLocalState>();
	return OperatorPartitionData(lstate.batch_index);
}

void PragmaStorageInfo::RegisterFunction(BuiltinFunctions &set) {
	TableFunction storage_info(
	    "pragma_storage_info", FunctionSignature().AddPositionalOnly("table_name", LogicalType::VARCHAR),
	    PragmaStorageInfoFunction, PragmaStorageInfoBind, PragmaStorageInfoInitGlobal, PragmaStorageInfoInitLocal);
	storage_info.get_partition_data = PragmaStorageInfoGetPartitionData;
	storage_info.table_scan_progress = PragmaStorageInfoProgress;
	storage_info.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("include_segment_info", LogicalType::BOOLEAN).Add("loaded_segments_only", LogicalType::BOOLEAN);
	});
	set.AddFunction(std::move(storage_info));
}

} // namespace duckdb
