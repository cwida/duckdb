#include "duckdb/common/json_document.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "yyjson.hpp"

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

//===--------------------------------------------------------------------===//
// Flag translation
//===--------------------------------------------------------------------===//
static yyjson_read_flag TranslateReadFlags(JSONReadFlags flags) {
	const auto raw = static_cast<uint32_t>(flags);
	yyjson_read_flag result = 0;
	if (raw & static_cast<uint32_t>(JSONReadFlags::ALLOW_INVALID_UNICODE)) {
		result |= YYJSON_READ_ALLOW_INVALID_UNICODE;
	}
	if (raw & static_cast<uint32_t>(JSONReadFlags::ALLOW_INF_AND_NAN)) {
		result |= YYJSON_READ_ALLOW_INF_AND_NAN;
	}
	if (raw & static_cast<uint32_t>(JSONReadFlags::ALLOW_TRAILING_COMMAS)) {
		result |= YYJSON_READ_ALLOW_TRAILING_COMMAS;
	}
	if (raw & static_cast<uint32_t>(JSONReadFlags::BIGNUM_AS_RAW)) {
		result |= YYJSON_READ_BIGNUM_AS_RAW;
	}
	return result;
}

static yyjson_write_flag TranslateWriteFlags(JSONWriteFlags flags) {
	const auto raw = static_cast<uint32_t>(flags);
	yyjson_write_flag result = 0;
	if (raw & static_cast<uint32_t>(JSONWriteFlags::ALLOW_INVALID_UNICODE)) {
		result |= YYJSON_WRITE_ALLOW_INVALID_UNICODE;
	}
	if (raw & static_cast<uint32_t>(JSONWriteFlags::ALLOW_INF_AND_NAN)) {
		result |= YYJSON_WRITE_ALLOW_INF_AND_NAN;
	}
	if (raw & static_cast<uint32_t>(JSONWriteFlags::PRETTY)) {
		result |= YYJSON_WRITE_PRETTY;
	}
	return result;
}

//===--------------------------------------------------------------------===//
// JSONValue
//===--------------------------------------------------------------------===//
JSONValue::JSONValue() : val(nullptr) {
}

JSONValue::JSONValue(yyjson_val *val_p) : val(val_p) {
}

bool JSONValue::IsValid() const {
	return val != nullptr;
}

JSONValueType JSONValue::GetType() const {
	if (!val) {
		return JSONValueType::INVALID;
	}
	switch (yyjson_get_tag(val)) {
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
		return JSONValueType::ARRAY;
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
		return JSONValueType::OBJECT;
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
		return JSONValueType::STRING;
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
		return JSONValueType::BOOLEAN;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
		return JSONValueType::UNSIGNED_INTEGER;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
		return JSONValueType::SIGNED_INTEGER;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
		return JSONValueType::DOUBLE;
	case YYJSON_TYPE_RAW | YYJSON_SUBTYPE_NONE:
		return JSONValueType::RAW;
	case YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE:
		return JSONValueType::JSON_NULL;
	default:
		return JSONValueType::INVALID;
	}
}

bool JSONValue::IsNull() const {
	return val && yyjson_is_null(val);
}

bool JSONValue::IsString() const {
	return val && yyjson_is_str(val);
}

bool JSONValue::IsArray() const {
	return val && yyjson_is_arr(val);
}

bool JSONValue::IsObject() const {
	return val && yyjson_is_obj(val);
}

bool JSONValue::IsInteger() const {
	return val && yyjson_is_int(val);
}

bool JSONValue::IsNumber() const {
	return val && yyjson_is_num(val);
}

string JSONValue::GetString() const {
	const auto str = yyjson_get_str(val);
	const auto len = yyjson_get_len(val);
	return string(str, len);
}

bool JSONValue::GetBoolean() const {
	return yyjson_get_bool(val);
}

uint64_t JSONValue::GetUnsignedInteger() const {
	return unsafe_yyjson_get_uint(val);
}

int64_t JSONValue::GetSignedInteger() const {
	return unsafe_yyjson_get_sint(val);
}

double JSONValue::GetDouble() const {
	return unsafe_yyjson_get_real(val);
}

double JSONValue::GetNumber() const {
	return unsafe_yyjson_get_num(val);
}

void JSONValue::IterateArray(const std::function<void(JSONValue)> &callback) const {
	size_t idx, max;
	yyjson_val *child;
	yyjson_arr_foreach(val, idx, max, child) {
		callback(JSONValue(child));
	}
}

void JSONValue::IterateObject(const std::function<void(const string &, JSONValue)> &callback) const {
	size_t idx, max;
	yyjson_val *key, *value;
	yyjson_obj_foreach(val, idx, max, key, value) {
		callback(string(yyjson_get_str(key), yyjson_get_len(key)), JSONValue(value));
	}
}

JSONValue JSONValue::GetMember(const string &key) const {
	if (!val || !yyjson_is_obj(val)) {
		return JSONValue();
	}
	return JSONValue(yyjson_obj_get(val, key.c_str()));
}

string JSONValue::ToString(JSONWriteFlags flags) const {
	size_t len;
	yyjson_write_err write_error;
	auto json = yyjson_val_write_opts(val, TranslateWriteFlags(flags), nullptr, &len, &write_error);
	if (!json) {
		if (write_error.code == YYJSON_WRITE_ERROR_MEMORY_ALLOCATION) {
			throw std::bad_alloc();
		}
		throw SerializationException("Failed to write JSON string");
	}
	string result(json, len);
	free(json);
	return result;
}

//===--------------------------------------------------------------------===//
// JSONDocument
//===--------------------------------------------------------------------===//
JSONDocument::JSONDocument() : doc(nullptr) {
}

JSONDocument::~JSONDocument() {
	if (doc) {
		yyjson_doc_free(doc);
	}
}

