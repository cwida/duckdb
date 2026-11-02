#pragma once

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception.hpp"

#include <type_traits>

namespace duckdb {

template <typename Visitor, typename ReturnType = typename Visitor::result_type>
class VariantVisitor {
	// Detects if T has a static VisitMetadata with signature
	// void VisitMetadata(VariantLogicalType, Args...)
	template <typename T, typename... Args>
	class has_visit_metadata {
	private:
		template <typename U>
		static auto test(int) -> decltype(U::VisitMetadata(std::declval<VariantLogicalType>(), std::declval<Args>()...),
		                                  std::true_type {});

		template <typename>
		static std::false_type test(...);

	public:
		static constexpr bool value = decltype(test<T>(0))::value;
	};

public:
	//! Root visits start at depth zero; child visits increment the parent depth.
	template <typename... Args>
	static ReturnType Visit(const UnifiedVariantVectorData &variant, idx_t row, uint32_t values_idx, idx_t depth,
	                        Args &&...args) {
		CheckDepth(depth);
		if (!variant.RowIsValid(row)) {
			VisitMetadata(VariantLogicalType::VARIANT_NULL, args...);
			return Visitor::VisitNull(std::forward<Args>(args)...);
		}

		auto type_id = variant.GetTypeId(row, values_idx);
		auto byte_offset = variant.GetByteOffset(row, values_idx);
		auto blob_data = const_data_ptr_cast(variant.GetData(row).GetData());
		auto ptr = const_data_ptr_cast(blob_data + byte_offset);

		VisitMetadata(type_id, args...);

		switch (type_id) {
		case VariantLogicalType::VARIANT_NULL:
			return Visitor::VisitNull(std::forward<Args>(args)...);
		case VariantLogicalType::BOOL_TRUE:
			return Visitor::VisitBoolean(true, std::forward<Args>(args)...);
		case VariantLogicalType::BOOL_FALSE:
			return Visitor::VisitBoolean(false, std::forward<Args>(args)...);
		case VariantLogicalType::INT8:
			return Visitor::template VisitInteger<int8_t>(Load<int8_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::INT16:
			return Visitor::template VisitInteger<int16_t>(Load<int16_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::INT32:
			return Visitor::template VisitInteger<int32_t>(Load<int32_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::INT64:
			return Visitor::template VisitInteger<int64_t>(Load<int64_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::INT128:
			return Visitor::template VisitInteger<hugeint_t>(Load<hugeint_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::UINT8:
			return Visitor::template VisitInteger<uint8_t>(Load<uint8_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::UINT16:
			return Visitor::template VisitInteger<uint16_t>(Load<uint16_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::UINT32:
			return Visitor::template VisitInteger<uint32_t>(Load<uint32_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::UINT64:
			return Visitor::template VisitInteger<uint64_t>(Load<uint64_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::UINT128:
			return Visitor::template VisitInteger<uhugeint_t>(Load<uhugeint_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::FLOAT:
			return Visitor::VisitFloat(Load<float>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::DOUBLE:
			return Visitor::VisitDouble(Load<double>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::UUID:
			return Visitor::VisitUUID(Load<hugeint_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::DATE:
			return Visitor::VisitDate(date_t(Load<int32_t>(ptr)), std::forward<Args>(args)...);
		case VariantLogicalType::INTERVAL:
			return Visitor::VisitInterval(Load<interval_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::VARCHAR:
		case VariantLogicalType::BLOB:
		case VariantLogicalType::BITSTRING:
		case VariantLogicalType::BIGNUM:
		case VariantLogicalType::GEOMETRY:
			return VisitString(type_id, variant, row, values_idx, std::forward<Args>(args)...);
		case VariantLogicalType::DECIMAL:
			return VisitDecimal(variant, row, values_idx, std::forward<Args>(args)...);
		case VariantLogicalType::ARRAY: {
			auto nested_data = VariantUtils::DecodeNestedData(variant, row, values_idx);
			return Visitor::VisitArray(variant, row, nested_data, depth, std::forward<Args>(args)...);
		}
		case VariantLogicalType::OBJECT: {
			auto nested_data = VariantUtils::DecodeNestedData(variant, row, values_idx);
			return Visitor::VisitObject(variant, row, nested_data, depth, std::forward<Args>(args)...);
		}
		case VariantLogicalType::TIME_MICROS:
			return Visitor::VisitTime(Load<dtime_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::TIME_NANOS:
			return Visitor::VisitTimeNanos(Load<dtime_ns_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::TIME_MICROS_TZ:
			return Visitor::VisitTimeTZ(Load<dtime_tz_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::TIMESTAMP_SEC:
			return Visitor::VisitTimestampSec(Load<timestamp_sec_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::TIMESTAMP_MILIS:
			return Visitor::VisitTimestampMs(Load<timestamp_ms_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::TIMESTAMP_MICROS:
			return Visitor::VisitTimestamp(Load<timestamp_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::TIMESTAMP_NANOS:
			return Visitor::VisitTimestampNanos(Load<timestamp_ns_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::TIMESTAMP_MICROS_TZ:
			return Visitor::VisitTimestampTZ(Load<timestamp_tz_t>(ptr), std::forward<Args>(args)...);
		case VariantLogicalType::TIMESTAMP_NANOS_TZ:
			return Visitor::VisitTimestampTZNanos(Load<timestamp_tz_ns_t>(ptr), std::forward<Args>(args)...);
		default:
			return Visitor::VisitDefault(type_id, ptr, std::forward<Args>(args)...);
		}
	}

	template <typename... Args>
	static ReturnType Visit(const VariantNode &node, idx_t depth, Args &&...args) {
		CheckDepth(depth);
		if (node.IsNull() || node.IsMissing()) {
			VisitMetadata(VariantLogicalType::VARIANT_NULL, args...);
			return Visitor::VisitNull(std::forward<Args>(args)...);
		}

		auto type_id = node.GetTypeId();
		VisitMetadata(type_id, args...);

		switch (type_id) {
		case VariantLogicalType::VARIANT_NULL:
			return Visitor::VisitNull(std::forward<Args>(args)...);
		case VariantLogicalType::BOOL_TRUE:
			return Visitor::VisitBoolean(true, std::forward<Args>(args)...);
		case VariantLogicalType::BOOL_FALSE:
			return Visitor::VisitBoolean(false, std::forward<Args>(args)...);
		case VariantLogicalType::INT8:
			return Visitor::template VisitInteger<int8_t>(node.GetData<int8_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::INT16:
			return Visitor::template VisitInteger<int16_t>(node.GetData<int16_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::INT32:
			return Visitor::template VisitInteger<int32_t>(node.GetData<int32_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::INT64:
			return Visitor::template VisitInteger<int64_t>(node.GetData<int64_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::INT128:
			return Visitor::template VisitInteger<hugeint_t>(node.GetData<hugeint_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::UINT8:
			return Visitor::template VisitInteger<uint8_t>(node.GetData<uint8_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::UINT16:
			return Visitor::template VisitInteger<uint16_t>(node.GetData<uint16_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::UINT32:
			return Visitor::template VisitInteger<uint32_t>(node.GetData<uint32_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::UINT64:
			return Visitor::template VisitInteger<uint64_t>(node.GetData<uint64_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::UINT128:
			return Visitor::template VisitInteger<uhugeint_t>(node.GetData<uhugeint_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::FLOAT:
			return Visitor::VisitFloat(node.GetData<float>(), std::forward<Args>(args)...);
		case VariantLogicalType::DOUBLE:
			return Visitor::VisitDouble(node.GetData<double>(), std::forward<Args>(args)...);
		case VariantLogicalType::UUID:
			return Visitor::VisitUUID(node.GetData<hugeint_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::DATE:
			return Visitor::VisitDate(node.GetData<date_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::INTERVAL:
			return Visitor::VisitInterval(node.GetData<interval_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::VARCHAR:
		case VariantLogicalType::BLOB:
		case VariantLogicalType::BITSTRING:
		case VariantLogicalType::BIGNUM:
		case VariantLogicalType::GEOMETRY:
			return VisitString(type_id, node.GetString(), std::forward<Args>(args)...);
		case VariantLogicalType::DECIMAL:
			return VisitDecimal(node, std::forward<Args>(args)...);
		case VariantLogicalType::ARRAY:
			return Visitor::VisitArray(node, depth, std::forward<Args>(args)...);
		case VariantLogicalType::OBJECT:
			return Visitor::VisitObject(node, depth, std::forward<Args>(args)...);
		case VariantLogicalType::TIME_MICROS:
			return Visitor::VisitTime(node.GetData<dtime_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::TIME_NANOS:
			return Visitor::VisitTimeNanos(node.GetData<dtime_ns_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::TIME_MICROS_TZ:
			return Visitor::VisitTimeTZ(node.GetData<dtime_tz_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::TIMESTAMP_SEC:
			return Visitor::VisitTimestampSec(node.GetData<timestamp_sec_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::TIMESTAMP_MILIS:
			return Visitor::VisitTimestampMs(node.GetData<timestamp_ms_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::TIMESTAMP_MICROS:
			return Visitor::VisitTimestamp(node.GetData<timestamp_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::TIMESTAMP_NANOS:
			return Visitor::VisitTimestampNanos(node.GetData<timestamp_ns_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::TIMESTAMP_MICROS_TZ:
			return Visitor::VisitTimestampTZ(node.GetData<timestamp_tz_t>(), std::forward<Args>(args)...);
		case VariantLogicalType::TIMESTAMP_NANOS_TZ:
			return Visitor::VisitTimestampTZNanos(node.GetData<timestamp_tz_ns_t>(), std::forward<Args>(args)...);
		default:
			return Visitor::VisitDefault(type_id, nullptr, std::forward<Args>(args)...);
		}
	}

	// Non-void version
	template <typename R = ReturnType, typename... Args>
	static typename std::enable_if<!std::is_void<R>::value, vector<R>>::type
	VisitArrayItems(const UnifiedVariantVectorData &variant, idx_t row, const VariantNestedData &array_data,
	                idx_t depth, Args &&...args) {
		vector<R> array_items;
		array_items.reserve(array_data.child_count);
		for (idx_t i = 0; i < array_data.child_count; i++) {
			auto values_index = variant.GetValuesIndex(row, array_data.children_idx + i);
			array_items.emplace_back(Visit(variant, row, values_index, depth + 1, args...));
		}
		return array_items;
	}

	// Void version
	template <typename R = ReturnType, typename... Args>
	static typename std::enable_if<std::is_void<R>::value, void>::type
	VisitArrayItems(const UnifiedVariantVectorData &variant, idx_t row, const VariantNestedData &array_data,
	                idx_t depth, Args &&...args) {
		for (idx_t i = 0; i < array_data.child_count; i++) {
			auto values_index = variant.GetValuesIndex(row, array_data.children_idx + i);
			Visit(variant, row, values_index, depth + 1, args...);
		}
	}

	// Non-void version
	template <typename R = ReturnType, typename... Args>
	static std::enable_if_t<!std::is_void_v<R>, vector<R>> VisitArrayItems(const VariantNode &array, idx_t depth,
	                                                                       Args &&...args) {
		vector<R> array_items;
		array_items.reserve(array.GetArrayChildren().size());
		for (const auto &child : array.GetArrayChildren()) {
			array_items.emplace_back(Visit(child, depth + 1, args...));
		}
		return array_items;
	}

	// Void version
	template <typename R = ReturnType, typename... Args>
	static std::enable_if_t<std::is_void_v<R>, void> VisitArrayItems(const VariantNode &array, idx_t depth,
	                                                                 Args &&...args) {
		for (const auto &child : array.GetArrayChildren()) {
			Visit(child, depth + 1, args...);
		}
	}

	template <typename... Args>
	static child_list_t<ReturnType> VisitObjectItems(const UnifiedVariantVectorData &variant, idx_t row,
	                                                 const VariantNestedData &object_data, idx_t depth,
	                                                 Args &&...args) {
		child_list_t<ReturnType> object_items;
		for (idx_t i = 0; i < object_data.child_count; i++) {
			auto values_index = variant.GetValuesIndex(row, object_data.children_idx + i);
			auto val = Visit(variant, row, values_index, depth + 1, args...);

			auto keys_index = variant.GetKeysIndex(row, object_data.children_idx + i);
			auto &key = variant.GetKey(row, keys_index);

			object_items.emplace_back(key.GetString(), std::move(val));
		}
		std::stable_sort(
		    object_items.begin(), object_items.end(),
		    [](const std::pair<Identifier, ReturnType> &lhs, const std::pair<Identifier, ReturnType> &rhs) {
			    return lhs.first.GetIdentifierName() < rhs.first.GetIdentifierName();
		    });
		return object_items;
	}

private:
	static constexpr idx_t MAX_RECURSION_DEPTH = 128;

	static void CheckDepth(idx_t depth) {
		if (depth >= MAX_RECURSION_DEPTH) {
			throw InvalidInputException("VARIANT exceeds maximum recursion depth of %d", MAX_RECURSION_DEPTH);
		}
	}

private:
	template <typename V = Visitor, typename... Args>
	static typename std::enable_if<has_visit_metadata<V, Args...>::value, void>::type
	VisitMetadata(VariantLogicalType type_id, Args &&...args) {
		Visitor::VisitMetadata(type_id, std::forward<Args>(args)...);
	}

	// Fallback if the method does not exist
	template <typename V = Visitor, typename... Args>
	static typename std::enable_if<!has_visit_metadata<V, Args...>::value, void>::type VisitMetadata(VariantLogicalType,
	                                                                                                 Args &&...) {
		// do nothing
	}

	template <typename... Args>
	static ReturnType VisitString(VariantLogicalType type_id, const UnifiedVariantVectorData &variant, idx_t row,
	                              uint32_t values_idx, Args &&...args) {
		auto decoded_string = VariantUtils::DecodeStringData(variant, row, values_idx);
		if (type_id == VariantLogicalType::VARCHAR) {
			return Visitor::VisitString(decoded_string, std::forward<Args>(args)...);
		}
		if (type_id == VariantLogicalType::BLOB) {
			return Visitor::VisitBlob(decoded_string, std::forward<Args>(args)...);
		}
		if (type_id == VariantLogicalType::BIGNUM) {
			return Visitor::VisitBignum(decoded_string, std::forward<Args>(args)...);
		}
		if (type_id == VariantLogicalType::GEOMETRY) {
			return Visitor::VisitGeometry(decoded_string, std::forward<Args>(args)...);
		}
		if (type_id == VariantLogicalType::BITSTRING) {
			return Visitor::VisitBitstring(decoded_string, std::forward<Args>(args)...);
		}
		throw InternalException("String-backed variant type (%s) not handled", EnumUtil::ToString(type_id));
	}

	template <typename... Args>
	static ReturnType VisitString(VariantLogicalType type_id, const string_t &value, Args &&...args) {
		if (type_id == VariantLogicalType::VARCHAR) {
			return Visitor::VisitString(value, std::forward<Args>(args)...);
		}
		if (type_id == VariantLogicalType::BLOB) {
			return Visitor::VisitBlob(value, std::forward<Args>(args)...);
		}
		if (type_id == VariantLogicalType::BIGNUM) {
			return Visitor::VisitBignum(value, std::forward<Args>(args)...);
		}
		if (type_id == VariantLogicalType::GEOMETRY) {
			return Visitor::VisitGeometry(value, std::forward<Args>(args)...);
		}
		if (type_id == VariantLogicalType::BITSTRING) {
			return Visitor::VisitBitstring(value, std::forward<Args>(args)...);
		}
		throw InternalException("String-backed variant type (%s) not handled", EnumUtil::ToString(type_id));
	}

	template <typename... Args>
	static ReturnType VisitDecimal(const UnifiedVariantVectorData &variant, idx_t row, uint32_t values_idx,
	                               Args &&...args) {
		auto decoded_decimal = VariantUtils::DecodeDecimalData(variant, row, values_idx);
		auto &width = decoded_decimal.width;
		auto &scale = decoded_decimal.scale;
		auto &ptr = decoded_decimal.value_ptr;
		if (width > DecimalWidth<hugeint_t>::max) {
			throw InternalException("Can't handle decimal of width: %d", width);
		} else if (width > DecimalWidth<int64_t>::max) {
			return Visitor::template VisitDecimal<hugeint_t>(Load<hugeint_t>(ptr), width, scale,
			                                                 std::forward<Args>(args)...);
		} else if (width > DecimalWidth<int32_t>::max) {
			return Visitor::template VisitDecimal<int64_t>(Load<int64_t>(ptr), width, scale,
			                                               std::forward<Args>(args)...);
		} else if (width > DecimalWidth<int16_t>::max) {
			return Visitor::template VisitDecimal<int32_t>(Load<int32_t>(ptr), width, scale,
			                                               std::forward<Args>(args)...);
		} else {
			return Visitor::template VisitDecimal<int16_t>(Load<int16_t>(ptr), width, scale,
			                                               std::forward<Args>(args)...);
		}
	}

	template <typename... Args>
	static ReturnType VisitDecimal(const VariantNode &node, Args &&...args) {
		auto decimal = node.GetDecimal();
		if (decimal.width > DecimalWidth<hugeint_t>::max) {
			throw InternalException("Can't handle decimal of width: %d", decimal.width);
		} else if (decimal.width > DecimalWidth<int64_t>::max) {
			return Visitor::template VisitDecimal<hugeint_t>(node.GetData<hugeint_t>(), decimal.width, decimal.scale,
			                                                 std::forward<Args>(args)...);
		} else if (decimal.width > DecimalWidth<int32_t>::max) {
			return Visitor::template VisitDecimal<int64_t>(node.GetData<int64_t>(), decimal.width, decimal.scale,
			                                               std::forward<Args>(args)...);
		} else if (decimal.width > DecimalWidth<int16_t>::max) {
			return Visitor::template VisitDecimal<int32_t>(node.GetData<int32_t>(), decimal.width, decimal.scale,
			                                               std::forward<Args>(args)...);
		}
		return Visitor::template VisitDecimal<int16_t>(node.GetData<int16_t>(), decimal.width, decimal.scale,
		                                               std::forward<Args>(args)...);
	}
};

} // namespace duckdb
