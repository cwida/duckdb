#include "capi_tester.hpp"

using namespace duckdb;

static void test_file_system(duckdb_file_system fs, string file_name) {
	REQUIRE(fs != nullptr);

	duckdb_state state = DuckDBSuccess;
	duckdb_file_handle file;

	auto file_path = TestDirectoryPath() + "/" + file_name;

	auto options = duckdb_create_file_open_options();
	state = duckdb_file_open_options_set_flag(options, DUCKDB_FILE_FLAG_WRITE, true);
	REQUIRE(state == DuckDBSuccess);
	state = duckdb_file_open_options_set_flag(options, DUCKDB_FILE_FLAG_READ, true);
	REQUIRE(state == DuckDBSuccess);

	// Try to open non-existing file without create flag
	state = duckdb_file_system_open(fs, file_path.c_str(), options, &file);
	REQUIRE(state != DuckDBSuccess);
	auto error_data = duckdb_file_system_error_data(fs);
	auto error_type = duckdb_error_data_error_type(error_data);
	REQUIRE(error_type == DUCKDB_ERROR_IO);
	duckdb_destroy_error_data(&error_data);

	// Try to open file without options
	state = duckdb_file_system_open(fs, file_path.c_str(), nullptr, &file);
	REQUIRE(state == DuckDBError);
	error_data = duckdb_file_system_error_data(fs);
	error_type = duckdb_error_data_error_type(error_data);
	REQUIRE(error_type == DUCKDB_ERROR_IO);
	duckdb_destroy_error_data(&error_data);

	// Try to open incorrect file
	state = duckdb_file_system_open(fs, file_path.c_str(), options, nullptr);
	REQUIRE(state == DuckDBError);
	error_data = duckdb_file_system_error_data(fs);
	error_type = duckdb_error_data_error_type(error_data);
	REQUIRE(error_type == DUCKDB_ERROR_IO);
	duckdb_destroy_error_data(&error_data);

	// Try to open file with incorrect filename
	state = duckdb_file_system_open(fs, nullptr, options, &file);
	REQUIRE(state == DuckDBError);
	error_data = duckdb_file_system_error_data(fs);
	error_type = duckdb_error_data_error_type(error_data);
	REQUIRE(error_type == DUCKDB_ERROR_IO);
	duckdb_destroy_error_data(&error_data);

	// Try to open with all incorrect input parameters
	// fs is incorrect, error message verification is not possible
	state = duckdb_file_system_open(nullptr, nullptr, nullptr, nullptr);
	REQUIRE(state == DuckDBError);

	// Try to write to a null file handle
	auto failed_bytes_written = duckdb_file_handle_write(file, "data", 4);
	REQUIRE(failed_bytes_written == -1);
	auto file_error_data = duckdb_file_handle_error_data(file);
	auto has_error = duckdb_error_data_has_error(file_error_data);
	REQUIRE(has_error == false);
	duckdb_destroy_error_data(&file_error_data);

	// Set create flag
	state = duckdb_file_open_options_set_flag(options, DUCKDB_FILE_FLAG_CREATE, true);
	REQUIRE(state == DuckDBSuccess);

	// Create and open a file
	state = duckdb_file_system_open(fs, file_path.c_str(), options, &file);
	REQUIRE(state == DuckDBSuccess);
	REQUIRE(file != nullptr);

	// Write to the file
	const char *data = "Hello, DuckDB File System!";
	auto bytes_written = duckdb_file_handle_write(file, data, strlen(data));
	REQUIRE(bytes_written == (int64_t)strlen(data));
	auto position = duckdb_file_handle_tell(file);
	REQUIRE(position == bytes_written);
	auto size = duckdb_file_handle_size(file);
	REQUIRE(size == bytes_written);

	// Sync
	state = duckdb_file_handle_sync(file);

	// Seek to the beginning
	state = duckdb_file_handle_seek(file, 0);
	REQUIRE(state == DuckDBSuccess);
	position = duckdb_file_handle_tell(file);
	REQUIRE(position == 0);

	// Read from the file
	char buffer[30];
	memset(buffer, 0, sizeof(buffer));
	auto bytes_read = duckdb_file_handle_read(file, buffer, sizeof(buffer) - 1);
	REQUIRE(bytes_read == bytes_written);
	REQUIRE(strcmp(buffer, data) == 0);
	position = duckdb_file_handle_tell(file);
	REQUIRE(position == bytes_read);
	size = duckdb_file_handle_size(file);
	REQUIRE(size == bytes_written);

	// Seek to the end
	state = duckdb_file_handle_seek(file, bytes_written);
	REQUIRE(state == DuckDBSuccess);
	position = duckdb_file_handle_tell(file);
	REQUIRE(position == bytes_written);
	size = duckdb_file_handle_size(file);
	REQUIRE(size == bytes_written);

	// Try to read from the end of the file
	memset(buffer, 0, sizeof(buffer));
	bytes_read = duckdb_file_handle_read(file, buffer, sizeof(buffer) - 1);
	REQUIRE(bytes_read == 0); // EOF
	position = duckdb_file_handle_tell(file);
	REQUIRE(position == bytes_written);
	size = duckdb_file_handle_size(file);
	REQUIRE(size == bytes_written);

	// Seek back to the beginning
	state = duckdb_file_handle_seek(file, 0);
	REQUIRE(state == DuckDBSuccess);
	position = duckdb_file_handle_tell(file);
	REQUIRE(position == 0);
	size = duckdb_file_handle_size(file);
	REQUIRE(size == bytes_written);

	// Close the file
	duckdb_file_handle_close(file);
	duckdb_destroy_file_handle(&file);

	// Open file again for reading
	state = duckdb_file_system_open(fs, file_path.c_str(), options, &file);
	REQUIRE(state == DuckDBSuccess);
	REQUIRE(file != nullptr);
	size = duckdb_file_handle_size(file);
	REQUIRE(size == bytes_written);
	// Check that the data is still there
	memset(buffer, 0, sizeof(buffer));
	bytes_read = duckdb_file_handle_read(file, buffer, sizeof(buffer) - 1);
	REQUIRE(bytes_read == bytes_written);
	REQUIRE(strcmp(buffer, data) == 0);
	position = duckdb_file_handle_tell(file);
	REQUIRE(position == bytes_read);
	size = duckdb_file_handle_size(file);
	REQUIRE(size == bytes_written);

	// Close the file again
	duckdb_file_handle_close(file);
	duckdb_destroy_file_handle(&file);

	duckdb_destroy_file_open_options(&options);

	REQUIRE(file == nullptr);
	REQUIRE(options == nullptr);

	// Try destroy again for good measure
	duckdb_destroy_file_handle(&file);
	duckdb_destroy_file_open_options(&options);

	REQUIRE(file == nullptr);
	REQUIRE(options == nullptr);
}

