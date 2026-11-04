#include "duckdb/common/arrow/arrow.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/arrow/arrow_format.hpp"
#include "duckdb/function/table/arrow.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/query_parameters.hpp"
#include "fmt/format.h"

using duckdb::ArrowConverter;
using duckdb::ArrowFormat;
using duckdb::ArrowResultWrapper;
using duckdb::CClientArrowOptionsWrapper;
using duckdb::Connection;
using duckdb::DataChunk;
using duckdb::LogicalType;
using duckdb::PreparedStatementWrapper;
using duckdb::QueryParameters;
using duckdb::QueryResult;

namespace {

//! Every duckdb_arrow result is produced in the Arrow format, so the engine's worker threads build
//! the Arrow arrays and duckdb_query_arrow_array only hands them out
QueryParameters ArrowParameters() {
	return QueryParameters(duckdb::make_shared_ptr<ArrowFormat>(STANDARD_VECTOR_SIZE));
}

//! The rows a CHANGED_ROWS statement affected
idx_t ChangedRows(QueryResult &result) {
	if (result.HasError()) {
		return 0;
	}
	if (result.GetStatementProperties().return_type != duckdb::StatementReturnType::CHANGED_ROWS) {
		return 0;
	}
	auto &types = result.GetTypes();
	if (types.size() != 1 || types[0].id() != duckdb::LogicalTypeId::BIGINT) {
		return 0;
	}
	auto &arrays = result.Collection<ArrowFormat>();
	if (arrays.empty()) {
		return 0;
	}
	return ArrowFormat::ChangedRows(arrays.front()->arrow_array);
}

//! Rejects an array whose shape disagrees with the resolved schema before anything reads it, so a malformed
//! array is an input error rather than a crash inside the conversion (mirrors CV2ValidateArrowArray).
void ValidateArrowArrayShape(duckdb::ArrowTableSchema &arrow_table, const ArrowArray &array) {
	auto column_count = arrow_table.GetTypes().size();
	if (duckdb::NumericCast<idx_t>(array.n_children) != column_count) {
		throw duckdb::InvalidInputException("Arrow array child count does not match the schema's column count");
	}
	if (column_count > 0 && !array.children) {
		throw duckdb::InvalidInputException("Arrow array has null children");
	}
	for (idx_t i = 0; i < column_count; i++) {
		auto *child_array = array.children[i];
		if (!child_array || !child_array->release) {
			throw duckdb::InvalidInputException("Arrow array child is null or already released");
		}
		if (child_array->length != array.length) {
			throw duckdb::InvalidInputException("Arrow array child length does not match the array length");
		}
	}
}

} // namespace

duckdb_error_data duckdb_to_arrow_schema(duckdb_arrow_options arrow_options, duckdb_logical_type *types,
                                         const char **names, idx_t column_count, struct ArrowSchema *out_schema) {
	if (!arrow_options || !out_schema) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, "Invalid argument(s) to duckdb_to_arrow_schema");
	}
	// types and names can be nullptr when column_count is 0
	if (column_count > 0 && (!types || !names)) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, "Invalid argument(s) to duckdb_to_arrow_schema");
	}

	duckdb::vector<LogicalType> schema_types;
	duckdb::vector<std::string> schema_names;
	for (idx_t i = 0; i < column_count; i++) {
		schema_names.emplace_back(names[i]);
		schema_types.emplace_back(*reinterpret_cast<duckdb::LogicalType *>(types[i]));
	}
	const auto arrow_options_wrapper = reinterpret_cast<CClientArrowOptionsWrapper *>(arrow_options);
	try {
		ArrowConverter::ToArrowSchema(out_schema, schema_types, schema_names, arrow_options_wrapper->properties);
	} catch (const duckdb::Exception &ex) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, ex.what());
	} catch (const std::exception &ex) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, ex.what());
	} catch (...) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, "Unknown error occurred during conversion");
	}
	return nullptr;
}

