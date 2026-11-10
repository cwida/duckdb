#include "duckdb/storage/table/column_data_checkpointer.hpp"
#include "duckdb/storage/compression/standard_compression_state.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/update_segment.hpp"
#include "duckdb/storage/data_table.hpp"

namespace duckdb {

//! ColumnDataCheckpointData

const CompressionFunction &ColumnDataCheckpointData::GetCompressionFunction(CompressionType compression_type) {
	auto &db = col_data->GetDatabase();
	auto &column_type = col_data->type;
	auto &config = DBConfig::GetConfig(db);
	return config.GetCompressionFunction(compression_type, column_type.InternalType());
}

DatabaseInstance &ColumnDataCheckpointData::GetDatabase() {
	return col_data->GetDatabase();
}

const LogicalType &ColumnDataCheckpointData::GetType() const {
	return col_data->type;
}

ColumnData &ColumnDataCheckpointData::GetColumnData() {
	return *col_data;
}

const RowGroup &ColumnDataCheckpointData::GetRowGroup() {
	return *row_group;
}

ColumnCheckpointState &ColumnDataCheckpointData::GetCheckpointState() {
	return *checkpoint_state;
}

StorageManager &ColumnDataCheckpointData::GetStorageManager() {
	return *storage_manager;
}

//! ColumnDataCheckpointer

static void CreateIntermediateVector(vector<reference<ColumnCheckpointState>> &states, DataChunk &chunk) {
	D_ASSERT(!states.empty());

	auto &first_state = states[0];
	auto &col_data = first_state.get().original_column;
	auto &type = col_data.type;

	vector<LogicalType> types;
	if (type.id() == LogicalTypeId::VALIDITY) {
		types.emplace_back(LogicalType::BOOLEAN);
	} else if (type.InternalType() == PhysicalType::LIST) {
		types.emplace_back(LogicalType::UBIGINT);
	} else {
		types.emplace_back(type);
	}
	chunk.Initialize(Allocator::DefaultAllocator(), types);
	if (type.id() == LogicalTypeId::VALIDITY) {
		auto data = FlatVector::GetDataMutable<bool>(chunk.data[0]);
		memset(data, 0, sizeof(bool) * STANDARD_VECTOR_SIZE);
	}
}

ColumnDataCheckpointer::ColumnDataCheckpointer(vector<reference<ColumnCheckpointState>> &checkpoint_states,
                                               StorageManager &storage_manager, const RowGroup &row_group,
                                               ColumnCheckpointInfo &checkpoint_info)
    : checkpoint_states(checkpoint_states), storage_manager(storage_manager), row_group(row_group),
      checkpoint_info(checkpoint_info) {
	auto &db = storage_manager.GetDatabase();
	auto &config = DBConfig::GetConfig(db);
	compression_functions.resize(checkpoint_states.size());
	for (idx_t i = 0; i < checkpoint_states.size(); i++) {
		auto &col_data = checkpoint_states[i].get().original_column;
		auto to_add = config.GetCompressionFunctions(col_data.type.InternalType());
		auto &functions = compression_functions[i];
		for (auto &func : to_add) {
			functions.push_back(&func.get());
		}
	}
	CreateIntermediateVector(checkpoint_states, intermediate);
}

void ColumnDataCheckpointer::ScanSegments(const std::function<void(Vector &)> &callback) {
	auto &first_state = checkpoint_states[0];
	auto &col_data = first_state.get().original_column;

	// TODO: scan all the nodes from all segments, no need for CheckpointScan to virtualize this I think..
	for (auto &segment_node : col_data.data.SegmentNodes()) {
		auto &segment = segment_node.GetNode();
		ColumnScanState scan_state(nullptr);
		scan_state.current = segment_node;
		segment.InitializeScan(scan_state);

		auto &scan_vector = intermediate.data[0];
		for (idx_t base_row_index = 0; base_row_index < segment.count; base_row_index += STANDARD_VECTOR_SIZE) {
			intermediate.Reset();

			idx_t count = MinValue<idx_t>(segment.count - base_row_index, STANDARD_VECTOR_SIZE);
			scan_state.offset_in_column = segment_node.GetRowStart() + base_row_index;

			col_data.CheckpointScan(segment, scan_state, count, scan_vector);
			scan_vector.BufferMutable().SetVectorSize(count);
			callback(scan_vector);
		}
	}
}

CompressionType ForceCompression(StorageManager &storage_manager,
                                 vector<optional_ptr<const CompressionFunction>> &compression_functions,
                                 CompressionType compression_type) {
	// One of the force_compression flags has been set
	// check if this compression method is available
	// auto compression_availability_result = CompressionTypeIsAvailable(compression_type, storage_manager);
	// if (!compression_availability_result.IsAvailable()) {
	//	throw InvalidInputException("The forced compression method (%s) is not available in the current storage
	// version", CompressionTypeToString(compression_type));
	//}

	bool found = false;
	for (idx_t i = 0; i < compression_functions.size(); i++) {
		auto &compression_function = *compression_functions[i];
		if (compression_function.type == compression_type) {
			found = true;
			break;
		}
	}
	if (!found) {
		return CompressionType::COMPRESSION_AUTO;
	}
	// the force_compression method is available
	// clear all other compression methods
	// except the uncompressed method, so we can fall back on that
	for (idx_t i = 0; i < compression_functions.size(); i++) {
		auto &compression_function = *compression_functions[i];
		if (compression_function.type == CompressionType::COMPRESSION_UNCOMPRESSED) {
			continue;
		}
		if (compression_function.type != compression_type) {
			compression_functions[i] = nullptr;
		}
	}
	return compression_type;
}

void ColumnDataCheckpointer::InitAnalyze() {
	analyze_states.resize(checkpoint_states.size());
	for (idx_t i = 0; i < checkpoint_states.size(); i++) {
		auto &functions = compression_functions[i];
		auto &states = analyze_states[i];
		auto &checkpoint_state = checkpoint_states[i];
		auto &coldata = checkpoint_state.get().GetResultColumn();
		states.resize(functions.size());
		for (idx_t j = 0; j < functions.size(); j++) {
			auto &func = functions[j];
			if (!func) {
				continue;
			}
			states[j] = func->init_analyze(coldata, coldata.type.InternalType());
		}
	}
}

vector<CheckpointAnalyzeResult> ColumnDataCheckpointer::DetectBestCompressionMethod() {
	D_ASSERT(!compression_functions.empty());
	auto &db = storage_manager.GetDatabase();
	auto &config = DBConfig::GetConfig(db);
	vector<CompressionType> forced_methods(checkpoint_states.size(), CompressionType::COMPRESSION_AUTO);

	auto compression_type = checkpoint_info.GetCompressionType();
	for (idx_t i = 0; i < checkpoint_states.size(); i++) {
		auto &functions = compression_functions[i];
		if (compression_type != CompressionType::COMPRESSION_AUTO) {
			forced_methods[i] = ForceCompression(storage_manager, functions, compression_type);
		}
		if (compression_type == CompressionType::COMPRESSION_AUTO) {
			auto force_compression = Settings::Get<ForceCompressionSetting>(config);
			if (force_compression != CompressionType::COMPRESSION_AUTO) {
				forced_methods[i] = ForceCompression(storage_manager, functions, force_compression);
			}
		}
	}

	InitAnalyze();
	// scan over all the segments and run the analyze step
	ScanSegments([&](Vector &scan_vector) {
		for (idx_t i = 0; i < checkpoint_states.size(); i++) {
			auto &functions = compression_functions[i];
			auto &states = analyze_states[i];
			for (idx_t j = 0; j < functions.size(); j++) {
				auto &state = states[j];
				auto &func = functions[j];

				if (!state) {
					continue;
				}
				if (!func->analyze(*state, scan_vector)) {
					state = nullptr;
					func = nullptr;
				}
			}
		}
	});

	vector<CheckpointAnalyzeResult> result;
	result.resize(checkpoint_states.size());

	for (idx_t i = 0; i < checkpoint_states.size(); i++) {
		auto &functions = compression_functions[i];
		auto &states = analyze_states[i];
		auto &forced_method = forced_methods[i];

		unique_ptr<AnalyzeState> chosen_state;
		idx_t best_score = NumericLimits<idx_t>::Maximum();
		idx_t compression_idx = DConstants::INVALID_INDEX;

		D_ASSERT(functions.size() == states.size());
		for (idx_t j = 0; j < functions.size(); j++) {
			auto &function = functions[j];
			auto &state = states[j];

			if (!state) {
				continue;
			}

			//! Check if the method type is the forced method (if forced is used)
			bool forced_method_found = function->type == forced_method;
			// now that we have passed over all the data, we need to figure out the best method
			// we do this using the final_analyze method
			auto score = function->final_analyze(*state);

			//! The finalize method can return this value from final_analyze to indicate it should not be used.
			if (score == DConstants::INVALID_INDEX) {
				continue;
			}

			if (score < best_score || forced_method_found) {
				compression_idx = j;
				best_score = score;
				chosen_state = std::move(state);
			}
			//! If we have found the forced method, we're done
			if (forced_method_found) {
				break;
			}
		}

		auto &checkpoint_state = checkpoint_states[i];
		auto &col_data = checkpoint_state.get().GetResultColumn();
		if (!chosen_state) {
			throw FatalException("No suitable compression/storage method found to store column of type %s",
			                     col_data.type.ToString());
		}
		D_ASSERT(compression_idx != DConstants::INVALID_INDEX);

		auto &best_function = *functions[compression_idx];
		DUCKDB_LOG_TRACE(db, "ColumnDataCheckpointer FinalAnalyze(%s) result for %s.%s.%d(%s): %d",
		                 EnumUtil::ToString(best_function.type), col_data.info.GetSchemaName(),
		                 col_data.info.GetTableName(), col_data.column_index, col_data.type.ToString(), best_score);
		result[i] = CheckpointAnalyzeResult(std::move(chosen_state), best_function);
	}
	return result;
}

struct CheckpointBlockIdDropper : public BlockIdVisitor {
	explicit CheckpointBlockIdDropper(BlockManager &manager) : manager(manager) {
	}