JSONDocument::JSONDocument(JSONDocument &&other) noexcept : doc(other.doc) {
	other.doc = nullptr;
}

JSONDocument &JSONDocument::operator=(JSONDocument &&other) noexcept {
	std::swap(doc, other.doc);
	return *this;
}

unique_ptr<JSONDocument> JSONDocument::TryParse(const char *data, idx_t len, JSONParseError &error,
                                                JSONReadFlags flags) {
	yyjson_read_err read_error;
	auto parsed = yyjson_read_opts((char *)data, len, TranslateReadFlags(flags), nullptr, &read_error); // NOLINT
	if (!parsed || read_error.code != YYJSON_READ_SUCCESS) {
		if (parsed) {
			yyjson_doc_free(parsed);
		}
		if (read_error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION) {
			throw std::bad_alloc();
		}
		error.has_error = true;
		error.position = read_error.pos;
		error.message = read_error.msg ? read_error.msg : "Unknown error";
		return nullptr;
	}
	auto result = make_uniq<JSONDocument>();
	result->doc = parsed;
	return result;
}

unique_ptr<JSONDocument> JSONDocument::Parse(const char *data, idx_t len, JSONReadFlags flags) {
	JSONParseError error;
	auto result = TryParse(data, len, error, flags);
	if (!result) {
		throw InvalidInputException("Failed to parse JSON at byte %llu of input: %s", error.position, error.message);
	}
	return result;
}

JSONValue JSONDocument::GetRoot() const {
	return JSONValue(doc ? yyjson_doc_get_root(doc) : nullptr);
}

string JSONDocument::ToString(JSONWriteFlags flags) const {
	size_t len;
	auto json = yyjson_write(doc, TranslateWriteFlags(flags), &len);
	if (!json) {
		throw SerializationException("Failed to write JSON string");
	}
	string result(json, len);
	free(json);
	return result;
}

//===--------------------------------------------------------------------===//
// JSONMutableValue
//===--------------------------------------------------------------------===//
JSONMutableValue::JSONMutableValue() : doc(nullptr), val(nullptr) {
}

JSONMutableValue::JSONMutableValue(yyjson_mut_doc *doc_p, yyjson_mut_val *val_p) : doc(doc_p), val(val_p) {
}

bool JSONMutableValue::IsValid() const {
	return val != nullptr;
}

void JSONMutableValue::Add(const string &key, JSONMutableValue value) {
	auto key_val = yyjson_mut_strncpy(doc, key.c_str(), key.size());
	yyjson_mut_obj_add(val, key_val, value.val);
}

void JSONMutableValue::AddString(const string &key, const string &value) {
	auto key_val = yyjson_mut_strncpy(doc, key.c_str(), key.size());
	auto value_val = yyjson_mut_strncpy(doc, value.c_str(), value.size());
	yyjson_mut_obj_add(val, key_val, value_val);
}

void JSONMutableValue::Append(JSONMutableValue value) {
	yyjson_mut_arr_append(val, value.val);
}

void JSONMutableValue::AppendString(const string &value) {
	auto value_val = yyjson_mut_strncpy(doc, value.c_str(), value.size());
	yyjson_mut_arr_append(val, value_val);
}

//===--------------------------------------------------------------------===//
// JSONWriter
//===--------------------------------------------------------------------===//
JSONWriter::JSONWriter() : doc(yyjson_mut_doc_new(nullptr)) {
}

JSONWriter::~JSONWriter() {
	if (doc) {
		yyjson_mut_doc_free(doc);
	}
}

JSONWriter::JSONWriter(JSONWriter &&other) noexcept : doc(other.doc) {
	other.doc = nullptr;
}

JSONWriter &JSONWriter::operator=(JSONWriter &&other) noexcept {
	std::swap(doc, other.doc);
	return *this;
}

JSONMutableValue JSONWriter::CreateObject() {
	return JSONMutableValue(doc, yyjson_mut_obj(doc));
}

JSONMutableValue JSONWriter::CreateArray() {
	return JSONMutableValue(doc, yyjson_mut_arr(doc));
}

JSONMutableValue JSONWriter::CreateString(const string &value) {
	return JSONMutableValue(doc, yyjson_mut_strncpy(doc, value.c_str(), value.size()));
}

JSONMutableValue JSONWriter::CreateNull() {
	return JSONMutableValue(doc, yyjson_mut_null(doc));
}

JSONMutableValue JSONWriter::CreateBoolean(bool value) {
	return JSONMutableValue(doc, yyjson_mut_bool(doc, value));
}

JSONMutableValue JSONWriter::CreateUnsignedInteger(uint64_t value) {
	return JSONMutableValue(doc, yyjson_mut_uint(doc, value));
}

JSONMutableValue JSONWriter::CreateSignedInteger(int64_t value) {
	return JSONMutableValue(doc, yyjson_mut_sint(doc, value));
}

JSONMutableValue JSONWriter::CreateDouble(double value) {
	return JSONMutableValue(doc, yyjson_mut_real(doc, value));
}

JSONMutableValue JSONWriter::CreateCopy(const JSONValue &value) {
	return JSONMutableValue(doc, yyjson_val_mut_copy(doc, value.val));
}

void JSONWriter::SetRoot(JSONMutableValue value) {
	yyjson_mut_doc_set_root(doc, value.val);
}

string JSONWriter::ToString(JSONWriteFlags flags) const {
	yyjson_write_err err;
	size_t len;
	auto json = yyjson_mut_write_opts(doc, TranslateWriteFlags(flags), nullptr, &len, &err);
	if (!json) {
		throw SerializationException("Failed to write JSON string: %s", err.msg);
	}
	string result(json, len);
	free(json);
	return result;
}

} // namespace duckdb
