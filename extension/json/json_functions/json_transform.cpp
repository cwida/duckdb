#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "json_transform.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/string_cast.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/value_map.hpp"
#include "json_geojson.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "json_functions.hpp"
#include "json_scan.hpp"

namespace duckdb {

JSONTransformOptions::JSONTransformOptions() : parameters(false, &error_message) {
}

JSONTransformOptions::JSONTransformOptions(bool strict_cast_p, bool error_duplicate_key_p, bool error_missing_key_p,
                                           bool error_unknown_key_p)
    : strict_cast(strict_cast_p), error_duplicate_key(error_duplicate_key_p), error_missing_key(error_missing_key_p),
      error_unknown_key(error_unknown_key_p), parameters(false, &error_message) {
}

//! Forward declaration for recursion
static LogicalType StructureStringToType(yyjson_val *val, ClientContext &context);

static LogicalType StructureStringToTypeArray(yyjson_val *arr, ClientContext &context) {
	if (yyjson_arr_size(arr) != 1) {
		throw BinderException("Too many values in array of JSON structure");
	}
	return LogicalType::LIST(StructureStringToType(yyjson_arr_get_first(arr), context));
}

static LogicalType StructureToTypeObject(yyjson_val *obj, ClientContext &context) {
	// STRUCT field names are case-insensitive
	identifier_set_t names;
	child_list_t<LogicalType> child_types;
	size_t idx, max;
	yyjson_val *key, *val;
	yyjson_obj_foreach(obj, idx, max, key, val) {
		val = yyjson_obj_iter_get_val(key);
		string key_str(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
		if (key_str.empty()) {
			throw BinderException("Empty key in object in JSON structure");
		}
		if (!names.insert(Identifier(key_str)).second) {
			JSONCommon::ThrowValFormatError("Duplicate keys in object in JSON structure: %s", val);
		}
		child_types.emplace_back(key_str, StructureStringToType(val, context));
	}
	D_ASSERT(yyjson_obj_size(obj) == names.size());
	if (child_types.empty()) {
		throw BinderException("Empty object in JSON structure");
	}
	return LogicalType::STRUCT(child_types);
}

static LogicalType StructureStringToType(yyjson_val *val, ClientContext &context) {
	switch (yyjson_get_tag(val)) {
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
		return StructureStringToTypeArray(val, context);
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
		return StructureToTypeObject(val, context);
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
		return TransformStringToLogicalType(unsafe_yyjson_get_str(val), context);
	default:
		throw BinderException("invalid JSON structure");
	}
}

static unique_ptr<FunctionData> JSONTransformBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	D_ASSERT(bound_function.GetArguments().size() == 2);
	auto structure_val = input.GetConstant(1);
	if (structure_val.IsNull() || arguments[1]->GetReturnType() == LogicalTypeId::SQLNULL) {
		bound_function.SetReturnType(LogicalTypeId::SQLNULL);
	} else {
		auto json_structure_val = structure_val.DefaultTryCastAs(LogicalType::JSON());
		if (!json_structure_val) {
			throw BinderException("Cannot cast JSON structure to string");
		}
		auto structure_string = json_structure_val->GetValueUnsafe<string_t>();
		JSONAllocator json_allocator(Allocator::DefaultAllocator());
		auto doc = JSONCommon::ReadDocument(structure_string, JSONCommon::READ_FLAG, json_allocator.GetYYAlc());
		bound_function.SetReturnType(StructureStringToType(doc->root, context));
	}
	return make_uniq<VariableReturnBindData>(bound_function.GetReturnType());
}

static inline string_t GetString(yyjson_val *val) {
	return string_t(unsafe_yyjson_get_str(val), unsafe_yyjson_get_len(val));
}

template <class T, class OP = TryCast>
static inline bool GetValueNumerical(yyjson_val *val, T &result, JSONTransformOptions &options) {
	D_ASSERT(unsafe_yyjson_get_tag(val) != (YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE));
	bool success;
	switch (unsafe_yyjson_get_tag(val)) {
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
		success = OP::template Operation<string_t, T>(GetString(val), result, options.strict_cast);
		break;
	case YYJSON_TYPE_RAW | YYJSON_SUBTYPE_NONE: {
		// a number kept as text for a DECIMAL column reads as the default flags would have read it
		yyjson_val number;
		if (JSONCommon::ReadRawNumber(val, number)) {
			return GetValueNumerical<T, OP>(&number, result, options);
		}
		success = OP::template Operation<string_t, T>(GetString(val), result, options.strict_cast);
		break;
	}
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
		success = false;
		break;
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
		success = OP::template Operation<bool, T>(unsafe_yyjson_get_bool(val), result, options.strict_cast);
		break;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
		success = OP::template Operation<uint64_t, T>(unsafe_yyjson_get_uint(val), result, options.strict_cast);
		break;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
		success = OP::template Operation<int64_t, T>(unsafe_yyjson_get_sint(val), result, options.strict_cast);
		break;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
		success = OP::template Operation<double, T>(unsafe_yyjson_get_real(val), result, options.strict_cast);
		break;
	default:
		throw InternalException("Unknown yyjson tag in GetValueNumerical");
	}
	if (!success && options.strict_cast) {
		options.error_message =
		    StringUtil::Format("Failed to cast value to numerical: %s", JSONCommon::ValToString(val, 50));
	}
	return success;
}

template <class T, class OP = TryCastToDecimal>
static inline bool GetValueDecimal(yyjson_val *val, T &result, uint8_t w, uint8_t s, JSONTransformOptions &options) {
	D_ASSERT(unsafe_yyjson_get_tag(val) != (YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE));
	bool success;
	switch (unsafe_yyjson_get_tag(val)) {
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
	case YYJSON_TYPE_RAW | YYJSON_SUBTYPE_NONE:
		success = OP::template Operation<string_t, T>(GetString(val), result, options.parameters, w, s);
		break;
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
		success = false;
		break;
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
		success = OP::template Operation<bool, T>(unsafe_yyjson_get_bool(val), result, options.parameters, w, s);
		break;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
		success = OP::template Operation<uint64_t, T>(unsafe_yyjson_get_uint(val), result, options.parameters, w, s);
		break;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
		success = OP::template Operation<int64_t, T>(unsafe_yyjson_get_sint(val), result, options.parameters, w, s);
		break;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
		success = OP::template Operation<double, T>(unsafe_yyjson_get_real(val), result, options.parameters, w, s);
		break;
	default:
		throw InternalException("Unknown yyjson tag in GetValueString");
	}
	if (!success && options.strict_cast) {
		options.error_message =
		    StringUtil::Format("Failed to cast value to decimal: %s", JSONCommon::ValToString(val, 50));
	}
	return success;
}

static inline bool GetValueString(yyjson_val *val, yyjson_alc *alc, string_t &result, Vector &vector,
                                  optional_ptr<JSONRawNumberWriter> raw_writer) {
	D_ASSERT(unsafe_yyjson_get_tag(val) != (YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE));
	switch (unsafe_yyjson_get_tag(val)) {
	case YYJSON_TYPE_RAW | YYJSON_SUBTYPE_NONE: {
		// a number kept as text for a DECIMAL column reads as the default flags would have written it
		yyjson_val number;
		if (JSONCommon::ReadRawNumber(val, number)) {
			return GetValueString(&number, alc, result, vector, raw_writer);
		}
		result = string_t(unsafe_yyjson_get_str(val), unsafe_yyjson_get_len(val));
		return true;
	}
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
		result = string_t(unsafe_yyjson_get_str(val), unsafe_yyjson_get_len(val));
		return true;
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
		result = raw_writer ? raw_writer->Write(val) : JSONCommon::WriteVal<yyjson_val>(val, alc);
		return true;
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
		result = StringCast::Operation<bool>(unsafe_yyjson_get_bool(val), StringVector::GetStringHeap(vector));
		return true;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
		result = StringCast::Operation<uint64_t>(unsafe_yyjson_get_uint(val), StringVector::GetStringHeap(vector));
		return true;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
		result = StringCast::Operation<int64_t>(unsafe_yyjson_get_sint(val), StringVector::GetStringHeap(vector));
		return true;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
		result = StringCast::Operation<double>(unsafe_yyjson_get_real(val), StringVector::GetStringHeap(vector));
		return true;
	default:
		throw InternalException("Unknown yyjson tag in GetValueString");
	}
}

template <class T>
static bool TransformNumerical(yyjson_val *vals[], Vector &result, const idx_t count, JSONTransformOptions &options) {
	auto data = FlatVector::GetDataMutable<T>(result);
	auto &validity = FlatVector::ValidityMutable(result);

	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (!val || unsafe_yyjson_is_null(val)) {
			validity.SetInvalid(i);
		} else if (!GetValueNumerical<T>(val, data[i], options)) {
			validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.object_index = i;
				success = false;
			}
		}
	}
	return success;
}

