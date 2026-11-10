#include "duckdb/main/valid_checker.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"

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

void ValidChecker::InvalidateIfValid(string error) {
	lock_guard<mutex> l(invalidate_lock);
	if (is_invalidated) {
		return;
	}
	is_invalidated = true;
	invalidated_msg = std::move(error);
}

void ValidChecker::Invalidate(DatabaseInstance &db, const ErrorData &error) {
	if (error.ExtraInfo().count(Exception::INVALIDATED_DATABASE_KEY)) {
		// the attached database that raised the error has already been invalidated
		return;
	}
	Get(db).Invalidate(error.RawMessage());
}

string ValidChecker::InvalidatedMessage() {
	lock_guard<mutex> l(invalidate_lock);
	return invalidated_msg;
}

} // namespace duckdb
