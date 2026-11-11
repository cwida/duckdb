#include "capi_tester.hpp"
#include "duckdb/common/arrow/arrow_wrapper.hpp"
#include "duckdb/common/arrow/arrow_appender.hpp"
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/types/arrow_string_view_type.hpp"

using namespace duckdb;

TEST_CASE("Test arrow in C API", "[capi][arrow]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	duckdb_prepared_statement stmt = nullptr;
	duckdb_arrow arrow_result = nullptr;

	// open the database in in-memory mode
	REQUIRE(tester.OpenDatabase(nullptr));

	SECTION("test rows changed") {
		REQUIRE_NO_FAIL(tester.Query("CREATE TABLE test(a INTEGER);"));
		auto state = duckdb_query_arrow(tester.connection, "INSERT INTO test VALUES (1), (2);", &arrow_result);
		REQUIRE(state == DuckDBSuccess);
		REQUIRE(duckdb_arrow_rows_changed(arrow_result) == 2);
		duckdb_destroy_arrow(&arrow_result);
		REQUIRE_NO_FAIL(tester.Query("DROP TABLE test;"));
	}

	SECTION("test rows changed after the arrays are drained") {
		REQUIRE_NO_FAIL(tester.Query("CREATE TABLE test(a INTEGER);"));
		REQUIRE_NO_FAIL(tester.Query("INSERT INTO test VALUES (1), (2), (3);"));
		auto state = duckdb_query_arrow(tester.connection, "UPDATE test SET a = a + 1;", &arrow_result);
		REQUIRE(state == DuckDBSuccess);

		// Draining first is what a client does; fetching copies, so the count is still there
		while (true) {
			ArrowArray arrow_array;
			arrow_array.Init();
			auto arrow_array_ptr = &arrow_array;
			state = duckdb_query_arrow_array(arrow_result, reinterpret_cast<duckdb_arrow_array *>(&arrow_array_ptr));
			REQUIRE(state == DuckDBSuccess);
			if (arrow_array.length == 0) {
				break;
			}
			arrow_array.release(arrow_array_ptr);
		}
		REQUIRE(duckdb_arrow_rows_changed(arrow_result) == 3);

		duckdb_destroy_arrow(&arrow_result);
		REQUIRE_NO_FAIL(tester.Query("DROP TABLE test;"));
	}

	SECTION("test rows changed by a statement that matches nothing") {
		REQUIRE_NO_FAIL(tester.Query("CREATE TABLE test(a INTEGER);"));
		REQUIRE_NO_FAIL(tester.Query("INSERT INTO test VALUES (1);"));
		auto state = duckdb_query_arrow(tester.connection, "UPDATE test SET a = a + 1 WHERE a > 1;", &arrow_result);
		REQUIRE(state == DuckDBSuccess);
		// The zero still arrives as a count row, so it is read from the array rather than defaulted
		REQUIRE(duckdb_arrow_row_count(arrow_result) == 1);
		REQUIRE(duckdb_arrow_rows_changed(arrow_result) == 0);
		duckdb_destroy_arrow(&arrow_result);
		REQUIRE_NO_FAIL(tester.Query("DROP TABLE test;"));
	}

	SECTION("test query arrow") {
		auto state = duckdb_query_arrow(tester.connection, "SELECT 42 AS VALUE, [1,2,3,4,5] AS LST;", &arrow_result);
		REQUIRE(state == DuckDBSuccess);
		REQUIRE(duckdb_arrow_row_count(arrow_result) == 1);
		REQUIRE(duckdb_arrow_column_count(arrow_result) == 2);
		REQUIRE(duckdb_arrow_rows_changed(arrow_result) == 0);

		// query the arrow schema
		ArrowSchema arrow_schema;
		arrow_schema.Init();
		auto arrow_schema_ptr = &arrow_schema;

		state = duckdb_query_arrow_schema(arrow_result, reinterpret_cast<duckdb_arrow_schema *>(&arrow_schema_ptr));
		REQUIRE(state == DuckDBSuccess);
		REQUIRE(string(arrow_schema.name) == "duckdb_query_result");
		if (arrow_schema.release) {
			arrow_schema.release(arrow_schema_ptr);
		}

		// query array data
		ArrowArray arrow_array;
		arrow_array.Init();
		auto arrow_array_ptr = &arrow_array;

		state = duckdb_query_arrow_array(arrow_result, reinterpret_cast<duckdb_arrow_array *>(&arrow_array_ptr));
		REQUIRE(state == DuckDBSuccess);
		REQUIRE(arrow_array.length == 1);
		REQUIRE(arrow_array.release != nullptr);
		arrow_array.release(arrow_array_ptr);

		duckdb_arrow_array null_array = nullptr;
		state = duckdb_query_arrow_array(arrow_result, &null_array);
		REQUIRE(state == DuckDBSuccess);
		REQUIRE(null_array == nullptr);

		// destroy the arrow result
		duckdb_destroy_arrow(&arrow_result);
	}

	SECTION("test multiple chunks") {
		// create a table that consists of multiple chunks
		REQUIRE_NO_FAIL(tester.Query("CREATE TABLE test(a INTEGER);"));
		REQUIRE_NO_FAIL(
		    tester.Query("INSERT INTO test SELECT i FROM (VALUES (1), (2), (3), (4), (5)) t(i), range(500);"));
		auto state = duckdb_query_arrow(tester.connection, "SELECT CAST(a AS INTEGER) AS a FROM test ORDER BY a;",
		                                &arrow_result);
		REQUIRE(state == DuckDBSuccess);

		// query the arrow schema
		ArrowSchema arrow_schema;
		arrow_schema.Init();
		auto arrow_schema_ptr = &arrow_schema;

		state = duckdb_query_arrow_schema(arrow_result, reinterpret_cast<duckdb_arrow_schema *>(&arrow_schema_ptr));
		REQUIRE(state == DuckDBSuccess);
		REQUIRE(arrow_schema.release != nullptr);
		arrow_schema.release(arrow_schema_ptr);

		int total_count = 0;
		while (true) {
			// query array data
			ArrowArray arrow_array;
			arrow_array.Init();
			auto arrow_array_ptr = &arrow_array;

			state = duckdb_query_arrow_array(arrow_result, reinterpret_cast<duckdb_arrow_array *>(&arrow_array_ptr));
			REQUIRE(state == DuckDBSuccess);

			if (arrow_array.length == 0) {
				// nothing to release
				REQUIRE(total_count == 2500);
				break;
			}

			REQUIRE(arrow_array.length > 0);
			total_count += arrow_array.length;
			REQUIRE(arrow_array.release != nullptr);
			arrow_array.release(arrow_array_ptr);
		}

		// destroy the arrow result
		duckdb_destroy_arrow(&arrow_result);
		REQUIRE_NO_FAIL(tester.Query("DROP TABLE test;"));
	}

	SECTION("test prepare query arrow") {
		auto state = duckdb_prepare(tester.connection, "SELECT CAST($1 AS BIGINT)", &stmt);
		REQUIRE(state == DuckDBSuccess);
		REQUIRE(stmt != nullptr);
		REQUIRE(duckdb_bind_int64(stmt, 1, 42) == DuckDBSuccess);

		// prepare and execute the arrow schema
		ArrowSchema prepared_schema;
		prepared_schema.Init();
		auto prepared_schema_ptr = &prepared_schema;

		state = duckdb_prepared_arrow_schema(stmt, reinterpret_cast<duckdb_arrow_schema *>(&prepared_schema_ptr));
		REQUIRE(state == DuckDBSuccess);
		REQUIRE(string(prepared_schema.format) == "+s");
		REQUIRE(duckdb_execute_prepared_arrow(stmt, nullptr) == DuckDBError);
		REQUIRE(duckdb_execute_prepared_arrow(stmt, &arrow_result) == DuckDBSuccess);
		REQUIRE(prepared_schema.release != nullptr);
		prepared_schema.release(prepared_schema_ptr);

		// query the arrow schema
		ArrowSchema arrow_schema;
		arrow_schema.Init();
		auto arrow_schema_ptr = &arrow_schema;

		state = duckdb_query_arrow_schema(arrow_result, reinterpret_cast<duckdb_arrow_schema *>(&arrow_schema_ptr));
		REQUIRE(state == DuckDBSuccess);
		REQUIRE(string(arrow_schema.format) == "+s");
		REQUIRE(arrow_schema.release != nullptr);
		arrow_schema.release(arrow_schema_ptr);

		ArrowArray arrow_array;
		arrow_array.Init();
		auto arrow_array_ptr = &arrow_array;

		state = duckdb_query_arrow_array(arrow_result, reinterpret_cast<duckdb_arrow_array *>(&arrow_array_ptr));
		REQUIRE(state == DuckDBSuccess);
		REQUIRE(arrow_array.length == 1);
		REQUIRE(arrow_array.release);
		arrow_array.release(arrow_array_ptr);

		duckdb_destroy_arrow(&arrow_result);
		duckdb_destroy_prepare(&stmt);
	}

	SECTION("test scan") {
		const auto logical_types = duckdb::vector<LogicalType> {LogicalType(LogicalTypeId::INTEGER)};
		const auto column_names = duckdb::vector<string> {"value"};

		// arrow schema, release after use
		ArrowSchema arrow_schema;
		arrow_schema.Init();
		auto arrow_schema_ptr = &arrow_schema;

		ClientProperties options = (reinterpret_cast<Connection *>(tester.connection)->context->GetClientProperties());
		duckdb::ArrowConverter::ToArrowSchema(arrow_schema_ptr, logical_types, column_names, options);

		ArrowArray arrow_array;
		arrow_array.Init();
		auto arrow_array_ptr = &arrow_array;

		SECTION("empty array") {
			// Create an empty view with a `value` column.
			string view_name = "foo_empty_table";

			// arrow array scan, destroy out_stream after use
			ArrowArrayStream *arrow_array_stream;
			auto out_stream = reinterpret_cast<duckdb_arrow_stream *>(&arrow_array_stream);
			auto state = duckdb_arrow_array_scan(tester.connection, view_name.c_str(),
			                                     reinterpret_cast<duckdb_arrow_schema>(arrow_schema_ptr),
			                                     reinterpret_cast<duckdb_arrow_array>(arrow_array_ptr), out_stream);
			REQUIRE(state == DuckDBSuccess);

			// get the created view from the DB
			auto get_query = "SELECT * FROM " + view_name + ";";
			state = duckdb_prepare(tester.connection, get_query.c_str(), &stmt);
			REQUIRE(state == DuckDBSuccess);
			REQUIRE(stmt != nullptr);
			state = duckdb_execute_prepared_arrow(stmt, &arrow_result);
			REQUIRE(state == DuckDBSuccess);

			// recover the arrow array from the arrow result
			ArrowArray out_array;
			out_array.Init();
			auto out_array_ptr = &out_array;

			state = duckdb_query_arrow_array(arrow_result, reinterpret_cast<duckdb_arrow_array *>(&out_array_ptr));
			REQUIRE(state == DuckDBSuccess);
			REQUIRE(out_array.length == 0);
			REQUIRE(out_array.release == nullptr);

			duckdb_destroy_arrow_stream(out_stream);
			REQUIRE(arrow_array.release == nullptr);
		}

		SECTION("big array") {
			// Create a view with a `value` column containing 4096 values.
			int num_buffers = 2, size = STANDARD_VECTOR_SIZE * num_buffers;
			unordered_map<idx_t, const duckdb::shared_ptr<ArrowTypeExtensionData>> extension_type_cast;
			ArrowAppender appender(logical_types, size, options, extension_type_cast);
			Allocator allocator;

			auto data_chunks = std::vector<DataChunk>(num_buffers);
			for (int i = 0; i < num_buffers; i++) {
				auto data_chunk = &data_chunks[i];
				data_chunk->Initialize(allocator, logical_types, STANDARD_VECTOR_SIZE);
				auto &col = data_chunk->data[0];
				for (idx_t row = 0; row < STANDARD_VECTOR_SIZE; row++) {
					col.Append(duckdb::Value(i));
				}
				data_chunk->SetChildCardinality(STANDARD_VECTOR_SIZE);
				appender.Append(*data_chunk, 0, data_chunk->size(), data_chunk->size());
			}

			*arrow_array_ptr = appender.Finalize();

			// Create the view.
			string view_name = "foo_table";

			// arrow array scan, destroy out_stream after use
			ArrowArrayStream *arrow_array_stream;
			auto out_stream = reinterpret_cast<duckdb_arrow_stream *>(&arrow_array_stream);
			auto state = duckdb_arrow_array_scan(tester.connection, view_name.c_str(),
			                                     reinterpret_cast<duckdb_arrow_schema>(arrow_schema_ptr),
			                                     reinterpret_cast<duckdb_arrow_array>(arrow_array_ptr), out_stream);
			REQUIRE(state == DuckDBSuccess);

			// Get the created view from the DB.
			auto get_query = "SELECT * FROM " + view_name + ";";
			state = duckdb_prepare(tester.connection, get_query.c_str(), &stmt);
			REQUIRE(state == DuckDBSuccess);
			REQUIRE(stmt != nullptr);
			state = duckdb_execute_prepared_arrow(stmt, &arrow_result);
			REQUIRE(state == DuckDBSuccess);

			// Recover the arrow array from the arrow result.
			ArrowArray out_array;
			out_array.Init();
			auto out_array_ptr = &out_array;
			state = duckdb_query_arrow_array(arrow_result, reinterpret_cast<duckdb_arrow_array *>(&out_array_ptr));
			REQUIRE(state == DuckDBSuccess);
			REQUIRE(out_array.length == STANDARD_VECTOR_SIZE);
			REQUIRE(out_array.release != nullptr);
			out_array.release(out_array_ptr);

			out_array.Init();
			state = duckdb_query_arrow_array(arrow_result, reinterpret_cast<duckdb_arrow_array *>(&out_array_ptr));
			REQUIRE(state == DuckDBSuccess);
			REQUIRE(out_array.length == STANDARD_VECTOR_SIZE);
			REQUIRE(out_array.release != nullptr);
			out_array.release(out_array_ptr);

			out_array.Init();
			state = duckdb_query_arrow_array(arrow_result, reinterpret_cast<duckdb_arrow_array *>(&out_array_ptr));
			REQUIRE(state == DuckDBSuccess);
			REQUIRE(out_array.length == 0);
			REQUIRE(out_array.release == nullptr);

			duckdb_destroy_arrow_stream(out_stream);
			REQUIRE(arrow_array.release != nullptr);
		}

		SECTION("null schema") {
			// Creating a view with a null schema should fail gracefully.
			string view_name = "foo_empty_table_null_schema";

			// arrow array scan, destroy out_stream after use
			ArrowArrayStream *arrow_array_stream;
			auto out_stream = reinterpret_cast<duckdb_arrow_stream *>(&arrow_array_stream);
			auto state = duckdb_arrow_array_scan(tester.connection, view_name.c_str(), nullptr,
			                                     reinterpret_cast<duckdb_arrow_array>(arrow_array_ptr), out_stream);
			REQUIRE(state == DuckDBError);
			duckdb_destroy_arrow_stream(out_stream);
		}

		if (arrow_schema.release) {
			arrow_schema.release(arrow_schema_ptr);
		}
		if (arrow_array.release) {
			arrow_array.release(arrow_array_ptr);
		}

		duckdb_destroy_arrow(&arrow_result);
		duckdb_destroy_prepare(&stmt);
	}

	// FIXME: needs test for scanning a fixed size list
	// this likely requires nanoarrow to create the array to scan
}

