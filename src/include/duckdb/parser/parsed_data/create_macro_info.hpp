//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/create_macro_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/create_function_info.hpp"
#include "duckdb/function/macro_function.hpp"

namespace duckdb {

struct CreateMacroInfo : public CreateFunctionInfo {
	explicit CreateMacroInfo(CatalogType type);

	vector<unique_ptr<MacroFunction>> macros;

public:
	unique_ptr<CreateInfo> Copy() const override;

	string ToString() const override;
	DUCKDB_API void Serialize(Serializer &serializer) const override;
	DUCKDB_API static unique_ptr<CreateInfo> Deserialize(Deserializer &deserializer);

	//! This is a weird function that exists only for backwards compatibility of serialization
	//! Essentially we used to only support a single function in the CreateMacroInfo
	//! In order to not break backwards/forwards compatibility, we serialize the first function in the old manner
	//! Extra functions are serialized if present in a separate field
	vector<unique_ptr<MacroFunction>> GetAllButFirstFunction() const;
	//! This is a weird constructor that exists only for serialization, similarly to GetAllButFirstFunction
	CreateMacroInfo(CatalogType type, unique_ptr<MacroFunction> function,
	                vector<unique_ptr<MacroFunction>> extra_functions);
};

} // namespace duckdb
