#include "catch.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "test_helpers.hpp"

// the last part of the test limits the threads that can start through the default thread attributes, a glibc extension
#if defined(__linux__) && !defined(DUCKDB_NO_THREADS) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 18)
#define SIGNATURE_CHECK_TEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
#endif
#endif

#ifdef SIGNATURE_CHECK_TEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
#include <fstream>
#include <mutex>
#include <pthread.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#endif

using namespace duckdb;

namespace {

// An RSA 2048 key pair made for this test with openssl genrsa. This is the public key in the compact form that
// CREATE EXTENSION REPOSITORY takes (base64 of the DER encoding). The private key only signed SIGNATURE_HEX below.
constexpr const char *SIGNING_PUBLIC_KEY =
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4TQiDZgqbR0dQu/x9vqx+kxCQ5jJxYf75y/aHAffMGcn0W1rfwkbX+48SJu3wepD4Q"
    "hBg02XYaNgnlqeMa9eYHkxo4tQ09g36c1cMNuVLvb75fG0MrZMP0GU086ydp/UbjLcRMZHUuLE8PMCOFUqrwMlt/qA82I2naNpS9Efjid/btzE"
    "F/gtWUY8XyLAKRcjCupdzIaxBhwOQkeAZJrP+r9A8FdoA0TnUCauiUW4qkNq0qe2wp41R48yFuBJlX0+miJ9QaNorYkmv7kpknypZ+oEeLd3Ql"
    "KPwqCCDPYc7OYkAncoOu9QIHVGC66urNqKTCMOUK8BZY8CBEAbeyUUdwIDAQAB";

// The extension buffer of the test is BUFFER_SIZE bytes, the signature in its last 256 bytes. The signed part is
// hashed in four 1 MiB chunks (the last one 256 bytes short), one task each. Every byte of chunk k is k, so a chunk
// hashed from the wrong offset changes the hash.
constexpr idx_t BUFFER_SIZE = 4ULL * 1024 * 1024;
constexpr idx_t CHUNK_SIZE = 1024ULL * 1024;

// The RSASSA PKCS1 v1.5 signature of the two level hash of the signed part (sha256 of the concatenated sha256 of
// each chunk), made with openssl pkeyutl in sign mode with the sha256 digest option
constexpr const char *SIGNATURE_HEX =
    "c8d7aaeda40583aedc1948d9455d4d00ea6c1398d7ac73fb7fe4e9afc100f1c178b93810e06be899a3310f3a95e58056a79b31467643fba9"
    "0f3ef18502a86b99fa1c4cbbd403df10ef255e16bb267feb2087cb529b0a47194e6bab2c16f3106d3ec96f93e949200701ce8b6c1e2db989"
    "aca2e39be316c7ccd6990f3dac5c93426e389b84ecc57dc6a4eda8dc695ee3788d983e874a18dded2e293f55299ebe759a35c651ca3fc1f6"
    "5e59c71d98d80a29fd5bc9cb814de18913234cb34f93696b9c0598f98a17a43a83bc74c68927bf94230c06a39ae508c6fa99d8fd18f6651f"
    "cd58edcd3d6928d6552f2988071cc7d93c89bdd80e1f6c215eb085cb63c849f2";

vector<char> SignedBuffer() {
	vector<char> buffer(BUFFER_SIZE);
	for (idx_t i = 0; i < BUFFER_SIZE; i++) {
		buffer[i] = static_cast<char>(i / CHUNK_SIZE);
	}
	const string hex(SIGNATURE_HEX);
	REQUIRE(hex.size() == 2 * ParsedExtensionMetaData::SIGNATURE_SIZE);
	auto signature = buffer.data() + BUFFER_SIZE - ParsedExtensionMetaData::SIGNATURE_SIZE;
	for (idx_t i = 0; i < ParsedExtensionMetaData::SIGNATURE_SIZE; i++) {
		signature[i] = static_cast<char>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
	}
	return buffer;
}

//! Opens a database that trusts the signing key of the test for the repository "signed"
unique_ptr<DuckDB> OpenDatabaseWithSigningKey(DBConfig &config, const string &repository_directory) {
	config.SetOptionByName("extension_repository_directory", TestCreatePath(repository_directory));
	config.SetOptionByName("allow_extension_repositories", "allowed");
	auto db = make_uniq<DuckDB>(nullptr, &config);
	Connection con(*db);
	REQUIRE_NO_FAIL(con.Query(
	    StringUtil::Format("CREATE EXTENSION REPOSITORY signed WITH PREFIX 'http://signed.test' USING PUBLIC KEY '%s'",
	                       SIGNING_PUBLIC_KEY)));
	return db;
}

//! Opens a database without worker threads, which hashes the chunks on the checking thread, as a starting database
//! does for the extensions it loads
unique_ptr<DuckDB> OpenDatabaseWithoutWorkers(DBConfig &config, const string &repository_directory) {
	config.options.maximum_threads = 1;
	config.options.async_threads = 0;
	return OpenDatabaseWithSigningKey(config, repository_directory);
}

//! Checks the buffer as INSTALL checks a downloaded extension
bool SignatureVerifies(DuckDB &db, const vector<char> &buffer) {
	return ExtensionHelper::CheckExtensionBufferSignature(*db.instance, buffer.data(), BUFFER_SIZE,
	                                                      ExtensionRepositoryType::USER_PROVIDED, "signed");
}

//! Serves the signed buffer as the file of an extension, and fails every read of one chunk when one is given
class SignedFileSystem : public FileSystem {
public:
	SignedFileSystem(const vector<char> &buffer_p, optional_idx unreadable_chunk_p)
	    : buffer(buffer_p), unreadable_chunk(unreadable_chunk_p) {
	}

public:
	string GetName() const override {
		return "SignedFileSystem";
	}
	int64_t GetFileSize(FileHandle &handle) override {
		return NumericCast<int64_t>(buffer.size());
	}
	void Read(FileHandle &handle, void *target, int64_t nr_bytes, idx_t location) override {
		if (unreadable_chunk.IsValid() && location / CHUNK_SIZE == unreadable_chunk.GetIndex()) {
			throw IOException("the extension file could not be read");
		}
		memcpy(target, buffer.data() + location, NumericCast<idx_t>(nr_bytes));
	}

private:
	const vector<char> &buffer;
	optional_idx unreadable_chunk;
};

class SignedFileHandle : public FileHandle {
public:
	explicit SignedFileHandle(FileSystem &file_system)
	    : FileHandle(file_system, "signed.duckdb_extension", FileFlags::FILE_FLAGS_READ) {
	}

public:
	void Close() override {
	}
};

//! Checks the file as LOAD checks an extension file
bool FileSignatureVerifies(DuckDB &db, SignedFileSystem &file_system) {
	SignedFileHandle handle(file_system);
	ParsedExtensionMetaData metadata;
	metadata.signature.resize(ParsedExtensionMetaData::SIGNATURE_SIZE);
	return ExtensionHelper::CheckExtensionSignature(*db.instance, handle, metadata,
	                                                ExtensionRepositoryType::USER_PROVIDED, "signed");
}

//! The check of a file with an unreadable chunk fails with the read error of that chunk
void RequireReadError(DuckDB &db, SignedFileSystem &file_system) {
	bool failed = false;
	try {
		FileSignatureVerifies(db, file_system);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		REQUIRE(error.Type() == ExceptionType::IO);
		REQUIRE(StringUtil::Contains(error.RawMessage(), "could not be read"));
		failed = true;
	}
	REQUIRE(failed);
}

#ifdef SIGNATURE_CHECK_TEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES

//! The address space of this process in bytes, 0 when it cannot be read
idx_t CurrentAddressSpace() {
	std::ifstream statm("/proc/self/statm");
	idx_t pages = 0;
	statm >> pages;
	auto page_size = sysconf(_SC_PAGESIZE);
	if (!statm || page_size <= 0) {
		return 0;
	}
	return pages * NumericCast<idx_t>(page_size);
}

//! Lets no thread start while it lives. Every new thread gets a 256 MiB stack, the address space limit leaves room
//! for two of them, and two threads are parked on it, so the next pthread_create fails with EAGAIN, as it does under
//! a thread limit. The destructor releases the threads and restores both settings.
class NoMoreThreads {
public:
	static constexpr idx_t STACK_SIZE = 256ULL * 1024 * 1024;
	static constexpr idx_t PARKED_THREADS = 2;

