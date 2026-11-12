//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_reader_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_common.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/enums/file_compression_type.hpp"
#include "json_enums.hpp"
#include "duckdb/common/types/type_map.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"

namespace duckdb {

struct DateFormatMap {
	friend class MutableDateFormatMap;

public:
	explicit DateFormatMap(type_id_map_t<vector<StrpTimeFormat>> candidate_formats_p)
	    : candidate_formats(std::move(candidate_formats_p)) {
	}

	bool HasFormats(LogicalTypeId type) const {
		return HasFormats(candidate_formats, type);
	}

	const StrpTimeFormat &GetFormat(LogicalTypeId type) const {
		D_ASSERT(candidate_formats.find(type) != candidate_formats.end());
		return candidate_formats.find(type)->second.back();
	}

	const vector<StrpTimeFormat> &GetFormats(LogicalTypeId type) const {
		D_ASSERT(candidate_formats.find(type) != candidate_formats.end());
		return candidate_formats.find(type)->second;
	}

public:
	static void AddFormat(type_id_map_t<vector<StrpTimeFormat>> &candidate_formats, LogicalTypeId type,
	                      const string &format_string) {
		auto &formats = candidate_formats[type];
		formats.emplace_back();
		formats.back().format_specifier = format_string;
		const auto error = StrpTimeFormat::ParseFormatSpecifier(formats.back().format_specifier, formats.back());
		if (!error.empty()) {
			formats.pop_back();
			throw InvalidInputException(error);
		}
	}

	static bool HasFormats(const type_id_map_t<vector<StrpTimeFormat>> &candidate_formats, LogicalTypeId type) {
		return candidate_formats.find(type) != candidate_formats.end();
	}

private:
	type_id_map_t<vector<StrpTimeFormat>> candidate_formats;
};

class MutableDateFormatMap {
public:
	explicit MutableDateFormatMap(DateFormatMap &date_format_map) : date_format_map(date_format_map) {
	}

	bool HasFormats(LogicalTypeId type) {
		lock_guard<mutex> lock(format_lock);
		return date_format_map.HasFormats(type);
	}

	idx_t NumberOfFormats(LogicalTypeId type) {
		lock_guard<mutex> lock(format_lock);
		return date_format_map.candidate_formats.at(type).size();
	}

	bool GetFormatAtIndex(LogicalTypeId type, idx_t index, StrpTimeFormat &format) {
		lock_guard<mutex> lock(format_lock);
		auto &formats = date_format_map.candidate_formats.at(type);
		if (index >= formats.size()) {
			return false;
		}
		format = formats[index];
		return true;
	}

private:
	mutex format_lock;
	DateFormatMap &date_format_map;
};

struct JSONReaderOptions {
	//! Scan type
	JSONScanType type = JSONScanType::READ_JSON;
	//! The format of the JSON
	JSONFormat format = JSONFormat::AUTO_DETECT;
	//! Whether record types in the JSON
	JSONRecordType record_type = JSONRecordType::AUTO_DETECT;
	//! Whether file is compressed or not, and if so which compression type
	FileCompressionType compression = FileCompressionType::AUTO_DETECT;
	//! Whether or not we should ignore malformed JSON (default to NULL)
	bool ignore_errors = false;
	//! Maximum JSON object size (defaults to 16MB minimum)
	idx_t maximum_object_size = 16777216;
	//! Whether we auto-detect a schema
	bool auto_detect = false;
	//! Whether "format" was explicitly specified
	bool format_specified = false;
	//! Whether "auto_detect" was explicitly specified
	bool auto_detect_specified = false;
	//! Sample size for detecting schema
	idx_t sample_size = idx_t(STANDARD_VECTOR_SIZE) * 10;
	//! Max depth we go to detect nested JSON schema (defaults to unlimited)
	idx_t max_depth = NumericLimits<idx_t>::Maximum();
	//! We divide the number of appearances of each JSON field by the auto-detection sample size
	//! If the average over the fields of an object is less than this threshold,
	//! we default to the MAP type with value type of merged field types
	double field_appearance_threshold = 0.1;
	//! The maximum number of files we sample to sample sample_size rows
	idx_t maximum_sample_files = 32;
	//! Whether we auto-detect and convert JSON strings to integers
	bool convert_strings_to_integers = false;
	//! Whether object keys that differ only in ASCII case are the same key, when detecting the schema and when
	//! matching keys to the columns
	bool case_insensitive_keys = false;
	//! Whether GeoJSON geometry fragments are inferred as the GEOMETRY type, and Features are unnested into
	//! geometry + property columns. Unset means "decide from the file name" (.geojson / .geojsonl)
	optional<bool> geojson;
	//! If a struct contains more fields than this threshold with at least 80% similar types,
	//! we infer it as MAP type
	idx_t map_inference_threshold = 200;
	//! User-provided list of names (in order)
	vector<Identifier> name_list;
	//! User-provided list of types (in order)
	vector<LogicalType> sql_type_list;
	//! Forced date/timestamp formats
	string date_format;
	string timestamp_format;
};

} // namespace duckdb
