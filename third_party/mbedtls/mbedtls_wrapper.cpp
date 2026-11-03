#include "mbedtls_wrapper.hpp"

// otherwise we have different definitions for mbedtls_pk_context / mbedtls_sha256_context
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include "duckdb/common/crypto/md5.hpp"
#include "duckdb/common/helper.hpp"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/cipher.h"

#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/common/encryption_types.hpp"

#include <stdexcept>

using namespace duckdb_mbedtls;
using CipherType = duckdb::EncryptionTypes::CipherType;
using EncryptionVersion = duckdb::EncryptionTypes::EncryptionVersion;
using MainHeader = duckdb::MainHeader;

/*
# Command line tricks to help here
# Create a new key
openssl genrsa -out private.pem 2048

# Export public key
openssl rsa -in private.pem -outform PEM -pubout -out public.pem

# Calculate digest and write to 'hash' file on command line
openssl dgst -binary -sha256 dummy > hash

# Calculate signature from hash
openssl pkeyutl -sign -in hash -inkey private.pem -pkeyopt digest:sha256 -out dummy.sign
*/

void MbedTlsWrapper::ComputeSha256Hash(const char *in, size_t in_len, char *out) {

	mbedtls_sha256_context sha_context;
	mbedtls_sha256_init(&sha_context);
	if (mbedtls_sha256_starts(&sha_context, false) ||
	    mbedtls_sha256_update(&sha_context, reinterpret_cast<const unsigned char *>(in), in_len) ||
	    mbedtls_sha256_finish(&sha_context, reinterpret_cast<unsigned char *>(out))) {
		throw std::runtime_error("SHA256 Error");
	}
	mbedtls_sha256_free(&sha_context);
}

std::string MbedTlsWrapper::ComputeSha256Hash(const std::string &file_content) {
	std::string hash;
	hash.resize(MbedTlsWrapper::SHA256_HASH_LENGTH_BYTES);
	ComputeSha256Hash(file_content.data(), file_content.size(), (char *)hash.data());
	return hash;
}

static bool IsSupportedRSAKeySize(size_t bitlen) {
	return bitlen == 2048 || bitlen == 3072 || bitlen == 4096;
}

bool MbedTlsWrapper::IsValidRSAPublicKey(const std::string &pubkey) {
	mbedtls_pk_context pk_context;
	mbedtls_pk_init(&pk_context);

	bool valid = mbedtls_pk_parse_public_key(&pk_context, reinterpret_cast<const unsigned char *>(pubkey.c_str()),
	                                         pubkey.size() + 1) == 0;
	valid = valid && mbedtls_pk_can_do(&pk_context, MBEDTLS_PK_RSA) &&
	        IsSupportedRSAKeySize(mbedtls_pk_get_bitlen(&pk_context));

	mbedtls_pk_free(&pk_context);
	return valid;
}

bool MbedTlsWrapper::IsValidSha256Signature(const std::string &pubkey, const std::string &signature,
                                            const std::string &sha256_hash) {
	if (sha256_hash.size() != 32) {
		throw std::runtime_error("Invalid input lengths, expected hash length 32, got " +
		                         std::to_string(sha256_hash.size()));
	}

	mbedtls_pk_context pk_context;
	mbedtls_pk_init(&pk_context);

	if (mbedtls_pk_parse_public_key(&pk_context, reinterpret_cast<const unsigned char *>(pubkey.c_str()),
	                                pubkey.size() + 1)) {
		mbedtls_pk_free(&pk_context);
		throw std::runtime_error("RSA public key import error");
	}

	// the signature must be exactly as long as the modulus of a supported RSA key
	size_t bitlen = mbedtls_pk_get_bitlen(&pk_context);
	if (!mbedtls_pk_can_do(&pk_context, MBEDTLS_PK_RSA) || !IsSupportedRSAKeySize(bitlen) ||
	    signature.size() != bitlen / 8) {
		mbedtls_pk_free(&pk_context);
		return false;
	}

	// actually verify
	bool valid = mbedtls_pk_verify(&pk_context, MBEDTLS_MD_SHA256,
	                               reinterpret_cast<const unsigned char *>(sha256_hash.data()), sha256_hash.size(),
	                               reinterpret_cast<const unsigned char *>(signature.data()), signature.length()) == 0;

	mbedtls_pk_free(&pk_context);
	return valid;
}

