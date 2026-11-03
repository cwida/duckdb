#include "catch.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/random_engine.hpp"
#include "duckdb/main/extension.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/settings.hpp"
#include "mbedtls/pk.h"
#include "mbedtls_wrapper.hpp"
#include "test_helpers.hpp"

#include <fstream>
#include <sstream>

using namespace duckdb;

static constexpr idx_t METADATA_SIZE = ParsedExtensionMetaData::METADATA_SIZE;
// byte offsets of reader fields within a metadata block, which stores its fields in reverse
static constexpr idx_t MARKER_FIELD_OFFSET = 32;
static constexpr idx_t VERSION_FIELD_OFFSET = 4 * 32;

static string MarkerFor(idx_t signature_size) {
	return signature_size == ParsedExtensionMetaData::SIGNATURE_SIZE ? "" : std::to_string(signature_size);
}

// Builds the signed prefix of a stub extension: body + metadata block (fields written in reverse)
static string MakeSignedPrefix(const string &marker, idx_t body_size = 1000) {
	string fields[8];
	fields[0] = "4";
	fields[1] = DuckDB::Platform();
	fields[2] = ExtensionHelper::GetVersionDirectoryName();
	fields[3] = "v0.0.1";
	fields[4] = "CPP";
	fields[6] = marker;
	string result(body_size, 'x');
	for (idx_t i = 8; i > 0; i--) {
		auto field = fields[i - 1];
		field.resize(32, '\0');
		result += field;
	}
	return result;
}

// Appends the signature and, for larger signatures, the tail metadata copy and the zero trailer
static string FinishExtension(const string &prefix, const string &signature) {
	if (signature.size() == ParsedExtensionMetaData::SIGNATURE_SIZE) {
		return prefix + signature;
	}
	return prefix + signature + prefix.substr(prefix.size() - METADATA_SIZE) + string(256, '\0');
}

static string MakeStubExtension(idx_t signature_size, idx_t body_size = 1000) {
	return FinishExtension(MakeSignedPrefix(MarkerFor(signature_size), body_size), string(signature_size, 's'));
}

static string MakeStubWithMarker(idx_t signature_size, const string &marker) {
	return FinishExtension(MakeSignedPrefix(marker), string(signature_size, 's'));
}

static idx_t SignatureOffset(const string &file, idx_t signature_size) {
	if (signature_size == ParsedExtensionMetaData::SIGNATURE_SIZE) {
		return file.size() - signature_size;
	}
	return file.size() - ParsedExtensionMetaData::FOOTER_SIZE - signature_size;
}

// The metadata block covered by the signature (the only one in a legacy footer)
static idx_t SignedMetadataOffset(const string &file, idx_t signature_size) {
	return SignatureOffset(file, signature_size) - METADATA_SIZE;
}

static idx_t TailMetadataOffset(const string &file) {
	return file.size() - ParsedExtensionMetaData::FOOTER_SIZE;
}

static ParsedExtensionMetaData ParseFooter(const string &file) {
	return ExtensionHelper::ParseExtensionFooter(file.data(), file.size(), file.size());
}

static void RequireValidFooter(const string &file, idx_t expected_size) {
	auto parsed = ParseFooter(file);
	REQUIRE(parsed.AppearsValid());
	REQUIRE(parsed.metadata_error.empty());
	REQUIRE(parsed.GetInvalidMetadataError().empty());
	REQUIRE(parsed.signature_size == expected_size);
	REQUIRE(parsed.signature_offset == SignatureOffset(file, expected_size));
	REQUIRE(parsed.extension_version == "v0.0.1");
}

static void RequireFooterError(const string &file, const string &expected_error) {
	auto parsed = ParseFooter(file);
	REQUIRE(parsed.AppearsValid());
	REQUIRE(parsed.HasFooterError());
	REQUIRE(StringUtil::Contains(parsed.GetInvalidMetadataError(), expected_error));
}

