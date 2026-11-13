#include "duckdb/main/extension_repository_manager.hpp"

#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/serializer/buffered_file_writer.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/http/http_transport_manager.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/main/settings.hpp"
#include "mbedtls_wrapper.hpp"
#include "yyjson.hpp"

#include <cstring>

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

static constexpr const char *PEM_HEADER = "-----BEGIN PUBLIC KEY-----";
static constexpr const char *PEM_FOOTER = "-----END PUBLIC KEY-----";
//! The number of base64 characters that are written per line in a PEM encoded key
static constexpr const idx_t PEM_LINE_LENGTH = 64;
//! Sanity limit on the size of the metadata file that is fetched from a repository
static constexpr const idx_t MAX_METADATA_FILE_SIZE = 65536;

//! Wrapper that ensures the yyjson document is freed when an exception is thrown
struct RepositoryJSONDocument {
	explicit RepositoryJSONDocument(yyjson_doc *doc_p) : doc(doc_p) {
	}
	~RepositoryJSONDocument() {
		yyjson_doc_free(doc);
	}
	yyjson_doc *doc;
};

//===--------------------------------------------------------------------===//
// Public keys
//===--------------------------------------------------------------------===//
string ExtensionRepositoryManager::ToPEMPublicKey(const string &compact_key) {
	string result = string(PEM_HEADER) + "\n";
	for (idx_t pos = 0; pos < compact_key.size(); pos += PEM_LINE_LENGTH) {
		result += compact_key.substr(pos, PEM_LINE_LENGTH) + "\n";
	}
	result += string(PEM_FOOTER) + "\n";
	return result;
}

string ExtensionRepositoryManager::ToCompactPublicKey(const string &public_key) {
	string key = public_key;

	// strip the PEM header and footer if the key is provided in PEM form
	auto header_pos = key.find(PEM_HEADER);
	if (header_pos != string::npos) {
		auto footer_pos = key.find(PEM_FOOTER, header_pos);
		if (footer_pos == string::npos) {
			throw InvalidInputException("Invalid public key: found '%s' but no matching '%s'", PEM_HEADER, PEM_FOOTER);
		}
		auto body_start = header_pos + strlen(PEM_HEADER);
		key = key.substr(body_start, footer_pos - body_start);
	} else if (StringUtil::Contains(key, "-----BEGIN")) {
		throw InvalidInputException(
		    "Invalid public key: only keys in SubjectPublicKeyInfo format (starting with '%s') are supported",
		    PEM_HEADER);
	}

	// the compact representation is the base64 body of the PEM encoding without any whitespace
	string result;
	for (auto c : key) {
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			continue;
		}
		if (!StringUtil::CharacterIsAlpha(c) && !StringUtil::CharacterIsDigit(c) && c != '+' && c != '/' && c != '=') {
			throw InvalidInputException("Invalid public key: unexpected character '%c' in base64 encoded key", c);
		}
		result += c;
	}

	if (!duckdb_mbedtls::MbedTlsWrapper::IsValidRSA2048PublicKey(ToPEMPublicKey(result))) {
		throw InvalidInputException("Invalid public key: extensions are signed using 2048 bit RSA keys, provide the "
		                            "public key either in PEM format or as the base64 encoded body of the PEM format");
	}
	return result;
}

vector<string> ExtensionRepositoryManager::ToCompactPublicKeys(const string &public_keys) {
	vector<string> result;
	auto header_pos = public_keys.find(PEM_HEADER);
	if (header_pos == string::npos) {
		// no PEM header - treat the entire blob as a single compact key
		result.push_back(ToCompactPublicKey(public_keys));
		return result;
	}
	// the blob contains one or more concatenated PEM blocks - extract each of them
	while (header_pos != string::npos) {
		auto footer_pos = public_keys.find(PEM_FOOTER, header_pos);
		if (footer_pos == string::npos) {
			throw InvalidInputException("Invalid public key: found '%s' but no matching '%s'", PEM_HEADER, PEM_FOOTER);
		}
		auto block_end = footer_pos + strlen(PEM_FOOTER);
		result.push_back(ToCompactPublicKey(public_keys.substr(header_pos, block_end - header_pos)));
		header_pos = public_keys.find(PEM_HEADER, block_end);
	}
	return result;
}

vector<string> ExtensionRepositoryManager::NormalizePublicKeys(const vector<string> &public_keys) {
	vector<string> result;
	for (auto &blob : public_keys) {
		for (auto &key : ToCompactPublicKeys(blob)) {
			result.push_back(key);
		}
	}
	return result;
}

