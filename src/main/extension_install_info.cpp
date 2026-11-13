#include "duckdb/main/extension_install_info.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/serializer/buffered_file_reader.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"

namespace duckdb {

string ExtensionRepository::GetRepository(const string &repository_url) {
	auto resolved_repository = TryConvertUrlToKnownRepository(repository_url);
	if (resolved_repository.empty()) {
		return repository_url;
	}
	return resolved_repository;
}

//! The built-in repositories. The type determines which public keys are trusted to sign the extensions that the
//! repository serves - it is a property of the repository itself, never something that is derived from a url
struct KnownExtensionRepository {
	const char *name;
	const char *url;
	ExtensionRepositoryType type;
};

static constexpr const KnownExtensionRepository KNOWN_REPOSITORIES[] = {
    {"core", ExtensionRepository::CORE_REPOSITORY_URL, ExtensionRepositoryType::CORE},
    {"core_nightly", ExtensionRepository::CORE_NIGHTLY_REPOSITORY_URL, ExtensionRepositoryType::CORE},
    {"community", ExtensionRepository::COMMUNITY_REPOSITORY_URL, ExtensionRepositoryType::COMMUNITY},
    {"local_build_debug", ExtensionRepository::BUILD_DEBUG_REPOSITORY_PATH, ExtensionRepositoryType::CORE},
    {"local_build_release", ExtensionRepository::BUILD_RELEASE_REPOSITORY_PATH, ExtensionRepositoryType::CORE},
    {nullptr, nullptr, ExtensionRepositoryType::CORE}};

bool ExtensionRepository::TryGetKnownRepository(const string &repository, ExtensionRepository &result) {
	for (idx_t i = 0; KNOWN_REPOSITORIES[i].name; i++) {
		auto &known_repository = KNOWN_REPOSITORIES[i];
		if (repository == known_repository.name) {
			result = ExtensionRepository(known_repository.name, known_repository.url);
			result.type = known_repository.type;
			return true;
		}
	}
	return false;
}

vector<string> ExtensionRepository::GetKnownRepositoryNames() {
	vector<string> result;
	for (idx_t i = 0; KNOWN_REPOSITORIES[i].name; i++) {
		result.emplace_back(KNOWN_REPOSITORIES[i].name);
	}
	return result;
}

string ExtensionRepository::TryGetRepositoryUrl(const string &repository) {
	ExtensionRepository result;
	if (!TryGetKnownRepository(repository, result)) {
		return "";
	}
	return result.path;
}

string ExtensionRepository::TryConvertUrlToKnownRepository(const string &url) {
	for (idx_t i = 0; KNOWN_REPOSITORIES[i].name; i++) {
		if (url == KNOWN_REPOSITORIES[i].url) {
			return KNOWN_REPOSITORIES[i].name;
		}
	}
	return "";
}

string ExtensionRepository::TryGetBackupRepositoryUrl(const string &url) {
	if (url == CORE_REPOSITORY_URL) {
		return CORE_BACKUP_REPOSITORY_URL;
	}
	if (url == COMMUNITY_REPOSITORY_URL) {
		return COMMUNITY_BACKUP_REPOSITORY_URL;
	}
	return "";
}

bool ExtensionRepository::IsOfficialRepositoryUrl(const string &url) {
	static constexpr const char *OFFICIAL_REPOSITORY_URLS[] = {CORE_REPOSITORY_URL, CORE_NIGHTLY_REPOSITORY_URL,
	                                                           COMMUNITY_REPOSITORY_URL, CORE_BACKUP_REPOSITORY_URL,
	                                                           COMMUNITY_BACKUP_REPOSITORY_URL};
	// strip the scheme: the official repositories are served over both http and https
	string host_and_path;
	if (StringUtil::StartsWith(url, "http://")) {
		host_and_path = url.substr(7);
	} else if (StringUtil::StartsWith(url, "https://")) {
		host_and_path = url.substr(8);
	} else {
		return false;
	}
	for (auto repository_url : OFFICIAL_REPOSITORY_URLS) {
		string repository_host = string(repository_url).substr(7);
		// the host must match exactly, so that e.g. "extensions.duckdb.org.example.com" does not match
		if (host_and_path == repository_host || StringUtil::StartsWith(host_and_path, repository_host + "/")) {
			return true;
		}
	}
	return false;
}

ExtensionRepository ExtensionRepository::GetDefaultRepository(optional_ptr<DBConfig> config) {
	if (config) {
		auto custom_extension_repo = Settings::Get<CustomExtensionRepositorySetting>(*config);
		if (!custom_extension_repo.empty()) {
			return ExtensionRepository("", custom_extension_repo);
		}
	}

	return GetCoreRepository();
}
ExtensionRepository ExtensionRepository::GetDefaultRepository(ClientContext &context) {
	auto &config = DBConfig::GetConfig(context);
	return GetDefaultRepository(config);
}

ExtensionRepository ExtensionRepository::GetCoreRepository() {
	return {"core", CORE_REPOSITORY_URL};
}

ExtensionRepository ExtensionRepository::GetRepositoryByUrl(const string &url) {
	if (url.empty()) {
		return GetCoreRepository();
	}

	auto repo_name = TryConvertUrlToKnownRepository(url);
	return {repo_name, url};
}

ExtensionRepository::ExtensionRepository() : name("core"), path(CORE_REPOSITORY_URL) {
}
ExtensionRepository::ExtensionRepository(const string &name_p, const string &path_p, vector<string> public_keys_p)
    : name(name_p), path(path_p), public_keys(std::move(public_keys_p)) {
}

string ExtensionRepository::ToReadableString() {
	if (!name.empty()) {
		return name;
	}
	return path;
}

unique_ptr<ExtensionInstallInfo> ExtensionInstallInfo::TryReadInfoFile(FileSystem &fs,
                                                                       const std::string &info_file_path,
                                                                       const std::string &extension_name) {
	unique_ptr<ExtensionInstallInfo> result;

	string hint = StringUtil::Format("Try reinstalling the extension using 'FORCE INSTALL %s;'", extension_name);

	// Return empty info if the file is missing (TODO: throw error here in the future?)
	if (!fs.FileExists(info_file_path)) {
		return make_uniq<ExtensionInstallInfo>();
	}

	BufferedFileReader file_reader(fs, info_file_path.c_str());
	if (!file_reader.Finished()) {
		try {
			result = BinaryDeserializer::Deserialize<ExtensionInstallInfo>(file_reader);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			throw IOException(
			    "Failed to read info file for '%s' extension: '%s'.\nA serialization error occurred: '%s'\n%s",
			    extension_name, info_file_path, error.RawMessage(), hint);
		}
	}

	if (!result) {
		throw IOException("Failed to read info file for '%s' extension: '%s'.\nThe file appears to be empty!\n%s",
		                  extension_name, info_file_path, hint);
	}

	return result;
}

} // namespace duckdb