template <class T>
static bool TransformDecimal(yyjson_val *vals[], Vector &result, const idx_t count, uint8_t width, uint8_t scale,
                             JSONTransformOptions &options) {
	auto data = FlatVector::GetDataMutable<T>(result);
	auto &validity = FlatVector::ValidityMutable(result);

	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (!val || unsafe_yyjson_is_null(val)) {
			validity.SetInvalid(i);
		} else if (!GetValueDecimal<T>(val, data[i], width, scale, options)) {
			validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.object_index = i;
				success = false;
			}
		}
	}
	return success;
}

bool JSONTransform::GetStringVector(yyjson_val *vals[], const idx_t count, const LogicalType &target,
                                    Vector &string_vector, JSONTransformOptions &options) {
	if (count > STANDARD_VECTOR_SIZE) {
		string_vector.Initialize(VectorDataInitialization::UNINITIALIZED, count);
	}
	auto data = FlatVector::GetDataMutable<string_t>(string_vector);
	auto &validity = FlatVector::ValidityMutable(string_vector);
	validity.SetAllValid(count);

	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (!val || unsafe_yyjson_is_null(val)) {
			validity.SetInvalid(i);
			continue;
		}

		if (!unsafe_yyjson_is_str(val)) {
			validity.SetInvalid(i);
			if (success && options.strict_cast && !unsafe_yyjson_is_str(val)) {
				options.error_message = StringUtil::Format(
				    "Unable to cast '%s' to %s", JSONCommon::ValToString(val, 50), EnumUtil::ToString(target.id()));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		data[i] = GetString(val);
	}
	return success;
}

static bool TransformFromString(yyjson_val *vals[], Vector &result, const idx_t count, JSONTransformOptions &options) {
	Vector string_vector(LogicalTypeId::VARCHAR, count);

	bool success = true;
	if (!JSONTransform::GetStringVector(vals, count, result.GetType(), string_vector, options)) {
		success = false;
	}

	if (!VectorOperations::DefaultTryCast(string_vector, result, count, &options.error_message) &&
	    options.strict_cast) {
		options.object_index = 0; // Can't get line number information here
		options.error_message +=
		    "\n If this error occurred during read_json, line/object number information is approximate";
		success = false;
	}
	return success;
}

//! GEOMETRY accepts both WKT strings and GeoJSON geometry fragments. A JSON object is never valid WKT, so the two
//! are unambiguous and no option is needed to tell them apart.
static bool TransformGeometry(yyjson_val *vals[], Vector &result, const idx_t count, JSONTransformOptions &options) {
	bool any_object = false;
	for (idx_t i = 0; i < count; i++) {
		if (vals[i] && unsafe_yyjson_is_obj(vals[i])) {
			any_object = true;
			break;
		}
	}
	if (!any_object) {
		// Nothing but WKT here, so use the regular string cast
		return TransformFromString(vals, result, count, options);
	}

	auto data = FlatVector::GetDataMutable<string_t>(result);
	auto &validity = FlatVector::ValidityMutable(result);
	validity.SetAllValid(count);

	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (!val || unsafe_yyjson_is_null(val)) {
			validity.SetInvalid(i);
			continue;
		}

		string error;
		if (unsafe_yyjson_is_obj(val)) {
			if (JSONGeometry::FromGeoJSON(val, data[i], result, error)) {
				continue;
			}
		} else if (unsafe_yyjson_is_str(val)) {
			try {
				if (Geometry::FromString(GetString(val), data[i], result, true)) {
					continue;
				}
			} catch (std::exception &ex) {
				ErrorData error_data(ex);
				error = error_data.RawMessage();
			}
		}

		if (error.empty()) {
			error = StringUtil::Format("Unable to cast '%s' to GEOMETRY", JSONCommon::ValToString(val, 50));
		}
		validity.SetInvalid(i);
		if (success && options.strict_cast) {
			options.error_message = error;
			options.object_index = i;
			success = false;
		}
	}
	return success;
}

