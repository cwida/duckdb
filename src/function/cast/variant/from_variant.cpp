#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/vector/shredded_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "yyjson_memory.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types/variant_visitor.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/common/serializer/varint.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/string_cast.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "yyjson.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

namespace {

struct FromVariantConversionData {
public:
	FromVariantConversionData(RecursiveUnifiedVectorFormat &variant_format, optional_ptr<ClientContext> client)
	    : variant(variant_format), client(client) {
	}

	optional<Value> TryCastAs(const Value &value, const LogicalType &target_type, string *error_message = nullptr) {
		if (client) {
			return value.TryCastAs(*client, target_type, error_message);
		}
		return value.DefaultTryCastAs(target_type, error_message);
	}

public:
	//! The input Variant column
	UnifiedVariantVectorData variant;
	//! The optional client to use for casting
	optional_ptr<ClientContext> client;
	//! Checked at execution time so prepared casts see newly registered overrides.
	optional<bool> varchar_cast_override;
	//! If unsuccessful - the error of the conversion
	string error;
};

struct EmptyConversionPayloadFromVariant {};

//! string data
struct StringConversionPayload {
public:
	explicit StringConversionPayload(Vector &vec) : vec(vec) {
	}

public:
	//! The string vector that needs to own the non-inlined data
	Vector &vec;
};

//! decimal
struct DecimalConversionPayloadFromVariant {
public:
	DecimalConversionPayloadFromVariant(idx_t width, idx_t scale) : width(width), scale(scale) {
	}

public:
	idx_t width;
	idx_t scale;
};

struct ToVariantCastData : public BoundCastData {
public:
	explicit ToVariantCastData(optional_ptr<ClientContext> client) : client(client) {
	}

	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<ToVariantCastData>(client);
	}

public:
	optional_ptr<ClientContext> client;
};

} // namespace

//===--------------------------------------------------------------------===//
// VARIANT -> ANY
//===--------------------------------------------------------------------===//

static bool FinalizeErrorMessage(FromVariantConversionData &conversion_data, Vector &result,
                                 CastParameters &parameters) {
	auto conversion_error = StringUtil::Format("%s to '%s'", conversion_data.error, result.GetType().ToString());
	if (parameters.error_message) {
		*parameters.error_message = conversion_error;
		return false;
	}
	throw ConversionException(conversion_error);
}

//! ------- Primitive Conversion Methods -------

//! bool
struct VariantBooleanConversion {
	using type = bool;
	static bool Convert(const VariantLogicalType type_id, uint32_t byte_offset, const_data_ptr_t value, idx_t,
	                    bool &ret, const EmptyConversionPayloadFromVariant &payload, string &error) {
		if (type_id != VariantLogicalType::BOOL_FALSE && type_id != VariantLogicalType::BOOL_TRUE) {
			if (error.empty()) {
				error = StringUtil::Format("Can't convert from VARIANT(%s)", EnumUtil::ToString(type_id));
			}
			return false;
		}
		ret = type_id == VariantLogicalType::BOOL_TRUE;
		return true;
	}
};

//! any direct conversion (int8, date_t, dtime_t, timestamp, etc..)
template <class T, VariantLogicalType TYPE_ID>
struct VariantDirectConversion {
	using type = T;
	static bool Convert(const VariantLogicalType type_id, uint32_t byte_offset, const_data_ptr_t value, idx_t blob_size,
	                    T &ret, const EmptyConversionPayloadFromVariant &payload, string &error) {
		if (type_id != TYPE_ID) {
			if (error.empty()) {
				error = StringUtil::Format("Can't convert from VARIANT(%s)", EnumUtil::ToString(type_id));
			}
			return false;
		}
		VariantUtils::Available(blob_size, byte_offset, sizeof(T));
		ret = Load<T>(value + byte_offset);
		return true;
	}

	static bool Convert(const VariantLogicalType type_id, uint32_t byte_offset, const_data_ptr_t value, idx_t blob_size,
	                    T &ret, const StringConversionPayload &payload, string &error) {
		if (type_id != TYPE_ID) {
			if (error.empty()) {
				error = StringUtil::Format("Can't convert from VARIANT(%s)", EnumUtil::ToString(type_id));
			}
			return false;
		}
		VariantUtils::Available(blob_size, byte_offset, 1);
		auto ptr = value + byte_offset;
		auto length = VarintDecode<idx_t>(ptr);
		VariantUtils::Available(blob_size, NumericCast<uint32_t>(ptr - value), length);
		ret = StringVector::AddStringOrBlob(payload.vec, reinterpret_cast<const char *>(ptr), length);
		return true;
	}
};

//! decimal
template <class T>
struct VariantDecimalConversion {
	using type = T;
	static constexpr VariantLogicalType TYPE_ID = VariantLogicalType::DECIMAL;
	static bool Convert(const VariantLogicalType type_id, uint32_t byte_offset, const_data_ptr_t value, idx_t blob_size,
	                    T &ret, const DecimalConversionPayloadFromVariant &payload, string &error) {
		if (type_id != TYPE_ID) {
			if (error.empty()) {
				error = StringUtil::Format("Can't convert from VARIANT(%s)", EnumUtil::ToString(type_id));
			}
			return false;
		}
		VariantUtils::Available(blob_size, byte_offset, 1);
		auto ptr = value + byte_offset;
		auto width = VarintDecode<idx_t>(ptr);
		auto scale = VarintDecode<idx_t>(ptr);

		if (width != payload.width || scale != payload.scale) {
			error = StringUtil::Format("Can't convert from VARIANT(DECIMAL(%d, %d))", width, scale);
			return false;
		}
		VariantUtils::Available(blob_size, NumericCast<uint32_t>(ptr - value), sizeof(T));
		ret = Load<T>(ptr);
		return true;
	}
};

//! Casts a single value by converting it to a Value first
static bool CastVariantValue(FromVariantConversionData &conversion_data, Vector &result, idx_t result_index,
                             idx_t row_index, uint32_t values_index) {
	auto &target_type = result.GetType();
	auto value = VariantUtils::ConvertVariantToValue(conversion_data.variant, row_index, values_index);
	auto cast_value = conversion_data.TryCastAs(value, target_type);
	const bool success = cast_value.has_value();
	if (!success) {
		auto type_id = conversion_data.variant.GetTypeId(row_index, values_index);
		conversion_data.error =
		    StringUtil::Format("Can't convert VARIANT(%s) value '%s'", EnumUtil::ToString(type_id), value.ToString());
		cast_value = Value(target_type);
	}
	result.SetValue(result_index, *cast_value);
	return success;
}

