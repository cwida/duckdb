#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/function/scalar/variant_functions.hpp"
#include "duckdb/common/serializer/varint.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/algorithm.hpp"

namespace duckdb {

static bool IsPrimitiveType(VariantLogicalType type) {
	return type != VariantLogicalType::OBJECT && type != VariantLogicalType::ARRAY;
}

static void VariantTypeofFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto count = input.size();

	D_ASSERT(input.ColumnCount() == 1);
	const auto &variant_vec = input.data[0];
	D_ASSERT(variant_vec.GetType() == LogicalType::VARIANT());

	RecursiveUnifiedVectorFormat source_format;
	Vector::RecursiveToUnifiedFormat(variant_vec, source_format);

	UnifiedVariantVectorData variant(source_format);

	auto result_data = FlatVector::Writer<string_t>(result, count);
	//! Reused across rows to avoid allocating per row
	string type_str;
	vector<string_t> object_keys;
	for (idx_t i = 0; i < count; i++) {
		if (!variant.RowIsValid(i)) {
			result_data.WriteValue(string_t("VARIANT_NULL"));
			continue;
		}

		auto type = variant.GetTypeId(i, 0);

		if (IsPrimitiveType(type)) {
			if (type != VariantLogicalType::DECIMAL) {
				type_str = EnumUtil::ToString(type);
			} else {
				auto decimal_data = VariantUtils::DecodeDecimalData(variant, i, 0);
				type_str = StringUtil::Format("DECIMAL(%d, %d)", decimal_data.width, decimal_data.scale);
			}
		} else if (type == VariantLogicalType::OBJECT) {
			auto nested_data = VariantUtils::DecodeNestedData(variant, i, 0);
			object_keys.clear();
			for (idx_t child_idx = 0; child_idx < nested_data.child_count; child_idx++) {
				auto key_index = variant.GetKeysIndex(i, nested_data.children_idx + child_idx);
				object_keys.push_back(variant.GetKey(i, key_index));
			}
			std::sort(object_keys.begin(), object_keys.end());
			type_str = "OBJECT(";
			for (idx_t key_idx = 0; key_idx < object_keys.size(); key_idx++) {
				if (key_idx > 0) {
					type_str += ", ";
				}
				type_str.append(object_keys[key_idx].GetData(), object_keys[key_idx].GetSize());
			}
			type_str += ")";
		} else {
			D_ASSERT(type == VariantLogicalType::ARRAY);
			auto nested_data = VariantUtils::DecodeNestedData(variant, i, 0);
			type_str = StringUtil::Format("ARRAY(%d)", nested_data.child_count);
		}
		result_data.WriteValue(string_t(type_str.c_str()));
	}
}

ScalarFunction VariantTypeofFun::GetFunction() {
	auto variant_type = LogicalType::VARIANT();
	auto res = ScalarFunction("variant_typeof", {}, LogicalType::VARCHAR, VariantTypeofFunction);
	res.GetSignature().AddParameter("input_variant", variant_type);
	res.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return res;
}

} // namespace duckdb