TEST_CASE("Test File System in C API", "[capi]") {
	CAPITester tester;
	duckdb_client_context context;
	duckdb_file_system fs;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));

	// get a file system from the client context
	duckdb_connection_get_client_context(tester.connection, &context);
	fs = duckdb_client_context_get_file_system(context);
	REQUIRE(fs != nullptr);

	test_file_system(fs, "test_file_capi_1.txt");

	duckdb_destroy_file_system(&fs);
	REQUIRE(fs == nullptr);

	duckdb_destroy_client_context(&context);

	// Try to destory fs again
	duckdb_destroy_file_system(&fs);
	REQUIRE(fs == nullptr);
}

#ifndef _WIN32
TEST_CASE("Test a failed close of a written file in the C API", "[capi]") {
	CAPITester tester;
	duckdb_client_context context;
	REQUIRE(tester.OpenDatabase(nullptr));
	duckdb_connection_get_client_context(tester.connection, &context);
	auto fs = duckdb_client_context_get_file_system(context);
	auto options = duckdb_create_file_open_options();
	REQUIRE(duckdb_file_open_options_set_flag(options, DUCKDB_FILE_FLAG_WRITE, true) == DuckDBSuccess);
	REQUIRE(duckdb_file_open_options_set_flag(options, DUCKDB_FILE_FLAG_CREATE, true) == DuckDBSuccess);
	auto file_path = TestCreatePath("test_file_capi_failed_close.txt");
	duckdb_file_handle file = nullptr;
	auto open_file = [&]() {
		REQUIRE(duckdb_file_system_open(fs, file_path.c_str(), options, &file) == DuckDBSuccess);
	};

	REQUIRE(TestOpenWithFailingClose(file_path, open_file));
	REQUIRE(duckdb_file_handle_close(file) == DuckDBError);
	auto error_data = duckdb_file_handle_error_data(file);
	REQUIRE(duckdb_error_data_error_type(error_data) == DUCKDB_ERROR_IO);
	REQUIRE(StringUtil::Contains(duckdb_error_data_message(error_data), "Could not close file"));
	duckdb_destroy_error_data(&error_data);
	duckdb_destroy_file_handle(&file);

	// destroying the handle ignores the error
	REQUIRE(TestOpenWithFailingClose(file_path, open_file));
	duckdb_destroy_file_handle(&file);
	REQUIRE(file == nullptr);

	duckdb_destroy_file_open_options(&options);
	duckdb_destroy_file_system(&fs);
	duckdb_destroy_client_context(&context);
}
#endif