template <class CONVERT>
static bool CastVariantRows(FromVariantConversionData &conversion_data, Vector &result, const SelectionVector &sel,
                            idx_t offset, idx_t count, optional_idx row, CONVERT &&convert) {
	auto &variant = conversion_data.variant;
	auto &result_validity = FlatVector::ValidityMutable(result);
	bool all_valid = true;
	for (idx_t i = 0; i < count; i++) {
		auto row_index = row.IsValid() ? row.GetIndex() : i;
		auto result_index = offset + i;
		if (!variant.RowIsValid(row_index)) {
			result_validity.SetInvalid(result_index);
			continue;
		}
		if (!result_validity.RowIsValid(result_index)) {
			continue;
		}
		if (!convert(row_index, sel[i], result_index) &&
		    !CastVariantValue(conversion_data, result, result_index, row_index, sel[i])) {
			all_valid = false;
		}
	}
	return all_valid;
}

template <class OP, class T = typename OP::type, class PAYLOAD_CLASS>
static bool CastVariantToPrimitive(FromVariantConversionData &conversion_data, Vector &result,
                                   const SelectionVector &sel, idx_t offset, idx_t count, optional_idx row,
                                   PAYLOAD_CLASS payload) {
	auto &variant = conversion_data.variant;
	auto result_data = FlatVector::GetDataMutable<T>(result);
	auto convert = [&](idx_t row_index, uint32_t values_index, idx_t result_index) {
		auto type_id = variant.GetTypeId(row_index, values_index);
		if (type_id == VariantLogicalType::OBJECT || type_id == VariantLogicalType::ARRAY) {
			return false;
		}
		auto byte_offset = variant.GetByteOffset(row_index, values_index);
		auto &blob = variant.GetData(row_index);
		return OP::Convert(type_id, byte_offset, const_data_ptr_cast(blob.GetData()), blob.GetSize(),
		                   result_data[result_index], payload, conversion_data.error);
	};
	return CastVariantRows(conversion_data, result, sel, offset, count, row, convert);
}

namespace {

//! Writes a VARIANT value as the text produced by casting its Value representation to VARCHAR, without
//! materializing the Value. It handles the types JSON produces and the scalar types whose text does not depend on
//! settings. Serialize returns false for values that need the Value conversion (objects that become MAPs or have
//! case-insensitively duplicate keys, time zone dependent types, ...).
class VariantVarcharSerializer {
public:
	using result_type = bool;
	using Visitor = VariantVisitor<VariantVarcharSerializer>;

	explicit VariantVarcharSerializer(const UnifiedVariantVectorData &variant)
	    : variant(variant), object_entries(Visitor::MAX_RECURSION_DEPTH), first_entries(Visitor::MAX_RECURSION_DEPTH),
	      other_entries(Visitor::MAX_RECURSION_DEPTH) {
	}

	bool Serialize(idx_t row, uint32_t values_index) {
		buffer.clear();
		heap.GetAllocator().Reset();
		return WriteValue(row, values_index, 0);
	}

	const string &GetResult() const {
		return buffer;
	}

private:
	struct ObjectEntry {
		string_t key;
		uint32_t values_index;
	};
	//! Collects the entries of an OBJECT, sorted by key. Returns false if the object is not converted to a STRUCT
	//! with unique names.
	bool GetSortedEntries(idx_t row, const VariantNestedData &nested_data, vector<ObjectEntry> &entries) {
		entries.clear();
		for (idx_t i = 0; i < nested_data.child_count; i++) {
			auto &key = variant.GetKey(row, variant.GetKeysIndex(row, nested_data.children_idx + i));
			if (key.Empty()) {
				return false;
			}
			entries.push_back(ObjectEntry {key, variant.GetValuesIndex(row, nested_data.children_idx + i)});
		}
		std::sort(entries.begin(), entries.end(), [](const ObjectEntry &left, const ObjectEntry &right) {
			return StringUtil::CILessThan(std::string_view(left.key.GetData(), left.key.GetSize()),
			                              std::string_view(right.key.GetData(), right.key.GetSize()));
		});
		for (idx_t i = 1; i < entries.size(); i++) {
			auto &left = entries[i - 1].key;
			auto &right = entries[i].key;
			if (StringUtil::CIEquals(left.GetData(), left.GetSize(), right.GetData(), right.GetSize())) {
				return false;
			}
		}
		std::sort(entries.begin(), entries.end(),
		          [](const ObjectEntry &left, const ObjectEntry &right) { return left.key < right.key; });
		return true;
	}

	//! A LIST casts its elements to the type of the first one, which renames STRUCT fields whose names only differ in
	//! case. Returns true if that can rename a field of `other`.
	bool NamesDifferInCase(idx_t row, uint32_t first, uint32_t other, idx_t depth) {
		if (depth >= Visitor::MAX_RECURSION_DEPTH) {
			return true;
		}
		auto first_type = variant.GetTypeId(row, first);
		auto other_type = variant.GetTypeId(row, other);
		if (first_type == VariantLogicalType::ARRAY && other_type == VariantLogicalType::ARRAY) {
			auto first_data = VariantUtils::DecodeNestedData(variant, row, first);
			auto other_data = VariantUtils::DecodeNestedData(variant, row, other);
			if (first_data.child_count == 0 || other_data.child_count == 0) {
				return false;
			}
			return NamesDifferInCase(row, variant.GetValuesIndex(row, first_data.children_idx),
			                         variant.GetValuesIndex(row, other_data.children_idx), depth + 1);
		}
		if (first_type != VariantLogicalType::OBJECT || other_type != VariantLogicalType::OBJECT) {
			return false;
		}
		auto &first_sorted = first_entries[depth];
		auto &other_sorted = other_entries[depth];
		if (!GetSortedEntries(row, VariantUtils::DecodeNestedData(variant, row, first), first_sorted) ||
		    !GetSortedEntries(row, VariantUtils::DecodeNestedData(variant, row, other), other_sorted) ||
		    first_sorted.size() != other_sorted.size()) {
			return false;
		}
		for (idx_t i = 0; i < first_sorted.size(); i++) {
			auto &first_key = first_sorted[i].key;
			auto &other_key = other_sorted[i].key;
			if (!StringUtil::CIEquals(first_key.GetData(), first_key.GetSize(), other_key.GetData(),
			                          other_key.GetSize())) {
				return false;
			}
			if (first_key != other_key ||
			    NamesDifferInCase(row, first_sorted[i].values_index, other_sorted[i].values_index, depth + 1)) {
				return true;
			}
		}
		return false;
	}

