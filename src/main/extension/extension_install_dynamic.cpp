#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/main/http/http_transport_manager.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/main/database_file_opener.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_install_info.hpp"
#include "duckdb/main/extension_repository_manager.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/windows_undefs.hpp"

#include <fstream>

namespace duckdb {

static unsafe_unique_array<data_t> ReadExtensionFileFromDisk(FileSystem &fs, const string &path, idx_t &file_size) {
	auto source_file = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	file_size = source_file->GetFileSize();
	auto in_buffer = make_unsafe_uniq_array<data_t>(file_size);
	source_file->Read(QueryContext(), in_buffer.get(), file_size, 0);
	source_file->Close();
	return in_buffer;
}

static void WriteExtensionFileToDisk(QueryContext &query_context, FileSystem &fs, const string &path, void *data,
                                     idx_t data_size, DatabaseInstance &db, ExtensionInstallInfo &info) {
	if (!Settings::Get<AllowUnsignedExtensionsSetting>(db)) {
		string signature_key_fingerprint;
		const bool signature_valid = ExtensionHelper::CheckExtensionBufferSignature(
		    db, static_cast<char *>(data), data_size, info.repository_type, info.repository_name,
		    &signature_key_fingerprint);
		if (!signature_valid) {
			throw IOException("Attempting to install an extension file that doesn't have a valid signature, see "
			                  "https://duckdb.org/docs/current/operations_manual/securing_duckdb/securing_extensions");
		}
		// record which trusted key signed the extension, so it can be surfaced through duckdb_extensions()
		info.signature_key_fingerprint = signature_key_fingerprint;
	}

	// Now signature has been checked (if signature checking is enabled)

	// Open target_file, at this points ending with '.duckdb_extension'
	auto target_file =
	    fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_APPEND |
	                          FileFlags::FILE_FLAGS_FILE_CREATE_NEW | FileFlags::FILE_FLAGS_ENABLE_EXTENSION_INSTALL);
	// Write content to the file
	target_file->Write(query_context, data, data_size);

	target_file->Close();
	target_file.reset();
}

static void WriteExtensionMetadataFileToDisk(FileSystem &fs, const string &path, ExtensionInstallInfo &metadata) {
	// the metadata file records which repository the extension came from, and thereby which keys may have signed it,
	// so it is part of the extension trust domain and reserved from other writers
	auto file_writer = BufferedFileWriter(fs, path,
	                                      FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
	                                          FileFlags::FILE_FLAGS_ENABLE_EXTENSION_INSTALL);
	BinarySerializer::Serialize(metadata, file_writer);
	file_writer.Sync();
}

static void CheckExtensionMetadataOnInstall(DatabaseInstance &db, void *in_buffer, idx_t file_size,
                                            ExtensionInstallInfo &info, const string &extension_name) {
	if (file_size < ParsedExtensionMetaData::FOOTER_SIZE) {
		throw IOException("Failed to install '%s', file too small to be a valid DuckDB extension!", extension_name);
	}

	auto parsed_metadata = ExtensionHelper::ParseExtensionMetaData(static_cast<char *>(in_buffer) +
	                                                               (file_size - ParsedExtensionMetaData::FOOTER_SIZE));

	auto metadata_mismatch_error = parsed_metadata.GetInvalidMetadataError();

	if (!metadata_mismatch_error.empty() && !Settings::Get<AllowExtensionsMetadataMismatchSetting>(db)) {
		throw IOException("Failed to install '%s'\n%s", extension_name, metadata_mismatch_error);
	}

	info.version = parsed_metadata.extension_version;
}

