#include "duckdb/main/valid_checker.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"

namespace duckdb {

ValidChecker::ValidChecker(DatabaseInstance &db) : is_invalidated(false), db(db) {
}

void ValidChecker::Invalidate(string error) {
	lock_guard<mutex> l(invalidate_lock);
	is_invalidated = true;
	invalidated_msg = std::move(error);
}

bool ValidChecker::IsInvalidated() {
	if (!is_invalidated) {
		return false;
	}
	return !Settings::Get<DisableDatabaseInvalidationSetting>(db);
}

void ValidChecker::Invalidate(DatabaseInstance &db, const ErrorData &error) {
	auto &extra_info = error.ExtraInfo();
	auto entry = extra_info.find(Exception::INVALIDATED_DATABASE_KEY);
	if (entry != extra_info.end()) {
		auto attached = DatabaseManager::Get(db).GetDatabase(Identifier(entry->second));
		if (attached && attached->SupportsScopedInvalidation()) {
			if (!attached->GetValidChecker().is_invalidated) {
				attached->Invalidate("of a fatal error", error.RawMessage());
			}
			return;
		}
	}
	Get(db).Invalidate(error.RawMessage());
}

string ValidChecker::InvalidatedMessage() {
	lock_guard<mutex> l(invalidate_lock);
	return invalidated_msg;
}

} // namespace duckdb