	template <bool IS_KEY>
	void WriteEscaped(const string_t &text) {
		bool needs_quotes;
		auto length = VectorCastHelpers::CalculateEscapedStringLength<IS_KEY>(text, needs_quotes);
		auto offset = buffer.size();
		buffer.resize(offset + length);
		VectorCastHelpers::WriteEscapedString<IS_KEY>(&buffer[offset], text, needs_quotes);
	}

	void WriteText(const string_t &text, bool escape) {
		if (escape) {
			WriteEscaped<false>(text);
		} else {
			buffer.append(text.GetData(), text.GetSize());
		}
	}

	bool WriteObject(idx_t row, const VariantNestedData &nested_data, idx_t depth) {
		auto &entries = object_entries[depth];
		if (!GetSortedEntries(row, nested_data, entries)) {
			return false;
		}
		buffer.push_back('{');
		for (idx_t i = 0; i < entries.size(); i++) {
			if (i > 0) {
				buffer.append(", ");
			}
			WriteEscaped<true>(entries[i].key);
			buffer.append(": ");
			if (!WriteValue(row, entries[i].values_index, depth + 1)) {
				return false;
			}
		}
		buffer.push_back('}');
		return true;
	}

	bool WriteArray(idx_t row, const VariantNestedData &nested_data, idx_t depth) {
		buffer.push_back('[');
		uint32_t first = 0;
		for (idx_t i = 0; i < nested_data.child_count; i++) {
			auto child = variant.GetValuesIndex(row, nested_data.children_idx + i);
			if (i == 0) {
				first = child;
			} else {
				if (NamesDifferInCase(row, first, child, depth + 1)) {
					return false;
				}
				buffer.append(", ");
			}
			if (!WriteValue(row, child, depth + 1)) {
				return false;
			}
		}
		buffer.push_back(']');
		return true;
	}

	//! Primitive members of a STRUCT or LIST are escaped.
	bool WriteValue(idx_t row, uint32_t values_index, idx_t depth) {
		return Visitor::Visit(variant, row, values_index, depth, *this, depth > 0);
	}

public:
	static bool VisitNull(VariantVarcharSerializer &writer, bool) {
		writer.buffer.append("NULL");
		return true;
	}

	template <class T>
	static bool VisitInteger(T value, VariantVarcharSerializer &writer, bool escape) {
		writer.WriteText(StringCast::Operation(value, writer.heap), escape);
		return true;
	}

	static bool VisitBoolean(bool value, VariantVarcharSerializer &writer, bool escape) {
		return VisitInteger(value, writer, escape);
	}

	static bool VisitFloat(float value, VariantVarcharSerializer &writer, bool escape) {
		return VisitInteger(value, writer, escape);
	}

	static bool VisitDouble(double value, VariantVarcharSerializer &writer, bool escape) {
		return VisitInteger(value, writer, escape);
	}

	static bool VisitDate(date_t value, VariantVarcharSerializer &writer, bool escape) {
		return VisitInteger(value, writer, escape);
	}

	static bool VisitTime(dtime_t value, VariantVarcharSerializer &writer, bool escape) {
		return VisitInteger(value, writer, escape);
	}

	static bool VisitTimestamp(timestamp_t value, VariantVarcharSerializer &writer, bool escape) {
		return VisitInteger(value, writer, escape);
	}

	static bool VisitInterval(interval_t value, VariantVarcharSerializer &writer, bool escape) {
		return VisitInteger(value, writer, escape);
	}

	static bool VisitUUID(hugeint_t value, VariantVarcharSerializer &writer, bool escape) {
		char uuid[UUID::STRING_SIZE];
		UUID::ToString(value, uuid);
		writer.WriteText(string_t(uuid, UUID::STRING_SIZE), escape);
		return true;
	}

	template <class T>
	static bool VisitDecimal(T value, uint32_t width, uint32_t scale, VariantVarcharSerializer &writer, bool escape) {
		writer.WriteText(StringCastFromDecimal::Operation(value, width, scale, writer.heap), escape);
		return true;
	}

	static bool VisitString(const string_t &value, VariantVarcharSerializer &writer, bool escape) {
		writer.WriteText(value, escape);
		return true;
	}

	static bool VisitObject(const UnifiedVariantVectorData &, idx_t row, const VariantNestedData &nested_data,
	                        idx_t depth, VariantVarcharSerializer &writer, bool) {
		return writer.WriteObject(row, nested_data, depth);
	}

	static bool VisitArray(const UnifiedVariantVectorData &, idx_t row, const VariantNestedData &nested_data,
	                       idx_t depth, VariantVarcharSerializer &writer, bool) {
		return writer.WriteArray(row, nested_data, depth);
	}

	//! Use the Value conversion for session-dependent and uncommon types.
	static bool VisitTimeNanos(dtime_ns_t, VariantVarcharSerializer &, bool) {
		return false;
	}
	static bool VisitTimeTZ(dtime_tz_t, VariantVarcharSerializer &, bool) {
		return false;
	}
	static bool VisitTimestampSec(timestamp_sec_t, VariantVarcharSerializer &, bool) {
		return false;
	}
	static bool VisitTimestampMs(timestamp_ms_t, VariantVarcharSerializer &, bool) {
		return false;
	}
	static bool VisitTimestampNanos(timestamp_ns_t, VariantVarcharSerializer &, bool) {
		return false;
	}
	static bool VisitTimestampTZ(timestamp_tz_t, VariantVarcharSerializer &, bool) {
		return false;
	}
	static bool VisitTimestampTZNanos(timestamp_tz_ns_t, VariantVarcharSerializer &, bool) {
		return false;
	}
	static bool VisitBlob(const string_t &, VariantVarcharSerializer &, bool) {
		return false;
	}
	static bool VisitBignum(const string_t &, VariantVarcharSerializer &, bool) {
		return false;
	}
	static bool VisitGeometry(const string_t &, VariantVarcharSerializer &, bool) {
		return false;
	}
	static bool VisitBitstring(const string_t &, VariantVarcharSerializer &, bool) {
		return false;
	}
	static bool VisitDefault(VariantLogicalType, const_data_ptr_t, VariantVarcharSerializer &, bool) {
		return false;
	}

private:
	const UnifiedVariantVectorData &variant;
	string buffer;
	StringHeap heap;
	//! Per-depth scratch space for the sorted entries of an OBJECT
	vector<vector<ObjectEntry>> object_entries;
	vector<vector<ObjectEntry>> first_entries;
	vector<vector<ObjectEntry>> other_entries;
};

} // namespace