static bool TryOneFormat(const StrpTimeFormat &fmt, LogicalTypeId type_id, const string_t &input, Vector &result,
                         idx_t i, string &error_message) {
	if (type_id == LogicalTypeId::DATE) {
		date_t val;
		if (!TryParseDate::Operation<date_t>(fmt, input, val, error_message)) {
			return false;
		}
		FlatVector::GetDataMutable<date_t>(result)[i] = val;
	} else {
		timestamp_t val;
		if (!TryParseTimeStamp::Operation<timestamp_t>(fmt, input, val, error_message)) {
			return false;
		}
		FlatVector::GetDataMutable<timestamp_t>(result)[i] = val;
	}
	return true;
}

template <class OP, class T>
static bool TransformStringWithFormat(const Vector &string_vector, const StrpTimeFormat &format, const idx_t count,
                                      Vector &result, JSONTransformOptions &options) {
	const auto source_strings = FlatVector::GetData<string_t>(string_vector);
	const auto &source_validity = FlatVector::Validity(string_vector);

	auto target_vals = FlatVector::GetDataMutable<T>(result);
	auto &target_validity = FlatVector::ValidityMutable(result);

	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		if (!source_validity.RowIsValid(i)) {
			target_validity.SetInvalid(i);
		} else if (!OP::template Operation<T>(format, source_strings[i], target_vals[i], options.error_message)) {
			target_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.object_index = i;
				success = false;
			}
		}
	}
	return success;
}

static bool TransformFromStringWithFormat(yyjson_val *vals[], Vector &result, const idx_t count,
                                          JSONTransformOptions &options) {
	Vector string_vector(LogicalTypeId::VARCHAR, count);
	bool success = true;
	if (!JSONTransform::GetStringVector(vals, count, result.GetType(), string_vector, options)) {
		success = false;
	}

	const auto result_type_id = result.GetType().id();
	D_ASSERT(result_type_id == LogicalTypeId::DATE || result_type_id == LogicalTypeId::TIMESTAMP);
	const auto &formats = options.date_format_map->GetFormats(result_type_id);
	const auto source_strings = FlatVector::GetData<string_t>(string_vector);
	const auto &source_validity = FlatVector::Validity(string_vector);
	auto &target_validity = FlatVector::ValidityMutable(result);

	// Probe first non-null value to find the most-specific matching format for this vector
	idx_t matched_idx = DConstants::INVALID_INDEX;
	for (idx_t i = 0; i < count && matched_idx == DConstants::INVALID_INDEX; i++) {
		if (!source_validity.RowIsValid(i)) {
			continue;
		}
		string error_message;
		for (idx_t j = formats.size(); j > 0; j--) {
			if (TryOneFormat(formats[j - 1], result_type_id, source_strings[i], result, i, error_message)) {
				matched_idx = j - 1;
				break;
			}
		}
	}

	const idx_t parse_limit = (matched_idx != DConstants::INVALID_INDEX) ? matched_idx + 1 : formats.size();
	for (idx_t i = 0; i < count; i++) {
		if (!source_validity.RowIsValid(i)) {
			target_validity.SetInvalid(i);
			continue;
		}
		bool parsed = false;
		string error_message;
		// Reverse iter formats, beginning at matched_idx until one parses
		for (idx_t j = parse_limit; j > 0 && !parsed; j--) {
			parsed = TryOneFormat(formats[j - 1], result_type_id, source_strings[i], result, i, error_message);
		}
		if (!parsed) {
			target_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.object_index = i;
				success = false;
			}
		}
	}
	return success;
}

static bool TransformToString(yyjson_val *vals[], yyjson_alc *alc, Vector &result, const idx_t count,
                              const JSONTransformOptions &options) {
	auto data = FlatVector::GetDataMutable<string_t>(result);
	auto &validity = FlatVector::ValidityMutable(result);
	JSONRawNumberWriter raw_writer(alc);
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (!val || unsafe_yyjson_is_null(vals[i])) {
			validity.SetInvalid(i);
		} else if (!GetValueString(val, alc, data[i], result, options.numbers_as_raw ? &raw_writer : nullptr)) {
			validity.SetInvalid(i);
		}
	}
	// Can always transform to string
	return true;
}

