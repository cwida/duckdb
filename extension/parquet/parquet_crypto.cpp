#include "parquet_crypto.hpp"

#include <string.h>
#include <memory>
#include <utility>

#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/storage/arena_allocator.hpp"
#include "duckdb/common/encryption_functions.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/encryption_state.hpp"
#include "duckdb/common/encryption_types.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/original/std/memory.hpp"
#include "thrift/TBase.h"
#include "thrift/protocol/TCompactProtocol.h"
#include "thrift/protocol/TProtocol.h"
#include "thrift/transport/TTransport.h"

namespace duckdb {
class ClientContext;
} // namespace duckdb

using duckdb_parquet::ColumnChunk;

namespace duckdb {

ParquetKeys &ParquetKeys::Get(ClientContext &context) {
	auto &cache = ObjectCache::GetObjectCache(context);
	return *cache.GetOrCreate<ParquetKeys>(ParquetKeys::ObjectType());
}

void ParquetKeys::AddKey(const string &key_name, const string &key) {
	lock_guard<mutex> guard(lock);
	keys[key_name] = key;
}

bool ParquetKeys::HasKey(const string &key_name) const {
	lock_guard<mutex> guard(lock);
	return keys.find(key_name) != keys.end();
}

string ParquetKeys::GetKey(const string &key_name) const {
	lock_guard<mutex> guard(lock);
	D_ASSERT(keys.find(key_name) != keys.end());
	return keys.at(key_name);
}

string ParquetKeys::ObjectType() {
	return "parquet_keys";
}

string ParquetKeys::GetObjectType() {
	return ObjectType();
}

ParquetAdditionalAuthenticatedData::ParquetAdditionalAuthenticatedData(Allocator &allocator)
    : AdditionalAuthenticatedData(allocator) {
}

ParquetAdditionalAuthenticatedData::~ParquetAdditionalAuthenticatedData() = default;

idx_t ParquetAdditionalAuthenticatedData::GetPrefixSize() const {
	if (!additional_authenticated_data_prefix_size.IsValid()) {
		return 0;
	}
	return additional_authenticated_data_prefix_size.GetIndex();
}

void ParquetAdditionalAuthenticatedData::Rewind() const {
	additional_authenticated_data->SetPosition(GetPrefixSize());
}

void ParquetAdditionalAuthenticatedData::WriteParquetAAD(const CryptoMetaData &crypto_meta_data) {
	// For the parquet encryption spec, additional authenticated data (AAD) consists of:
	// (1) a unique prefix constructed by:
	// an optional aad-prefix (arbitrary length -- ignored for now)
	// + a unique file identifier (default 8 bytes)
	if (GetPrefixSize() == 0) {
		WritePrefix(crypto_meta_data.unique_file_identifier);
	}
	// (2) a suffix, which length varies according to the module type, consisting of:
	// + module type (1 byte)
	// + row group ordinal (2 bytes, optional)
	// + column ordinal (2 bytes, optional)
	// + page ordinal (2 bytes, optional)
	WriteSuffix(crypto_meta_data);
}

void ParquetAdditionalAuthenticatedData::WritePrefix(const std::string &prefix) {
	if (prefix.empty()) {
		throw InvalidInputException("Prefix for Additional Authenticated Data is empty");
	}
	WriteStringData(prefix);
	additional_authenticated_data_prefix_size = additional_authenticated_data->GetPosition();
}

void ParquetAdditionalAuthenticatedData::WriteSuffix(const CryptoMetaData &crypto_meta_data) {
	if (!additional_authenticated_data_prefix_size.IsValid()) {
		throw InvalidInputException("Prefix for Parquet additional authenticated data is not set");
	}

	if (crypto_meta_data.module < 0) {
		throw InvalidInputException("Parquet Crypto Module not initialized");
	}
	WriteData<int8_t>(crypto_meta_data.module);

	if (crypto_meta_data.row_group_ordinal < 0) {
		if (crypto_meta_data.module != ParquetCrypto::FOOTER) {
			throw InvalidInputException("Parquet Encryption: Row group not initialized");
		}
		// Footer
		return;
	}
	WriteData<int16_t>(crypto_meta_data.row_group_ordinal);

	if (crypto_meta_data.column_ordinal < 0) {
		return;
	}
	WriteData<int16_t>(crypto_meta_data.column_ordinal);

	if (crypto_meta_data.page_ordinal < 0) {
		return;
	}
	WriteData<int16_t>(crypto_meta_data.page_ordinal);
}

ParquetEncryptionConfig::ParquetEncryptionConfig() {
}

ParquetEncryptionConfig::ParquetEncryptionConfig(string footer_key_p) : footer_key(std::move(footer_key_p)) {
}

ParquetEncryptionConfig::ParquetEncryptionConfig(ClientContext &context, const Value &arg) {
	if (arg.type().id() != LogicalTypeId::STRUCT) {
		throw BinderException("Parquet encryption_config must be of type STRUCT");
	}
	const auto &child_types = StructType::GetChildTypes(arg.type());
	auto &children = StructValue::GetChildren(arg);
	const auto &keys = ParquetKeys::Get(context);
	for (idx_t i = 0; i < StructType::GetChildCount(arg.type()); i++) {
		auto &struct_key = child_types[i].first;
		if ((struct_key == "footer_key" || struct_key == "footer_key_value") && children[i].IsNull()) {
			throw BinderException("Parquet encryption_config %s cannot be NULL", struct_key);
		}
		if (struct_key == "footer_key") {
			const auto footer_key_name = StringValue::Get(children[i].DefaultCastAs(LogicalType::VARCHAR));
			if (!keys.HasKey(footer_key_name)) {
				throw BinderException(
				    "No key with name \"%s\" exists. Add it with PRAGMA add_parquet_key('<key_name>','<key>');",
				    footer_key_name);
			}
			// footer key name provided - read the key from the config
			const auto &keys = ParquetKeys::Get(context);
			footer_key = keys.GetKey(footer_key_name);
		} else if (struct_key == "footer_key_value") {
			footer_key = StringValue::Get(children[i].DefaultCastAs(LogicalType::BLOB));
		} else if (struct_key == "column_keys") {
			throw NotImplementedException("Parquet encryption_config column_keys not yet implemented");
		} else {
			throw BinderException("Unknown key in encryption_config %s", struct_key);
		}
	}
}

shared_ptr<ParquetEncryptionConfig> ParquetEncryptionConfig::Create(ClientContext &context, const Value &arg) {
	return shared_ptr<ParquetEncryptionConfig>(new ParquetEncryptionConfig(context, arg));
}

const string &ParquetEncryptionConfig::GetFooterKey() const {
	return footer_key;
}

using duckdb_apache::thrift::protocol::TCompactProtocolFactoryT;
using duckdb_apache::thrift::transport::TTransport;

//! Encryption wrapper for a transport protocol
class EncryptionTransport : public TTransport {
public:
	EncryptionTransport(TProtocol &prot_p, const string &key, const EncryptionUtil &encryption_util_p)
	    : prot(prot_p), trans(*prot.getTransport()),
	      allocator(Allocator::DefaultAllocator(), ParquetCrypto::CRYPTO_BLOCK_SIZE) {
		auto metadata = make_uniq<EncryptionStateMetadata>(EncryptionTypes::GCM, key.size(),
		                                                   EncryptionTypes::EncryptionVersion::NONE);
		aes = encryption_util_p.CreateEncryptionState(std::move(metadata));

		Initialize(key);
	}

