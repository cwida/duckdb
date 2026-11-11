#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/allocator.hpp"

duckdb_data_chunk duckdb_stream_fetch_chunk(duckdb_result result) {
	if (!result.internal_data) {
		return nullptr;
	}
	// A streaming entry point hands out a retained result for a statement it does not stream, and its rows are read
	// here too, since this is the reader its caller was told to use
	return duckdb_fetch_chunk(result);
}

duckdb_data_chunk duckdb_fetch_chunk(duckdb_result result) {
	if (!result.internal_data) {
		return nullptr;
	}
	auto &result_data = *((duckdb::DuckDBResultData *)result.internal_data);
	if (result_data.result_set_type == duckdb::CAPIResultSetType::CAPI_RESULT_TYPE_DEPRECATED) {
		return nullptr;
	}
	result_data.result_set_type = duckdb::CAPIResultSetType::CAPI_RESULT_TYPE_STREAMING;
	try {
		auto chunk = result_data.Fetch();
		if (chunk) {
			for (auto &vector : chunk->data) {
				duckdb::PrepareCAPIVector(vector);
			}
		}
		return reinterpret_cast<duckdb_data_chunk>(chunk.release());
	} catch (std::exception &e) {
		// Set the error on the result so duckdb_result_error can retrieve it
		duckdb::ErrorData error(e);
		if (result_data.stream) {
			result_data.stream->SetError(std::move(error));
		} else {
			result_data.result->SetError(std::move(error));
		}
		return nullptr;
	}
}
