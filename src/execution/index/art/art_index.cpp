#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_index_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/execution/index/art/art_operator.hpp"
#include "duckdb/execution/index/bound_index.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table/append_state.hpp"

namespace duckdb {
namespace {

//----------------------------------------------------------------------------------------------------------------------
// Bind
//----------------------------------------------------------------------------------------------------------------------
class ARTBuildBindData : public IndexBuildBindData {
public:
	bool sorted = false;
};

unique_ptr<IndexBuildBindData> ARTBuildBind(IndexBuildBindInput &input) {
	auto bind_data = make_uniq<ARTBuildBindData>();

	// TODO: Verify that the the ART is applicable for the given columns and types.

	// We used to not sort for VARCHAR and multi-column indexes with the old sort implementation
	// The new sorting implementation handles these cases much better and sorting improves performance now
	bind_data->sorted = true;

	return std::move(bind_data);
}

bool ARTBuildSort(IndexBuildSortInput &input) {
	auto &bind_data = input.bind_data->Cast<ARTBuildBindData>();
	return bind_data.sorted;
}

//----------------------------------------------------------------------------------------------------------------------
// Global State
//----------------------------------------------------------------------------------------------------------------------
class ARTBuildGlobalState : public IndexBuildGlobalState {
public:
	unique_ptr<BoundIndex> global_index;
};

unique_ptr<IndexBuildGlobalState> ARTBuildGlobalInit(IndexBuildInitGlobalStateInput &input) {
	auto state = make_uniq<ARTBuildGlobalState>();

	auto &storage = input.table.GetStorage();
	state->global_index = make_uniq<ART>(input.info.GetIndexName(), input.info.constraint_type, input.storage_ids,
	                                     TableIOManager::Get(storage), input.expressions, storage.db);

	return std::move(state);
}

//----------------------------------------------------------------------------------------------------------------------
// Local State
//----------------------------------------------------------------------------------------------------------------------
class ARTBuildLocalState : public IndexBuildLocalState {
public:
	unique_ptr<BoundIndex> local_index;
	ArenaAllocator arena_allocator;

	unsafe_vector<ARTKey> keys;
	unsafe_vector<ARTKey> row_ids;

	explicit ARTBuildLocalState(ClientContext &context) : arena_allocator(Allocator::Get(context)) {};
};

unique_ptr<IndexBuildLocalState> ARTBuildLocalInit(IndexBuildInitLocalStateInput &input) {
	// Create the local sink state and add the local index.
	auto state = make_uniq<ARTBuildLocalState>(input.context);

	auto &storage = input.table.GetStorage();
	state->local_index = make_uniq<ART>(input.info.GetIndexName(), input.info.constraint_type, input.storage_ids,
	                                    TableIOManager::Get(storage), input.expressions, storage.db);

	// Initialize the local sink state.
	state->keys.resize(STANDARD_VECTOR_SIZE);
	state->row_ids.resize(STANDARD_VECTOR_SIZE);

	return std::move(state);
}

//----------------------------------------------------------------------------------------------------------------------
// Sink
//----------------------------------------------------------------------------------------------------------------------
void ARTBuildSinkUnsorted(IndexBuildSinkInput &input, DataChunk &key_chunk, DataChunk &row_chunk) {
	auto &l_state = input.local_state.Cast<ARTBuildLocalState>();
	auto row_count = key_chunk.size();
	auto &art = l_state.local_index->Cast<ART>();

	// Insert each key and its corresponding row ID.
	for (idx_t i = 0; i < row_count; i++) {
		auto status = art.root_ptr.GetGateStatus();
		auto conflict_type =
		    ARTOperator::Insert(l_state.arena_allocator, art, art.root_ptr, l_state.keys[i], 0, l_state.row_ids[i],
		                        status, DeleteIndexInfo(), IndexAppendMode::DEFAULT);
		if (conflict_type == ARTConflictType::CONSTRAINT) {
			throw ConstraintException("Data contains duplicates on indexed column(s)");
		}
	}
}

void ARTBuildSinkSorted(IndexBuildSinkInput &input, DataChunk &key_chunk, DataChunk &row_chunk) {
	auto &l_state = input.local_state.Cast<ARTBuildLocalState>();
	auto &storage = input.table.GetStorage();
	auto &l_index = l_state.local_index;

	// Construct an ART for this chunk.
	auto art = make_uniq<ART>(input.info.GetIndexName(), l_index->GetConstraintType(), l_index->GetColumnIds(),
	                          l_index->table_io_manager, l_index->unbound_expressions, storage.db,
	                          l_index->Cast<ART>().allocators);
	if (art->Build(l_state.keys, l_state.row_ids, key_chunk.size()) != ARTConflictType::NO_CONFLICT) {
		throw ConstraintException("Data contains duplicates on indexed column(s)");
	}

	// Merge the ART into the local ART.
	if (!l_index->MergeIndexes(*art)) {
		throw ConstraintException("Data contains duplicates on indexed column(s)");
	}
}

void ARTBuildSink(IndexBuildSinkInput &input, DataChunk &key_chunk, DataChunk &row_chunk) {
	auto &bind_data = input.bind_data->Cast<ARTBuildBindData>();
	auto &lstate = input.local_state.Cast<ARTBuildLocalState>();

	lstate.arena_allocator.Reset();

	lstate.local_index->Cast<ART>().GenerateKeyVectors(lstate.arena_allocator, key_chunk, row_chunk.data[0],
	                                                   lstate.keys, lstate.row_ids);

	if (bind_data.sorted) {
		return ARTBuildSinkSorted(input, key_chunk, row_chunk);
	}
	return ARTBuildSinkUnsorted(input, key_chunk, row_chunk);
}

//----------------------------------------------------------------------------------------------------------------------
// Combine
//----------------------------------------------------------------------------------------------------------------------
void ARTBuildCombine(IndexBuildCombineInput &input) {
	auto &gstate = input.global_state.Cast<ARTBuildGlobalState>();
	auto &lstate = input.local_state.Cast<ARTBuildLocalState>();

	if (!gstate.global_index->MergeIndexes(*lstate.local_index)) {
		throw ConstraintException("Data contains duplicates on indexed column(s)");
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Finalize
//----------------------------------------------------------------------------------------------------------------------
unique_ptr<BoundIndex> ARTBuildFinalize(IndexBuildFinalizeInput &input) {
	auto &gstate = input.global_state.Cast<ARTBuildGlobalState>();
	return std::move(gstate.global_index);
}

} // namespace

//----------------------------------------------------------------------------------------------------------------------
// ART::GetIndexType
//----------------------------------------------------------------------------------------------------------------------
IndexType ART::GetARTIndexType() {
	IndexType art_index_type;
	art_index_type.name = ART::TYPE_NAME;
	art_index_type.create_instance = ART::Create;
	art_index_type.build_bind = ARTBuildBind;
	art_index_type.build_sort = ARTBuildSort;
	art_index_type.build_global_init = ARTBuildGlobalInit;
	art_index_type.build_local_init = ARTBuildLocalInit;
	art_index_type.build_sink = ARTBuildSink;
	art_index_type.build_combine = ARTBuildCombine;
	art_index_type.build_finalize = ARTBuildFinalize;
	return art_index_type;
}

} // namespace duckdb