static string WriteStubFile(const string &name, const string &content) {
	auto path = TestCreatePath(name + ".duckdb_extension");
	std::ofstream out(path, std::ios::binary);
	out.write(content.data(), NumericCast<std::streamsize>(content.size()));
	return path;
}

static const char *const UNKNOWN_MARKER = "unrecognized signature-size marker";
static const char *const TRUNCATED = "footer is truncated";
static const char *const NONZERO_TRAILER = "must be zero";
static const char *const COPY_MISMATCH = "metadata copies do not match";

// Every malformed footer of a larger signature, with the error it must report
static vector<std::pair<string, string>> MalformedFooters(idx_t signature_size) {
	vector<std::pair<string, string>> result;
	auto stub = MakeStubExtension(signature_size);
	result.emplace_back(MakeStubWithMarker(signature_size, "0" + MarkerFor(signature_size)), UNKNOWN_MARKER);
	auto trailer = stub;
	trailer[trailer.size() - 1] = 1;
	result.emplace_back(trailer, NONZERO_TRAILER);
	auto signed_copy = stub;
	signed_copy[SignedMetadataOffset(stub, signature_size) + VERSION_FIELD_OFFSET]++;
	result.emplace_back(signed_copy, COPY_MISMATCH);
	auto tail_copy = stub;
	tail_copy[TailMetadataOffset(stub) + VERSION_FIELD_OFFSET]++;
	result.emplace_back(tail_copy, COPY_MISMATCH);
	result.emplace_back(
	    stub.substr(stub.size() - (ParsedExtensionMetaData::FOOTER_SIZE + METADATA_SIZE + signature_size - 1)),
	    TRUNCATED);
	return result;
}

TEST_CASE("Extension footer parses every supported signature size", "[extension_footer]") {
	for (idx_t size : {idx_t(256), idx_t(384), idx_t(512)}) {
		INFO(size);
		RequireValidFooter(MakeStubExtension(size), size);
		RequireValidFooter(MakeStubExtension(size, 0), size);
	}
	REQUIRE(ParseFooter(MakeStubExtension(384)).signature_offset == 1000 + METADATA_SIZE);
	REQUIRE(ParseFooter(MakeStubExtension(512)).signature_offset == 1000 + METADATA_SIZE);

	// a legacy footer parses exactly as the plain metadata parser at the legacy offset
	auto legacy = MakeStubExtension(256);
	auto direct = ExtensionHelper::ParseExtensionMetaData(legacy.data() + TailMetadataOffset(legacy));
	auto parsed = ParseFooter(legacy);
	REQUIRE(parsed.magic_value == direct.magic_value);
	REQUIRE(parsed.platform == direct.platform);
	REQUIRE(parsed.duckdb_version == direct.duckdb_version);
	REQUIRE(parsed.extension_version == direct.extension_version);
	REQUIRE(parsed.signature == direct.signature);
	REQUIRE(parsed.signature_size == direct.signature_size);
	REQUIRE(parsed.signature_offset == legacy.size() - 256);

	// only the tail of the file is needed
	auto tail = legacy.substr(legacy.size() - ParsedExtensionMetaData::FOOTER_SIZE);
	auto from_tail = ExtensionHelper::ParseExtensionFooter(tail.data(), tail.size(), legacy.size());
	REQUIRE(from_tail.metadata_error.empty());
	REQUIRE(from_tail.signature_offset == legacy.size() - 256);
}

TEST_CASE("Extension footer rejects non-canonical or unsupported markers", "[extension_footer]") {
	for (auto &marker :
	     {"300", "0384", "abc", "+384", " 384", "384 ", "384x", "38\0014", "\001", "0", "256", "0256", "1024"}) {
		INFO(marker);
		RequireFooterError(MakeStubWithMarker(384, marker), UNKNOWN_MARKER);
		RequireFooterError(MakeStubWithMarker(512, marker), UNKNOWN_MARKER);
		RequireFooterError(MakeStubWithMarker(256, marker), UNKNOWN_MARKER);
	}
	// a valid marker in a legacy-shaped file is checked against the larger layout
	RequireFooterError(MakeStubWithMarker(256, "384"), NONZERO_TRAILER);
}