namespace {

struct ArrowScanStream {
	static constexpr idx_t ROWS = 3;

	int32_t values[ROWS] = {1, 2, 3};
	const void *child_buffers[2] = {nullptr, values};
	const void *parent_buffers[1] = {nullptr};
	ArrowSchema child_schema = {};
	ArrowSchema *schema_children[1] = {&child_schema};
	ArrowArray child_array = {};
	ArrowArray *array_children[1] = {&child_array};
	bool emitted = false;
	ArrowArrayStream stream = {};

	ArrowScanStream() {
		stream.get_schema = GetSchema;
		stream.get_next = GetNext;
		stream.get_last_error = GetLastError;
		stream.release = ReleaseStream;
		stream.private_data = this;
	}

	static int GetSchema(ArrowArrayStream *stream, ArrowSchema *out) {
		auto &self = *static_cast<ArrowScanStream *>(stream->private_data);
		self.child_schema = {};
		self.child_schema.format = "i";
		self.child_schema.name = "value";
		self.child_schema.flags = ARROW_FLAG_NULLABLE;
		self.child_schema.release = ReleaseSchema;

		*out = {};
		out->format = "+s";
		out->n_children = 1;
		out->children = self.schema_children;
		out->release = ReleaseSchema;
		return 0;
	}

	static int GetNext(ArrowArrayStream *stream, ArrowArray *out) {
		auto &self = *static_cast<ArrowScanStream *>(stream->private_data);
		*out = {};
		if (self.emitted) {
			return 0;
		}

		self.child_array = {};
		self.child_array.length = ROWS;
		self.child_array.n_buffers = 2;
		self.child_array.buffers = self.child_buffers;
		self.child_array.release = ReleaseArray;

		out->length = ROWS;
		out->n_buffers = 1;
		out->buffers = self.parent_buffers;
		out->n_children = 1;
		out->children = self.array_children;
		out->release = ReleaseArray;
		self.emitted = true;
		return 0;
	}

