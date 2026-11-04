//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/external_resource_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
class Serializer;
class Deserializer;

//! The `[NEW TEMPORARY] EXTERNAL RESOURCE '<type>' [(params)]` clause, owned by AttachInfo/ConnectInfo.
//! provider is its own field rather than a magic key in params: the TYPE is the registry key, not a param.
struct ExternalResourceOptions {
	//! Create params (key -> expression); transient — consumed at bind, then `params` holds them.
	case_insensitive_map_t<unique_ptr<ParsedExpression>> parsed_params;
	//! The resource type (provider) — the registered external-resource-type name to provision. A string
	//! literal, so it is fixed at parse time.
	string provider;
	//! Bound create params forwarded to the type's create function as a MAP(VARCHAR, VARCHAR).
	unordered_map<string, Value> params;
	//! Reference form: a bare identifier naming a registered resource to BORROW rather than provision,
	//! so no teardown is owned. Mutually exclusive with `provider`.
	string reference_name;

	unique_ptr<ExternalResourceOptions> Copy() const;
	//! Renders `NEW TEMPORARY EXTERNAL RESOURCE '<type>' [(k v, ...)]`, or the reference form.
	string ToString() const;

	void Serialize(Serializer &serializer) const;
	static unique_ptr<ExternalResourceOptions> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