string ExtensionRepositoryManager::GetPublicKeyFingerprint(const string &compact_key) {
	auto der_key = Blob::FromBase64(string_t(compact_key));

	char hash[duckdb_mbedtls::MbedTlsWrapper::SHA256_HASH_LENGTH_BYTES];
	duckdb_mbedtls::MbedTlsWrapper::ComputeSha256Hash(der_key.data(), der_key.size(), hash);

	char hex[duckdb_mbedtls::MbedTlsWrapper::SHA256_HASH_LENGTH_TEXT];
	duckdb_mbedtls::MbedTlsWrapper::ToBase16(hash, hex, duckdb_mbedtls::MbedTlsWrapper::SHA256_HASH_LENGTH_BYTES);

	return "sha256:" + string(hex, duckdb_mbedtls::MbedTlsWrapper::SHA256_HASH_LENGTH_TEXT);
}

//===--------------------------------------------------------------------===//
// Repository metadata
//===--------------------------------------------------------------------===//
//! Read a small file that may live on a remote file system (autoloading the extension that provides it), bounded by
//! MAX_METADATA_FILE_SIZE: a metadata file is a few lines of json
//! Remote file systems are provided by extensions - autoload the one `path` needs if necessary. A missing extension is
//! reported as such: it is not something the remote side did
static void LoadExtensionForRemotePath(DatabaseInstance &db, optional_ptr<ClientContext> context, const string &path) {
	auto &fs = db.GetFileSystem();
	string required_extension;
	if (fs.IsRemoteFile(path, required_extension) && !db.ExtensionIsLoaded(required_extension)) {
		if (!context || !Settings::Get<AutoloadKnownExtensionsSetting>(*context)) {
			throw MissingExtensionException(
			    StringUtil::Format("reading '%s' requires the %s extension, which is not loaded: run `INSTALL AND LOAD "
			                       "%s;` first",
			                       path, required_extension, required_extension));
		}
		try {
			ExtensionHelper::AutoLoadExtension(*context, required_extension);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			throw MissingExtensionException(StringUtil::Format(
			    "reading '%s' requires the %s extension, which could not be autoloaded: %s\nRun `INSTALL AND LOAD %s;` "
			    "first",
			    path, required_extension, error.RawMessage(), required_extension));
		}
	}
}

static string ReadSmallRemoteFile(DatabaseInstance &db, optional_ptr<ClientContext> context, const string &path) {
	LoadExtensionForRemotePath(db, context, path);
	auto &fs = db.GetFileSystem();
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto file_size = handle->GetFileSize();
	if (file_size > MAX_METADATA_FILE_SIZE) {
		throw IOException("File is %llu bytes, which is too large to be a repository metadata file", file_size);
	}
	auto buffer = make_unsafe_uniq_array<char>(file_size);
	handle->Read(buffer.get(), file_size, 0);
	return string(buffer.get(), file_size);
}

//! Fetch the public keys that the repository publishes at its prefix. Note that the keys are pinned when the repository
//! is created: the fingerprints that are reported back are what identify the keys from there on
vector<string> ExtensionRepositoryManager::FetchPublicKeys(DatabaseInstance &db, optional_ptr<ClientContext> context,
                                                           const string &prefix) {
	auto metadata_path = prefix;
	StringUtil::RTrim(metadata_path, "/");
	metadata_path += "/" + string(METADATA_FILE);

	string contents;
	try {
		contents = ReadSmallRemoteFile(db, context, metadata_path);
	} catch (MissingExtensionException &) {
		throw;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		throw IOException("Failed to read the metadata file of the extension repository from '%s': %s\nProvide the key "
		                  "directly using `USING PUBLIC KEY '<key>'` if the repository does not serve it",
		                  metadata_path, error.RawMessage());
	}
	return ParsePublicKeys(contents, metadata_path);
}

vector<string> ExtensionRepositoryManager::ParsePublicKeys(const string &contents, const string &metadata_path) {
	RepositoryJSONDocument document(yyjson_read(contents.c_str(), contents.size(), 0));
	if (!document.doc) {
		throw IOException("Failed to read extension repository metadata file '%s': the file is not valid json",
		                  metadata_path);
	}
	auto root = yyjson_doc_get_root(document.doc);
	if (!root || !yyjson_is_obj(root)) {
		throw IOException("Failed to read extension repository metadata file '%s': expected a json object",
		                  metadata_path);
	}
	auto keys = yyjson_obj_get(root, "signature_keys");
	if (!keys || !yyjson_is_arr(keys)) {
		throw IOException(
		    "Failed to read extension repository metadata file '%s': missing or invalid field 'signature_keys'",
		    metadata_path);
	}

	vector<string> result;
	size_t idx, max;
	yyjson_val *key;
	yyjson_arr_foreach(keys, idx, max, key) {
		if (!yyjson_is_str(key)) {
			throw IOException(
			    "Failed to read extension repository metadata file '%s': 'signature_keys' must be a list of strings",
			    metadata_path);
		}
		result.push_back(ToCompactPublicKey(string(yyjson_get_str(key), yyjson_get_len(key))));
	}
	if (result.empty()) {
		throw IOException(
		    "Failed to read extension repository metadata file '%s': 'signature_keys' cannot be empty\nProvide the key "
		    "directly using `USING PUBLIC KEY '<key>'` if the repository does not serve it",
		    metadata_path);
	}
	return result;
}

