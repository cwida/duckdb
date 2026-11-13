#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {
namespace capiv2 {

static auto Convert(ColumnDataAppendState *state) -> duckdb_v2_column_data_collection_append_state_handle {
	return reinterpret_cast<duckdb_v2_column_data_collection_append_state_handle>(state);
}

static auto Convert(duckdb_v2_column_data_collection_append_state_handle state) -> ColumnDataAppendState * {
	return reinterpret_cast<ColumnDataAppendState *>(state);
}

static auto Convert(ColumnDataParallelScanState *state) -> duckdb_v2_column_data_collection_shared_scan_state_handle {
	return reinterpret_cast<duckdb_v2_column_data_collection_shared_scan_state_handle>(state);
}

static auto Convert(duckdb_v2_column_data_collection_shared_scan_state_handle state) -> ColumnDataParallelScanState * {
	return reinterpret_cast<ColumnDataParallelScanState *>(state);
}

static auto Convert(ColumnDataLocalScanState *state) -> duckdb_v2_column_data_collection_worker_scan_state_handle {
	return reinterpret_cast<duckdb_v2_column_data_collection_worker_scan_state_handle>(state);
}

static auto Convert(duckdb_v2_column_data_collection_worker_scan_state_handle state) -> ColumnDataLocalScanState * {
	return reinterpret_cast<ColumnDataLocalScanState *>(state);
}

static void CreateColumnDataCollection(DatabaseInstance &db, const duckdb_v2_logical_type_handle *types_array,
                                       idx_t types_count, duckdb_v2_column_data_collection_handle *out_collection,
                                       const char *function_name) {
	*out_collection = nullptr;
	if (types_count == 0) {
		throw InvalidInputException("%s requires at least one column type", function_name);
	}
	vector<LogicalType> types;
	types.reserve(types_count);
	for (idx_t i = 0; i < types_count; i++) {
		if (!types_array[i]) {
			throw InvalidInputException("null logical type at index %llu", i);
		}
		auto type_ref = Convert(types_array[i]);
		const auto &type = *type_ref;
		// ANY is a signature wildcard with no physical layout; a collection
		// allocates storage, so reject it (mirrors data_chunk_create).
		if (TypeVisitor::Contains(type, LogicalTypeId::ANY)) {
			throw InvalidInputException("logical type at index %llu cannot be ANY", i);
		}
		types.push_back(type);
	}
	auto collection = make_uniq<ColumnDataCollection>(BufferManager::GetBufferManager(db), std::move(types));
	*out_collection = Convert(collection.release());
}

// The engine append/scan only D_ASSERT the chunk against the collection's
// types; validate here so a mismatch is refused before anything is copied.
static void VerifyChunkTypes(const ColumnDataCollection &collection, const DataChunk &chunk,
                             const char *function_name) {
	auto &types = collection.Types();
	if (chunk.ColumnCount() != types.size()) {
		throw InvalidInputException("%s: chunk column count %llu does not match the collection's column count %llu",
		                            function_name, chunk.ColumnCount(), types.size());
	}
	for (idx_t i = 0; i < types.size(); i++) {
		if (chunk.data[i].GetType() != types[i]) {
			throw InvalidInputException("%s: chunk type mismatch at column %llu: expected %s, got %s", function_name, i,
			                            types[i].ToString(), chunk.data[i].GetType().ToString());
		}
	}
}

} // namespace capiv2
} // namespace duckdb

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_column_data_collection_create(duckdb_v2_factory_handle factory,
                                                        const duckdb_v2_logical_type_handle *types_array,
                                                        idx_t types_count,
                                                        duckdb_v2_column_data_collection_handle *out_collection,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(types_array);
	DUCKDB_CHECK_ARG(out_collection);
	return WithErrorHandler(err, [&]() {
		CreateColumnDataCollection(Convert(factory)->GetDatabase(), types_array, types_count, out_collection,
		                           "duckdb_v2_column_data_collection_create");
	});
}

DUCKDB_V2_ERROR duckdb_v2_column_data_collection_reset(duckdb_v2_column_data_collection_handle collection,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(collection);
	return WithErrorHandler(err, [&]() { Convert(collection)->Reset(); });
}

DUCKDB_V2_ERROR duckdb_v2_column_data_collection_clear(duckdb_v2_column_data_collection_handle collection,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(collection);
	return WithErrorHandler(err, [&]() { Convert(collection)->ResetForReuse(); });
}