// Note: since this method is not atomic, this can fail in different ways, that should all be handled properly by
// DuckDB:
//   1. Crash after extension removal: extension is now uninstalled, metadata file still present
//   2. Crash after metadata removal: extension is now uninstalled, extension dir is clean
//   3. Crash after extension move: extension is now uninstalled, new metadata file present
static void WriteExtensionFiles(QueryContext &query_context, FileSystem &fs, const string &temp_path,
                                const string &local_extension_path, void *in_buffer, idx_t file_size,
                                ExtensionInstallInfo &info, DatabaseInstance &db) {
	// temp_path ends with '.duckdb_extension'
	if (!StringUtil::EndsWith(temp_path, ".duckdb_extension")) {
		throw InternalException("Extension install temp_path of '%s' is not valid, should end in '.duckdb_extension'",
		                        temp_path);
	}
	// local_extension_path ends with '.duckdb_extension', and given it will be written only after signature checks,
	// it's now loadable
	if (!StringUtil::EndsWith(local_extension_path, ".duckdb_extension")) {
		throw InternalException("Extension install local_extension_path of '%s' is not valid, should end in "
		                        "'.duckdb_extension'",
		                        temp_path);
	}

	// Write extension to tmp file - the repository the extension comes from determines which keys are trusted to
	// sign it. This also records the fingerprint of the key that signed the extension in the install info
	WriteExtensionFileToDisk(query_context, fs, temp_path, in_buffer, file_size, db, info);
	// When this exit, signature has already being checked (if enabled by config)

	// Write metadata to tmp file
	auto metadata_tmp_path = temp_path + ".info";
	auto metadata_file_path = local_extension_path + ".info";
	WriteExtensionMetadataFileToDisk(fs, metadata_tmp_path, info);

	fs.MoveFile(metadata_tmp_path, metadata_file_path);
	fs.MoveFile(temp_path, local_extension_path);
}

// Install an extension using a filesystem
static unique_ptr<ExtensionInstallInfo> DirectInstallExtension(DatabaseInstance &db, FileSystem &fs, const string &path,
                                                               const string &temp_path, const string &extension_name,
                                                               const string &local_extension_path,
                                                               ExtensionInstallOptions &options,
                                                               optional_ptr<ClientContext> context) {
	string extension;
	string file;
	if (fs.IsRemoteFile(path, extension)) {
		file = path;
		// Try autoloading httpfs for loading extensions over https
		if (context) {
			auto &db = DatabaseInstance::GetDatabase(*context);
			if (extension == "httpfs" && !db.ExtensionIsLoaded("httpfs") &&
			    Settings::Get<AutoloadKnownExtensionsSetting>(*context)) {
				ExtensionHelper::AutoLoadExtension(*context, "httpfs");
			}
		}
	} else {
		file = fs.ConvertSeparators(path);
	}

	// Check if file exists
	bool exists = fs.FileExists(file);

	// Recheck without .gz
	if (!exists && StringUtil::EndsWith(file, CompressionExtensionFromType(FileCompressionType::GZIP))) {
		file = file.substr(0, file.size() - 3);
		exists = fs.FileExists(file);
	}

	// Throw error on failure
	if (!exists) {
		if (!fs.IsRemoteFile(file)) {
			throw IOException("Failed to install local extension \"%s\", no access to the file at PATH \"%s\"\n",
			                  extension_name, file);
		}
		if (StringUtil::StartsWith(file, "https://")) {
			throw IOException("Failed to install remote extension \"%s\" from url \"%s\"", extension_name, file);
		}
	}

	idx_t file_size;
	auto in_buffer = ReadExtensionFileFromDisk(fs, file, file_size);

	ExtensionInstallInfo info;

	string decompressed_data;
	void *extension_decompressed;
	idx_t extension_decompressed_size;

	if (GZipFileSystem::CheckIsZip(const_char_ptr_cast(in_buffer.get()), file_size)) {
		decompressed_data = GZipFileSystem::UncompressGZIPString(const_char_ptr_cast(in_buffer.get()), file_size);
		extension_decompressed = (void *)decompressed_data.data();
		extension_decompressed_size = decompressed_data.size();
	} else {
		extension_decompressed = (void *)in_buffer.get();
		extension_decompressed_size = file_size;
	}

	CheckExtensionMetadataOnInstall(db, extension_decompressed, extension_decompressed_size, info, extension_name);

	if (!options.repository) {
		info.mode = ExtensionInstallMode::CUSTOM_PATH;
		info.full_path = file;
	} else {
		info.mode = ExtensionInstallMode::REPOSITORY;
		info.full_path = file;
		info.repository_url = options.repository->path;
		info.repository_type = options.repository->type;
		info.repository_name = options.repository->name;
	}

	QueryContext query_context(context);
	WriteExtensionFiles(query_context, fs, temp_path, local_extension_path, extension_decompressed,
	                    extension_decompressed_size, info, db);

	return make_uniq<ExtensionInstallInfo>(info);
}