ExtensionRepositoryAccess ExtensionRepositoryManager::ParseAccess(const string &value) {
	auto lower = StringUtil::Lower(value);
	if (lower == "hosts") {
		return ExtensionRepositoryAccess::HOSTS;
	}
	if (lower == "allowed") {
		return ExtensionRepositoryAccess::ALLOWED;
	}
	if (lower == "forbidden") {
		return ExtensionRepositoryAccess::FORBIDDEN;
	}
	if (lower == "hosts_only") {
		return ExtensionRepositoryAccess::HOSTS_ONLY;
	}
	throw InvalidInputException("Invalid value '%s' for allow_extension_repositories: expected 'hosts', 'allowed', "
	                            "'hosts_only' or 'forbidden'",
	                            value);
}

string ExtensionRepositoryManager::AccessToString(ExtensionRepositoryAccess access) {
	switch (access) {
	case ExtensionRepositoryAccess::ALLOWED:
		return "allowed";
	case ExtensionRepositoryAccess::FORBIDDEN:
		return "forbidden";
	case ExtensionRepositoryAccess::HOSTS_ONLY:
		return "hosts_only";
	default:
		return "hosts";
	}
}

ExtensionRepositoryAccess ExtensionRepositoryManager::GetAccess(DatabaseInstance &db) {
	return ParseAccess(Settings::Get<AllowExtensionRepositoriesSetting>(db));
}

bool ExtensionRepositoryManager::TrustsExistingRepositories(DatabaseInstance &db) {
	// the access values that trust existing repositories are listed explicitly, any other value distrusts them
	switch (GetAccess(db)) {
	case ExtensionRepositoryAccess::HOSTS:
	case ExtensionRepositoryAccess::ALLOWED:
	case ExtensionRepositoryAccess::HOSTS_ONLY:
		return true;
	default:
		return false;
	}
}

bool ExtensionRepositoryManager::TrustsRepository(DatabaseInstance &db, const ExtensionRepository &repository) {
	if (!TrustsExistingRepositories(db)) {
		return false;
	}
	if (GetAccess(db) == ExtensionRepositoryAccess::HOSTS_ONLY) {
		// only a repository resolved from its host is trusted - one added by prefix is distrusted
		return !repository.host.empty();
	}
	return true;
}

void ExtensionRepositoryManager::CheckNotLocked(DatabaseInstance &db) {
	// modifying the trusted repositories changes the set of keys that are trusted to sign extensions, so it is gated
	// by the configuration lock: a locked configuration freezes the trust store
	db.config.CheckLock(AllowExtensionRepositoriesSetting::Name);
}

void ExtensionRepositoryManager::CheckCanCreate(DatabaseInstance &db, bool from_host) {
	CheckNotLocked(db);
	// adding a repository adds a new trusted signing key, so the user must explicitly opt in first - except for a
	// repository resolved from its host: the name says who is trusted, and the host's own file says what, so the
	// 'hosts' default lets it through
	switch (GetAccess(db)) {
	case ExtensionRepositoryAccess::ALLOWED:
		return;
	case ExtensionRepositoryAccess::FORBIDDEN:
		throw PermissionException("Adding extension repositories has been forbidden through configuration");
	case ExtensionRepositoryAccess::HOSTS_ONLY:
		if (from_host) {
			return;
		}
		throw PermissionException("Adding extension repositories by prefix is not permitted while "
		                          "allow_extension_repositories='hosts_only': use `CREATE EXTENSION REPOSITORY <host>` "
		                          "without a prefix, so that the host names its repository");
	default:
		if (from_host) {
			return;
		}
		throw PermissionException("Adding extension repositories is not enabled: set "
		                          "allow_extension_repositories='allowed' to opt in, or use `CREATE EXTENSION "
		                          "REPOSITORY <host>` without a prefix, so that the host names its repository");
	}
}