//! Assigns the values of each object's keys to the columns; templated on the key map so that the exact match keeps
//! its own hash and equality
template <class KEY_MAP>
static bool AssignObjectValues(yyjson_val *objects[], yyjson_alc *alc, const idx_t count, const vector<string> &names,
                               vector<yyjson_val **> &nested_vals, JSONTransformOptions &options,
                               bool error_unknown_key) {
	const idx_t column_count = names.size();
	// Build hash map from key to column index so we don't have to linearly search using the key
	KEY_MAP key_map;
	for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
		key_map.insert({{names[col_idx].c_str(), names[col_idx].length()}, col_idx});
	}

	idx_t found_key_count;
	auto found_keys = JSONCommon::AllocateArray<bool>(alc, column_count);

	bool success = true;

	size_t idx, max;
	yyjson_val *key, *val;
	for (idx_t i = 0; i < count; i++) {
		const auto &obj = objects[i];
		if (!obj || unsafe_yyjson_is_null(obj)) {
			// Set nested val to null so the recursion doesn't break
			for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
				nested_vals[col_idx][i] = nullptr;
			}
			continue;
		}

		if (!unsafe_yyjson_is_obj(obj)) {
			// Set nested val to null so the recursion doesn't break
			for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
				nested_vals[col_idx][i] = nullptr;
			}
			if (success && options.strict_cast && obj) {
				options.error_message =
				    StringUtil::Format("Expected OBJECT, but got %s: %s", JSONCommon::ValTypeToString(obj),
				                       JSONCommon::ValToString(obj, 50));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		found_key_count = 0;
		memset(found_keys, false, column_count);
		yyjson_obj_foreach(objects[i], idx, max, key, val) {
			auto key_ptr = unsafe_yyjson_get_str(key);
			auto key_len = unsafe_yyjson_get_len(key);
			auto it = key_map.find({key_ptr, key_len});
			if (it != key_map.end()) {
				const auto &col_idx = it->second;
				if (found_keys[col_idx]) {
					if (success && options.error_duplicate_key) {
						options.error_message =
						    StringUtil::Format("Object %s has duplicate key \"%s\"",
						                       JSONCommon::ValToString(objects[i], 50), string(key_ptr, key_len));
						options.object_index = i;
						success = false;
					}
				} else {
					nested_vals[col_idx][i] = val;
					found_keys[col_idx] = true;
					found_key_count++;
				}
			} else if (success && error_unknown_key && options.error_unknown_key) {
				options.error_message =
				    StringUtil::Format("Object %s has unknown key \"%s\"", JSONCommon::ValToString(objects[i], 50),
				                       string(key_ptr, key_len));
				options.object_index = i;
				success = false;
			}
		}

		if (found_key_count != column_count) {
			// If 'error_missing_key, we throw an error if one of the keys was not found.
			// If not, we set the nested val to null so the recursion doesn't break
			for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
				if (found_keys[col_idx]) {
					continue;
				}
				nested_vals[col_idx][i] = nullptr;

				if (success && options.error_missing_key) {
					options.error_message = StringUtil::Format("Object %s does not have key \"%s\"",
					                                           JSONCommon::ValToString(objects[i], 50), names[col_idx]);
					options.object_index = i;
					success = false;
				}
			}
		}
	}

	return success;
}

bool JSONTransform::TransformObject(yyjson_val *objects[], yyjson_alc *alc, const idx_t count,
                                    const vector<string> &names, const vector<Vector *> &result_vectors,
                                    JSONTransformOptions &options,
                                    optional_ptr<const vector<ColumnIndex>> column_indices, bool error_unknown_key) {
	if (column_indices && column_indices->empty()) {
		column_indices = nullptr;
	}
	D_ASSERT(alc);
	D_ASSERT(names.size() == result_vectors.size());
	D_ASSERT(!column_indices || column_indices->size() == names.size());
	const idx_t column_count = names.size();

	vector<yyjson_val **> nested_vals;
	nested_vals.reserve(column_count);
	for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
		nested_vals.push_back(JSONCommon::AllocateArray<yyjson_val *>(alc, count));
	}

	bool success = options.key_match == JSONKeyMatch::CASE_INSENSITIVE
	                   ? AssignObjectValues<json_key_ci_map_t<idx_t>>(objects, alc, count, names, nested_vals, options,
	                                                                  error_unknown_key)
	                   : AssignObjectValues<json_key_map_t<idx_t>>(objects, alc, count, names, nested_vals, options,
	                                                               error_unknown_key);

	for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
		auto child_column_index = column_indices ? &(*column_indices)[col_idx] : nullptr;
		if (!JSONTransform::Transform(nested_vals[col_idx], alc, *result_vectors[col_idx], count, options,
		                              child_column_index)) {
			success = false;
		}
	}

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}

	return success;
}

static bool TransformObjectInternal(yyjson_val *objects[], yyjson_alc *alc, Vector &result, const idx_t count,
                                    JSONTransformOptions &options, optional_ptr<const ColumnIndex> column_index) {
	if (column_index && column_index->ChildIndexCount() == 0) {
		column_index = nullptr;
	}

	// Set validity first
	auto &result_validity = FlatVector::ValidityMutable(result);
	for (idx_t i = 0; i < count; i++) {
		const auto &obj = objects[i];
		if (!obj || unsafe_yyjson_is_null(obj)) {
			result_validity.SetInvalid(i);
		}
	}

	// Get child vectors and names
	auto &child_vs = StructVector::GetEntries(result);
	vector<string> child_names;
	vector<Vector *> child_vectors;

	const auto child_count = column_index ? column_index->ChildIndexCount() : child_vs.size();
	child_names.reserve(child_count);
	child_vectors.reserve(child_count);

	unordered_set<idx_t> projected_indices;
	for (idx_t child_i = 0; child_i < child_count; child_i++) {
		const auto actual_i = column_index ? column_index->GetChildIndex(child_i).GetPrimaryIndex() : child_i;
		projected_indices.insert(actual_i);

		child_names.emplace_back(StructType::GetChildName(result.GetType(), actual_i));
		child_vectors.push_back(&child_vs[actual_i]);
	}

	for (idx_t child_i = 0; child_i < child_vs.size(); child_i++) {
		if (projected_indices.find(child_i) == projected_indices.end()) {
			ConstantVector::SetNull(child_vs[child_i], count_t(count));
		}
	}

	auto child_indices = column_index ? &column_index->GetChildIndexes() : nullptr;
	const auto error_unknown_key = child_count == child_vs.size(); // Nothing projected out, error if unknown
	return JSONTransform::TransformObject(objects, alc, count, child_names, child_vectors, options, child_indices,
	                                      error_unknown_key);
}