// used in s3fs
void MbedTlsWrapper::Hmac256(const char *key, size_t key_len, const char *message, size_t message_len, char *out) {
	mbedtls_md_context_t hmac_ctx;
	const mbedtls_md_info_t *md_type = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	if (!md_type) {
		throw std::runtime_error("failed to init hmac");
	}

	if (mbedtls_md_setup(&hmac_ctx, md_type, 1) ||
	    mbedtls_md_hmac_starts(&hmac_ctx, reinterpret_cast<const unsigned char *>(key), key_len) ||
	    mbedtls_md_hmac_update(&hmac_ctx, reinterpret_cast<const unsigned char *>(message), message_len) ||
	    mbedtls_md_hmac_finish(&hmac_ctx, reinterpret_cast<unsigned char *>(out))) {
		throw std::runtime_error("HMAC256 Error");
	}
	mbedtls_md_free(&hmac_ctx);
}

void MbedTlsWrapper::ToBase16(char *in, char *out, size_t len) {
	static char const HEX_CODES[] = "0123456789abcdef";
	size_t i, j;

	for (j = i = 0; i < len; i++) {
		int a = in[i];
		out[j++] = HEX_CODES[(a >> 4) & 0xf];
		out[j++] = HEX_CODES[a & 0xf];
	}
}

class MbedTLSCryptoHashState : public duckdb::CryptoHashState {
public:
	explicit MbedTLSCryptoHashState(duckdb::CryptoHashFunction function) : duckdb::CryptoHashState(function) {
		switch (function) {
		case duckdb::CryptoHashFunction::MD5:
			break;
		case duckdb::CryptoHashFunction::SHA1:
			mbedtls_sha1_init(&sha1_context);
			break;
		case duckdb::CryptoHashFunction::SHA256:
			mbedtls_sha256_init(&sha256_context);
			break;
		default:
			throw duckdb::InternalException("Unsupported crypto hash function");
		}
	}

	~MbedTLSCryptoHashState() override {
		switch (GetFunction()) {
		case duckdb::CryptoHashFunction::MD5:
			break;
		case duckdb::CryptoHashFunction::SHA1:
			mbedtls_sha1_free(&sha1_context);
			break;
		case duckdb::CryptoHashFunction::SHA256:
			mbedtls_sha256_free(&sha256_context);
			break;
		default:
			break;
		}
	}

	void Hash(duckdb::const_data_ptr_t input, duckdb::idx_t input_len, duckdb::data_ptr_t output) override {
		switch (GetFunction()) {
		case duckdb::CryptoHashFunction::MD5: {
			duckdb::MD5Context context;
			context.Add(input, input_len);
			context.Finish(output);
			return;
		}
		case duckdb::CryptoHashFunction::SHA1:
			if (mbedtls_sha1_starts(&sha1_context) || mbedtls_sha1_update(&sha1_context, input, input_len) ||
			    mbedtls_sha1_finish(&sha1_context, output)) {
				throw std::runtime_error("SHA1 Error");
			}
			return;
		case duckdb::CryptoHashFunction::SHA256:
			if (mbedtls_sha256_starts(&sha256_context, false) ||
			    mbedtls_sha256_update(&sha256_context, input, input_len) ||
			    mbedtls_sha256_finish(&sha256_context, output)) {
				throw std::runtime_error("SHA256 Error");
			}
			return;
		default:
			throw duckdb::InternalException("Unsupported crypto hash function");
		}
	}

private:
	mbedtls_sha1_context sha1_context;
	mbedtls_sha256_context sha256_context;
};

