#include <stdint.h>
#include <string>
#include <unordered_map>
#include <utility>

#include "writer/variant_column_writer.hpp"
#include "parquet_writer.hpp"
#include "column_writer.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/vector/unified_vector_format.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "parquet_column_schema.hpp"

namespace duckdb {

//! Auto-shredding stops at this depth of nesting: an OBJECT or ARRAY nested this deep is kept whole in 'value', with
//! no 'typed_value' of its own. Every shredded level of nesting adds several levels to the Parquet schema (and to the
//! recursion of the writer and of every reader), so an unbounded depth would produce a file no reader accepts, or
//! overflow the stack writing it.
static constexpr idx_t MAX_SHREDDING_DEPTH = 32;

unique_ptr<ParquetAnalyzeSchemaState> VariantColumnWriter::AnalyzeSchemaInit() {
	if (child_writers.size() == 2 && !is_analyzed) {
		return make_uniq<VariantAnalyzeSchemaState>();
	}
	//! Variant is already shredded explicitly, no need to analyze
	return nullptr;
}

static void AnalyzeSchemaInternal(VariantAnalyzeData &state, UnifiedVariantVectorData &variant, idx_t row,
                                  uint32_t values_index, idx_t depth) {
	state.total_count++;
	if (!variant.RowIsValid(row)) {
		state.type_map[static_cast<uint8_t>(VariantLogicalType::VARIANT_NULL)]++;
		return;
	}

	auto type_id = variant.GetTypeId(row, values_index);
	state.type_map[static_cast<uint8_t>(type_id)]++;

	if (type_id == VariantLogicalType::OBJECT || type_id == VariantLogicalType::ARRAY) {
		if (depth >= MAX_SHREDDING_DEPTH) {
			//! Nested too deep to shred: the value is kept whole in 'value', so its children are not analyzed
			return;
		}
	}
	if (type_id == VariantLogicalType::OBJECT) {
		if (!state.object_data) {
			state.object_data = make_uniq<ObjectAnalyzeData>();
		}
		auto &object_data = *state.object_data;

		auto nested_data = VariantUtils::DecodeNestedData(variant, row, values_index);
		for (idx_t i = 0; i < nested_data.child_count; i++) {
			auto child_values_index = variant.GetValuesIndex(row, i + nested_data.children_idx);
			auto child_key_index = variant.GetKeysIndex(row, i + nested_data.children_idx);

			auto &key = variant.GetKey(row, child_key_index);
			auto &child_state = object_data.GetOrCreateField(key);
			AnalyzeSchemaInternal(child_state, variant, row, child_values_index, depth + 1);
		}
	} else if (type_id == VariantLogicalType::ARRAY) {
		if (!state.array_data) {
			state.array_data = make_uniq<ArrayAnalyzeData>();
		}
		auto &array_data = *state.array_data;
		auto nested_data = VariantUtils::DecodeNestedData(variant, row, values_index);
		for (idx_t i = 0; i < nested_data.child_count; i++) {
			auto child_values_index = variant.GetValuesIndex(row, i + nested_data.children_idx);
			auto &child_state = array_data.child;
			AnalyzeSchemaInternal(child_state, variant, row, child_values_index, depth + 1);
		}
	} else if (type_id == VariantLogicalType::DECIMAL) {
		auto decimal_data = VariantUtils::DecodeDecimalData(variant, row, values_index);
		auto decimal_count = state.type_map[static_cast<uint8_t>(VariantLogicalType::DECIMAL)];
		decimal_count--;
		if (!decimal_count) {
			state.decimal_width = decimal_data.width;
			state.decimal_scale = decimal_data.scale;
			state.decimal_consistent = true;
			return;
		}
		if (!state.decimal_consistent) {
			return;
		}
		if (decimal_data.width != state.decimal_width || decimal_data.scale != state.decimal_scale) {
			state.decimal_consistent = false;
		}
	} else if (type_id == VariantLogicalType::BOOL_FALSE) {
		//! Move it to bool_true to have the counts all in one place
		state.type_map[static_cast<uint8_t>(VariantLogicalType::BOOL_TRUE)]++;
		state.type_map[static_cast<uint8_t>(VariantLogicalType::BOOL_FALSE)]--;
	}
}

void VariantColumnWriter::AnalyzeSchema(ParquetAnalyzeSchemaState &state_p, Vector &input, idx_t count) {
	auto &state = state_p.Cast<VariantAnalyzeSchemaState>();

	RecursiveUnifiedVectorFormat recursive_format;
	Vector::RecursiveToUnifiedFormat(input, recursive_format);
	UnifiedVariantVectorData variant(recursive_format);

	for (idx_t i = 0; i < count; i++) {
		AnalyzeSchemaInternal(state.analyze_data, variant, i, 0, 0);
	}
}

namespace {

struct ShredAnalysisState {
	idx_t highest_count = 0;
	LogicalType type = LogicalType::INVALID;
};

} // namespace

template <VariantLogicalType VARIANT_TYPE, LogicalTypeId SHREDDED_TYPE>
static void CheckPrimitive(const VariantAnalyzeData &state, ShredAnalysisState &result) {
	auto count = state.type_map[static_cast<uint8_t>(VARIANT_TYPE)];
	if (count <= result.highest_count) {
		return;
	}
	if (VARIANT_TYPE == VariantLogicalType::DECIMAL) {
		D_ASSERT(count);
		if (!state.decimal_consistent) {
			return;
		}
		result.highest_count = count;
		result.type = LogicalType::DECIMAL(state.decimal_width, state.decimal_scale);
	} else {
		result.highest_count = count;
		result.type = SHREDDED_TYPE;
	}
}

static bool ConstructShreddedType(const VariantAnalyzeData &state, LogicalType &out, idx_t depth) {
	ShredAnalysisState result;

	if (state.type_map[0] == state.total_count) {
		//! All NULL, emit INT32
		out = LogicalType::INTEGER;
		return true;
	}

	CheckPrimitive<VariantLogicalType::BOOL_TRUE, LogicalTypeId::BOOLEAN>(state, result);
	CheckPrimitive<VariantLogicalType::INT8, LogicalTypeId::TINYINT>(state, result);
	CheckPrimitive<VariantLogicalType::INT16, LogicalTypeId::SMALLINT>(state, result);
	CheckPrimitive<VariantLogicalType::INT32, LogicalTypeId::INTEGER>(state, result);
	CheckPrimitive<VariantLogicalType::INT64, LogicalTypeId::BIGINT>(state, result);
	CheckPrimitive<VariantLogicalType::FLOAT, LogicalTypeId::FLOAT>(state, result);
	CheckPrimitive<VariantLogicalType::DOUBLE, LogicalTypeId::DOUBLE>(state, result);
	CheckPrimitive<VariantLogicalType::DECIMAL, LogicalTypeId::DECIMAL>(state, result);
	CheckPrimitive<VariantLogicalType::DATE, LogicalTypeId::DATE>(state, result);
	CheckPrimitive<VariantLogicalType::TIME_MICROS, LogicalTypeId::TIME>(state, result);
	CheckPrimitive<VariantLogicalType::TIMESTAMP_MICROS, LogicalTypeId::TIMESTAMP>(state, result);
	CheckPrimitive<VariantLogicalType::TIMESTAMP_NANOS, LogicalTypeId::TIMESTAMP_NS>(state, result);
	CheckPrimitive<VariantLogicalType::TIMESTAMP_MICROS_TZ, LogicalTypeId::TIMESTAMP_TZ>(state, result);
	CheckPrimitive<VariantLogicalType::TIMESTAMP_NANOS_TZ, LogicalTypeId::TIMESTAMP_TZ_NS>(state, result);
	CheckPrimitive<VariantLogicalType::BLOB, LogicalTypeId::BLOB>(state, result);
	CheckPrimitive<VariantLogicalType::VARCHAR, LogicalTypeId::VARCHAR>(state, result);
	CheckPrimitive<VariantLogicalType::UUID, LogicalTypeId::UUID>(state, result);
	// these types are not natively supported in Parquet - we convert them during write
	// during analysis map them to the type we convert them into
	CheckPrimitive<VariantLogicalType::UINT8, LogicalTypeId::SMALLINT>(state, result);
	CheckPrimitive<VariantLogicalType::UINT16, LogicalTypeId::INTEGER>(state, result);
	CheckPrimitive<VariantLogicalType::UINT32, LogicalTypeId::BIGINT>(state, result);
	CheckPrimitive<VariantLogicalType::UINT64, LogicalTypeId::BIGINT>(state, result);
	CheckPrimitive<VariantLogicalType::UINT128, LogicalTypeId::BIGINT>(state, result);
	CheckPrimitive<VariantLogicalType::INT128, LogicalTypeId::BIGINT>(state, result);

	auto array_count = state.type_map[static_cast<uint8_t>(VariantLogicalType::ARRAY)];
	auto object_count = state.type_map[static_cast<uint8_t>(VariantLogicalType::OBJECT)];
	if (array_count > object_count) {
		if (array_count > result.highest_count) {
			if (depth >= MAX_SHREDDING_DEPTH) {
				//! Nested too deep to shred: the array is kept whole in 'value' (its children were not analyzed)
				out = LogicalType::VARIANT();
				return true;
			}
			auto &array_data = *state.array_data;
			LogicalType child_type;
			if (!ConstructShreddedType(array_data.child, child_type, depth + 1)) {
				return false;
			}
			out = LogicalType::LIST(child_type);
			return true;
		}
	} else {
		if (object_count > result.highest_count) {
			if (depth >= MAX_SHREDDING_DEPTH) {
				//! Nested too deep to shred: the object is kept whole in 'value' (its fields were not analyzed)
				out = LogicalType::VARIANT();
				return true;
			}
			auto &object_data = *state.object_data;

			//! TODO: implement some logic to determine which fields are worth shredding, considering the overhead when
			//! only 10% of rows make use of the field
			child_list_t<LogicalType> field_types;
			for (auto &field : object_data.fields) {
				if (field.first.Empty()) {
					//! A STRUCT field cannot have an empty name, so the empty key stays in the 'value'
					continue;
				}
				LogicalType child_type;
				if (!ConstructShreddedType(field.second, child_type, depth + 1)) {
					// cannot shred on this field - skip
					continue;
				}
				field_types.emplace_back(field.first.GetString(), child_type);
			}
			if (field_types.empty()) {
				// no field types to shred on - avoid shredding
				return false;
			}
			out = LogicalType::STRUCT(field_types);
			return true;
		}
	}
	if (result.type.id() == LogicalTypeId::INVALID) {
		return false;
	}
	out = result.type;
	return true;
}

void VariantColumnWriter::AnalyzeSchemaFinalize(const ParquetAnalyzeSchemaState &state_p) {
	auto &state = state_p.Cast<VariantAnalyzeSchemaState>();
	LogicalType shredded_type;
	if (!ConstructShreddedType(state.analyze_data, shredded_type, 0)) {
		//! Can't shred, keep the original children
		//! Mark as analyzed to prevent re-analysis from modifying child_writers
		//! after InitializeSchemaElements has already locked in the schema
		is_analyzed = true;
		return;
	}
	is_analyzed = true;
	analyzed_shredding_type = ShreddingType(shredded_type);
	auto typed_value = TransformTypedValueRecursive(shredded_type);
	auto &schema = Schema();
	auto &context = writer.GetContext();
	D_ASSERT(child_writers.size() == 2);
	child_writers.pop_back();
	//! Recreate the column writer for 'value' because this is now "optional"
	child_writers.push_back(ColumnWriter::CreateWriterRecursive(context, writer, schema_path, LogicalType::BLOB,
	                                                            "value", false, nullptr, nullptr, schema.max_repeat,
	                                                            schema.max_define + 1, true));
	child_writers.push_back(ColumnWriter::CreateWriterRecursive(context, writer, schema_path, typed_value,
	                                                            "typed_value", false, nullptr, nullptr,
	                                                            schema.max_repeat, schema.max_define + 1, true));
}

bool VariantColumnWriter::TryExportPreparedShreddingType(ShreddingType &result) const {
	if (analyzed_shredding_type.set) {
		result = analyzed_shredding_type.Copy();
		return true;
	}
	return StructColumnWriter::TryExportPreparedShreddingType(result);
}

} // namespace duckdb