duckdb_error_data duckdb_data_chunk_to_arrow(duckdb_arrow_options arrow_options, duckdb_data_chunk chunk,
                                             struct ArrowArray *out_arrow_array) {
	if (!arrow_options || !chunk || !out_arrow_array) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT,
		                                "Invalid argument(s) to duckdb_data_chunk_to_arrow");
	}
	auto dchunk = reinterpret_cast<duckdb::DataChunk *>(chunk);
	auto arrow_options_wrapper = reinterpret_cast<CClientArrowOptionsWrapper *>(arrow_options);
	auto extension_type_cast = duckdb::ArrowTypeExtensionData::GetExtensionTypes(
	    *arrow_options_wrapper->properties.client_context, dchunk->GetTypes());

	try {
		ArrowConverter::ToArrowArray(*dchunk, out_arrow_array, arrow_options_wrapper->properties, extension_type_cast);
	} catch (const duckdb::Exception &ex) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, ex.what());
	} catch (const std::exception &ex) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, ex.what());
	} catch (...) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, "Unknown error occurred during conversion");
	}
	return nullptr;
}

duckdb_error_data duckdb_schema_from_arrow(duckdb_connection connection, struct ArrowSchema *schema,
                                           duckdb_arrow_converted_schema *out_types) {
	if (!connection || !out_types || !schema) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT,
		                                "Invalid argument(s) to duckdb_data_chunk_to_arrow");
	}
	duckdb::vector<std::string> names;
	const auto conn = reinterpret_cast<Connection *>(connection);
	auto arrow_table = duckdb::make_uniq<duckdb::ArrowTableSchema>();
	try {
		duckdb::vector<LogicalType> return_types;
		duckdb::ArrowTableFunction::PopulateArrowTableSchema(*conn->context, *arrow_table, *schema);
	} catch (const duckdb::Exception &ex) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, ex.what());
	} catch (const std::exception &ex) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, ex.what());
	} catch (...) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, "Unknown error occurred during conversion");
	}
	*out_types = reinterpret_cast<duckdb_arrow_converted_schema>(arrow_table.release());
	return nullptr;
}

duckdb_error_data duckdb_data_chunk_from_arrow(duckdb_connection connection, struct ArrowArray *arrow_array,
                                               duckdb_arrow_converted_schema converted_schema,
                                               duckdb_data_chunk *out_chunk) {
	if (!connection || !converted_schema || !out_chunk || !arrow_array) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT,
		                                "Invalid argument(s) to duckdb_data_chunk_to_arrow");
	}
	auto arrow_table = reinterpret_cast<duckdb::ArrowTableSchema *>(converted_schema);
	auto conn = reinterpret_cast<Connection *>(connection);
	auto &types = arrow_table->GetTypes();

	try {
		ValidateArrowArrayShape(*arrow_table, *arrow_array);
	} catch (const duckdb::Exception &ex) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, ex.what());
	} catch (const std::exception &ex) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, ex.what());
	}

	auto dchunk = duckdb::make_uniq<duckdb::DataChunk>();
	dchunk->Initialize(duckdb::Allocator::DefaultAllocator(), types, duckdb::NumericCast<idx_t>(arrow_array->length));

	auto &arrow_types = arrow_table->GetColumns();
	dchunk->SetChildCardinality(duckdb::NumericCast<idx_t>(arrow_array->length));
	// All columns share ownership of the array, so it is only released once no column references it anymore
	auto owned_data = duckdb::make_shared_ptr<duckdb::ArrowArrayWrapper>();
	owned_data->arrow_array = *arrow_array;
	// We set it to nullptr to effectively transfer the ownership
	arrow_array->release = nullptr;
	for (idx_t i = 0; i < dchunk->ColumnCount(); i++) {
		auto &parent_array = *arrow_array;
		auto &array = parent_array.children[i];
		auto arrow_type = arrow_types.at(i);
		auto array_physical_type = arrow_type->GetPhysicalType();
		auto array_state = duckdb::make_uniq<duckdb::ArrowArrayScanState>(*conn->context);
		array_state->owned_data = owned_data;
		try {
			switch (array_physical_type) {
			case duckdb::ArrowArrayPhysicalType::DICTIONARY_ENCODED:
				duckdb::ArrowToDuckDBConversion::ColumnArrowToDuckDBDictionary(dchunk->data[i], *array, 0, *array_state,
				                                                               dchunk->size(), *arrow_type);
				break;
			case duckdb::ArrowArrayPhysicalType::RUN_END_ENCODED:
				duckdb::ArrowToDuckDBConversion::ColumnArrowToDuckDBRunEndEncoded(
				    dchunk->data[i], *array, 0, *array_state, dchunk->size(), *arrow_type);
				break;
			case duckdb::ArrowArrayPhysicalType::DEFAULT:
				duckdb::ArrowToDuckDBConversion::SetValidityMask(dchunk->data[i], *array, 0, dchunk->size(),
				                                                 parent_array.offset, -1);

				duckdb::ArrowToDuckDBConversion::ColumnArrowToDuckDB(dchunk->data[i], *array, 0, *array_state,
				                                                     dchunk->size(), *arrow_type);
				break;
			default:
				return duckdb_create_error_data(DUCKDB_ERROR_NOT_IMPLEMENTED,
				                                "Only Default Physical Types are currently supported");
			}
		} catch (const duckdb::Exception &ex) {
			return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, ex.what());
		} catch (const std::exception &ex) {
			return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, ex.what());
		} catch (...) {
			return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT, "Unknown error occurred during conversion");
		}
	}
	*out_chunk = reinterpret_cast<duckdb_data_chunk>(dchunk.release());
	return nullptr;
}

