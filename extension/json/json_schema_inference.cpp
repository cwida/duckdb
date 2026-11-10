#include "json_schema_inference.hpp"

#include "duckdb/common/enum_util.hpp"
#include "json_geojson.hpp"
#include "json_scan.hpp"
#include "json_transform.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

static bool IsNumeric(LogicalTypeId type) {
	return type == LogicalTypeId::DOUBLE || type == LogicalTypeId::UBIGINT || type == LogicalTypeId::BIGINT ||
	       type == LogicalTypeId::HUGEINT;
}

static LogicalTypeId MaxNumericType(const LogicalTypeId &a, const LogicalTypeId &b) {
	D_ASSERT(a != b);
	if (a == LogicalTypeId::DOUBLE || b == LogicalTypeId::DOUBLE) {
		return LogicalTypeId::DOUBLE;
	}
	if (a == LogicalTypeId::HUGEINT || b == LogicalTypeId::HUGEINT) {
		return LogicalTypeId::HUGEINT;
	}
	// One is BIGINT and the other is UBIGINT - need HUGEINT to represent both ranges
	if ((a == LogicalTypeId::BIGINT && b == LogicalTypeId::UBIGINT) ||
	    (a == LogicalTypeId::UBIGINT && b == LogicalTypeId::BIGINT)) {
		return LogicalTypeId::HUGEINT;
	}
	return LogicalTypeId::BIGINT;
}

JSONStructureNode::JSONStructureNode(const bool case_insensitive_keys_p)
    : count(0), null_count(0), case_insensitive_keys(case_insensitive_keys_p) {
}

JSONStructureNode::JSONStructureNode(const char *key_ptr, const size_t key_len, const bool case_insensitive_keys_p)
    : JSONStructureNode(case_insensitive_keys_p) {
	key = make_uniq<string>(key_ptr, key_len);
}

JSONStructureNode JSONStructureNode::EmptyLike(const JSONStructureNode &parent) {
	return JSONStructureNode(parent.case_insensitive_keys);
}

static void SwapJSONStructureNode(JSONStructureNode &a, JSONStructureNode &b) noexcept {
	std::swap(a.key, b.key);
	std::swap(a.initialized, b.initialized);
	std::swap(a.descriptions, b.descriptions);
	std::swap(a.count, b.count);
	std::swap(a.null_count, b.null_count);
	std::swap(a.case_insensitive_keys, b.case_insensitive_keys);
}

JSONStructureNode::JSONStructureNode(JSONStructureNode &&other) noexcept {
	SwapJSONStructureNode(*this, other);
}

JSONStructureNode &JSONStructureNode::operator=(JSONStructureNode &&other) noexcept {
	SwapJSONStructureNode(*this, other);
	return *this;
}

JSONStructureDescription &JSONStructureNode::GetOrCreateDescription(const LogicalTypeId type) {
	if (descriptions.empty()) {
		// Empty, just put this type in there
		descriptions.emplace_back(type, case_insensitive_keys);
		return descriptions.back();
	}

	if (descriptions.size() == 1 && descriptions[0].type == LogicalTypeId::SQLNULL) {
		// Only a NULL in there, override
		descriptions[0].type = type;
		return descriptions[0];
	}

	if (type == LogicalTypeId::SQLNULL) {
		// 'descriptions' is non-empty, so let's not add NULL
		return descriptions.back();
	}

	// Check if type is already in there or if we can merge numerics
	const auto is_numeric = IsNumeric(type);
	for (auto &description : descriptions) {
		if (type == description.type) {
			return description;
		}
		if (is_numeric && IsNumeric(description.type)) {
			description.type = MaxNumericType(type, description.type);
			return description;
		}
	}
	// Type was not there, create a new description
	descriptions.emplace_back(type, case_insensitive_keys);
	return descriptions.back();
}

bool JSONStructureNode::ContainsVarchar() const {
	if (descriptions.size() != 1) {
		// We can't refine types if we have more than 1 description (yet), defaults to JSON type for now
		return false;
	}
	auto &description = descriptions[0];
	if (description.type == LogicalTypeId::VARCHAR) {
		return true;
	}
	for (auto &child : description.children) {
		if (child.ContainsVarchar()) {
			return true;
		}
	}

	return false;
}