	static const char *GetLastError(ArrowArrayStream *stream) {
		return nullptr;
	}

	static void ReleaseSchema(ArrowSchema *schema) {
		schema->release = nullptr;
	}

	static void ReleaseArray(ArrowArray *array) {
		array->release = nullptr;
	}

	static void ReleaseStream(ArrowArrayStream *stream) {
		stream->release = nullptr;
		stream->private_data = nullptr;
	}
};

// A two-column Arrow struct array (a = 1..4, b = 10..40) whose release callback poisons the buffers, so a read after
// release shows up as wrong values instead of silently reading freed memory. With nested_b, column b is wrapped in a
// struct, STRUCT(v BIGINT).
struct PoisoningArrowArray {
	static constexpr idx_t ROWS = 4;

	int64_t a[ROWS];
	int64_t b[ROWS];
	const void *buffers_a[2] = {nullptr, a};
	const void *buffers_b[2] = {nullptr, b};
	const void *buffers_parent[1] = {nullptr};
	const void *buffers_struct[1] = {nullptr};
	ArrowArray child_a;
	ArrowArray child_b;
	ArrowArray struct_b;
	ArrowArray *struct_children[1] = {&child_b};
	ArrowArray *children[2] = {&child_a, &child_b};
	ArrowArray array;
	idx_t release_calls = 0;

	explicit PoisoningArrowArray(bool nested_b = false) {
		for (idx_t i = 0; i < ROWS; i++) {
			a[i] = int64_t(i + 1);
			b[i] = int64_t((i + 1) * 10);
		}
		InitChild(child_a, buffers_a);
		InitChild(child_b, buffers_b);
		if (nested_b) {
			struct_b = {int64_t(ROWS), 0, 0, 1, 1, buffers_struct, struct_children, nullptr, ReleaseChild, nullptr};
			children[1] = &struct_b;
		}
		array = {int64_t(ROWS), 0, 0, 1, 2, buffers_parent, children, nullptr, ReleaseParent, this};
	}

	static void InitChild(ArrowArray &child, const void **buffers) {
		child = {int64_t(ROWS), 0, 0, 2, 0, buffers, nullptr, nullptr, ReleaseChild, nullptr};
	}
	static void ReleaseChild(ArrowArray *child) {
		for (int64_t i = 0; i < child->n_children; i++) {
			child->children[i]->release(child->children[i]);
		}
		child->release = nullptr;
	}
	static void ReleaseParent(ArrowArray *parent) {
		auto &self = *static_cast<PoisoningArrowArray *>(parent->private_data);
		self.release_calls++;
		memset(self.a, 0xAB, sizeof(self.a));
		memset(self.b, 0xAB, sizeof(self.b));
		for (int64_t i = 0; i < parent->n_children; i++) {
			parent->children[i]->release(parent->children[i]);
		}
		parent->release = nullptr;
	}
};

} // namespace