void duckdb_destroy_arrow_converted_schema(duckdb_arrow_converted_schema *arrow_converted_schema) {
	if (arrow_converted_schema && *arrow_converted_schema) {
		auto converted_schema = reinterpret_cast<duckdb::ArrowTableSchema *>(*arrow_converted_schema);
		delete converted_schema;
		*arrow_converted_schema = nullptr;
	}
}

duckdb_state duckdb_query_arrow(duckdb_connection connection, const char *query, duckdb_arrow *out_result) {
	Connection *conn = (Connection *)connection;
	auto wrapper = new ArrowResultWrapper();
	try {
		wrapper->result = conn->context->Query(query, ArrowParameters());
	} catch (...) {
		delete wrapper;
		return DuckDBError;
	}
	*out_result = (duckdb_arrow)wrapper;
	return !wrapper->result->HasError() ? DuckDBSuccess : DuckDBError;
}

duckdb_state duckdb_query_arrow_schema(duckdb_arrow result, duckdb_arrow_schema *out_schema) {
	if (!out_schema) {
		return DuckDBSuccess;
	}
	auto wrapper = reinterpret_cast<ArrowResultWrapper *>(result);
	try {
		ArrowConverter::ToArrowSchema((ArrowSchema *)*out_schema, wrapper->result->GetTypes(),
		                              duckdb::IdentifiersToStrings(wrapper->result->GetNames()),
		                              wrapper->result->client_properties);
	} catch (...) {
		return DuckDBError;
	}
	return DuckDBSuccess;
}

duckdb_state duckdb_prepared_arrow_schema(duckdb_prepared_statement prepared, duckdb_arrow_schema *out_schema) {
	if (!out_schema) {
		return DuckDBSuccess;
	}
	auto wrapper = reinterpret_cast<PreparedStatementWrapper *>(prepared);
	if (!wrapper || !wrapper->statement) {
		return DuckDBError;
	}
	auto context = wrapper->statement->TryGetContext();
	if (!context) {
		return DuckDBError;
	}
	auto properties = context->GetClientProperties();
	duckdb::vector<duckdb::LogicalType> prepared_types;
	duckdb::vector<duckdb::string> prepared_names;

	auto count = wrapper->statement->GetParameterCount();
	for (idx_t i = 0; i < count; i++) {
		// Every prepared parameter type is UNKNOWN, which we need to map to NULL according to the spec of
		// 'AdbcStatementGetParameterSchema'
		const auto type = LogicalType::SQLNULL;

		// FIXME: we don't support named parameters yet, but when we do, this needs to be updated
		auto name = std::to_string(i);
		prepared_types.push_back(type);
		prepared_names.push_back(name);
	}

	auto result_schema = (ArrowSchema *)*out_schema;
	if (!result_schema) {
		return DuckDBError;
	}

	if (result_schema->release) {
		// Need to release the existing schema before we overwrite it
		result_schema->release(result_schema);
		D_ASSERT(!result_schema->release);
	}

	ArrowConverter::ToArrowSchema(result_schema, prepared_types, prepared_names, properties);
	return DuckDBSuccess;
}