DUCKDB_V2_ERROR duckdb_v2_column_data_collection_destroy(duckdb_v2_column_data_collection_handle *collection) {
	return WithErrorHandler(nullptr, [&]() {
		if (collection && *collection) {
			delete Convert(*collection);
			*collection = nullptr;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_column_data_collection_combine(duckdb_v2_column_data_collection_handle target,
                                                         duckdb_v2_column_data_collection_handle *source,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(target);
	DUCKDB_CHECK_ARG(source);
	DUCKDB_CHECK_ARG(*source);
	return WithErrorHandler(err, [&]() {
		auto &target_cdc = *Convert(target);
		auto *source_cdc = Convert(*source);
		if (source_cdc == &target_cdc) {
			throw duckdb::InvalidInputException("cannot combine a column data collection with itself");
		}
		// The engine throws InternalException on mismatching types; refuse here instead.
		if (target_cdc.Types() != source_cdc->Types()) {
			throw duckdb::InvalidInputException("cannot combine column data collections with mismatching types");
		}
		target_cdc.Combine(*source_cdc);
		delete source_cdc;
		*source = nullptr;
	});
}

DUCKDB_V2_ERROR duckdb_v2_column_data_collection_row_count(duckdb_v2_column_data_collection_handle collection,
                                                           idx_t *out_row_count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(collection);
	DUCKDB_CHECK_ARG(out_row_count);
	return WithErrorHandler(err, [&]() { *out_row_count = Convert(collection)->Count(); });
}

DUCKDB_V2_ERROR
duckdb_v2_column_data_collection_append_state_create(duckdb_v2_column_data_collection_handle collection,
                                                     duckdb_v2_column_data_collection_append_state_handle *out_state,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(collection);
	DUCKDB_CHECK_ARG(out_state);
	*out_state = nullptr;
	return WithErrorHandler(err, [&]() {
		auto state = duckdb::make_uniq<duckdb::ColumnDataAppendState>();
		Convert(collection)->InitializeAppend(*state);
		*out_state = Convert(state.release());
	});
}

DUCKDB_V2_ERROR
duckdb_v2_column_data_collection_append_state_destroy(duckdb_v2_column_data_collection_append_state_handle *state) {
	return WithErrorHandler(nullptr, [&]() {
		if (state && *state) {
			delete Convert(*state);
			*state = nullptr;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_column_data_collection_append(duckdb_v2_column_data_collection_handle collection,
                                                        duckdb_v2_column_data_collection_append_state_handle state,
                                                        duckdb_v2_data_chunk_handle chunk,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(collection);
	DUCKDB_CHECK_ARG(state);
	DUCKDB_CHECK_ARG(chunk);
	return WithErrorHandler(err, [&]() {
		auto &cdc = *Convert(collection);
		auto &input = *Convert(chunk);
		VerifyChunkTypes(cdc, input, "duckdb_v2_column_data_collection_append");
		cdc.Append(*Convert(state), input);
	});
}

DUCKDB_V2_ERROR duckdb_v2_column_data_collection_shared_scan_state_create(
    duckdb_v2_column_data_collection_handle collection,
    duckdb_v2_column_data_collection_shared_scan_state_handle *out_state, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(collection);
	DUCKDB_CHECK_ARG(out_state);
	*out_state = nullptr;
	return WithErrorHandler(err, [&]() {
		auto state = duckdb::make_uniq<duckdb::ColumnDataParallelScanState>();
		// Zero-copy: scanned chunks reference buffers pinned by the worker scan
		// state, valid until that worker's next scan or the state's destruction.
		Convert(collection)->InitializeScan(*state, duckdb::ColumnDataScanProperties::ALLOW_ZERO_COPY);
		*out_state = Convert(state.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_column_data_collection_shared_scan_state_destroy(
    duckdb_v2_column_data_collection_shared_scan_state_handle *state) {
	return WithErrorHandler(nullptr, [&]() {
		if (state && *state) {
			delete Convert(*state);
			*state = nullptr;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_column_data_collection_worker_scan_state_create(
    duckdb_v2_column_data_collection_handle collection,
    duckdb_v2_column_data_collection_worker_scan_state_handle *out_state, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(collection);
	DUCKDB_CHECK_ARG(out_state);
	return WithErrorHandler(err, [&]() {
		// The collection is not needed yet; keep the argument for future compatibility.
		auto state = duckdb::make_uniq<duckdb::ColumnDataLocalScanState>();
		*out_state = Convert(state.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_column_data_collection_worker_scan_state_destroy(
    duckdb_v2_column_data_collection_worker_scan_state_handle *state) {
	return WithErrorHandler(nullptr, [&]() {
		if (state && *state) {
			delete Convert(*state);
			*state = nullptr;
		}
	});
}

DUCKDB_V2_ERROR
duckdb_v2_column_data_collection_scan(duckdb_v2_column_data_collection_handle collection,
                                      duckdb_v2_column_data_collection_shared_scan_state_handle shared_state,
                                      duckdb_v2_column_data_collection_worker_scan_state_handle worker_state,
                                      duckdb_v2_data_chunk_handle out_chunk, bool *did_produce_chunk,
                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(collection);
	DUCKDB_CHECK_ARG(shared_state);
	DUCKDB_CHECK_ARG(worker_state);
	DUCKDB_CHECK_ARG(out_chunk);
	DUCKDB_CHECK_ARG(did_produce_chunk);
	return WithErrorHandler(err, [&]() {
		auto &cdc = *Convert(collection);
		auto &chunk = *Convert(out_chunk);
		VerifyChunkTypes(cdc, chunk, "duckdb_v2_column_data_collection_scan");
		*did_produce_chunk = cdc.Scan(*Convert(shared_state), *Convert(worker_state), chunk);
	});
}
