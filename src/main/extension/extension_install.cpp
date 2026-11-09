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
#include "duckdb/main/extension/external_extension_provider.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_install_info.hpp"
#include "duckdb/main/extension_repository_manager.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/windows_undefs.hpp"

#include <fstream>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Install Extension
//===--------------------------------------------------------------------===//
const string ExtensionHelper::NormalizeVersionTag(const string &version_tag) {
	if (!version_tag.empty() && version_tag[0] != 'v') {
		return "v" + version_tag;
	}
	return version_tag;
}

bool ExtensionHelper::IsRelease(const string &version_tag) {
	return VersioningUtils::IsReleaseVersion(version_tag);
}

const string ExtensionHelper::GetVersionDirectoryName() {
#ifdef DUCKDB_WASM_VERSION
	return DUCKDB_QUOTE_DEFINE(DUCKDB_WASM_VERSION);
#endif
	if (IsRelease(DuckDB::LibraryVersion())) {
		return NormalizeVersionTag(DuckDB::LibraryVersion());
	} else {
		return DuckDB::SourceID();
	}
}

const vector<string> ExtensionHelper::PathComponents() {
	return vector<string> {GetVersionDirectoryName(), DuckDB::Platform()};
}

string ExtensionHelper::ExtensionInstallDocumentationLink(const string &extension_name) {
	auto components = PathComponents();

	string link = "https://duckdb.org/docs/current/extensions/troubleshooting";

	if (components.size() >= 2) {
		link += "?version=" + components[0] + "&platform=" + components[1] + "&extension=" + extension_name;
	}

	return link;
}

vector<duckdb::string> ExtensionHelper::DefaultExtensionFolders(FileSystem &fs) {
	vector<duckdb::string> default_folders;
// These fallbacks are necessary if the user doesn't use the CMake build.
#ifndef DUCKDB_EXTENSION_DIRECTORIES
#ifdef _WIN32
#define DUCKDB_EXTENSION_DIRECTORIES "~\\.duckdb\\extensions"
#else
#define DUCKDB_EXTENSION_DIRECTORIES "~/.duckdb/extensions"
#endif
#endif
	string dirs_string(DUCKDB_EXTENSION_DIRECTORIES);

	// Skip if empty
	if (dirs_string.empty()) {
		return default_folders;
	}

	// Split the string by separator
	auto directories = StringUtil::Split(dirs_string, ';');

	for (auto &dir : directories) {
		// Skip empty directories
		if (dir.empty()) {
			continue;
		}

		default_folders.push_back(dir);
	}

	return default_folders;
}

vector<string> ExtensionHelper::GetExtensionDirectoryPath(ClientContext &context) {
	auto &db = DatabaseInstance::GetDatabase(context);
	auto &fs = FileSystem::GetFileSystem(context);
	return GetExtensionDirectoryPath(db, fs);
}

vector<string> ExtensionHelper::GetExtensionDirectoryPath(DatabaseInstance &db, FileSystem &fs) {
	vector<string> extension_directories;
	auto &config = db.config;

	auto custom_extension_directory = Settings::Get<ExtensionDirectorySetting>(config);
	if (!custom_extension_directory.empty()) {
		extension_directories.push_back(custom_extension_directory);
	}

	// Add all configured extension directories
	for (auto &dir : config.GetExtensionDirectories()) {
		extension_directories.push_back(std::move(dir));
	}
	if (extension_directories.empty()) {
		// Add default extension directory if no custom directories configured
		for (const auto &default_dir : ExtensionHelper::DefaultExtensionFolders(fs)) {
			extension_directories.push_back(default_dir);
		}
	}

	// Process all directories with common path operations
	auto path_components = PathComponents();
	for (auto &extension_directory : extension_directories) {
		// convert random separators to platform-canonic
		extension_directory = fs.ConvertSeparators(extension_directory);
		// expand ~ in extension directory
		extension_directory = fs.ExpandPath(extension_directory);

		// Add path components (version and platform)
		for (auto &path_ele : path_components) {
			extension_directory = fs.JoinPath(extension_directory, path_ele);
		}
	}

	return extension_directories;
}

bool ExtensionHelper::IsInstalledExtensionFile(const string &path) {
	// an install writes <name>.duckdb_extension.tmp-<uuid>.duckdb_extension before it moves the file in place
	return StringUtil::EndsWith(path, ".duckdb_extension") && !StringUtil::Contains(path, ".duckdb_extension.tmp-");
}