void MbedTlsWrapper::AESStateMBEDTLSFactory::Hash(duckdb::CryptoHashFunction function, duckdb::const_data_ptr_t input,
                                                  duckdb::idx_t input_len, duckdb::data_ptr_t output) const {
	switch (function) {
	case duckdb::CryptoHashFunction::MD5: {
		duckdb::MD5Context context;
		context.Add(input, input_len);
		context.Finish(output);
		return;
	}
	case duckdb::CryptoHashFunction::SHA1:
		if (mbedtls_sha1(input, input_len, output)) {
			throw std::runtime_error("SHA1 Error");
		}
		return;
	case duckdb::CryptoHashFunction::SHA256:
		if (mbedtls_sha256(input, input_len, output, false)) {
			throw std::runtime_error("SHA256 Error");
		}
		return;
	default:
		throw duckdb::InternalException("Unsupported crypto hash function");
	}
}

duckdb::unique_ptr<duckdb::CryptoHashState>
MbedTlsWrapper::AESStateMBEDTLSFactory::CreateHashState(duckdb::CryptoHashFunction function) const {
	return duckdb::make_uniq<MbedTLSCryptoHashState>(function);
}

void MbedTlsWrapper::AESStateMBEDTLSFactory::Hmac(duckdb::CryptoHashFunction function, duckdb::const_data_ptr_t key,
                                                  duckdb::idx_t key_len, duckdb::const_data_ptr_t input,
                                                  duckdb::idx_t input_len, duckdb::data_ptr_t output) const {
	if (function != duckdb::CryptoHashFunction::SHA256) {
		throw duckdb::NotImplementedException("MbedTLS HMAC currently only supports SHA256");
	}
	MbedTlsWrapper::Hmac256(duckdb::const_char_ptr_cast(key), key_len, duckdb::const_char_ptr_cast(input), input_len,
	                        duckdb::char_ptr_cast(output));
}

bool MbedTlsWrapper::AESStateMBEDTLSFactory::SupportsHash(duckdb::CryptoHashFunction function) const {
	switch (function) {
	case duckdb::CryptoHashFunction::MD5:
	case duckdb::CryptoHashFunction::SHA1:
	case duckdb::CryptoHashFunction::SHA256:
		return true;
	default:
		return false;
	}
}

bool MbedTlsWrapper::AESStateMBEDTLSFactory::SupportsHmac(duckdb::CryptoHashFunction function) const {
	return function == duckdb::CryptoHashFunction::SHA256;
}

MbedTlsWrapper::SHA256State::SHA256State() : sha_context(new mbedtls_sha256_context()) {
	auto context = reinterpret_cast<mbedtls_sha256_context *>(sha_context);

	mbedtls_sha256_init(context);

	if (mbedtls_sha256_starts(context, false)) {
		throw std::runtime_error("SHA256 Error");
	}
}

MbedTlsWrapper::SHA256State::~SHA256State() {
	auto context = reinterpret_cast<mbedtls_sha256_context *>(sha_context);
	mbedtls_sha256_free(context);
	delete context;
}

void MbedTlsWrapper::SHA256State::AddString(const std::string &str) {
	auto context = reinterpret_cast<mbedtls_sha256_context *>(sha_context);
	if (mbedtls_sha256_update(context, (unsigned char *)str.data(), str.size())) {
		throw std::runtime_error("SHA256 Error");
	}
}

void MbedTlsWrapper::SHA256State::AddBytes(duckdb::const_data_ptr_t input_bytes, duckdb::idx_t len) {
	auto context = reinterpret_cast<mbedtls_sha256_context *>(sha_context);
	if (mbedtls_sha256_update(context, input_bytes, len)) {
		throw std::runtime_error("SHA256 Error");
	}
}