bool ExtensionRepositoryManager::TryNormalizeRepositoryName(const string &name, string &result) {
	if (name.empty()) {
		return false;
	}
	result = StringUtil::Lower(name);
	for (auto c : result) {
		if (!StringUtil::CharacterIsAlpha(c) && !StringUtil::CharacterIsDigit(c) && c != '_' && c != '-' && c != '.') {
			return false;
		}
	}
	// a dot lets a repository be named after a host ('carlopi.github.io'); as a file name it must still be a plain
	// name: not hidden, not '.' or '..' or a run of dots
	if (result.front() == '.' || result.back() == '.' || result.find("..") != string::npos) {
		return false;
	}
	return true;
}

//! The repository name a stored file carries in its name: the file name without the fixed suffix
static string RepositoryNameOfFile(const string &file_name) {
	D_ASSERT(StringUtil::EndsWith(file_name, ExtensionRepositoryManager::FILE_EXTENSION));
	return file_name.substr(0, file_name.size() - strlen(ExtensionRepositoryManager::FILE_EXTENSION));
}

string ExtensionRepositoryManager::NormalizeRepositoryName(const string &name) {
	string result;
	if (!TryNormalizeRepositoryName(name, result)) {
		throw InvalidInputException("Invalid extension repository name '%s': names cannot be empty and can only "
		                            "contain letters, digits, '_', '-' and '.' (not at the start or the end, and not "
		                            "two in a row)",
		                            name);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Repositories resolved from their host: CREATE EXTENSION REPOSITORY <host> without a prefix
//
// The name is the publisher's host. The host serves `https://<host>/.well-known/duckdb-extension-repo.json`, the same
// metadata file a prefix serves, and only its `{"version": 1, "repository_url": "<url>"}` is read: where the repository
// is. From there on it is CREATE EXTENSION REPOSITORY <host> WITH PREFIX '<url>', keys fetched from the prefix and
// pinned. The fetch over https from the host is what makes the file the host's own statement.
//===--------------------------------------------------------------------===//

static constexpr const idx_t METADATA_FORMAT_VERSION = 1;

//! Whether a name is one DuckDB will reach over the network as a host: two or more labels of letters, digits and '-'
//! separated by single dots, no label starting or ending with '-', the last label letters only (so no IPv4 literal),
//! and not localhost. A single word is not a host: it would be resolved through search domains to a host the user did
//! not name. There is no stricter test short of the public suffix list; this one keeps out what is certainly not a
//! public host
bool ExtensionRepositoryManager::IsHostName(const string &text) {
	idx_t label_count = 0;
	idx_t label_start = 0;
	for (idx_t i = 0; i <= text.size(); i++) {
		if (i < text.size() && text[i] != '.') {
			auto c = text[i];
			if (!StringUtil::CharacterIsAlpha(c) && !StringUtil::CharacterIsDigit(c) && c != '-') {
				return false;
			}
			continue;
		}
		// end of a label
		if (i == label_start || text[label_start] == '-' || text[i - 1] == '-') {
			return false;
		}
		label_count++;
		label_start = i + 1;
	}
	if (label_count < 2) {
		return false;
	}
	for (idx_t i = text.rfind('.') + 1; i < text.size(); i++) {
		if (!StringUtil::CharacterIsAlpha(text[i])) {
			return false;
		}
	}
	auto lower = StringUtil::Lower(text);
	return lower != "localhost" && !StringUtil::EndsWith(lower, ".localhost");
}

//! Read a small file over https for resolving a repository from its host. The file decides which prefix and which
//! signing keys get trusted, so it is not read through the file system, whose http settings the user controls: the url
//! must be https, a secret or setting that switches verification off for it is refused, and the server certificate is
//! verified whatever the provider's defaults are.
//!
//! Redirects are followed as the client follows them, wherever they lead, http included. The first hop is verified
//! https, so a redirect can only come from the host itself: it is the host speaking, and where it sends its visitors
//! (a moved site, a different domain, even plain http) is the host's own decision and the host's own risk, exactly as
//! the content of the file is. Nothing here inspects the hops.
static string ReadAuthenticatedHTTPS(DatabaseInstance &db, optional_ptr<ClientContext> context, const string &url,
                                     const char *what) {
	if (!StringUtil::StartsWith(url, "https://")) {
		throw InvalidInputException("The %s must be reached over https, got '%s'", what, url);
	}
	LoadExtensionForRemotePath(db, context, url);

	auto &manager = db.config.GetHTTPTransportManager();
	auto session = context ? manager.CreateSession(*context, url) : manager.CreateSession(db, url);
	auto &params = session.Parameters();
	// a secret or setting that switches verification off for this url is refused outright, rather than silently
	// overruled: it says the user does not want this host verified, and an unverified host cannot vouch for anything
	if (params.override_verify_ssl && !params.verify_ssl) {
		throw InvalidInputException(
		    "Cannot read the %s '%s': certificate verification is disabled for it (a VERIFY_SSL "
		    "false secret or setting applies), and a repository is only resolved from a host "
		    "that is verified",
		    what, url);
	}
	// ...and verification is required regardless of the provider's defaults
	params.RequireServerCertificateVerification();
	params.keep_alive = false;

	// the body is received through a handler so that the transfer stops as soon as it exceeds the cap, or before it
	// starts when the announced length does: the cap is on what is downloaded, not on what is kept afterwards
	string body;
	bool too_large = false;
	auto response_handler = [&](const HTTPResponse &response) {
		// called once per response (a redirect or a retry starts another one): begin afresh
		body.clear();
		if (too_large) {
			// a previous attempt already received too much: do not download it again
			return false;
		}
		if (response.HasHeader("Content-Length")) {
			idx_t content_length;
			if (TryCast::Operation<string_t, idx_t>(string_t(response.GetHeaderValue("Content-Length")), content_length,
			                                        false) &&
			    content_length > MAX_METADATA_FILE_SIZE) {
				too_large = true;
				return false;
			}
		}
		return true;
	};
	auto content_handler = [&](const_data_ptr_t data, idx_t data_length) {
		if (body.size() + data_length > MAX_METADATA_FILE_SIZE) {
			too_large = true;
			return false;
		}
		body.append(const_char_ptr_cast(data), data_length);
		return true;
	};
	HTTPHeaders headers(db);
	GetRequestInfo get_request(url, headers, params, response_handler, content_handler);
	get_request.try_request = true;
	auto response = session.Request(get_request);
	if (too_large) {
		throw IOException("Failed to read the %s '%s': it is larger than the %llu bytes a repository metadata file can "
		                  "take",
		                  what, url, MAX_METADATA_FILE_SIZE);
	}
	if (response->HasRequestError()) {
		throw IOException("Failed to read the %s '%s': %s", what, url, response->GetRequestError());
	}
	if (!response->Success()) {
		throw IOException("Failed to read the %s '%s': HTTP %d %s", what, url, static_cast<int32_t>(response->status),
		                  response->GetError());
	}
	return body;
}

//! The root object of a host's metadata file, after checking its version
static yyjson_val *ReadVersionedObject(RepositoryJSONDocument &document, const string &path) {
	if (!document.doc) {
		throw IOException("Failed to read the extension repository metadata file '%s': the file is not valid json",
		                  path);
	}
	auto root = yyjson_doc_get_root(document.doc);
	if (!root || !yyjson_is_obj(root)) {
		throw IOException("Failed to read the extension repository metadata file '%s': expected a json object", path);
	}
	auto version = yyjson_obj_get(root, "version");
	if (!version || !yyjson_is_uint(version)) {
		throw IOException("Failed to read the extension repository metadata file '%s': missing or invalid field "
		                  "'version'",
		                  path);
	}
	if (yyjson_get_uint(version) != METADATA_FORMAT_VERSION) {
		throw IOException("Failed to read the extension repository metadata file '%s': it has version %llu, this "
		                  "version of DuckDB understands version %llu",
		                  path, yyjson_get_uint(version), METADATA_FORMAT_VERSION);
	}
	return root;
}

ExtensionRepositoryResolution ExtensionRepositoryManager::ResolveRepositoryFromHost(DatabaseInstance &db,
                                                                                    optional_ptr<ClientContext> context,
                                                                                    const string &name_p) {
	// called by CreateRepository once the access check and the conflict check have passed
	auto name = NormalizeRepositoryName(name_p);
	// only a name that is a host on the public internet is reached: a single word would resolve through search
	// domains to a host the user did not name, an IP literal or localhost is nobody's statement
	if (!IsHostName(name)) {
		throw InvalidInputException(
		    "Cannot create extension repository '%s': '%s' is not a host name. A host has two or "
		    "more labels of letters, digits and '-' separated by dots, the last one letters "
		    "only, and is not localhost\nUse `WITH PREFIX '<url>'` to name the repository's "
		    "location directly",
		    name_p, name);
	}
	ExtensionRepositoryResolution resolution;
	resolution.host = name;

	// the host is the name the user typed, reached over verified https: that is what makes the file the host's own
	// statement
	auto metadata_path = "https://" + name + "/" + string(METADATA_FILE);
	string contents;
	try {
		contents = ReadAuthenticatedHTTPS(db, context, metadata_path, "host's metadata file");
	} catch (MissingExtensionException &) {
		throw;
	} catch (InvalidInputException &) {
		// a policy refusal (verification switched off for the url): not something the host did
		throw;
	} catch (std::exception &ex) {
		ErrorData error(ex);
		throw IOException("Cannot create extension repository '%s': the host does not serve a metadata file (failed to "
		                  "read '%s': %s)\nUse `WITH PREFIX '<url>'` to name the repository's location directly",
		                  name, metadata_path, error.RawMessage());
	}
	RepositoryJSONDocument document(yyjson_read(contents.c_str(), contents.size(), 0));
	auto root = ReadVersionedObject(document, metadata_path);
	auto url = yyjson_obj_get(root, "repository_url");
	if (!url || !yyjson_is_str(url) || yyjson_get_len(url) == 0) {
		throw IOException("Failed to read the extension repository metadata file '%s': missing or invalid field "
		                  "'repository_url'",
		                  metadata_path);
	}
	resolution.url = string(yyjson_get_str(url), yyjson_get_len(url));
	StringUtil::RTrim(resolution.url, "/");
	// the keys are fetched from the prefix next: over anything but https they could be anyone's
	if (!StringUtil::StartsWith(resolution.url, "https://")) {
		throw InvalidInputException("Cannot create extension repository '%s': the host names the repository at '%s', "
		                            "but a repository resolved from its host must be served over https, so that its "
		                            "signing keys are fetched from where the host says they are",
		                            name, resolution.url);
	}
	return resolution;
}

//===--------------------------------------------------------------------===//
// Repository storage
//===--------------------------------------------------------------------===//
string ExtensionRepositoryManager::GetRepositoryDirectory(DatabaseInstance &db, FileSystem &fs) {
	auto directory = Settings::Get<ExtensionRepositoryDirectorySetting>(db);
	if (directory.empty()) {
		directory = fs.JoinPath(fs.JoinPath(fs.GetHomeDirectory(), ".duckdb"), "extension_repositories");
	}
	return fs.ExpandPath(fs.ConvertSeparators(directory));
}

string ExtensionRepositoryManager::GetRepositoryPath(DatabaseInstance &db, FileSystem &fs, const string &name) {
	return fs.JoinPath(GetRepositoryDirectory(db, fs), name + FILE_EXTENSION);
}

//! Wrapper that ensures the mutable yyjson document is freed when an exception is thrown
struct RepositoryMutableJSONDocument {
	RepositoryMutableJSONDocument() : doc(yyjson_mut_doc_new(nullptr)) {
	}
	~RepositoryMutableJSONDocument() {
		yyjson_mut_doc_free(doc);
	}
	yyjson_mut_doc *doc;
};

static string ReadJSONString(yyjson_val *root, const char *field, const string &path) {
	auto val = yyjson_obj_get(root, field);
	if (!val || !yyjson_is_str(val)) {
		throw IOException("Failed to read extension repository file '%s': missing or invalid field '%s'", path, field);
	}
	return string(yyjson_get_str(val), yyjson_get_len(val));
}

//! Read the trusted public keys of a repository. Keys are stored in the "public_keys" array
static vector<string> ReadJSONKeys(yyjson_val *root, const string &path) {
	vector<string> result;
	auto keys = yyjson_obj_get(root, "public_keys");
	if (!keys || !yyjson_is_arr(keys)) {
		throw IOException("Failed to read extension repository file '%s': missing or invalid field 'public_keys'",
		                  path);
	}
	size_t idx, max;
	yyjson_val *key;
	yyjson_arr_foreach(keys, idx, max, key) {
		if (!yyjson_is_str(key)) {
			throw IOException("Failed to read extension repository file '%s': 'public_keys' must be a list of strings",
			                  path);
		}
		result.push_back(
		    ExtensionRepositoryManager::ToCompactPublicKey(string(yyjson_get_str(key), yyjson_get_len(key))));
	}
	if (result.empty()) {
		throw IOException("Failed to read extension repository file '%s': 'public_keys' cannot be empty", path);
	}
	return result;
}

ExtensionRepository ExtensionRepositoryManager::ReadRepository(FileSystem &fs, const string &path) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto file_size = handle->GetFileSize();
	auto buffer = make_unsafe_uniq_array<char>(file_size);
	handle->Read(buffer.get(), file_size, 0);
	string contents(buffer.get(), file_size);

	RepositoryJSONDocument document(yyjson_read(contents.c_str(), contents.size(), 0));
	if (!document.doc) {
		throw IOException("Failed to read extension repository file '%s': the file is not valid json", path);
	}
	auto root = yyjson_doc_get_root(document.doc);
	if (!root || !yyjson_is_obj(root)) {
		throw IOException("Failed to read extension repository file '%s': expected a json object", path);
	}

	ExtensionRepository result;
	result.name = ReadJSONString(root, "name", path);
	result.path = ReadJSONString(root, "url", path);
	result.public_keys = ReadJSONKeys(root, path);
	result.type = ExtensionRepositoryType::USER_PROVIDED;
	auto host = yyjson_obj_get(root, "host");
	if (host && yyjson_is_str(host)) {
		result.host = string(yyjson_get_str(host), yyjson_get_len(host));
	}

	auto expected_name = RepositoryNameOfFile(fs.ExtractName(path));
	if (NormalizeRepositoryName(result.name) != expected_name) {
		throw IOException("Failed to read extension repository file '%s': the file contains repository '%s', but '%s' "
		                  "was expected based on the file name",
		                  path, result.name, expected_name);
	}
	// a host means "resolved from that host", and a repository resolved from a host is named after it: a file that
	// says otherwise was not written by DuckDB, and is not trusted on its word
	if (!result.host.empty() && (!IsHostName(result.host) || StringUtil::Lower(result.host) != expected_name)) {
		throw IOException("Failed to read extension repository file '%s': the file names host '%s', but a repository "
		                  "resolved from a host is named after it ('%s')",
		                  path, result.host, expected_name);
	}
	// a user provided repository must never shadow a built-in repository: those are resolved first, so a file named
	// after a built-in repository would silently redirect installs to an attacker controlled key and url
	if (!ExtensionRepository::TryGetRepositoryUrl(result.name).empty()) {
		throw IOException("Failed to read extension repository file '%s': '%s' is a built-in repository name", path,
		                  result.name);
	}
	return result;
}

ExtensionRepository ExtensionRepositoryManager::CreateRepository(DatabaseInstance &db, FileSystem &fs,
                                                                 optional_ptr<ClientContext> context,
                                                                 const ExtensionRepository &requested,
                                                                 OnCreateConflict on_conflict) {
	// no prefix: the name is the host, and the host names the prefix. The host is derived here, never taken from the
	// caller, so a stored host always means the repository was resolved from it
	bool from_host = requested.path.empty();
	CheckCanCreate(db, from_host);

	auto name = NormalizeRepositoryName(requested.name);
	if (!ExtensionRepository::TryGetRepositoryUrl(name).empty()) {
		throw InvalidInputException("Cannot create extension repository '%s': this is a built-in repository name",
		                            name);
	}
	if (from_host && !requested.public_keys.empty()) {
		// the keys of a repository resolved from its host come from the repository the host names: a key the caller
		// supplies would be trusted for whatever the host points at, with nothing vouching for it
		throw InvalidInputException(
		    "Cannot create extension repository '%s': a repository resolved from its host takes "
		    "its signing keys from the repository the host names; USING PUBLIC KEY goes with "
		    "WITH PREFIX",
		    name);
	}

	// the conflict is decided before anything is fetched: IF NOT EXISTS returns the stored definition, a plain CREATE
	// reports the duplicate, and only OR REPLACE goes on to resolve - keeping the old definition if that fails
	auto directory = GetRepositoryDirectory(db, fs);
	auto file_path = fs.JoinPath(directory, name + FILE_EXTENSION);
	if (fs.FileExists(file_path)) {
		switch (on_conflict) {
		case OnCreateConflict::ERROR_ON_CONFLICT:
			throw CatalogException(
			    "Extension repository with name \"%s\" already exists!\nUse CREATE OR REPLACE to overwrite it", name);
		case OnCreateConflict::IGNORE_ON_CONFLICT:
			return ReadRepository(fs, file_path);
		default:
			break;
		}
	}

	ExtensionRepository repository(name, requested.path, requested.public_keys);
	vector<string> public_keys;
	if (from_host) {
		auto resolution = ResolveRepositoryFromHost(db, context, name);
		repository.path = resolution.url;
		repository.host = name;
		// the keys come from the prefix the host named, fetched as strictly as the host's own file was
		auto metadata_path = repository.path + "/" + string(METADATA_FILE);
		public_keys = ParsePublicKeys(ReadAuthenticatedHTTPS(db, context, metadata_path, "repository's metadata file"),
		                              metadata_path);
	} else if (repository.public_keys.empty()) {
		// no key was provided: the repository is expected to serve it at its prefix
		public_keys = FetchPublicKeys(db, context, repository.path);
	} else {
		public_keys = NormalizePublicKeys(repository.public_keys);
	}
	// the keys are pinned here: they are stored alongside the repository and never fetched again
	if (public_keys.empty()) {
		throw InvalidInputException("Cannot create extension repository '%s': no public key was provided or served",
		                            name);
	}
	// the fetches took time: the access may have been tightened meanwhile, so check again before persisting
	CheckCanCreate(db, from_host);

	if (!fs.DirectoryExists(directory)) {
		fs.CreateDirectoriesRecursive(directory);
	}

	RepositoryMutableJSONDocument document;
	auto root = yyjson_mut_obj(document.doc);
	yyjson_mut_doc_set_root(document.doc, root);
	yyjson_mut_obj_add_uint(document.doc, root, "version", FORMAT_VERSION);
	yyjson_mut_obj_add_strcpy(document.doc, root, "name", name.c_str());
	yyjson_mut_obj_add_strcpy(document.doc, root, "url", repository.path.c_str());
	if (!repository.host.empty()) {
		yyjson_mut_obj_add_strcpy(document.doc, root, "host", repository.host.c_str());
	}
	auto keys_array = yyjson_mut_arr(document.doc);
	yyjson_mut_obj_add_val(document.doc, root, "public_keys", keys_array);
	for (auto &public_key : public_keys) {
		yyjson_mut_arr_add_strcpy(document.doc, keys_array, public_key.c_str());
	}

	size_t json_size;
	auto json = yyjson_mut_write(document.doc, YYJSON_WRITE_PRETTY, &json_size);
	if (!json) {
		throw InternalException("Failed to serialize extension repository '%s'", name);
	}
	string contents(json, json_size);
	free(json);

	// write to a temporary file first, then move it in place
	auto temp_path = file_path + ".tmp-" + UUID::ToString(UUID::GenerateRandomUUID());
	fs.TryRemoveFile(temp_path);
	{
		auto file_writer = BufferedFileWriter(fs, temp_path,
		                                      FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
		                                          FileFlags::FILE_FLAGS_ENABLE_EXTENSION_INSTALL);
		file_writer.WriteData(const_data_ptr_cast(contents.c_str()), contents.size());
		file_writer.Close();
	}
	fs.TryRemoveFile(file_path);
	fs.MoveFile(temp_path, file_path);

	ExtensionRepository result(name, repository.path, std::move(public_keys));
	result.type = ExtensionRepositoryType::USER_PROVIDED;
	result.host = repository.host;
	return result;
}

void ExtensionRepositoryManager::DropRepository(DatabaseInstance &db, FileSystem &fs, const string &name_p,
                                                OnEntryNotFound on_entry_not_found) {
	// dropping a repository only removes a trusted key, so it is not gated by the opt-in; it is still frozen by a
	// locked configuration
	CheckNotLocked(db);

	auto name = NormalizeRepositoryName(name_p);
	auto file_path = GetRepositoryPath(db, fs, name);
	if (!fs.FileExists(file_path)) {
		if (on_entry_not_found == OnEntryNotFound::RETURN_NULL) {
			return;
		}
		throw CatalogException("Extension repository with name \"%s\" does not exist!", name);
	}
	fs.RemoveFile(file_path);
}

vector<ExtensionRepository> ExtensionRepositoryManager::GetRepositories(DatabaseInstance &db, FileSystem &fs) {
	// note that existing repositories are always readable, even when adding new ones is not allowed: extensions from
	// repositories that were created earlier can still be installed and loaded
	vector<ExtensionRepository> result;
	auto directory = GetRepositoryDirectory(db, fs);
	if (!fs.DirectoryExists(directory)) {
		return result;
	}
	vector<string> files;
	fs.ListFiles(directory, [&](const string &file_name, bool is_dir) {
		if (!is_dir && StringUtil::EndsWith(file_name, FILE_EXTENSION)) {
			files.push_back(file_name);
		}
	});
	// sort the files to get a deterministic order
	std::sort(files.begin(), files.end());
	for (auto &file_name : files) {
		auto base_name = RepositoryNameOfFile(file_name);
		// ignore files that are named after a built-in repository: those are always served by the built-in table
		string normalized;
		if (TryNormalizeRepositoryName(base_name, normalized) &&
		    !ExtensionRepository::TryGetRepositoryUrl(normalized).empty()) {
			continue;
		}
		result.push_back(ReadRepository(fs, fs.JoinPath(directory, file_name)));
	}
	return result;
}

bool ExtensionRepositoryManager::TryGetRepository(DatabaseInstance &db, FileSystem &fs, const string &name_p,
                                                  ExtensionRepository &result) {
	// existing repositories are always resolvable, even when adding new ones is not allowed
	string name;
	if (!TryNormalizeRepositoryName(name_p, name)) {
		// this is not a valid repository name - it might still be a url
		return false;
	}
	if (!ExtensionRepository::TryGetRepositoryUrl(name).empty()) {
		// this is a built-in repository - it is always resolved through the built-in table, never through a file that
		// happens to be named after it
		return false;
	}
	auto file_path = GetRepositoryPath(db, fs, name);
	if (!fs.FileExists(file_path)) {
		return false;
	}
	result = ReadRepository(fs, file_path);
	return true;
}

} // namespace duckdb