void JSONStructureNode::InitializeCandidateTypes(const idx_t max_depth, const bool convert_strings_to_integers,
                                                 const bool user_specified_timestamp_format, const idx_t depth) {
	if (depth >= max_depth) {
		return;
	}
	if (descriptions.size() != 1) {
		// We can't refine types if we have more than 1 description (yet), defaults to JSON type for now
		return;
	}
	auto &description = descriptions[0];
	if (description.type == LogicalTypeId::VARCHAR && !initialized) {
		// We loop through the candidate types and format templates from back to front.
		// TIMESTAMP_TZ is only a candidate when the user did not pass timestamp_format=; never infer that
		// from the date-format map (auto-detect can also converge on a single format).
		// Trial order puts TIMESTAMP_TZ before TIMESTAMP so back-to-front tries TZ first (issue #14919).
		if (convert_strings_to_integers) {
			if (user_specified_timestamp_format) {
				description.candidate_types = {LogicalTypeId::UUID, LogicalTypeId::BIGINT, LogicalTypeId::TIMESTAMP,
				                               LogicalTypeId::DATE, LogicalTypeId::TIME};
			} else {
				description.candidate_types = {LogicalTypeId::UUID,      LogicalTypeId::BIGINT,
				                               LogicalTypeId::TIMESTAMP, LogicalTypeId::TIMESTAMP_TZ,
				                               LogicalTypeId::DATE,      LogicalTypeId::TIME};
			}
		} else if (user_specified_timestamp_format) {
			description.candidate_types = {LogicalTypeId::UUID, LogicalTypeId::TIMESTAMP, LogicalTypeId::DATE,
			                               LogicalTypeId::TIME};
		} else {
			description.candidate_types = {LogicalTypeId::UUID, LogicalTypeId::TIMESTAMP, LogicalTypeId::TIMESTAMP_TZ,
			                               LogicalTypeId::DATE, LogicalTypeId::TIME};
		}
		initialized = true;
	} else {
		for (auto &child : description.children) {
			child.InitializeCandidateTypes(max_depth, convert_strings_to_integers, user_specified_timestamp_format,
			                               depth + 1);
		}
	}
}

void JSONStructureNode::RefineCandidateTypes(yyjson_val *vals[], const idx_t val_count, Vector &string_vector,
                                             ArenaAllocator &allocator, MutableDateFormatMap &date_format_map,
                                             const bool user_specified_timestamp_format) {
	if (descriptions.size() != 1) {
		// We can't refine types if we have more than 1 description (yet), defaults to JSON type for now
		return;
	}
	if (!ContainsVarchar()) {
		return;
	}
	auto &description = descriptions[0];
	switch (description.type) {
	case LogicalTypeId::LIST:
		return RefineCandidateTypesArray(vals, val_count, string_vector, allocator, date_format_map,
		                                 user_specified_timestamp_format);
	case LogicalTypeId::STRUCT:
		return RefineCandidateTypesObject(vals, val_count, string_vector, allocator, date_format_map,
		                                  user_specified_timestamp_format);
	case LogicalTypeId::VARCHAR:
		return RefineCandidateTypesString(vals, val_count, string_vector, date_format_map,
		                                  user_specified_timestamp_format);
	default:
		return;
	}
}

void JSONStructureNode::RefineCandidateTypesArray(yyjson_val *vals[], const idx_t val_count, Vector &string_vector,
                                                  ArenaAllocator &allocator, MutableDateFormatMap &date_format_map,
                                                  const bool user_specified_timestamp_format) {
	D_ASSERT(descriptions.size() == 1 && descriptions[0].type == LogicalTypeId::LIST);
	auto &desc = descriptions[0];
	D_ASSERT(desc.children.size() == 1);
	auto &child = desc.children[0];

	idx_t total_list_size = 0;
	for (idx_t i = 0; i < val_count; i++) {
		if (vals[i] && !unsafe_yyjson_is_null(vals[i])) {
			D_ASSERT(yyjson_is_arr(vals[i]));
			total_list_size += unsafe_yyjson_get_len(vals[i]);
		}
	}

	idx_t offset = 0;
	auto child_vals =
	    reinterpret_cast<yyjson_val **>(allocator.AllocateAligned(total_list_size * sizeof(yyjson_val *)));

	size_t idx, max;
	yyjson_val *child_val;
	for (idx_t i = 0; i < val_count; i++) {
		if (vals[i] && !unsafe_yyjson_is_null(vals[i])) {
			yyjson_arr_foreach(vals[i], idx, max, child_val) {
				child_vals[offset++] = child_val;
			}
		}
	}
	child.RefineCandidateTypes(child_vals, total_list_size, string_vector, allocator, date_format_map,
	                           user_specified_timestamp_format);
}