TEST_CASE("Extension footer rejects malformed larger-signature layouts", "[extension_footer]") {
	for (idx_t size : {idx_t(384), idx_t(512)}) {
		INFO(size);
		for (auto &malformed : MalformedFooters(size)) {
			INFO(malformed.second);
			RequireFooterError(malformed.first, malformed.second);
		}
		auto stub = MakeStubExtension(size);
		auto trailer = stub;
		trailer[trailer.size() - 256] = 1;
		RequireFooterError(trailer, NONZERO_TRAILER);
		// the shortest file that holds the footer is accepted
		RequireValidFooter(MakeStubExtension(size, 0), size);
		REQUIRE(MakeStubExtension(size, 0).size() == ParsedExtensionMetaData::FOOTER_SIZE + METADATA_SIZE + size);
	}
}

TEST_CASE("Extension footer rejects files that are too small", "[extension_footer]") {
	auto legacy = MakeStubExtension(256, 0);
	for (idx_t size : {idx_t(0), idx_t(1), idx_t(255), idx_t(256), idx_t(511)}) {
		auto tail = legacy.substr(legacy.size() - size);
		REQUIRE(!ParseFooter(tail).AppearsValid());
	}

	DuckDB db(nullptr);
	auto tail = legacy.substr(1);
	REQUIRE(!ExtensionHelper::CheckExtensionBufferSignature(*db.instance, tail.data(), tail.size(),
	                                                        ExtensionRepositoryType::CORE, ""));
	Connection con(db);
	auto path = WriteStubFile("footer_truncated", tail);
	auto result = con.Query(StringUtil::Format("LOAD '%s'", path));
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "must be at least"));
}

// Engine-style parse: only the last min(file_size, MAX_FOOTER_SIZE) bytes are passed in
static ParsedExtensionMetaData ParseFooterTail(const string &file) {
	auto tail_size = MinValue<idx_t>(file.size(), ParsedExtensionMetaData::MAX_FOOTER_SIZE);
	return ExtensionHelper::ParseExtensionFooter(file.data() + file.size() - tail_size, tail_size, file.size());
}

TEST_CASE("Extension footer parses from a bounded tail at the size boundaries", "[extension_footer]") {
	REQUIRE(ParsedExtensionMetaData::MAX_FOOTER_SIZE == 1280);
	for (idx_t size : {idx_t(256), idx_t(384), idx_t(512)}) {
		INFO(size);
		auto minimal = MakeStubExtension(size, 0);
		auto minimal_size = size == ParsedExtensionMetaData::SIGNATURE_SIZE
		                        ? ParsedExtensionMetaData::FOOTER_SIZE
		                        : ParsedExtensionMetaData::FOOTER_SIZE + METADATA_SIZE + size;
		REQUIRE(minimal.size() == minimal_size);
		for (idx_t body_size : {idx_t(0), idx_t(1), ParsedExtensionMetaData::MAX_FOOTER_SIZE - minimal_size,
		                        ParsedExtensionMetaData::MAX_FOOTER_SIZE - minimal_size + 1, idx_t(100000)}) {
			INFO(body_size);
			auto file = MakeStubExtension(size, body_size);
			auto parsed = ParseFooterTail(file);
			REQUIRE(parsed.AppearsValid());
			REQUIRE(parsed.metadata_error.empty());
			REQUIRE(parsed.signature_size == size);
			REQUIRE(parsed.signature_offset == SignatureOffset(file, size));
			REQUIRE(parsed.signature_offset == body_size + METADATA_SIZE);
		}
		if (size != ParsedExtensionMetaData::SIGNATURE_SIZE) {
			// one byte short of the footer, both as the whole file and as a tail of a larger file
			auto truncated = minimal.substr(1);
			auto parsed = ParseFooterTail(truncated);
			REQUIRE(parsed.HasFooterError());
			REQUIRE(StringUtil::Contains(parsed.GetInvalidMetadataError(), TRUNCATED));
			auto large = string(100000, 'x') + truncated;
			parsed = ParseFooterTail(large);
			REQUIRE(ParsedExtensionMetaData::MAX_FOOTER_SIZE < large.size());
			REQUIRE(parsed.HasFooterError());
		}
	}
	REQUIRE(MakeStubExtension(384, 0).size() == 1152);
	REQUIRE(MakeStubExtension(512, 0).size() == 1280);
}