//! Throws if the url of an extension download is not allowed by the file access settings
static void VerifyCanDownloadExtension(DatabaseInstance &db, const string &url) {
	if (Settings::Get<EnableExternalAccessSetting>(db) || ExtensionRepository::IsOfficialRepositoryUrl(url)) {
		return;
	}
	// urls are matched against allowed_directories by prefix without being normalized, so reject dot segments
	// (also percent-encoded) that the server could resolve to a location outside of the allowed directory
	auto lower_url = StringUtil::Lower(url);
	for (auto &segment : StringUtil::Split(lower_url, '/')) {
		if (segment == "." || segment == ".." || StringUtil::Contains(segment, "%2e")) {
			throw PermissionException("Cannot download extension from \"%s\" - urls with dot segments are not "
			                          "allowed when external access is disabled",
			                          url);
		}
	}
	if (db.config.CanAccessFile(url, FileType::FILE_TYPE_REGULAR)) {
		return;
	}
	// with httpfs loaded http repositories are bumped to https: allow them if the http url is allowed
	if (StringUtil::StartsWith(url, "https://") &&
	    db.config.CanAccessFile("http://" + url.substr(8), FileType::FILE_TYPE_REGULAR)) {
		return;
	}
	throw PermissionException("Cannot download extension from \"%s\" - external access is disabled by "
	                          "configuration. Add the repository to allowed_directories to allow it",
	                          url);
}

//! Perform the GET request for an extension against a single url
static unique_ptr<HTTPResponse> RequestExtension(DatabaseInstance &db, optional_ptr<ClientContext> context,
                                                 const string &url, const HTTPHeaders &headers) {
	// the official repositories are reachable even when external access is disabled, any other url must be allowed
	// through allowed_directories or allowed_paths
	VerifyCanDownloadExtension(db, url);
	auto &manager = db.config.GetHTTPTransportManager();
	auto session = context ? manager.CreateSession(*context, url) : manager.CreateSession(db, url);
	auto &params = session.Parameters();

	// Unclear what's peculiar about extension install flow, but those two parameters are needed
	// to avoid lengthy retry on 304
	params.follow_location = false;
	params.keep_alive = false;
	// the url was checked above, which also allows the official repositories, so skip the central check
	params.skip_external_access_check = true;

	GetRequestInfo get_request(url, headers, params, nullptr, nullptr);
	get_request.try_request = true;

	return session.Request(get_request);
}

//! Whether a failed response means the server itself is unavailable, as opposed to e.g. the extension not existing
static bool ServerUnavailable(const HTTPResponse &response) {
	if (response.HasRequestError()) {
		// the request never completed: DNS failure, connection refused, timeout, ...
		return true;
	}
	return response.status >= HTTPStatusCode::InternalServerError_500;
}

//! Install an extension from a http url. If a backup url is given it is tried when the server at url is unavailable
static unique_ptr<ExtensionInstallInfo>
InstallFromHttpUrl(DatabaseInstance &db, const string &url, const string &extension_name, const string &temp_path,
                   const string &local_extension_path, ExtensionInstallOptions &options,
                   optional_ptr<ClientContext> context, const string &backup_url = string()) {
	unique_ptr<ExtensionInstallInfo> install_info;
	{
		auto &fs = FileSystem::GetLocal(db);
		if (fs.FileExists(local_extension_path + ".info")) {
			try {
				install_info =
				    ExtensionInstallInfo::TryReadInfoFile(fs, local_extension_path + ".info", extension_name);
			} catch (...) {
				if (!options.force_install) {
					// We are going to rewrite the file anyhow, so this is fine
					throw;
				}
			}
		}
	}

	HTTPHeaders headers(db);
	if (options.use_etags && install_info && !install_info->etag.empty()) {
		headers.Insert("If-None-Match", StringUtil::Format("%s", install_info->etag));
	}

	string download_url = url;
	auto response = RequestExtension(db, context, url, headers);
	if (!response->Success() && !backup_url.empty() && ServerUnavailable(*response)) {
		// the server is unavailable - retry against the backup server. If that fails as well we report the error of
		// the primary server, as that is the one the user asked for
		auto backup_response = RequestExtension(db, context, backup_url, headers);
		if (backup_response->Success()) {
			download_url = backup_url;
			response = std::move(backup_response);
		}
	}
	if (!response->Success()) {
		// if we should not retry or exceeded the number of retries - bubble up the error
		string message;
		ExtensionHelper::CreateSuggestions(extension_name, message);

		auto documentation_link = ExtensionHelper::ExtensionInstallDocumentationLink(extension_name);
		if (!documentation_link.empty()) {
			message += "\nFor more info, visit " + documentation_link;
		}
		if (response->HasRequestError()) {
			// request error - this means something went wrong performing the request
			throw IOException("Failed to download extension \"%s\" at URL \"%s\"\n%s (ERROR %s)", extension_name, url,
			                  message, response->GetRequestError());
		}
		// if this was not a request error this means the server responded - report the response status and response
		throw HTTPException(*response, "Failed to download extension \"%s\" at URL \"%s\" (HTTP %n)\n%s",
		                    extension_name, url, int(response->status), message);
	}
	if (response->status == HTTPStatusCode::NotModified_304 && install_info) {
		return install_info;
	}

	string decompressed_body;
	void *extension_data;
	idx_t extension_size;

	if (GZipFileSystem::CheckIsZip(response->body.data(), response->body.size())) {
		decompressed_body = GZipFileSystem::UncompressGZIPString(response->body);
		extension_data = (void *)decompressed_body.data();
		extension_size = decompressed_body.size();
	} else {
		extension_data = (void *)response->body.data();
		extension_size = response->body.size();
	}

	ExtensionInstallInfo info;
	CheckExtensionMetadataOnInstall(db, extension_data, extension_size, info, extension_name);
	if (response->HasHeader("ETag")) {
		info.etag = response->GetHeaderValue("ETag");
	}

	if (options.repository) {
		info.mode = ExtensionInstallMode::REPOSITORY;
		info.full_path = download_url;
		info.repository_url = options.repository->path;
		info.repository_type = options.repository->type;
		info.repository_name = options.repository->name;
	} else {
		info.mode = ExtensionInstallMode::CUSTOM_PATH;
		info.full_path = download_url;
	}

	QueryContext query_context(context);
	auto fs = FileSystem::CreateLocal();
	WriteExtensionFiles(query_context, *fs, temp_path, local_extension_path, extension_data, extension_size, info, db);

	return make_uniq<ExtensionInstallInfo>(info);
}

