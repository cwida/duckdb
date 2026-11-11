//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_crypto.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>
#include <string>
#include <unordered_map>

#include "parquet_types.h"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/encryption_functions.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unique_ptr.hpp"

namespace duckdb_apache {
namespace thrift {
class TBase;

namespace protocol {
class TProtocol;
} // namespace protocol
} // namespace thrift
} // namespace duckdb_apache

namespace duckdb {
class ParquetAdditionalAuthenticatedData;
class ClientContext;
class Deserializer;
class EncryptionUtil;
class Serializer;
struct FunctionParameters;

using duckdb_apache::thrift::TBase;
using duckdb_apache::thrift::protocol::TProtocol;
using duckdb_parquet::ColumnChunk;
using duckdb_parquet::PageType;
class Allocator;
class BufferedFileWriter;

class ParquetKeys : public ObjectCacheEntry {
public:
	static ParquetKeys &Get(ClientContext &context);

public:
	void AddKey(const string &key_name, const string &key);
	bool HasKey(const string &key_name) const;
	string GetKey(const string &key_name) const;

public:
	static string ObjectType();
	string GetObjectType() override;
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx {};
	}

private:
	mutable mutex lock;
	unordered_map<string, string> keys;
};

struct CryptoMetaData {
	CryptoMetaData(Allocator &allocator);
	void Initialize(const std::string &unique_file_identifier_p, int16_t row_group_ordinal = -1,
	                int16_t column_ordinal = -1, int8_t module = -1, int16_t page_ordinal = -1);
	void ClearAdditionalAuthenticatedData();
	void SetModule(int8_t module_p);
	bool IsEmpty() const;

public:
	string unique_file_identifier = "";
	int8_t module;
	int16_t row_group_ordinal;
	int16_t column_ordinal;
	int16_t page_ordinal;

public:
	unique_ptr<ParquetAdditionalAuthenticatedData> additional_authenticated_data;
};

class ParquetAdditionalAuthenticatedData : public AdditionalAuthenticatedData {
public:
	explicit ParquetAdditionalAuthenticatedData(Allocator &allocator);
	~ParquetAdditionalAuthenticatedData() override;

public:
	idx_t GetPrefixSize() const;
	void Rewind() const;
	void WriteParquetAAD(const CryptoMetaData &crypto_meta_data);

private:
	void WritePrefix(const std::string &prefix);
	void WriteSuffix(const CryptoMetaData &crypto_meta_data);

private:
	optional_idx additional_authenticated_data_prefix_size;
};

class ParquetEncryptionConfig {
public:
	explicit ParquetEncryptionConfig();
	ParquetEncryptionConfig(ClientContext &context, const Value &arg);
	ParquetEncryptionConfig(string footer_key);

public:
	static shared_ptr<ParquetEncryptionConfig> Create(ClientContext &context, const Value &arg);
	const string &GetFooterKey() const;

public:
	void Serialize(Serializer &serializer) const;
	static shared_ptr<ParquetEncryptionConfig> Deserialize(Deserializer &deserializer);

private:
	//! The encryption key used for the footer
	string footer_key;
	//! Mapping from column name to key name
	unordered_map<string, string> column_keys;
};

class ParquetCrypto {
public:
	//! Encrypted modules
	static constexpr idx_t LENGTH_BYTES = 4;
	static constexpr idx_t NONCE_BYTES = 12;
	static constexpr idx_t TAG_BYTES = 16;

	//! Block size we encrypt/decrypt
	static constexpr idx_t CRYPTO_BLOCK_SIZE = 4096;
	static constexpr idx_t BLOCK_SIZE = 16;

	// Module types for encryption
	static constexpr int8_t FOOTER = 0;
	static constexpr int8_t COLUMN_METADATA = 1;
	static constexpr int8_t DATA_PAGE = 2;
	static constexpr int8_t DICTIONARY_PAGE = 3;
	static constexpr int8_t DATA_PAGE_HEADER = 4;
	static constexpr int8_t DICTIONARY_PAGE_HEADER = 5;
	static constexpr int8_t COLUMN_INDEX = 6;
	static constexpr int8_t OFFSET_INDEX = 7;
	static constexpr int8_t BLOOM_FILTER_HEADER = 8;
	static constexpr int8_t BLOOM_FILTER_BITSET = 9;

	// Standard AAD length for file
	static constexpr int32_t UNIQUE_FILE_ID_LEN = 8;
	// Maximum Parquet AAD suffix bytes
	static constexpr int32_t AAD_MAX_SUFFIX_BYTES = 7;

public:
	//! Decrypt and read a Thrift object from the transport protocol
	static uint32_t Read(TBase &object, TProtocol &iprot, const string &key, const EncryptionUtil &encryption_util_p,
	                     const CryptoMetaData &crypto_meta_data);
	//! Encrypt and write a Thrift object to the transport protocol
	static uint32_t Write(const TBase &object, TProtocol &oprot, const string &key,
	                      const EncryptionUtil &encryption_util_p);
	//! Decrypt and read a buffer
	static uint32_t ReadData(TProtocol &iprot, const data_ptr_t buffer, const uint32_t buffer_size, const string &key,
	                         const EncryptionUtil &encryption_util_p, const CryptoMetaData &crypto_meta_data);
	//! Encrypt and write a buffer to a file
	static uint32_t WriteData(TProtocol &oprot, const const_data_ptr_t buffer, const uint32_t buffer_size,
	                          const string &key, const EncryptionUtil &encryption_util_p);

public:
	static void AddKey(ClientContext &context, const FunctionParameters &parameters);
	static bool ValidKey(const std::string &key);

public:
	static int8_t GetModuleHeader(const ColumnChunk &chunk, uint16_t page_ordinal);
	static int8_t GetModule(const ColumnChunk &chunk, PageType::type page_type, uint16_t page_ordinal);
	static int16_t GetFinalPageOrdinal(const ColumnChunk &chunk, uint8_t module, uint16_t page_ordinal);
	static void GenerateAdditionalAuthenticatedData(Allocator &allocator, CryptoMetaData &aad_crypto_metadata);
	static unique_ptr<ParquetAdditionalAuthenticatedData> GenerateFooterAAD(Allocator &allocator,
	                                                                        const std::string &unique_file_identifier);
};

} // namespace duckdb
