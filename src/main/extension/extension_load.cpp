#include "duckdb.h"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/capi/extension_api.hpp"
#include "duckdb/main/capi_v2/extension_load_v2.hpp"
#include "duckdb/main/error_manager.hpp"
#include "duckdb/main/extension/external_extension_provider.hpp"
#include "duckdb/main/extension/linked_extension_registry.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_manager.hpp"
#include "duckdb/main/extension_repository_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "mbedtls_wrapper.hpp"

#ifndef DUCKDB_NO_THREADS
#include <thread>
#endif // DUCKDB_NO_THREADS

namespace duckdb {

//===--------------------------------------------------------------------===//
// Extension C API
//===--------------------------------------------------------------------===//

//! Which C API family an extension targets. C_STRUCT_UNSTABLE always means V2: everything that was unstable in the V1
//! API was stabilized into v1.5.6, so a V1 extension pins that version rather than building against "unstable".
static bool UsesCAPIV2(const ExtensionInitResult &init_result) {
	if (init_result.abi_type == ExtensionABIType::C_STRUCT_UNSTABLE) {
		return true;
	}
	if (init_result.abi_type != ExtensionABIType::C_STRUCT) {
		return false;
	}
	return VersioningUtils::IsCAPIV2Version(init_result.duckdb_capi_version);
}

//! State that is kept during the load phase of a C API extension
struct DuckDBExtensionLoadState {
	explicit DuckDBExtensionLoadState(DatabaseInstance &db_p, ExtensionInitResult &init_result_p)
	    : db(db_p), init_result(init_result_p), database_data(nullptr) {
	}

	//! Create a DuckDBExtensionLoadState reference from a C API opaque pointer
	static DuckDBExtensionLoadState &Get(duckdb_extension_info info) {
		D_ASSERT(info);

		return *reinterpret_cast<duckdb::DuckDBExtensionLoadState *>(info);
	}

	//! Convert to an opaque C API pointer
	duckdb_extension_info ToCStruct() {
		return reinterpret_cast<duckdb_extension_info>(this);
	}

	//! Ref to the database being loaded
	DatabaseInstance &db;

	//! The init result from initializing the extension
	ExtensionInitResult &init_result;

	//! The borrowed duckdb_database passed to the extension during initialization. Replaced on each GetDatabase call
	//! and destroyed when the entrypoint returns; the extension must not retain or free it.
	unique_ptr<DatabaseWrapper> database_data;

	//! The function pointer struct passed to the extension. The extension is expected to copy this struct during
	//! initialization
	duckdb_ext_api_v1 api_struct;

	//! Error handling
	bool has_error = false;
	//! The stored error from the loading process
	ErrorData error_data;
};

//! Contains the callbacks that are passed to CAPI extensions to allow initialization
struct ExtensionAccess {
	//! Create the struct of function pointers to pass to the extension for initialization
	static duckdb_extension_access CreateAccessStruct() {
		return {SetError, GetDatabase, GetAPI};
	}

	//! Called by the extension to indicate failure to initialize the extension
	static void SetError(duckdb_extension_info info, const char *error) {
		auto &load_state = DuckDBExtensionLoadState::Get(info);

		load_state.has_error = true;
		load_state.error_data =
		    error ? ErrorData(error)
		          : ErrorData(ExceptionType::UNKNOWN_TYPE, "Extension has indicated an error occurred during "
		                                                   "initialization, but did not set an error message.");
	}

	//! Called by the extension get a pointer to the database that is loading it
	static duckdb_database *GetDatabase(duckdb_extension_info info) {
		auto &load_state = DuckDBExtensionLoadState::Get(info);

		try {
			// Create the duckdb_database
			load_state.database_data = make_uniq<DatabaseWrapper>();
			load_state.database_data->database = make_shared_ptr<DuckDB>(load_state.db);
			return reinterpret_cast<duckdb_database *>(load_state.database_data.get());
		} catch (std::exception &ex) {
			load_state.has_error = true;
			load_state.error_data = ErrorData(ex);
			return nullptr;
		} catch (...) {
			load_state.has_error = true;
			load_state.error_data =
			    ErrorData(ExceptionType::UNKNOWN_TYPE, "Unknown error in GetDatabase when trying to load extension!");
			return nullptr;
		}
	}

