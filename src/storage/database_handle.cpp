#include "duckdb/storage/database_handle.hpp"
#include "duckdb/storage/single_file_block_manager.hpp"
#include "duckdb/common/query_context.hpp"

#include <cstring>

namespace duckdb {

DatabaseHandle::DatabaseHandle(unique_ptr<FileHandle> handle_p) : handle(std::move(handle_p)) {
}
DatabaseHandle::DatabaseHandle(unique_ptr<MemoryMappedFile> mmap_handle_p) : mmap_handle(std::move(mmap_handle_p)) {
}

//! The file system to open the database file with. The handle outlives the statement that opens it, but file systems
//! take what they need from the opener when the file is opened: through the client's file system, a remote database
//! file uses the settings of the session that attached it (http_proxy, ca_cert_file, ...) and reports its requests to
//! that session's logger. Without a client, the database-wide file system only sees global settings.
static FileSystem &GetDatabaseFileSystem(QueryContext context, AttachedDatabase &db) {
	auto client_context = context.GetClientContext();
	if (client_context) {
		return FileSystem::GetFileSystem(*client_context);
	}
	return FileSystem::Get(db);
}

unique_ptr<DatabaseHandle> DatabaseHandle::Open(QueryContext context, AttachedDatabase &db, const string &path,
                                                const StorageManagerOptions &options, DatabaseOpenMode open_mode) {
	if (options.io_mode == FileIOMode::MMAP) {
		// mmap reads are zero-copy from the mapping, so the prefetched header is not needed here.
		return OpenMemoryMap(context, db, path, options, open_mode);
	} else {
		return OpenFile(context, db, path, options, open_mode);
	}
}

unique_ptr<DatabaseHandle> DatabaseHandle::OpenFile(QueryContext context, AttachedDatabase &db, const string &path,
                                                    const StorageManagerOptions &options, DatabaseOpenMode open_mode) {
	FileOpenFlags file_flags;
	if (options.read_only) {
		D_ASSERT(open_mode == DatabaseOpenMode::OPEN_EXISTING_FILE);
		file_flags = FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS | FileLockType::READ_LOCK;
	} else {
		file_flags = FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_READ | FileLockType::WRITE_LOCK;
		if (open_mode == DatabaseOpenMode::CREATE_NEW_FILE) {
			file_flags |= FileFlags::FILE_FLAGS_FILE_CREATE;
		}
	}
	if (options.io_mode == FileIOMode::DIRECT_IO) {
		file_flags |= FileFlags::FILE_FLAGS_DIRECT_IO;
	}
	// database files can be read from in parallel
	file_flags |= FileFlags::FILE_FLAGS_PARALLEL_ACCESS;
	file_flags |= FileFlags::FILE_FLAGS_MULTI_CLIENT_ACCESS;

	auto &fs = GetDatabaseFileSystem(context, db);
	auto file_handle = fs.OpenFile(path, file_flags);
	if (!file_handle) {
		// this can only happen in read-only mode - as that is when we set FILE_FLAGS_NULL_IF_NOT_EXISTS
		throw IOException("Cannot open database \"%s\" in read-only mode: database does not exist", path);
	}

	auto result = make_uniq<DatabaseHandle>(std::move(file_handle));
	// Adopt the prefetched header bytes (if any) to serve the initial header reads from memory.
	if (options.prefetched.HasHeader()) {
		result->prefetched_header = options.prefetched.header;
	}
	return result;
}

unique_ptr<DatabaseHandle> DatabaseHandle::OpenMemoryMap(QueryContext context, AttachedDatabase &db, const string &path,
                                                         const StorageManagerOptions &options,
                                                         DatabaseOpenMode open_mode) {
	if (options.encryption_options.encryption_enabled) {
		// In-place decryption would write decrypted bytes back through the mapping.
		throw InvalidInputException("MMAP mode is not supported for encrypted databases");
	}
	// Default reserve covers the bulk of analytical databases; users override via MMAP_RESERVE_SIZE.
	static constexpr idx_t MMAP_DEFAULT_RESERVE_SIZE = idx_t(256) * 1024 * 1024 * 1024; // 256 GiB
	MMapOptions mmap_options;
	mmap_options.reserve_size =
	    options.mmap_reserve_size.IsValid() ? options.mmap_reserve_size.GetIndex() : MMAP_DEFAULT_RESERVE_SIZE;
	FileOpenFlags mmap_flags;
	if (options.read_only) {
		D_ASSERT(open_mode == DatabaseOpenMode::OPEN_EXISTING_FILE);
		mmap_flags = FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS | FileLockType::READ_LOCK;
	} else {
		mmap_flags = FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_WRITE | FileLockType::WRITE_LOCK;
		if (open_mode == DatabaseOpenMode::CREATE_NEW_FILE) {
			mmap_flags |= FileFlags::FILE_FLAGS_FILE_CREATE;
		}
	}
	auto &fs = GetDatabaseFileSystem(context, db);
	auto mmap_handle = fs.MemoryMapFile(path, mmap_flags, mmap_options);
	if (!mmap_handle) {
		// Only happens in read-only mode, where FILE_FLAGS_NULL_IF_NOT_EXISTS is set.
		throw IOException("Cannot open database \"%s\" in read-only mode: database does not exist", path);
	}
	return make_uniq<DatabaseHandle>(std::move(mmap_handle));
}

bool DatabaseHandle::OnDiskFile() const {
	if (mmap_handle) {
		return mmap_handle->OnDiskFile();
	}
	return handle->OnDiskFile();
}

void DatabaseHandle::CheckMagicBytes(QueryContext context) {
	if (mmap_handle) {
		MainHeader::CheckMagicBytes(*mmap_handle);
		return;
	}
	// Verify against the prefetched header when it covers the magic-byte range.
	const idx_t magic_end = MainHeader::MAGIC_BYTE_OFFSET + MainHeader::MAGIC_BYTE_SIZE;
	if (prefetched_header && prefetched_header->size() >= magic_end) {
		if (memcmp(prefetched_header->data() + MainHeader::MAGIC_BYTE_OFFSET, MainHeader::MAGIC_BYTES,
		           MainHeader::MAGIC_BYTE_SIZE) != 0) {
			throw IOException("The file \"%s\" exists, but it is not a valid DuckDB database file!", handle->GetPath());
		}
		return;
	}
	MainHeader::CheckMagicBytes(context, *handle);
}

void DatabaseHandle::Read(QueryContext context, FileBuffer &block, uint64_t location) const {
	if (mmap_handle) {
		block.Read(context, *mmap_handle, location);
		return;
	}
	// Serve fully-covered reads from the prefetched header (only the first few header reads qualify).
	if (prefetched_header && location + block.AllocSize() <= prefetched_header->size()) {
		block.ReadFromMemory(const_data_ptr_cast(prefetched_header->data()), location);
		return;
	}
	block.Read(context, *handle, location);
}

void DatabaseHandle::Write(QueryContext context, FileBuffer &buffer, uint64_t location) {
	if (mmap_handle) {
		EnsureMappedSize(location + buffer.AllocSize());
		buffer.Write(context, *mmap_handle, location);
	} else {
		buffer.Write(context, *handle, location);
	}
}

void DatabaseHandle::Sync() {
	if (mmap_handle) {
		mmap_handle->Sync();
	} else {
		handle->Sync();
	}
}

void DatabaseHandle::Truncate(idx_t new_size) {
	if (mmap_handle) {
		// File stays at reserve size in MAP mode; punch a hole to release the tail.
		mmap_handle->Trim(new_size, mmap_handle->Size() - new_size);
	} else {
		handle->Truncate(NumericCast<int64_t>(new_size));
	}
}

void DatabaseHandle::Trim(idx_t offset, idx_t length) {
	if (mmap_handle) {
		mmap_handle->Trim(offset, length);
	} else {
		handle->Trim(offset, length);
	}
}

idx_t DatabaseHandle::GetFileSize() {
	if (mmap_handle) {
		return 0;
	}
	return handle->GetFileSize();
}

FileHandle &DatabaseHandle::GetFileHandle() {
	return *handle;
}

void DatabaseHandle::EnsureMappedSize(idx_t required_size) const {
	if (!mmap_handle) {
		return;
	}
	if (required_size > mmap_handle->Size()) {
		throw IOException("Database file \"%s\" would grow to %llu bytes, which exceeds the memory-mapped reserved "
		                  "size of %llu bytes. Re-attach with a larger MMAP_RESERVE_SIZE, or without "
		                  "IO_MODE='MMAP', to allow further growth.",
		                  mmap_handle->GetPath(), required_size, mmap_handle->Size());
	}
}

} // namespace duckdb
