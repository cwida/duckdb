#include "duckdb/common/exception.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/arrow_wrapper.hpp"
namespace duckdb {

ArrowSchemaWrapper::~ArrowSchemaWrapper() {
	if (arrow_schema.release) {
		for (int64_t child_idx = 0; child_idx < arrow_schema.n_children; child_idx++) {
			auto &child = *arrow_schema.children[child_idx];
			if (child.release) {
				child.release(&child);
			}
		}
		arrow_schema.release(&arrow_schema);
		arrow_schema.release = nullptr;
	}
}

ArrowArrayWrapper::~ArrowArrayWrapper() {
	if (arrow_array.release) {
		for (int64_t child_idx = 0; child_idx < arrow_array.n_children; child_idx++) {
			auto &child = *arrow_array.children[child_idx];
			if (child.release) {
				child.release(&child);
			}
		}
		arrow_array.release(&arrow_array);
		arrow_array.release = nullptr;
	}
}

ArrowArrayStreamWrapper::~ArrowArrayStreamWrapper() {
	if (arrow_array_stream.release) {
		arrow_array_stream.release(&arrow_array_stream);
		arrow_array_stream.release = nullptr;
	}
}

void ArrowArrayStreamWrapper::GetSchema(ArrowSchemaWrapper &schema) {
	D_ASSERT(arrow_array_stream.get_schema);
	// LCOV_EXCL_START
	if (arrow_array_stream.get_schema(&arrow_array_stream, &schema.arrow_schema)) {
		throw InvalidInputException("arrow_scan: get_schema failed(): %s", string(GetError()));
	}
	if (!schema.arrow_schema.release) {
		throw InvalidInputException("arrow_scan: released schema passed");
	}
	if (schema.arrow_schema.n_children < 1) {
		throw InvalidInputException("arrow_scan: empty schema passed");
	}
	// LCOV_EXCL_STOP
}

unique_ptr<ArrowArrayWrapper> ArrowArrayStreamWrapper::GetNextChunk() {
	auto current_chunk = make_unique<ArrowArrayWrapper>();
	if (arrow_array_stream.get_next(&arrow_array_stream, &current_chunk->arrow_array)) { // LCOV_EXCL_START
		throw InvalidInputException("arrow_scan: get_next failed(): %s", string(GetError()));
	} // LCOV_EXCL_STOP

	return current_chunk;
}

const char *ArrowArrayStreamWrapper::GetError() { // LCOV_EXCL_START
	return arrow_array_stream.get_last_error(&arrow_array_stream);
} // LCOV_EXCL_STOP

} // namespace duckdb