TEST_CASE("duckdb_arrow_scan creates a temporary view in a file-backed database", "[capi][arrow]") {
	auto path = TestCreatePath("capi_arrow_scan_temporary.db");
	DeleteDatabase(path);

	CAPITester tester;
	REQUIRE(tester.OpenDatabase(path.c_str()));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE destination(value INTEGER)"));
	REQUIRE_NO_FAIL(tester.Query("BEGIN TRANSACTION"));

	ArrowScanStream arrow_stream;
	REQUIRE(duckdb_arrow_scan(tester.connection, "arrow_input",
	                          reinterpret_cast<duckdb_arrow_stream>(&arrow_stream.stream)) == DuckDBSuccess);

	{
		auto result = tester.Query("SELECT temporary FROM duckdb_views() WHERE view_name = 'arrow_input'");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->row_count() == 1);
		REQUIRE(result->Fetch<bool>(0, 0));
	}

	duckdb_connection second_connection = nullptr;
	REQUIRE(duckdb_connect(tester.database, &second_connection) == DuckDBSuccess);
	duckdb_result second_result;
	REQUIRE(duckdb_query(second_connection, "SELECT count(*) FROM duckdb_views() WHERE view_name = 'arrow_input'",
	                     &second_result) == DuckDBSuccess);
	REQUIRE(duckdb_value_int64(&second_result, 0, 0) == 0);
	duckdb_destroy_result(&second_result);
	duckdb_disconnect(&second_connection);

	REQUIRE_NO_FAIL(tester.Query("INSERT INTO destination SELECT value FROM arrow_input"));
	REQUIRE_NO_FAIL(tester.Query("DROP VIEW arrow_input"));
	arrow_stream.stream.release(&arrow_stream.stream);
	REQUIRE(arrow_stream.stream.release == nullptr);
	REQUIRE_NO_FAIL(tester.Query("COMMIT"));

	tester.Cleanup();
	REQUIRE(tester.OpenDatabase(path.c_str()));
	{
		auto result = tester.Query("SELECT value FROM destination ORDER BY value");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->row_count() == ArrowScanStream::ROWS);
		for (idx_t row = 0; row < ArrowScanStream::ROWS; row++) {
			REQUIRE(result->Fetch<int32_t>(0, row) == NumericCast<int32_t>(row + 1));
		}
	}

	tester.Cleanup();
	DeleteDatabase(path);
}

TEST_CASE("Test C-API Arrow conversion functions", "[capi][arrow]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));

	SECTION("roundtrip: duckdb table -> arrow -> duckdb chunk, validate correctness") {
		// 1. Create and populate table
		REQUIRE_NO_FAIL(tester.Query("CREATE TABLE big_table(i INTEGER);"));
		REQUIRE_NO_FAIL(tester.Query("INSERT INTO big_table SELECT i FROM range(10000) tbl(i);"));

		// 2. Query the table and fetch all results as data chunks
		duckdb_result result;
		REQUIRE(duckdb_query(tester.connection, "SELECT i FROM big_table ORDER BY i", &result) == DuckDBSuccess);
		idx_t chunk_count = duckdb_result_chunk_count(result);
		idx_t total_rows = 0;
		std::vector<ArrowArray> arrow_arrays;
		std::vector<duckdb_data_chunk> duckdb_chunks;
		std::vector<int32_t> all_duckdb_values;
		std::vector<int32_t> all_arrow_values;

		// 3. For each chunk, convert to Arrow Array and collect values
		for (idx_t chunk_idx = 0; chunk_idx < chunk_count; chunk_idx++) {
			duckdb_data_chunk chunk = duckdb_result_get_chunk(result, chunk_idx);
			duckdb_chunks.push_back(chunk); // for later roundtrip
			idx_t chunk_size = duckdb_data_chunk_get_size(chunk);
			total_rows += chunk_size;
			auto vec = duckdb_data_chunk_get_vector(chunk, 0);
			auto data = static_cast<int32_t *>(duckdb_vector_get_data(vec));
			for (idx_t i = 0; i < chunk_size; i++) {
				all_duckdb_values.push_back(data[i]);
			}

			ArrowArray duckdb_arrow_array;
			duckdb_arrow_options arrow_options;
			duckdb_connection_get_arrow_options(tester.connection, &arrow_options);
			duckdb_error_data err = duckdb_data_chunk_to_arrow(arrow_options, chunk, &duckdb_arrow_array);
			duckdb_destroy_arrow_options(&arrow_options);
			REQUIRE(err == nullptr);
			arrow_arrays.push_back(duckdb_arrow_array);
		}
		REQUIRE(total_rows == 10000);
		REQUIRE(all_duckdb_values.size() == 10000);

		// 4. Prepare Arrow schema for roundtrip
		duckdb_logical_type type = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
		duckdb_logical_type types[1] = {type};
		const char *names[1] = {"i"};
		ArrowSchemaWrapper arrow_schema_wrapper;
		duckdb_arrow_options arrow_options;
		duckdb_connection_get_arrow_options(tester.connection, &arrow_options);
		duckdb_error_data err =
		    duckdb_to_arrow_schema(arrow_options, types, names, 1, &arrow_schema_wrapper.arrow_schema);
		duckdb_destroy_arrow_options(&arrow_options);
		REQUIRE(err == nullptr);
		duckdb_arrow_converted_schema converted_schema = nullptr;
		// Convert schema (simulate real use)

		err = duckdb_schema_from_arrow(tester.connection, &arrow_schema_wrapper.arrow_schema, &converted_schema);
		REQUIRE(err == nullptr);
		// 5. For each Arrow array, convert back to DuckDB chunk and validate
		for (size_t idx = 0, offset = 0; idx < arrow_arrays.size(); idx++) {
			ArrowArray *duckdb_arrow_array = &arrow_arrays[idx];
			// Prepare output chunk
			duckdb_data_chunk out_chunk;
			// Convert Arrow array to DuckDB chunk
			err = duckdb_data_chunk_from_arrow(tester.connection, duckdb_arrow_array, converted_schema, &out_chunk);
			REQUIRE(err == nullptr);
			idx_t chunk_size = duckdb_data_chunk_get_size(out_chunk);
			auto vec = duckdb_data_chunk_get_vector(out_chunk, 0);
			auto data = static_cast<int32_t *>(duckdb_vector_get_data(vec));
			for (idx_t i = 0; i < chunk_size; i++, offset++) {
				REQUIRE(data[i] == all_duckdb_values[offset]);
				all_arrow_values.push_back(data[i]);
			}
			duckdb_destroy_data_chunk(&out_chunk);
		}
		REQUIRE(all_arrow_values.size() == 10000);
		REQUIRE(all_arrow_values == all_duckdb_values);

		// 6. Cleanup
		duckdb_destroy_arrow_converted_schema(&converted_schema);
		for (auto arrow_array : arrow_arrays) {
			if (arrow_array.release) {
				arrow_array.release(&arrow_array);
			}
		}
		for (auto chunk : duckdb_chunks) {
			duckdb_destroy_data_chunk(&chunk);
		}
		duckdb_destroy_logical_type(&type);
		duckdb_destroy_result(&result);
	}

	SECTION("C-API Arrow Tess Null pointer inputs") {
		duckdb_error_data err;
		duckdb_logical_type type = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
		const char *names[1] = {"i"};
		// Test duckdb_to_arrow_schema
		ArrowSchema duckdb_arrow_schema;
		err = duckdb_to_arrow_schema(nullptr, &type, names, 1, &duckdb_arrow_schema);
		duckdb_arrow_options arrow_options;
		duckdb_connection_get_arrow_options(tester.connection, &arrow_options);
		REQUIRE(err != nullptr);
		duckdb_destroy_error_data(&err);
		err = duckdb_to_arrow_schema(arrow_options, nullptr, names, 1, &duckdb_arrow_schema);
		REQUIRE(err != nullptr);
		duckdb_destroy_error_data(&err);
		err = duckdb_to_arrow_schema(arrow_options, &type, nullptr, 1, &duckdb_arrow_schema);
		REQUIRE(err != nullptr);
		duckdb_destroy_error_data(&err);
		// Zero columns
		err = duckdb_to_arrow_schema(arrow_options, &type, names, 0, &duckdb_arrow_schema);
		REQUIRE(err == nullptr); // zero columns is allowed, but produces an empty schema
		if (duckdb_arrow_schema.release) {
			duckdb_arrow_schema.release(&duckdb_arrow_schema);
		}
		duckdb_destroy_logical_type(&type);

		// Test duckdb_data_chunk_to_arrow
		ArrowArray duckdb_arrow_array;

		duckdb_destroy_error_data(&err);
		err = duckdb_data_chunk_to_arrow(arrow_options, nullptr, &duckdb_arrow_array);
		REQUIRE(err != nullptr);
		duckdb_destroy_error_data(&err);
		err = duckdb_data_chunk_to_arrow(nullptr, nullptr, &duckdb_arrow_array);
		REQUIRE(err != nullptr);
		duckdb_destroy_error_data(&err);

		// Test duckdb_schema_from_arrow
		ArrowSchema schema;
		duckdb_arrow_converted_schema converted_schema = nullptr;
		err = duckdb_schema_from_arrow(nullptr, &schema, &converted_schema);
		REQUIRE(err != nullptr);
		duckdb_destroy_error_data(&err);
		err = duckdb_schema_from_arrow(tester.connection, &schema, nullptr);
		REQUIRE(err != nullptr);
		duckdb_destroy_error_data(&err);

		// Test duckdb_data_chunk_from_arrow
		ArrowArray arr;
		duckdb_data_chunk out_chunk = nullptr;
		err = duckdb_data_chunk_from_arrow(nullptr, &arr, converted_schema, &out_chunk);
		REQUIRE(err != nullptr);
		duckdb_destroy_error_data(&err);
		err = duckdb_data_chunk_from_arrow(tester.connection, &arr, nullptr, &out_chunk);
		REQUIRE(err != nullptr);
		duckdb_destroy_error_data(&err);
		err = duckdb_data_chunk_from_arrow(tester.connection, &arr, converted_schema, nullptr);
		REQUIRE(err != nullptr);
		duckdb_destroy_error_data(&err);
		duckdb_destroy_arrow_options(&arrow_options);
	}

	SECTION("data_chunk_from_arrow keeps the array alive while any of its columns is referenced") {
		duckdb_logical_type bigint = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
		const char *member_names[1] = {"v"};
		duckdb_logical_type struct_type = duckdb_create_struct_type(&bigint, member_names, 1);
		const char *names[2] = {"a", "b"};

		// Column b is a plain BIGINT first, then STRUCT(v BIGINT), whose data lives in a nested child vector.
		for (bool nested_b : {false, true}) {
			duckdb_logical_type types[2] = {bigint, nested_b ? struct_type : bigint};
			ArrowSchemaWrapper arrow_schema_wrapper;
			duckdb_arrow_options arrow_options;
			duckdb_connection_get_arrow_options(tester.connection, &arrow_options);
			duckdb_error_data err =
			    duckdb_to_arrow_schema(arrow_options, types, names, 2, &arrow_schema_wrapper.arrow_schema);
			duckdb_destroy_arrow_options(&arrow_options);
			REQUIRE(err == nullptr);
			duckdb_arrow_converted_schema converted_schema = nullptr;
			err = duckdb_schema_from_arrow(tester.connection, &arrow_schema_wrapper.arrow_schema, &converted_schema);
			REQUIRE(err == nullptr);

			// Keep a single column alive by referencing it from another chunk, then destroy the source chunk.
			for (idx_t column = 0; column < 2; column++) {
				PoisoningArrowArray producer(nested_b);
				duckdb_data_chunk source = nullptr;
				err = duckdb_data_chunk_from_arrow(tester.connection, &producer.array, converted_schema, &source);
				REQUIRE(err == nullptr);

				duckdb_data_chunk target = duckdb_create_data_chunk(&types[column], 1);
				duckdb_vector_reference_vector(duckdb_data_chunk_get_vector(target, 0),
				                               duckdb_data_chunk_get_vector(source, column));
				duckdb_data_chunk_set_size(target, PoisoningArrowArray::ROWS);
				duckdb_destroy_data_chunk(&source);

				REQUIRE(producer.release_calls == 0);
				auto vector = duckdb_data_chunk_get_vector(target, 0);
				if (column == 1 && nested_b) {
					vector = duckdb_struct_vector_get_child(vector, 0);
				}
				auto data = static_cast<int64_t *>(duckdb_vector_get_data(vector));
				for (idx_t i = 0; i < PoisoningArrowArray::ROWS; i++) {
					REQUIRE(data[i] == int64_t((i + 1) * (column == 0 ? 1 : 10)));
				}

				duckdb_destroy_data_chunk(&target);
				REQUIRE(producer.release_calls == 1);
			}

			duckdb_destroy_arrow_converted_schema(&converted_schema);
		}
		duckdb_destroy_logical_type(&struct_type);
		duckdb_destroy_logical_type(&bigint);
	}
}