duckdb_state duckdb_query_arrow_array(duckdb_arrow result, duckdb_arrow_array *out_array) {
	if (!out_array) {
		return DuckDBSuccess;
	}
	auto wrapper = reinterpret_cast<ArrowResultWrapper *>(result);
	duckdb::unique_ptr<duckdb::ArrowArrayWrapper> array;
	try {
		array = wrapper->result->Fetch<ArrowFormat>();
	} catch (std::exception &ex) { // LCOV_EXCL_START
		wrapper->result->SetError(duckdb::ErrorData(ex));
		return DuckDBError;
	} catch (...) {
		wrapper->result->SetError(duckdb::ErrorData("Unknown error in Fetch"));
		return DuckDBError;
	} // LCOV_EXCL_STOP
	if (!array) {
		return DuckDBSuccess;
	}
	array->MoveTo(*reinterpret_cast<ArrowArray *>(*out_array));
	return DuckDBSuccess;
}

void duckdb_result_arrow_array(duckdb_result result, duckdb_data_chunk chunk, duckdb_arrow_array *out_array) {
	if (!out_array) {
		return;
	}
	auto dchunk = reinterpret_cast<duckdb::DataChunk *>(chunk);
	auto &result_data = *(reinterpret_cast<duckdb::DuckDBResultData *>(result.internal_data));
	auto &client_properties = result_data.GetClientProperties();
	auto extension_type_cast =
	    duckdb::ArrowTypeExtensionData::GetExtensionTypes(*client_properties.client_context, result_data.GetTypes());

	ArrowConverter::ToArrowArray(*dchunk, reinterpret_cast<ArrowArray *>(*out_array), client_properties,
	                             extension_type_cast);
}

idx_t duckdb_arrow_row_count(duckdb_arrow result) {
	auto wrapper = reinterpret_cast<ArrowResultWrapper *>(result);
	if (wrapper->result->HasError()) {
		return 0;
	}
	return wrapper->result->RowCount();
}

idx_t duckdb_arrow_column_count(duckdb_arrow result) {
	auto wrapper = reinterpret_cast<ArrowResultWrapper *>(result);
	return wrapper->result->ColumnCount();
}

idx_t duckdb_arrow_rows_changed(duckdb_arrow result) {
	auto wrapper = reinterpret_cast<ArrowResultWrapper *>(result);
	try {
		return ChangedRows(*wrapper->result);
	} catch (...) { // LCOV_EXCL_START
		return 0;
	} // LCOV_EXCL_STOP
}

const char *duckdb_query_arrow_error(duckdb_arrow result) {
	auto wrapper = reinterpret_cast<ArrowResultWrapper *>(result);
	return wrapper->result->GetError().c_str();
}

void duckdb_destroy_arrow(duckdb_arrow *result) {
	if (*result) {
		auto wrapper = reinterpret_cast<ArrowResultWrapper *>(*result);
		delete wrapper;
		*result = nullptr;
	}
}

void duckdb_destroy_arrow_stream(duckdb_arrow_stream *stream_p) {
	auto stream = reinterpret_cast<ArrowArrayStream *>(*stream_p);
	if (!stream) {
		return;
	}
	if (stream->release) {
		stream->release(stream);
	}
	D_ASSERT(!stream->release);

	delete stream;
	*stream_p = nullptr;
}