static bool TransformArrayToList(yyjson_val *arrays[], yyjson_alc *alc, Vector &result, const idx_t count,
                                 JSONTransformOptions &options) {
	bool success = true;

	// Initialize list vector
	auto list_entries = FlatVector::GetDataMutable<list_entry_t>(result);
	auto &list_validity = FlatVector::ValidityMutable(result);
	idx_t offset = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto &arr = arrays[i];
		if (!arr || unsafe_yyjson_is_null(arr)) {
			list_validity.SetInvalid(i);
			continue;
		}

		if (!unsafe_yyjson_is_arr(arr)) {
			list_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message =
				    StringUtil::Format("Expected ARRAY, but got %s: %s", JSONCommon::ValTypeToString(arrays[i]),
				                       JSONCommon::ValToString(arrays[i], 50));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		auto &entry = list_entries[i];
		entry.offset = offset;
		entry.length = unsafe_yyjson_get_len(arr);
		offset += entry.length;
	}
	ListVector::SetListSize(result, offset);
	ListVector::Reserve(result, offset);

	// Initialize array for the nested values
	auto nested_vals = JSONCommon::AllocateArray<yyjson_val *>(alc, offset);

	// Get array values
	size_t idx, max;
	yyjson_val *val;
	idx_t list_i = 0;
	for (idx_t i = 0; i < count; i++) {
		if (!list_validity.RowIsValid(i)) {
			continue; // We already marked this as invalid
		}
		yyjson_arr_foreach(arrays[i], idx, max, val) {
			nested_vals[list_i] = val;
			list_i++;
		}
	}
	D_ASSERT(list_i == offset);

	if (!success) {
		// Set object index in case of error in nested list so we can get accurate line number information
		for (idx_t i = 0; i < count; i++) {
			if (!list_validity.RowIsValid(i)) {
				continue;
			}
			auto &entry = list_entries[i];
			if (options.object_index >= entry.offset && options.object_index < entry.offset + entry.length) {
				options.object_index = i;
			}
		}
	}

	// Transform array values
	if (!JSONTransform::Transform(nested_vals, alc, ListVector::GetChildMutable(result), offset, options, nullptr)) {
		success = false;
	}

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}

	return success;
}

static bool TransformArrayToTuple(yyjson_val *arrays[], yyjson_alc *alc, Vector &result, const idx_t count,
                                  JSONTransformOptions &options) {
	// A TUPLE is serialized as a JSON array - read each array element positionally into the matching struct child
	bool success = true;
	auto &result_validity = FlatVector::ValidityMutable(result);
	auto &child_vs = StructVector::GetEntries(result);
	const idx_t child_count = child_vs.size();

	// nested_vals[child_i * count + i] holds the JSON value for child child_i of row i
	auto nested_vals = JSONCommon::AllocateArray<yyjson_val *>(alc, child_count * count);
	for (idx_t i = 0; i < count; i++) {
		const auto &arr = arrays[i];
		bool valid = arr && !unsafe_yyjson_is_null(arr) && unsafe_yyjson_is_arr(arr);
		if (!valid) {
			result_validity.SetInvalid(i);
			if (success && options.strict_cast && arr && !unsafe_yyjson_is_null(arr)) {
				options.error_message =
				    StringUtil::Format("Expected ARRAY, but got %s: %s", JSONCommon::ValTypeToString(arr),
				                       JSONCommon::ValToString(arr, 50));
				options.object_index = i;
				success = false;
			}
			for (idx_t child_i = 0; child_i < child_count; child_i++) {
				nested_vals[child_i * count + i] = nullptr;
			}
			continue;
		}
		// gather up to child_count elements - missing elements become NULL, extra elements are ignored
		size_t idx, max;
		yyjson_val *val;
		idx_t child_i = 0;
		yyjson_arr_foreach(arr, idx, max, val) {
			if (child_i < child_count) {
				nested_vals[child_i * count + i] = val;
			}
			child_i++;
		}
		for (; child_i < child_count; child_i++) {
			nested_vals[child_i * count + i] = nullptr;
		}
	}

	for (idx_t child_i = 0; child_i < child_count; child_i++) {
		if (!JSONTransform::Transform(nested_vals + child_i * count, alc, child_vs[child_i], count, options, nullptr)) {
			success = false;
		}
	}

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}
	return success;
}