#ifdef D_ASSERT_IS_ENABLED
//! The serializer must produce the same text as the Value conversion
static void VerifyVarcharSerialization(FromVariantConversionData &conversion_data, const LogicalType &target_type,
                                       idx_t row_index, uint32_t values_index, const string &serialized) {
	auto value = VariantUtils::ConvertVariantToValue(conversion_data.variant, row_index, values_index);
	auto expected = conversion_data.TryCastAs(value, target_type);
	if (!expected || StringValue::Get(*expected) != serialized) {
		throw InternalException("VARIANT to VARCHAR serialization produced \"%s\", expected \"%s\"", serialized,
		                        expected ? StringValue::Get(*expected) : "a cast error");
	}
}
#endif

static bool HasVarcharCastOverride(FromVariantConversionData &conversion_data) {
	if (conversion_data.varchar_cast_override) {
		return *conversion_data.varchar_cast_override;
	}
	conversion_data.varchar_cast_override = false;
	if (!conversion_data.client) {
		return false;
	}
	auto &casts = CastFunctionSet::Get(*conversion_data.client);
	for (auto &type : casts.GetRegisteredCastSourceTypes(LogicalType::VARCHAR)) {
		// VARIANT does not preserve aliases, including JSON in nested types.
		if (TypeVisitor::Contains(type, [](const LogicalType &child) { return child.HasAlias(); })) {
			continue;
		}
		switch (type.id()) {
		case LogicalTypeId::ANY:
		case LogicalTypeId::SQLNULL:
		case LogicalTypeId::BOOLEAN:
		case LogicalTypeId::TINYINT:
		case LogicalTypeId::SMALLINT:
		case LogicalTypeId::INTEGER:
		case LogicalTypeId::BIGINT:
		case LogicalTypeId::HUGEINT:
		case LogicalTypeId::UTINYINT:
		case LogicalTypeId::USMALLINT:
		case LogicalTypeId::UINTEGER:
		case LogicalTypeId::UBIGINT:
		case LogicalTypeId::UHUGEINT:
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE:
		case LogicalTypeId::DATE:
		case LogicalTypeId::TIME:
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::INTERVAL:
		case LogicalTypeId::UUID:
		case LogicalTypeId::DECIMAL:
		case LogicalTypeId::STRUCT:
		case LogicalTypeId::LIST:
		case LogicalTypeId::VARIANT:
			conversion_data.varchar_cast_override = true;
			return true;
		default:
			// The serializer already falls back for the remaining types.
			break;
		}
	}
	return false;
}

static bool CastVariantToVarchar(FromVariantConversionData &conversion_data, Vector &result, const SelectionVector &sel,
                                 idx_t offset, idx_t count, optional_idx row) {
	if (result.GetType().HasAlias() || HasVarcharCastOverride(conversion_data)) {
		StringConversionPayload payload(result);
		return CastVariantToPrimitive<VariantDirectConversion<string_t, VariantLogicalType::VARCHAR>>(
		    conversion_data, result, sel, offset, count, row, payload);
	}
	auto &variant = conversion_data.variant;
	auto result_data = FlatVector::GetDataMutable<string_t>(result);
	auto &result_validity = FlatVector::ValidityMutable(result);

	unique_ptr<VariantVarcharSerializer> serializer;
	auto convert = [&](idx_t row_index, uint32_t values_index, idx_t result_index) {
		auto type_id = variant.GetTypeId(row_index, values_index);
		if (type_id == VariantLogicalType::VARIANT_NULL) {
			result_validity.SetInvalid(result_index);
			return true;
		}
		if (type_id == VariantLogicalType::VARCHAR) {
			result_data[result_index] =
			    StringVector::AddStringOrBlob(result, VariantUtils::DecodeStringData(variant, row_index, values_index));
			return true;
		}
		if (!serializer) {
			serializer = make_uniq<VariantVarcharSerializer>(variant);
		}
		if (!serializer->Serialize(row_index, values_index)) {
			return false;
		}
		auto &serialized = serializer->GetResult();
		// Let the string heap validate the full size before narrowing it to string_t's length.
		result_data[result_index] = StringVector::GetStringHeap(result).AddBlob(serialized.data(), serialized.size());
#ifdef D_ASSERT_IS_ENABLED
		VerifyVarcharSerialization(conversion_data, result.GetType(), row_index, values_index, serialized);
#endif
		return true;
	};
	return CastVariantRows(conversion_data, result, sel, offset, count, row, convert);
}

static bool FindValues(UnifiedVariantVectorData &variant, idx_t row_index, SelectionVector &sel,
                       const VariantNestedData &nested_data_entry) {
	for (idx_t child_idx = 0; child_idx < nested_data_entry.child_count; child_idx++) {
		auto value_id = variant.GetValuesIndex(row_index, nested_data_entry.children_idx + child_idx);
		sel[child_idx] = value_id;
	}
	return true;
}

static bool CastVariant(FromVariantConversionData &conversion_data, Vector &result, const SelectionVector &sel,
                        idx_t offset, idx_t count, optional_idx row);

static bool ConvertVariantToList(FromVariantConversionData &conversion_data, Vector &result, const SelectionVector &sel,
                                 idx_t offset, idx_t count, optional_idx row) {
	const auto owned_child_data = make_unsafe_uniq_array_uninitialized<VariantNestedData>(count);
	const array_ptr child_data(owned_child_data.get(), count);

	//! Initialize the validity with that of the result (in case some rows are already set to invalid, we need to
	//! respect that)
	auto &result_validity = FlatVector::ValidityMutable(result);
	ValidityMask validity(count);
	for (idx_t i = 0; i < count; i++) {
		if (!result_validity.RowIsValid(offset + i)) {
			validity.SetInvalid(i);
		}
	}

	const auto collection_result = VariantUtils::CollectNestedData(conversion_data.variant, VariantLogicalType::ARRAY,
	                                                               sel, count, row, offset, child_data, validity);
	if (!collection_result.success) {
		conversion_data.error =
		    StringUtil::Format("Expected to find VARIANT(ARRAY), found VARIANT(%s) instead, can't convert",
		                       EnumUtil::ToString(collection_result.wrong_type));
		return false;
	}
	idx_t total_children = 0;
	idx_t max_children = 0;
	for (idx_t i = 0; i < count; i++) {
		auto &child_data_entry = child_data[i];
		if (!validity.RowIsValid(i)) {
			continue;
		}
		if (child_data_entry.child_count > max_children) {
			max_children = child_data_entry.child_count;
		}
		total_children += child_data_entry.child_count;
	}

	SelectionVector new_sel;
	new_sel.Initialize(max_children);
	idx_t total_offset = 0;
	if (offset) {
		total_offset += ListVector::GetListSize(result);
	}

	ListVector::Reserve(result, total_offset + total_children);
	auto &child = ListVector::GetChildMutable(result);
	auto result_data = FlatVector::Writer<list_entry_t>(result, count, offset);
	for (idx_t i = 0; i < count; i++) {
		auto row_index = row.IsValid() ? row.GetIndex() : i;
		auto &child_data_entry = child_data[i];

		if (!validity.RowIsValid(i)) {
			result_data.WriteNull();
			continue;
		}

		list_entry_t entry;
		entry.offset = total_offset;
		entry.length = child_data_entry.child_count;
		result_data.WriteValue(entry);
		total_offset += entry.length;

		FindValues(conversion_data.variant, row_index, new_sel, child_data_entry);
		if (!CastVariant(conversion_data, child, new_sel, entry.offset, child_data_entry.child_count, row_index)) {
			// a TRY_CAST reports the failure by returning false rather than throwing, so the writer has to be
			// told it is deliberately short of count before it goes out of scope
			result_data.Truncate();
			return false;
		}
	}
	ListVector::SetListSize(result, total_offset);
	return true;
}