	bool isOpen() const override {
		return trans.isOpen();
	}

	void open() override {
		trans.open();
	}

	void close() override {
		trans.close();
	}

	void write_virt(const uint8_t *buf, uint32_t len) override {
		memcpy(allocator.Allocate(len), buf, len);
	}

	uint32_t Finalize() {
		// Write length
		const auto ciphertext_length = allocator.SizeInBytes();
		const uint32_t total_length = ParquetCrypto::NONCE_BYTES + ciphertext_length + ParquetCrypto::TAG_BYTES;

		trans.write(const_data_ptr_cast(&total_length), ParquetCrypto::LENGTH_BYTES);
		// Write nonce at the start of encrypted chunk
		trans.write(nonce.data(), ParquetCrypto::NONCE_BYTES);

		data_t aes_buffer[ParquetCrypto::CRYPTO_BLOCK_SIZE];
		auto current = allocator.GetTail();

		// Loop through the whole chunk
		while (current != nullptr) {
			for (idx_t pos = 0; pos < current->current_position; pos += ParquetCrypto::CRYPTO_BLOCK_SIZE) {
				auto next = MinValue<idx_t>(current->current_position - pos, ParquetCrypto::CRYPTO_BLOCK_SIZE);
				auto write_size =
				    aes->Process(current->data.get() + pos, next, aes_buffer, ParquetCrypto::CRYPTO_BLOCK_SIZE);
				trans.write(aes_buffer, write_size);
			}
			current = current->prev;
		}

		// Finalize the last encrypted data
		data_t tag[ParquetCrypto::TAG_BYTES];
		auto write_size = aes->Finalize(aes_buffer, 0, tag, ParquetCrypto::TAG_BYTES);
		trans.write(aes_buffer, write_size);
		// Write tag for verification
		trans.write(tag, ParquetCrypto::TAG_BYTES);

		return ParquetCrypto::LENGTH_BYTES + total_length;
	}

private:
	void Initialize(const string &key) {
		// Generate Nonce
		aes->GenerateRandomData(nonce.data(), nonce.size());
		// Initialize Encryption
		aes->InitializeEncryption(nonce, reinterpret_cast<const_data_ptr_t>(key.data()));
	}

private:
	//! Protocol and corresponding transport that we're wrapping
	TProtocol &prot;
	TTransport &trans;