void MbedTlsWrapper::SHA256State::AddBytes(duckdb::data_ptr_t input_bytes, duckdb::idx_t len) {
	AddBytes(duckdb::const_data_ptr_t(input_bytes), len);
}

void MbedTlsWrapper::SHA256State::AddSalt(unsigned char *salt, size_t salt_len) {
	auto context = reinterpret_cast<mbedtls_sha256_context *>(sha_context);
	if (mbedtls_sha256_update(context, salt, salt_len)) {
		throw std::runtime_error("SHA256 Error");
	}
}

void MbedTlsWrapper::SHA256State::FinalizeDerivedKey(duckdb::data_ptr_t hash) {
	auto context = reinterpret_cast<mbedtls_sha256_context *>(sha_context);

	if (mbedtls_sha256_finish(context, (duckdb::data_ptr_t)hash)) {
		throw std::runtime_error("SHA256 Error");
	}
}

std::string MbedTlsWrapper::SHA256State::Finalize() {
	auto context = reinterpret_cast<mbedtls_sha256_context *>(sha_context);

	std::string hash;
	hash.resize(MbedTlsWrapper::SHA256_HASH_LENGTH_BYTES);

	if (mbedtls_sha256_finish(context, (unsigned char *)hash.data())) {
		throw std::runtime_error("SHA256 Error");
	}

	return hash;
}

void MbedTlsWrapper::SHA256State::FinishHex(char *out) {
	auto context = reinterpret_cast<mbedtls_sha256_context *>(sha_context);

	std::string hash;
	hash.resize(MbedTlsWrapper::SHA256_HASH_LENGTH_BYTES);

	if (mbedtls_sha256_finish(context, (unsigned char *)hash.data())) {
		throw std::runtime_error("SHA256 Error");
	}

	MbedTlsWrapper::ToBase16(const_cast<char *>(hash.c_str()), out, MbedTlsWrapper::SHA256_HASH_LENGTH_BYTES);
}

MbedTlsWrapper::SHA1State::SHA1State() : sha_context(new mbedtls_sha1_context()) {
	auto context = reinterpret_cast<mbedtls_sha1_context *>(sha_context);

	mbedtls_sha1_init(context);

	if (mbedtls_sha1_starts(context)) {
		throw std::runtime_error("SHA1 Error");
	}
}

MbedTlsWrapper::SHA1State::~SHA1State() {
	auto context = reinterpret_cast<mbedtls_sha1_context *>(sha_context);
	mbedtls_sha1_free(context);
	delete context;
}

void MbedTlsWrapper::SHA1State::AddString(const std::string &str) {
	auto context = reinterpret_cast<mbedtls_sha1_context *>(sha_context);
	if (mbedtls_sha1_update(context, (unsigned char *)str.data(), str.size())) {
		throw std::runtime_error("SHA1 Error");
	}
}

std::string MbedTlsWrapper::SHA1State::Finalize() {
	auto context = reinterpret_cast<mbedtls_sha1_context *>(sha_context);

	std::string hash;
	hash.resize(MbedTlsWrapper::SHA1_HASH_LENGTH_BYTES);

	if (mbedtls_sha1_finish(context, (unsigned char *)hash.data())) {
		throw std::runtime_error("SHA1 Error");
	}

	return hash;
}

void MbedTlsWrapper::SHA1State::FinishHex(char *out) {
	auto context = reinterpret_cast<mbedtls_sha1_context *>(sha_context);

	std::string hash;
	hash.resize(MbedTlsWrapper::SHA1_HASH_LENGTH_BYTES);

	if (mbedtls_sha1_finish(context, (unsigned char *)hash.data())) {
		throw std::runtime_error("SHA1 Error");
	}

	MbedTlsWrapper::ToBase16(const_cast<char *>(hash.c_str()), out, MbedTlsWrapper::SHA1_HASH_LENGTH_BYTES);
}

