#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/numeric_blob.hpp"
#include "duckdb/common/types/numeric_type.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector/numeric_vector.hpp"
#include "duckdb/common/vector/shredded_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/cast/default_casts.hpp"

#include <cmath>

namespace duckdb {

//===--------------------------------------------------------------------===//
// to NUMERIC
//===--------------------------------------------------------------------===//
//! An exact value is a NUMERIC value: the result references the source as its representation
static bool CastExactToNumeric(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	result.Shred(source, count);
	return true;
}

//! Text is parsed into the representation that holds every number of the vector; rows that are not exact numbers
//! fail on their own, and numbers with no common representation are kept as canonical values
static bool CastTextToNumeric(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	if (count == 0) {
		return true;
	}
	UnifiedVectorFormat vdata;
	source.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
	// the rows that are numbers, referencing the source text
	Vector numbers(LogicalType::VARCHAR, count);
	StringVector::AddHeapReference(numbers, source);
	auto number_data = FlatVector::GetDataMutable<string_t>(numbers);
	auto &number_validity = FlatVector::ValidityMutable(numbers);
	idx_t integer_digits = 0;
	idx_t fractional_digits = 0;
	bool found = false;
	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			number_validity.SetInvalid(i);
			continue;
		}
		auto &text = data[idx];
		idx_t value_integer_digits, value_fractional_digits;
		if (!NumericType::TryCountDigits(text.GetData(), text.GetSize(), value_integer_digits,
		                                 value_fractional_digits)) {
			HandleCastError::AssignError(
			    StringUtil::Format("Cannot convert '%s' to NUMERIC: not an exact number", text.GetString()),
			    parameters);
			number_validity.SetInvalid(i);
			success = false;
			continue;
		}
		if (value_integer_digits + value_fractional_digits > Decimal::MAX_WIDTH_DECIMAL) {
			HandleCastError::AssignError(
			    StringUtil::Format("NUMERIC value needs %d digits, more than the maximum of %d",
			                       value_integer_digits + value_fractional_digits, Decimal::MAX_WIDTH_DECIMAL),
			    parameters);
			number_validity.SetInvalid(i);
			success = false;
			continue;
		}
		if (NumericType::HasExponent(text.GetData(), text.GetSize())) {
			// the decimal casts do not take every exponent: plain decimal notation
			auto plain = NumericType::ExpandExponent(text.GetData(), text.GetSize());
			number_data[i] = StringVector::AddString(numbers, plain);
		} else {
			number_data[i] = text;
		}
		integer_digits = MaxValue(integer_digits, value_integer_digits);
		fractional_digits = MaxValue(fractional_digits, value_fractional_digits);
		found = true;
	}
	LogicalType type = NumericType::DefaultType();
	if (found && !NumericType::TryForDigits(integer_digits, fractional_digits, type)) {
		// no representation holds every number: canonical values
		result.SetVectorType(VectorType::FLAT_VECTOR);
		auto writer = FlatVector::Writer<string_t>(result, count);
		string blob;
		for (idx_t i = 0; i < count; i++) {
			if (!number_validity.RowIsValid(i)) {
				writer.WriteNull();
				continue;
			}
			if (!NumericBlob::TryEncode(number_data[i].GetData(), number_data[i].GetSize(), blob)) {
				throw InternalException("Number '%s' does not encode as NUMERIC", number_data[i].GetString());
			}
			writer.WriteValue(StringVector::AddStringOrBlob(result, blob.c_str(), blob.size()));
		}
		return success;
	}
	Vector typed(type, count);
	if (!VectorOperations::DefaultTryCast(numbers, typed, count, parameters.error_message, parameters.strict)) {
		success = false;
	}
	result.Shred(typed, count);
	return success;
}

//! Floating point values take their shortest round-trip decimal text (NaN and infinities are not exact numbers)
static bool CastFloatingToNumeric(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	Vector text(LogicalType::VARCHAR, count);
	VectorOperations::DefaultCast(source, text, count, true);
	return CastTextToNumeric(text, result, count, parameters);
}

//! A VARIANT holds its numbers as typed values of known types (integers, decimals, doubles) or as text: the text of
//! the VARIANT is the number as it would print, which parses into NUMERIC like any text
static bool CastVariantToNumeric(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	Vector text(LogicalType::VARCHAR, count);
	if (!VectorOperations::DefaultTryCast(source, text, count, parameters.error_message, parameters.strict)) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(result, count_t(count));
		return false;
	}
	return CastTextToNumeric(text, result, count, parameters);
}