static bool ConvertVariantToArray(FromVariantConversionData &conversion_data, Vector &result,
                                  const SelectionVector &sel, idx_t offset, idx_t count, optional_idx row) {
	const auto owned_child_data = make_unsafe_uniq_array_uninitialized<VariantNestedData>(count);
	const array_ptr child_data(owned_child_data.get(), count);

	//! Initialize the validity with that of the result (in case some rows are already set to invalid, we need to
	//! respect that)
	auto &result_validity = FlatVector::ValidityMutable(result);
	ValidityMask validity(count);
	for (idx_t i = 0; i < count; i++) {
		if (!result_validity.RowIsValid(offset + i)) {
			validity.SetInvalid(i);
		}
	}

	auto collection_result = VariantUtils::CollectNestedData(conversion_data.variant, VariantLogicalType::ARRAY, sel,
	                                                         count, row, offset, child_data, validity);
	if (!collection_result.success) {
		conversion_data.error =
		    StringUtil::Format("Expected to find VARIANT(ARRAY), found VARIANT(%s) instead, can't convert",
		                       EnumUtil::ToString(collection_result.wrong_type));
		return false;
	}

	const auto array_size = ArrayType::GetSize(result.GetType());
	for (idx_t i = 0; i < count; i++) {
		auto &child_data_entry = child_data[i];
		if (!validity.RowIsValid(i)) {
			continue;
		}
		if (child_data_entry.child_count != array_size) {
			conversion_data.error =
			    StringUtil::Format("Array size '%d' was expected, found '%d', can't convert VARIANT", array_size,
			                       child_data_entry.child_count);
			return false;
		}
	}

	SelectionVector new_sel;
	new_sel.Initialize(array_size);

	auto &child = ArrayVector::GetChildMutable(result);
	idx_t total_offset = offset * array_size;
	for (idx_t i = 0; i < count; i++) {
		auto row_index = row.IsValid() ? row.GetIndex() : i;
		auto &child_data_entry = child_data[i];

		if (!validity.RowIsValid(i)) {
			FlatVector::SetNull(result, offset + i, true);
			total_offset += array_size;
			continue;
		}

		FindValues(conversion_data.variant, row_index, new_sel, child_data_entry);
		if (!CastVariant(conversion_data, child, new_sel, total_offset, array_size, row_index)) {
			return false;
		}
		total_offset += array_size;
	}
	return true;
}

static bool ConvertVariantToStruct(FromVariantConversionData &conversion_data, Vector &result,
                                   const SelectionVector &sel, idx_t offset, idx_t count, optional_idx row) {
	auto &target_type = result.GetType();
	const auto owned_child_data = make_unsafe_uniq_array_uninitialized<VariantNestedData>(count);
	array_ptr child_data(owned_child_data.get(), count);

	//! Initialize the validity with that of the result (in case some rows are already set to invalid, we need to
	//! respect that)
	auto &result_validity = FlatVector::ValidityMutable(result);
	ValidityMask validity(count);
	for (idx_t i = 0; i < count; i++) {
		if (!result_validity.RowIsValid(offset + i)) {
			validity.SetInvalid(i);
		}
	}

	auto collection_result = VariantUtils::CollectNestedData(conversion_data.variant, VariantLogicalType::OBJECT, sel,
	                                                         count, row, offset, child_data, validity);
	if (!collection_result.success) {
		conversion_data.error =
		    StringUtil::Format("Expected to find VARIANT(OBJECT), found VARIANT(%s) instead, can't convert",
		                       EnumUtil::ToString(collection_result.wrong_type));
		return false;
	}

	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			FlatVector::SetNull(result, offset + i, true);
		}
	}

	auto &children = StructVector::GetEntries(result);
	auto &child_types = StructType::GetChildTypes(target_type);

	SelectionVector child_values_sel;
	child_values_sel.Initialize(count);

	auto row_sel = SelectionVector::Incremental(0ULL, count);
	if (row.IsValid()) {
		auto row_index = row.GetIndex();
		for (idx_t i = 0; i < count; i++) {
			row_sel[i] = static_cast<uint32_t>(row_index);
		}
	}

	for (idx_t child_idx = 0; child_idx < child_types.size(); child_idx++) {
		auto &child_name = child_types[child_idx].first;

		//! Then find the relevant child of the OBJECTs we're converting
		//! FIXME: there is nothing preventing an OBJECT from containing the same key twice I believe ?
		VariantPathComponent component;
		component.key = child_name.GetIdentifierName();
		component.lookup_mode = VariantChildLookupMode::BY_KEY;
		ValidityMask lookup_validity(count);
		VariantUtils::FindChildValues(conversion_data.variant, component, row_sel, child_values_sel, lookup_validity,
		                              child_data, validity, count);
		if (lookup_validity.CanHaveNull()) {
			optional_idx nested_index;
			for (idx_t i = 0; i < count; i++) {
				if (!lookup_validity.RowIsValid(i)) {
					nested_index = i;
					break;
				}
			}
			D_ASSERT(nested_index.IsValid());
			auto row_index = row.IsValid() ? row.GetIndex() : nested_index.GetIndex();
			auto object_keys =
			    VariantUtils::GetObjectKeys(conversion_data.variant, row_index, child_data[nested_index.GetIndex()]);
			std::sort(object_keys.begin(), object_keys.end());
			conversion_data.error = StringUtil::Format("VARIANT(OBJECT(%s)) is missing key '%s'",
			                                           StringUtil::Join(object_keys, ","), component.key);
			return false;
		}
		//! Now cast all the values we found to the target type
		auto &child = children[child_idx];
		if (!CastVariant(conversion_data, child, child_values_sel, offset, count, row)) {
			return false;
		}
	}
	return true;
}