duckdb_state duckdb_execute_prepared_arrow(duckdb_prepared_statement prepared_statement, duckdb_arrow *out_result) {
	auto wrapper = reinterpret_cast<PreparedStatementWrapper *>(prepared_statement);
	if (!wrapper || !wrapper->statement || wrapper->statement->HasError() || !out_result) {
		return DuckDBError;
	}
	auto arrow_wrapper = new ArrowResultWrapper();
	try {
		arrow_wrapper->result = wrapper->statement->Execute(wrapper->values, ArrowParameters());
	} catch (...) {
		delete arrow_wrapper;
		return DuckDBError;
	}
	*out_result = reinterpret_cast<duckdb_arrow>(arrow_wrapper);
	return !arrow_wrapper->result->HasError() ? DuckDBSuccess : DuckDBError;
}

namespace arrow_array_stream_wrapper {
namespace {
struct PrivateData {
	ArrowSchema *schema;
	ArrowArray *array;
	bool done = false;
};

// LCOV_EXCL_START
// This function is never called, but used to set ArrowSchema's release functions to a non-null NOOP.
void EmptySchemaRelease(ArrowSchema *schema) {
	schema->release = nullptr;
}
// LCOV_EXCL_STOP

void EmptyArrayRelease(ArrowArray *array) {
	array->release = nullptr;
}

void EmptyStreamRelease(ArrowArrayStream *stream) {
	stream->release = nullptr;
}

void FactoryGetSchema(ArrowArrayStream *stream, ArrowSchema &schema) {
	stream->get_schema(stream, &schema);

	// Need to nullify the root schema's release function here, because streams don't allow us to set the release
	// function. For the schema's children, we nullify the release functions in `duckdb_arrow_scan`, so we don't need to
	// handle them again here. We set this to nullptr and not EmptySchemaRelease to prevent ArrowSchemaWrapper's
	// destructor from destroying the schema (it's the caller's responsibility).
	schema.release = nullptr;
}

int GetSchema(struct ArrowArrayStream *stream, struct ArrowSchema *out) {
	auto private_data = static_cast<arrow_array_stream_wrapper::PrivateData *>((stream->private_data));
	if (private_data->schema == nullptr) {
		return DuckDBError;
	}

	*out = *private_data->schema;
	out->release = EmptySchemaRelease;
	return DuckDBSuccess;
}

int GetNext(struct ArrowArrayStream *stream, struct ArrowArray *out) {
	auto private_data = static_cast<arrow_array_stream_wrapper::PrivateData *>((stream->private_data));
	*out = *private_data->array;
	if (private_data->done) {
		out->release = nullptr;
	} else {
		out->release = EmptyArrayRelease;
	}

	private_data->done = true;
	return DuckDBSuccess;
}

class CArrowScanFactory : public duckdb::ArrowScanFactory {
public:
	explicit CArrowScanFactory(ArrowArrayStream &stream_p) : stream(stream_p) {
	}

	void GetSchema(ArrowSchema &schema) override {
		FactoryGetSchema(&stream.get(), schema);
	}