	//! AES context and buffers
	shared_ptr<EncryptionState> aes;

	//! Nonce created by Initialize()
	EncryptionNonce nonce;

	//! Arena Allocator to fully materialize in memory before encrypting
	ArenaAllocator allocator;
};

//! Decryption wrapper for a transport protocol
class DecryptionTransport : public TTransport {
public:
	DecryptionTransport(TProtocol &prot_p, const string &key, const EncryptionUtil &encryption_util_p,
	                    const CryptoMetaData &crypto_meta_data)
	    : prot(prot_p), trans(*prot.getTransport()), read_buffer_size(0), read_buffer_offset(0) {
		auto metadata = make_uniq<EncryptionStateMetadata>(EncryptionTypes::GCM, key.size(),
		                                                   EncryptionTypes::EncryptionVersion::NONE);
		aes = encryption_util_p.CreateEncryptionState(std::move(metadata));
		Initialize(key, crypto_meta_data);
	}

	uint32_t read_virt(uint8_t *buf, uint32_t len) override {
		const uint32_t result = len;

		if (len > transport_remaining - ParquetCrypto::TAG_BYTES + read_buffer_size - read_buffer_offset) {
			throw InvalidInputException("Too many bytes requested from crypto buffer");
		}

		while (len != 0) {
			if (read_buffer_offset == read_buffer_size) {
				ReadBlock(buf);
			}
			const auto next = MinValue(read_buffer_size - read_buffer_offset, len);
			read_buffer_offset += next;
			buf += next;
			len -= next;
		}

		return result;
	}

	uint32_t Finalize() {
		if (read_buffer_offset != read_buffer_size) {
			throw InternalException("DecryptionTransport::Finalize was called with bytes remaining in read buffer: \n"
			                        "read buffer offset: %d, read buffer size: %d",
			                        read_buffer_offset, read_buffer_size);
		}

		data_t computed_tag[ParquetCrypto::TAG_BYTES];
		transport_remaining -= trans.read(computed_tag, ParquetCrypto::TAG_BYTES);
		aes->Finalize(read_buffer, 0, computed_tag, ParquetCrypto::TAG_BYTES);

		if (transport_remaining != 0) {
			throw InvalidInputException("Encoded ciphertext length differs from actual ciphertext length");
		}

		return ParquetCrypto::LENGTH_BYTES + total_bytes;
	}