//! A TUPLE is read from a variant ARRAY - each element is read positionally into the matching tuple child
static bool ConvertVariantToTuple(FromVariantConversionData &conversion_data, Vector &result,
                                  const SelectionVector &sel, idx_t offset, idx_t count, optional_idx row) {
	auto &target_type = result.GetType();
	const auto owned_child_data = make_unsafe_uniq_array_uninitialized<VariantNestedData>(count);
	array_ptr child_data(owned_child_data.get(), count);

	auto &result_validity = FlatVector::ValidityMutable(result);
	ValidityMask validity(count);
	for (idx_t i = 0; i < count; i++) {
		if (!result_validity.RowIsValid(offset + i)) {
			validity.SetInvalid(i);
		}
	}

	auto collection_result = VariantUtils::CollectNestedData(conversion_data.variant, VariantLogicalType::ARRAY, sel,
	                                                         count, row, offset, child_data, validity);
	if (!collection_result.success) {
		conversion_data.error =
		    StringUtil::Format("Expected to find VARIANT(ARRAY), found VARIANT(%s) instead, can't convert",
		                       EnumUtil::ToString(collection_result.wrong_type));
		return false;
	}

	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			FlatVector::SetNull(result, offset + i, true);
		}
	}

	auto &children = StructVector::GetEntries(result);
	auto &child_types = StructType::GetChildTypes(target_type);

	SelectionVector child_values_sel;
	child_values_sel.Initialize(count);

	auto row_sel = SelectionVector::Incremental(0ULL, count);
	if (row.IsValid()) {
		auto row_index = row.GetIndex();
		for (idx_t i = 0; i < count; i++) {
			row_sel[i] = static_cast<uint32_t>(row_index);
		}
	}

	for (idx_t child_idx = 0; child_idx < child_types.size(); child_idx++) {
		//! Find the relevant element of the ARRAYs we're converting by position
		VariantPathComponent component(NumericCast<uint32_t>(child_idx));
		ValidityMask lookup_validity(count);
		VariantUtils::FindChildValues(conversion_data.variant, component, row_sel, child_values_sel, lookup_validity,
		                              child_data, validity, count);
		if (lookup_validity.CanHaveNull()) {
			conversion_data.error =
			    StringUtil::Format("VARIANT(ARRAY) is missing element at index %d, can't convert to TUPLE", child_idx);
			return false;
		}
		//! Now cast all the values we found to the target type
		auto &child = children[child_idx];
		if (!CastVariant(conversion_data, child, child_values_sel, offset, count, row)) {
			return false;
		}
	}
	return true;
}

static bool CastVariantToJSON(FromVariantConversionData &conversion_data, Vector &result, const SelectionVector &sel,
                              idx_t offset, idx_t count, optional_idx row) {
	auto &error = conversion_data.error;

	ConvertedJSONHolder holder(Allocator::DefaultAllocator());

	auto &variant = conversion_data.variant;
	auto result_data = FlatVector::Writer<string_t>(result, count, offset);
	for (idx_t i = 0; i < count; i++) {
		const auto row_index = row.IsValid() ? row.GetIndex() : i;
		if (!variant.RowIsValid(row_index)) {
			// a SQL NULL row stays a SQL NULL - rendering it as the JSON token `null` would make it
			// indistinguishable from a JSON null actually stored in the variant
			result_data.WriteNull();
			continue;
		}
		const auto json_val =
		    VariantCasts::ConvertVariantToJSON(holder.GetDocument(), conversion_data.variant, row_index, sel[i]);
		if (!json_val) {
			error = StringUtil::Format("Failed to convert to JSON object");
			// same as the list path: a TRY_CAST returns false instead of throwing, so the writer must be
			// told it is deliberately short of count
			result_data.Truncate();
			return false;
		}

		const auto serialized = holder.Serialize(json_val, error);
		if (!serialized) {
			result_data.Truncate();
			return false;
		}

		result_data.WriteValue(*serialized);
		holder.Reset();
	}

	return true;
}

//! * @param conversion_data The constant data relevant at all rows of the conversion
//! * @param result The typed Vector to populate in this call
//! * @param sel The selection of value indices to cast
//! * @param offset The offset into the result where to write the converted values
//! * @param count The amount of values we're converting
//! * @param row The row of the Variant to pull data from, if 'IsValid()' is true
//! Maps cannot have NULL or duplicate keys - such rows are set to NULL, reporting the error
static bool VerifyConvertedMaps(FromVariantConversionData &conversion_data, Vector &result, idx_t offset, idx_t count) {
	bool all_valid = true;
	SelectionVector row_sel(1);
	for (idx_t i = 0; i < count; i++) {
		if (FlatVector::IsNull(result, offset + i)) {
			continue;
		}
		row_sel.set_index(0, offset + i);
		auto reason = MapVector::CheckMapValidity(result, 1, row_sel);
		if (reason == MapInvalidReason::VALID) {
			continue;
		}
		if (conversion_data.error.empty()) {
			conversion_data.error =
			    reason == MapInvalidReason::DUPLICATE_KEY ? "Map keys must be unique." : "Map keys can not be NULL.";
		}
		FlatVector::SetNull(result, offset + i, true);
		all_valid = false;
	}
	return all_valid;
}

//! A VARIANT holds a MAP as a list of {key, value} structs - cast it to the entries of the target MAP
static optional<Value> TryCastVariantValueToMap(FromVariantConversionData &conversion_data, const Value &value,
                                                const LogicalType &target_type) {
	auto entry_type = ListType::GetChildType(target_type);
	auto entries = conversion_data.TryCastAs(value, LogicalType::LIST(entry_type));
	if (!entries) {
		return entries;
	}
	if (entries->IsNull()) {
		return Value(target_type);
	}
	auto &children = ListValue::GetChildren(*entries);
	for (auto &child : children) {
		if (child.IsNull()) {
			// a NULL entry has no key
			if (conversion_data.error.empty()) {
				conversion_data.error = "Map keys can not be NULL.";
			}
			return optional<Value>();
		}
	}
	try {
		return Value::MAP(entry_type, children);
	} catch (const InvalidInputException &ex) {
		// the entries do not form a valid map (e.g. duplicate keys)
		if (conversion_data.error.empty()) {
			conversion_data.error = ErrorData(ex).RawMessage();
		}
		return optional<Value>();
	}
}