static bool TransformArrayToArray(yyjson_val *arrays[], yyjson_alc *alc, Vector &result, const idx_t count,
                                  JSONTransformOptions &options) {
	bool success = true;

	// Initialize array vector
	auto &result_validity = FlatVector::ValidityMutable(result);
	auto array_size = ArrayType::GetSize(result.GetType());
	auto child_count = count * array_size;

	for (idx_t i = 0; i < count; i++) {
		const auto &arr = arrays[i];
		if (!arr || unsafe_yyjson_is_null(arr)) {
			result_validity.SetInvalid(i);
			continue;
		}

		if (!unsafe_yyjson_is_arr(arr)) {
			result_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message =
				    StringUtil::Format("Expected ARRAY, but got %s: %s", JSONCommon::ValTypeToString(arrays[i]),
				                       JSONCommon::ValToString(arrays[i], 50));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		auto json_arr_size = unsafe_yyjson_get_len(arr);
		if (json_arr_size != array_size) {
			result_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message =
				    StringUtil::Format("Expected array of size %u, but got '%s' with size %u", array_size,
				                       JSONCommon::ValToString(arrays[i], 50), json_arr_size);
				options.object_index = i;
				success = false;
			}
			continue;
		}
	}

	// Initialize array for the nested values
	auto nested_vals = JSONCommon::AllocateArray<yyjson_val *>(alc, child_count);

	// Get array values
	size_t idx, max;
	yyjson_val *val;
	idx_t nested_elem_idx = 0;
	for (idx_t i = 0; i < count; i++) {
		if (!result_validity.RowIsValid(i)) {
			// We already marked this as invalid, but we still need to increment nested_elem_idx
			// and set the nullptrs (otherwise indexing will break after compaction)
			for (idx_t j = 0; j < array_size; j++) {
				nested_vals[nested_elem_idx] = nullptr;
				nested_elem_idx++;
			};
		} else {
			yyjson_arr_foreach(arrays[i], idx, max, val) {
				nested_vals[nested_elem_idx] = val;
				nested_elem_idx++;
			}
		}
	}

	if (!success) {
		// Set object index in case of error in nested array so we can get accurate line number information
		for (idx_t i = 0; i < count; i++) {
			if (!result_validity.RowIsValid(i)) {
				continue;
			}
			auto offset = i * array_size;
			if (options.object_index >= offset && options.object_index < offset + array_size) {
				options.object_index = i;
			}
		}
	}

	// Transform array values
	if (!JSONTransform::Transform(nested_vals, alc, ArrayVector::GetChildMutable(result), child_count, options,
	                              nullptr)) {
		success = false;
	}

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}

	return success;
}

static bool TransformObjectToMap(yyjson_val *objects[], yyjson_alc *alc, Vector &result, const idx_t count,
                                 JSONTransformOptions &options) {
	// Pre-allocate list vector
	idx_t list_size = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto &obj = objects[i];
		if (!obj || !unsafe_yyjson_is_obj(obj)) {
			continue;
		}
		list_size += unsafe_yyjson_get_len(obj);
	}
	ListVector::Reserve(result, list_size);
	ListVector::SetListSize(result, list_size);

	auto list_entries = FlatVector::GetDataMutable<list_entry_t>(result);
	auto &list_validity = FlatVector::ValidityMutable(result);

	auto keys = JSONCommon::AllocateArray<yyjson_val *>(alc, list_size);
	auto vals = JSONCommon::AllocateArray<yyjson_val *>(alc, list_size);

	bool success = true;
	idx_t list_offset = 0;

	size_t idx, max;
	yyjson_val *key, *val;
	for (idx_t i = 0; i < count; i++) {
		const auto &obj = objects[i];
		if (!obj || unsafe_yyjson_is_null(obj)) {
			list_validity.SetInvalid(i);
			continue;
		}

		if (!unsafe_yyjson_is_obj(obj)) {
			list_validity.SetInvalid(i);
			if (success && options.strict_cast && !unsafe_yyjson_is_obj(obj)) {
				options.error_message =
				    StringUtil::Format("Expected OBJECT, but got %s: %s", JSONCommon::ValTypeToString(obj),
				                       JSONCommon::ValToString(obj, 50));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		auto &list_entry = list_entries[i];
		list_entry.offset = list_offset;
		list_entry.length = unsafe_yyjson_get_len(obj);

		yyjson_obj_foreach(obj, idx, max, key, val) {
			keys[list_offset] = key;
			vals[list_offset] = val;
			list_offset++;
		}
	}
	D_ASSERT(list_offset == list_size);

	// Transform keys
	if (!JSONTransform::Transform(keys, alc, MapVector::GetKeys(result), list_size, options, nullptr)) {
		throw ConversionException(
		    StringUtil::Format("%s. Cannot default to NULL, because map keys cannot be NULL", options.error_message));
	}

	// Transform values
	if (!JSONTransform::Transform(vals, alc, MapVector::GetValues(result), list_size, options, nullptr)) {
		success = false;
	}

	// objects can have duplicate keys, and keys that fail to transform in non-strict mode are NULL
	auto &map_keys = MapVector::GetKeys(result);
	auto key_validity = map_keys.Validity();
	value_set_t unique_keys;
	for (idx_t i = 0; i < count; i++) {
		if (!list_validity.RowIsValid(i)) {
			continue;
		}
		const auto &list_entry = list_entries[i];
		const char *invalid_reason = nullptr;
		unique_keys.clear();
		for (idx_t key_idx = list_entry.offset; key_idx < list_entry.offset + list_entry.length; key_idx++) {
			if (!key_validity.IsValid(key_idx)) {
				invalid_reason = "Map keys can not be NULL";
				break;
			}
			if (!unique_keys.insert(map_keys.GetValue(key_idx)).second) {
				invalid_reason = "Map keys must be unique";
				break;
			}
		}
		if (!invalid_reason) {
			continue;
		}
		list_validity.SetInvalid(i);
		if (success && options.strict_cast) {
			options.error_message = invalid_reason;
			options.object_index = i;
			success = false;
		}
	}

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}

	return success;
}

