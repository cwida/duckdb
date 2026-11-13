//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/language_macro_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/macro_function.hpp"

namespace duckdb {

//! A macro member written in an extension-defined language: instead of a parsed SQL expression it
//! carries the language name and the verbatim source text. Core stores it opaquely, the language's
//! binder (a transient system catalog entry) resolves calls lazily.
class LanguageMacroFunction : public MacroFunction {
public:
	static constexpr const MacroType TYPE = MacroType::LANGUAGE_MACRO;

public:
	LanguageMacroFunction(string language, string source);
	LanguageMacroFunction();

	//! The language the member is written in, names the binder an extension registers
	string language;
	//! The verbatim source text, the durable form of the definition
	string source;

public:
	unique_ptr<MacroFunction> Copy() const override;

	string ToSQL() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<MacroFunction> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
