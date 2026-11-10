//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/storage_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/encryption_state.hpp"

namespace duckdb {

enum class CompressInMemory { AUTOMATIC, COMPRESS, DO_NOT_COMPRESS };

enum class FileIOMode : uint8_t {
	//! Buffered pread/pwrite via FileHandle (default).
	BUFFERED_IO,
	//! Memory-map the file; reads/writes go through the mapped region.
	MMAP,
	//! Unbuffered I/O (O_DIRECT / FILE_FLAG_NO_BUFFERING / F_NOCACHE).
	DIRECT_IO,
};

struct StorageOptions {
	//! The allocation size of blocks for this attached database file (if any)
	optional_idx block_alloc_size;
	//! The row group size for this attached database (if any)
	optional_idx row_group_size;
	//! Target storage version (if any)
	StorageVersion storage_version = StorageVersion::INVALID;
	//! Block header size (only used for encryption)
	optional_idx block_header_size;

	CompressInMemory compress_in_memory = CompressInMemory::AUTOMATIC;

	//! IO_MODE attach option; empty defaults to BUFFERED_IO.
	optional<FileIOMode> io_mode;
	//! MMAP_RESERVE_SIZE attach option; empty uses the StorageManager default.
	optional_idx mmap_reserve_size;

	//! Whether the database is encrypted
	bool encryption = false;
	//! Encryption algorithm
	EncryptionTypes::CipherType encryption_cipher = EncryptionTypes::INVALID;
	//! encryption key
	//! FIXME: change to a unique_ptr in the future
	shared_ptr<string> user_key;
	//! encryption version (set default to 1)
	EncryptionTypes::EncryptionVersion encryption_version = EncryptionTypes::NONE;

	void SetEncryptionVersion(string &storage_version_user_provided);
	void Initialize(Context &context, unordered_map<string, Value> &options);
};

} // namespace duckdb