TEST_CASE("Larger-signature footers keep legacy-readable metadata and a zero trailer at the tail",
          "[extension_footer]") {
	for (idx_t size : {idx_t(384), idx_t(512)}) {
		auto stub = MakeStubExtension(size);
		// the legacy reader parses the metadata at the fixed tail position and takes the last 256 bytes as signature
		auto legacy_view = ExtensionHelper::ParseExtensionMetaData(stub.data() + TailMetadataOffset(stub));
		REQUIRE(legacy_view.AppearsValid());
		REQUIRE(legacy_view.extension_version == "v0.0.1");
		REQUIRE(stub.substr(stub.size() - 256) == string(256, '\0'));
	}
}

TEST_CASE("Extension install and load use the footer parser", "[extension_footer]") {
	DBConfig config;
	config.SetOptionByName("extension_directory", TestCreatePath("footer_ext_dir"));
	config.SetOptionByName("allow_unsigned_extensions", true);
	DuckDB db(nullptr, &config);
	Connection con(db);

	for (idx_t size : {idx_t(256), idx_t(384), idx_t(512)}) {
		auto name = "footer_ok_" + std::to_string(size);
		auto path = WriteStubFile(name, MakeStubExtension(size));
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("INSTALL '%s'", path)));
		auto version = con.Query(
		    StringUtil::Format("SELECT extension_version FROM duckdb_extensions() WHERE extension_name = '%s'", name));
		REQUIRE(CHECK_COLUMN(version, 0, {"v0.0.1"}));
	}

	// a platform mismatch is overridable, unlike a malformed footer
	auto foreign = MakeStubExtension(384);
	foreign[SignedMetadataOffset(foreign, 384) + 6 * 32]++;
	foreign[TailMetadataOffset(foreign) + 6 * 32]++;
	auto foreign_path = WriteStubFile("footer_foreign", foreign);
	REQUIRE_FAIL(con.Query(StringUtil::Format("INSTALL '%s'", foreign_path)));

	REQUIRE_NO_FAIL(con.Query("SET allow_extensions_metadata_mismatch=true"));
	REQUIRE_NO_FAIL(con.Query(StringUtil::Format("INSTALL '%s'", foreign_path)));
	idx_t index = 0;
	for (idx_t size : {idx_t(384), idx_t(512)}) {
		for (auto &malformed : MalformedFooters(size)) {
			INFO(malformed.second);
			auto path = WriteStubFile("footer_malformed_" + std::to_string(index++), malformed.first);
			for (auto &statement : {"INSTALL '%s'", "FORCE INSTALL '%s'", "LOAD '%s'"}) {
				auto result = con.Query(StringUtil::Format(statement, path));
				REQUIRE(result->HasError());
				REQUIRE(StringUtil::Contains(result->GetError(), malformed.second));
			}
		}
	}
	auto unknown_path = WriteStubFile("footer_unknown", MakeStubWithMarker(256, "256"));
	auto result = con.Query(StringUtil::Format("INSTALL '%s'", unknown_path));
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), UNKNOWN_MARKER));
}

TEST_CASE("Signed extension loading rejects stubs of every footer size", "[extension_footer]") {
	DuckDB db(nullptr);
	Connection con(db);
	for (idx_t size : {idx_t(256), idx_t(384), idx_t(512)}) {
		auto path = WriteStubFile("footer_unsigned_" + std::to_string(size), MakeStubExtension(size));
		auto result = con.Query(StringUtil::Format("LOAD '%s'", path));
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "signature is either missing or invalid"));
	}
}