BoundCastInfo DefaultCasts::ToNumericCastSwitch(BindCastInput &input, const LogicalType &source,
                                                const LogicalType &target) {
	D_ASSERT(target.id() == LogicalTypeId::NUMERIC);
	if (NumericType::IsExact(source)) {
		return CastExactToNumeric;
	}
	switch (source.id()) {
	case LogicalTypeId::VARCHAR:
		return CastTextToNumeric;
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return CastFloatingToNumeric;
	case LogicalTypeId::VARIANT:
		return CastVariantToNumeric;
	default:
		return TryVectorNullCast;
	}
}

//===--------------------------------------------------------------------===//
// from NUMERIC
//===--------------------------------------------------------------------===//
//! The decimal text of every value, the same whatever its representation: no exponent, no trailing zeros
//! Writes the decimal text of a fixed-size DECIMAL representation without the trailing zeros of the fraction
template <class T>
static void WriteDecimalText(const Vector &typed, idx_t count, uint32_t width, uint32_t scale, Vector &result) {
	UnifiedVectorFormat vdata;
	typed.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<T>(vdata);
	auto writer = FlatVector::Writer<string_t>(result, count);
	char buffer[64];
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			writer.WriteNull();
			continue;
		}
		auto value = data[idx];
		auto length = idx_t(DecimalToString::DecimalLength<T>(value, width, scale));
		DecimalToString::FormatDecimal<T>(value, width, scale, buffer, length);
		while (buffer[length - 1] == '0') {
			length--;
		}
		if (buffer[length - 1] == '.') {
			length--;
		}
		if (length == 2 && buffer[0] == '-' && buffer[1] == '0') {
			// a negative zero after trimming
			length = 1;
			buffer[0] = '0';
		}
		if (length <= string_t::INLINE_LENGTH) {
			writer.WriteStringRef(string_t(buffer, UnsafeNumericCast<uint32_t>(length)));
		} else {
			auto &text = writer.WriteEmptyString(length);
			memcpy(text.GetDataWriteable(), buffer, length);
			text.Finalize();
		}
	}
}

static bool CastNumericToVarchar(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	if (source.GetVectorType() == VectorType::SHREDDED_VECTOR) {
		auto &typed = ShreddedVector::GetShreddedVector(source);
		auto &type = typed.GetType();
		if (type.id() != LogicalTypeId::DECIMAL || DecimalType::GetScale(type) == 0) {
			// whole numbers print as they are
			VectorOperations::DefaultCast(typed, result, count, true);
			return true;
		}
		auto width = DecimalType::GetWidth(type);
		auto scale = DecimalType::GetScale(type);
		result.SetVectorType(VectorType::FLAT_VECTOR);
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			WriteDecimalText<int16_t>(typed, count, width, scale, result);
			return true;
		case PhysicalType::INT32:
			WriteDecimalText<int32_t>(typed, count, width, scale, result);
			return true;
		case PhysicalType::INT64:
			WriteDecimalText<int64_t>(typed, count, width, scale, result);
			return true;
		case PhysicalType::INT128:
			WriteDecimalText<hugeint_t>(typed, count, width, scale, result);
			return true;
		default:
			break;
		}
		// wide decimals: the text of the representation without the trailing zeros of the fraction, in place
		Vector text(LogicalType::VARCHAR, count);
		VectorOperations::DefaultCast(typed, text, count, true);
		UnifiedVectorFormat vdata;
		text.ToUnifiedFormat(vdata);
		auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
		StringVector::AddHeapReference(result, text);
		auto writer = FlatVector::Writer<string_t>(result, count);
		for (idx_t i = 0; i < count; i++) {
			auto idx = vdata.sel->get_index(i);
			if (!vdata.validity.RowIsValid(idx)) {
				writer.WriteNull();
				continue;
			}
			auto ptr = data[idx].GetData();
			idx_t end = data[idx].GetSize();
			while (end > 0 && ptr[end - 1] == '0') {
				end--;
			}
			if (end > 0 && ptr[end - 1] == '.') {
				end--;
			}
			if (end == 2 && ptr[0] == '-' && ptr[1] == '0') {
				// a negative zero after trimming
				writer.WriteValue(StringVector::AddString(result, "0"));
				continue;
			}
			writer.WriteStringRef(string_t(ptr, UnsafeNumericCast<uint32_t>(end)));
		}
		return true;
	}
	UnifiedVectorFormat vdata;
	source.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto writer = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			writer.WriteNull();
			continue;
		}
		auto value_text = NumericBlob::ToText(data[idx]);
		writer.WriteValue(StringVector::AddString(result, value_text.c_str(), value_text.size()));
	}
	return true;
}