static bool CastVariant(FromVariantConversionData &conversion_data, Vector &result, const SelectionVector &sel,
                        idx_t offset, idx_t count, optional_idx row) {
	auto &target_type = result.GetType();
	auto &error = conversion_data.error;

	if (target_type.IsNested()) {
		switch (target_type.id()) {
		case LogicalTypeId::STRUCT: {
			if (ConvertVariantToStruct(conversion_data, result, sel, offset, count, row)) {
				return true;
			}
			break;
		}
		case LogicalTypeId::TUPLE: {
			if (ConvertVariantToTuple(conversion_data, result, sel, offset, count, row)) {
				return true;
			}
			break;
		}
		case LogicalTypeId::ARRAY:
			if (ConvertVariantToArray(conversion_data, result, sel, offset, count, row)) {
				return true;
			}
			break;
		case LogicalTypeId::LIST:
			if (ConvertVariantToList(conversion_data, result, sel, offset, count, row)) {
				return true;
			}
			break;
		case LogicalTypeId::MAP: {
			if (ConvertVariantToList(conversion_data, result, sel, offset, count, row)) {
				return VerifyConvertedMaps(conversion_data, result, offset, count);
			}
			break;
		}
		case LogicalTypeId::UNION: {
			error = "Can't convert VARIANT";
			for (idx_t i = 0; i < count; i++) {
				FlatVector::SetNull(result, offset + i, true);
			}
			return false;
		}
		default: {
			error = StringUtil::Format("Nested type: '%s' not handled, can't convert VARIANT", target_type.ToString());
			for (idx_t i = 0; i < count; i++) {
				FlatVector::SetNull(result, offset + i, true);
			}
			return false;
		}
		};

		// Fallback: try to convert to Value and then cast
		// This may throw bind-time exceptions for incompatible type combinations
		// In those cases, we should fail gracefully
		bool all_valid = true;
		for (idx_t i = 0; i < count; i++) {
			auto row_index = row.IsValid() ? row.GetIndex() : i;

			//! Get the index into 'values'
			uint32_t value_index = sel[i];
			auto value = VariantUtils::ConvertVariantToValue(conversion_data.variant, row_index, value_index);
			try {
				auto cast_value = target_type.id() == LogicalTypeId::MAP
				                      ? TryCastVariantValueToMap(conversion_data, value, target_type)
				                      : conversion_data.TryCastAs(value, target_type);
				if (!cast_value) {
					cast_value = Value(target_type);
					all_valid = false;
				}
				result.SetValue(i + offset, *cast_value);
			} catch (const BinderException &) {
				// Bind-time exceptions (e.g., incompatible struct layouts) should be treated as conversion failures
				// Set the value to NULL and mark as failed
				FlatVector::SetNull(result, offset + i, true);
				all_valid = false;
			} catch (const ConversionException &ex) {
				// strict casts of the converted value (e.g. VARCHAR to an ARRAY of a different size) can throw
				if (error.empty()) {
					error = ErrorData(ex).RawMessage();
				}
				FlatVector::SetNull(result, offset + i, true);
				all_valid = false;
			}
		}
		if (target_type.id() == LogicalTypeId::MAP) {
			all_valid = VerifyConvertedMaps(conversion_data, result, offset, count) && all_valid;
		}
		return all_valid;
	} else {
		EmptyConversionPayloadFromVariant empty_payload;
		switch (target_type.id()) {
		case LogicalTypeId::BOOLEAN:
			return CastVariantToPrimitive<VariantBooleanConversion>(conversion_data, result, sel, offset, count, row,
			                                                        empty_payload);
		case LogicalTypeId::TINYINT:
			return CastVariantToPrimitive<VariantDirectConversion<int8_t, VariantLogicalType::INT8>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::SMALLINT:
			return CastVariantToPrimitive<VariantDirectConversion<int16_t, VariantLogicalType::INT16>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::INTEGER:
			return CastVariantToPrimitive<VariantDirectConversion<int32_t, VariantLogicalType::INT32>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::BIGINT:
			return CastVariantToPrimitive<VariantDirectConversion<int64_t, VariantLogicalType::INT64>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::HUGEINT:
			return CastVariantToPrimitive<VariantDirectConversion<hugeint_t, VariantLogicalType::INT128>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::UTINYINT:
			return CastVariantToPrimitive<VariantDirectConversion<uint8_t, VariantLogicalType::UINT8>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::USMALLINT:
			return CastVariantToPrimitive<VariantDirectConversion<uint16_t, VariantLogicalType::UINT16>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::UINTEGER:
			return CastVariantToPrimitive<VariantDirectConversion<uint32_t, VariantLogicalType::UINT32>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::UBIGINT:
			return CastVariantToPrimitive<VariantDirectConversion<uint64_t, VariantLogicalType::UINT64>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::UHUGEINT:
			return CastVariantToPrimitive<VariantDirectConversion<uhugeint_t, VariantLogicalType::UINT128>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::FLOAT:
			return CastVariantToPrimitive<VariantDirectConversion<float, VariantLogicalType::FLOAT>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::DOUBLE:
			return CastVariantToPrimitive<VariantDirectConversion<double, VariantLogicalType::DOUBLE>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::DATE:
			return CastVariantToPrimitive<VariantDirectConversion<date_t, VariantLogicalType::DATE>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::TIMESTAMP:
			return CastVariantToPrimitive<VariantDirectConversion<timestamp_t, VariantLogicalType::TIMESTAMP_MICROS>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::TIMESTAMP_MS:
			return CastVariantToPrimitive<VariantDirectConversion<timestamp_ms_t, VariantLogicalType::TIMESTAMP_MILIS>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::TIMESTAMP_SEC:
			return CastVariantToPrimitive<VariantDirectConversion<timestamp_sec_t, VariantLogicalType::TIMESTAMP_SEC>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::TIMESTAMP_NS:
			return CastVariantToPrimitive<VariantDirectConversion<timestamp_ns_t, VariantLogicalType::TIMESTAMP_NANOS>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::BLOB: {
			StringConversionPayload string_payload(result);
			return CastVariantToPrimitive<VariantDirectConversion<string_t, VariantLogicalType::BLOB>>(
			    conversion_data, result, sel, offset, count, row, string_payload);
		}
		case LogicalTypeId::GEOMETRY: {
			StringConversionPayload string_payload(result);
			return CastVariantToPrimitive<VariantDirectConversion<string_t, VariantLogicalType::GEOMETRY>>(
			    conversion_data, result, sel, offset, count, row, string_payload);
		}
		case LogicalTypeId::VARCHAR: {
			if (target_type.IsJSONType()) {
				return CastVariantToJSON(conversion_data, result, sel, offset, count, row);
			}
			return CastVariantToVarchar(conversion_data, result, sel, offset, count, row);
		}
		case LogicalTypeId::INTERVAL:
			return CastVariantToPrimitive<VariantDirectConversion<interval_t, VariantLogicalType::INTERVAL>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::DECIMAL: {
			auto physical_type = target_type.InternalType();
			uint32_t width;
			uint32_t scale;
			target_type.GetDecimalProperties(width, scale);
			DecimalConversionPayloadFromVariant decimal_payload(width, scale);

			switch (physical_type) {
			case PhysicalType::INT16:
				return CastVariantToPrimitive<VariantDecimalConversion<int16_t>>(conversion_data, result, sel, offset,
				                                                                 count, row, decimal_payload);
			case PhysicalType::INT32:
				return CastVariantToPrimitive<VariantDecimalConversion<int32_t>>(conversion_data, result, sel, offset,
				                                                                 count, row, decimal_payload);
			case PhysicalType::INT64:
				return CastVariantToPrimitive<VariantDecimalConversion<int64_t>>(conversion_data, result, sel, offset,
				                                                                 count, row, decimal_payload);
			case PhysicalType::INT128:
				return CastVariantToPrimitive<VariantDecimalConversion<hugeint_t>>(conversion_data, result, sel, offset,
				                                                                   count, row, decimal_payload);
			default:
				throw NotImplementedException("Can't convert VARIANT to DECIMAL value of physical type: %s",
				                              EnumUtil::ToString(physical_type));
			};
		}
		case LogicalTypeId::TIME:
			return CastVariantToPrimitive<VariantDirectConversion<dtime_t, VariantLogicalType::TIME_MICROS>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::TIME_NS:
			return CastVariantToPrimitive<VariantDirectConversion<dtime_ns_t, VariantLogicalType::TIME_NANOS>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::TIME_TZ:
			return CastVariantToPrimitive<VariantDirectConversion<dtime_tz_t, VariantLogicalType::TIME_MICROS_TZ>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::TIMESTAMP_TZ:
			return CastVariantToPrimitive<
			    VariantDirectConversion<timestamp_tz_t, VariantLogicalType::TIMESTAMP_MICROS_TZ>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::TIMESTAMP_TZ_NS:
			return CastVariantToPrimitive<
			    VariantDirectConversion<timestamp_tz_ns_t, VariantLogicalType::TIMESTAMP_NANOS_TZ>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::UUID:
			return CastVariantToPrimitive<VariantDirectConversion<hugeint_t, VariantLogicalType::UUID>>(
			    conversion_data, result, sel, offset, count, row, empty_payload);
		case LogicalTypeId::BIT: {
			StringConversionPayload string_payload(result);
			return CastVariantToPrimitive<VariantDirectConversion<string_t, VariantLogicalType::BITSTRING>>(
			    conversion_data, result, sel, offset, count, row, string_payload);
		}
		case LogicalTypeId::BIGNUM: {
			StringConversionPayload string_payload(result);
			return CastVariantToPrimitive<VariantDirectConversion<string_t, VariantLogicalType::BIGNUM>>(
			    conversion_data, result, sel, offset, count, row, string_payload);
		}
		default:
			error = "Can't convert VARIANT";
			for (idx_t i = 0; i < count; i++) {
				FlatVector::SetNull(result, offset + i, true);
			}
			return false;
		}
	}
}

