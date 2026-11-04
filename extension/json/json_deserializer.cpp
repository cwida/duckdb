#include "json_deserializer.hpp"
#include "duckdb/common/types/blob.hpp"

namespace duckdb {

namespace {

template <class T>
T CheckSignedRange(int64_t value, const char *type_name) {
	if (value < int64_t(NumericLimits<T>::Minimum()) || value > int64_t(NumericLimits<T>::Maximum())) {
		throw SerializationException("Value %lld is out of range for type '%s'", value, type_name);
	}
	return static_cast<T>(value);
}

template <class T>
T CheckUnsignedRange(uint64_t value, const char *type_name) {
	if (value > uint64_t(NumericLimits<T>::Maximum())) {
		throw SerializationException("Value %llu is out of range for type '%s'", value, type_name);
	}
	return static_cast<T>(value);
}

} // namespace

void JsonDeserializer::OnPropertyBegin(const field_id_t, const char *tag) {
	current_tag = tag;
}

void JsonDeserializer::OnPropertyEnd() {
}

bool JsonDeserializer::OnOptionalPropertyBegin(const field_id_t, const char *tag) {
	auto parent = Current();
	auto present = yyjson_obj_get(parent.val, tag) != nullptr;
	if (present) {
		current_tag = tag;
	}
	return present;
}

void JsonDeserializer::OnOptionalPropertyEnd(bool) {
}

// If inside an object, return the value associated by the current tag (property name)
// If inside an array, return the next element in the sequence
yyjson_val *JsonDeserializer::GetNextValue() {
	auto &parent_val = Current();
	yyjson_val *val;
	if (yyjson_is_obj(parent_val.val)) {
		val = yyjson_obj_get(parent_val.val, current_tag);
		if (!val) {
			const char *json = yyjson_val_write(Current().val, 0, nullptr);
			auto msg =
			    StringUtil::Format("Expected but did not find property '%s' in json object: '%s'", current_tag, json);
			free((void *)json);
			throw ParserException(msg);
		}
	} else if (yyjson_is_arr(parent_val.val)) {
		val = yyjson_arr_iter_next(&parent_val.arr_iter);
		if (!val) {
			const char *json = yyjson_val_write(Current().val, 0, nullptr);
			auto msg =
			    StringUtil::Format("Expected but did not find another value after exhausting json array: '%s'", json);
			free((void *)json);
			throw ParserException(msg);
		}
	} else {
		// unreachable?
		throw InternalException("Cannot get value from non-array/object");
	}
	return val;
}

void JsonDeserializer::ThrowTypeError(yyjson_val *val, const char *expected) {
	auto actual = yyjson_get_type_desc(val);
	auto &parent = Current();
	if (yyjson_is_obj(parent.val)) {
		auto msg =
		    StringUtil::Format("property '%s' expected type '%s', but got type: '%s'", current_tag, expected, actual);
		throw ParserException(msg);
	} else if (yyjson_is_arr(parent.val)) {
		auto msg = StringUtil::Format("Sequence expect child of type '%s', but got type: %s", expected, actual);
		throw ParserException(msg);
	} else {
		// unreachable?
		throw InternalException("cannot get nested value from non object or array-type");
	}
}

void JsonDeserializer::DumpDoc() {
	const char *json = yyjson_write(doc, 0, nullptr);
	printf("json: %s\n", json);
	free((void *)json);
}

void JsonDeserializer::DumpCurrent() {
	const char *json = yyjson_val_write(Current().val, 0, nullptr);
	printf("json: %s\n", json);
	free((void *)json);
}

void JsonDeserializer::Dump(yyjson_mut_val *val) {
	const char *json = yyjson_mut_val_write(val, 0, nullptr);
	printf("json: %s\n", json);
	free((void *)json);
}

void JsonDeserializer::Dump(yyjson_val *val) {
	const char *json = yyjson_val_write(val, 0, nullptr);
	printf("json: %s\n", json);
	free((void *)json);
}

//===--------------------------------------------------------------------===//
// Nested Types Hooks
//===--------------------------------------------------------------------===//
void JsonDeserializer::OnObjectBegin() {
	auto val = GetNextValue();
	if (!yyjson_is_obj(val)) {
		ThrowTypeError(val, "object");
	}
	Push(val);
}

void JsonDeserializer::OnObjectEnd() {
	stack.pop_back();
}

idx_t JsonDeserializer::OnListBegin() {
	auto val = GetNextValue();
	if (!yyjson_is_arr(val)) {
		ThrowTypeError(val, "array");
	}
	Push(val);
	return yyjson_arr_size(val);
}

void JsonDeserializer::OnListEnd() {
	Pop();
}

bool JsonDeserializer::OnNullableBegin() {
	auto &parent_val = Current();
	yyjson_arr_iter iter;
	if (yyjson_is_arr(parent_val.val)) {
		iter = parent_val.arr_iter;
	}
	auto val = GetNextValue();

	// Recover the iterator if we are inside an array
	if (yyjson_is_arr(parent_val.val)) {
		parent_val.arr_iter = iter;
	}

	if (yyjson_is_null(val)) {
		return false;
	}

	return true;
}

void JsonDeserializer::OnNullableEnd() {
}

bool JsonDeserializer::CanDeserializeProperty(const field_id_t, const char *tag) {
	auto parent = Current();
	return yyjson_obj_get(parent.val, tag) != nullptr;
}

//===--------------------------------------------------------------------===//
// Primitive Types
//===--------------------------------------------------------------------===//
bool JsonDeserializer::ReadBool() {
	auto val = GetNextValue();
	if (!yyjson_is_bool(val)) {
		ThrowTypeError(val, "bool");
	}
	return yyjson_get_bool(val);
}

int8_t JsonDeserializer::ReadSignedInt8() {
	auto val = GetNextValue();
	if (!yyjson_is_int(val)) {
		ThrowTypeError(val, "int8_t");
	}
	return CheckSignedRange<int8_t>(yyjson_get_sint(val), "int8_t");
}

uint8_t JsonDeserializer::ReadUnsignedInt8() {
	auto val = GetNextValue();
	if (!yyjson_is_uint(val)) {
		ThrowTypeError(val, "uint8_t");
	}
	return CheckUnsignedRange<uint8_t>(yyjson_get_uint(val), "uint8_t");
}

int16_t JsonDeserializer::ReadSignedInt16() {
	auto val = GetNextValue();
	if (!yyjson_is_int(val)) {
		ThrowTypeError(val, "int16_t");
	}
	return CheckSignedRange<int16_t>(yyjson_get_sint(val), "int16_t");
}

uint16_t JsonDeserializer::ReadUnsignedInt16() {
	auto val = GetNextValue();
	if (!yyjson_is_uint(val)) {
		ThrowTypeError(val, "uint16_t");
	}
	return CheckUnsignedRange<uint16_t>(yyjson_get_uint(val), "uint16_t");
}

int32_t JsonDeserializer::ReadSignedInt32() {
	auto val = GetNextValue();
	if (!yyjson_is_int(val)) {
		ThrowTypeError(val, "int32_t");
	}
	return CheckSignedRange<int32_t>(yyjson_get_sint(val), "int32_t");
}

uint32_t JsonDeserializer::ReadUnsignedInt32() {
	auto val = GetNextValue();
	if (!yyjson_is_uint(val)) {
		ThrowTypeError(val, "uint32_t");
	}
	return CheckUnsignedRange<uint32_t>(yyjson_get_uint(val), "uint32_t");
}

int64_t JsonDeserializer::ReadSignedInt64() {
	auto val = GetNextValue();
	if (!yyjson_is_int(val)) {
		ThrowTypeError(val, "int64_t");
	}
	return yyjson_get_sint(val);
}

uint64_t JsonDeserializer::ReadUnsignedInt64() {
	auto val = GetNextValue();
	if (!yyjson_is_uint(val)) {
		ThrowTypeError(val, "uint64_t");
	}
	return yyjson_get_uint(val);
}

float JsonDeserializer::ReadFloat() {
	return LossyNumericCast<float>(ReadDouble());
}

double JsonDeserializer::ReadDouble() {
	auto val = GetNextValue();
	if (yyjson_is_raw(val)) {
		double res;
		string_t input(yyjson_get_raw(val));
		if (!TryCast::Operation<string_t, double>(input, res)) {
			throw InvalidInputException("Expected a floating point number, but got the string %s", input.GetString());
		}
		return res;
	}
	if (!yyjson_is_real(val)) {
		ThrowTypeError(val, "double");
	}
	return yyjson_get_real(val);
}

string JsonDeserializer::ReadString() {
	auto val = GetNextValue();
	if (!yyjson_is_str(val)) {
		ThrowTypeError(val, "string");
	}
	return yyjson_get_str(val);
}

hugeint_t JsonDeserializer::ReadHugeInt() {
	auto val = GetNextValue();
	if (!yyjson_is_obj(val)) {
		ThrowTypeError(val, "object");
	}
	Push(val);
	hugeint_t result;
	ReadProperty(100, "upper", result.upper);
	ReadProperty(101, "lower", result.lower);
	Pop();
	return result;
}

uhugeint_t JsonDeserializer::ReadUhugeInt() {
	auto val = GetNextValue();
	if (!yyjson_is_obj(val)) {
		ThrowTypeError(val, "object");
	}
	Push(val);
	uhugeint_t result;
	ReadProperty(100, "upper", result.upper);
	ReadProperty(101, "lower", result.lower);
	Pop();
	return result;
}

void JsonDeserializer::ReadDataPtr(data_ptr_t &ptr, idx_t count) {
	auto val = GetNextValue();
	if (!yyjson_is_str(val)) {
		ThrowTypeError(val, "string");
	}
	auto str = yyjson_get_str(val);
	auto len = yyjson_get_len(val);
	D_ASSERT(len == count);
	auto blob = string_t(str, len);
	Blob::ToString(blob, char_ptr_cast(ptr));
}

} // namespace duckdb
