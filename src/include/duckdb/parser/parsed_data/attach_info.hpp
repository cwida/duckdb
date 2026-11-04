//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/attach_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/parser/parsed_data/external_resource_options.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

struct AttachInfo : public ParseInfo {
public:
	static constexpr const ParseInfoType TYPE = ParseInfoType::ATTACH_INFO;

public:
	AttachInfo() : ParseInfo(TYPE) {
	}

	//! The alias of the attached database
	Identifier name;
	//! The path to the attached database
	string path;
	//! The path expression to the attached database
	unique_ptr<ParsedExpression> parsed_path;
	//! Set of (key, value) options
	case_insensitive_map_t<unique_ptr<ParsedExpression>> parsed_options;
	//! Set of bound (key, value) options
	unordered_map<string, Value> options;
	//! What to do on create conflict
	OnCreateConflict on_conflict = OnCreateConflict::ERROR_ON_CONFLICT;
	//! Set iff parsed as `ATTACH TO [NEW TEMPORARY] EXTERNAL RESOURCE ...`: the resource to attach.
	unique_ptr<ExternalResourceOptions> external_resource;

public:
	//! Copies this AttachInfo and returns an unique pointer to the new AttachInfo.
	unique_ptr<AttachInfo> Copy() const;
	string ToString() const;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParseInfo> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