// Install an extension using a hand-rolled http request
static unique_ptr<ExtensionInstallInfo> InstallFromRepository(DatabaseInstance &db, FileSystem &fs, const string &url,
                                                              const string &extension_name, const string &temp_path,
                                                              const string &local_extension_path,
                                                              ExtensionInstallOptions &options,
                                                              optional_ptr<ClientContext> context) {
	string url_template = ExtensionHelper::ExtensionUrlTemplate(db, *options.repository, options.version);
	string generated_url = ExtensionHelper::ExtensionFinalizeUrlTemplate(url_template, extension_name);

	// Special handling for http repository: avoid using regular filesystem (note: the filesystem is not used here)
	if (HTTPUtil::IsHTTPProtocol(options.repository->path)) {
		// The core and community repositories have backup servers that serve the same binaries - fall back to those
		// if the primary server is unavailable. Custom repositories have no backup, so there the error surfaces
		// directly
		string backup_url;
		auto backup_repository_url = ExtensionRepository::TryGetBackupRepositoryUrl(options.repository->path);
		if (!backup_repository_url.empty()) {
			backup_url = StringUtil::Replace(generated_url, options.repository->path, backup_repository_url);
		}
		if (db.ExtensionIsLoaded("httpfs")) {
			HTTPUtil::BumpToSecureProtocol(generated_url);
			HTTPUtil::BumpToSecureProtocol(backup_url);
		}
		return InstallFromHttpUrl(db, generated_url, extension_name, temp_path, local_extension_path, options, context,
		                          backup_url);
	}

	// Default case, let the FileSystem figure it out
	return DirectInstallExtension(db, fs, generated_url, temp_path, extension_name, local_extension_path, options,
	                              context);
}

static void ThrowErrorOnMismatchingExtensionOrigin(FileSystem &fs, const string &local_extension_path,
                                                   const string &extension_name, const string &extension,
                                                   optional_ptr<ExtensionRepository> repository) {
	auto install_info = ExtensionInstallInfo::TryReadInfoFile(fs, local_extension_path + ".info", extension_name);

	string format_string = "Installing extension '%s' failed. The extension is already installed "
	                       "but the origin is different.\n"
	                       "Currently installed extension is from %s '%s', while the extension to be "
	                       "installed is from %s '%s'.\n"
	                       "To solve this rerun this command with `FORCE INSTALL`";
	string repo = "repository";
	string custom_path = "custom_path";

	if (install_info) {
		if (install_info->mode == ExtensionInstallMode::REPOSITORY && repository &&
		    install_info->repository_url != repository->path) {
			throw InvalidInputException(format_string, extension_name, repo, install_info->repository_url, repo,
			                            repository->path);
		}
		if (install_info->mode == ExtensionInstallMode::REPOSITORY && ExtensionHelper::IsFullPath(extension)) {
			throw InvalidInputException(format_string, extension_name, repo, install_info->repository_url, custom_path,
			                            extension);
		}
	}
}