	void Visit(block_id_t block_id) override {
		manager.MarkBlockAsModified(block_id);
	}

	BlockManager &manager;
};

void ColumnDataCheckpointer::DropSegments() {
	// first we check the current segments
	// if there are any persistent segments, we will mark their old block ids as modified
	// since the segments will be rewritten their old on disk data is no longer required

	for (idx_t i = 0; i < checkpoint_states.size(); i++) {
		auto &state = checkpoint_states[i];
		auto &col_data = state.get().original_column;

		// Drop the segments, as we'll be replacing them with new ones, because there are changes
		CheckpointBlockIdDropper dropper(storage_manager.GetBlockManager());
		for (auto &segment : col_data.data.Segments()) {
			segment.VisitBlockIds(dropper);
		}
	}
}

bool ColumnDataCheckpointer::ValidityCoveredByBasedata(vector<CheckpointAnalyzeResult> &result) {
	if (result.size() != 2) {
		return false;
	}
	auto &base = result[0];
	D_ASSERT(base.function);
	return base.function->validity == CompressionValidity::NO_VALIDITY_REQUIRED;
}

void ColumnDataCheckpointer::WriteToDisk() { // Analyze the candidate functions to select one of them to use for
	                                         // compression
	auto analyze_result = DetectBestCompressionMethod();
	if (ValidityCoveredByBasedata(analyze_result)) {
		D_ASSERT(analyze_result.size() == 2);
		auto &validity = analyze_result[1];
		auto &db = storage_manager.GetDatabase();
		auto &config = DBConfig::GetConfig(db);
		// Override the function to the COMPRESSION_EMPTY
		// turning the compression+final compress steps into a no-op, saving a single empty segment
		validity.function = config.GetCompressionFunction(CompressionType::COMPRESSION_EMPTY, PhysicalType::BIT).get();
	}

	// Initialize the compression for the selected function
	D_ASSERT(analyze_result.size() == checkpoint_states.size());
	vector<ColumnDataCheckpointData> checkpoint_data(checkpoint_states.size());
	vector<unique_ptr<CompressionState>> compression_states(checkpoint_states.size());
	for (idx_t i = 0; i < analyze_result.size(); i++) {
		auto &analyze_state = analyze_result[i].analyze_state;
		auto &function = analyze_result[i].function;

		auto &checkpoint_state = checkpoint_states[i];
		auto &col_data = checkpoint_state.get().GetResultColumn();

		checkpoint_data[i] =
		    ColumnDataCheckpointData(checkpoint_state, col_data, col_data.GetDatabase(), row_group, storage_manager);
		compression_states[i] = function->init_compression(checkpoint_data[i], std::move(analyze_state));
	}

	// Scan over the existing segment + changes and compress the data
	ScanSegments([&](Vector &scan_vector) {
		for (idx_t i = 0; i < checkpoint_states.size(); i++) {
			auto &function = analyze_result[i].function;
			auto &compression_state = compression_states[i];
			function->compress(*compression_state, scan_vector);
		}
	});

	// Finalize the compression
	for (idx_t i = 0; i < checkpoint_states.size(); i++) {
		auto &function = analyze_result[i].function;
		auto &compression_state = compression_states[i];
		function->compress_finalize(*compression_state);
	}

	// after we finish checkpointing we can drop this segment
	DropSegments();
}

bool ColumnDataCheckpointer::HasChanges(ColumnData &col_data) {
	return col_data.HasAnyChanges();
}

void ColumnDataCheckpointer::WritePersistentSegments(ColumnCheckpointState &state) {
	// all segments are persistent and there are no updates
	// we only need to write the metadata

	auto &col_data = state.original_column;

	optional_idx error_segment_start;
	idx_t current_row = 0;
	for (auto &segment_node : col_data.data.SegmentNodes()) {
		auto &segment = segment_node.GetNode();
		auto segment_start = segment_node.GetRowStart();
		if (segment_start != current_row) {
			error_segment_start = segment_start;
			break;
		}
		auto pointer = segment.GetDataPointer(current_row);
		current_row += segment.count;

		// merge the persistent stats into the global column stats
		state.global_stats->Merge(segment.GetStats());
		state.data_pointers.push_back(std::move(pointer));
	}
	if (error_segment_start.IsValid()) {
		string extra_info;
		for (auto &s : col_data.data.SegmentNodes()) {
			extra_info += "\n";
			extra_info += StringUtil::Format("Start %d, count %d", s.GetRowStart(), s.GetNode().count.load());
		}
		throw InternalException(
		    "Failure in RowGroup::Checkpoint - column data pointer is unaligned with row group "
		    "start\nRow group start: %d\nRow group count %d\nCurrent row: %d\nSegment start: %d\nColumn index: "
		    "%d\nColumn type: %s\nRoot type: %s\nTable: %s.%s\nAll segments:%s",
		    row_group.count.load(), current_row, error_segment_start.GetIndex(), col_data.column_index, col_data.type,
		    col_data.type, col_data.info.GetSchemaName(), col_data.info.GetTableName(), extra_info);
	}
}

struct CheckpointBlockIdMarker : public BlockIdVisitor {
	explicit CheckpointBlockIdMarker(BlockManager &manager) : manager(manager) {
	}

