#include "duckdb/storage/single_file_block_manager.hpp"

#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/checksum.hpp"
#include "duckdb/common/encryption_functions.hpp"
#include "duckdb/common/encryption_key_manager.hpp"
#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/enums/checkpoint_abort.hpp"
#include "duckdb/common/enums/storage_block_prefetch.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/block_allocator.hpp"
#include "duckdb/storage/metadata/metadata_reader.hpp"
#include "duckdb/storage/metadata/metadata_writer.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/storage_manager.hpp"

#include <algorithm>
#include <cstring>

namespace duckdb {

const char MainHeader::MAGIC_BYTES[] = "DUCK";
const char MainHeader::CANARY[] = "DUCKKEY";
static constexpr idx_t ENCRYPTION_METADATA_LEN = 8;

void SerializeVersionNumber(WriteStream &ser, const string &version_str) {
	data_t version[MainHeader::MAX_VERSION_SIZE];
	memset(version, 0, MainHeader::MAX_VERSION_SIZE);
	memcpy(version, version_str.c_str(), MinValue<idx_t>(version_str.size(), MainHeader::MAX_VERSION_SIZE));
	ser.WriteData(version, MainHeader::MAX_VERSION_SIZE);
}

void SerializeDBIdentifier(WriteStream &ser, data_ptr_t db_identifier_p) {
	data_t db_identifier[MainHeader::DB_IDENTIFIER_LEN];
	memset(db_identifier, 0, MainHeader::DB_IDENTIFIER_LEN);
	memcpy(db_identifier, db_identifier_p, MainHeader::DB_IDENTIFIER_LEN);
	ser.WriteData(db_identifier, MainHeader::DB_IDENTIFIER_LEN);
}

void SerializeEncryptionMetadata(WriteStream &ser, data_ptr_t metadata_p, const bool encrypted) {
	// Zero-initialize.
	data_t metadata[MainHeader::ENCRYPTION_METADATA_LEN];
	memset(metadata, 0, MainHeader::ENCRYPTION_METADATA_LEN);

	// Write metadata, if encrypted.
	if (encrypted) {
		memcpy(metadata, metadata_p, MainHeader::ENCRYPTION_METADATA_LEN);
	}
	ser.WriteData(metadata, MainHeader::ENCRYPTION_METADATA_LEN);
}

void SerializeIV(WriteStream &ser, data_ptr_t metadata_p, const bool encrypted) {
	// Used for Canary encryption
	// Zero-initialize.
	data_t iv[MainHeader::AES_NONCE_LEN];
	memset(iv, 0, MainHeader::AES_NONCE_LEN);

	// Write metadata, if encrypted.
	if (encrypted) {
		memcpy(iv, metadata_p, MainHeader::AES_NONCE_LEN);
	}
	ser.WriteData(iv, MainHeader::AES_NONCE_LEN);
}

void SerializeTag(WriteStream &ser, data_ptr_t metadata_p, const bool encrypted) {
	// Used for Canary encryption
	// Zero-initialize.
	data_t tag[MainHeader::AES_TAG_LEN];
	memset(tag, 0, MainHeader::AES_TAG_LEN);

	// Write metadata, if encrypted.
	if (encrypted) {
		memcpy(tag, metadata_p, MainHeader::AES_TAG_LEN);
	}
	ser.WriteData(tag, MainHeader::AES_TAG_LEN);
}

void DeserializeVersionNumber(ReadStream &stream, data_t *dest) {
	memset(dest, 0, MainHeader::MAX_VERSION_SIZE);
	stream.ReadData(dest, MainHeader::MAX_VERSION_SIZE);
}

void DeserializeEncryptionData(ReadStream &stream, data_t *dest, idx_t size) {
	memset(dest, 0, size);
	stream.ReadData(dest, size);
}

void GenerateDBIdentifier(uint8_t *db_identifier) {
	memset(db_identifier, 0, MainHeader::DB_IDENTIFIER_LEN);
	RandomEngine engine;
	engine.RandomData(db_identifier, MainHeader::DB_IDENTIFIER_LEN);
}

void EncryptCanary(MainHeader &main_header, const shared_ptr<EncryptionState> &encryption_state,
                   const_data_ptr_t derived_key) {
	EncryptionCanary canary;
	EncryptionNonce nonce(EncryptionTypes::CipherType::GCM, encryption_state->metadata->GetVersion());
	memset(nonce.data(), 0, nonce.size());
	EncryptionTag tag;

	switch (encryption_state->metadata->GetVersion()) {
	case EncryptionTypes::V0_0:
		D_ASSERT(nonce.total_size() == MainHeader::AES_NONCE_LEN_DEPRECATED);
		encryption_state->InitializeEncryption(nonce, derived_key);
		encryption_state->Process(reinterpret_cast<const_data_ptr_t>(MainHeader::CANARY), canary.size(), canary.data(),
		                          canary.size());
		break;
	case EncryptionTypes::V0_1:
		// for GCM, total nonce size should be always equal to 12 bytes
		D_ASSERT(nonce.total_size() == MainHeader::AES_NONCE_LEN);
		encryption_state->GenerateRandomData(nonce.data(), nonce.size());
		main_header.SetCanaryIV(nonce.data());
		encryption_state->InitializeEncryption(nonce, derived_key);
		encryption_state->Process(reinterpret_cast<const_data_ptr_t>(MainHeader::CANARY), canary.size(), canary.data(),
		                          canary.size());
		encryption_state->Finalize(canary.data(), canary.size(), tag.data(), MainHeader::AES_TAG_LEN);
		main_header.SetCanaryTag(tag.data());
		break;
	default:
		throw InvalidInputException("No valid encryption version found!");
	}

	main_header.SetEncryptedCanary(canary.data());
}

bool DecryptCanary(MainHeader &main_header, const shared_ptr<EncryptionState> &encryption_state,
                   data_ptr_t derived_key) {
	auto encryption_version = encryption_state->metadata->GetVersion();
	EncryptionNonce nonce(EncryptionTypes::CipherType::GCM, encryption_version);
	EncryptionTag tag;
	EncryptionCanary decrypted_canary;

	switch (encryption_version) {
	case EncryptionTypes::V0_0:
		D_ASSERT(nonce.total_size() == MainHeader::AES_NONCE_LEN_DEPRECATED);
		//! Decrypt the canary, Nonce is zeroed out
		encryption_state->InitializeDecryption(nonce, derived_key);
		encryption_state->Process(main_header.GetEncryptedCanary(), decrypted_canary.size(), decrypted_canary.data(),
		                          decrypted_canary.size());
		break;
	case EncryptionTypes::V0_1:
		D_ASSERT(nonce.total_size() == MainHeader::AES_NONCE_LEN);
		// get the IV and the Tag
		memcpy(nonce.data(), main_header.GetIV(), nonce.total_size());
		memcpy(tag.data(), main_header.GetTag(), tag.size());

		//! Decrypt the canary
		encryption_state->InitializeDecryption(nonce, derived_key);
		encryption_state->Process(main_header.GetEncryptedCanary(), decrypted_canary.size(), decrypted_canary.data(),
		                          decrypted_canary.size());
		try {
			encryption_state->Finalize(decrypted_canary.data(), decrypted_canary.size(), tag.data(), tag.size());
		} catch (const std::exception &e) {
			throw InvalidInputException("Wrong encryption key used to open the database file");
		}
		break;
	default:
		throw InvalidInputException("No valid encryption version found!");
	}

	//! compare to check whether the decrypted canary is correct
	if (memcmp(decrypted_canary.data(), MainHeader::CANARY, MainHeader::CANARY_BYTE_SIZE) != 0) {
		return false;
	}

	return true;
}

void MainHeader::Write(WriteStream &ser) {
	ser.WriteData(const_data_ptr_cast(MAGIC_BYTES), MAGIC_BYTE_SIZE);
	if (static_cast<StorageVersion>(version_number) >= StorageVersion::V2_0_0) {
		// from v2.0.0 we write 999, to indicate that the version number is deprecated
		version_number = static_cast<idx_t>(StorageVersion::DEPRECATED);
	}
	ser.Write<idx_t>(version_number);
	for (idx_t i = 0; i < FLAG_COUNT; i++) {
		ser.Write<uint64_t>(flags[i]);
	}

	SerializeVersionNumber(ser, DuckDB::LibraryVersion());
	SerializeVersionNumber(ser, DuckDB::SourceID());

	// We always serialize, and write zeros, if not set.
	auto encryption_enabled = IsEncrypted();
	SerializeEncryptionMetadata(ser, encryption_metadata, encryption_enabled);
	SerializeDBIdentifier(ser, db_identifier);
	SerializeEncryptionMetadata(ser, encrypted_canary, encryption_enabled);
	SerializeIV(ser, canary_iv, encryption_enabled);
	SerializeTag(ser, canary_tag, encryption_enabled);
}

void MainHeader::CheckMagicBytes(QueryContext context, FileHandle &handle) {
	data_t magic_bytes[MAGIC_BYTE_SIZE];
	if (handle.GetFileSize() < MainHeader::MAGIC_BYTE_SIZE + MainHeader::MAGIC_BYTE_OFFSET) {
		throw IOException("The file \"%s\" exists, but it is not a valid DuckDB database file!", handle.path);
	}
	handle.Read(context, magic_bytes, MainHeader::MAGIC_BYTE_SIZE, MainHeader::MAGIC_BYTE_OFFSET);
	if (memcmp(magic_bytes, MainHeader::MAGIC_BYTES, MainHeader::MAGIC_BYTE_SIZE) != 0) {
		throw IOException("The file \"%s\" exists, but it is not a valid DuckDB database file!", handle.path);
	}
}

void MainHeader::CheckMagicBytes(MemoryMappedFile &handle) {
	auto magic_bytes = handle.GetData(MainHeader::MAGIC_BYTE_OFFSET, MainHeader::MAGIC_BYTE_SIZE);
	if (memcmp(magic_bytes, MainHeader::MAGIC_BYTES, MainHeader::MAGIC_BYTE_SIZE) != 0) {
		throw IOException("The file \"%s\" exists, but it is not a valid DuckDB database file!", handle.GetPath());
	}
}

static void ShowUnsupportedStorageVersionError(const idx_t version_number) {
	// Check the version number to determine if we can read this file.
	auto version = GetDuckDBVersions(static_cast<StorageVersion>(version_number));
	string version_text;
	if (!version.empty()) {
		// Known version.
		version_text = "DuckDB version " + string(version);
	} else if (version_number > VERSION_NUMBER_UPPER) {
		version_text = "a newer version of DuckDB";
	} else {
		version_text = "an older development version of DuckDB";
	}
	throw IOException(
	    "Trying to read a database file with storage version number %lld, but we can only read storage versions "
	    "between %lld and %lld.\n"
	    "The database file was created with %s.\n\n"
	    "Newer DuckDB version might introduce backward incompatible changes (possibly guarded by compatibility "
	    "settings).\n"
	    "See the storage page for migration strategy and more information: https://duckdb.org/internals/storage",
	    version_number, VERSION_NUMBER_LOWER, VERSION_NUMBER_UPPER, version_text);
}

MainHeader MainHeader::Read(ReadStream &source) {
	data_t magic_bytes[MAGIC_BYTE_SIZE];

	MainHeader header;
	source.ReadData(magic_bytes, MainHeader::MAGIC_BYTE_SIZE);
	if (memcmp(magic_bytes, MainHeader::MAGIC_BYTES, MainHeader::MAGIC_BYTE_SIZE) != 0) {
		throw IOException("The file is not a valid DuckDB database file!");
	}

	header.version_number = source.Read<idx_t>();

	if (static_cast<StorageVersion>(header.version_number) == DEPRECATED_VERSION_NUMBER) {
		// if the version number in the main header is deprecated, then we just ignore the main header version number
		// TODO: if we are confident, we can remove the check below
	} else if (header.version_number < VERSION_NUMBER_LOWER || header.version_number > VERSION_NUMBER_UPPER) {
		ShowUnsupportedStorageVersionError(header.version_number);
	}

	// Read the flags.
	for (idx_t i = 0; i < FLAG_COUNT; i++) {
		header.flags[i] = source.Read<uint64_t>();
	}
	DeserializeVersionNumber(source, header.library_git_desc);
	DeserializeVersionNumber(source, header.library_git_hash);

	// We always deserialize, and read zeros, if not set.
	DeserializeEncryptionData(source, header.encryption_metadata, MainHeader::ENCRYPTION_METADATA_LEN);
	DeserializeEncryptionData(source, header.db_identifier, MainHeader::DB_IDENTIFIER_LEN);
	DeserializeEncryptionData(source, header.encrypted_canary, MainHeader::CANARY_BYTE_SIZE);
	DeserializeEncryptionData(source, header.canary_iv, MainHeader::AES_NONCE_LEN);
	DeserializeEncryptionData(source, header.canary_tag, MainHeader::AES_TAG_LEN);

	return header;
}

void DatabaseHeader::Write(WriteStream &ser) {
	ser.Write<uint64_t>(iteration);
	ser.Write<idx_t>(meta_block);
	ser.Write<idx_t>(free_list);
	ser.Write<uint64_t>(block_count);
	ser.Write<idx_t>(block_alloc_size);
	ser.Write<idx_t>(vector_size);

	if (storage_compatibility < StorageVersion::V2_0_0) {
		string storage_version_string = StorageVersionInfo::GetStorageVersionString(storage_compatibility);
		auto ser_version = GetSerializationVersionDeprecated(storage_version_string.c_str());
		ser.Write<idx_t>(ser_version);
	} else {
		ser.Write<idx_t>(static_cast<idx_t>(storage_compatibility));
	}
}

void DatabaseHeader::SetStorageVersionInDatabaseHeader(DatabaseHeader &header, StorageVersion main_version,
                                                       StorageVersion read_version) {
	if ((main_version == MainHeader::DEPRECATED_VERSION_NUMBER) || (read_version >= StorageVersion::V2_0_0)) {
		// From v2.0.0 onwards, we use and store only the storage version number
		switch (read_version) {
		case StorageVersion::V2_0_0:
			header.storage_compatibility = StorageVersion::V2_0_0;
			break;
		case StorageVersion::V2_1_0:
			header.storage_compatibility = StorageVersion::V2_1_0;
			break;
			// new versions should be added here
		default:
			if (static_cast<idx_t>(read_version) > VERSION_NUMBER_UPPER) {
				ShowUnsupportedStorageVersionError(static_cast<idx_t>(read_version));
			}
			throw InvalidInputException("Unsupported Storage Version '%d' in the database header!",
			                            static_cast<idx_t>(read_version));
		}
	} else {
		// Before V2.0.0 the Storage Version in the main header could be written in two different ways
		// 1) When the DB is created from scratch -- with e.g. ATTACH (STORAGE_VERSION "v1.4.0")
		// 2) if the db file got bumped to a higher version
		// (e.g. "ATTACH 'bump.dp' (STORAGE_VERSION 'v.1.5.0'), when bump.db already exists")
		// in case 1, the explicit storage version is serialized in the MAIN header (e.g. 1.4.0 = 67, in the example)
		// in case 2, the version number in the MAIN header is bumped to at most v65 (v1.2.0)
		// thus, in case 2, the main header storage version is often lower then the actual storage version
		// that's also why we need the logic below for backwards compatibility
		// if the main header version and db header version are < v2.0.0
		// then we fall back to the serialization version
		switch (static_cast<idx_t>(read_version)) {
			// In some old duckdb versions, storage version (64)
			// is (by mistake) serialized instead of serialization version
		case static_cast<idx_t>(StorageVersion::INVALID):
			// If read version is 0
		case static_cast<idx_t>(SerializationVersionDeprecated::V0_10_2):
		case static_cast<idx_t>(StorageVersion::V0_10_2):
		case static_cast<idx_t>(SerializationVersionDeprecated::V1_0_0):
		case static_cast<idx_t>(SerializationVersionDeprecated::V1_1_0):
			header.storage_compatibility = StorageVersion::V0_10_2;
			break;
		case static_cast<idx_t>(SerializationVersionDeprecated::V1_2_0):
			header.storage_compatibility = StorageVersion::V1_2_0;
			break;
		case static_cast<idx_t>(SerializationVersionDeprecated::V1_3_0):
			header.storage_compatibility = StorageVersion::V1_3_0;
			break;
		case static_cast<idx_t>(SerializationVersionDeprecated::V1_4_0):
			header.storage_compatibility = StorageVersion::V1_4_0;
			break;
		case static_cast<idx_t>(SerializationVersionDeprecated::V1_5_0):
			header.storage_compatibility = StorageVersion::V1_5_0;
			break;
		default:
			throw InvalidInputException("Deprecated Serialization Version '%d' is not found!",
			                            static_cast<idx_t>(read_version));
		}
	}
}

DatabaseHeader DatabaseHeader::Read(const MainHeader &main_header, ReadStream &source) {
	DatabaseHeader header;
	header.iteration = source.Read<uint64_t>();
	header.meta_block = source.Read<idx_t>();
	header.free_list = source.Read<idx_t>();
	header.block_count = source.Read<uint64_t>();
	header.block_alloc_size = source.Read<idx_t>();

	// backwards compatibility
	if (!header.block_alloc_size) {
		header.block_alloc_size = DEFAULT_BLOCK_ALLOC_SIZE;
	}

	header.vector_size = source.Read<idx_t>();
	if (!header.vector_size) {
		// backwards compatibility
		header.vector_size = DEFAULT_STANDARD_VECTOR_SIZE;
	}
	if (header.vector_size != STANDARD_VECTOR_SIZE) {
		throw IOException("Cannot read database file: DuckDB's compiled vector size is %llu bytes, but the file has a "
		                  "vector size of %llu bytes.",
		                  STANDARD_VECTOR_SIZE, header.vector_size);
	}

	// storage version from the database header
	auto h_storage_version = source.Read<idx_t>();
	auto database_header_storage_version = static_cast<StorageVersion>(h_storage_version);
	SetStorageVersionInDatabaseHeader(header, static_cast<StorageVersion>(main_header.version_number),
	                                  database_header_storage_version);

	return header;
}

template <class T>
void SerializeHeaderStructure(T header, data_ptr_t ptr) {
	MemoryStream ser(ptr, Storage::FILE_HEADER_SIZE);
	header.Write(ser);
}

MainHeader DeserializeMainHeader(data_ptr_t ptr) {
	MemoryStream source(ptr, Storage::FILE_HEADER_SIZE);
	return MainHeader::Read(source);
}

DatabaseHeader DeserializeDatabaseHeader(const MainHeader &main_header, data_ptr_t ptr) {
	MemoryStream source(ptr, Storage::FILE_HEADER_SIZE);
	return DatabaseHeader::Read(main_header, source);
}

SingleFileBlockManager::SingleFileBlockManager(AttachedDatabase &db_p, const string &path_p,
                                               const StorageManagerOptions &options)
    : BlockManager(BufferManager::GetBufferManager(db_p), options.block_alloc_size, options.block_header_size),
      db(db_p), path(path_p), header_buffer(BlockAllocator::Get(db_p), FileBufferType::MANAGED_BUFFER,
                                            Storage::FILE_HEADER_SIZE - options.block_header_size.GetIndex(),
                                            options.block_header_size.GetIndex()),
      iteration_count(0), options(options) {
}

SingleFileBlockManager::~SingleFileBlockManager() {
	// flip the flag to not perform UnregisterBlock on the block manager that is being destructed
	this->in_destruction = true;
}

void SingleFileBlockManager::AddStorageVersionTag() {
	db.tags["storage_version"] = GetStorageVersionName(options.storage_version, true);
}

StorageVersion SingleFileBlockManager::GetVersionNumber() const {
	auto storage_version = options.storage_version;
	if (StorageManager::IsPriorToVersion(StorageVersion::V1_2_0, storage_version)) {
		return StorageVersion::V0_10_2;
	}
	// Look up the matching version number.
	auto version_name = GetStorageVersionName(storage_version, false);
	return GetStorageVersion(version_name.c_str());
}

MainHeader ConstructMainHeader(StorageVersion version_number) {
	MainHeader header;
	header.version_number = static_cast<idx_t>(version_number);
	memset(header.flags, 0, sizeof(uint64_t) * MainHeader::FLAG_COUNT);
	return header;
}

void SingleFileBlockManager::StoreEncryptedCanary(AttachedDatabase &db, MainHeader &main_header, const string &key_id) {
	const_data_ptr_t key = EncryptionEngine::GetKeyFromCache(db.GetDatabase(), key_id);
	// Encrypt canary with the derived key
	shared_ptr<EncryptionState> encryption_state;
	auto encryption_version = static_cast<EncryptionTypes::EncryptionVersion>(main_header.GetEncryptionVersion());
	if (encryption_version > EncryptionTypes::V0_0 && encryption_version != EncryptionTypes::NONE) {
		// From Encryption Version 1+, always encrypt canary with GCM
		auto metadata = make_uniq<EncryptionStateMetadata>(
		    EncryptionTypes::GCM, MainHeader::DEFAULT_ENCRYPTION_KEY_LENGTH, encryption_version);
		encryption_state =
		    db.GetDatabase().GetEncryptionUtil(db.IsReadOnly())->CreateEncryptionState(std::move(metadata));
	} else {
		auto metadata = make_uniq<EncryptionStateMetadata>(
		    static_cast<EncryptionTypes::CipherType>(main_header.GetEncryptionCipher()),
		    MainHeader::DEFAULT_ENCRYPTION_KEY_LENGTH, encryption_version);
		encryption_state =
		    db.GetDatabase().GetEncryptionUtil(db.IsReadOnly())->CreateEncryptionState(std::move(metadata));
	}

	EncryptCanary(main_header, encryption_state, key);
}

void SingleFileBlockManager::StoreDBIdentifier(MainHeader &main_header, data_ptr_t db_identifier) {
	main_header.SetDBIdentifier(db_identifier);
}

template <typename T>
void SingleFileBlockManager::WriteEncryptionData(MemoryStream &stream, const T &val) {
	stream.WriteData(reinterpret_cast<const_data_ptr_t>(&val), sizeof(val));
}

void SingleFileBlockManager::StoreEncryptionMetadata(MainHeader &main_header) const {
	// The first byte is the key derivation function (kdf).
	// The second byte is for the usage of AAD.
	// The third byte is for the cipher.
	// The subsequent byte is empty.
	// The last 4 bytes are the key length.
	auto metadata_stream = make_uniq<MemoryStream>(ENCRYPTION_METADATA_LEN);

	WriteEncryptionData<uint8_t>(*metadata_stream, options.encryption_options.kdf);
	WriteEncryptionData<uint8_t>(*metadata_stream, options.encryption_options.additional_authenticated_data);
	WriteEncryptionData<uint8_t>(*metadata_stream, db.GetStorageManager().GetCipher());
	WriteEncryptionData<uint8_t>(*metadata_stream, options.encryption_options.encryption_version);
	WriteEncryptionData<uint32_t>(*metadata_stream, options.encryption_options.key_length);

	main_header.SetEncryptionMetadata(metadata_stream->GetData());
}

void SingleFileBlockManager::CheckAndAddEncryptionKey(MainHeader &main_header, string &user_key) {
	//! Get the database identifier.
	uint8_t db_identifier[MainHeader::DB_IDENTIFIER_LEN];
	memset(db_identifier, 0, MainHeader::DB_IDENTIFIER_LEN);
	memcpy(db_identifier, main_header.GetDBIdentifier(), MainHeader::DB_IDENTIFIER_LEN);

	//! Check if the correct key is used to decrypt the database
	// Derive the encryption key and add it to cache
	data_t derived_key[MainHeader::DEFAULT_ENCRYPTION_KEY_LENGTH];
	EncryptionKeyManager::DeriveKey(user_key, db_identifier, derived_key);

	shared_ptr<EncryptionState> encryption_state;
	auto encryption_version = static_cast<EncryptionTypes::EncryptionVersion>(main_header.GetEncryptionVersion());
	if (encryption_version > EncryptionTypes::V0_0 && encryption_version != EncryptionTypes::NONE) {
		// From Encryption Version 1+, always encrypt canary with GCM
		auto metadata = make_uniq<EncryptionStateMetadata>(
		    EncryptionTypes::GCM, MainHeader::DEFAULT_ENCRYPTION_KEY_LENGTH, encryption_version);
		encryption_state =
		    db.GetDatabase().GetEncryptionUtil(db.IsReadOnly())->CreateEncryptionState(std::move(metadata));
	} else {
		auto metadata = make_uniq<EncryptionStateMetadata>(
		    static_cast<EncryptionTypes::CipherType>(main_header.GetEncryptionCipher()),
		    MainHeader::DEFAULT_ENCRYPTION_KEY_LENGTH, encryption_version);
		encryption_state =
		    db.GetDatabase().GetEncryptionUtil(db.IsReadOnly())->CreateEncryptionState(std::move(metadata));
	}

	if (!DecryptCanary(main_header, encryption_state, derived_key)) {
		throw InvalidInputException("Wrong encryption key used to open the database file");
	}

	options.encryption_options.derived_key_id = EncryptionEngine::AddKeyToCache(db.GetDatabase(), derived_key);
	auto &catalog = db.GetCatalog().Cast<DuckCatalog>();
	catalog.SetEncryptionKeyId(options.encryption_options.derived_key_id);
	catalog.SetIsEncrypted();

	std::fill(user_key.begin(), user_key.end(), 0);
	user_key.clear();
}

void SingleFileBlockManager::CheckAndAddEncryptionKey(MainHeader &main_header) {
	return CheckAndAddEncryptionKey(main_header, *options.encryption_options.user_key);
}

void SingleFileBlockManager::CreateNewDatabase(QueryContext context) {
	auto encryption_enabled = options.encryption_options.encryption_enabled;
	if (encryption_enabled) {
		// Check if we can read/write the encrypted database
		db.GetDatabase().GetEncryptionUtil(options.read_only);
	}

	// MAP mode opens only the mmap; other modes open the FileHandle.
	handle = DatabaseHandle::Open(context, db, path, options, DatabaseOpenMode::CREATE_NEW_FILE);
	header_buffer.Clear();

	if (options.storage_version == StorageVersion::INVALID) {
		options.storage_version = StorageCompatibility::Latest().storage_version;
	}

	options.version_number = GetVersionNumber();
	db.GetStorageManager().SetStorageVersion(options.storage_version);
	GetMetadataManager().SetStorageVersion(options.storage_version);
	AddStorageVersionTag();

	MainHeader main_header = ConstructMainHeader(options.version_number);

	// Derive the encryption key and add it to the cache.
	// Not used for plain databases.
	data_t derived_key[MainHeader::DEFAULT_ENCRYPTION_KEY_LENGTH];

	// We need the unique database identifier, if the storage version is new enough.
	// If encryption is enabled, we also use it as the salt.
	memset(options.db_identifier, 0, MainHeader::DB_IDENTIFIER_LEN);
	if (encryption_enabled || StorageManager::TargetAtLeastVersion(StorageVersion::V1_4_0, options.version_number)) {
		GenerateDBIdentifier(options.db_identifier);
	}

	if (encryption_enabled) {
		// The key is given via ATTACH.
		EncryptionKeyManager::DeriveKey(*options.encryption_options.user_key, options.db_identifier, derived_key);
		options.encryption_options.user_key = nullptr;

		// if no encryption cipher is specified, use GCM
		if (db.GetStorageManager().GetCipher() == EncryptionTypes::INVALID) {
			db.GetStorageManager().SetCipher(EncryptionTypes::GCM);
		}

		// Set the encrypted DB bit to 1.
		main_header.SetEncrypted();

		if (options.encryption_options.encryption_version == EncryptionTypes::NONE) {
			throw InvalidConfigurationException("No Encryption type set");
		}

		main_header.SetEncryptionVersion(options.encryption_options.encryption_version);

		// The derived key is wiped in AddKeyToCache.
		options.encryption_options.derived_key_id = EncryptionEngine::AddKeyToCache(db.GetDatabase(), derived_key);
		auto &catalog = db.GetCatalog().Cast<DuckCatalog>();
		catalog.SetEncryptionKeyId(options.encryption_options.derived_key_id);
		catalog.SetIsEncrypted();
	}

	// Store all metadata in the main header.
	if (encryption_enabled) {
		StoreEncryptionMetadata(main_header);
	}
	// Always store the database identifier.
	StoreDBIdentifier(main_header, options.db_identifier);
	if (encryption_enabled) {
		StoreEncryptedCanary(db, main_header, options.encryption_options.derived_key_id);
	}

	// Write the main database header.
	SerializeHeaderStructure<MainHeader>(main_header, header_buffer.GetDataMutable());
	ChecksumAndWrite(context, header_buffer, 0, true);

	// write the database headers
	// initialize meta_block and free_list to INVALID_BLOCK because the database file does not contain any actual
	// content yet
	DatabaseHeader h1;
	// header 1
	h1.iteration = 0;
	h1.meta_block = idx_t(INVALID_BLOCK);
	h1.free_list = idx_t(INVALID_BLOCK);
	h1.block_count = 0;
	// We create the SingleFileBlockManager with the desired block allocation size before calling CreateNewDatabase.
	h1.block_alloc_size = GetBlockAllocSize();
	h1.vector_size = STANDARD_VECTOR_SIZE;
	h1.storage_compatibility = options.storage_version;
	SerializeHeaderStructure<DatabaseHeader>(h1, header_buffer.GetDataMutable());
	ChecksumAndWrite(context, header_buffer, Storage::FILE_HEADER_SIZE);

	// header 2
	DatabaseHeader h2;
	h2.iteration = 0;
	h2.meta_block = idx_t(INVALID_BLOCK);
	h2.free_list = idx_t(INVALID_BLOCK);
	h2.block_count = 0;
	// We create the SingleFileBlockManager with the desired block allocation size before calling CreateNewDatabase.
	h2.block_alloc_size = GetBlockAllocSize();
	h2.vector_size = STANDARD_VECTOR_SIZE;
	h2.storage_compatibility = options.storage_version;
	SerializeHeaderStructure<DatabaseHeader>(h2, header_buffer.GetDataMutable());
	ChecksumAndWrite(context, header_buffer, Storage::FILE_HEADER_SIZE * 2ULL);

	// ensure that writing to disk is completed before returning
	handle->Sync();
	// we start with h2 as active_header, this way our initial write will be in h1
	iteration_count = 0;
	active_header = 1;
	max_block = 0;
}

void SingleFileBlockManager::LoadExistingDatabase(QueryContext context) {
	handle = DatabaseHandle::Open(context, db, path, options, DatabaseOpenMode::OPEN_EXISTING_FILE);
	handle->CheckMagicBytes(context);

	// otherwise, we check the metadata of the file
	ReadAndChecksum(context, header_buffer, 0, true);

	uint64_t delta = 0;
	if (GetBlockHeaderSize() > DEFAULT_BLOCK_HEADER_STORAGE_SIZE) {
		delta = GetBlockHeaderSize() - DEFAULT_BLOCK_HEADER_STORAGE_SIZE;
	}

	MainHeader main_header = DeserializeMainHeader(header_buffer.GetDataMutable() - delta);
	memcpy(options.db_identifier, main_header.GetDBIdentifier(), MainHeader::DB_IDENTIFIER_LEN);

	if (!main_header.IsEncrypted() && options.encryption_options.encryption_enabled) {
		throw CatalogException("A key is explicitly specified, but database \"%s\" is not encrypted", path);
		// database is not encrypted, but is tried to be opened with a key
	}

	if (main_header.IsEncrypted()) {
		auto &storage_manager = db.GetStorageManager();
		if (options.encryption_options.encryption_enabled) {
			//! Encryption is set
			D_ASSERT(db.GetStorageManager().IsEncrypted());
			options.encryption_options.encryption_version =
			    static_cast<EncryptionTypes::EncryptionVersion>(main_header.GetEncryptionVersion());

			//! Check if our encryption module can write, if not, we throw
			db.GetDatabase().GetEncryptionUtil(options.read_only);

			//! Check if the given key upon attach is correct
			// Derive the encryption key and add it to cache
			CheckAndAddEncryptionKey(main_header);
			// delete user key ptr
			options.encryption_options.user_key = nullptr;
		} else {
			// if encrypted, but no encryption key given
			throw CatalogException("Cannot open encrypted database \"%s\" without a key", path);
		}

		// if a cipher was provided, check if it is the same as in the config
		auto stored_cipher = static_cast<EncryptionTypes::CipherType>(main_header.GetEncryptionCipher());
		auto config_cipher = storage_manager.GetCipher();
		if (config_cipher != EncryptionTypes::INVALID && config_cipher != stored_cipher) {
			throw CatalogException("Cannot open encrypted database \"%s\" with a different cipher (%s) than the one "
			                       "used to create it (%s)",
			                       path, EncryptionTypes::CipherToString(config_cipher),
			                       EncryptionTypes::CipherToString(stored_cipher));
		}

		// This avoids the cipher from being downgrades by an attacker
		// FIXME: we likely want to have a proper validation
		// of the cipher used instead of this trick to avoid downgrades
		if (stored_cipher != EncryptionTypes::GCM) {
			if (config_cipher == EncryptionTypes::INVALID) {
				throw CatalogException(
				    "Cannot open encrypted database \"%s\" without explicitly specifying the "
				    "encryption cipher for security reasons. Please make sure you understand the security implications "
				    "and re-attach the database specifying the desired cipher.",
				    path);
			}
		}

		// this is ugly, but the storage manager does not know the cipher type before
		storage_manager.SetCipher(stored_cipher);
		// encryption version can be overridden by the serialized encryption version
		storage_manager.SetEncryptionVersion(
		    static_cast<EncryptionTypes::EncryptionVersion>(main_header.GetEncryptionVersion()));
	}

	options.version_number = static_cast<StorageVersion>(main_header.version_number);

	// read the database headers from disk
	DatabaseHeader h1;
	ReadAndChecksum(context, header_buffer, Storage::FILE_HEADER_SIZE);
	h1 = DeserializeDatabaseHeader(main_header, header_buffer.GetDataMutable());

	DatabaseHeader h2;
	ReadAndChecksum(context, header_buffer, Storage::FILE_HEADER_SIZE * 2ULL);
	h2 = DeserializeDatabaseHeader(main_header, header_buffer.GetDataMutable());

	// check the header with the highest iteration count
	if (h1.iteration > h2.iteration) {
		// h1 is active header
		active_header = 0;
		Initialize(h1, GetOptionalBlockAllocSize());
	} else {
		// h2 is active header
		active_header = 1;
		Initialize(h2, GetOptionalBlockAllocSize());
	}
	AddStorageVersionTag();
	LoadFreeList(context);
}

void SingleFileBlockManager::CheckChecksum(data_ptr_t start_ptr, uint64_t delta, bool skip_block_header) const {
	uint64_t stored_checksum;
	uint64_t computed_checksum;

	if (skip_block_header && delta > 0) {
		//! Even with encryption enabled, the main header should be plaintext
		stored_checksum = Load<uint64_t>(start_ptr);
		computed_checksum = Checksum(start_ptr + DEFAULT_BLOCK_HEADER_STORAGE_SIZE, GetBlockSize() + delta);
	} else {
		//! We do have to decrypt other headers
		stored_checksum = Load<uint64_t>(start_ptr + delta);
		computed_checksum = Checksum(start_ptr + GetBlockHeaderSize(), GetBlockSize());
	}

	// verify the checksum
	if (stored_checksum != computed_checksum) {
		throw DataCorruptionException(
		    "Corrupt database file: computed checksum %llu does not match stored checksum %llu in block "
		    "at location %llu",
		    computed_checksum, stored_checksum, start_ptr);
	}
}

void SingleFileBlockManager::CheckChecksum(FileBuffer &block, uint64_t location, uint64_t delta,
                                           bool skip_block_header) const {
	uint64_t stored_checksum;
	uint64_t computed_checksum;

	if (skip_block_header && delta > 0) {
		//! Even with encryption enabled, the main header should be plaintext
		stored_checksum = Load<uint64_t>(block.InternalBuffer());
		computed_checksum = Checksum(block.GetDataMutable() - delta, block.Size() + delta);
	} else {
		//! We do have to decrypt other headers
		stored_checksum = Load<uint64_t>(block.InternalBuffer() + delta);
		computed_checksum = Checksum(block.GetDataMutable(), block.Size());
	}

	// verify the checksum
	if (stored_checksum != computed_checksum) {
		throw DataCorruptionException(
		    "Corrupt database file: computed checksum %llu does not match stored checksum %llu in block "
		    "at location %llu",
		    computed_checksum, stored_checksum, location);
	}
}

void SingleFileBlockManager::ReadAndChecksum(QueryContext context, FileBuffer &block, uint64_t location,
                                             bool skip_block_header) const {
	// read the buffer from disk
	handle->Read(context, block, location);

	//! calculate delta header bytes (if any)
	uint64_t delta = GetBlockHeaderSize() - Storage::DEFAULT_BLOCK_HEADER_SIZE;

	if (options.encryption_options.encryption_enabled && !skip_block_header) {
		auto key_id = options.encryption_options.derived_key_id;
		EncryptionEngine::DecryptBlock(db, key_id, block.InternalBuffer(), block.Size(), delta);
	}

	CheckChecksum(block, location, delta, skip_block_header);
}

void SingleFileBlockManager::ChecksumAndWrite(QueryContext context, FileBuffer &block, uint64_t location,
                                              bool skip_block_header) const {
	auto delta = GetBlockHeaderSize() - Storage::DEFAULT_BLOCK_HEADER_SIZE;
	uint64_t checksum;

	if (skip_block_header && delta > 0) {
		//! This happens only for the main database header
		//! We do not encrypt the main database header
		memmove(block.InternalBuffer() + Storage::DEFAULT_BLOCK_HEADER_SIZE, block.GetDataMutable(), block.Size());
		//! zero out the last bytes of the block
		memset(block.InternalBuffer() + block.Size() + Storage::DEFAULT_BLOCK_HEADER_SIZE, 0, delta);
		checksum = Checksum(block.GetDataMutable() - delta, block.Size() + delta);
		delta = 0;
	} else {
		checksum = Checksum(block.GetDataMutable(), block.Size());
	}

	Store<uint64_t>(checksum, block.InternalBuffer() + delta);

	// encrypt if required
	unique_ptr<FileBuffer> temp_buffer_manager;
	if (options.encryption_options.encryption_enabled && !skip_block_header) {
		auto key_id = options.encryption_options.derived_key_id;
		temp_buffer_manager =
		    make_uniq<FileBuffer>(BlockAllocator::Get(db), block.GetBufferType(), block.Size(), GetBlockHeaderSize());
		EncryptionEngine::EncryptBlock(db, key_id, block, *temp_buffer_manager, delta);
		temp_buffer_manager->Write(context, handle->GetFileHandle(), location);
	} else {
		handle->Write(context, block, location);
	}
}

void SingleFileBlockManager::Initialize(const DatabaseHeader &header, const optional_idx block_alloc_size) {
	try {
		Storage::VerifyBlockAllocSize(header.block_alloc_size);
	} catch (const InvalidInputException &) {
		throw DataCorruptionException("Corrupt database file: invalid block allocation size %llu",
		                              header.block_alloc_size);
	}

	free_list_id = header.free_list;
	meta_block = header.meta_block;
	iteration_count = header.iteration;
	if (header.block_count > static_cast<uint64_t>(NumericLimits<block_id_t>::Maximum())) {
		throw DataCorruptionException("Corrupt database file: invalid block count %llu", header.block_count);
	}
	max_block = UnsafeNumericCast<block_id_t>(header.block_count);
	if (options.storage_version != StorageVersion::INVALID) {
		// storage version specified explicitly - use requested storage version
		auto requested_compat_version = options.storage_version;
		if (requested_compat_version < header.storage_compatibility) {
			throw InvalidInputException(
			    "Error opening \"%s\": cannot initialize database with storage version %d - which is lower than what "
			    "the database itself uses (%d). The storage version of an existing database cannot be lowered.",
			    path, requested_compat_version, header.storage_compatibility);
		}
	} else {
		// load storage version from header
		options.storage_version = header.storage_compatibility;
	}
	if (header.storage_compatibility > StorageCompatibility::Latest().storage_version) {
		throw InvalidInputException(
		    "Error opening \"%s\": file was written with a storage version greater than the latest version supported "
		    "by this DuckDB instance. Try opening the file with a newer version of DuckDB.",
		    path);
	}

	db.GetStorageManager().SetStorageVersion(options.storage_version);
	GetMetadataManager().SetStorageVersion(options.storage_version);

	if (block_alloc_size.IsValid() && block_alloc_size.GetIndex() != header.block_alloc_size) {
		throw InvalidInputException(
		    "Error opening \"%s\": cannot initialize the same database with a different block size: provided block "
		    "size: %llu, file block size: %llu",
		    path, GetBlockAllocSize(), header.block_alloc_size);
	}

	SetBlockAllocSize(header.block_alloc_size);
}

void SingleFileBlockManager::RewriteMainHeader(QueryContext &context, StorageVersion version_number) {
	MainHeader main_header = ConstructMainHeader(version_number);
	SerializeHeaderStructure<MainHeader>(main_header, header_buffer.GetDataMutable());
	// now write the header to the file
	ChecksumAndWrite(context, header_buffer, 0);
	header_buffer.Clear();
	// avoid having the Sync at the end write two blocks
	handle->Sync();
}

void SingleFileBlockManager::LoadFreeList(QueryContext context) {
	MetaBlockPointer free_pointer(free_list_id, 0);
	if (!free_pointer.IsValid()) {
		// no free list
		return;
	}
	MetadataReader reader(GetMetadataManager(), free_pointer, nullptr, BlockReaderType::REGISTER_BLOCKS);
	auto free_list_count = reader.Read<uint64_t>(context);
	free_list.clear();
	for (idx_t i = 0; i < free_list_count; i++) {
		auto block = reader.Read<block_id_t>(context);
		free_list.insert(block);
	}
	auto multi_use_blocks_count = reader.Read<uint64_t>(context);
	multi_use_blocks.clear();
	for (idx_t i = 0; i < multi_use_blocks_count; i++) {
		auto block_id = reader.Read<block_id_t>(context);
		auto usage_count = reader.Read<uint32_t>(context);
		multi_use_blocks[block_id] = usage_count;
	}
	GetMetadataManager().Read(reader);
	GetMetadataManager().MarkBlocksAsModified();
}

bool SingleFileBlockManager::IsRootBlock(MetaBlockPointer root) {
	return root.block_pointer == meta_block;
}

block_id_t SingleFileBlockManager::GetFreeBlockIdInternal(FreeBlockType type) {
	lock_guard<mutex> lock(single_file_block_lock);
	block_id_t block_id;
	if (!free_list.empty()) {
		// The free list is not empty, so we take its first element.
		block_id = *free_list.begin();
		// erase the entry from the free list again
		free_list.erase(free_list.begin());
	} else {
		block_id = max_block++;
	}
	// add the entry to the list of newly used blocks
	if (type == FreeBlockType::NEWLY_USED_BLOCK) {
		newly_used_blocks.insert(block_id);
	}
	if (BlockIsRegistered(block_id)) {
		throw InternalException("Free block %d is already registered", block_id);
	}
	return block_id;
}

block_id_t SingleFileBlockManager::GetFreeBlockId() {
	return GetFreeBlockIdInternal(FreeBlockType::NEWLY_USED_BLOCK);
}

block_id_t SingleFileBlockManager::GetFreeBlockIdForCheckpoint() {
	return GetFreeBlockIdInternal(FreeBlockType::CHECKPOINTED_BLOCK);
}

block_id_t SingleFileBlockManager::PeekFreeBlockId() {
	lock_guard<mutex> lock(single_file_block_lock);
	if (!free_list.empty()) {
		return *free_list.begin();
	} else {
		return max_block;
	}
}

void SingleFileBlockManager::MarkBlockAsCheckpointed(block_id_t block_id) {
	lock_guard<mutex> lock(single_file_block_lock);
	D_ASSERT(block_id >= 0);
	newly_used_blocks.erase(block_id);
}

void SingleFileBlockManager::MarkBlockAsUsed(block_id_t block_id) {
	lock_guard<mutex> lock(single_file_block_lock);
	D_ASSERT(block_id >= 0);
	if (max_block <= block_id) {
		// the block is past the current max_block
		// in this case we need to increment  "max_block" to "block_id"
		// any blocks in the middle are added to the free list
		// i.e. if max_block = 0, and block_id = 3, we need to add blocks 1 and 2 to the free list
		while (max_block < block_id) {
			free_list.insert(max_block);
			max_block++;
		}
		max_block++;
	} else if (free_list.find(block_id) != free_list.end()) {
		// block is currently in the free list - erase
		free_list.erase(block_id);
	} else {
		// block is already in use - increase reference count
		IncreaseBlockReferenceCountInternal(block_id);
	}
}

void SingleFileBlockManager::MarkBlockAsModified(block_id_t block_id) {
	unique_lock<mutex> lock(single_file_block_lock);
	D_ASSERT(block_id >= 0);
	D_ASSERT(block_id < max_block);

	// check if the block is a multi-use block
	auto entry = multi_use_blocks.find(block_id);
	if (entry != multi_use_blocks.end()) {
		// it is! reduce the reference count of the block
		entry->second--;
		// check the reference count: is the block still a multi-use block?
		if (entry->second <= 1) {
			// no longer a multi-use block!
			multi_use_blocks.erase(entry);
		}
		return;
	}
	// Check for multi-free
	if (modified_blocks.find(block_id) != modified_blocks.end()) {
		throw InternalException("MarkBlockAsModified called with already modified block id %d", block_id);
	}
	if (free_list.find(block_id) != free_list.end()) {
		throw InternalException("MarkBlockAsModified called with already freed block id %d", block_id);
	}
	auto newly_used_entry = newly_used_blocks.find(block_id);
	if (newly_used_entry != newly_used_blocks.end()) {
		// this block was newly used - and now we are labeling it as no longer being required
		// we can directly add it back to the free list
		newly_used_blocks.erase(block_id);
		AddFreeBlock(lock, block_id);
	} else {
		// this block was used in storage, we cannot directly re-use it
		// add it to the modified blocks indicating it will be re-usable after the next checkpoint
		modified_blocks.insert(block_id);
	}
}

void SingleFileBlockManager::IncreaseBlockReferenceCountInternal(block_id_t block_id) {
	D_ASSERT(block_id >= 0);
	D_ASSERT(block_id < max_block);
	D_ASSERT(free_list.find(block_id) == free_list.end());
	auto entry = multi_use_blocks.find(block_id);
	if (entry != multi_use_blocks.end()) {
		entry->second++;
	} else {
		multi_use_blocks[block_id] = 2;
	}
}

void SingleFileBlockManager::VerifyBlocks(const unordered_map<block_id_t, idx_t> &block_usage_count) {
	// probably don't need this?
	lock_guard<mutex> lock(single_file_block_lock);
	// all blocks should be accounted for - either in the block_usage_count, or in the free list
	set<block_id_t> referenced_blocks;
	for (auto &block : block_usage_count) {
		if (block.first == INVALID_BLOCK) {
			continue;
		}
		if (block.first >= max_block) {
			throw InternalException("Block %lld is used, but it is bigger than the max block %d", block.first,
			                        max_block);
		}
		referenced_blocks.insert(block.first);
		if (block.second > 1) {
			// multi-use block
			auto entry = multi_use_blocks.find(block.first);
			if (entry == multi_use_blocks.end()) {
				throw InternalException("Block %lld was used %llu times, but not present in multi_use_blocks",
				                        block.first, block.second);
			}
			if (entry->second != block.second) {
				throw InternalException(
				    "Block %lld was used %llu times, but multi_use_blocks says it is used %llu times", block.first,
				    block.second, entry->second);
			}
		} else {
			D_ASSERT(block.second > 0);
			auto entry = free_list.find(block.first);
			if (entry != free_list.end()) {
				throw InternalException("Block %lld was used, but it is present in the free list", block.first);
			}
		}
	}
	for (auto &newly_used_block : newly_used_blocks) {
		referenced_blocks.insert(newly_used_block);
	}
	for (auto &free_block : free_list) {
		referenced_blocks.insert(free_block);
	}
	for (auto &free_block : free_blocks_in_use) {
		referenced_blocks.insert(free_block);
	}
	if (referenced_blocks.size() != NumericCast<idx_t>(max_block)) {
		// not all blocks are accounted for
		string missing_blocks;
		for (block_id_t i = 0; i < max_block; i++) {
			if (referenced_blocks.find(i) == referenced_blocks.end()) {
				if (!missing_blocks.empty()) {
					missing_blocks += ", ";
				}
				missing_blocks += to_string(i);
			}
		}
		string free_list_str;
		for (auto &block : free_list) {
			if (!free_list_str.empty()) {
				free_list_str += ", ";
			}
			free_list_str += to_string(block);
		}
		string block_usage_str;
		for (auto &entry : block_usage_count) {
			if (!block_usage_str.empty()) {
				block_usage_str += ", ";
			}
			block_usage_str += to_string(entry.first);
		}
		string multi_use_blocks_str;
		for (auto &entry : multi_use_blocks) {
			if (!multi_use_blocks_str.empty()) {
				multi_use_blocks_str += ", ";
			}
			multi_use_blocks_str += to_string(entry.first);
		}
		string newly_used_blocks_str;
		for (auto &block : newly_used_blocks) {
			if (!newly_used_blocks_str.empty()) {
				newly_used_blocks_str += ", ";
			}
			newly_used_blocks_str += to_string(block);
		}

		throw InternalException(
		    "Block verification failed - blocks \"%s\" were not found as being used OR marked as free\nMax block: "
		    "%d\nBlock usage: %s\nFree list: %s\nMulti-use blocks: %s\nNewly used blocks: %s",
		    missing_blocks, max_block, block_usage_str, free_list_str, multi_use_blocks_str, newly_used_blocks_str);
	}
}

void SingleFileBlockManager::IncreaseBlockReferenceCount(block_id_t block_id) {
	lock_guard<mutex> lock(single_file_block_lock);
	IncreaseBlockReferenceCountInternal(block_id);
}

idx_t SingleFileBlockManager::GetMetaBlock() {
	return meta_block;
}

idx_t SingleFileBlockManager::TotalBlocks() {
	lock_guard<mutex> lock(single_file_block_lock);
	return NumericCast<idx_t>(max_block);
}

idx_t SingleFileBlockManager::FreeBlocks() {
	lock_guard<mutex> lock(single_file_block_lock);
	return free_list.size();
}

bool SingleFileBlockManager::IsRemote() {
	return !handle->OnDiskFile();
}

bool SingleFileBlockManager::Prefetch() {
	switch (Settings::Get<StorageBlockPrefetchSetting>(db.GetDatabase())) {
	case StorageBlockPrefetch::NEVER:
		return false;
	case StorageBlockPrefetch::DEBUG_FORCE_ALWAYS:
	case StorageBlockPrefetch::ALWAYS_PREFETCH:
		return !InMemory();
	case StorageBlockPrefetch::REMOTE_ONLY:
		return IsRemote();
	default:
		throw InternalException("Unknown StorageBlockPrefetch type");
	}
}

unique_ptr<Block> SingleFileBlockManager::ConvertBlock(block_id_t block_id, FileBuffer &source_buffer) {
	D_ASSERT(source_buffer.AllocSize() == GetBlockAllocSize());
	// FIXME; maybe we should pass the block header size explicitly
	return make_uniq<Block>(source_buffer, block_id, GetBlockHeaderSize());
}

unique_ptr<Block> SingleFileBlockManager::CreateBlock(block_id_t block_id, FileBuffer *source_buffer) {
	// FIXME; maybe we should pass the block header size explicitly
	unique_ptr<Block> result;
	if (source_buffer) {
		result = ConvertBlock(block_id, *source_buffer);
	} else {
		result = make_uniq<Block>(BlockAllocator::Get(db), block_id, *this);
	}
	result->Initialize(options.debug_initialize);
	return result;
}

idx_t SingleFileBlockManager::GetBlockLocation(block_id_t block_id) const {
	return BLOCK_START + NumericCast<idx_t>(block_id) * GetBlockAllocSize();
}

void SingleFileBlockManager::ReadBlock(data_ptr_t internal_buffer, uint64_t block_size, bool skip_block_header) const {
	//! calculate delta header bytes (if any)
	uint64_t delta = GetBlockHeaderSize() - Storage::DEFAULT_BLOCK_HEADER_SIZE;

	if (options.encryption_options.encryption_enabled && !skip_block_header) {
		EncryptionEngine::DecryptBlock(db, options.encryption_options.derived_key_id, internal_buffer, block_size,
		                               delta);
	}

	CheckChecksum(internal_buffer, delta, skip_block_header);
}

void SingleFileBlockManager::ReadBlock(Block &block, bool skip_block_header) const {
	// read the buffer from disk
	auto location = GetBlockLocation(block.id);
	handle->Read(QueryContext(), block, location);

	//! calculate delta header bytes (if any)
	uint64_t delta = GetBlockHeaderSize() - Storage::DEFAULT_BLOCK_HEADER_SIZE;

	if (options.encryption_options.encryption_enabled && !skip_block_header) {
		EncryptionEngine::DecryptBlock(db, options.encryption_options.derived_key_id, block.InternalBuffer(),
		                               block.Size(), delta);
	}

	CheckChecksum(block, location, delta, skip_block_header);
}

void SingleFileBlockManager::Read(QueryContext context, Block &block) {
	D_ASSERT(block.id >= 0);
	D_ASSERT(!BlockIsFreeListed(block.id));
	ReadAndChecksum(context, block, GetBlockLocation(block.id));
}

bool SingleFileBlockManager::BlockIsFreeListed(block_id_t block_id) {
	lock_guard<mutex> lock(single_file_block_lock);
	return free_list.find(block_id) != free_list.end();
}

void SingleFileBlockManager::ReadBlocks(QueryContext context, FileBuffer &buffer, block_id_t start_block,
                                        idx_t block_count) {
	D_ASSERT(start_block >= 0);
	D_ASSERT(block_count >= 1);

	// read the buffer from disk
	auto location = GetBlockLocation(start_block);
	handle->Read(context, buffer, location);

	// for each of the blocks - verify the checksum
	auto ptr = buffer.InternalBuffer();
	for (idx_t i = 0; i < block_count; i++) {
		auto start_ptr = ptr + i * GetBlockAllocSize();
		ReadBlock(start_ptr, GetBlockSize());
	}
}

void SingleFileBlockManager::Write(FileBuffer &buffer, block_id_t block_id) {
	Write(QueryContext(), buffer, block_id);
}

void SingleFileBlockManager::Write(QueryContext context, FileBuffer &buffer, block_id_t block_id) {
	D_ASSERT(block_id >= 0);
	ChecksumAndWrite(context, buffer, BLOCK_START + NumericCast<idx_t>(block_id) * GetBlockAllocSize());
}

void SingleFileBlockManager::Truncate() {
	BlockManager::Truncate();

	lock_guard<mutex> guard(single_file_block_lock);
	idx_t blocks_to_truncate = 0;
	// reverse iterate over the free-list
	for (auto entry = free_list.rbegin(); entry != free_list.rend(); entry++) {
		auto block_id = *entry;
		if (block_id + 1 != max_block) {
			break;
		}
		blocks_to_truncate++;
		max_block--;
	}
	if (blocks_to_truncate > 0) {
		// truncate the file
		free_list.erase(free_list.lower_bound(max_block), free_list.end());
	}
	auto new_size = NumericCast<idx_t>(BLOCK_START + NumericCast<idx_t>(max_block) * GetBlockAllocSize());
	if (blocks_to_truncate > 0 || handle->GetFileSize() > new_size) {
		handle->Truncate(new_size);
	}
}

vector<MetadataHandle> SingleFileBlockManager::GetFreeListBlocks() {
	vector<MetadataHandle> free_list_blocks;
	auto &metadata_manager = GetMetadataManager();

	// reserve all blocks that we are going to write the free list to
	// since these blocks are no longer free we cannot just include them in the free list!
	auto block_size = metadata_manager.GetMetadataBlockSize() - sizeof(idx_t);
	idx_t allocated_size = 0;
	while (true) {
		idx_t free_list_count;
		idx_t multi_use_blocks_count;
		{
			lock_guard<mutex> guard(single_file_block_lock);
			free_list_count =
			    free_list.size() + modified_blocks.size() + free_blocks_in_use.size() + newly_used_blocks.size();
			multi_use_blocks_count = multi_use_blocks.size();
		}
		auto free_list_size = sizeof(uint64_t) + sizeof(block_id_t) * free_list_count;
		auto multi_use_blocks_size =
		    sizeof(uint64_t) + (sizeof(block_id_t) + sizeof(uint32_t)) * multi_use_blocks_count;
		auto metadata_blocks =
		    sizeof(uint64_t) + (sizeof(block_id_t) + sizeof(idx_t)) * GetMetadataManager().BlockCount();
		auto total_size = free_list_size + multi_use_blocks_size + metadata_blocks;
		if (total_size < allocated_size) {
			break;
		}
		auto free_list_handle = GetMetadataManager().AllocateHandle();
		free_list_blocks.push_back(std::move(free_list_handle));
		allocated_size += block_size;
	}

	return free_list_blocks;
}

class FreeListBlockWriter : public MetadataWriter {
public:
	FreeListBlockWriter(MetadataManager &manager, vector<MetadataHandle> free_list_blocks_p)
	    : MetadataWriter(manager), free_list_blocks(std::move(free_list_blocks_p)), index(0) {
	}

