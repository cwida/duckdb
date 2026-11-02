#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

#include "duckdb/main/database.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/main/http/http_util.hpp"

namespace duckdb {

SettingLookupResult ClientContextFileOpener::TryGetCurrentSetting(const Identifier &key, Value &result) {
	return context.TryGetCurrentSetting(key, result);
}

Logger &ClientContextFileOpener::GetLogger() const {
	return Logger::Get(context);
}

// LCOV_EXCL_START
SettingLookupResult ClientContextFileOpener::TryGetCurrentSetting(const Identifier &key, Value &result,
                                                                  FileOpenerInfo &) {
	return context.TryGetCurrentSetting(key, result);
}

optional_ptr<DatabaseInstance> ClientContextFileOpener::TryGetDatabase() {
	return context.db.get();
}

HTTPUtil &ClientContextFileOpener::GetHTTPUtil() {
	return HTTPUtil::Get(*TryGetDatabase());
}

unique_ptr<CatalogTransaction> FileOpener::TryGetCatalogTransaction(optional_ptr<FileOpener> opener) {
	if (!opener) {
		return nullptr;
	}
	auto context = opener->TryGetClientContext();
	if (context) {
		return make_uniq<CatalogTransaction>(CatalogTransaction::GetSystemCatalogTransaction(*context));
	}

	auto db = opener->TryGetDatabase();
	if (db) {
		return make_uniq<CatalogTransaction>(CatalogTransaction::GetSystemTransaction(*db));
	}
	return nullptr;
}

optional_ptr<ClientContext> FileOpener::TryGetClientContext(optional_ptr<FileOpener> opener) {
	if (!opener) {
		return nullptr;
	}
	return opener->TryGetClientContext();
}

optional_ptr<DatabaseInstance> FileOpener::TryGetDatabase(optional_ptr<FileOpener> opener) {
	if (!opener) {
		return nullptr;
	}
	return opener->TryGetDatabase();
}

optional_ptr<SecretManager> FileOpener::TryGetSecretManager(optional_ptr<FileOpener> opener) {
	if (!opener) {
		return nullptr;
	}

	auto db = opener->TryGetDatabase();
	if (!db) {
		return nullptr;
	}

	return &db->GetSecretManager();
}

SettingLookupResult FileOpener::TryGetCurrentSetting(optional_ptr<FileOpener> opener, const Identifier &key,
                                                     Value &result) {
	if (!opener) {
		return SettingLookupResult();
	}
	return opener->TryGetCurrentSetting(key, result);
}

SettingLookupResult FileOpener::TryGetCurrentSetting(optional_ptr<FileOpener> opener, const Identifier &key,
                                                     Value &result, FileOpenerInfo &info) {
	if (!opener) {
		return SettingLookupResult();
	}
	return opener->TryGetCurrentSetting(key, result, info);
}

SettingLookupResult FileOpener::TryGetCurrentSetting(const Identifier &key, Value &result, FileOpenerInfo &info) {
	return TryGetCurrentSetting(key, result);
}
// LCOV_EXCL_STOP
} // namespace duckdb