static void ReleaseUnalignedSchema(ArrowSchema *schema) {
	schema->release = nullptr;
}

static void ReleaseUnalignedArray(ArrowArray *array) {
	array->release = nullptr;
}

// The C Data Interface allows buffers that are not aligned to their value type (see #26076)
TEST_CASE("Test C-API Arrow conversion of unaligned buffers", "[capi][arrow]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));

	constexpr idx_t ROWS = 3;
	constexpr idx_t COLUMNS = 11;
	const int32_t int32_values[ROWS] = {1, -2, 3};
	const int64_t int64_values[ROWS] = {10000000000, -2, 3};
	const double double_values[ROWS] = {1.5, -2.25, 3.125};
	const int32_t utf8_offsets[ROWS + 1] = {0, 1, 3, 6};
	const int64_t large_utf8_offsets[ROWS + 1] = {0, 3, 5, 6};
	const char *utf8_data = "abbccc";
	const int32_t time32_values[ROWS] = {1000, 61000, 3661000};
	const hugeint_t decimal_values[ROWS] = {hugeint_t(1500), hugeint_t(-2250), hugeint_t(3125)};
	const int32_t decimal32_values[ROWS] = {1500, -2250, 3125};
	const int64_t decimal64_values[ROWS] = {1500, -2250, 3125};
	const int32_t dictionary_indices[ROWS] = {2, 0, 1};

	// Each buffer starts one byte past an 8-byte boundary
	uint64_t storage[COLUMNS][2 * ROWS + 2];
	auto unaligned = [&](idx_t column, const void *values, idx_t size) {
		auto ptr = reinterpret_cast<uint8_t *>(storage[column]) + 1;
		memcpy(ptr, values, size);
		return static_cast<const void *>(ptr);
	};
	const void *buffers[COLUMNS][3] = {
	    {nullptr, unaligned(0, int32_values, sizeof(int32_values))},
	    {nullptr, unaligned(1, int64_values, sizeof(int64_values))},
	    {nullptr, unaligned(2, double_values, sizeof(double_values))},
	    {nullptr, unaligned(3, utf8_offsets, sizeof(utf8_offsets)), utf8_data},
	    {nullptr, unaligned(4, large_utf8_offsets, sizeof(large_utf8_offsets)), utf8_data},
	    {nullptr, unaligned(5, time32_values, sizeof(time32_values))},
	    {nullptr, unaligned(6, decimal_values, sizeof(decimal_values))},
	    {nullptr, unaligned(7, dictionary_indices, sizeof(dictionary_indices))},
	    {nullptr, unaligned(8, decimal32_values, sizeof(decimal32_values))},
	    {nullptr, unaligned(9, decimal64_values, sizeof(decimal64_values))},
	    {nullptr, unaligned(10, decimal_values, sizeof(decimal_values))},
	};
	const char *formats[COLUMNS] = {"i", "l", "g", "u", "U", "ttm", "d:18,3", "i", "d:9,3,32", "d:18,3,64", "d:38,3"};
	const char *names[COLUMNS] = {"i", "l", "g", "u", "large_u", "t", "d", "dict", "d32", "d64", "d128"};

	// The dictionary column has int32 indices into the utf8 column, with its offsets unaligned too
	ArrowSchema dictionary_schema = ArrowSchema();
	dictionary_schema.format = "u";
	dictionary_schema.name = "";
	dictionary_schema.flags = ARROW_FLAG_NULLABLE;
	dictionary_schema.release = ReleaseUnalignedSchema;
	ArrowArray dictionary_array = ArrowArray();
	dictionary_array.length = ROWS;
	dictionary_array.n_buffers = 3;
	dictionary_array.buffers = buffers[3];
	dictionary_array.release = ReleaseUnalignedArray;

	ArrowSchema child_schemas[COLUMNS];
	ArrowSchema *child_schema_ptrs[COLUMNS];
	ArrowArray child_arrays[COLUMNS];
	ArrowArray *child_array_ptrs[COLUMNS];
	for (idx_t i = 0; i < COLUMNS; i++) {
		child_schemas[i] = ArrowSchema();
		child_schemas[i].format = formats[i];
		child_schemas[i].name = names[i];
		child_schemas[i].flags = ARROW_FLAG_NULLABLE;
		child_schemas[i].release = ReleaseUnalignedSchema;
		child_schema_ptrs[i] = &child_schemas[i];

		child_arrays[i] = ArrowArray();
		child_arrays[i].length = ROWS;
		child_arrays[i].n_buffers = buffers[i][2] ? 3 : 2;
		child_arrays[i].buffers = buffers[i];
		child_arrays[i].release = ReleaseUnalignedArray;
		child_array_ptrs[i] = &child_arrays[i];
	}
	child_schemas[7].dictionary = &dictionary_schema;
	child_arrays[7].dictionary = &dictionary_array;

	ArrowSchema schema = ArrowSchema();
	schema.format = "+s";
	schema.name = "";
	schema.n_children = COLUMNS;
	schema.children = child_schema_ptrs;
	schema.release = ReleaseUnalignedSchema;

	const void *struct_buffers[1] = {nullptr};
	ArrowArray array = ArrowArray();
	array.length = ROWS;
	array.n_buffers = 1;
	array.buffers = struct_buffers;
	array.n_children = COLUMNS;
	array.children = child_array_ptrs;
	array.release = ReleaseUnalignedArray;

	duckdb_arrow_converted_schema converted_schema = nullptr;
	auto err = duckdb_schema_from_arrow(tester.connection, &schema, &converted_schema);
	REQUIRE(err == nullptr);
	duckdb_data_chunk chunk = nullptr;
	err = duckdb_data_chunk_from_arrow(tester.connection, &array, converted_schema, &chunk);
	REQUIRE(err == nullptr);
	REQUIRE(duckdb_data_chunk_get_size(chunk) == ROWS);

	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE unaligned(i INTEGER, l BIGINT, g DOUBLE, u VARCHAR, large_u VARCHAR, "
	                             "t TIME, d DECIMAL(18,3), dict VARCHAR, d32 DECIMAL(9,3), "
	                             "d64 DECIMAL(18,3), d128 DECIMAL(38,3))"));
	duckdb_appender appender;
	REQUIRE(duckdb_appender_create(tester.connection, nullptr, "unaligned", &appender) == DuckDBSuccess);
	REQUIRE(duckdb_append_data_chunk(appender, chunk) == DuckDBSuccess);
	REQUIRE(duckdb_appender_destroy(&appender) == DuckDBSuccess);

	auto result = tester.Query("SELECT * FROM unaligned");
	REQUIRE_NO_FAIL(*result);
	const vector<vector<string>> expected = {
	    {"1", "10000000000", "1.5", "a", "abb", "00:00:01", "1.500", "ccc", "1.500", "1.500", "1.500"},
	    {"-2", "-2", "-2.25", "bb", "cc", "00:01:01", "-2.250", "a", "-2.250", "-2.250", "-2.250"},
	    {"3", "3", "3.125", "ccc", "c", "01:01:01", "3.125", "bb", "3.125", "3.125", "3.125"},
	};
	for (idx_t row = 0; row < ROWS; row++) {
		for (idx_t col = 0; col < COLUMNS; col++) {
			REQUIRE(result->Fetch<string>(col, row) == expected[row][col]);
		}
	}

	// Exporting reads the imported vectors again
	duckdb_arrow_options arrow_options;
	duckdb_connection_get_arrow_options(tester.connection, &arrow_options);
	ArrowArray exported;
	err = duckdb_data_chunk_to_arrow(arrow_options, chunk, &exported);
	REQUIRE(err == nullptr);
	REQUIRE(exported.length == ROWS);
	REQUIRE(exported.n_children == COLUMNS);
	auto exported_int64 = static_cast<const int64_t *>(exported.children[1]->buffers[1]);
	for (idx_t row = 0; row < ROWS; row++) {
		REQUIRE(exported_int64[row] == int64_values[row]);
	}

	exported.release(&exported);
	duckdb_destroy_arrow_options(&arrow_options);
	duckdb_destroy_data_chunk(&chunk);
	duckdb_destroy_arrow_converted_schema(&converted_schema);
}

