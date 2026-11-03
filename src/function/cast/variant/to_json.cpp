#include "yyjson.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/common/types/variant_visitor.hpp"

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

//! ------------ Variant -> JSON ------------

namespace {

struct JSONConverterBase {
	using result_type = yyjson_mut_val *;

	static yyjson_mut_val *VisitNull(yyjson_mut_doc *doc) {
		return yyjson_mut_null(doc);
	}

	static yyjson_mut_val *VisitBoolean(bool val, yyjson_mut_doc *doc) {
		return val ? yyjson_mut_true(doc) : yyjson_mut_false(doc);
	}

	template <typename T>
	static yyjson_mut_val *VisitInteger(T val, yyjson_mut_doc *doc) {
		if constexpr (std::is_same_v<T, int8_t> || std::is_same_v<T, int16_t> || std::is_same_v<T, int32_t> ||
		              std::is_same_v<T, int64_t> || std::is_same_v<T, uint8_t> || std::is_same_v<T, uint16_t> ||
		              std::is_same_v<T, uint32_t>) {
			return yyjson_mut_sint(doc, val);
		} else if constexpr (std::is_same_v<T, uint64_t>) {
			return yyjson_mut_uint(doc, val);
		} else {
			auto val_str = val.ToString();
			return yyjson_mut_rawncpy(doc, val_str.c_str(), val_str.size());
		}
	}