	NoMoreThreads() : closed(gate) {
		// everything that can fail comes before the settings change, so the destructor always sees what it restores
		threads.reserve(PARKED_THREADS + 1);
		auto address_space = CurrentAddressSpace();
		if (address_space == 0 || pthread_getattr_default_np(&old_attr) != 0) {
			return;
		}
		attr_read = true;
		pthread_attr_init(&new_attr);
		if (pthread_attr_setstacksize(&new_attr, STACK_SIZE) != 0 || getrlimit(RLIMIT_AS, &old_limit) != 0) {
			return;
		}
		auto new_limit = old_limit;
		new_limit.rlim_cur = address_space + STACK_SIZE * 5 / 2;
		attr_set = pthread_setattr_default_np(&new_attr) == 0;
		limit_set = attr_set && setrlimit(RLIMIT_AS, &new_limit) == 0;
		if (!limit_set) {
			return;
		}
		// the parked threads wait for the gate, which the destructor opens, and one more thread must fail to start
		try {
			for (idx_t i = 0; i <= PARKED_THREADS; i++) {
				threads.emplace_back([this]() { std::lock_guard<std::mutex> hold(gate); });
			}
		} catch (std::exception &) {
			works = threads.size() == PARKED_THREADS;
		}
	}
	~NoMoreThreads() {
		closed.unlock();
		for (auto &thread : threads) {
			thread.join();
		}
		if (limit_set) {
			setrlimit(RLIMIT_AS, &old_limit);
		}
		if (attr_set) {
			pthread_setattr_default_np(&old_attr);
		}
		if (attr_read) {
			pthread_attr_destroy(&new_attr);
			pthread_attr_destroy(&old_attr);
		}
	}

public:
	//! Whether two threads started and the third did not, which needs a platform that honors both settings
	bool Works() const {
		return works;
	}

private:
	std::mutex gate;
	std::unique_lock<std::mutex> closed;
	vector<std::thread> threads;
	pthread_attr_t old_attr;
	pthread_attr_t new_attr;
	bool attr_read = false;
	bool attr_set = false;
	rlimit old_limit;
	bool limit_set = false;
	bool works = false;
};

#endif

} // namespace

