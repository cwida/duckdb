#pragma once
#include "json_common.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

class JsonDeserializer : public Deserializer {
public:
	JsonDeserializer(yyjson_val *val, const yyjson_doc_ptr &doc) : doc(doc.get()) {
		deserialize_enum_from_string = true;
		stack.emplace_back(val);
	}

private:
	struct StackFrame {
		yyjson_val *val;
		yyjson_arr_iter arr_iter;
		explicit StackFrame(yyjson_val *val) : val(val) {
			yyjson_arr_iter_init(val, &arr_iter);
		}
	};

	//! Deserializing recurses once per nesting level of the JSON - keep it within small thread stacks
	static constexpr idx_t MAX_NESTING_DEPTH = 256;

	yyjson_doc *doc;
	const char *current_tag = nullptr;
	vector<StackFrame> stack;

	void DumpDoc();
	void DumpCurrent();
	void Dump(yyjson_mut_val *val);
	void Dump(yyjson_val *val);

	// Get the current json value
	inline StackFrame &Current() {
		return stack.back();
	};

	inline void Push(yyjson_val *val) {
		if (stack.size() >= MAX_NESTING_DEPTH) {
			throw SerializationException("Serialized JSON exceeds the maximum nesting depth of %d", MAX_NESTING_DEPTH);
		}
		stack.emplace_back(val);
	}
	inline void Pop() {
		stack.pop_back();
	}
	yyjson_val *GetNextValue();

	void ThrowTypeError(yyjson_val *val, const char *expected);

	//===--------------------------------------------------------------------===//
	// Nested Types Hooks
	//===--------------------------------------------------------------------===//
	void OnPropertyBegin(const field_id_t field_id, const char *tag) final;
	void OnPropertyEnd() final;
	bool OnOptionalPropertyBegin(const field_id_t field_id, const char *tag) final;
	void OnOptionalPropertyEnd(bool present) final;

	void OnObjectBegin() final;
	void OnObjectEnd() final;
	idx_t OnListBegin() final;
	void OnListEnd() final;
	bool OnNullableBegin() final;
	void OnNullableEnd() final;

	bool CanDeserializeProperty(const field_id_t field_id, const char *tag) final;

	//===--------------------------------------------------------------------===//
	// Primitive Types
	//===--------------------------------------------------------------------===//
	bool ReadBool() final;
	int8_t ReadSignedInt8() final;
	uint8_t ReadUnsignedInt8() final;
	int16_t ReadSignedInt16() final;
	uint16_t ReadUnsignedInt16() final;
	int32_t ReadSignedInt32() final;
	uint32_t ReadUnsignedInt32() final;
	int64_t ReadSignedInt64() final;
	uint64_t ReadUnsignedInt64() final;
	float ReadFloat() final;
	double ReadDouble() final;
	string ReadString() final;
	hugeint_t ReadHugeInt() final;
	uhugeint_t ReadUhugeInt() final;
	void ReadDataPtr(data_ptr_t &ptr, idx_t count) final;
};

} // namespace duckdb
