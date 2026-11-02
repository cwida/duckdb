#include "json_serializer.hpp"
#include "duckdb/common/types/blob.hpp"

namespace duckdb {

void JsonSerializer::PushValue(yyjson_mut_val *val) {
	auto current = Current();
	// Array case, just append the value
	if (yyjson_mut_is_arr(current)) {
		yyjson_mut_arr_append(current, val);
	}
	// Object case, use the currently set tag.
	else if (yyjson_mut_is_obj(current)) {
		yyjson_mut_obj_add(current, current_tag, val);
	}
	// Else throw
	else {
		throw InternalException("Cannot add value to non-array/object json value");
	}
}

void JsonSerializer::OnPropertyBegin(const field_id_t, const char *tag) {
	current_tag = yyjson_mut_strcpy(doc, tag);
	property_can_be_omitted.push_back(false);
}

void JsonSerializer::OnPropertyEnd() {
	property_can_be_omitted.pop_back();
}

void JsonSerializer::OnOptionalPropertyBegin(const field_id_t, const char *tag, bool present) {
	if (present) {
		current_tag = yyjson_mut_strcpy(doc, tag);
	}
	property_can_be_omitted.push_back(present);
}

void JsonSerializer::OnOptionalPropertyEnd(bool) {
	property_can_be_omitted.pop_back();
}

//-------------------------------------------------------------------------
// Nested Types
//-------------------------------------------------------------------------
void JsonSerializer::OnNullableBegin(bool present) {
	if (!present && !(skip_if_null && yyjson_mut_is_obj(Current()) && CurrentPropertyCanBeOmitted())) {
		WriteNull();
	}
}

void JsonSerializer::OnNullableEnd() {
}

void JsonSerializer::OnListBegin(idx_t count) {
	auto new_value = yyjson_mut_arr(doc);
	auto can_be_omitted = yyjson_mut_is_obj(Current()) && CurrentPropertyCanBeOmitted();
	// We always push a value to the stack, we just don't add it as a child to the current value
	// if skipping empty. Even though it is "unnecessary" to create an empty value just to discard it,
	// this allows the rest of the code to keep on like normal.
	if (!(count == 0 && skip_if_empty && can_be_omitted)) {
		PushValue(new_value);
	}
	stack.push_back(new_value);
	stack_can_be_omitted.push_back(can_be_omitted);
}

void JsonSerializer::OnListEnd() {
	stack.pop_back();
	stack_can_be_omitted.pop_back();
}

void JsonSerializer::OnObjectBegin() {
	auto new_value = yyjson_mut_obj(doc);
	auto can_be_omitted = yyjson_mut_is_obj(Current()) && CurrentPropertyCanBeOmitted();
	PushValue(new_value);
	stack.push_back(new_value);
	stack_can_be_omitted.push_back(can_be_omitted);
}

void JsonSerializer::OnObjectEnd() {
	auto obj = Current();
	auto count = yyjson_mut_obj_size(obj);
	auto can_be_omitted = stack_can_be_omitted.back();

	stack.pop_back();
	stack_can_be_omitted.pop_back();

	if (count == 0 && skip_if_empty && can_be_omitted && !stack.empty()) {
		// remove obj from parent since it was empty
		auto parent = Current();
		if (yyjson_mut_is_arr(parent)) {
			size_t idx;
			size_t max;
			yyjson_mut_val *item;
			size_t found;
			yyjson_mut_arr_foreach(parent, idx, max, item) {
				if (item == obj) {
					found = idx;
				}
			}
			yyjson_mut_arr_remove(parent, found);
		} else if (yyjson_mut_is_obj(parent)) {
			size_t idx;
			size_t max;
			yyjson_mut_val *item;
			yyjson_mut_val *key;
			const char *found;
			// NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast): yyjson iteration macro
			yyjson_mut_obj_foreach(parent, idx, max, key, item) {
				if (item == obj) {
					found = yyjson_mut_get_str(key);
				}
			}
			yyjson_mut_obj_remove_key(parent, found);
		}
	}
}

//-------------------------------------------------------------------------
// Primitive Types
//-------------------------------------------------------------------------
void JsonSerializer::WriteNull() {
	if (skip_if_null && yyjson_mut_is_obj(Current()) && CurrentPropertyCanBeOmitted()) {
		return;
	}
	auto val = yyjson_mut_null(doc);
	PushValue(val);
}

void JsonSerializer::WriteValue(uint8_t value) {
	auto val = yyjson_mut_uint(doc, value);
	PushValue(val);
}

void JsonSerializer::WriteValue(int8_t value) {
	auto val = yyjson_mut_sint(doc, value);
	PushValue(val);
}

void JsonSerializer::WriteValue(uint16_t value) {
	auto val = yyjson_mut_uint(doc, value);
	PushValue(val);
}

void JsonSerializer::WriteValue(int16_t value) {
	auto val = yyjson_mut_sint(doc, value);
	PushValue(val);
}

void JsonSerializer::WriteValue(uint32_t value) {
	auto val = yyjson_mut_uint(doc, value);
	PushValue(val);
}

void JsonSerializer::WriteValue(int32_t value) {
	auto val = yyjson_mut_sint(doc, value);
	PushValue(val);
}

void JsonSerializer::WriteValue(uint64_t value) {
	auto val = yyjson_mut_uint(doc, value);
	PushValue(val);
}

void JsonSerializer::WriteValue(int64_t value) {
	auto val = yyjson_mut_sint(doc, value);
	PushValue(val);
}

void JsonSerializer::WriteValue(hugeint_t value) {
	auto val = yyjson_mut_obj(doc);
	PushValue(val);
	stack.push_back(val);
	WriteProperty(100, "upper", value.upper);
	WriteProperty(101, "lower", value.lower);
	stack.pop_back();
}

void JsonSerializer::WriteValue(uhugeint_t value) {
	auto val = yyjson_mut_obj(doc);
	PushValue(val);
	stack.push_back(val);
	WriteProperty(100, "upper", value.upper);
	WriteProperty(101, "lower", value.lower);
	stack.pop_back();
}

yyjson_mut_val *DoubleToJSONValue(yyjson_mut_doc *doc, double value) {
	if (Value::DoubleIsFinite(value)) {
		// simple - finite json
		return yyjson_mut_real(doc, value);
	}
	// represent infinities as strings
	const char *str;
	if (!Value::IsNan(value)) {
		str = value < 0 ? "-Infinity" : "Infinity";
	} else {
		str = "NAN";
	}
	return yyjson_mut_raw(doc, str);
}

void JsonSerializer::WriteValue(float value) {
	auto val = DoubleToJSONValue(doc, value);
	PushValue(val);
}

void JsonSerializer::WriteValue(double value) {
	auto val = DoubleToJSONValue(doc, value);
	PushValue(val);
}

void JsonSerializer::WriteValue(const string &value) {
	if (skip_if_empty && value.empty() && yyjson_mut_is_obj(Current()) && CurrentPropertyCanBeOmitted()) {
		return;
	}
	auto val = yyjson_mut_strncpy(doc, value.c_str(), value.size());
	PushValue(val);
}

void JsonSerializer::WriteValue(const string_t value) {
	if (skip_if_empty && value.GetSize() == 0 && yyjson_mut_is_obj(Current()) && CurrentPropertyCanBeOmitted()) {
		return;
	}
	auto val = yyjson_mut_strncpy(doc, value.GetData(), value.GetSize());
	PushValue(val);
}

void JsonSerializer::WriteValue(const char *value) {
	if (skip_if_empty && strlen(value) == 0 && yyjson_mut_is_obj(Current()) && CurrentPropertyCanBeOmitted()) {
		return;
	}
	auto val = yyjson_mut_strcpy(doc, value);
	PushValue(val);
}

void JsonSerializer::WriteValue(bool value) {
	auto val = yyjson_mut_bool(doc, value);
	PushValue(val);
}

void JsonSerializer::WriteDataPtr(const_data_ptr_t ptr, idx_t count) {
	auto blob = Blob::ToString(string_t(const_char_ptr_cast(ptr), count));
	auto val = yyjson_mut_strcpy(doc, blob.c_str());
	PushValue(val);
}

} // namespace duckdb