	//! Called by the extension get a pointer the correctly versioned extension C API struct.
	static const void *GetAPI(duckdb_extension_info info, const char *version) {
		string version_string = version;
		auto &load_state = DuckDBExtensionLoadState::Get(info);

		// Only reached for V1 extensions: C_STRUCT_UNSTABLE now selects the V2 entrypoint, which has its own get_api
		if (load_state.init_result.abi_type == ExtensionABIType::C_STRUCT) {
			idx_t major, minor, patch;
			auto parsed = VersioningUtils::ParseSemver(version_string, major, minor, patch);

			if (!parsed || major != DUCKDB_EXTENSION_API_VERSION_MAJOR ||
			    !VersioningUtils::IsSupportedCAPIVersion(major, minor, patch)) {
				load_state.has_error = true;
				load_state.error_data = ErrorData(
				    ExceptionType::UNKNOWN_TYPE,
				    "Unsupported C CAPI version detected during extension initialization: " + string(version));
				return nullptr;
			}
		} else {
			load_state.has_error = true;
			load_state.error_data =
			    ErrorData(ExceptionType::UNKNOWN_TYPE,
			              StringUtil::Format("Unknown ABI Type of value '%d' found when loading extension '%s'",
			                                 static_cast<uint8_t>(load_state.init_result.abi_type),
			                                 load_state.init_result.filename));
			return nullptr;
		}

		load_state.api_struct = load_state.db.GetExtensionAPIV1();
		return &load_state.api_struct;
	}
};

//===--------------------------------------------------------------------===//
// Static C API Extension Loading
//===--------------------------------------------------------------------===//
void DuckDB::LoadStaticExtension(duckdb_extension_describe_t describe) {
	StaticExtensionDescription description;
	auto error = LinkedExtensionRegistry::Describe(describe, description);
	if (!error.empty()) {
		throw InvalidInputException("Failed to load statically linked extension: %s", error);
	}
	if (description.descriptor.database_callback) {
		reinterpret_cast<void (*)(DatabaseInstance &)>(description.descriptor.database_callback)(*instance);
		return;
	}
	// C++ first, then C API v2, then C API v1
	auto &descriptor = description.descriptor;
	if (!descriptor.entry_cpp) {
		if (descriptor.entry_capi_v2) {
			LoadStaticCAPIExtensionV2(description.name,
			                          reinterpret_cast<ext_init_c_api_v2_fun_t>(descriptor.entry_capi_v2));
		} else {
			LoadStaticCAPIExtension(description.name, reinterpret_cast<ext_init_c_api_fun_t>(descriptor.entry_capi_v1));
		}
		return;
	}
	auto &manager = ExtensionManager::Get(*instance);
	auto load_info = manager.BeginLoad({description.name});
	if (!load_info) {
		// already loaded
		return;
	}

	ExtensionLoader loader(*load_info);
	(*reinterpret_cast<ext_init_cpp_fun_t>(descriptor.entry_cpp))(loader);
	loader.FinalizeLoad();

	ExtensionInstallInfo install_info;
	install_info.mode = ExtensionInstallMode::STATICALLY_LINKED;
	install_info.version = description.version;
	load_info->FinishLoad(install_info);
}

void DuckDB::LoadStaticCAPIExtension(const string &name, ext_init_c_api_fun_t init_fun) {
	auto &manager = ExtensionManager::Get(*instance);
	auto load_info = manager.BeginLoad({name});
	if (!load_info) {
		// already loaded
		return;
	}

	ExtensionInitResult init_result;
	init_result.filename = name;
	init_result.filebase = name;
	// Statically compiled extensions are always tied to the exact DuckDB version
	init_result.abi_type = ExtensionABIType::C_STRUCT_UNSTABLE;
	init_result.lib_hdl = nullptr;

	DuckDBExtensionLoadState load_state(*instance, init_result);

	// For static loading, get_api is null - the extension uses direct DuckDB symbols (no vtable needed)
	duckdb_extension_access access;
	access.set_error = ExtensionAccess::SetError;
	access.get_database = ExtensionAccess::GetDatabase;
	access.get_api = nullptr;

	if (!(*init_fun)(load_state.ToCStruct(), &access)) {
		string msg = load_state.has_error ? load_state.error_data.Message() : "unknown error";
		load_info->LoadFail(ErrorData(msg));
		throw IOException("Failed to load static C API extension '%s': %s", name, msg);
	}

	ExtensionInstallInfo install_info;
	install_info.mode = ExtensionInstallMode::STATICALLY_LINKED;
	load_info->FinishLoad(install_info);
}

void DuckDB::LoadStaticCAPIExtensionV2(const string &name, ext_init_c_api_v2_fun_t init_fun) {
	auto &manager = ExtensionManager::Get(*instance);
	auto load_info = manager.BeginLoad({name});
	if (!load_info) {
		// already loaded
		return;
	}

	ExtensionInitResult init_result;
	init_result.filename = name;
	init_result.filebase = name;
	// Statically compiled extensions are always tied to the exact DuckDB version
	init_result.abi_type = ExtensionABIType::C_STRUCT_UNSTABLE;
	init_result.lib_hdl = nullptr;

	try {
		// Statically linked extensions bind DuckDB's symbols at link time, so they never fetch the vtable. There is no
		// client context this early either, so the entrypoint gets a connection of its own.
		instance->InvokeExtensionEntrypointV2(init_result, name, init_fun, nullptr, /* statically_linked */ true);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		load_info->LoadFail(error);
		throw;
	}

	ExtensionInstallInfo install_info;
	install_info.mode = ExtensionInstallMode::STATICALLY_LINKED;
	load_info->FinishLoad(install_info);
}

//===--------------------------------------------------------------------===//
// Load External Extension
//===--------------------------------------------------------------------===//
// The C++ init function
typedef void (*ext_init_fun_t)(ExtensionLoader &);
// The C init function
typedef bool (*ext_init_c_api_fun_t)(duckdb_extension_info info, duckdb_extension_access *access);

static void ComputeSHA256Buffer(const char *buffer, const idx_t start, const idx_t end, string *res) {
	// Invoke MbedTls function to actually compute sha256
	char hash[duckdb_mbedtls::MbedTlsWrapper::SHA256_HASH_LENGTH_BYTES];
	duckdb_mbedtls::MbedTlsWrapper::ComputeSha256Hash(buffer + start, end - start, hash);
	*res = std::string(hash, duckdb_mbedtls::MbedTlsWrapper::SHA256_HASH_LENGTH_BYTES);
}

static void ComputeSHA256String(const string &to_hash, string *res) {
	ComputeSHA256Buffer(to_hash.data(), 0, to_hash.length(), res);
}

static string ComputeFinalHash(const vector<string> &chunks) {
	string hash_concatenation;
	hash_concatenation.reserve(32 * chunks.size()); // 256 bits -> 32 bytes per chunk

	for (auto &chunk : chunks) {
		hash_concatenation += chunk;
	}

	string two_level_hash;
	ComputeSHA256String(hash_concatenation, &two_level_hash);

	return two_level_hash;
}

static void InitializeAncillaryData(vector<string> &hash_chunks, vector<idx_t> &splits, idx_t length) {
	const idx_t maxLenChunks = 1024ULL * 1024ULL;
	const idx_t numChunks = (length + maxLenChunks - 1) / maxLenChunks;
	hash_chunks.resize(numChunks);
	splits.resize(numChunks + 1);

	for (idx_t i = 0; i < numChunks; i++) {
		splits[i] = maxLenChunks * i;
	}
	splits.back() = length;
}

static void ComputeSHA256FileSegment(FileHandle *handle, const idx_t start, const idx_t end, string *res) {
	idx_t iter = start;
	const idx_t segment_size = 1024ULL * 8ULL;

	duckdb_mbedtls::MbedTlsWrapper::SHA256State state;

	string to_hash;
	while (iter < end) {
		idx_t len = std::min(end - iter, segment_size);
		to_hash.resize(len);
		handle->Read((void *)to_hash.data(), len, iter);

		state.AddString(to_hash);

		iter += segment_size;
	}

	*res = state.Finalize();
}

template <typename T, typename F>
static void ComputeHashesOnSegments(F ComputeHashFun, T handle, const vector<idx_t> &splits,
                                    vector<string> &hash_chunks) {
#ifndef DUCKDB_NO_THREADS
	// an exception leaving a thread would terminate the process, so each thread keeps its error for after the join
	vector<ErrorData> errors(hash_chunks.size());
	vector<std::thread> threads;
	threads.reserve(hash_chunks.size());
	for (idx_t i = 0; i < hash_chunks.size(); i++) {
		threads.emplace_back([&, i]() {
			try {
				ComputeHashFun(handle, splits[i], splits[i + 1], &hash_chunks[i]);
			} catch (std::exception &ex) {
				errors[i] = ErrorData(ex);
			}
		});
	}

	for (auto &thread : threads) {
		thread.join();
	}
	for (auto &error : errors) {
		if (error.HasError()) {
			error.Throw();
		}
	}
#else
	for (idx_t i = 0; i < hash_chunks.size(); i++) {
		ComputeHashFun(handle, splits[i], splits[i + 1], &hash_chunks[i]);
	}
#endif // DUCKDB_NO_THREADS
}

static string FilterZeroAtEnd(string s) {
	while (!s.empty() && s.back() == '\0') {
		s.pop_back();
	}
	return s;
}

ParsedExtensionMetaData ExtensionHelper::ParseExtensionMetaData(const char *metadata) noexcept {
	ParsedExtensionMetaData result;

	vector<string> metadata_field;
	for (idx_t i = 0; i < 8; i++) {
		string field = string(metadata + i * 32, 32);
		metadata_field.emplace_back(field);
	}

	std::reverse(metadata_field.begin(), metadata_field.end());

	// Fetch the magic value and early out if this is invalid: the rest will just be bogus
	result.magic_value = FilterZeroAtEnd(metadata_field[0]);
	if (!result.AppearsValid()) {
		return result;
	}

	result.platform = FilterZeroAtEnd(metadata_field[1]);

	result.extension_version = FilterZeroAtEnd(metadata_field[3]);

	auto extension_abi_metadata = FilterZeroAtEnd(metadata_field[4]);

	if (extension_abi_metadata == "C_STRUCT") {
		result.abi_type = ExtensionABIType::C_STRUCT;
		result.duckdb_capi_version = FilterZeroAtEnd(metadata_field[2]);
	} else if (extension_abi_metadata == "C_STRUCT_UNSTABLE") {
		result.abi_type = ExtensionABIType::C_STRUCT_UNSTABLE;
		result.duckdb_version = FilterZeroAtEnd(metadata_field[2]);
	} else if (extension_abi_metadata == "CPP" || extension_abi_metadata.empty()) {
		result.abi_type = ExtensionABIType::CPP;
		result.duckdb_version = FilterZeroAtEnd(metadata_field[2]);
	} else {
		result.abi_type = ExtensionABIType::UNKNOWN;
		result.duckdb_version = "unknown";
		result.extension_abi_metadata = extension_abi_metadata;
	}

	result.signature = string(metadata, ParsedExtensionMetaData::FOOTER_SIZE - ParsedExtensionMetaData::SIGNATURE_SIZE);
	return result;
}

ParsedExtensionMetaData ExtensionHelper::ParseExtensionMetaData(FileHandle &handle) {
	const string engine_version = string(ExtensionHelper::GetVersionDirectoryName());
	const string engine_platform = string(DuckDB::Platform());

	string metadata_segment;
	metadata_segment.resize(ParsedExtensionMetaData::FOOTER_SIZE);

	if (handle.GetFileSize() < ParsedExtensionMetaData::FOOTER_SIZE) {
		throw InvalidInputException(
		    "File '%s' is not a DuckDB extension. Valid DuckDB extensions must be at least %llu bytes", handle.path,
		    ParsedExtensionMetaData::FOOTER_SIZE);
	}

	handle.Read((void *)metadata_segment.data(), metadata_segment.size(),
	            handle.GetFileSize() - ParsedExtensionMetaData::FOOTER_SIZE);

	return ParseExtensionMetaData(metadata_segment.data());
}

static bool CheckKnownSignatures(DatabaseInstance &db, const string &two_level_hash, const string &signature,
                                 ExtensionRepositoryType repository_type, const string &repository_name,
                                 optional_ptr<string> signature_key_fingerprint = nullptr) {
	for (auto &key : ExtensionHelper::GetTrustedPublicKeys(db, repository_type, repository_name)) {
		if (duckdb_mbedtls::MbedTlsWrapper::IsValidSha256Signature(key, signature, two_level_hash)) {
			if (signature_key_fingerprint) {
				// report the fingerprint of the matching key in the same format as CREATE EXTENSION REPOSITORY
				auto compact_key = ExtensionRepositoryManager::ToCompactPublicKey(key);
				*signature_key_fingerprint = ExtensionRepositoryManager::GetPublicKeyFingerprint(compact_key);
			}
			return true;
		}
	}

	return false;
}

bool ExtensionHelper::CheckExtensionSignature(DatabaseInstance &db, FileHandle &handle,
                                              ParsedExtensionMetaData &parsed_metadata,
                                              ExtensionRepositoryType repository_type, const string &repository_name) {
	auto signature_offset = handle.GetFileSize() - ParsedExtensionMetaData::SIGNATURE_SIZE;

	vector<string> hash_chunks;
	vector<idx_t> splits;
	InitializeAncillaryData(hash_chunks, splits, signature_offset);

	ComputeHashesOnSegments(ComputeSHA256FileSegment, &handle, splits, hash_chunks);

	const string resulting_hash = ComputeFinalHash(hash_chunks);

	// TODO maybe we should do a stream read / hash update here
	handle.Read((void *)parsed_metadata.signature.data(), parsed_metadata.signature.size(), signature_offset);

	return CheckKnownSignatures(db, resulting_hash, parsed_metadata.signature, repository_type, repository_name);
}

bool ExtensionHelper::CheckExtensionBufferSignature(DatabaseInstance &db, const char *buffer, idx_t buffer_length,
                                                    const string &signature, ExtensionRepositoryType repository_type,
                                                    const string &repository_name,
                                                    optional_ptr<string> signature_key_fingerprint) {
	vector<string> hash_chunks;
	vector<idx_t> splits;
	InitializeAncillaryData(hash_chunks, splits, buffer_length);

	ComputeHashesOnSegments(ComputeSHA256Buffer, buffer, splits, hash_chunks);

	const string resulting_hash = ComputeFinalHash(hash_chunks);

	return CheckKnownSignatures(db, resulting_hash, signature, repository_type, repository_name,
	                            signature_key_fingerprint);
}

bool ExtensionHelper::CheckExtensionBufferSignature(DatabaseInstance &db, const char *buffer, idx_t total_buffer_length,
                                                    ExtensionRepositoryType repository_type,
                                                    const string &repository_name,
                                                    optional_ptr<string> signature_key_fingerprint) {
	auto signature_offset = total_buffer_length - ParsedExtensionMetaData::SIGNATURE_SIZE;
	string signature = std::string(buffer + signature_offset, ParsedExtensionMetaData::SIGNATURE_SIZE);

	return CheckExtensionBufferSignature(db, buffer, signature_offset, signature, repository_type, repository_name,
	                                     signature_key_fingerprint);
}

bool ExtensionHelper::TryInitialLoad(DatabaseInstance &db, FileSystem &fs, const string &extension,
                                     const string &repository_name, bool core_only, ExtensionInitResult &result,
                                     string &error) {
	auto &provider = db.config.GetExternalExtensionProvider();
	if (!provider.SupportsExternalExtensions()) {
		throw PermissionException("Loading external extensions is not supported: this build does not link the "
		                          "loadable_extensions library");
	}
	if (!Settings::Get<EnableExternalAccessSetting>(db)) {
		throw PermissionException("Loading external extensions is disabled through configuration");
	}
	auto filename = fs.ConvertSeparators(extension);

	bool direct_load;

	// resolve the FROM repository (LOAD httpfs FROM core, LOAD x FROM community, LOAD x FROM myrepo). A bare LOAD keeps
	// the flat top-level layout and only resolves core and community extensions; user-provided repositories live in a
	// per-repository subfolder, so their extensions require an explicit FROM to be found
	bool expect_user_repo = false;
	ExtensionRepositoryType expected_repository_type = ExtensionRepositoryType::CORE;
	if (!repository_name.empty()) {
		if (ExtensionHelper::IsFullPath(extension)) {
			error = "Cannot combine a FROM repository with a file path";
			return false;
		}
		ExtensionRepository repository;
		if (ExtensionRepositoryManager::TryGetRepository(db, fs, repository_name, repository)) {
			expected_repository_type = repository.type;
			expect_user_repo = repository.type == ExtensionRepositoryType::USER_PROVIDED;
		} else if (ExtensionRepository::TryGetKnownRepository(repository_name, repository)) {
			expected_repository_type = repository.type;
		} else {
			error = StringUtil::Format("'%s' is not a known extension repository", repository_name);
			return false;
		}
	}

	// shorthand case
	if (!ExtensionHelper::IsFullPath(extension)) {
		direct_load = false;
		string extension_name = ApplyExtensionAlias(extension);
#ifdef WASM_LOADABLE_EXTENSIONS
		string url_template = ExtensionUrlTemplate(&config, "");
		string url = ExtensionFinalizeUrlTemplate(url_template, extension_name);

		char *str = (char *)EM_ASM_PTR(
		    {
			    var jsString = ((typeof runtime == 'object') && runtime && (typeof runtime.whereToLoad == 'function') &&
			                    runtime.whereToLoad)
			                       ? runtime.whereToLoad(UTF8ToString($0))
			                       : (UTF8ToString($1));
			    var lengthBytes = lengthBytesUTF8(jsString) + 1;
			    // 'jsString.length' would return the length of the string as UTF-16
			    // units, but Emscripten C strings operate as UTF-8.
			    var stringOnWasmHeap = _malloc(lengthBytes);
			    stringToUTF8(jsString, stringOnWasmHeap, lengthBytes);
			    return stringOnWasmHeap;
		    },
		    filename.c_str(), url.c_str());
		string address(str);
		free(str);

		filename = address;
#else

		// Local function to process local path
		auto ComputeLocalExtensionPath = [&](const string &base_path, const string &extension_name) -> string {
			// convert random separators to platform-canonic
			string local_path = fs.ConvertSeparators(base_path);
			// expand ~ in extension directory
			local_path = fs.ExpandPath(local_path);
			auto path_components = PathComponents();
			for (auto &path_ele : path_components) {
				local_path = fs.JoinPath(local_path, path_ele);
			}
			if (expect_user_repo) {
				local_path = fs.JoinPath(fs.JoinPath(local_path, "repositories"), repository_name);
			}
			return fs.JoinPath(local_path, extension_name + ".duckdb_extension");
		};

		// Collect all directories to search for extensions
		vector<string> search_directories;
		auto custom_extension_directory = Settings::Get<ExtensionDirectorySetting>(db);
		if (!custom_extension_directory.empty()) {
			search_directories.push_back(custom_extension_directory);
		}

		// Add all configured extension directories
		for (auto &dir : db.config.GetExtensionDirectories()) {
			search_directories.push_back(std::move(dir));
		}

		// Add default extension directory if no custom directories configured
		if (search_directories.empty()) {
			for (const auto &path : ExtensionHelper::DefaultExtensionFolders(fs)) {
				search_directories.push_back(path);
			}
		}

		// Try each directory in sequence until extension is found
		bool found = false;
		for (const auto &directory : search_directories) {
			filename = ComputeLocalExtensionPath(directory, extension_name);
			if (fs.FileExists(filename)) {
				found = true;
				break;
			}
		}

		// If not found in any directory, use the first directory for error reporting
		if (!found) {
			filename = ComputeLocalExtensionPath(search_directories[0], extension_name);
		}
#endif
	} else {
		direct_load = true;
		filename = fs.ExpandPath(filename);
	}
	if (!StringUtil::EndsWith(filename, ".duckdb_extension")) {
		throw PermissionException(
		    "DuckDB extensions are files ending with '.duckdb_extension', loading different "
		    "files is not possible, error while loading from '%s', consider 'INSTALL <path>; LOAD <name>;'",
		    filename);
	}
	if (!fs.FileExists(filename)) {
		string message;
		bool exact_match = ExtensionHelper::CreateSuggestions(extension, message);
		if (exact_match) {
			message += "\nInstall it first using \"INSTALL " + extension + "\".";
		}
		error = StringUtil::Format("Extension \"%s\" not found.\n%s", filename, message);
		return false;
	}

	auto handle = fs.OpenFile(filename, FileFlags::FILE_FLAGS_READ);

	// Parse the extension metadata from the extension binary
	auto parsed_metadata = ParseExtensionMetaData(*handle);

	auto metadata_mismatch_error = parsed_metadata.GetInvalidMetadataError();

	if (!metadata_mismatch_error.empty()) {
		metadata_mismatch_error = StringUtil::Format("Failed to load '%s', %s", extension, metadata_mismatch_error);
	}

	auto filebase = fs.ExtractBaseName(filename);
	auto lowercase_extension_name = StringUtil::Lower(filebase);

	// The install info tells us where the extension came from, which determines the keys that are trusted to sign it.
	// Extensions that are loaded directly have no install info: those are verified against the core keys
	unique_ptr<ExtensionInstallInfo> install_info;
	if (!direct_load) {
		install_info = ExtensionInstallInfo::TryReadInfoFile(fs, filename + ".info", lowercase_extension_name);
	}

	// a load with an explicit FROM (LOAD x FROM core, LOAD x FROM myrepo) asserts that the extension really came from
	// that repository. This prevents loading an extension under a repository whose signing keys the user did not
	// intend to trust
	if (!repository_name.empty() && install_info) {
		bool origin_matches = expect_user_repo
		                          ? (install_info->repository_type == ExtensionRepositoryType::USER_PROVIDED &&
		                             install_info->repository_name == repository_name)
		                          : (install_info->repository_type == expected_repository_type);
		if (!origin_matches) {
			error =
			    StringUtil::Format("Extension '%s' was not installed from repository '%s'", extension, repository_name);
			return false;
		}
	}

	if (!Settings::Get<AllowUnsignedExtensionsSetting>(db)) {
		bool signature_valid;
		// The recorded origin (from the .info) determines which keys could have signed the extension, but without an
		// explicit FROM a bare load - which is also how autoloading loads - trusts only the fixed core/community keys,
		// never a user-provided repository's own keys. So an extension a repository merely redeploys still loads if it
		// is genuinely core-signed, while one signed by the repository's own key does not. This stops a repointed
		// extension_directory (a repository's served layout matches an extension directory's) from loading native code
		// under a user-provided key.
		bool bare_load = repository_name.empty();
		auto recorded_origin = install_info ? install_info->repository_type : ExtensionRepositoryType::CORE;
		auto trusted_origin = ExtensionHelper::ResolveTrustedSignatureOrigin(!bare_load, core_only, recorded_origin);
		auto signing_repository_name = install_info ? install_info->repository_name : string();
		if (trusted_origin != recorded_origin) {
			// forced onto the core keys - the repository's own name is no longer the trust anchor
			signing_repository_name = string();
		}
		if (parsed_metadata.AppearsValid()) {
			signature_valid =
			    CheckExtensionSignature(db, *handle, parsed_metadata, trusted_origin, signing_repository_name);
		} else {
			signature_valid = false;
		}

		if (!metadata_mismatch_error.empty()) {
			throw InvalidInputException(metadata_mismatch_error);
		}

		if (!signature_valid) {
			// an extension from a user-provided repository is trusted only when the repository is named: a plain load
			// verifies it against the core keys, so it fails here. Keep the message generic - reaching this needs an
			// unusual setup (a repointed extension_directory), and it must not spell out how to load the refused file
			if (bare_load && install_info && install_info->repository_type == ExtensionRepositoryType::USER_PROVIDED) {
				throw IOException("Extension '%s' was installed from a user-provided repository; a plain LOAD only "
				                  "loads core and community extensions. Load it explicitly from its repository.",
				                  extension);
			}
			throw IOException(db.config.error_manager->FormatException(ErrorType::UNSIGNED_EXTENSION, filename));
		}
	} else if (!Settings::Get<AllowExtensionsMetadataMismatchSetting>(db)) {
		if (!metadata_mismatch_error.empty()) {
			// Unsigned extensions AND configuration allowing n, loading allowed, mainly for
			// debugging purposes
			throw InvalidInputException(metadata_mismatch_error);
		}
	}

	auto lib_hdl = provider.OpenLibrary(filename, filebase);

	// Initialize the ExtensionInitResult
	result.filebase = lowercase_extension_name;
	result.filename = filename;
	result.lib_hdl = lib_hdl;
	result.abi_type = parsed_metadata.abi_type;
	result.duckdb_capi_version = parsed_metadata.duckdb_capi_version;

	if (!direct_load) {
		result.install_info = std::move(install_info);

		if (result.install_info->mode == ExtensionInstallMode::UNKNOWN) {
			// The info file was missing, we just set the version, since we have it from the parsed footer
			result.install_info->version = parsed_metadata.extension_version;
		}

		if (result.install_info->version != parsed_metadata.extension_version) {
			throw IOException("Metadata mismatch detected when loading extension '%s'\nPlease try reinstalling the "
			                  "extension using `FORCE INSTALL '%s'`",
			                  filename, extension);
		}
	} else {
		result.install_info = make_uniq<ExtensionInstallInfo>();
		result.install_info->mode = ExtensionInstallMode::NOT_INSTALLED;
		result.install_info->full_path = filename;
		result.install_info->version = parsed_metadata.extension_version;
	}

	return true;
}

ExtensionInitResult ExtensionHelper::InitialLoad(DatabaseInstance &db, FileSystem &fs, const string &extension,
                                                 const string &repository_name, bool core_only) {
	string error;
	ExtensionInitResult result;
	if (!TryInitialLoad(db, fs, extension, repository_name, core_only, result, error)) {
		if (!Settings::Get<AutoinstallKnownExtensionsSetting>(db) || !ExtensionHelper::AllowAutoInstall(extension)) {
			throw IOException(error);
		}
		// the extension load failed - try installing the extension, from the requested repository if one was given
		ExtensionInstallOptions options;
		ExtensionRepository repository;
		if (repository_name.empty() ||
		    !(ExtensionRepositoryManager::TryGetRepository(db, fs, repository_name, repository) ||
		      ExtensionRepository::TryGetKnownRepository(repository_name, repository))) {
			repository = GetAutoinstallRepository(db);
		}
		options.repository = repository;
		ExtensionHelper::InstallExtension(db, fs, extension, options);
		// try loading again
		if (!TryInitialLoad(db, fs, extension, repository_name, core_only, result, error)) {
			throw IOException(error);
		}
	}
	return result;
}

bool ExtensionHelper::IsFullPath(const string &extension) {
	return StringUtil::Contains(extension, ".") || StringUtil::Contains(extension, "/") ||
	       StringUtil::Contains(extension, "\\");
}

string ExtensionHelper::GetExtensionName(const string &original_name) {
	auto extension = StringUtil::Lower(original_name);
	if (!IsFullPath(extension)) {
		return ExtensionHelper::ApplyExtensionAlias(extension);
	}
	// split the name if it's a full path
	auto splits = StringUtil::Split(StringUtil::Replace(extension, "\\", "/"), '/');
	if (splits.empty()) {
		return ExtensionHelper::ApplyExtensionAlias(extension);
	}
	splits = StringUtil::Split(splits.back(), '.');
	if (splits.empty()) {
		return ExtensionHelper::ApplyExtensionAlias(extension);
	}
	return ExtensionHelper::ApplyExtensionAlias(splits.front());
}

void ExtensionHelper::LoadExternalExtension(DatabaseInstance &db, FileSystem &fs, const ExtensionLoadOptions &options,
                                            optional_ptr<ClientContext> context) {
	// Loading a second copy of an extension that is already linked into this binary is an ODR
	// violation, so ask the registry of extensions linked into this binary first.
	// Statically linked extensions are inherently core-trusted, so only take this shortcut for a bare
	// load or an explicit core namespace - never let community/x or myrepo/x resolve to a linked core extension.
	bool allow_static_shortcut = options.repository.empty() || StringUtil::CIEquals(options.repository, "core");
	if (allow_static_shortcut && !ExtensionHelper::IsFullPath(options.extension_name)) {
		auto logical_name = ExtensionHelper::GetExtensionName(options.extension_name);
		DuckDB db_wrapper(db);
		if (ExtensionHelper::LoadExtension(db_wrapper, logical_name) == ExtensionLoadResult::LOADED_EXTENSION) {
			return;
		}
	}

	auto &manager = ExtensionManager::Get(db);
	auto info = manager.BeginLoad(options);
	if (!info) {
		return;
	}
	try {
		LoadExternalExtensionInternal(db, fs, options.extension_name, options.repository, options.core_only, *info,
		                              context);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		info->LoadFail(error);
		throw;
	}
}

void ExtensionHelper::LoadExternalExtensionInternal(DatabaseInstance &db, FileSystem &fs, const string &extension,
                                                    const string &repository_name, bool core_only,
                                                    ExtensionActiveLoad &info, optional_ptr<ClientContext> context) {
	auto extension_init_result = InitialLoad(db, fs, extension, repository_name, core_only);

	// C++ ABI
	if (extension_init_result.abi_type == ExtensionABIType::CPP) {
		auto init_fun_name = extension_init_result.filebase + "_duckdb_cpp_init";
		ext_init_fun_t init_fun = reinterpret_cast<ext_init_fun_t>(
		    db.config.GetExternalExtensionProvider().TryLoadFunction(extension_init_result.lib_hdl, init_fun_name));
		if (!init_fun) {
			throw IOException("Extension '%s' did not contain the expected entrypoint function '%s'", extension,
			                  init_fun_name);
		}

		try {
			ExtensionLoader loader(info);
			(*init_fun)(loader);
			loader.FinalizeLoad();
		} catch (std::exception &e) {
			ErrorData error(e);
			throw InvalidInputException("Initialization function \"%s\" from file \"%s\" threw an exception: \"%s\"",
			                            init_fun_name, extension_init_result.filename, error.RawMessage());
		}

		D_ASSERT(extension_init_result.install_info);

		info.FinishLoad(*extension_init_result.install_info);
		return;
	}

	// C ABI, V2
	if (UsesCAPIV2(extension_init_result)) {
		auto init_fun_name = extension_init_result.filebase + "_init_c_api_v2";
		auto init_fun_capi_v2 = reinterpret_cast<ext_init_c_api_v2_fun_t>(
		    db.config.GetExternalExtensionProvider().TryLoadFunction(extension_init_result.lib_hdl, init_fun_name));

		if (!init_fun_capi_v2) {
			throw IOException(
			    "File \"%s\" did not contain function \"%s\". Extensions built against the unstable C API, or against "
			    "C API v2.x.y, must use the entrypoint from duckdb_extension_v2.h. To keep using the V1 C API, pin it "
			    "with build_loadable_extension_capi(%s 1 5 6 ...), which provides \"%s_init_c_api\" instead.",
			    extension_init_result.filename, init_fun_name, extension_init_result.filebase,
			    extension_init_result.filebase);
		}

		db.InvokeExtensionEntrypointV2(extension_init_result, extension, init_fun_capi_v2, context,
		                               /* statically_linked */ false);

		D_ASSERT(extension_init_result.install_info);

		info.FinishLoad(*extension_init_result.install_info);
		return;
	}

	// C ABI, V1
	if (extension_init_result.abi_type == ExtensionABIType::C_STRUCT) {
		auto init_fun_name = extension_init_result.filebase + "_init_c_api";
		ext_init_c_api_fun_t init_fun_capi = reinterpret_cast<ext_init_c_api_fun_t>(
		    db.config.GetExternalExtensionProvider().TryLoadFunction(extension_init_result.lib_hdl, init_fun_name));

		if (!init_fun_capi) {
			throw IOException("File \"%s\" did not contain function \"%s\": %s", extension_init_result.filename,
			                  init_fun_name, db.config.GetExternalExtensionProvider().GetLibraryError());
		}
		// Create the load state
		DuckDBExtensionLoadState load_state(db, extension_init_result);

		auto access = ExtensionAccess::CreateAccessStruct();
		auto result = (*init_fun_capi)(load_state.ToCStruct(), &access);

		// Throw any error that the extension might have encountered
		if (load_state.has_error) {
			load_state.error_data.Throw("An error was thrown during initialization of the extension '" + extension +
			                            "': ");
		}

		// Extensions are expected to either set an error or return true indicating successful initialization
		if (result == false) {
			throw FatalException(
			    "Extension '%s' failed to initialize but did not return an error. This indicates an "
			    "error in the extension: C API extensions should return a boolean `true` to indicate successful "
			    "initialization. "
			    "This means that the Extension may be partially initialized resulting in an inconsistent state of "
			    "DuckDB.",
			    extension);
		}

		D_ASSERT(extension_init_result.install_info);

		info.FinishLoad(*extension_init_result.install_info);
		return;
	}

	throw IOException("Unknown ABI type of value '%s' for extension '%s'",
	                  static_cast<uint8_t>(extension_init_result.abi_type), extension);
}

void ExtensionHelper::LoadExternalExtension(ClientContext &context, const ExtensionLoadOptions &options) {
	LoadExternalExtension(DatabaseInstance::GetDatabase(context), FileSystem::GetFileSystem(context), options, context);
}

string ExtensionHelper::ExtractExtensionPrefixFromPath(const string &path) {
	auto first_colon = path.find(':');
	if (first_colon == string::npos || first_colon < 2) { // needs to be at least two characters because windows c: ...
		return "";
	}
	auto extension = path.substr(0, first_colon);

	if (path.substr(first_colon, 3) == "://") {
		// these are not extensions
		return "";
	}

	D_ASSERT(extension.size() > 1);
	// needs to be alphanumeric
	for (auto &ch : extension) {
		if (!isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
			return "";
		}
	}
	return extension;
}

} // namespace duckdb