//! Whether the values of a typed NUMERIC vector are the same bits as values of the target: the same physical type
//! and scale, and a value range the target holds (a DECIMAL(12,2) representation read as DECIMAL(10,2))
static bool SameBits(Vector &source, const Vector &typed, const LogicalType &target, idx_t count) {
	auto &type = typed.GetType();
	if (type.InternalType() != target.InternalType() || !NumericType::IsExact(target)) {
		return false;
	}
	uint32_t source_scale = type.id() == LogicalTypeId::DECIMAL ? DecimalType::GetScale(type) : 0;
	hugeint_t min, max;
	uint32_t target_scale;
	if (!NumericType::TryGetRepresentationRange(target, min, max, target_scale) || target_scale != source_scale) {
		return false;
	}
	ShreddedNumericRange range;
	if (!NumericVector::TryGetRange(source, count, range)) {
		return false;
	}
	return min <= range.min && range.max <= max;
}

//! Casts the representation of a NUMERIC vector to the target. A UHUGEINT value beyond the FLOAT range is out of
//! range, as it is for a canonical value: the plain UHUGEINT cast rounds it to infinity instead
static bool CastRepresentation(Vector &typed, Vector &result, idx_t count, CastParameters &parameters) {
	if (typed.GetType().id() != LogicalTypeId::UHUGEINT || result.GetType().id() != LogicalTypeId::FLOAT) {
		return VectorOperations::DefaultTryCast(typed, result, count, parameters.error_message, parameters.strict);
	}
	auto values = typed.Values<uhugeint_t>();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto writer = FlatVector::Writer<float>(result, count);
	bool success = true;
	for (auto entry : values) {
		if (!entry.IsValid()) {
			writer.WriteNull();
			continue;
		}
		float value;
		Uhugeint::TryCast(entry.GetValue(), value);
		if (!std::isfinite(value)) {
			HandleCastError::AssignError(
			    StringUtil::Format("Casting value \"%s\" to type %s failed: value is out of range!",
			                       entry.GetValue().ToString(), result.GetType().ToString()),
			    parameters);
			writer.WriteNull();
			success = false;
			continue;
		}
		writer.WriteValue(value);
	}
	return success;
}

static bool CastNumericToTyped(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	if (source.GetVectorType() == VectorType::SHREDDED_VECTOR) {
		auto &typed = ShreddedVector::GetShreddedVector(source);
		if (typed.GetType() == result.GetType()) {
			result.Reference(typed);
			return true;
		}
		if (SameBits(source, typed, result.GetType(), count)) {
			result.Reinterpret(typed);
			return true;
		}
		return CastRepresentation(typed, result, count, parameters);
	}
	// canonical values: decoded directly when every value fits the target
	if (NumericType::IsExact(result.GetType()) && NumericBlob::TryDecode(source, count, result)) {
		return true;
	}
	// otherwise through the representation that holds them, so that the conversion follows the numeric cast
	// semantics and reports the values that do not fit
	LogicalType type;
	if (NumericBlob::TryRangeType(source, count, type)) {
		Vector typed(type, count);
		NumericBlob::Decode(source, count, typed);
		return CastRepresentation(typed, result, count, parameters);
	}
	// values with no common representation convert one at a time
	UnifiedVectorFormat vdata;
	source.ToUnifiedFormat(vdata);
	auto data = UnifiedVectorFormat::GetData<string_t>(vdata);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &target = result.GetType();
	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		auto idx = vdata.sel->get_index(i);
		if (!vdata.validity.RowIsValid(idx)) {
			FlatVector::SetNull(result, i, true);
			continue;
		}
		auto text = NumericBlob::ToText(data[idx]);
		Value value;
		if (!NumericType::TryParseLiteral(text, value)) {
			throw InternalException("NUMERIC value '%s' does not parse", text);
		}
		string error;
		auto cast = value.DefaultTryCastAs(target, &error, parameters.strict);
		if (!cast) {
			HandleCastError::AssignError(
			    error.empty() ? StringUtil::Format("Cannot convert NUMERIC value %s to %s", text, target.ToString())
			                  : error,
			    parameters);
			FlatVector::SetNull(result, i, true);
			success = false;
			continue;
		}
		result.SetValue(i, *cast);
	}
	return success;
}

BoundCastInfo DefaultCasts::FromNumericCastSwitch(BindCastInput &input, const LogicalType &source,
                                                  const LogicalType &target) {
	D_ASSERT(source.id() == LogicalTypeId::NUMERIC);
	if (NumericType::IsExact(target)) {
		return CastNumericToTyped;
	}
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return CastNumericToVarchar;
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::BOOLEAN:
		return CastNumericToTyped;
	default:
		return TryVectorNullCast;
	}
}

} // namespace duckdb
