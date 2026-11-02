//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/connect_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/parser/parsed_data/external_resource_options.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

struct ConnectInfo : public ParseInfo {
public:
	static constexpr const ParseInfoType TYPE = ParseInfoType::CONNECT_INFO;

public:
	ConnectInfo() : ParseInfo(TYPE) {
	}

	//! Target — empty for bare `CONNECT;` and for `CONNECT LOCAL;` (in the latter case,
	//! `target_is_local` is true). Otherwise either an identifier (attached-db name) or the
	//! contents of a string literal (connection-string form).
	Identifier name;
	//! True iff the target was parsed as the LOCAL keyword (`CONNECT LOCAL;`). When true, `name`
	//! is empty and `name_is_string_literal` is false.
	bool target_is_local = false;
	//! True iff the target was parsed as a StringLiteral. Differentiates `CONNECT 'foo'`
	//! (connection-string form) from `CONNECT foo` (attached-db identifier) at the AST level,
	//! so ToString roundtrips the source form and downstream impl can dispatch correctly.
	bool name_is_string_literal = false;
	//! Set of parsed (key, value) options — only for the connection-string form. Bound in
	//! bind_connect and passed through to the implicit ATTACH performed by `CONNECT '<uri>'`.
	case_insensitive_map_t<unique_ptr<ParsedExpression>> parsed_options;
	//! Set of bound (key, value) options forwarded to the implicit ATTACH.
	unordered_map<string, Value> options;
	//! Set iff parsed as `CONNECT TO [NEW TEMPORARY] EXTERNAL RESOURCE ...`: the resource to connect to.
	unique_ptr<ExternalResourceOptions> external_resource;

public:
	unique_ptr<ConnectInfo> Copy() const;
	string ToString() const;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParseInfo> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
