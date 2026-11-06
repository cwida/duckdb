//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/statistics/stats_struct_fields.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

//! The fields of a struct value in the shape BaseStatistics::ToStruct produces, read by name (or index)
class StatsStructFields {
public:
	//! Throws unless the value is a non-NULL STRUCT (a VARIANT object is converted); `path` names it in errors
	DUCKDB_API StatsStructFields(const Value &value, string path);

public:
	//! The non-NULL value of a field, or nullptr when the struct has no such field or it is NULL
	DUCKDB_API optional_ptr<const Value> Get(const string &name);
	//! The value of a field cast to `type`; false when it is absent or NULL, throws when it does not cast
	DUCKDB_API bool TryGet(const string &name, const LogicalType &type, Value &result);

	DUCKDB_API idx_t FieldCount() const;
	DUCKDB_API const string &FieldName(idx_t index) const;
	//! The value of a field by index, which counts as read; nullptr when it is NULL
	DUCKDB_API optional_ptr<const Value> FieldValue(idx_t index);
	//! The type of a field's value (of what a VARIANT holds), which does not count as read
	DUCKDB_API LogicalType FieldType(idx_t index) const;

	//! The dotted path of this struct, and of one of its fields, for error messages
	DUCKDB_API const string &Path() const;
	DUCKDB_API string FieldPath(const string &name) const;
	//! Throws an InvalidInputException about this struct, or about one of its fields
	[[noreturn]] DUCKDB_API void Throw(const string &problem) const;
	[[noreturn]] DUCKDB_API void Throw(const string &name, const string &problem) const;
	//! Throws for fields that were never read: statistics of `type` have no such part
	DUCKDB_API void VerifyAllRead(const LogicalType &type) const;

	//! A VARIANT value as a plain value (objects become STRUCTs, arrays LISTs); other values unchanged
	DUCKDB_API static Value Plain(const Value &value);

private:
	Value value;
	string path;
	vector<bool> read;
};

} // namespace duckdb
