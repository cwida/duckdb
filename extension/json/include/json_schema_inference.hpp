//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_schema_inference.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_common.hpp"
#include "duckdb/common/types/type_map.hpp"

namespace duckdb {

struct JSONStructureDescription;
struct JSONColumnFormats;

//! The candidates of a type with formats that read every sampled value
struct JSONFormatCandidates {
	//! Ascending indexes into the DateFormatMap formats of the type
	vector<idx_t> formats;
	//! Whether the plain cast reads them too
	bool cast = false;

	bool Empty() const {
		return formats.empty() && !cast;
	}
};
struct DateFormatMap;
struct StrpTimeFormat;
class MutableDateFormatMap;

//! Sticky ISO-8601 offset observation across refinement vectors and parallel merges (issue #14919)
enum class TimestampOffsetState : uint8_t { UNKNOWN, WITH_OFFSET, WITHOUT_OFFSET, MIXED };

struct JSONStructureNode {
public:
	explicit JSONStructureNode(bool case_insensitive_keys);
	JSONStructureNode(const char *key_ptr, const size_t key_len, bool case_insensitive_keys);
	//! An empty node that matches object keys the way parent does
	static JSONStructureNode EmptyLike(const JSONStructureNode &parent);

	//! Disable copy constructors
	JSONStructureNode(const JSONStructureNode &other) = delete;
	JSONStructureNode &operator=(const JSONStructureNode &) = delete;
	//! Enable move constructors
	JSONStructureNode(JSONStructureNode &&other) noexcept;
	JSONStructureNode &operator=(JSONStructureNode &&) noexcept;

	JSONStructureDescription &GetOrCreateDescription(LogicalTypeId type);

	bool ContainsVarchar() const;
	//! The node of the field key, when this node is only ever an object and has that field
	optional_ptr<const JSONStructureNode> GetField(const string &field_key) const;
	void InitializeCandidateTypes(idx_t max_depth, bool convert_strings_to_integers,
	                              bool user_specified_timestamp_format = false, idx_t depth = 0);
	void RefineCandidateTypes(yyjson_val *vals[], idx_t val_count, Vector &string_vector, ArenaAllocator &allocator,
	                          MutableDateFormatMap &date_format_map, bool user_specified_timestamp_format = false);

private:
	void RefineCandidateTypesArray(yyjson_val *vals[], idx_t val_count, Vector &string_vector,
	                               ArenaAllocator &allocator, MutableDateFormatMap &date_format_map,
	                               bool user_specified_timestamp_format);
	void RefineCandidateTypesObject(yyjson_val *vals[], idx_t val_count, Vector &string_vector,
	                                ArenaAllocator &allocator, MutableDateFormatMap &date_format_map,
	                                bool user_specified_timestamp_format);
	void RefineCandidateTypesString(yyjson_val *vals[], idx_t val_count, Vector &string_vector,
	                                MutableDateFormatMap &date_format_map, bool user_specified_timestamp_format);
	void EliminateCandidateTypes(idx_t vec_count, Vector &string_vector, MutableDateFormatMap &date_format_map,
	                             bool user_specified_timestamp_format);
	bool CandidateReadsVector(LogicalTypeId type, idx_t vec_count, Vector &string_vector,
	                          MutableDateFormatMap &date_format_map, bool user_specified_timestamp_format);
	bool EliminateCandidateFormats(idx_t vec_count, Vector &string_vector, const Vector &result_vector,
	                               MutableDateFormatMap &date_format_map);

public:
	unique_ptr<string> key;
	bool initialized = false;
	vector<JSONStructureDescription> descriptions;
	idx_t count;
	idx_t null_count;
	//! Object keys that differ only in ASCII case are the same key (read_json case_insensitive_keys). Set when the
	//! node is created: the root from the reader options, every child from its parent
	bool case_insensitive_keys = false;
};

struct JSONStructureDescription {
public:
	JSONStructureDescription(LogicalTypeId type_p, bool case_insensitive_keys);
	//! Disable copy constructors
	JSONStructureDescription(const JSONStructureDescription &other) = delete;
	JSONStructureDescription &operator=(const JSONStructureDescription &) = delete;
	//! Enable move constructors
	JSONStructureDescription(JSONStructureDescription &&other) noexcept;
	JSONStructureDescription &operator=(JSONStructureDescription &&) noexcept;

	JSONStructureNode &GetOrCreateChild();
	JSONStructureNode &GetOrCreateChild(const char *key_ptr, size_t key_size);
	JSONStructureNode &GetOrCreateChild(yyjson_val *key, yyjson_val *val, bool ignore_errors, bool detect_geojson,
	                                    const idx_t depth);
	//! The most preferred candidate, TIMESTAMP_TZ only when every timestamp of the sample had an offset, or VARCHAR
	LogicalTypeId PreferredCandidateType() const;

public:
	//! Type of this description
	LogicalTypeId type = LogicalTypeId::INVALID;

	//! Map to children and children
	json_key_optional_case_map_t<idx_t> key_map;
	vector<JSONStructureNode> children;

	//! Candidate types that read every sampled value (if auto-detecting and type == VARCHAR), most preferred last
	vector<LogicalTypeId> candidate_types;
	//! Per candidate type with formats, what reads every sampled value; a type without an entry has not been tried,
	//! so all of its candidates remain
	type_id_map_t<JSONFormatCandidates> format_candidates;

	//! Whether any UBIGINT value exceeds the BIGINT range (i.e., >= 2^63)
	bool has_large_ubigint = false;

	//! MIXED clears the timestamp candidates (VARCHAR): neither TIMESTAMPTZ nor TIMESTAMP is safe
	TimestampOffsetState timestamp_offset_state = TimestampOffsetState::UNKNOWN;
};

struct JSONStructure {
public:
	static void ExtractStructure(yyjson_val *val, JSONStructureNode &node, bool ignore_errors, bool detect_geojson,
	                             const idx_t depth = 0);
	static void MergeNodes(JSONStructureNode &merged, const JSONStructureNode &node);
	static LogicalType StructureToType(ClientContext &context, const JSONStructureNode &node, idx_t max_depth,
	                                   double field_appearance_threshold, idx_t map_inference_threshold,
	                                   idx_t depth = 0, const LogicalType &null_type = LogicalType::JSON());
	//! The formats detected for the DATE and TIMESTAMP values of type, which StructureToType made from node
	static JSONColumnFormats StructureToFormats(const JSONStructureNode &node, const LogicalType &type);
};

} // namespace duckdb