static bool TryFromShreddedCast(Vector &variant_vec, Vector &result) {
	if (variant_vec.GetVectorType() != VectorType::SHREDDED_VECTOR) {
		// input vector is not shredded
		return false;
	}
	// check if we are fully shredded on this type
	if (result.GetType().IsNested()) {
		// shredded casts for nested types not yet supported
		return false;
	}
	auto &shredded_vec = ShreddedVector::GetShreddedVector(variant_vec);
	if (shredded_vec.GetType() == result.GetType()) {
		// direct type match: variant vector is fully shredded on this primitive type
		result.Reference(shredded_vec);
		return true;
	}
	// check if this is a {typed_value ..., untyped_value ...} struct with an empty (constant NULL) untyped_value
	if (ShreddedVector::IsFullyShredded(variant_vec) && shredded_vec.GetType().id() == LogicalTypeId::STRUCT) {
		// it is! check if the type of the typed_value entry matches
		auto &shredded_entries = StructVector::GetEntries(shredded_vec);
		auto &typed_value = shredded_entries[VariantStats::TYPED_VALUE_INDEX];
		if (typed_value.GetType() == result.GetType()) {
			// the typed_value matches - directly reference it
			result.Reference(typed_value);
			return true;
		}
	}
	return false;
}

static bool CastFromVARIANT(Vector &variant_vec, Vector &result, idx_t count, CastParameters &parameters) {
	if (TryFromShreddedCast(variant_vec, result)) {
		return true;
	}
	// fallback to conversion
	D_ASSERT(variant_vec.GetType().id() == LogicalTypeId::VARIANT);
	RecursiveUnifiedVectorFormat variant_format;
	Vector::RecursiveToUnifiedFormat(variant_vec, variant_format);
	optional_ptr<ClientContext> client;
	if (parameters.cast_data) {
		client = parameters.cast_data->Cast<ToVariantCastData>().client;
	}
	FromVariantConversionData conversion_data(variant_format, client);

	reference<const SelectionVector> sel(*ConstantVector::ZeroSelectionVector());
	SelectionVector zero_sel;
	if (count >= STANDARD_VECTOR_SIZE) {
		zero_sel.Initialize(count);
		for (idx_t i = 0; i < count; i++) {
			zero_sel[i] = 0;
		}
		sel = zero_sel;
	}

	auto success = CastVariant(conversion_data, result, sel, 0, count, optional_idx());
	if (variant_vec.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
	if (!success) {
		return FinalizeErrorMessage(conversion_data, result, parameters);
	}
	return true;
}

BoundCastInfo DefaultCasts::VariantCastSwitch(BindCastInput &input, const LogicalType &source,
                                              const LogicalType &target) {
	auto cast_data = make_uniq<ToVariantCastData>(input.context);
	D_ASSERT(source.id() == LogicalTypeId::VARIANT);
	switch (target.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::BIT:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::TIME:
	case LogicalTypeId::LIST:
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::MAP:
	case LogicalTypeId::UNION:
	case LogicalTypeId::UUID:
	case LogicalTypeId::ARRAY:
		return BoundCastInfo(CastFromVARIANT, std::move(cast_data));
	case LogicalTypeId::GEOMETRY:
		return BoundCastInfo(CastFromVARIANT, std::move(cast_data));
	case LogicalTypeId::VARCHAR: {
		return BoundCastInfo(CastFromVARIANT, std::move(cast_data));
	}
	default:
		return TryVectorNullCast;
	}
}

} // namespace duckdb