	AllocatedData ReadAll() {
		D_ASSERT(transport_remaining == total_bytes - ParquetCrypto::NONCE_BYTES);
		auto result = Allocator::DefaultAllocator().Allocate(transport_remaining - ParquetCrypto::TAG_BYTES);
		read_virt(result.get(), transport_remaining - ParquetCrypto::TAG_BYTES);
		Finalize();
		return result;
	}

private:
	void Initialize(const string &key, const CryptoMetaData &crypto_meta_data) {
		// Read encoded length (don't add to read_bytes)
		data_t length_buf[ParquetCrypto::LENGTH_BYTES];
		trans.read(length_buf, ParquetCrypto::LENGTH_BYTES);
		total_bytes = Load<uint32_t>(length_buf);
		if (total_bytes < ParquetCrypto::NONCE_BYTES + ParquetCrypto::TAG_BYTES) {
			throw InvalidInputException("Encoded ciphertext length %d is smaller than the nonce and tag", total_bytes);
		}
		transport_remaining = total_bytes;
		// Read nonce and initialize AES
		transport_remaining -= trans.read(nonce.data(), nonce.total_size());
		// check whether context is initialized
		if (!crypto_meta_data.IsEmpty()) {
			aes->InitializeDecryption(nonce, reinterpret_cast<const_data_ptr_t>(key.data()),
			                          crypto_meta_data.additional_authenticated_data->data(),
			                          crypto_meta_data.additional_authenticated_data->size());
			crypto_meta_data.additional_authenticated_data->Rewind();
		} else {
			aes->InitializeDecryption(nonce, reinterpret_cast<const_data_ptr_t>(key.data()));
		}
	}

	void ReadBlock(uint8_t *buf) {
		// Read from transport into read_buffer at one AES block size offset (up to the tag)
		read_buffer_size = MinValue(ParquetCrypto::CRYPTO_BLOCK_SIZE, transport_remaining - ParquetCrypto::TAG_BYTES);
		transport_remaining -= trans.read(read_buffer + ParquetCrypto::BLOCK_SIZE, read_buffer_size);

		// Decrypt from read_buffer + block size into read_buffer start (decryption can trail behind in same buffer)
#ifdef DEBUG
		auto size = aes->Process(read_buffer + ParquetCrypto::BLOCK_SIZE, read_buffer_size, buf,
		                         ParquetCrypto::CRYPTO_BLOCK_SIZE + ParquetCrypto::BLOCK_SIZE);
		D_ASSERT(size == read_buffer_size);
#else
		aes->Process(read_buffer + ParquetCrypto::BLOCK_SIZE, read_buffer_size, buf,
		             ParquetCrypto::CRYPTO_BLOCK_SIZE + ParquetCrypto::BLOCK_SIZE);
#endif
		read_buffer_offset = 0;
	}

private:
	//! Protocol and corresponding transport that we're wrapping
	TProtocol &prot;
	TTransport &trans;

	//! AES context and buffers
	shared_ptr<EncryptionState> aes;

	//! We read/decrypt big blocks at a time
	data_t read_buffer[ParquetCrypto::CRYPTO_BLOCK_SIZE + ParquetCrypto::BLOCK_SIZE];
	uint32_t read_buffer_size;
	uint32_t read_buffer_offset;

	//! Remaining bytes to read, set by Initialize(), decremented by ReadBlock()
	uint32_t total_bytes;
	uint32_t transport_remaining;
	//! Nonce read by Initialize()
	EncryptionNonce nonce;
};

class SimpleReadTransport : public TTransport {
public:
	explicit SimpleReadTransport(data_ptr_t read_buffer_p, uint32_t read_buffer_size_p)
	    : read_buffer(read_buffer_p), read_buffer_size(read_buffer_size_p), read_buffer_offset(0) {
	}

