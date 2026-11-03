//===----------------------------------------------------------------------===//
//                         DuckDB
//
// mbedtls_wrapper.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/encryption_state.hpp"

#include <string>

typedef struct mbedtls_cipher_context_t mbedtls_cipher_context_t;
typedef struct mbedtls_cipher_info_t mbedtls_cipher_info_t;

namespace duckdb_mbedtls {



class MbedTlsWrapper {
public:
	static void ComputeSha256Hash(const char *in, size_t in_len, char *out);
	static std::string ComputeSha256Hash(const std::string &file_content);
	static bool IsValidRSAPublicKey(const std::string &pubkey);
	static bool IsValidSha256Signature(const std::string &pubkey, const std::string &signature,
	                                   const std::string &sha256_hash);
	static void Hmac256(const char *key, size_t key_len, const char *message, size_t message_len, char *out);
	static void ToBase16(char *in, char *out, size_t len);

	static constexpr size_t SHA256_HASH_LENGTH_BYTES = 32;
	static constexpr size_t SHA256_HASH_LENGTH_TEXT = 64;

	class SHA256State {
	public:
		SHA256State();
		~SHA256State();
		void AddString(const std::string &str);
		void AddBytes(duckdb::data_ptr_t input_bytes, duckdb::idx_t len);
		void AddBytes(duckdb::const_data_ptr_t input_bytes, duckdb::idx_t len);
		void AddSalt(unsigned char *salt, size_t salt_len);
		std::string Finalize();
		void FinishHex(char *out);
		void FinalizeDerivedKey(duckdb::data_ptr_t hash);

	private:
		void *sha_context;

	};

	static constexpr size_t SHA1_HASH_LENGTH_BYTES = 20;
	static constexpr size_t SHA1_HASH_LENGTH_TEXT = 40;

	class SHA1State {
	public:
		SHA1State();
		~SHA1State();
		void AddString(const std::string &str);
		std::string Finalize();
		void FinishHex(char *out);

	private:
		void *sha_context;
	};

class AESStateMBEDTLS : public duckdb::EncryptionState {
	public:
		DUCKDB_API explicit AESStateMBEDTLS(duckdb::unique_ptr<duckdb::EncryptionStateMetadata> metadata);
		DUCKDB_API ~AESStateMBEDTLS() override;

	public:
		DUCKDB_API void InitializeEncryption(duckdb::EncryptionNonce &nonce, duckdb::const_data_ptr_t key, duckdb::const_data_ptr_t aad, duckdb::idx_t aad_len) override;
		DUCKDB_API void InitializeDecryption(duckdb::EncryptionNonce &nonce, duckdb::const_data_ptr_t key, duckdb::const_data_ptr_t aad, duckdb::idx_t aad_len) override;

		DUCKDB_API size_t Process(duckdb::const_data_ptr_t in, duckdb::idx_t in_len, duckdb::data_ptr_t out,
		                          duckdb::idx_t out_len) override;
		DUCKDB_API size_t Finalize(duckdb::data_ptr_t out, duckdb::idx_t out_len, duckdb::data_ptr_t tag, duckdb::idx_t tag_len) override;

		DUCKDB_API static void GenerateRandomDataStatic(duckdb::data_ptr_t data, duckdb::idx_t len);
		DUCKDB_API void GenerateRandomData(duckdb::data_ptr_t data, duckdb::idx_t len) override;
		DUCKDB_API void FinalizeGCM(duckdb::data_ptr_t tag, duckdb::idx_t tag_len);
		DUCKDB_API const mbedtls_cipher_info_t *GetCipher();
		DUCKDB_API static void SecureClearData(duckdb::data_ptr_t data, duckdb::idx_t len);

	public:
		void ForceMbedTLSUnsafe() {
			force_mbedtls = true;
		}

		void UndoForceMbedTLSUnsafe() {
			force_mbedtls = false;
		}


	private:
		DUCKDB_API void InitializeInternal(duckdb::EncryptionNonce &nonce, duckdb::const_data_ptr_t aad, duckdb::idx_t aad_len);
		DUCKDB_API void GenerateRandomDataInsecure(duckdb::data_ptr_t data, duckdb::idx_t len);

	private:
		duckdb::EncryptionTypes::Mode mode;
		duckdb::unique_ptr<mbedtls_cipher_context_t> context;
		bool force_mbedtls = false;
	};

	class AESStateMBEDTLSFactory : public duckdb::EncryptionUtil {

	public:
		duckdb::shared_ptr<duckdb::EncryptionState> CreateEncryptionState(duckdb::unique_ptr<duckdb::EncryptionStateMetadata> metadata_p) const override {
			auto mbedtls_state = duckdb::make_shared_ptr<MbedTlsWrapper::AESStateMBEDTLS>(std::move(metadata_p));

			if (force_mbedtls_factory) {
				mbedtls_state->ForceMbedTLSUnsafe();
			}

			return mbedtls_state;
		}

		DUCKDB_API void Hash(duckdb::CryptoHashFunction function, duckdb::const_data_ptr_t input,
		                     duckdb::idx_t input_len, duckdb::data_ptr_t output) const override;
		DUCKDB_API duckdb::unique_ptr<duckdb::CryptoHashState>
		CreateHashState(duckdb::CryptoHashFunction function) const override;
		DUCKDB_API void Hmac(duckdb::CryptoHashFunction function, duckdb::const_data_ptr_t key, duckdb::idx_t key_len,
		                     duckdb::const_data_ptr_t input, duckdb::idx_t input_len,
		                     duckdb::data_ptr_t output) const override;
		DUCKDB_API bool SupportsHash(duckdb::CryptoHashFunction function) const override;
		DUCKDB_API bool SupportsHmac(duckdb::CryptoHashFunction function) const override;

		~AESStateMBEDTLSFactory() override {} //

	public:
		void ForceMbedTLSUnsafe() {
			force_mbedtls_factory = true;
		}

		void UndoForceMbedTLSUnsafe() {
			force_mbedtls_factory = false;
		}

	private:
		bool force_mbedtls_factory = false;
	};
};

} // namespace duckdb_mbedtls
