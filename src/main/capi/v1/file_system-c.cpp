#include "duckdb/main/capi/capi_internal.hpp"

namespace duckdb {
namespace {
struct CFileSystem {
	FileSystem &fs;
	ErrorData error_data;

	explicit CFileSystem(FileSystem &fs_p) : fs(fs_p) {
	}

	void SetError(const char *message) {
		error_data = ErrorData(ExceptionType::IO, message);
	}
	void SetError(const std::exception &ex) {
		error_data = ErrorData(ex);
	}
};

struct CFileOpenOptions {
	duckdb::FileOpenFlags flags;
};

struct CFileHandle {
	ErrorData error_data;
	unique_ptr<FileHandle> handle;

	void SetError(const char *message) {
		error_data = ErrorData(ExceptionType::IO, message);
	}
	void SetError(const std::exception &ex) {
		error_data = ErrorData(ex);
	}
};

} // namespace
} // namespace duckdb

duckdb_file_system duckdb_client_context_get_file_system(duckdb_client_context context) {
	if (!context) {
		return nullptr;
	}
	auto ctx = reinterpret_cast<duckdb::CClientContextWrapper *>(context);
	auto wrapper = new duckdb::CFileSystem(duckdb::FileSystem::GetFileSystem(ctx->context));
	return reinterpret_cast<duckdb_file_system>(wrapper);
}

void duckdb_destroy_file_system(duckdb_file_system *file_system) {
	if (!file_system || !*file_system) {
		return;
	}
	const auto fs = reinterpret_cast<duckdb::CFileSystem *>(*file_system);
	delete fs;
	*file_system = nullptr;
}

duckdb_file_open_options duckdb_create_file_open_options() {
	auto options = new duckdb::CFileOpenOptions();
	return reinterpret_cast<duckdb_file_open_options>(options);
}

duckdb_state duckdb_file_open_options_set_flag(duckdb_file_open_options options, duckdb_file_flag flag, bool value) {
	if (!options) {
		return DuckDBError;
	}
	auto coptions = reinterpret_cast<duckdb::CFileOpenOptions *>(options);

	switch (flag) {
	case DUCKDB_FILE_FLAG_READ:
		coptions->flags |= duckdb::FileOpenFlags::FILE_FLAGS_READ;
		break;
	case DUCKDB_FILE_FLAG_WRITE:
		coptions->flags |= duckdb::FileOpenFlags::FILE_FLAGS_WRITE;
		break;
	case DUCKDB_FILE_FLAG_APPEND:
		coptions->flags |= duckdb::FileOpenFlags::FILE_FLAGS_APPEND;
		break;
	case DUCKDB_FILE_FLAG_CREATE:
		coptions->flags |= duckdb::FileOpenFlags::FILE_FLAGS_FILE_CREATE;
		break;
	case DUCKDB_FILE_FLAG_CREATE_NEW:
		coptions->flags |= duckdb::FileOpenFlags::FILE_FLAGS_EXCLUSIVE_CREATE;
		break;
	default:
		return DuckDBError;
	}
	return DuckDBSuccess;
}

void duckdb_destroy_file_open_options(duckdb_file_open_options *options) {
	if (!options || !*options) {
		return;
	}
	auto coptions = reinterpret_cast<duckdb::CFileOpenOptions *>(*options);
	delete coptions;
	*options = nullptr;
}

duckdb_state duckdb_file_system_open(duckdb_file_system fs, const char *path, duckdb_file_open_options options,
                                     duckdb_file_handle *out_file) {
	if (!fs) {
		if (out_file) {
			*out_file = nullptr;
		}
		return DuckDBError;
	}
	auto cfs = reinterpret_cast<duckdb::CFileSystem *>(fs);
	if (!out_file) {
		cfs->SetError("Invalid out file to duckdb_file_system_open");
		return DuckDBError;
	}
	if (!path || !options) {
		cfs->SetError("Invalid input to duckdb_file_system_open");
		*out_file = nullptr;
		return DuckDBError;
	}

	try {
		auto coptions = reinterpret_cast<duckdb::CFileOpenOptions *>(options);
		auto handle = cfs->fs.OpenFile(duckdb::string(path), coptions->flags);
		auto wrapper = new duckdb::CFileHandle();
		wrapper->handle = std::move(handle);
		*out_file = reinterpret_cast<duckdb_file_handle>(wrapper);
		return DuckDBSuccess;
	} catch (const std::exception &ex) {
		cfs->SetError(ex);
		*out_file = nullptr;
		return DuckDBError;
	} catch (...) {
		cfs->SetError("Unknown error occurred during file open");
		*out_file = nullptr;
		return DuckDBError;
	}
}

