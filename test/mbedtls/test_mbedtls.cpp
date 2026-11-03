#include "catch.hpp"
#include "mbedtls_wrapper.hpp"

#include <chrono>
#include <thread>
#include <fstream>
#include <sstream>

using namespace duckdb_mbedtls;

static std::string file_to_string(std::string filename) {
	std::ifstream stream(filename, std::ios_base::binary);
	duckdb::stringstream buffer;
	buffer << stream.rdbuf();
	return buffer.str();
}

TEST_CASE("Test that we can verify a signature", "[mbedtls]") {
	// those files are created with the create_files.sh script
	auto file_content = file_to_string("test/mbedtls/dummy_file");
	auto signature = file_to_string("test/mbedtls/dummy_file.signature");
	auto pubkey = file_to_string("test/mbedtls/public.pem");

	auto hash = MbedTlsWrapper::ComputeSha256Hash(file_content);
	REQUIRE(MbedTlsWrapper::IsValidSha256Signature(pubkey, signature, hash));
	std::string empty_string = "";

	auto borked_pubkey = pubkey;
	borked_pubkey[10]++;

	// a borked public key is an exception, this should never happen
	REQUIRE_THROWS(MbedTlsWrapper::IsValidSha256Signature(borked_pubkey, signature, hash));
	REQUIRE_THROWS(MbedTlsWrapper::IsValidSha256Signature(empty_string, signature, hash));

	// a signature not matching the key modulus is rejected, a wrong-length hash should never happen
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey, empty_string, hash));
	REQUIRE_THROWS(MbedTlsWrapper::IsValidSha256Signature(pubkey, signature, empty_string));

	// lets flip some bits in the file, it should not validate
	auto borked_file = file_content;
	borked_file[10]++;
	auto hash2 = MbedTlsWrapper::ComputeSha256Hash(borked_file);
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey, signature, hash2));

	auto borked_signature = signature;
	borked_signature[10]++;
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey, borked_signature, hash));

	auto borked_hash = hash;
	borked_hash[10]++;
	auto hash3 = MbedTlsWrapper::ComputeSha256Hash(empty_string);
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey, signature, hash3));
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey, signature, borked_hash));

	// seems all right!
	REQUIRE(MbedTlsWrapper::IsValidSha256Signature(pubkey, signature, hash));
}

TEST_CASE("Test that we can verify an RSA-3072 (384-byte) signature", "[mbedtls]") {
	// those files are created with the create_files.sh script (3072-bit key, 384-byte PKCS#1 v1.5 SHA-256 signature)
	auto file_content = file_to_string("test/mbedtls/dummy_file");
	auto signature_3072 = file_to_string("test/mbedtls/dummy_file.signature_3072");
	auto pubkey_3072 = file_to_string("test/mbedtls/public_3072.pem");

	// the 3072 signature is exactly 384 bytes (3072 / 8)
	REQUIRE(signature_3072.size() == 384);

	auto hash = MbedTlsWrapper::ComputeSha256Hash(file_content);

	// (a) a valid RSA-3072 signature + matching 3072 key verifies TRUE
	REQUIRE(MbedTlsWrapper::IsValidSha256Signature(pubkey_3072, signature_3072, hash));

	// (b) regression: the existing RSA-2048 (256-byte) case still verifies TRUE
	auto signature_2048 = file_to_string("test/mbedtls/dummy_file.signature");
	auto pubkey_2048 = file_to_string("test/mbedtls/public.pem");
	REQUIRE(signature_2048.size() == 256);
	REQUIRE(MbedTlsWrapper::IsValidSha256Signature(pubkey_2048, signature_2048, hash));

	// (c) a tampered 3072 signature (still 384 bytes) verifies FALSE
	auto borked_signature_3072 = signature_3072;
	borked_signature_3072[10]++;
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey_3072, borked_signature_3072, hash));

	// a 3072 signature against a file whose hash changed verifies FALSE
	auto borked_file = file_content;
	borked_file[10]++;
	auto borked_hash = MbedTlsWrapper::ComputeSha256Hash(borked_file);
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey_3072, signature_3072, borked_hash));

	// (d) a truncated 3072 signature is rejected
	auto truncated_signature = signature_3072.substr(0, 300);
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey_3072, truncated_signature, hash));

	// defense-in-depth: a 384-byte signature presented against a 2048-bit key is rejected (modulus mismatch), FALSE
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey_2048, signature_3072, hash));
	// and a 256-byte signature presented against a 3072-bit key is rejected (modulus mismatch), FALSE
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey_3072, signature_2048, hash));

	// a 3072-bit public key is accepted as a valid RSA signing key
	REQUIRE(MbedTlsWrapper::IsValidRSAPublicKey(pubkey_3072));
	// as is the legacy 2048-bit key
	REQUIRE(MbedTlsWrapper::IsValidRSAPublicKey(pubkey_2048));
}

TEST_CASE("Test that we can verify an RSA-4096 (512-byte) signature", "[mbedtls]") {
	auto file_content = file_to_string("test/mbedtls/dummy_file");
	auto hash = MbedTlsWrapper::ComputeSha256Hash(file_content);
	auto signature_4096 = file_to_string("test/mbedtls/dummy_file.signature_4096");
	auto pubkey_4096 = file_to_string("test/mbedtls/public_4096.pem");
	auto signature_3072 = file_to_string("test/mbedtls/dummy_file.signature_3072");
	auto pubkey_3072 = file_to_string("test/mbedtls/public_3072.pem");
	REQUIRE(signature_4096.size() == 512);

	REQUIRE(MbedTlsWrapper::IsValidSha256Signature(pubkey_4096, signature_4096, hash));
	REQUIRE(MbedTlsWrapper::IsValidRSAPublicKey(pubkey_4096));

	auto borked_signature = signature_4096;
	borked_signature[10]++;
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey_4096, borked_signature, hash));

	// signature length must match the key modulus
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey_3072, signature_4096, hash));
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey_4096, signature_3072, hash));
	REQUIRE(!MbedTlsWrapper::IsValidSha256Signature(pubkey_4096, signature_4096.substr(0, 384), hash));
}
