//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/column_index.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "json_common.hpp"
#include "json_enums.hpp"

namespace duckdb {

struct DateFormatMap;
class JSONReader;

//! Options for error handling while transforming JSON
struct JSONTransformOptions {
public:
	JSONTransformOptions();
	JSONTransformOptions(bool strict_cast, bool error_duplicate_key, bool error_missing_key, bool error_unknown_key);

public:
	//! Throws an error if the cast doesn't work (instead of NULL-ing it)
	bool strict_cast = false;
	//! Throws an error if there is a duplicate key (instead of ignoring it)
	bool error_duplicate_key = false;
	//! Throws an error if a key is missing (instead of NULL-ing it)
	bool error_missing_key = false;
	//! Throws an error if an object has a key we didn't know about
	bool error_unknown_key = false;
	//! How object keys are matched to names
	JSONKeyMatch key_match = JSONKeyMatch::EXACT;

	//! Whether to delay the error when transforming (e.g., when non-strict casting or reading from file)
	bool delay_error = false;
	//! Whether the documents keep numbers as text (JSONCommon::NumbersAsRaw): nested values become strings through
	//! JSONRawNumberWriter, so that they read the same as without the DECIMAL column
	bool numbers_as_raw = false;
	//! Date format used for parsing (can be NULL)
	optional_ptr<const DateFormatMap> date_format_map = nullptr;
	//! String to store errors in
	string error_message;
	//! Index of the object where the error occurred
	idx_t object_index = DConstants::INVALID_INDEX;
	//! Cast parameters
	CastParameters parameters;

public:
	void Serialize(Serializer &serializer) const;
	static JSONTransformOptions Deserialize(Deserializer &deserializer);
};

struct TryParseDate {
	template <class T>
	static inline bool Operation(const StrpTimeFormat &format, const string_t &input, T &result,
	                             string &error_message) {
		return format.TryParseDate(input, result, error_message);
	}
};

struct TryParseTimeStamp {
	template <class T>
	static inline bool Operation(const StrpTimeFormat &format, const string_t &input, T &result,
	                             string &error_message) {
		return format.TryParseTimestamp(input, result, error_message);
	}
};

//! The DATE or TIMESTAMP format auto-detection chose for a column, and those of its nested values, in the shape of
//! the column's type: one child per STRUCT field, one for the element of a LIST and one for the value of a MAP
struct JSONColumnFormats {
	//! Index into the DateFormatMap formats of the column's type, invalid when no format was detected
	optional_idx format_index;
	//! Whether the column is read with the plain cast instead, because no format read every sampled value
	bool cast = false;
	vector<JSONColumnFormats> children;

	static optional_ptr<const JSONColumnFormats> GetChild(optional_ptr<const JSONColumnFormats> formats, idx_t i) {
		if (!formats || i >= formats->children.size()) {
			return nullptr;
		}
		return &formats->children[i];
	}
};

struct JSONTransform {
	static bool Transform(yyjson_val *vals[], yyjson_alc *alc, Vector &result, const idx_t count,
	                      JSONTransformOptions &options, optional_ptr<const ColumnIndex> column_index,
	                      optional_ptr<const JSONColumnFormats> formats);
	//! formats has one child per column, at the column's primary index when column_indices is given
	static bool TransformObject(yyjson_val *objects[], yyjson_alc *alc, const idx_t count, const vector<string> &names,
	                            const vector<Vector *> &result_vectors, JSONTransformOptions &options,
	                            optional_ptr<const vector<ColumnIndex>> column_indices, bool error_unknown_key,
	                            optional_ptr<const JSONColumnFormats> formats);
	static bool GetStringVector(yyjson_val *vals[], const idx_t count, const LogicalType &target, Vector &string_vector,
	                            JSONTransformOptions &options);
};

} // namespace duckdb