struct TestSigningKey {
	idx_t signature_size;
	const char *private_key_path;
	const char *public_key_path;
};

static const TestSigningKey TEST_SIGNING_KEYS[] = {
    {256, "test/mbedtls/private.pem", "test/mbedtls/public.pem"},
    {384, "test/mbedtls/private_3072.pem", "test/mbedtls/public_3072.pem"},
    {512, "test/mbedtls/private_4096.pem", "test/mbedtls/public_4096.pem"}};

static string ReadTestFile(const string &path) {
	std::ifstream in(path, std::ios::binary);
	std::stringstream buffer;
	buffer << in.rdbuf();
	return buffer.str();
}

// Same scheme as the engine: SHA-256 of the concatenated SHA-256 hashes of 1 MiB chunks
static string ComputeExtensionHash(const string &signed_region) {
	const idx_t chunk_size = 1024ULL * 1024ULL;
	string chunk_hashes;
	for (idx_t start = 0; start < signed_region.size(); start += chunk_size) {
		chunk_hashes += duckdb_mbedtls::MbedTlsWrapper::ComputeSha256Hash(signed_region.substr(start, chunk_size));
	}
	return duckdb_mbedtls::MbedTlsWrapper::ComputeSha256Hash(chunk_hashes);
}

static int TestRandom(void *rng, unsigned char *output, size_t length) {
	auto &engine = *static_cast<RandomEngine *>(rng);
	for (size_t i = 0; i < length; i++) {
		output[i] = static_cast<unsigned char>(engine.NextRandomInteger());
	}
	return 0;
}

// RSA PKCS#1 v1.5 SHA-256 signature of the hash
static string SignHash(const string &private_key_pem, const string &hash) {
	RandomEngine engine(42);
	mbedtls_pk_context pk;
	mbedtls_pk_init(&pk);
	string signature(MBEDTLS_PK_SIGNATURE_MAX_SIZE, '\0');
	size_t signature_length = 0;
	auto key = reinterpret_cast<const unsigned char *>(private_key_pem.c_str());
	auto hash_data = reinterpret_cast<const unsigned char *>(hash.data());
	bool success = mbedtls_pk_parse_key(&pk, key, private_key_pem.size() + 1, nullptr, 0, TestRandom, &engine) == 0 &&
	               mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash_data, hash.size(),
	                               reinterpret_cast<unsigned char *>(&signature[0]), signature.size(),
	                               &signature_length, TestRandom, &engine) == 0;
	mbedtls_pk_free(&pk);
	REQUIRE(success);
	signature.resize(signature_length);
	return signature;
}

static string MakeSignedStub(const TestSigningKey &key) {
	auto prefix = MakeSignedPrefix(MarkerFor(key.signature_size));
	auto signature = SignHash(ReadTestFile(key.private_key_path), ComputeExtensionHash(prefix));
	REQUIRE(signature.size() == key.signature_size);
	return FinishExtension(prefix, signature);
}

static void WriteBinaryFile(const string &path, const string &content) {
	std::ofstream out(path, std::ios::binary);
	out.write(content.data(), NumericCast<std::streamsize>(content.size()));
}

