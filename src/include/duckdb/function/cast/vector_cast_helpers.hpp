//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/cast/vector_cast_helpers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/general_cast.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/likely.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/function/cast/nested_to_varchar_cast.hpp"

namespace duckdb {

template <class OP>
struct VectorStringCastOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, StringHeap &heap) {
		return OP::template Operation<INPUT_TYPE>(input, heap);
	}
};

template <class OP>
struct VectorTryCastOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, VectorTryCastData &data) {
		RESULT_TYPE output;
		if (DUCKDB_LIKELY(OP::template Operation<INPUT_TYPE, RESULT_TYPE>(input, output))) {
			return output;
		}
		return HandleVectorCastError::Operation<RESULT_TYPE>(CastExceptionText<INPUT_TYPE, RESULT_TYPE>(input), mask,
		                                                     idx, data);
	}
};

template <class OP>
struct VectorTryCastStrictOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, VectorTryCastData &data) {
		RESULT_TYPE output;
		if (DUCKDB_LIKELY(OP::template Operation<INPUT_TYPE, RESULT_TYPE>(input, output, data.parameters.strict))) {
			return output;
		}
		return HandleVectorCastError::Operation<RESULT_TYPE>(CastExceptionText<INPUT_TYPE, RESULT_TYPE>(input), mask,
		                                                     idx, data);
	}
};

template <class OP>
struct VectorTryCastErrorOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, VectorTryCastData &data) {
		RESULT_TYPE output;
		if (DUCKDB_LIKELY(OP::template Operation<INPUT_TYPE, RESULT_TYPE>(input, output, data.parameters))) {
			return output;
		}
		bool has_error = data.parameters.error_message && !data.parameters.error_message->empty();
		return HandleVectorCastError::Operation<RESULT_TYPE>(
		    has_error ? *data.parameters.error_message : CastExceptionText<INPUT_TYPE, RESULT_TYPE>(input), mask, idx,
		    data);
	}
};

template <class OP>
struct VectorTryCastStringOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, VectorTryCastData &data) {
		RESULT_TYPE output;
		if (DUCKDB_LIKELY(
		        OP::template Operation<INPUT_TYPE, RESULT_TYPE>(input, output, data.result, data.parameters))) {
			return output;
		}
		return HandleVectorCastError::Operation<RESULT_TYPE>(
		    CastExceptionText<INPUT_TYPE>(input, data.result.GetType()), mask, idx, data);
	}
};

struct VectorDecimalCastData {
	VectorDecimalCastData(Vector &result, CastParameters &parameters, uint8_t width_p, uint8_t scale_p)
	    : vector_cast_data(result, parameters), width(width_p), scale(scale_p) {
	}

	VectorTryCastData vector_cast_data;
	uint8_t width;
	uint8_t scale;
};

template <class OP>
struct VectorDecimalCastOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t idx, VectorDecimalCastData &data) {
		RESULT_TYPE result_value;
		if (!OP::template Operation<INPUT_TYPE, RESULT_TYPE>(input, result_value, data.vector_cast_data.parameters,
		                                                     data.width, data.scale)) {
			return HandleVectorCastError::Operation<RESULT_TYPE>("Failed to cast decimal value", mask, idx,
			                                                     data.vector_cast_data);
		}
		return result_value;
	}
};

struct VectorCastHelpers {
	template <class SRC, class DST, class OP>
	static bool TemplatedCastLoop(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		UnaryExecutor::Execute<SRC, DST, OP>(source, result, count);
		return true;
	}

	template <class SRC, class DST, class OP>
	static bool TemplatedTryCastLoop(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		VectorTryCastData input(result, parameters);
		UnaryExecutor::GenericExecute<SRC, DST, OP>(source, result, count, input, parameters.error_message);
		return input.all_converted;
	}

	template <class SRC, class DST, class OP>
	static bool TryCastLoop(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return TemplatedTryCastLoop<SRC, DST, VectorTryCastOperator<OP>>(source, result, count, parameters);
	}

	template <class SRC, class DST, class OP>
	static bool TryCastStrictLoop(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return TemplatedTryCastLoop<SRC, DST, VectorTryCastStrictOperator<OP>>(source, result, count, parameters);
	}

	template <class SRC, class DST, class OP>
	static bool TryCastErrorLoop(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return TemplatedTryCastLoop<SRC, DST, VectorTryCastErrorOperator<OP>>(source, result, count, parameters);
	}

	template <class SRC, class DST, class OP>
	static bool TryCastStringLoop(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return TemplatedTryCastLoop<SRC, DST, VectorTryCastStringOperator<OP>>(source, result, count, parameters);
	}