static bool TransformToJSON(yyjson_val *vals[], yyjson_alc *alc, Vector &result, const idx_t count,
                            const JSONTransformOptions &options) {
	auto data = FlatVector::GetDataMutable<string_t>(result);
	auto &validity = FlatVector::ValidityMutable(result);
	JSONRawNumberWriter raw_writer(alc);
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (!val || unsafe_yyjson_is_null(val)) {
			validity.SetInvalid(i);
		} else if (options.numbers_as_raw) {
			data[i] = raw_writer.Write(val);
		} else {
			data[i] = JSONCommon::WriteVal(val, alc);
		}
	}
	// Can always transform to JSON
	return true;
}

static bool TransformValueIntoUnion(yyjson_val **vals, yyjson_alc *alc, Vector &result, const idx_t count,
                                    JSONTransformOptions &options) {
	auto type = result.GetType();

	auto fields = UnionType::CopyMemberTypes(type);
	vector<string> names;
	for (const auto &field : fields) {
		names.emplace_back(field.first);
	}

	bool success = true;

	auto &validity = FlatVector::ValidityMutable(result);

	auto set_error = [&](idx_t i, const string &message) {
		validity.SetInvalid(i);
		result.SetValue(i, Value(nullptr));
		if (success && options.strict_cast) {
			options.error_message = message;
			options.object_index = i;
			success = false;
		}
	};

	for (idx_t i = 0; i < count; i++) {
		const auto &obj = vals[i];

		if (!obj || unsafe_yyjson_is_null(vals[i])) {
			validity.SetInvalid(i);
			result.SetValue(i, Value(nullptr));
			continue;
		}

		if (!unsafe_yyjson_is_obj(obj)) {
			set_error(i,
			          StringUtil::Format("Expected an object representing a union, got %s", yyjson_get_type_desc(obj)));
			continue;
		}

		auto len = unsafe_yyjson_get_len(obj);
		if (len > 1) {
			set_error(i, "Found object containing more than one key, instead of union");
			continue;
		} else if (len == 0) {
			set_error(i, "Found empty object, instead of union");
			continue;
		}

		auto key = unsafe_yyjson_get_first(obj);
		auto val = yyjson_obj_iter_get_val(key);

		auto tag = std::find(names.begin(), names.end(), unsafe_yyjson_get_str(key));
		if (tag == names.end()) {
			set_error(i, StringUtil::Format("Found object containing unknown key, instead of union: %s",
			                                unsafe_yyjson_get_str(key)));
			continue;
		}

		idx_t actual_tag = tag - names.begin();

		Vector single(UnionType::GetMemberType(type, actual_tag), 1);
		if (!JSONTransform::Transform(&val, alc, single, 1, options, nullptr)) {
			success = false;
		}

		result.SetValue(i, Value::UNION(fields, actual_tag, single.GetValue(0)));
	}

	return success;
}

bool JSONTransform::Transform(yyjson_val *vals[], yyjson_alc *alc, Vector &result, const idx_t count,
                              JSONTransformOptions &options, optional_ptr<const ColumnIndex> column_index) {
	auto result_type = result.GetType();
	if ((result_type == LogicalTypeId::TIMESTAMP || result_type == LogicalTypeId::DATE) && options.date_format_map &&
	    options.date_format_map->HasFormats(result_type.id())) {
		// Auto-detected date/timestamp format during sampling
		return TransformFromStringWithFormat(vals, result, count, options);
	}

	if (result_type.IsJSONType()) {
		return TransformToJSON(vals, alc, result, count, options);
	}

	switch (result_type.id()) {
	case LogicalTypeId::SQLNULL:
		FlatVector::ValidityMutable(result).SetAllInvalid(count);
		return true;
	case LogicalTypeId::BOOLEAN:
		return TransformNumerical<bool>(vals, result, count, options);
	case LogicalTypeId::TINYINT:
		return TransformNumerical<int8_t>(vals, result, count, options);
	case LogicalTypeId::SMALLINT:
		return TransformNumerical<int16_t>(vals, result, count, options);
	case LogicalTypeId::INTEGER:
		return TransformNumerical<int32_t>(vals, result, count, options);
	case LogicalTypeId::BIGINT:
		return TransformNumerical<int64_t>(vals, result, count, options);
	case LogicalTypeId::UTINYINT:
		return TransformNumerical<uint8_t>(vals, result, count, options);
	case LogicalTypeId::USMALLINT:
		return TransformNumerical<uint16_t>(vals, result, count, options);
	case LogicalTypeId::UINTEGER:
		return TransformNumerical<uint32_t>(vals, result, count, options);
	case LogicalTypeId::UBIGINT:
		return TransformNumerical<uint64_t>(vals, result, count, options);
	case LogicalTypeId::HUGEINT:
		return TransformNumerical<hugeint_t>(vals, result, count, options);
	case LogicalTypeId::UHUGEINT:
		return TransformNumerical<uhugeint_t>(vals, result, count, options);
	case LogicalTypeId::FLOAT:
		return TransformNumerical<float>(vals, result, count, options);
	case LogicalTypeId::DOUBLE:
		return TransformNumerical<double>(vals, result, count, options);
	case LogicalTypeId::DECIMAL: {
		auto width = DecimalType::GetWidth(result_type);
		auto scale = DecimalType::GetScale(result_type);
		switch (result_type.InternalType()) {
		case PhysicalType::INT16:
			return TransformDecimal<int16_t>(vals, result, count, width, scale, options);
		case PhysicalType::INT32:
			return TransformDecimal<int32_t>(vals, result, count, width, scale, options);
		case PhysicalType::INT64:
			return TransformDecimal<int64_t>(vals, result, count, width, scale, options);
		case PhysicalType::INT128:
			return TransformDecimal<hugeint_t>(vals, result, count, width, scale, options);
		default:
			throw InternalException("Unimplemented physical type for decimal");
		}
	}
	case LogicalTypeId::ENUM:
	case LogicalTypeId::DATE:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::UUID:
		return TransformFromString(vals, result, count, options);
	case LogicalTypeId::GEOMETRY:
		return TransformGeometry(vals, result, count, options);
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
		return TransformToString(vals, alc, result, count, options);
	case LogicalTypeId::STRUCT:
		return TransformObjectInternal(vals, alc, result, count, options, column_index);
	case LogicalTypeId::TUPLE:
		return TransformArrayToTuple(vals, alc, result, count, options);
	case LogicalTypeId::LIST:
		return TransformArrayToList(vals, alc, result, count, options);
	case LogicalTypeId::MAP:
		return TransformObjectToMap(vals, alc, result, count, options);
	case LogicalTypeId::UNION:
		return TransformValueIntoUnion(vals, alc, result, count, options);
	case LogicalTypeId::ARRAY:
		return TransformArrayToArray(vals, alc, result, count, options);
	default:
		throw NotImplementedException("Cannot read a value of type %s from a json file", result_type.ToString());
	}
}

