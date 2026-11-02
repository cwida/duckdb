#include <string>
#include <utility>
#include <vector>

#include "writer/variant_column_writer.hpp"
#include "column_writer.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_types.h"
#include "duckdb/common/types/variant/parquet_variant_iterator.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

static optional_ptr<ColumnWriter> FindChildWriterByName(ColumnWriter &parent, const char *name) {
	for (auto &child : parent.ChildWriters()) {
		if (child->Schema().name == name) {
			return *child;
		}
	}
	return nullptr;
}

static void MarkTypedValueShreddingGroupsRequired(ColumnWriter &typed_value);

// Field/element group: mark REQUIRED, then recurse into nested typed_value if present
static void MarkShreddedGroupRequired(ColumnWriter &group) {
	D_ASSERT(StructType::IsStruct(group.Type().id()));
	group.MarkRepetitionRequired();
	auto nested_typed_value = FindChildWriterByName(group, "typed_value");
	if (nested_typed_value) {
		MarkTypedValueShreddingGroupsRequired(*nested_typed_value);
	}
}

// Field/element groups under typed_value must be REQUIRED (VariantShredding.md)
static void MarkTypedValueShreddingGroupsRequired(ColumnWriter &typed_value) {
	auto type_id = typed_value.Type().id();
	if (StructType::IsStruct(type_id)) {
		for (auto &field_group : typed_value.ChildWriters()) {
			MarkShreddedGroupRequired(*field_group);
		}
	} else if (type_id == LogicalTypeId::LIST || type_id == LogicalTypeId::ARRAY) {
		D_ASSERT(typed_value.ChildWriters().size() == 1);
		MarkShreddedGroupRequired(*typed_value.ChildWriters()[0]);
	}
}

idx_t VariantColumnWriter::FinalizeSchema(duckdb_parquet::SchemaElementList &schemas) {
	idx_t schema_idx = schemas.size();

	auto &schema = Schema();
	schema.SetSchemaIndex(schema_idx);

	auto &repetition_type = schema.repetition_type;
	auto &name = schema.name;
	auto &field_id = schema.field_id;

	// Mark shredded field/element groups REQUIRED (reserved Parquet names only)
	auto typed_value = FindChildWriterByName(*this, "typed_value");
	if (typed_value) {
		MarkTypedValueShreddingGroupsRequired(*typed_value);
	}

	// variant group
	schemas.emplace_back();
	auto &top_element = schemas.back();
	top_element.repetition_type = repetition_type;
	top_element.num_children = NumericCast<int32_t>(child_writers.size());
	top_element.logicalType.__isset.VARIANT = true;
	top_element.logicalType.VARIANT.__isset.specification_version = true;
	top_element.logicalType.VARIANT.specification_version = 1;
	top_element.__isset.logicalType = true;
	top_element.__isset.num_children = true;
	top_element.__isset.repetition_type = true;
	top_element.name = name;
	if (field_id.IsValid()) {
		top_element.__isset.field_id = true;
		top_element.field_id = NumericCast<int32_t>(field_id.GetIndex());
	}

	idx_t unique_columns = 0;
	for (auto &child_writer : child_writers) {
		unique_columns += child_writer->FinalizeSchema(schemas);
	}
	return unique_columns;
}

LogicalType VariantColumnWriter::TransformTypedValueRecursive(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::STRUCT: {
		//! Wrap all fields of the struct in a struct with 'value' and 'typed_value' fields
		auto &child_types = StructType::GetChildTypes(type);
		child_list_t<LogicalType> replaced_types;
		for (auto &entry : child_types) {
			child_list_t<LogicalType> child_children;
			child_children.emplace_back("value", LogicalType::BLOB);
			if (entry.second.id() != LogicalTypeId::VARIANT && entry.second.id() != LogicalTypeId::SQLNULL) {
				child_children.emplace_back("typed_value", TransformTypedValueRecursive(entry.second));
			}
			replaced_types.emplace_back(entry.first, LogicalType::STRUCT(child_children));
		}
		return LogicalType::STRUCT(replaced_types);
	}
	case LogicalTypeId::LIST: {
		auto &child_type = ListType::GetChildType(type);
		child_list_t<LogicalType> replaced_types;
		replaced_types.emplace_back("value", LogicalType::BLOB);
		if (child_type.id() != LogicalTypeId::VARIANT && child_type.id() != LogicalTypeId::SQLNULL) {
			replaced_types.emplace_back("typed_value", TransformTypedValueRecursive(child_type));
		}
		return LogicalType::LIST(LogicalType::STRUCT(replaced_types));
	}
	case LogicalTypeId::UNION:
	case LogicalTypeId::MAP:
	case LogicalTypeId::VARIANT:
	case LogicalTypeId::ARRAY:
		throw BinderException("'%s' can't appear inside a 'typed_value' shredded type!", type.ToString());
	default:
		return type;
	}
}