	void Visit(block_id_t block_id) override {
		manager.MarkBlockAsCheckpointed(block_id);
	}

	BlockManager &manager;
};

void ColumnDataCheckpointer::Checkpoint() {
	for (idx_t i = 0; i < checkpoint_states.size(); i++) {
		auto &state = checkpoint_states[i];
		auto &col_data = state.get().original_column;
		if (col_data.HasChanges()) {
			has_changes = true;
			break;
		}
	}

	if (!has_changes) {
		// Nothing has undergone any changes, no need to checkpoint
		// just move on to finalizing
		// mark block ids as checkpointed
		CheckpointBlockIdMarker marker(storage_manager.GetBlockManager());
		for (idx_t i = 0; i < checkpoint_states.size(); i++) {
			auto &state = checkpoint_states[i];
			auto &col_data = state.get().original_column;
			col_data.VisitBlockIds(marker);
		}
		return;
	}

	WriteToDisk();
}

void ColumnDataCheckpointer::FinalizeCheckpoint() {
	if (has_changes) {
		// something has undergone changes, we rewrote everything
		// write the new data - not the old data
		return;
	}
	// no changes - copy over the original columns
	for (idx_t i = 0; i < checkpoint_states.size(); i++) {
		auto &state = checkpoint_states[i].get();
		WritePersistentSegments(state);
	}
}

} // namespace duckdb