void JSONStructureNode::RefineCandidateTypesObject(yyjson_val *vals[], const idx_t val_count, Vector &string_vector,
                                                   ArenaAllocator &allocator, MutableDateFormatMap &date_format_map,
                                                   const bool user_specified_timestamp_format) {
	D_ASSERT(descriptions.size() == 1 && descriptions[0].type == LogicalTypeId::STRUCT);
	auto &desc = descriptions[0];

	const idx_t child_count = desc.children.size();
	vector<yyjson_val **> child_vals;
	child_vals.reserve(child_count);
	for (idx_t child_idx = 0; child_idx < child_count; child_idx++) {
		child_vals.emplace_back(
		    reinterpret_cast<yyjson_val **>(allocator.AllocateAligned(val_count * sizeof(yyjson_val *))));
	}

	const auto found_keys = reinterpret_cast<bool *>(allocator.AllocateAligned(sizeof(bool) * child_count));

	const auto &key_map = desc.key_map;
	size_t idx, max;
	yyjson_val *child_key, *child_val;
	for (idx_t i = 0; i < val_count; i++) {
		if (vals[i] && !unsafe_yyjson_is_null(vals[i])) {
			idx_t found_key_count = 0;
			memset(found_keys, false, child_count);

			D_ASSERT(yyjson_is_obj(vals[i]));
			yyjson_obj_foreach(vals[i], idx, max, child_key, child_val) {
				D_ASSERT(yyjson_is_str(child_key));
				const auto key_ptr = unsafe_yyjson_get_str(child_key);
				const auto key_len = unsafe_yyjson_get_len(child_key);
				auto it = key_map.find({key_ptr, key_len});
				D_ASSERT(it != key_map.end());
				const auto child_idx = it->second;
				child_vals[child_idx][i] = child_val;
				found_key_count += !found_keys[child_idx];
				found_keys[child_idx] = true;
			}

			if (found_key_count != child_count) {
				// Set child val to nullptr so recursion doesn't break
				for (idx_t child_idx = 0; child_idx < child_count; child_idx++) {
					if (!found_keys[child_idx]) {
						child_vals[child_idx][i] = nullptr;
					}
				}
			}
		} else {
			for (idx_t child_idx = 0; child_idx < child_count; child_idx++) {
				child_vals[child_idx][i] = nullptr;
			}
		}
	}

	for (idx_t child_idx = 0; child_idx < child_count; child_idx++) {
		desc.children[child_idx].RefineCandidateTypes(child_vals[child_idx], val_count, string_vector, allocator,
		                                              date_format_map, user_specified_timestamp_format);
	}
}

void JSONStructureNode::RefineCandidateTypesString(yyjson_val *vals[], const idx_t val_count, Vector &string_vector,
                                                   MutableDateFormatMap &date_format_map,
                                                   const bool user_specified_timestamp_format) {
	D_ASSERT(descriptions.size() == 1 && descriptions[0].type == LogicalTypeId::VARCHAR);
	if (descriptions[0].candidate_types.empty()) {
		return;
	}
	static JSONTransformOptions OPTIONS;
	JSONTransform::GetStringVector(vals, val_count, LogicalType::SQLNULL, string_vector, OPTIONS);
	EliminateCandidateTypes(val_count, string_vector, date_format_map, user_specified_timestamp_format);
}

// Single commutative transition table, shared by per-vector refinement and parallel merges,
// so thread scheduling cannot change the detected type. UNKNOWN is the identity, MIXED is sticky.
static TimestampOffsetState MergeTimestampOffsetState(const TimestampOffsetState a, const TimestampOffsetState b) {
	if (a == TimestampOffsetState::UNKNOWN) {
		return b;
	}
	if (b == TimestampOffsetState::UNKNOWN) {
		return a;
	}
	if (a != b) {
		return TimestampOffsetState::MIXED;
	}
	return a;
}

// Date::TryConvertDate reuses has_offset for specials; only Z / ±HH[:MM] is a real offset.
static bool TimestampStringHasUtcOffset(const string_t &value) {
	const auto str = value.GetData();
	const auto len = value.GetSize();
	idx_t time_start = 0;
	for (idx_t i = 0; i < len; i++) {
		if (str[i] == 'T' || str[i] == ' ') {
			time_start = i + 1;
			break;
		}
	}
	if (time_start == 0 || time_start >= len) {
		return false;
	}
	for (idx_t i = time_start; i < len; i++) {
		if (str[i] == 'Z') {
			return true;
		}
		if (str[i] == '+' || str[i] == '-') {
			idx_t pos = i;
			int hh, mm, ss;
			return Timestamp::TryParseUTCOffset(str, pos, len, hh, mm, ss);
		}
	}
	return false;
}

// Update the sticky offset state from this vector's non-null strings.
// MIXED → VARCHAR: neither TIMESTAMPTZ (would invent session TZ for naive values)
// nor TIMESTAMP (would drop offsets) is safe.
static void UpdateTimestampOffsetState(JSONStructureDescription &description, const Vector &string_vector,
                                       const idx_t count) {
	const auto strings = FlatVector::GetData<string_t>(string_vector);
	const auto &validity = FlatVector::Validity(string_vector);
	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			continue; // nulls leave the state unchanged
		}
		timestamp_t ts;
		bool has_offset = false;
		string_t tz(nullptr, 0);
		auto res =
		    Timestamp::TryConvertTimestampTZ(strings[i].GetData(), strings[i].GetSize(), ts, true, has_offset, tz);
		if (res != TimestampCastResult::SUCCESS && res != TimestampCastResult::STRICT_UTC) {
			continue; // not a timestamp string — other candidates handle it
		}
		if (!ts.IsFinite()) {
			continue; // ±infinity is valid for TIMESTAMP and TIMESTAMPTZ
		}
		if (has_offset && !TimestampStringHasUtcOffset(strings[i])) {
			continue; // date special such as epoch, not a real offset
		}
		description.timestamp_offset_state = MergeTimestampOffsetState(
		    description.timestamp_offset_state,
		    has_offset ? TimestampOffsetState::WITH_OFFSET : TimestampOffsetState::WITHOUT_OFFSET);
		if (description.timestamp_offset_state == TimestampOffsetState::MIXED) {
			return;
		}
	}
}