const mbedtls_cipher_info_t *MbedTlsWrapper::AESStateMBEDTLS::GetCipher(){
	switch(metadata->GetCipher()) {
		case CipherType::GCM:
		    switch (metadata->GetKeyLen()) {
		    case 16:
			    return mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_GCM);
		    case 24:
			    return mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_192_GCM);
		    case 32:
			    return mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_256_GCM);
		    default:
			    throw std::runtime_error("Invalid AES key length for GCM");
		    }
		case CipherType::CTR:
		    switch (metadata->GetKeyLen()) {
		    case 16:
			    return mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_CTR);
		    case 24:
			    return mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_192_CTR);
		    case 32:
			    return mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_256_CTR);
		    default:
			    throw std::runtime_error("Invalid AES key length for CTR");
		    }
		case CipherType::CBC:
			switch (metadata->GetKeyLen()) {
			case 16:
				return mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_CBC);
			case 24:
				return mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_192_CBC);
			case 32:
				return mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_256_CBC);
			default:
				throw std::runtime_error("Invalid AES key length for CBC");
			}
		default:
				throw duckdb::InternalException("Invalid Encryption/Decryption Cipher: %s", duckdb::EncryptionTypes::CipherToString(metadata->GetCipher()));
	}
}

void MbedTlsWrapper::AESStateMBEDTLS::SecureClearData(duckdb::data_ptr_t data, duckdb::idx_t len) {
	mbedtls_platform_zeroize(data, len);
}

MbedTlsWrapper::AESStateMBEDTLS::AESStateMBEDTLS(duckdb::unique_ptr<duckdb::EncryptionStateMetadata> metadata_p) : EncryptionState(std::move(metadata_p)), context(duckdb::make_uniq<mbedtls_cipher_context_t>()) {
	mbedtls_cipher_init(context.get());

	auto cipher_info = GetCipher();

	if (!cipher_info) {
		throw std::runtime_error("Failed to get Cipher");
	}

	if (mbedtls_cipher_setup(context.get(), cipher_info)) {
		throw std::runtime_error("Failed to initialize cipher context");
	}

	if (metadata->GetCipher() == duckdb::EncryptionTypes::CBC && mbedtls_cipher_set_padding_mode(context.get(), MBEDTLS_PADDING_PKCS7)) {
		throw std::runtime_error("Failed to set CBC padding");

	}
}

MbedTlsWrapper::AESStateMBEDTLS::~AESStateMBEDTLS() {
	if (context) {
		mbedtls_cipher_free(context.get());
	}
}

void MbedTlsWrapper::AESStateMBEDTLS::GenerateRandomDataInsecure(duckdb::data_ptr_t data, duckdb::idx_t len) {
	if (!force_mbedtls) {
		// To use this insecure MbedTLS random number generator
		// we double check if force_mbedtls_unsafe is set
		// such that we do not accidentaly opt-in
		throw duckdb::InternalException("Insecure random generation called without setting 'force_mbedtls_unsafe' = true");
	}

	duckdb::RandomEngine random_engine;

	while (len != 0) {
		const auto random_integer = random_engine.NextRandomInteger();
		const auto next = duckdb::MinValue<duckdb::idx_t>(len, sizeof(random_integer));
		memcpy(data, duckdb::const_data_ptr_cast(&random_integer), next);
		data += next;
		len -= next;
	}
}

void MbedTlsWrapper::AESStateMBEDTLS::GenerateRandomData(duckdb::data_ptr_t data, duckdb::idx_t len) {
	// generate insecure random data
	GenerateRandomDataInsecure(data, len);
}

void MbedTlsWrapper::AESStateMBEDTLS::InitializeInternal(duckdb::EncryptionNonce &nonce, duckdb::const_data_ptr_t aad, duckdb::idx_t aad_len){
	if (mbedtls_cipher_set_iv(context.get(), nonce.data(), nonce.total_size())) {
		throw std::runtime_error("Failed to set IV for encryption");
	}

	if (aad_len > 0) {
		if (mbedtls_cipher_update_ad(context.get(), aad, aad_len)) {
			throw std::runtime_error("Failed to set AAD");
		}
	}
}