	template <class SRC, class OP, class RES = string_t>
	static bool StringCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		D_ASSERT(result.GetType().InternalType() == PhysicalType::VARCHAR);
		auto &heap = StringVector::GetStringHeap(result);
		UnaryExecutor::GenericExecute<SRC, RES, VectorStringCastOperator<OP>>(source, result, count, heap);
		return true;
	}

	template <class SRC, class T, class OP>
	static bool TemplatedDecimalCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters,
	                                 uint8_t width, uint8_t scale) {
		VectorDecimalCastData input(result, parameters, width, scale);
		UnaryExecutor::GenericExecute<SRC, T, VectorDecimalCastOperator<OP>>(source, result, count, input,
		                                                                     parameters.error_message);
		return input.vector_cast_data.all_converted;
	}

	template <class T>
	static bool ToDecimalCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		auto &result_type = result.GetType();
		auto width = DecimalType::GetWidth(result_type);
		auto scale = DecimalType::GetScale(result_type);
		switch (result_type.InternalType()) {
		case PhysicalType::INT16:
			return TemplatedDecimalCast<T, int16_t, TryCastToDecimal>(source, result, count, parameters, width, scale);
		case PhysicalType::INT32:
			return TemplatedDecimalCast<T, int32_t, TryCastToDecimal>(source, result, count, parameters, width, scale);
		case PhysicalType::INT64:
			return TemplatedDecimalCast<T, int64_t, TryCastToDecimal>(source, result, count, parameters, width, scale);
		case PhysicalType::INT128:
			return TemplatedDecimalCast<T, hugeint_t, TryCastToDecimal>(source, result, count, parameters, width,
			                                                            scale);
		default:
			InternalException::Throw("Unimplemented internal type for decimal");
		}
	}

	template <bool STRUCT_KEY>
	static idx_t CalculateEscapedStringLength(const string_t &string, bool &needs_quotes) {
		auto base_length = string.GetSize();
		idx_t length = 0;
		auto string_data = string.GetData();
		needs_quotes = false;

		if (base_length == 0) {
			//! Empty quotes
			needs_quotes = true;
			return 2;
		}

		if (STRUCT_KEY) {
			needs_quotes = true;
		} else if (StringUtil::CharacterIsSpace(string_data[0])) {
			needs_quotes = true;
		} else if (base_length >= 2 && StringUtil::CharacterIsSpace(string_data[base_length - 1])) {
			needs_quotes = true;
		} else if (StringUtil::CIEquals(string_data, base_length, "null", 4)) {
			needs_quotes = true;
		} else {
			const bool *table = NestedToVarcharCast::LOOKUP_TABLE;
			for (idx_t i = 0; i < base_length; i++) {
				needs_quotes |= table[(uint8_t)string_data[i]];
			}
		}

		if (!needs_quotes) {
			return base_length;
		}

		for (idx_t i = 0; i < base_length; i++) {
			length += 1 + (string_data[i] == '\'' || string_data[i] == '\\');
		}
		length += 2;
		return length;
	}

	static idx_t CalculateStringLength(const string_t &string, bool &needs_quotes) {
		needs_quotes = false;
		return string.GetSize();
	}

	template <bool STRUCT_KEY>
	static idx_t WriteEscapedString(void *dest, const string_t &string, bool needs_quotes) {
		auto base_length = string.GetSize();
		if (base_length == 0) {
			D_ASSERT(needs_quotes);
			memcpy(dest, "''", 2);
			return 2;
		}

		auto string_start = string.GetData();
		auto string_data = string_start;

		auto destination = reinterpret_cast<char *>(dest);
		if (!needs_quotes) {
			memcpy(destination, string_data, base_length);
			return base_length;
		}

		idx_t offset = 0;
		destination[offset++] = '\'';

		for (idx_t i = 0; i < base_length; i++) {
			const bool needs_quote = string_data[i] == '\\' || string_data[i] == '\'';
			destination[offset] = '\\';
			destination[offset + needs_quote] = string_data[i];
			offset += 1 + needs_quote;
		}

		destination[offset++] = '\'';
		return offset;
	}

	static idx_t WriteString(void *dest, const string_t &string, bool needs_quotes) {
		D_ASSERT(needs_quotes == false);
		auto len = string.GetSize();
		memcpy(dest, string.GetData(), len);
		return len;
	}
};

struct VectorStringToList {
	static idx_t CountPartsList(const string_t &input);
	static bool SplitStringList(const string_t &input, string_t *child_data, idx_t &child_start, Vector &child);
	static bool StringToNestedTypeCastLoop(const string_t *source_data, ValidityMask &source_mask, Vector &result,
	                                       ValidityMask &result_mask, idx_t count, CastParameters &parameters,
	                                       const SelectionVector *sel);
};

struct VectorStringToArray {
	static bool StringToNestedTypeCastLoop(const string_t *source_data, ValidityMask &source_mask, Vector &result,
	                                       ValidityMask &result_mask, idx_t count, CastParameters &parameters,
	                                       const SelectionVector *sel);
};

struct VectorStringToStruct {
	static bool SplitStruct(const string_t &input, vector<Vector> &varchar_vectors, idx_t &row_idx,
	                        string_map_t<idx_t> &child_names, vector<reference<ValidityMask>> &child_masks);
	static bool StringToNestedTypeCastLoop(const string_t *source_data, ValidityMask &source_mask, Vector &result,
	                                       ValidityMask &result_mask, idx_t count, CastParameters &parameters,
	                                       const SelectionVector *sel);
};

struct VectorStringToMap {
	static idx_t CountPartsMap(const string_t &input);
	static bool SplitStringMap(const string_t &input, string_t *child_key_data, string_t *child_val_data,
	                           idx_t &child_start, Vector &varchar_key, Vector &varchar_val);
	static bool StringToNestedTypeCastLoop(const string_t *source_data, ValidityMask &source_mask, Vector &result,
	                                       ValidityMask &result_mask, idx_t count, CastParameters &parameters,
	                                       const SelectionVector *sel);
};

} // namespace duckdb
