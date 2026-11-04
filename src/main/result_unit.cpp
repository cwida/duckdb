#include "duckdb/main/result_unit.hpp"

namespace duckdb {

ResultUnit::ResultUnit(idx_t row_count_p, idx_t byte_size_p) : row_count(row_count_p), byte_size(byte_size_p) {
}

ResultUnit::~ResultUnit() {
}

ChunkUnit::ChunkUnit(unique_ptr<DataChunk> chunk_p)
    : ResultUnit(chunk_p->size(), chunk_p->GetDataSize()), chunk(std::move(chunk_p)) {
}

} // namespace duckdb