TEST_CASE("Test C-API appending an Arrow chunk larger than the vector size with casts", "[capi][arrow]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));

	constexpr idx_t ROWS = 10000;
	vector<int64_t> values(ROWS);
	for (idx_t i = 0; i < ROWS; i++) {
		values[i] = static_cast<int64_t>(i);
	}

	constexpr idx_t COLUMNS = 3;
	const void *buffers[2] = {nullptr, values.data()};
	const char *names[COLUMNS] = {"same", "narrow", "text"};
	ArrowSchema child_schemas[COLUMNS];
	ArrowSchema *child_schema_ptrs[COLUMNS];
	ArrowArray child_arrays[COLUMNS];
	ArrowArray *child_array_ptrs[COLUMNS];
	for (idx_t i = 0; i < COLUMNS; i++) {
		child_schemas[i] = ArrowSchema();
		child_schemas[i].format = "l";
		child_schemas[i].name = names[i];
		child_schemas[i].flags = ARROW_FLAG_NULLABLE;
		child_schemas[i].release = ReleaseUnalignedSchema;
		child_schema_ptrs[i] = &child_schemas[i];

		child_arrays[i] = ArrowArray();
		child_arrays[i].length = ROWS;
		child_arrays[i].n_buffers = 2;
		child_arrays[i].buffers = buffers;
		child_arrays[i].release = ReleaseUnalignedArray;
		child_array_ptrs[i] = &child_arrays[i];
	}

	ArrowSchema schema = ArrowSchema();
	schema.format = "+s";
	schema.name = "";
	schema.n_children = COLUMNS;
	schema.children = child_schema_ptrs;
	schema.release = ReleaseUnalignedSchema;

	const void *struct_buffers[1] = {nullptr};
	ArrowArray array = ArrowArray();
	array.length = ROWS;
	array.n_buffers = 1;
	array.buffers = struct_buffers;
	array.n_children = COLUMNS;
	array.children = child_array_ptrs;
	array.release = ReleaseUnalignedArray;

	duckdb_arrow_converted_schema converted_schema = nullptr;
	auto err = duckdb_schema_from_arrow(tester.connection, &schema, &converted_schema);
	REQUIRE(err == nullptr);
	duckdb_data_chunk chunk = nullptr;
	err = duckdb_data_chunk_from_arrow(tester.connection, &array, converted_schema, &chunk);
	REQUIRE(err == nullptr);
	REQUIRE(duckdb_data_chunk_get_size(chunk) == ROWS);

	// "same" needs no cast, "narrow" and "text" are cast by the appender
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE large_cast(same BIGINT, narrow INTEGER, text VARCHAR)"));
	duckdb_appender appender;
	REQUIRE(duckdb_appender_create(tester.connection, nullptr, "large_cast", &appender) == DuckDBSuccess);
	REQUIRE(duckdb_append_data_chunk(appender, chunk) == DuckDBSuccess);
	REQUIRE(duckdb_appender_destroy(&appender) == DuckDBSuccess);

	auto result = tester.Query("SELECT COUNT(*), SUM(same), SUM(narrow), SUM(text::BIGINT), "
	                           "COUNT(*) FILTER (WHERE same <> narrow OR same::VARCHAR <> text) FROM large_cast");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int64_t>(0, 0) == ROWS);
	REQUIRE(result->Fetch<int64_t>(1, 0) == 49995000);
	REQUIRE(result->Fetch<int64_t>(2, 0) == 49995000);
	REQUIRE(result->Fetch<int64_t>(3, 0) == 49995000);
	REQUIRE(result->Fetch<int64_t>(4, 0) == 0);

	duckdb_destroy_data_chunk(&chunk);
	duckdb_destroy_arrow_converted_schema(&converted_schema);
}