	static yyjson_mut_val *VisitTime(dtime_t val, yyjson_mut_doc *doc) {
		auto val_str = Time::ToString(val);
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitTimeNanos(dtime_ns_t val, yyjson_mut_doc *doc) {
		auto val_str = Value::TIME_NS(val).ToString();
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitTimeTZ(dtime_tz_t val, yyjson_mut_doc *doc) {
		auto val_str = Value::TIMETZ(val).ToString();
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitTimestampSec(timestamp_sec_t val, yyjson_mut_doc *doc) {
		auto val_str = Value::TIMESTAMPSEC(val).ToString();
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitTimestampMs(timestamp_ms_t val, yyjson_mut_doc *doc) {
		auto val_str = Value::TIMESTAMPMS(val).ToString();
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitTimestamp(timestamp_t val, yyjson_mut_doc *doc) {
		auto val_str = Timestamp::ToString(val);
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitTimestampNanos(timestamp_ns_t val, yyjson_mut_doc *doc) {
		auto val_str = Value::TIMESTAMPNS(val).ToString();
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitTimestampTZ(timestamp_tz_t val, yyjson_mut_doc *doc) {
		auto val_str = Value::TIMESTAMPTZ(val).ToString();
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitTimestampTZNanos(timestamp_tz_ns_t val, yyjson_mut_doc *doc) {
		auto val_str = Value::TIMESTAMPTZNS(val).ToString();
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitFloat(float val, yyjson_mut_doc *doc) {
		return yyjson_mut_real(doc, val);
	}

	static yyjson_mut_val *VisitDouble(double val, yyjson_mut_doc *doc) {
		return yyjson_mut_real(doc, val);
	}

	static yyjson_mut_val *VisitUUID(hugeint_t val, yyjson_mut_doc *doc) {
		auto val_str = Value::UUID(val).ToString();
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitDate(date_t val, yyjson_mut_doc *doc) {
		auto val_str = Date::ToString(val);
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitInterval(interval_t val, yyjson_mut_doc *doc) {
		auto val_str = Value::INTERVAL(val).ToString();
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitString(const string_t &str, yyjson_mut_doc *doc) {
		return yyjson_mut_strncpy(doc, str.GetData(), str.GetSize());
	}

	static yyjson_mut_val *VisitBlob(const string_t &str, yyjson_mut_doc *doc) {
		auto val_str = Value::BLOB(const_data_ptr_cast(str.GetData()), str.GetSize()).ToString();
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitBignum(const string_t &str, yyjson_mut_doc *doc) {
		auto val_str = Value::BIGNUM(const_data_ptr_cast(str.GetData()), str.GetSize()).ToString();
		return yyjson_mut_rawncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitGeometry(const string_t &str, yyjson_mut_doc *doc) {
		auto val_str = Value::GEOMETRY(const_data_ptr_cast(str.GetData()), str.GetSize()).ToString();
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitBitstring(const string_t &str, yyjson_mut_doc *doc) {
		auto val_str = Value::BIT(const_data_ptr_cast(str.GetData()), str.GetSize()).ToString();
		return yyjson_mut_strncpy(doc, val_str.c_str(), val_str.size());
	}

	template <typename T>
	static yyjson_mut_val *VisitDecimal(T val, uint32_t width, uint32_t scale, yyjson_mut_doc *doc) {
		string val_str;
		if constexpr (std::is_same_v<T, int16_t>) {
			val_str = Decimal::ToString(val, static_cast<uint8_t>(width), static_cast<uint8_t>(scale));
		} else if constexpr (std::is_same_v<T, int32_t>) {
			val_str = Decimal::ToString(val, static_cast<uint8_t>(width), static_cast<uint8_t>(scale));
		} else if constexpr (std::is_same_v<T, int64_t>) {
			val_str = Decimal::ToString(val, static_cast<uint8_t>(width), static_cast<uint8_t>(scale));
		} else if constexpr (std::is_same_v<T, hugeint_t>) {
			val_str = Decimal::ToString(val, static_cast<uint8_t>(width), static_cast<uint8_t>(scale));
		} else {
			throw InternalException("Unhandled decimal type");
		}
		if (width == scale) {
			// Decimal::ToString omits the zero before the decimal point when all digits are fractional.
			// JSON numbers require an integer component.
			val_str.insert(val_str[0] == '-' ? 1 : 0, 1, '0');
		}
		return yyjson_mut_rawncpy(doc, val_str.c_str(), val_str.size());
	}

	static yyjson_mut_val *VisitDefault(VariantLogicalType type_id, const_data_ptr_t, yyjson_mut_doc *) {
		throw InternalException("VariantLogicalType(%s) not handled", EnumUtil::ToString(type_id));
	}
};

struct JSONConverter : JSONConverterBase {
	static yyjson_mut_val *VisitArray(const UnifiedVariantVectorData &variant, idx_t row,
	                                  const VariantNestedData &nested_data, idx_t depth, yyjson_mut_doc *doc) {
		auto arr = yyjson_mut_arr(doc);
		auto array_items = VariantVisitor<JSONConverter>::VisitArrayItems(variant, row, nested_data, depth, doc);
		for (auto &entry : array_items) {
			yyjson_mut_arr_add_val(arr, entry);
		}
		return arr;
	}

	static yyjson_mut_val *VisitObject(const UnifiedVariantVectorData &variant, idx_t row,
	                                   const VariantNestedData &nested_data, idx_t depth, yyjson_mut_doc *doc) {
		auto obj = yyjson_mut_obj(doc);
		auto object_items = VariantVisitor<JSONConverter>::VisitObjectItems(variant, row, nested_data, depth, doc);
		for (auto &entry : object_items) {
			yyjson_mut_obj_put(obj, yyjson_mut_strncpy(doc, entry.first.c_str(), entry.first.size()), entry.second);
		}
		return obj;
	}
};

template <VariantIterationOrder OBJECT_ORDER>
struct NodeJSONConverter : JSONConverterBase {
	static yyjson_mut_val *VisitArray(const VariantNode &variant, idx_t depth, yyjson_mut_doc *doc) {
		const auto arr = yyjson_mut_arr(doc);
		const auto array_items = VariantVisitor<NodeJSONConverter<OBJECT_ORDER>>::VisitArrayItems(variant, depth, doc);
		for (const auto &entry : array_items) {
			yyjson_mut_arr_add_val(arr, entry);
		}
		return arr;
	}

	static yyjson_mut_val *VisitObject(const VariantNode &variant, idx_t depth, yyjson_mut_doc *doc) {
		const auto obj = yyjson_mut_obj(doc);
		for (const auto &[key, value] : variant.GetObjectChildren(OBJECT_ORDER)) {
			const auto key_value = yyjson_mut_strncpy(doc, key.GetData(), key.GetSize());
			const auto json_value = VariantVisitor<NodeJSONConverter<OBJECT_ORDER>>::Visit(value, depth + 1, doc);
			if (!yyjson_mut_obj_add(obj, key_value, json_value)) {
				throw InternalException("Failed to append VARIANT object child to yyjson value");
			}
		}
		return obj;
	}
};

} // namespace

yyjson_mut_val *VariantCasts::ConvertVariantToJSON(yyjson_mut_doc *doc, const VariantNode &variant,
                                                   bool sort_object_keys) {
	if (sort_object_keys) {
		return VariantVisitor<NodeJSONConverter<VariantIterationOrder::LEXICOGRAPHIC>>::Visit(variant, 0, doc);
	}
	return VariantVisitor<NodeJSONConverter<VariantIterationOrder::INTERNAL>>::Visit(variant, 0, doc);
}

yyjson_mut_val *VariantCasts::ConvertVariantToJSON(yyjson_mut_doc *doc, const UnifiedVariantVectorData &variant,
                                                   idx_t row, uint32_t values_idx) {
	return VariantVisitor<JSONConverter>::Visit(variant, row, values_idx, 0, doc);
}

} // namespace duckdb
