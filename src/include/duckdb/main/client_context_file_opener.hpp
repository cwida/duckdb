//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/client_context_file_opener.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/file_opener.hpp"

namespace duckdb {

class Context;

//! FileOpener implementation backed by a Context.
//! The Context must outlive the opener.
class ClientContextFileOpener : public FileOpener {
public:
	explicit ClientContextFileOpener(Context &context_p) : context(context_p) {
	}

	Logger &GetLogger() const override;
	SettingLookupResult TryGetCurrentSetting(const Identifier &key, Value &result, FileOpenerInfo &info) override;
	SettingLookupResult TryGetCurrentSetting(const Identifier &key, Value &result) override;

	Context &TryGetContext() override {
		return context;
	}
	optional_ptr<DatabaseInstance> TryGetDatabase() override;
	HTTPUtil &GetHTTPUtil() override;

private:
	Context &context;
};

} // namespace duckdb
