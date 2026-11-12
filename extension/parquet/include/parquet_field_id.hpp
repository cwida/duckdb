#pragma once

#include <stdint.h>
#include <string>
#include <unordered_set>

#include "duckdb/common/serializer/buffered_file_writer.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

struct FieldID;
class Deserializer;
class Serializer;

struct ChildFieldIDs {
	ChildFieldIDs();
	ChildFieldIDs Copy() const;
	unique_ptr<identifier_map_t<FieldID>> ids;

	void Serialize(Serializer &serializer) const;
	static ChildFieldIDs Deserialize(Deserializer &source);
};

struct FieldID {
public:
	static constexpr const auto DUCKDB_FIELD_ID = "__duckdb_field_id";
	FieldID();
	explicit FieldID(int32_t field_id);
	FieldID Copy() const;
	bool set;
	int32_t field_id;
	ChildFieldIDs child_field_ids;

	void Serialize(Serializer &serializer) const;
	static FieldID Deserialize(Deserializer &source);

public:
	static void GenerateFieldIDs(ChildFieldIDs &field_ids, idx_t &field_id, const vector<Identifier> &names,
	                             const vector<LogicalType> &sql_types);
	static void GetFieldIDs(Context &context, const Value &field_ids_value, ChildFieldIDs &field_ids,
	                        unordered_set<uint32_t> &unique_field_ids,
	                        const case_insensitive_map_t<LogicalType> &name_to_type_map);
};

} // namespace duckdb