static bool TransformFunctionInternal(const Vector &input, const idx_t count, Vector &result, yyjson_alc *alc,
                                      JSONTransformOptions &options) {
	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(input_data);
	auto inputs = UnifiedVectorFormat::GetData<string_t>(input_data);

	// Read documents
	auto docs = JSONCommon::AllocateArray<yyjson_doc *>(alc, count);
	auto vals = JSONCommon::AllocateArray<yyjson_val *>(alc, count);
	auto &result_validity = FlatVector::ValidityMutable(result);
	options.numbers_as_raw = JSONCommon::NumbersAsRaw(result.GetType());
	const auto read_flags = JSONCommon::GetReadFlag(JSONCommon::READ_FLAG, options.numbers_as_raw);
	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			docs[i] = nullptr;
			vals[i] = nullptr;
			result_validity.SetInvalid(i);
		} else {
			docs[i] = JSONCommon::ReadDocument(inputs[idx], read_flags, alc);
			vals[i] = docs[i]->root;
		}
	}
	return JSONTransform::Transform(vals, alc, result, count, options, nullptr);
}

template <bool strict>
static void TransformFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();

	JSONTransformOptions options(strict, strict, strict, false);
	if (!TransformFunctionInternal(args.data[0], args.size(), result, alc, options)) {
		throw InvalidInputException(options.error_message);
	}

	JSONAllocator::AddBuffer(result, alc);
}

static void GetTransformFunctionInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction fun({}, LogicalType::ANY, TransformFunction<false>, JSONTransformBind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json", input_type).AddParameter("structure", LogicalType::VARCHAR);
	set.AddFunction(fun);
}

ScalarFunctionSet JSONFunctions::GetTransformFunction() {
	ScalarFunctionSet set("json_transform");
	GetTransformFunctionInternal(set, LogicalType::VARCHAR);
	GetTransformFunctionInternal(set, LogicalType::JSON());
	set.SetFallible();
	return set;
}

static void GetTransformStrictFunctionInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction fun({}, LogicalType::ANY, TransformFunction<true>, JSONTransformBind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json", input_type).AddParameter("structure", LogicalType::VARCHAR);
	set.AddFunction(fun);
}

ScalarFunctionSet JSONFunctions::GetTransformStrictFunction() {
	ScalarFunctionSet set("json_transform_strict");
	GetTransformStrictFunctionInternal(set, LogicalType::VARCHAR);
	GetTransformStrictFunctionInternal(set, LogicalType::JSON());
	set.SetFallible();
	return set;
}

static bool JSONToAnyCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &lstate = parameters.local_state->Cast<JSONFunctionLocalState>();
	lstate.json_allocator->Reset();
	auto alc = lstate.json_allocator->GetYYAlc();

	JSONTransformOptions options(true, true, true, true);
	options.delay_error = true;

	auto success = TransformFunctionInternal(source, count, result, alc, options);
	if (!success) {
		HandleCastError::AssignError(options.error_message, parameters);
	}
	return success;
}

static BoundCastInfo JSONToAnyCastBind(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	return BoundCastInfo(JSONToAnyCast, nullptr, JSONFunctionLocalState::InitCastLocalState);
}

void JSONFunctions::RegisterJSONTransformCastFunctions(ExtensionLoader &loader) {
	// JSON can be cast to anything
	for (const auto &type : LogicalType::AllTypes()) {
		LogicalType target_type;
		switch (type.id()) {
		case LogicalTypeId::STRUCT:
			target_type = LogicalType::STRUCT({{"any", LogicalType::ANY}});
			break;
		case LogicalTypeId::TUPLE:
			target_type = LogicalType::TUPLE({LogicalType::ANY});
			break;
		case LogicalTypeId::LIST:
			target_type = LogicalType::LIST(LogicalType::ANY);
			break;
		case LogicalTypeId::MAP:
			target_type = LogicalType::MAP(LogicalType::ANY, LogicalType::ANY);
			break;
		case LogicalTypeId::UNION:
			target_type = LogicalType::UNION({{"any", LogicalType::ANY}});
			break;
		case LogicalTypeId::ARRAY:
			target_type = LogicalType::ARRAY(LogicalType::ANY, optional_idx());
			break;
		case LogicalTypeId::VARCHAR:
			// We skip this one here as it's handled in json_functions.cpp
			continue;
		default:
			target_type = type;
		}
		// Going from JSON to another type has the same cost as going from VARCHAR to that type
		const auto json_to_target_cost =
		    CastFunctionSet::ImplicitCastCost(loader.GetDatabaseInstance(), LogicalType::VARCHAR, target_type);
		loader.RegisterCastFunction(LogicalType::JSON(), target_type, JSONToAnyCastBind, json_to_target_cost);
	}
}

} // namespace duckdb