	uint32_t read_virt(uint8_t *buf, uint32_t len) override {
		const auto remaining = read_buffer_size - read_buffer_offset;
		if (len > remaining) {
			return remaining;
		}
		memcpy(buf, read_buffer + read_buffer_offset, len);
		read_buffer_offset += len;
		return len;
	}

private:
	const data_ptr_t read_buffer;
	const uint32_t read_buffer_size;
	uint32_t read_buffer_offset;
};

uint32_t ParquetCrypto::Read(TBase &object, TProtocol &iprot, const string &key,
                             const EncryptionUtil &encryption_util_p, const CryptoMetaData &crypto_meta_data) {
	TCompactProtocolFactoryT<DecryptionTransport> tproto_factory;
	auto dprot = tproto_factory.getProtocol(
	    duckdb_base_std::make_shared<DecryptionTransport>(iprot, key, encryption_util_p, crypto_meta_data));
	auto &dtrans = reinterpret_cast<DecryptionTransport &>(*dprot->getTransport());

	// We have to read the whole thing otherwise thrift throws an error before we realize we're decryption is wrong
	auto all = dtrans.ReadAll();
	TCompactProtocolFactoryT<SimpleReadTransport> tsimple_proto_factory;
	auto simple_prot =
	    tsimple_proto_factory.getProtocol(duckdb_base_std::make_shared<SimpleReadTransport>(all.get(), all.GetSize()));

	// Read the object
	object.read(simple_prot.get());

	return ParquetCrypto::LENGTH_BYTES + ParquetCrypto::NONCE_BYTES + all.GetSize() + ParquetCrypto::TAG_BYTES;
}

uint32_t ParquetCrypto::Write(const TBase &object, TProtocol &oprot, const string &key,
                              const EncryptionUtil &encryption_util_p) {
	// Create encryption protocol
	TCompactProtocolFactoryT<EncryptionTransport> tproto_factory;
	auto eprot =
	    tproto_factory.getProtocol(duckdb_base_std::make_shared<EncryptionTransport>(oprot, key, encryption_util_p));
	auto &etrans = reinterpret_cast<EncryptionTransport &>(*eprot->getTransport());

	// Write the object in memory
	object.write(eprot.get());

	// Encrypt and write to oprot
	return etrans.Finalize();
}

uint32_t ParquetCrypto::ReadData(TProtocol &iprot, const data_ptr_t buffer, const uint32_t buffer_size,
                                 const string &key, const EncryptionUtil &encryption_util_p,
                                 const CryptoMetaData &crypto_meta_data) {
	// Create decryption protocol
	TCompactProtocolFactoryT<DecryptionTransport> tproto_factory;
	auto dprot = tproto_factory.getProtocol(
	    duckdb_base_std::make_shared<DecryptionTransport>(iprot, key, encryption_util_p, crypto_meta_data));
	auto &dtrans = reinterpret_cast<DecryptionTransport &>(*dprot->getTransport());

	// Read buffer
	dtrans.read(buffer, buffer_size);

	// Verify AES tag and read length
	return dtrans.Finalize();
}

uint32_t ParquetCrypto::WriteData(TProtocol &oprot, const const_data_ptr_t buffer, const uint32_t buffer_size,
                                  const string &key, const EncryptionUtil &encryption_util_p) {
	// FIXME: we know the size upfront so we could do a streaming write instead of this
	// Create encryption protocol
	TCompactProtocolFactoryT<EncryptionTransport> tproto_factory;
	auto eprot =
	    tproto_factory.getProtocol(duckdb_base_std::make_shared<EncryptionTransport>(oprot, key, encryption_util_p));
	auto &etrans = reinterpret_cast<EncryptionTransport &>(*eprot->getTransport());

	// Write the data in memory
	etrans.write(buffer, buffer_size);

	// Encrypt and write to oprot
	return etrans.Finalize();
}

int8_t ParquetCrypto::GetModuleHeader(const ColumnChunk &chunk, uint16_t page_ordinal) {
	if (page_ordinal > 0) {
		// always return data page header if ordinal > 0
		return DATA_PAGE_HEADER;
	}
	// There is at maximum 1 dictionary, index or bf filter page header per column chunk
	if (chunk.meta_data.__isset.dictionary_page_offset) {
		return DICTIONARY_PAGE_HEADER;
	} else if (chunk.meta_data.__isset.index_page_offset) {
		return OFFSET_INDEX;
	} else if (chunk.meta_data.__isset.bloom_filter_offset) {
		return ParquetCrypto::BLOOM_FILTER_HEADER;
	}

	return DATA_PAGE_HEADER;
}

int8_t ParquetCrypto::GetModule(const ColumnChunk &chunk, PageType::type page_type, uint16_t page_ordinal) {
	if (chunk.meta_data.__isset.bloom_filter_offset && page_ordinal == 0) {
		// return bitset if it is the first page ordinal
		return ParquetCrypto::BLOOM_FILTER_BITSET;
	}

	switch (page_type) {
	case PageType::DATA_PAGE:
	case PageType::DATA_PAGE_V2:
		return DATA_PAGE;
	case PageType::DICTIONARY_PAGE:
		return DICTIONARY_PAGE;
	case PageType::INDEX_PAGE:
		if (chunk.meta_data.__isset.index_page_offset) {
			return OFFSET_INDEX;
		}
		return COLUMN_INDEX;
	default:
		throw InvalidInputException("Module not found");
	}
}

int16_t ParquetCrypto::GetFinalPageOrdinal(const ColumnChunk &chunk, uint8_t module, uint16_t page_ordinal) {
	switch (module) {
	case DATA_PAGE_HEADER:
		if (chunk.meta_data.__isset.dictionary_page_offset) {
			page_ordinal -= 1;
		} else if (chunk.meta_data.__isset.index_page_offset) {
			page_ordinal -= 1;
		} else if (chunk.meta_data.__isset.bloom_filter_offset) {
			page_ordinal -= 1;
		}
		return NumericCast<int16_t>(page_ordinal);
	case DATA_PAGE:
		return NumericCast<int16_t>(page_ordinal);
	default:
		// All modules except DataPage(Header) are -1 (absent)
		return -1;
	}
}

void ParquetCrypto::GenerateAdditionalAuthenticatedData(Allocator &allocator, CryptoMetaData &aad_crypto_metadata) {
	if (aad_crypto_metadata.IsEmpty()) {
		// no aad, old duckdb-parquet crypto implementation
		aad_crypto_metadata.ClearAdditionalAuthenticatedData();
		return;
	}
	aad_crypto_metadata.additional_authenticated_data->WriteParquetAAD(aad_crypto_metadata);
}

bool ParquetCrypto::ValidKey(const std::string &key) {
	switch (key.size()) {
	case 16:
	case 24:
	case 32:
		return true;
	default:
		return false;
	}
}

static string Base64Decode(const string &key) {
	auto result_size = Blob::FromBase64Size(key);
	auto output = duckdb::unique_ptr<unsigned char[]>(new unsigned char[result_size]);
	Blob::FromBase64(key, output.get(), result_size);
	string decoded_key(reinterpret_cast<const char *>(output.get()), result_size);
	return decoded_key;
}

void ParquetCrypto::AddKey(ClientContext &context, const FunctionParameters &parameters) {
	if (parameters.values[0].IsNull() || parameters.values[1].IsNull()) {
		throw InvalidInputException("add_parquet_key: key name and key cannot be NULL");
	}
	const auto &key_name = StringValue::Get(parameters.values[0]);
	const auto &key = StringValue::Get(parameters.values[1]);

	auto &keys = ParquetKeys::Get(context);
	if (ValidKey(key)) {
		keys.AddKey(key_name, key);
	} else {
		string decoded_key;
		try {
			decoded_key = Base64Decode(key);
		} catch (const ConversionException &e) {
			throw InvalidInputException("Invalid AES key. Not a plain AES key NOR a base64 encoded string");
		}
		if (!ValidKey(decoded_key)) {
			throw InvalidInputException(
			    "Invalid AES key. Must have a length of 128, 192, or 256 bits (16, 24, or 32 bytes)");
		}
		keys.AddKey(key_name, decoded_key);
	}
}

CryptoMetaData::CryptoMetaData(Allocator &allocator) {
	additional_authenticated_data = make_uniq<ParquetAdditionalAuthenticatedData>(allocator);
}

void CryptoMetaData::Initialize(const std::string &unique_file_identifier_p, int16_t row_group_ordinal_p,
                                int16_t column_ordinal_p, int8_t module_p, int16_t page_ordinal_p) {
	if (unique_file_identifier_p.empty()) {
		// aad not used for encryption
		// this happens with old duckdb-parquet encryption
		return;
	}
	unique_file_identifier = unique_file_identifier_p;
	module = module_p;
	row_group_ordinal = row_group_ordinal_p;
	column_ordinal = column_ordinal_p;
	page_ordinal = page_ordinal_p;
}

void CryptoMetaData::SetModule(int8_t module_p) {
	module = module_p;
}

void CryptoMetaData::ClearAdditionalAuthenticatedData() {
	additional_authenticated_data = nullptr;
}

bool CryptoMetaData::IsEmpty() const {
	return unique_file_identifier.empty();
}

} // namespace duckdb