TEST_CASE("Test C-API Arrow string views retain data after aligned scratch is released", "[capi][arrow]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));

	constexpr idx_t ROWS = 5;
	const string external = "external string view data";
	const vector<string> expected = {"", "a", "abcdefghijkl", external};
	const arrow_string_view_t views[ROWS] = {
	    arrow_string_view_t(0, ""), arrow_string_view_t(1, "a"), arrow_string_view_t(12, "abcdefghijkl"),
	    arrow_string_view_t(NumericCast<int32_t>(external.size()), external.data(), 0, 0), arrow_string_view_t(0, "")};
	const uint8_t validity = 0x0F;
	const int64_t external_size = NumericCast<int64_t>(external.size());

	for (auto format : {"vu", "vz"}) {
		for (idx_t misalignment : {0, 1}) {
			for (idx_t offset : {0, 1}) {
				CAPTURE(format, misalignment, offset);
				uint64_t storage[(sizeof(views) + 1 + sizeof(uint64_t) - 1) / sizeof(uint64_t)];
				auto view_data = reinterpret_cast<uint8_t *>(storage) + misalignment;
				memcpy(view_data, views, sizeof(views));
				const void *buffers[] = {&validity, view_data, external.data(), &external_size};
				ArrowSchema child_schema = {};
				child_schema.format = format;
				child_schema.name = "v";
				child_schema.flags = ARROW_FLAG_NULLABLE;
				child_schema.release = ReleaseUnalignedSchema;
				ArrowSchema *children[] = {&child_schema};
				ArrowSchema schema = {};
				schema.format = "+s";
				schema.n_children = 1;
				schema.children = children;
				schema.release = ReleaseUnalignedSchema;

				ArrowArray child_array = {};
				child_array.length = ROWS - offset;
				child_array.offset = offset;
				child_array.null_count = 1;
				child_array.n_buffers = 4;
				child_array.buffers = buffers;
				child_array.release = ReleaseUnalignedArray;
				ArrowArray *arrays[] = {&child_array};
				const void *struct_buffers[] = {nullptr};
				ArrowArray array = {};
				array.length = ROWS - offset;
				array.n_buffers = 1;
				array.buffers = struct_buffers;
				array.n_children = 1;
				array.children = arrays;
				array.release = ReleaseUnalignedArray;

				duckdb_arrow_converted_schema converted = nullptr;
				REQUIRE(duckdb_schema_from_arrow(tester.connection, &schema, &converted) == nullptr);
				duckdb_data_chunk chunk = nullptr;
				REQUIRE(duckdb_data_chunk_from_arrow(tester.connection, &array, converted, &chunk) == nullptr);
				REQUIRE(duckdb_data_chunk_get_size(chunk) == ROWS - offset);

				auto vector = duckdb_data_chunk_get_vector(chunk, 0);
				auto strings = static_cast<duckdb_string_t *>(duckdb_vector_get_data(vector));
				for (idx_t row = 0; row < ROWS - offset - 1; row++) {
					auto data = duckdb_string_t_data(&strings[row]);
					auto size = duckdb_string_t_length(strings[row]);
					REQUIRE(string(data, size) == expected[row + offset]);
				}
				REQUIRE(!duckdb_validity_row_is_valid(duckdb_vector_get_validity(vector), ROWS - offset - 1));
				duckdb_destroy_data_chunk(&chunk);
				duckdb_destroy_arrow_converted_schema(&converted);
			}
		}
	}
}

