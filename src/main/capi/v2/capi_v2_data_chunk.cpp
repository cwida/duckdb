#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/type_visitor.hpp"

namespace duckdb {
namespace capiv2 {

static void CreateDataChunk(Allocator &allocator, const duckdb_v2_logical_type_handle *types, idx_t column_count,
                            duckdb_v2_data_chunk_handle *out_chunk) {
	*out_chunk = nullptr;
	vector<LogicalType> logical_types;
	logical_types.reserve(column_count);
	for (idx_t i = 0; i < column_count; i++) {
		if (!types[i]) {
			throw InvalidInputException("null logical type at index %llu", i);
		}
		auto ltype_ref = Convert(types[i]);
		const auto &ltype = *ltype_ref;
		// ANY is a signature wildcard with no physical layout; a chunk allocates
		// storage, so reject it (an ANY vector throws InternalException).
		if (TypeVisitor::Contains(ltype, LogicalTypeId::ANY)) {
			throw InvalidInputException("logical type at index %llu cannot be ANY", i);
		}
		logical_types.push_back(ltype);
	}
	auto chunk = make_uniq<CV2DataChunk>();
	chunk->Initialize(allocator, logical_types);
	*out_chunk = Convert(chunk.release());
}

static void CopyDataChunk(DatabaseInstance &db, duckdb_v2_data_chunk_handle chunk,
                          duckdb_v2_data_chunk_handle *out_chunk) {
	*out_chunk = nullptr;
	auto &source = *Convert(chunk);
	auto copy = make_uniq<CV2DataChunk>();
	copy->Initialize(Allocator::Get(db), source.GetTypes(), MaxValue<idx_t>(source.size(), STANDARD_VECTOR_SIZE));
	source.Copy(*copy);
	copy->SetCardinalityUnsafe(source.size());
	*out_chunk = Convert(copy.release());
}

} // namespace capiv2
} // namespace duckdb

using namespace duckdb::capiv2;

// TODO: this should be removed.
DUCKDB_V2_ERROR duckdb_v2_data_chunk_create(duckdb_v2_factory_handle factory,
                                            const duckdb_v2_logical_type_handle *types, idx_t column_count,
                                            duckdb_v2_data_chunk_handle *out_chunk, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(types);
	DUCKDB_CHECK_ARG(out_chunk);
	return WithErrorHandler(err, [&]() {
		CreateDataChunk(duckdb::Allocator::Get(Convert(factory)->GetDatabase()), types, column_count, out_chunk);
	});
}

DUCKDB_V2_ERROR duckdb_v2_data_chunk_copy(duckdb_v2_factory_handle factory, duckdb_v2_data_chunk_handle chunk,
                                          duckdb_v2_data_chunk_handle *out_chunk, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(chunk);
	DUCKDB_CHECK_ARG(out_chunk);
	return WithErrorHandler(err, [&]() { CopyDataChunk(Convert(factory)->GetDatabase(), chunk, out_chunk); });
}

DUCKDB_V2_ERROR duckdb_v2_data_chunk_clear(duckdb_v2_data_chunk_handle chunk, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(chunk);
	return WithErrorHandler(err, [&]() { Convert(chunk)->Reset(); });
}

DUCKDB_V2_ERROR duckdb_v2_data_chunk_destroy(duckdb_v2_data_chunk_handle *chunk) {
	return WithErrorHandler(nullptr, [&]() {
		if (!chunk) {
			return;
		}
		if (*chunk) {
			delete Convert(*chunk);
			*chunk = nullptr;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_data_chunk_get_size(duckdb_v2_data_chunk_handle chunk, idx_t *out_size,
                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(chunk);
	DUCKDB_CHECK_ARG(out_size);
	return WithErrorHandler(err, [&]() { *out_size = Convert(chunk)->size(); });
}

DUCKDB_V2_ERROR duckdb_v2_data_chunk_get_capacity(duckdb_v2_data_chunk_handle chunk, idx_t *out_capacity,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(chunk);
	DUCKDB_CHECK_ARG(out_capacity);
	return WithErrorHandler(err, [&]() { *out_capacity = Convert(chunk)->GetCapacity(); });
}

DUCKDB_V2_ERROR duckdb_v2_data_chunk_get_vector_count(duckdb_v2_data_chunk_handle chunk, idx_t *out_count,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(chunk);
	DUCKDB_CHECK_ARG(out_count);
	return WithErrorHandler(err, [&]() { *out_count = Convert(chunk)->ColumnCount(); });
}

DUCKDB_V2_ERROR duckdb_v2_data_chunk_get_vector(duckdb_v2_data_chunk_handle chunk, idx_t index,
                                                duckdb_v2_vector_handle *out_vector, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(chunk);
	DUCKDB_CHECK_ARG(out_vector);
	*out_vector = nullptr;
	return WithErrorHandler(err, [&]() {
		auto *c = Convert(chunk);
		if (index >= c->ColumnCount()) {
			throw duckdb::InvalidInputException("vector index out of range");
		}
		*out_vector = Convert(&c->data[index]);
	});
}