TEST_CASE("Test that a signature check hashes the chunks of an extension without starting a thread", "[api]") {
	auto buffer = SignedBuffer();
	DBConfig config;
	auto db = OpenDatabaseWithSigningKey(config, "extension_signature_check");
	DBConfig no_worker_config;
	auto no_worker_db = OpenDatabaseWithoutWorkers(no_worker_config, "extension_signature_check_no_workers");

	REQUIRE(SignatureVerifies(*db, buffer));
	REQUIRE(SignatureVerifies(*no_worker_db, buffer));
	SignedFileSystem readable_file(buffer, optional_idx());
	REQUIRE(FileSignatureVerifies(*db, readable_file));
	REQUIRE(FileSignatureVerifies(*no_worker_db, readable_file));

	// a changed byte fails the check
	auto tampered = buffer;
	tampered[2 * CHUNK_SIZE] ^= 1;
	REQUIRE(!SignatureVerifies(*db, tampered));

	// a chunk that cannot be read fails the check with the read error
	SignedFileSystem unreadable_chunk(buffer, 1);
	RequireReadError(*db, unreadable_chunk);
	RequireReadError(*no_worker_db, unreadable_chunk);

#ifdef SIGNATURE_CHECK_TEST_HAS_DEFAULT_PTHREAD_ATTRIBUTES
	NoMoreThreads no_more_threads;
	if (!no_more_threads.Works()) {
		WARN("the thread limit of this test does not take effect here, so the checks under it are skipped");
		return;
	}
	// the checks start no thread, so they verify the signature under the limit instead of terminating the process
	REQUIRE(SignatureVerifies(*db, buffer));
	REQUIRE(SignatureVerifies(*no_worker_db, buffer));
	REQUIRE(FileSignatureVerifies(*db, readable_file));
#endif
}
