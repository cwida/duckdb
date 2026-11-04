#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

//----------------------------------------------------------------------------------------------------------------------
// Bind
//----------------------------------------------------------------------------------------------------------------------
namespace {

struct WriteBlobBindData final : public TableFunctionData {
	FileCompressionType compression_type = FileCompressionType::AUTO_DETECT;
};

string ParseStringOption(const Value &value, const Identifier &option_name) {
	if (value.IsNull()) {
		return string();
	}
	if (value.type().id() == LogicalTypeId::LIST) {
		auto &children = ListValue::GetChildren(value);
		if (children.size() != 1) {
			throw BinderException("%s expects a single argument as a string value", option_name);
		}
		return ParseStringOption(children[0], option_name);
	}
	if (value.type().id() != LogicalTypeId::VARCHAR) {
		throw BinderException("%s expects a string argument!", option_name);
	}
	return value.GetValue<string>();
}

unique_ptr<FunctionData> WriteBlobBind(ClientContext &context, CopyFunctionBindInput &input,
                                       const vector<Identifier> &names, const vector<LogicalType> &sql_types) {
	if (sql_types.size() != 1 || sql_types.back().id() != LogicalTypeId::BLOB) {
		throw BinderException("\"COPY (FORMAT BLOB)\" only supports a single BLOB column");
	}

	auto result = make_uniq<WriteBlobBindData>();

	for (auto &[option_name, option_values] : input.info.options) {
		if (option_name == "compression") {
			if (option_values.empty()) {
				throw BinderException("COPY (FORMAT BLOB) option %s requires a value", SQLIdentifier(option_name));
			}
			auto compression_str = ParseStringOption(option_values[0], option_name);
			result->compression_type = FileCompressionType(compression_str);
		} else {
			throw BinderException("Unrecognized option for COPY (FORMAT BLOB): %s", SQLIdentifier(option_name));
		}
	}

	return std::move(result);
}

//----------------------------------------------------------------------------------------------------------------------
// Global State
//----------------------------------------------------------------------------------------------------------------------
struct WriteBlobGlobalState final : public GlobalFunctionData {
	unique_ptr<FileHandle> handle;
	mutex lock;
};

unique_ptr<GlobalFunctionData> WriteBlobInitializeGlobal(ClientContext &context, FunctionData &bind_data,
                                                         const string &file_path) {
	auto &bdata = bind_data.Cast<WriteBlobBindData>();
	auto &fs = FileSystem::GetFileSystem(context);

	auto flags = FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW | bdata.compression_type;
	auto handle = fs.OpenFile(file_path, flags);

	auto result = make_uniq<WriteBlobGlobalState>();
	result->handle = std::move(handle);

	return std::move(result);
}

//----------------------------------------------------------------------------------------------------------------------
// Local State
//----------------------------------------------------------------------------------------------------------------------
struct WriteBlobLocalState final : public LocalFunctionData {};

unique_ptr<LocalFunctionData> WriteBlobInitializeLocal(ExecutionContext &context, FunctionData &bind_data) {
	return make_uniq_base<LocalFunctionData, WriteBlobLocalState>();
}

void WriteBlobData(QueryContext &query_context, FileHandle &handle, data_ptr_t blob_ptr, idx_t blob_len) {
	while (blob_len > 0) {
		auto written = handle.Write(query_context, blob_ptr, blob_len);
		if (written <= 0) {
			throw IOException("Failed to write to file!");
		}
		blob_ptr += written;
		blob_len -= static_cast<idx_t>(written);
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Sink
//----------------------------------------------------------------------------------------------------------------------
void WriteBlobSink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                   LocalFunctionData &lstate, DataChunk &input) {
	D_ASSERT(input.ColumnCount() == 1);

	auto &state = gstate.Cast<WriteBlobGlobalState>();
	lock_guard<mutex> glock(state.lock);

	auto &handle = state.handle;

	QueryContext query_context(context.client);

	for (auto entry : input.data[0].Values<string_t>()) {
		if (entry.IsValid()) {
			auto &blob = entry.GetValue();
			WriteBlobData(query_context, *handle, data_ptr_cast(blob.GetDataWriteable()), blob.GetSize());
		}
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Prepare Batch
//----------------------------------------------------------------------------------------------------------------------
struct WriteBlobPreparedBatch final : public PreparedBatchData {
	vector<string> blobs;
};

unique_ptr<PreparedBatchData> WriteBlobPrepareBatch(ClientContext &context, FunctionData &bind_data,
                                                    GlobalFunctionData &gstate,
                                                    unique_ptr<ColumnDataCollection> collection) {
	auto result = make_uniq<WriteBlobPreparedBatch>();

	for (auto &chunk : collection->Chunks()) {
		D_ASSERT(chunk.ColumnCount() == 1);
		for (auto entry : chunk.data[0].Values<string_t>()) {
			if (!entry.IsValid()) {
				continue;
			}
			auto &blob = entry.GetValue();
			result->blobs.emplace_back(blob.GetDataWriteable(), blob.GetSize());
		}
	}

	return std::move(result);
}

//----------------------------------------------------------------------------------------------------------------------
// Flush Batch
//----------------------------------------------------------------------------------------------------------------------
void WriteBlobFlushBatch(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                         PreparedBatchData &batch) {
	auto &state = gstate.Cast<WriteBlobGlobalState>();
	auto &blob_batch = batch.Cast<WriteBlobPreparedBatch>();
	lock_guard<mutex> glock(state.lock);

	QueryContext query_context(context);
	for (auto &blob : blob_batch.blobs) {
		WriteBlobData(query_context, *state.handle, data_ptr_cast(blob.data()), blob.size());
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Combine
//----------------------------------------------------------------------------------------------------------------------
void WriteBlobCombine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                      LocalFunctionData &lstate) {
}

//----------------------------------------------------------------------------------------------------------------------
// Finalize
//----------------------------------------------------------------------------------------------------------------------
void WriteBlobFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate) {
	auto &state = gstate.Cast<WriteBlobGlobalState>();
	lock_guard<mutex> glock(state.lock);

	state.handle->Close();
}

} // namespace

//----------------------------------------------------------------------------------------------------------------------
// Register
//----------------------------------------------------------------------------------------------------------------------
void BuiltinFunctions::RegisterCopyFunctions() {
	CopyFunction info("blob");
	info.copy_to_bind = WriteBlobBind;
	info.copy_to_initialize_local = WriteBlobInitializeLocal;
	info.copy_to_initialize_global = WriteBlobInitializeGlobal;
	info.copy_to_sink = WriteBlobSink;
	info.copy_to_combine = WriteBlobCombine;
	info.copy_to_finalize = WriteBlobFinalize;
	info.prepare_batch = WriteBlobPrepareBatch;
	info.flush_batch = WriteBlobFlushBatch;
	info.extension = "blob";

	AddFunction(info);
}

} // namespace duckdb