unique_ptr<ExtensionInstallInfo> InstallExternalExtension(DatabaseInstance &db, FileSystem &fs,
                                                          const string &local_path, const string &extension,
                                                          ExtensionInstallOptions &options,
                                                          optional_ptr<ClientContext> context) {
	auto extension_name = ExtensionHelper::ApplyExtensionAlias(fs.ExtractBaseName(extension));

	if (ExtensionHelper::IsFullPath(extension) && options.repository) {
		throw InvalidInputException("Cannot pass both a repository and a full path url");
	}

	// Resolve default repository if there is none set
	ExtensionRepository resolved_repository;
	if (!ExtensionHelper::IsFullPath(extension) && !options.repository) {
		resolved_repository = ExtensionRepository::GetDefaultRepository(db.config);
		options.repository = resolved_repository;
	}

	// User-provided repositories install into a per-repository subfolder to avoid on-disk name collisions between
	// repositories. Core and community extensions keep the flat top-level layout for backwards compatibility
	string install_path = local_path;
	if (options.repository && options.repository->type == ExtensionRepositoryType::USER_PROVIDED) {
		// installing native code from a user-provided repository trusts that repository's signing keys, so it requires
		// the same explicit opt-in as adding one. Without this, pointing extension_repository_directory at an
		// attacker-controlled directory and installing from it would bypass the opt-in entirely
		if (ExtensionRepositoryManager::GetAccess(db) != ExtensionRepositoryAccess::ALLOWED) {
			throw PermissionException("Installing extensions from a user-provided repository requires "
			                          "allow_extension_repositories='allowed'");
		}
		install_path = fs.JoinPath(fs.JoinPath(local_path, "repositories"), options.repository->name);
		FileSystem::GetLocal(db).CreateDirectoriesRecursive(install_path);
	}

	string local_extension_path = fs.JoinPath(install_path, extension_name + ".duckdb_extension");
	string temp_path =
	    local_extension_path + ".tmp-" + UUID::ToString(UUID::GenerateRandomUUID()) + ".duckdb_extension";

	if (fs.FileExists(local_extension_path) && !options.force_install) {
		// File exists: throw error if origin mismatches
		if (options.throw_on_origin_mismatch && !Settings::Get<AllowExtensionsMetadataMismatchSetting>(db) &&
		    fs.FileExists(local_extension_path + ".info")) {
			ThrowErrorOnMismatchingExtensionOrigin(fs, local_extension_path, extension_name, extension,
			                                       options.repository);
		}

		// File exists, but that's okay, install is now a NOP
		return nullptr;
	}

	fs.TryRemoveFile(temp_path);

	// Install extension from local, direct url
	if (ExtensionHelper::IsFullPath(extension) && !FileSystem::IsRemoteFile(extension)) {
		auto &local_fs = FileSystem::GetLocal(db);
		return DirectInstallExtension(db, local_fs, extension, temp_path, extension, local_extension_path, options,
		                              context);
	}

	// Install extension from local url based on a repository (Note that this will install it as a local file)
	if (options.repository && !FileSystem::IsRemoteFile(options.repository->path)) {
		auto &local_fs = FileSystem::GetLocal(db);
		return InstallFromRepository(db, local_fs, extension, extension_name, temp_path, local_extension_path, options,
		                             context);
	}

#ifdef DISABLE_DUCKDB_REMOTE_INSTALL
	throw BinderException("Remote extension installation is disabled through configuration");
#else

	// Full path direct installation
	if (ExtensionHelper::IsFullPath(extension)) {
		if (StringUtil::StartsWith(extension, "http://")) {
			// HTTP takes separate path to avoid dependency on httpfs extension
			return InstallFromHttpUrl(db, extension, extension_name, temp_path, local_extension_path, options, context);
		}

		// Direct installation from local or remote path
		return DirectInstallExtension(db, fs, extension, temp_path, extension, local_extension_path, options, context);
	}

	// Repository installation
	return InstallFromRepository(db, fs, extension, extension_name, temp_path, local_extension_path, options, context);
#endif
}

} // namespace duckdb