	vector<MetadataHandle> free_list_blocks;
	idx_t index;

protected:
	MetadataHandle NextHandle() override {
		if (index >= free_list_blocks.size()) {
			throw InternalException("Free List Block Writer ran out of blocks, this means not enough blocks were "
			                        "allocated up front (%d total allocated)",
			                        free_list_blocks.size());
		}
		return std::move(free_list_blocks[index++]);
	}
};

bool SingleFileBlockManager::AddFreeBlock(unique_lock<mutex> &lock, block_id_t block_id) {
	if (!lock.owns_lock()) {
		throw InternalException("AddFreeBlock must be called while holding the lock");
	}
	shared_ptr<BlockHandle> block = TryGetBlock(block_id);
	if (!block) {
		// the block does not exist
		// regular free block
		free_list.insert(block_id);
		return true;
	}
	// the block exists - add to blocks in use
	free_blocks_in_use.insert(block_id);

	// release the lock while destroying the block since the block destructor can call UnregisterBlock
	lock.unlock();
	block.reset();
	lock.lock();
	return false;
}
void SingleFileBlockManager::WriteHeader(QueryContext context, DatabaseHeader header) {
	auto free_list_blocks = GetFreeListBlocks();

	// now handle the free list
	auto &metadata_manager = GetMetadataManager();
	// add all modified blocks to the free list: they can now be written to again
	metadata_manager.MarkBlocksAsModified();

	unique_lock<mutex> lock(single_file_block_lock);
	// set the iteration count
	header.iteration = ++iteration_count;

	set<block_id_t> all_free_blocks = free_list;
	all_free_blocks.insert(free_blocks_in_use.begin(), free_blocks_in_use.end());
	auto checkpoint_freed_blocks = modified_blocks;
	for (auto &block : checkpoint_freed_blocks) {
		all_free_blocks.insert(block);
	}
	auto written_multi_use_blocks = multi_use_blocks;
	// newly used blocks are still free blocks for this checkpoint - so add them to the free list that we write
	for (auto &newly_used_block : newly_used_blocks) {
		all_free_blocks.insert(newly_used_block);
		written_multi_use_blocks.erase(newly_used_block);
	}

	if (!free_list_blocks.empty()) {
		// there are blocks to write, either in the free_list or in the modified_blocks
		// we write these blocks specifically to the free_list_blocks
		// a normal MetadataWriter will fetch blocks to use from the free_list
		// but since we are WRITING the free_list, this behavior is sub-optimal
		FreeListBlockWriter writer(metadata_manager, std::move(free_list_blocks));

		auto ptr = writer.GetMetaBlockPointer();
		header.free_list = ptr.block_pointer;

		writer.Write<uint64_t>(all_free_blocks.size());
		for (auto &block_id : all_free_blocks) {
			writer.Write<block_id_t>(block_id);
		}
		writer.Write<uint64_t>(written_multi_use_blocks.size());
		for (auto &entry : written_multi_use_blocks) {
			writer.Write<block_id_t>(entry.first);
			writer.Write<uint32_t>(entry.second);
		}
		GetMetadataManager().Write(writer);
		writer.Flush();
	} else {
		// no blocks in the free list
		header.free_list = DConstants::INVALID_INDEX;
	}
	header.block_count = NumericCast<idx_t>(max_block);
	lock.unlock();
	metadata_manager.Flush(context);

	header.storage_compatibility = options.storage_version;

	auto debug_checkpoint_abort = Settings::Get<DebugCheckpointAbortSetting>(db.GetDatabase());
	if (debug_checkpoint_abort == CheckpointAbort::DEBUG_ABORT_AFTER_FREE_LIST_WRITE) {
		throw FatalException("Checkpoint aborted after free list write because of PRAGMA checkpoint_abort flag");
	}

	// We need to fsync BEFORE we write the header to ensure that all the previous blocks are written as well
	handle->Sync();

	header_buffer.Clear();
	// if we are upgrading the database from version 64 -> version 65, we need to re-write the main header
	if (options.version_number == StorageVersion::V0_10_2 && options.storage_version >= StorageVersion::V1_2_0) {
		RewriteMainHeader(context, StorageVersion::V1_2_0);
	}

	// if we are downgrading the database from v2.0.0 to an earlier version
	// we also need to re-write the main header
	if (options.version_number == MainHeader::DEPRECATED_VERSION_NUMBER &&
	    options.storage_version < StorageVersion::V2_0_0) {
		RewriteMainHeader(context, options.storage_version);
	}

	// set the header inside the buffer
	MemoryStream serializer(Allocator::Get(db));
	header.Write(serializer);
	memcpy(header_buffer.GetDataMutable(), serializer.GetData(), serializer.GetPosition());
	// now write the header to the file, active_header determines whether we write to h1 or h2
	// note that if active_header is h1 we write to h2, and vice versa
	auto location = active_header == 1 ? Storage::FILE_HEADER_SIZE : Storage::FILE_HEADER_SIZE * 2;
	ChecksumAndWrite(context, header_buffer, location);
	// switch active header to the other header
	active_header = 1 - active_header;
	//! Ensure the header write ends up on disk
	handle->Sync();
	set<block_id_t> fully_freed_blocks;
	{
		unique_lock<mutex> release_lock(single_file_block_lock);
		for (auto &block : checkpoint_freed_blocks) {
			modified_blocks.erase(block);
			if (AddFreeBlock(release_lock, block)) {
				fully_freed_blocks.insert(block);
			}
		}
	}
	// Release the free fully freed blocks to the filesystem.
	TrimFreeBlocks(fully_freed_blocks);
}

void SingleFileBlockManager::FileSync() {
	handle->Sync();
}

void SingleFileBlockManager::ReleaseFreeBlockInUse(unique_lock<mutex> &lock, block_id_t id) {
	if (!lock.owns_lock()) {
		throw InternalException("ReleaseFreeBlockInUse must be called while holding the lock");
	}
	// check if the block is part of the newly free list
	auto entry = free_blocks_in_use.find(id);
	if (entry != free_blocks_in_use.end()) {
		// it is! move it to the regular free list so the block can be re-used
		free_list.insert(id);
		free_blocks_in_use.erase(entry);
	}
}

void SingleFileBlockManager::UnregisterBlock(block_id_t id) {
	unique_lock<mutex> lock(single_file_block_lock);
	// perform the actual unregistration
	BlockManager::UnregisterBlock(id);
	ReleaseFreeBlockInUse(lock, id);
}

bool SingleFileBlockManager::UnregisterExpiredBlock(block_id_t id) {
	// hold the lock across the live-handle check and the free-list transition: otherwise a
	// re-registration and retirement of the block id can interleave between them, and the
	// free-list transition would run although a live handle exists
	// lock order: single_file_block_lock before blocks_lock, as in AddFreeBlock
	unique_lock<mutex> lock(single_file_block_lock);
	if (!BlockManager::UnregisterExpiredBlock(id)) {
		// a newer handle is registered for this block id - the block id is still in use
		return false;
	}
	ReleaseFreeBlockInUse(lock, id);
	return true;
}

void SingleFileBlockManager::TrimFreeBlockRange(block_id_t start, block_id_t end) {
	auto block_count = NumericCast<idx_t>(end + 1 - start);
	auto offset = BLOCK_START + (NumericCast<idx_t>(start) * GetBlockAllocSize());
	auto length = block_count * GetBlockAllocSize();
	handle->Trim(offset, length);
}

void SingleFileBlockManager::TrimFreeBlocks(const set<block_id_t> &blocks) {
	if (!DBConfig::Get(db).options.trim_free_blocks) {
		return;
	}
	lock_guard<mutex> lock(single_file_block_lock);
	for (auto itr = blocks.begin(); itr != blocks.end(); ++itr) {
		if (!free_list.count(*itr)) {
			continue;
		}
		block_id_t first = *itr;
		block_id_t last = first;
		// Find end of contiguous range.
		for (++itr; itr != blocks.end() && (*itr == last + 1) && free_list.count(*itr); ++itr) {
			last = *itr;
		}
		// We are now one too far.
		--itr;
		// Trim the range.
		TrimFreeBlockRange(first, last);
	}
}

} // namespace duckdb