void JSONStructureNode::EliminateCandidateTypes(const idx_t vec_count, Vector &string_vector,
                                                MutableDateFormatMap &date_format_map,
                                                const bool user_specified_timestamp_format) {
	D_ASSERT(descriptions.size() == 1 && descriptions[0].type == LogicalTypeId::VARCHAR);
	auto &description = descriptions[0];
	auto &candidate_types = description.candidate_types;

	// Monotonic across vectors: once MIXED, refuse to guess.
	if (!user_specified_timestamp_format) {
		UpdateTimestampOffsetState(description, string_vector, vec_count);
		if (description.timestamp_offset_state == TimestampOffsetState::MIXED) {
			candidate_types.clear();
			return;
		}
	}

	while (true) {
		if (candidate_types.empty()) {
			return;
		}
		const auto type = candidate_types.back();
		Vector result_vector(type, vec_count);
		if (type == LogicalTypeId::TIMESTAMP_TZ) {
			// Defense in depth: InitializeCandidateTypes already omits TZ when the user set timestamp_format=.
			if (user_specified_timestamp_format) {
				candidate_types.pop_back();
				continue;
			}
			// Only accept TIMESTAMPTZ when every non-null value seen so far carries an offset/Z.
			// UNKNOWN (e.g., null-only vectors) and WITHOUT_OFFSET drop TZ and try TIMESTAMP (etc.).
			if (description.timestamp_offset_state != TimestampOffsetState::WITH_OFFSET) {
				candidate_types.pop_back();
				continue;
			}
			string error_message;
			if (!VectorOperations::DefaultTryCast(string_vector, result_vector, vec_count, &error_message, true)) {
				candidate_types.pop_back();
			} else {
				return;
			}
		} else if (date_format_map.HasFormats(type)) {
			if (EliminateCandidateFormats(vec_count, string_vector, result_vector, date_format_map)) {
				return;
			} else {
				candidate_types.pop_back();
			}
		} else {
			string error_message;
			if (!VectorOperations::DefaultTryCast(string_vector, result_vector, vec_count, &error_message, true)) {
				candidate_types.pop_back();
			} else {
				return;
			}
		}
	}
}

template <class OP, class T>
bool TryParse(const Vector &string_vector, StrpTimeFormat &format, const idx_t count) {
	const auto strings = FlatVector::GetData<string_t>(string_vector);
	const auto &validity = FlatVector::Validity(string_vector);

	T result;
	string error_message;
	if (validity.CannotHaveNull()) {
		for (idx_t i = 0; i < count; i++) {
			if (!OP::template Operation<T>(format, strings[i], result, error_message)) {
				return false;
			}
		}
	} else {
		for (idx_t i = 0; i < count; i++) {
			if (validity.RowIsValid(i)) {
				if (!OP::template Operation<T>(format, strings[i], result, error_message)) {
					return false;
				}
			}
		}
	}
	return true;
}

bool JSONStructureNode::EliminateCandidateFormats(const idx_t vec_count, Vector &string_vector,
                                                  const Vector &result_vector, MutableDateFormatMap &date_format_map) {
	D_ASSERT(descriptions.size() == 1 && descriptions[0].type == LogicalTypeId::VARCHAR);

	const auto type = result_vector.GetType().id();
	auto i = date_format_map.NumberOfFormats(type);
	for (; i != 0; i--) {
		StrpTimeFormat format;
		if (!date_format_map.GetFormatAtIndex(type, i - 1, format)) {
			continue;
		}

		bool success;
		switch (type) {
		case LogicalTypeId::DATE:
			success = TryParse<TryParseDate, date_t>(string_vector, format, vec_count);
			break;
		case LogicalTypeId::TIMESTAMP:
			success = TryParse<TryParseTimeStamp, timestamp_t>(string_vector, format, vec_count);
			break;
		default:
			throw InternalException("No date/timestamp formats for %s", EnumUtil::ToString(type));
		}

		if (success) {
			return true;
		}
	}

	return false;
}

JSONStructureDescription::JSONStructureDescription(const LogicalTypeId type_p, const bool case_insensitive_keys)
    : type(type_p),
      key_map(0, JSONKeyOptionalCaseHash {case_insensitive_keys}, JSONKeyOptionalCaseEquality {case_insensitive_keys}) {
}