TEST_CASE("Signed extensions of every footer size verify against a trusted repository key", "[extension_footer]") {
	DBConfig config;
	config.SetOptionByName("extension_directory", TestCreatePath("footer_signed_ext_dir"));
	config.SetOptionByName("extension_repository_directory", TestCreatePath("footer_signed_repositories"));
	config.SetOptionByName("allow_extension_repositories", "allowed");
	DuckDB db(nullptr, &config);
	Connection con(db);
	auto &instance = *db.instance;
	LocalFileSystem fs;
	REQUIRE(!Settings::Get<AllowUnsignedExtensionsSetting>(instance));

	auto check = [&](const string &file, const string &repository) {
		return ExtensionHelper::CheckExtensionBufferSignature(instance, file.data(), file.size(),
		                                                      ExtensionRepositoryType::USER_PROVIDED, repository);
	};

	vector<string> prefixes;
	vector<string> stubs;
	for (auto &key : TEST_SIGNING_KEYS) {
		auto size = std::to_string(key.signature_size);
		INFO(size);
		auto repository = "signed" + size;
		auto name = "footer_signed_" + size;
		auto prefix = TestCreatePath("footer_signed_repo_" + size);
		auto extension_dir =
		    fs.JoinPath(fs.JoinPath(prefix, ExtensionHelper::GetVersionDirectoryName()), DuckDB::Platform());
		fs.CreateDirectoriesRecursive(extension_dir);
		auto extension_path = fs.JoinPath(extension_dir, name + ".duckdb_extension");
		auto stub = MakeSignedStub(key);
		WriteBinaryFile(extension_path, stub);
		REQUIRE_NO_FAIL(
		    con.Query(StringUtil::Format("CREATE EXTENSION REPOSITORY %s WITH PREFIX '%s' USING PUBLIC KEY '%s'",
		                                 repository, prefix, ReadTestFile(key.public_key_path))));

		REQUIRE(check(stub, repository));
		REQUIRE_NO_FAIL(con.Query(StringUtil::Format("INSTALL %s FROM %s", name, repository)));
		// the stub is not a shared library, so a load that passed signature verification fails opening it
		auto result = con.Query(StringUtil::Format("LOAD %s FROM %s", name, repository));
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "could not be loaded: "));
		REQUIRE(!StringUtil::Contains(result->GetError(), "signature"));

		auto tampered_body = stub;
		tampered_body[10]++;
		REQUIRE(!check(tampered_body, repository));
		auto tampered_marker = stub;
		tampered_marker[SignedMetadataOffset(stub, key.signature_size) + MARKER_FIELD_OFFSET]++;
		REQUIRE(!check(tampered_marker, repository));
		auto tampered_signature = stub;
		tampered_signature[SignatureOffset(stub, key.signature_size) + 5]++;
		REQUIRE(!check(tampered_signature, repository));
		// the same field changed in both metadata copies passes the copy check but not the signature
		auto tampered_metadata = stub;
		tampered_metadata[SignedMetadataOffset(stub, key.signature_size) + VERSION_FIELD_OFFSET]++;
		if (key.signature_size != ParsedExtensionMetaData::SIGNATURE_SIZE) {
			tampered_metadata[TailMetadataOffset(stub) + VERSION_FIELD_OFFSET]++;
		}
		auto tampered_parsed = ParseFooter(tampered_metadata);
		REQUIRE(tampered_parsed.metadata_error.empty());
		REQUIRE(tampered_parsed.extension_version == "w0.0.1");
		REQUIRE(!check(tampered_metadata, repository));
		if (key.signature_size != ParsedExtensionMetaData::SIGNATURE_SIZE) {
			// what an older reader verifies: the prefix before the zero trailer, against the trailer
			auto legacy_offset = stub.size() - 256;
			REQUIRE(!ExtensionHelper::CheckExtensionBufferSignature(
			    instance, stub.data(), legacy_offset, stub.substr(legacy_offset),
			    ExtensionRepositoryType::USER_PROVIDED, repository));
		}

		WriteBinaryFile(extension_path, tampered_body);
		result = con.Query(StringUtil::Format("FORCE INSTALL %s FROM %s", name, repository));
		REQUIRE(result->HasError());
		REQUIRE(StringUtil::Contains(result->GetError(), "doesn't have a valid signature"));

		prefixes.push_back(prefix);
		stubs.push_back(stub);
	}

	// a 384-byte signature never verifies against a 2048-bit key
	REQUIRE(!check(stubs[1], "signed256"));
	auto extension_dir =
	    fs.JoinPath(fs.JoinPath(prefixes[0], ExtensionHelper::GetVersionDirectoryName()), DuckDB::Platform());
	WriteBinaryFile(fs.JoinPath(extension_dir, "footer_wrong_key.duckdb_extension"), stubs[1]);
	auto result = con.Query("INSTALL footer_wrong_key FROM signed256");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "doesn't have a valid signature"));
}