	duckdb::unique_ptr<duckdb::ArrowArrayStreamWrapper>
	ProduceStream(duckdb::ArrowStreamParameters &parameters) override {
		auto result = duckdb::make_uniq<duckdb::ArrowArrayStreamWrapper>();
		result->arrow_array_stream = stream.get();
		result->arrow_array_stream.release = EmptyStreamRelease;
		return result;
	}

private:
	//! The caller retains ownership of the stream and its arrays.
	duckdb::reference<ArrowArrayStream> stream;
};

// LCOV_EXCL_START
// This function is never be called, because it's used to construct a stream wrapping around a caller-supplied
// ArrowArray. Thus, the stream itself cannot produce an error.
const char *GetLastError(struct ArrowArrayStream *stream) {
	return nullptr;
}
// LCOV_EXCL_STOP

void Release(struct ArrowArrayStream *stream) {
	if (stream->private_data != nullptr) {
		delete reinterpret_cast<PrivateData *>(stream->private_data);
	}

	stream->private_data = nullptr;
	stream->release = nullptr;
}

duckdb_state Ingest(duckdb_connection connection, const char *table_name, struct ArrowArrayStream *input) {
	try {
		auto cconn = reinterpret_cast<duckdb::Connection *>(connection);
		if (!input) {
			return DuckDBError;
		}
		auto factory = duckdb::make_shared_ptr<CArrowScanFactory>(*input);
		cconn->TableFunction("arrow_scan", {}, {}, std::move(factory))
		    ->CreateView(duckdb::Identifier(table_name), true, false);
	} catch (...) { // LCOV_EXCL_START
		// Tried covering this in tests, but it proved harder than expected. At the time of writing:
		// - Passing any name to `CreateView` worked without throwing an exception
		// - Passing a null Arrow array worked without throwing an exception
		// - Passing an invalid schema (without any columns) led to an InternalException with SIGABRT, which is meant to
		//   be un-catchable. This case likely needs to be handled gracefully within `arrow_scan`.
		// Ref: https://discord.com/channels/909674491309850675/921100573732909107/1115230468699336785
		return DuckDBError;
	} // LCOV_EXCL_STOP

	return DuckDBSuccess;
}
} // namespace
} // namespace arrow_array_stream_wrapper

duckdb_state duckdb_arrow_scan(duckdb_connection connection, const char *table_name, duckdb_arrow_stream arrow) {
	auto stream = reinterpret_cast<ArrowArrayStream *>(arrow);

	// Backup release functions - we nullify children schema release functions because we don't want to release on
	// behalf of the caller, downstream in our code. Note that Arrow releases target immediate children, but aren't
	// recursive. So we only back up immediate children here and restore their functions.
	ArrowSchema schema;
	if (stream->get_schema(stream, &schema) == DuckDBError) {
		return DuckDBError;
	}

	typedef void (*release_fn_t)(ArrowSchema *);
	std::vector<release_fn_t> release_fns(duckdb::NumericCast<idx_t>(schema.n_children));
	for (idx_t i = 0; i < duckdb::NumericCast<idx_t>(schema.n_children); i++) {
		auto child = schema.children[i];
		release_fns[i] = child->release;
		child->release = arrow_array_stream_wrapper::EmptySchemaRelease;
	}

	auto ret = arrow_array_stream_wrapper::Ingest(connection, table_name, stream);

	// Restore release functions.
	for (idx_t i = 0; i < duckdb::NumericCast<idx_t>(schema.n_children); i++) {
		schema.children[i]->release = release_fns[i];
	}

	return ret;
}

duckdb_state duckdb_arrow_array_scan(duckdb_connection connection, const char *table_name,
                                     duckdb_arrow_schema arrow_schema, duckdb_arrow_array arrow_array,
                                     duckdb_arrow_stream *out_stream) {
	auto private_data = new arrow_array_stream_wrapper::PrivateData;
	private_data->schema = reinterpret_cast<ArrowSchema *>(arrow_schema);
	private_data->array = reinterpret_cast<ArrowArray *>(arrow_array);
	private_data->done = false;

	ArrowArrayStream *stream;
	try {
		stream = new ArrowArrayStream;
	} catch (...) {
		delete private_data;
		return DuckDBError;
	}
	try {
		*out_stream = reinterpret_cast<duckdb_arrow_stream>(stream);
		stream->get_schema = arrow_array_stream_wrapper::GetSchema;
		stream->get_next = arrow_array_stream_wrapper::GetNext;
		stream->get_last_error = arrow_array_stream_wrapper::GetLastError;
		stream->release = arrow_array_stream_wrapper::Release;
		stream->private_data = private_data;

		return duckdb_arrow_scan(connection, table_name, reinterpret_cast<duckdb_arrow_stream>(stream));
	} catch (...) {
		delete private_data;
		delete stream;
		return DuckDBError;
	}
}