string ExtensionHelper::ExtensionDirectory(DatabaseInstance &db, FileSystem &fs) {
#ifdef WASM_LOADABLE_EXTENSIONS
	throw PermissionException("ExtensionDirectory functionality is not supported in duckdb-wasm");
#endif
	auto extension_directories = GetExtensionDirectoryPath(db, fs);
	// TODO: This should never be the case given the implementation of GetExtensionDirectoryPath
	// should we still keep this check?
	D_ASSERT(!extension_directories.empty());

	string extension_directory = extension_directories[0]; // Use first/primary directory
	{
		if (!fs.DirectoryExists(extension_directory)) {
			string home_directory = fs.GetHomeDirectory();
			if (extension_directory.rfind(home_directory, 0) == 0 && !fs.DirectoryExists(home_directory)) {
				throw IOException("Can't find the home directory at '%s'\nSpecify a home directory using the SET "
				                  "home_directory='/path/to/dir' option.",
				                  home_directory);
			}
			fs.CreateDirectoriesRecursive(extension_directory);
		}
	}
	D_ASSERT(fs.DirectoryExists(extension_directory));

	return extension_directory;
}

string ExtensionHelper::ExtensionDirectory(ClientContext &context) {
	auto &db = DatabaseInstance::GetDatabase(context);
	auto &fs = FileSystem::GetFileSystem(context);
	return ExtensionDirectory(db, fs);
}

bool ExtensionHelper::CreateSuggestions(const string &extension_name, string &message) {
	auto lowercase_extension_name = StringUtil::Lower(extension_name);
	vector<string> candidates;
	for (idx_t ext_count = ExtensionHelper::DefaultExtensionCount(), i = 0; i < ext_count; i++) {
		candidates.emplace_back(ExtensionHelper::GetDefaultExtension(i).name);
	}
	for (idx_t ext_count = ExtensionHelper::ExtensionAliasCount(), i = 0; i < ext_count; i++) {
		candidates.emplace_back(ExtensionHelper::GetInternalExtensionAlias(i).alias);
	}
	auto closest_extensions = StringUtil::TopNJaroWinkler(candidates, lowercase_extension_name);
	message = StringUtil::CandidatesMessage(closest_extensions, "Candidate extensions");
	for (auto &closest : closest_extensions) {
		if (closest == lowercase_extension_name) {
			message = "Extension \"" + extension_name + "\" is an existing extension.\n";
			return true;
		}
	}
	return false;
}

unique_ptr<ExtensionInstallInfo> ExtensionHelper::InstallExtension(DatabaseInstance &db, FileSystem &fs,
                                                                   const string &extension,
                                                                   ExtensionInstallOptions &options) {
#ifdef WASM_LOADABLE_EXTENSIONS
	// Install is currently a no-op
	return nullptr;
#endif
	string local_path = ExtensionDirectory(db, fs);
	return InstallExtensionInternal(db, fs, local_path, extension, options);
}

unique_ptr<ExtensionInstallInfo> ExtensionHelper::InstallExtension(ClientContext &context, const string &extension,
                                                                   ExtensionInstallOptions &options) {
#ifdef WASM_LOADABLE_EXTENSIONS
	// Install is currently a no-op
	return nullptr;
#endif
	auto &db = DatabaseInstance::GetDatabase(context);
	auto &fs = FileSystem::GetFileSystem(context);
	string local_path = ExtensionDirectory(context);
	return InstallExtensionInternal(db, fs, local_path, extension, options, context);
}

string ExtensionHelper::ExtensionUrlTemplate(optional_ptr<const DatabaseInstance> db,
                                             const ExtensionRepository &repository, const string &version) {
	string versioned_path;
	if (!version.empty()) {
		versioned_path = "/${NAME}/" + version + "/${REVISION}/${PLATFORM}/${NAME}.duckdb_extension";
	} else {
		versioned_path = "/${REVISION}/${PLATFORM}/${NAME}.duckdb_extension";
	}
#ifdef WASM_LOADABLE_EXTENSIONS
	versioned_path = versioned_path + ".wasm";
#else
	versioned_path = versioned_path + CompressionExtensionFromType(FileCompressionType::GZIP);
#endif
	string url_template = repository.path + versioned_path;
	return url_template;
}

string ExtensionHelper::ExtensionFinalizeUrlTemplate(const string &url_template, const string &extension_name) {
	auto url = StringUtil::Replace(url_template, "${REVISION}", GetVersionDirectoryName());
	url = StringUtil::Replace(url, "${PLATFORM}", DuckDB::Platform());
	url = StringUtil::Replace(url, "${NAME}", extension_name);
	return url;
}

unique_ptr<ExtensionInstallInfo> ExtensionHelper::InstallExtensionInternal(DatabaseInstance &db, FileSystem &fs,
                                                                           const string &local_path,
                                                                           const string &extension,
                                                                           ExtensionInstallOptions &options,
                                                                           optional_ptr<ClientContext> context) {
	return DBConfig::GetConfig(db).GetExternalExtensionProvider().Install(db, fs, local_path, extension, options,
	                                                                      context);
}

} // namespace duckdb