LogicalType VariantColumnWriter::GetParquetVariantType(optional_ptr<LogicalType> shredding) {
	child_list_t<LogicalType> children;
	children.emplace_back("metadata", LogicalType::BLOB);
	children.emplace_back("value", LogicalType::BLOB);
	if (shredding && shredding->id() != LogicalTypeId::VARIANT) {
		children.emplace_back("typed_value", VariantColumnWriter::TransformTypedValueRecursive(*shredding));
	}
	return LogicalType::STRUCT(std::move(children)).WithAlias("PARQUET_VARIANT");
}

static unique_ptr<FunctionData> BindTransform(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.empty()) {
		return nullptr;
	}
	auto type = ExpressionBinder::GetExpressionReturnType(*arguments[0]);

	if (arguments.size() == 2) {
		auto &shredding = *arguments[1];
		auto expr_return_type = ExpressionBinder::GetExpressionReturnType(shredding);
		expr_return_type = LogicalType::NormalizeType(expr_return_type);
		if (expr_return_type.id() != LogicalTypeId::VARCHAR) {
			throw BinderException("Optional second argument 'shredding' has to be of type VARCHAR, i.e: "
			                      "'STRUCT(my_field BOOLEAN)', found type: '%s' instead",
			                      expr_return_type);
		}
		auto type_str = input.GetNonNullConstant(1);
		auto shredded_type = TransformStringToLogicalType(type_str.GetValue<string>(), context);
		bound_function.SetReturnType(VariantColumnWriter::GetParquetVariantType(shredded_type));
	} else {
		bound_function.SetReturnType(VariantColumnWriter::GetParquetVariantType());
	}

	return nullptr;
}

//! The conversion itself lives in core (ParquetVariantConversion::ToParquetVariant), so the
//! arrow.parquet.variant extension type can call it directly without going through the binder.
static void ToParquetVariantFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	ParquetVariantConversion::ToParquetVariant(input.data[0], input.size(), result);
}

static void ToParquetVariantWriteFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	ToParquetVariantFunction(input, state, result);
	// Preserve SQL NULL at the VARIANT group.
	auto validity = input.data[0].Validity();
	if (validity.CannotHaveNull()) {
		return;
	}
	for (idx_t i = 0; i < input.size(); i++) {
		if (!validity.IsValid(i)) {
			FlatVector::SetNull(result, i, true);
		}
	}
}

ScalarFunction VariantColumnWriter::GetTransformFunction(bool preserve_nulls) {
	auto function = preserve_nulls ? ToParquetVariantWriteFunction : ToParquetVariantFunction;
	ScalarFunction transform("variant_to_parquet_variant", {}, LogicalType::ANY, function, BindTransform);
	transform.GetSignature().AddParameter("variant", LogicalType::VARIANT());
	transform.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// throws for values that are out of range for the parquet variant encoding
	transform.SetFallible();
	return transform;
}

static void VariantBytesToVariantFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	ParquetVariantConversion::ConvertBinary(input.data[0], result, input.size());
}

ScalarFunction VariantColumnWriter::GetBytesToVariantFunction() {
	ScalarFunction function("variant_bytes_to_variant", {}, LogicalType::VARIANT(), VariantBytesToVariantFunction);
	function.GetSignature().AddParameter("blob", LogicalType::BLOB);
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	function.SetFallible();
	return function;
}

} // namespace duckdb
