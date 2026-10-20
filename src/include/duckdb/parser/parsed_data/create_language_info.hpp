//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_language_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/create_info.hpp"

namespace duckdb {

class LanguageFunctionBinder;

//! Registers a function language in the system catalog. Language entries are transient: an
//! extension registers them at load and they are never written to a database file.
struct CreateLanguageInfo : public CreateInfo {
	explicit CreateLanguageInfo(Identifier name);

	//! The language name functions reference in CREATE FUNCTION ... LANGUAGE
	Identifier name;
	//! The binder that resolves calls to this language's members
	shared_ptr<LanguageFunctionBinder> binder;

public:
	unique_ptr<CreateInfo> Copy() const override;
	//! Language entries are transient and must never reach a serializer
	void Serialize(Serializer &serializer) const override;
};

} // namespace duckdb