void MbedTlsWrapper::AESStateMBEDTLS::InitializeEncryption(duckdb::EncryptionNonce &nonce, duckdb::const_data_ptr_t key, duckdb::const_data_ptr_t aad, duckdb::idx_t aad_len) {
	mode = duckdb::EncryptionTypes::ENCRYPT;

	if (mbedtls_cipher_setkey(context.get(), key, metadata->GetKeyLen() * 8, MBEDTLS_ENCRYPT) != 0) {
		throw std::runtime_error("Failed to set AES key for encryption");
	}

	InitializeInternal(nonce, aad, aad_len);
}

void MbedTlsWrapper::AESStateMBEDTLS::InitializeDecryption(duckdb::EncryptionNonce &nonce, duckdb::const_data_ptr_t key, duckdb::const_data_ptr_t aad, duckdb::idx_t aad_len) {
	mode = duckdb::EncryptionTypes::DECRYPT;

	if (mbedtls_cipher_setkey(context.get(), key, metadata->GetKeyLen() * 8, MBEDTLS_DECRYPT)) {
		throw std::runtime_error("Failed to set AES key for encryption");
	}

	InitializeInternal(nonce, aad, aad_len);
}

size_t MbedTlsWrapper::AESStateMBEDTLS::Process(duckdb::const_data_ptr_t in, duckdb::idx_t in_len, duckdb::data_ptr_t out,
                                                   duckdb::idx_t out_len) {

	// GCM works in-place, CTR and CBC don't
	auto use_out_copy = in == out && metadata->GetCipher() != CipherType::GCM;

	auto out_ptr = out;
	std::unique_ptr<duckdb::data_t[]> out_copy;
	if (use_out_copy) {
		out_copy.reset(new duckdb::data_t[out_len]);
		out_ptr = out_copy.get();
	}

	size_t out_len_res = duckdb::NumericCast<size_t>(out_len);
	if (mbedtls_cipher_update(context.get(), reinterpret_cast<const unsigned char *>(in), in_len, out_ptr,
	                      &out_len_res)) {
			throw std::runtime_error("Encryption or Decryption failed at Process");
		};

	if (use_out_copy) {
		memcpy(out, out_ptr, out_len_res);
	}
	return out_len_res;
}

void MbedTlsWrapper::AESStateMBEDTLS::FinalizeGCM(duckdb::data_ptr_t tag, duckdb::idx_t tag_len){

	switch (mode) {

	case duckdb::EncryptionTypes::ENCRYPT: {
		if (mbedtls_cipher_write_tag(context.get(), tag, tag_len)) {
			throw std::runtime_error("Writing tag failed");
		}
		break;
	}

	case duckdb::EncryptionTypes::DECRYPT: {
		if (mbedtls_cipher_check_tag(context.get(), tag, tag_len)) {
			throw duckdb::InvalidInputException(
			    "Computed AES tag differs from read AES tag, are you using the right key?");
		}
		break;
	}

	default:
		throw duckdb::InternalException("Unhandled encryption mode %d", static_cast<int>(mode));
	}
}

size_t MbedTlsWrapper::AESStateMBEDTLS::Finalize(duckdb::data_ptr_t out, duckdb::idx_t out_len, duckdb::data_ptr_t tag,
													duckdb::idx_t tag_len) {
	size_t result = out_len;
	if (mbedtls_cipher_finish(context.get(), out, &result)) {
		throw std::runtime_error("Encryption or Decryption failed at Finalize");
	}
	if (metadata->GetCipher() == duckdb::EncryptionTypes::GCM) {
		FinalizeGCM(tag, tag_len);
	}
	return result;
}
