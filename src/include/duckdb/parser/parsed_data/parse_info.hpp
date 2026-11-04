//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/parse_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {
class ParsedExpression;
class Value;

//! Render a statement's `(k v, ...)` option list, quoting each name, from the unbound and bound maps
//! alike. Empty string when there is nothing to render, so callers can append it unconditionally.
string RenderOptionList(const case_insensitive_map_t<unique_ptr<ParsedExpression>> &parsed_options,
                        const unordered_map<string, Value> &options);

enum class CatalogType : uint8_t;

enum class ParseInfoType : uint8_t {
	ALTER_INFO,
	ATTACH_INFO,
	COPY_INFO,
	CREATE_INFO,
	CREATE_SECRET_INFO,
	DETACH_INFO,
	DROP_INFO,
	BOUND_EXPORT_DATA,
	LOAD_INFO,
	PRAGMA_INFO,
	SHOW_SELECT_INFO,
	TRANSACTION_INFO,
	VACUUM_INFO,
	COMMENT_ON_INFO,
	COMMENT_ON_COLUMN_INFO,
	COPY_DATABASE_INFO,
	UPDATE_EXTENSIONS_INFO,
	CONNECT_INFO,
	DISCONNECT_INFO
};

struct ParseInfo {
	explicit ParseInfo(ParseInfoType info_type) : info_type(info_type) {
	}
	virtual ~ParseInfo() {
	}

	ParseInfoType info_type;

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}

	virtual void Serialize(Serializer &serializer) const;
	static unique_ptr<ParseInfo> Deserialize(Deserializer &deserializer);
	static string TypeToString(CatalogType type);
};

} // namespace duckdb