static void SwapJSONStructureDescription(JSONStructureDescription &a, JSONStructureDescription &b) noexcept {
	std::swap(a.type, b.type);
	std::swap(a.key_map, b.key_map);
	std::swap(a.children, b.children);
	std::swap(a.candidate_types, b.candidate_types);
	std::swap(a.has_large_ubigint, b.has_large_ubigint);
	std::swap(a.timestamp_offset_state, b.timestamp_offset_state);
}

JSONStructureDescription::JSONStructureDescription(JSONStructureDescription &&other) noexcept {
	SwapJSONStructureDescription(*this, other);
}

JSONStructureDescription &JSONStructureDescription::operator=(JSONStructureDescription &&other) noexcept {
	SwapJSONStructureDescription(*this, other);
	return *this;
}

JSONStructureNode &JSONStructureDescription::GetOrCreateChild() {
	D_ASSERT(type == LogicalTypeId::LIST);
	if (children.empty()) {
		children.emplace_back(key_map.hash_function().case_insensitive);
	}
	D_ASSERT(children.size() == 1);
	return children.back();
}

JSONStructureNode &JSONStructureDescription::GetOrCreateChild(const char *key_ptr, const size_t key_size) {
	// Check if there is already a child with the same key
	const JSONKey temp_key {key_ptr, key_size};
	const auto it = key_map.find(temp_key);
	if (it != key_map.end()) {
		return children[it->second]; // Found it
	}

	// Didn't find, create a new child (the first spelling of a key names it when keys are case-insensitive)
	children.emplace_back(key_ptr, key_size, key_map.hash_function().case_insensitive);
	const auto &persistent_key_string = *children.back().key;
	JSONKey new_key {persistent_key_string.c_str(), persistent_key_string.length()};
	key_map.emplace(new_key, children.size() - 1);
	return children.back();
}