duckdb_error_data duckdb_file_system_error_data(duckdb_file_system fs) {
	auto wrapper = new duckdb::ErrorDataWrapper();
	if (!fs) {
		return reinterpret_cast<duckdb_error_data>(wrapper);
	}
	auto cfs = reinterpret_cast<duckdb::CFileSystem *>(fs);
	wrapper->error_data = cfs->error_data;
	return reinterpret_cast<duckdb_error_data>(wrapper);
}

void duckdb_destroy_file_handle(duckdb_file_handle *file) {
	if (!file || !*file) {
		return;
	}
	auto cfile = reinterpret_cast<duckdb::CFileHandle *>(*file);
	// Ensure the file is closed before destroying
	try {
		cfile->handle->Close();
	} catch (...) { // NOLINT
	}
	delete cfile;
	*file = nullptr;
}

duckdb_error_data duckdb_file_handle_error_data(duckdb_file_handle file) {
	auto wrapper = new duckdb::ErrorDataWrapper();
	if (!file) {
		return reinterpret_cast<duckdb_error_data>(wrapper);
	}
	auto cfile = reinterpret_cast<duckdb::CFileHandle *>(file);
	wrapper->error_data = cfile->error_data;
	return reinterpret_cast<duckdb_error_data>(wrapper);
}

int64_t duckdb_file_handle_read(duckdb_file_handle file, void *buffer, int64_t size) {
	if (!file || !buffer || size < 0) {
		return -1;
	}
	auto cfile = reinterpret_cast<duckdb::CFileHandle *>(file);
	try {
		return cfile->handle->Read(buffer, static_cast<idx_t>(size));
	} catch (std::exception &ex) {
		cfile->SetError(ex);
		return -1;
	} catch (...) {
		cfile->SetError("Unknown error occurred during file read");
		return -1;
	}
}

int64_t duckdb_file_handle_write(duckdb_file_handle file, const void *buffer, int64_t size) {
	if (!file || !buffer || size < 0) {
		return -1;
	}
	auto cfile = reinterpret_cast<duckdb::CFileHandle *>(file);
	try {
		return cfile->handle->Write(const_cast<void *>(buffer), static_cast<idx_t>(size));
	} catch (std::exception &ex) {
		cfile->SetError(ex);
		return -1;
	} catch (...) {
		cfile->SetError("Unknown error occurred during file write");
		return -1;
	}
}

int64_t duckdb_file_handle_tell(duckdb_file_handle file) {
	if (!file) {
		return -1;
	}
	auto cfile = reinterpret_cast<duckdb::CFileHandle *>(file);
	try {
		return static_cast<int64_t>(cfile->handle->SeekPosition());
	} catch (std::exception &ex) {
		cfile->SetError(ex);
		return -1;
	} catch (...) {
		cfile->SetError("Unknown error occurred when getting file position");
		return -1;
	}
}

int64_t duckdb_file_handle_size(duckdb_file_handle file) {
	if (!file) {
		return -1;
	}
	auto cfile = reinterpret_cast<duckdb::CFileHandle *>(file);
	try {
		return static_cast<int64_t>(cfile->handle->GetFileSize());
	} catch (std::exception &ex) {
		cfile->SetError(ex);
		return -1;
	} catch (...) {
		cfile->SetError("Unknown error occurred when getting file size");
		return -1;
	}
}

duckdb_state duckdb_file_handle_seek(duckdb_file_handle file, int64_t position) {
	if (!file || position < 0) {
		return DuckDBError;
	}
	auto cfile = reinterpret_cast<duckdb::CFileHandle *>(file);
	try {
		cfile->handle->Seek(static_cast<idx_t>(position));
		return DuckDBSuccess;
	} catch (std::exception &ex) {
		cfile->SetError(ex);
		return DuckDBError;
	} catch (...) {
		cfile->SetError("Unknown error occurred when seeking in file");
		return DuckDBError;
	}
}

duckdb_state duckdb_file_handle_sync(duckdb_file_handle file) {
	if (!file) {
		return DuckDBError;
	}
	auto cfile = reinterpret_cast<duckdb::CFileHandle *>(file);
	try {
		cfile->handle->Sync();
		return DuckDBSuccess;
	} catch (std::exception &ex) {
		cfile->SetError(ex);
		return DuckDBError;
	} catch (...) {
		cfile->SetError("Unknown error occurred when syncing file");
		return DuckDBError;
	}
}

duckdb_state duckdb_file_handle_close(duckdb_file_handle file) {
	if (!file) {
		return DuckDBError;
	}
	auto cfile = reinterpret_cast<duckdb::CFileHandle *>(file);
	try {
		cfile->handle->Close();
		return DuckDBSuccess;
	} catch (std::exception &ex) {
		cfile->SetError(ex);
		return DuckDBError;
	} catch (...) {
		cfile->SetError("Unknown error occurred when closing file");
		return DuckDBError;
	}
}