// A hand-built ArrowArray whose shape disagrees with its resolved schema must be rejected cleanly,
// instead of indexing into a children array that doesn't match the schema's column count.
TEST_CASE("Test C-API Arrow array shape validation", "[capi][arrow]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));

	ArrowSchema child_schema = {};
	child_schema.format = "i";
	child_schema.name = "a";
	child_schema.release = ReleaseUnalignedSchema;
	ArrowSchema *children[] = {&child_schema};
	ArrowSchema schema = {};
	schema.format = "+s";
	schema.n_children = 1;
	schema.children = children;
	schema.release = ReleaseUnalignedSchema;

	duckdb_arrow_converted_schema converted = nullptr;
	REQUIRE(duckdb_schema_from_arrow(tester.connection, &schema, &converted) == nullptr);

	int32_t data[1] = {42};
	const void *child_buffers[] = {nullptr, data};
	ArrowArray child_array = {};
	child_array.length = 1;
	child_array.n_buffers = 2;
	child_array.buffers = child_buffers;
	child_array.release = ReleaseUnalignedArray;

	SECTION("fewer children than the schema expects") {
		ArrowArray array = {};
		array.length = 1;
		array.n_children = 0;
		array.children = nullptr;
		array.release = ReleaseUnalignedArray;

		duckdb_data_chunk chunk = nullptr;
		auto err = duckdb_data_chunk_from_arrow(tester.connection, &array, converted, &chunk);
		REQUIRE(err != nullptr);
		duckdb_destroy_error_data(&err);
	}

	SECTION("child length does not match the array length") {
		child_array.length = 5;
		ArrowArray *arrays[] = {&child_array};
		ArrowArray array = {};
		array.length = 1;
		array.n_children = 1;
		array.children = arrays;
		array.release = ReleaseUnalignedArray;

		duckdb_data_chunk chunk = nullptr;
		auto err = duckdb_data_chunk_from_arrow(tester.connection, &array, converted, &chunk);
		REQUIRE(err != nullptr);
		duckdb_destroy_error_data(&err);
	}

	SECTION("matching shape is accepted") {
		ArrowArray *arrays[] = {&child_array};
		ArrowArray array = {};
		array.length = 1;
		array.n_children = 1;
		array.children = arrays;
		array.release = ReleaseUnalignedArray;

		duckdb_data_chunk chunk = nullptr;
		REQUIRE(duckdb_data_chunk_from_arrow(tester.connection, &array, converted, &chunk) == nullptr);
		auto vector = duckdb_data_chunk_get_vector(chunk, 0);
		REQUIRE(static_cast<int32_t *>(duckdb_vector_get_data(vector))[0] == 42);
		duckdb_destroy_data_chunk(&chunk);
	}

	duckdb_destroy_arrow_converted_schema(&converted);
}

// Non-monotonic (decreasing) Arrow string offsets must be rejected, instead of wrapping around to
// produce a string_t whose length claims nearly 4GB over a tiny underlying data buffer.
TEST_CASE("Test C-API Arrow rejects non-monotonic string offsets", "[capi][arrow]") {
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));

	constexpr idx_t ROWS = 2;
	// offsets[2] (5) is less than offsets[1] (10): a classic 32-bit offset underflow when subtracted.
	const int32_t bad_offsets[ROWS + 1] = {0, 10, 5};
	const char data[16] = {0};
	const void *buffers[] = {nullptr, bad_offsets, data};

	ArrowSchema child_schema = {};
	child_schema.format = "u";
	child_schema.name = "a";
	child_schema.release = ReleaseUnalignedSchema;
	ArrowSchema *children[] = {&child_schema};
	ArrowSchema schema = {};
	schema.format = "+s";
	schema.n_children = 1;
	schema.children = children;
	schema.release = ReleaseUnalignedSchema;

	ArrowArray child_array = {};
	child_array.length = ROWS;
	child_array.n_buffers = 3;
	child_array.buffers = buffers;
	child_array.release = ReleaseUnalignedArray;
	ArrowArray *arrays[] = {&child_array};
	const void *struct_buffers[] = {nullptr};
	ArrowArray array = {};
	array.length = ROWS;
	array.n_buffers = 1;
	array.buffers = struct_buffers;
	array.n_children = 1;
	array.children = arrays;
	array.release = ReleaseUnalignedArray;

	duckdb_arrow_converted_schema converted = nullptr;
	REQUIRE(duckdb_schema_from_arrow(tester.connection, &schema, &converted) == nullptr);
	duckdb_data_chunk chunk = nullptr;
	auto err = duckdb_data_chunk_from_arrow(tester.connection, &array, converted, &chunk);
	REQUIRE(err != nullptr);
	duckdb_destroy_error_data(&err);
	duckdb_destroy_arrow_converted_schema(&converted);
}

TEST_CASE("Test C-API Arrow round trip of timestamps below the first midnight of the range", "[capi][arrow]") {
	if (STANDARD_VECTOR_SIZE < 3) {
		return;
	}
	CAPITester tester;
	REQUIRE(tester.OpenDatabase(nullptr));
	REQUIRE_NO_FAIL(
	    tester.Query("CREATE TABLE source AS SELECT * FROM (VALUES (make_timestamp(-9223372036854775807 - 1)), "
	                 "(make_timestamp(-9223372022400000001)), (make_timestamp(9223372036854775806))) t(ts)"));
	REQUIRE_NO_FAIL(tester.Query("CREATE TABLE target (ts TIMESTAMP)"));

	duckdb_result result;
	REQUIRE(duckdb_query(tester.connection, "SELECT ts FROM source", &result) == DuckDBSuccess);
	auto chunk = duckdb_fetch_chunk(result);
	REQUIRE(chunk);

	duckdb_arrow_options arrow_options;
	duckdb_connection_get_arrow_options(tester.connection, &arrow_options);
	ArrowArray arrow_array;
	REQUIRE(duckdb_data_chunk_to_arrow(arrow_options, chunk, &arrow_array) == nullptr);
	auto type = duckdb_create_logical_type(DUCKDB_TYPE_TIMESTAMP);
	const char *names[] = {"ts"};
	ArrowSchemaWrapper arrow_schema;
	REQUIRE(duckdb_to_arrow_schema(arrow_options, &type, names, 1, &arrow_schema.arrow_schema) == nullptr);
	duckdb_arrow_converted_schema converted = nullptr;
	REQUIRE(duckdb_schema_from_arrow(tester.connection, &arrow_schema.arrow_schema, &converted) == nullptr);
	duckdb_data_chunk imported = nullptr;
	REQUIRE(duckdb_data_chunk_from_arrow(tester.connection, &arrow_array, converted, &imported) == nullptr);

	duckdb_appender appender;
	REQUIRE(duckdb_appender_create(tester.connection, nullptr, "target", &appender) == DuckDBSuccess);
	REQUIRE(duckdb_append_data_chunk(appender, imported) == DuckDBSuccess);
	REQUIRE(duckdb_appender_destroy(&appender) == DuckDBSuccess);

	auto rendered = tester.Query("SELECT ts::VARCHAR FROM target ORDER BY ts");
	REQUIRE_NO_FAIL(*rendered);
	REQUIRE(rendered->Fetch<string>(0, 0) == "290309-12-21 (BC) 19:59:05.224192");
	REQUIRE(rendered->Fetch<string>(0, 1) == "290309-12-21 (BC) 23:59:59.999999");
	REQUIRE(rendered->Fetch<string>(0, 2) == "294247-01-10 04:00:54.775806");

	duckdb_destroy_data_chunk(&imported);
	if (arrow_array.release) {
		arrow_array.release(&arrow_array);
	}
	duckdb_destroy_arrow_converted_schema(&converted);
	duckdb_destroy_logical_type(&type);
	duckdb_destroy_arrow_options(&arrow_options);
	duckdb_destroy_data_chunk(&chunk);
	duckdb_destroy_result(&result);
}