JSONStructureNode &JSONStructureDescription::GetOrCreateChild(yyjson_val *key, yyjson_val *val,
                                                              const bool ignore_errors, const bool detect_geojson,
                                                              const idx_t depth) {
	D_ASSERT(yyjson_is_str(key));
	auto &child = GetOrCreateChild(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
	JSONStructure::ExtractStructure(val, child, ignore_errors, detect_geojson, depth);
	return child;
}

static void ExtractStructureArray(yyjson_val *arr, JSONStructureNode &node, const bool ignore_errors,
                                  const bool detect_geojson, const idx_t depth) {
	D_ASSERT(yyjson_is_arr(arr));
	auto &description = node.GetOrCreateDescription(LogicalTypeId::LIST);
	auto &child = description.GetOrCreateChild();

	size_t idx, max;
	yyjson_val *val;
	yyjson_arr_foreach(arr, idx, max, val) {
		JSONStructure::ExtractStructure(val, child, ignore_errors, detect_geojson, depth + 1);
	}
}

static void ExtractStructureObject(yyjson_val *obj, JSONStructureNode &node, const bool ignore_errors,
                                   const bool detect_geojson, const idx_t depth) {
	D_ASSERT(yyjson_is_obj(obj));
	auto &description = node.GetOrCreateDescription(LogicalTypeId::STRUCT);

	// Keep track of keys so we can detect duplicates
	unordered_set<string> obj_keys;
	case_insensitive_set_t ci_obj_keys;

	size_t idx, max;
	yyjson_val *key, *val;
	yyjson_obj_foreach(obj, idx, max, key, val) {
		const string obj_key(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
		auto insert_result = obj_keys.insert(obj_key);
		if (!ignore_errors && !insert_result.second) { // Exact match
			JSONCommon::ThrowValFormatError("Duplicate key \"" + obj_key + "\" in object %s", obj);
		}
		insert_result = ci_obj_keys.insert(obj_key);
		if (!ignore_errors && !insert_result.second) { // Case-insensitive match
			JSONCommon::ThrowValFormatError("Duplicate key (different case) \"" + obj_key + "\" and \"" +
			                                    *insert_result.first + "\" in object %s",
			                                obj);
		}
		description.GetOrCreateChild(key, val, ignore_errors, detect_geojson, depth + 1);
	}
}

static void ExtractStructureVal(yyjson_val *val, JSONStructureNode &node) {
	D_ASSERT(!yyjson_is_arr(val) && !yyjson_is_obj(val));
	const auto val_type = JSONCommon::ValTypeToLogicalTypeId(val);
	auto &desc = node.GetOrCreateDescription(val_type);
	if (val_type == LogicalTypeId::UBIGINT &&
	    unsafe_yyjson_get_uint(val) > static_cast<uint64_t>(NumericLimits<int64_t>::Maximum())) {
		desc.has_large_ubigint = true;
	}
}

void JSONStructure::ExtractStructure(yyjson_val *val, JSONStructureNode &node, const bool ignore_errors,
                                     const bool detect_geojson, const idx_t depth) {
	if (depth >= JSONCommon::MAX_RECURSION_DEPTH) {
		throw InvalidInputException("JSON exceeds maximum recursion depth of %d", JSONCommon::MAX_RECURSION_DEPTH);
	}
	node.count++;
	const auto tag = yyjson_get_tag(val);
	if (tag == (YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE)) {
		node.null_count++;
	}

	switch (tag) {
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
		return ExtractStructureArray(val, node, ignore_errors, detect_geojson, depth);
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
		// A GeoJSON geometry is a leaf: it becomes GEOMETRY rather than a struct of type/coordinates. Detecting it
		// here (rather than on the finished tree) means geometries of different kinds still merge into one type.
		if (detect_geojson && JSONGeometry::IsGeoJSONGeometry(val)) {
			node.GetOrCreateDescription(LogicalTypeId::GEOMETRY);
			return;
		}
		return ExtractStructureObject(val, node, ignore_errors, detect_geojson, depth);
	default:
		return ExtractStructureVal(val, node);
	}
}

static LogicalType StructureToTypeArray(ClientContext &context, const JSONStructureNode &node, const idx_t max_depth,
                                        const double field_appearance_threshold, const idx_t map_inference_threshold,
                                        const idx_t depth, const LogicalType &null_type) {
	D_ASSERT(node.descriptions.size() == 1 && node.descriptions[0].type == LogicalTypeId::LIST);
	const auto &desc = node.descriptions[0];
	D_ASSERT(desc.children.size() == 1);

	return LogicalType::LIST(JSONStructure::StructureToType(context, desc.children[0], max_depth,
	                                                        field_appearance_threshold, map_inference_threshold,
	                                                        depth + 1, null_type));
}

static void MergeNodeArray(JSONStructureNode &merged, const JSONStructureDescription &child_desc) {
	D_ASSERT(child_desc.type == LogicalTypeId::LIST);
	auto &merged_desc = merged.GetOrCreateDescription(LogicalTypeId::LIST);
	auto &merged_child = merged_desc.GetOrCreateChild();
	for (auto &list_child : child_desc.children) {
		JSONStructure::MergeNodes(merged_child, list_child);
	}
}

static void MergeNodeObject(JSONStructureNode &merged, const JSONStructureDescription &child_desc) {
	D_ASSERT(child_desc.type == LogicalTypeId::STRUCT);
	auto &merged_desc = merged.GetOrCreateDescription(LogicalTypeId::STRUCT);
	for (auto &struct_child : child_desc.children) {
		const auto &struct_child_key = *struct_child.key;
		auto &merged_child = merged_desc.GetOrCreateChild(struct_child_key.c_str(), struct_child_key.length());
		JSONStructure::MergeNodes(merged_child, struct_child);
	}
}

static void MergeNodeVal(JSONStructureNode &merged, const JSONStructureDescription &child_desc,
                         const bool node_initialized) {
	D_ASSERT(child_desc.type != LogicalTypeId::LIST && child_desc.type != LogicalTypeId::STRUCT);
	auto &merged_desc = merged.GetOrCreateDescription(child_desc.type);
	if (child_desc.has_large_ubigint) {
		merged_desc.has_large_ubigint = true;
	}
	if (merged_desc.type != LogicalTypeId::VARCHAR || !node_initialized || merged.descriptions.size() != 1) {
		return;
	}
	// Sticky offset state across parallel auto-detect tasks
	merged_desc.timestamp_offset_state =
	    MergeTimestampOffsetState(merged_desc.timestamp_offset_state, child_desc.timestamp_offset_state);
	if (merged_desc.timestamp_offset_state == TimestampOffsetState::MIXED) {
		merged_desc.candidate_types.clear(); // mixed offset/plain → VARCHAR
		merged.initialized = true;
		return;
	}
	if (!merged.initialized) {
		merged_desc.candidate_types = child_desc.candidate_types;
	} else if (merged_desc.candidate_types.empty() != child_desc.candidate_types.empty() // both empty or neither empty
	           || (!merged_desc.candidate_types.empty() &&
	               merged_desc.candidate_types.back() != child_desc.candidate_types.back())) { // non-empty: check type
		merged_desc.candidate_types.clear(); // Not the same, default to VARCHAR
	}

	merged.initialized = true;
}

void JSONStructure::MergeNodes(JSONStructureNode &merged, const JSONStructureNode &node) {
	merged.count += node.count;
	merged.null_count += node.null_count;
	for (const auto &child_desc : node.descriptions) {
		switch (child_desc.type) {
		case LogicalTypeId::LIST:
			MergeNodeArray(merged, child_desc);
			break;
		case LogicalTypeId::STRUCT:
			MergeNodeObject(merged, child_desc);
			break;
		default:
			MergeNodeVal(merged, child_desc, node.initialized);
			break;
		}
	}
}

static double CalculateTypeSimilarity(const LogicalType &merged, const LogicalType &type, idx_t max_depth, idx_t depth);

static double CalculateMapAndStructSimilarity(const LogicalType &map_type, const LogicalType &struct_type,
                                              const bool swapped, const idx_t max_depth, const idx_t depth) {
	const auto &map_value_type = MapType::ValueType(map_type);
	const auto &struct_child_types = StructType::GetChildTypes(struct_type);
	double total_similarity = 0;
	for (const auto &struct_child_type : struct_child_types) {
		const auto similarity =
		    swapped ? CalculateTypeSimilarity(struct_child_type.second, map_value_type, max_depth, depth + 1)
		            : CalculateTypeSimilarity(map_value_type, struct_child_type.second, max_depth, depth + 1);
		if (similarity < 0) {
			return similarity;
		}
		total_similarity += similarity;
	}
	return total_similarity / static_cast<double>(struct_child_types.size());
}

static double CalculateTypeSimilarity(const LogicalType &merged, const LogicalType &type, const idx_t max_depth,
                                      const idx_t depth) {
	if (depth >= max_depth || merged.id() == LogicalTypeId::SQLNULL || type.id() == LogicalTypeId::SQLNULL) {
		return 1;
	}
	if (merged.IsJSONType()) {
		// Incompatible types
		return -1;
	}
	if (type.IsJSONType() || merged == type) {
		return 1;
	}

	switch (merged.id()) {
	case LogicalTypeId::STRUCT: {
		if (type.id() == LogicalTypeId::MAP) {
			// This can happen for empty structs/maps ("{}"), or in rare cases where an inconsistent struct becomes
			// consistent when merged, but does not have enough children to be considered a map.
			return CalculateMapAndStructSimilarity(type, merged, true, max_depth, depth);
		} else if (type.id() != LogicalTypeId::STRUCT) {
			return -1;
		}

		// Only structs can be merged into a struct
		D_ASSERT(type.id() == LogicalTypeId::STRUCT);
		const auto &merged_child_types = StructType::GetChildTypes(merged);
		const auto &type_child_types = StructType::GetChildTypes(type);

		identifier_map_t<const LogicalType &> merged_child_types_map;
		for (const auto &merged_child : merged_child_types) {
			merged_child_types_map.emplace(merged_child.first, merged_child.second);
		}

		double total_similarity = 0;
		for (const auto &type_child_type : type_child_types) {
			const auto it = merged_child_types_map.find(type_child_type.first);
			if (it == merged_child_types_map.end()) {
				return -1;
			}
			const auto similarity = CalculateTypeSimilarity(it->second, type_child_type.second, max_depth, depth + 1);
			if (similarity < 0) {
				return similarity;
			}
			total_similarity += similarity;
		}
		return total_similarity / static_cast<double>(merged_child_types.size());
	}
	case LogicalTypeId::MAP: {
		if (type.id() == LogicalTypeId::MAP) {
			return CalculateTypeSimilarity(MapType::ValueType(merged), MapType::ValueType(type), max_depth, depth + 1);
		}

		// Only maps and structs can be merged into a map
		if (type.id() != LogicalTypeId::STRUCT) {
			return -1;
		}
		return CalculateMapAndStructSimilarity(merged, type, false, max_depth, depth);
	}
	case LogicalTypeId::LIST: {
		// Only lists can be merged into a list
		if (type.id() != LogicalTypeId::LIST) {
			return -1;
		}
		D_ASSERT(type.id() == LogicalTypeId::LIST);
		const auto &merged_child_type = ListType::GetChildType(merged);
		const auto &type_child_type = ListType::GetChildType(type);
		return CalculateTypeSimilarity(merged_child_type, type_child_type, max_depth, depth + 1);
	}
	default:
		// This is only reachable if type has been inferred using candidate_types, but candidate_types were not
		// consistent among all map values
		return 1;
	}
}

static bool IsStructureInconsistent(const JSONStructureDescription &desc, const idx_t sample_count,
                                    const idx_t null_count, const double field_appearance_threshold) {
	D_ASSERT(sample_count > null_count);
	double total_child_counts = 0;
	for (const auto &child : desc.children) {
		total_child_counts += static_cast<double>(child.count) / static_cast<double>(sample_count - null_count);
	}
	const auto avg_occurrence = total_child_counts / static_cast<double>(desc.children.size());
	return avg_occurrence < field_appearance_threshold;
}

static LogicalType GetMergedType(ClientContext &context, const JSONStructureNode &node, const idx_t max_depth,
                                 const double field_appearance_threshold, const idx_t map_inference_threshold,
                                 const idx_t depth, const LogicalType &null_type) {
	D_ASSERT(node.descriptions.size() == 1);
	auto &desc = node.descriptions[0];
	auto merged = JSONStructureNode::EmptyLike(node);
	for (const auto &child : desc.children) {
		JSONStructure::MergeNodes(merged, child);
	}
	return JSONStructure::StructureToType(context, merged, max_depth, field_appearance_threshold,
	                                      map_inference_threshold, depth + 1, null_type);
}

static LogicalType StructureToTypeObject(ClientContext &context, const JSONStructureNode &node, const idx_t max_depth,
                                         const double field_appearance_threshold, const idx_t map_inference_threshold,
                                         const idx_t depth, const LogicalType &null_type) {
	D_ASSERT(node.descriptions.size() == 1 && node.descriptions[0].type == LogicalTypeId::STRUCT);
	auto &desc = node.descriptions[0];

	if (desc.children.empty()) {
		if (map_inference_threshold != DConstants::INVALID_INDEX) {
			// Empty struct - use MAP(VARCHAR, JSON) as a generic container;
			// JSON() is safe for all output formats and is not affected by ExchangeNullType
			return LogicalType::MAP(LogicalType::VARCHAR, LogicalType::JSON());
		} else {
			return LogicalType::JSON();
		}
	}

	// An empty key can't be a STRUCT member name, so such objects can only be a MAP or JSON
	bool has_empty_key = false;
	for (auto &child : desc.children) {
		D_ASSERT(child.key);
		has_empty_key = has_empty_key || child.key->empty();
	}
	if (has_empty_key && map_inference_threshold == DConstants::INVALID_INDEX) {
		return LogicalType::JSON();
	}

	// If it's an inconsistent object we also just do MAP with the best-possible, recursively-merged value type
	if (has_empty_key || (map_inference_threshold != DConstants::INVALID_INDEX &&
	                      IsStructureInconsistent(desc, node.count, node.null_count, field_appearance_threshold))) {
		return LogicalType::MAP(LogicalType::VARCHAR,
		                        GetMergedType(context, node, max_depth, field_appearance_threshold,
		                                      map_inference_threshold, depth + 1, null_type));
	}

	// We have a consistent object
	child_list_t<LogicalType> child_types;
	child_types.reserve(desc.children.size());
	for (auto &child : desc.children) {
		D_ASSERT(child.key);
		child_types.emplace_back(*child.key,
		                         JSONStructure::StructureToType(context, child, max_depth, field_appearance_threshold,
		                                                        map_inference_threshold, depth + 1, null_type));
	}

	// If we have many children and all children have similar-enough types we infer map
	if (desc.children.size() >= map_inference_threshold) {
		LogicalType map_value_type = GetMergedType(context, node, max_depth, field_appearance_threshold,
		                                           map_inference_threshold, depth + 1, LogicalTypeId::SQLNULL);

		double total_similarity = 0;
		for (const auto &child_type : child_types) {
			const auto similarity = CalculateTypeSimilarity(map_value_type, child_type.second, max_depth, depth + 1);
			if (similarity < 0) {
				total_similarity = similarity;
				break;
			}
			total_similarity += similarity;
		}
		const auto avg_similarity = total_similarity / static_cast<double>(child_types.size());
		if (avg_similarity >= 0.8) {
			if (null_type != LogicalTypeId::SQLNULL) {
				map_value_type = GetMergedType(context, node, max_depth, field_appearance_threshold,
				                               map_inference_threshold, depth + 1, null_type);
			}
			return LogicalType::MAP(LogicalType::VARCHAR, map_value_type);
		}
	}

	return LogicalType::STRUCT(child_types);
}

static LogicalType StructureToTypeString(const JSONStructureNode &node) {
	D_ASSERT(node.descriptions.size() == 1 && node.descriptions[0].type == LogicalTypeId::VARCHAR);
	auto &desc = node.descriptions[0];
	if (desc.candidate_types.empty()) {
		return LogicalTypeId::VARCHAR;
	}
	return desc.candidate_types.back();
}

LogicalType JSONStructure::StructureToType(ClientContext &context, const JSONStructureNode &node, const idx_t max_depth,
                                           const double field_appearance_threshold, const idx_t map_inference_threshold,
                                           const idx_t depth, const LogicalType &null_type) {
	if (depth >= max_depth) {
		return LogicalType::JSON();
	}
	if (node.descriptions.empty()) {
		return LogicalType::JSON(); // no data observed, fall back to generic JSON type
	}
	if (node.descriptions.size() != 1) { // Inconsistent types, so we resort to JSON
		return LogicalType::JSON();
	}
	auto &desc = node.descriptions[0];
	D_ASSERT(desc.type != LogicalTypeId::INVALID);
	switch (desc.type) {
	case LogicalTypeId::LIST:
		return StructureToTypeArray(context, node, max_depth, field_appearance_threshold, map_inference_threshold,
		                            depth, null_type);
	case LogicalTypeId::STRUCT:
		return StructureToTypeObject(context, node, max_depth, field_appearance_threshold, map_inference_threshold,
		                             depth, null_type);
	case LogicalTypeId::VARCHAR:
		return StructureToTypeString(node);
	case LogicalTypeId::UBIGINT:
		if (desc.has_large_ubigint) {
			return LogicalTypeId::HUGEINT;
		}
		return LogicalTypeId::BIGINT;
	case LogicalTypeId::SQLNULL:
		return LogicalType::SQLNULL;
	case LogicalTypeId::GEOMETRY:
		return LogicalType::GEOMETRY();
	default:
		return desc.type;
	}
}

} // namespace duckdb
