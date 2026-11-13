//===----------------------------------------------------------------------===//
//                         DuckDB
//
// writer/variant_column_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "struct_column_writer.hpp"
#include "parquet_shredding.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"

namespace duckdb {

using variant_type_map = array<idx_t, static_cast<uint8_t>(VariantLogicalType::ENUM_SIZE)>;

struct ObjectAnalyzeData;
struct ArrayAnalyzeData;

struct VariantAnalyzeData {
public:
	VariantAnalyzeData();
	~VariantAnalyzeData();

public:
	//! Map for every value what type it is
	variant_type_map type_map = {};
	uint32_t decimal_width;
	uint32_t decimal_scale;
	bool decimal_consistent = false;
	idx_t total_count = 0;

	//! Map for every decimal value what physical type it has
	unique_ptr<ObjectAnalyzeData> object_data;
	unique_ptr<ArrayAnalyzeData> array_data;
};

struct ObjectAnalyzeData {
public:
	ObjectAnalyzeData() {
	}

public:
	VariantAnalyzeData &GetOrCreateField(const string_t &name) {
		auto entry = fields.find(name);
		if (entry != fields.end()) {
			return entry->second;
		}
		return fields[field_names.AddBlob(name)];
	}

public:
	case_insensitive_string_map_t<VariantAnalyzeData> fields;
	//! Owns the keys of 'fields'
	StringHeap field_names;
};

struct ArrayAnalyzeData {
public:
	ArrayAnalyzeData() {
	}

public:
	VariantAnalyzeData child;
};

struct VariantAnalyzeSchemaState : public ParquetAnalyzeSchemaState {
public:
	VariantAnalyzeSchemaState() {
	}
	~VariantAnalyzeSchemaState() override {
	}

public:
	VariantAnalyzeData analyze_data;
};

class VariantColumnWriter : public StructColumnWriter {
public:
	VariantColumnWriter(ParquetWriter &writer, ParquetColumnSchema &&column_schema, vector<Identifier> schema_path_p,
	                    vector<unique_ptr<ColumnWriter>> child_writers_p, bool auto_shred)
	    : StructColumnWriter(writer, std::move(column_schema), std::move(schema_path_p), std::move(child_writers_p)),
	      is_analyzed(!auto_shred) {
	}
	~VariantColumnWriter() override = default;

public:
	idx_t FinalizeSchema(duckdb_parquet::SchemaElementList &schemas) override;
	unique_ptr<ParquetAnalyzeSchemaState> AnalyzeSchemaInit() override;
	void AnalyzeSchema(ParquetAnalyzeSchemaState &state, Vector &input, idx_t count) override;
	void AnalyzeSchemaFinalize(const ParquetAnalyzeSchemaState &state) override;
	bool TryExportPreparedShreddingType(ShreddingType &result) const override;

	bool HasTransform() override {
		return true;
	}
	LogicalType TransformedType() const override {
		child_list_t<LogicalType> children;
		for (auto &writer : child_writers) {
			auto &child_name = writer->Schema().name;
			auto &child_type = writer->Schema().type;
			children.emplace_back(child_name, child_type);
		}
		return LogicalType::STRUCT(std::move(children));
	}
	unique_ptr<Expression> TransformExpression(unique_ptr<BoundReferenceExpression> expr) override {
		vector<unique_ptr<Expression>> arguments;
		arguments.push_back(unique_ptr_cast<BoundReferenceExpression, Expression>(std::move(expr)));

		BoundScalarFunction bound_func(GetTransformFunction(true));
		bound_func.SetReturnType(TransformedType());

		return make_uniq<BoundFunctionExpression>(std::move(bound_func), std::move(arguments), nullptr);
	}

public:
	static LogicalType GetParquetVariantType(optional_ptr<LogicalType> shredding = nullptr);
	static ScalarFunction GetTransformFunction(bool preserve_nulls = false);
	//! 'variant_bytes_to_variant': decode a binary Variant value (metadata followed by value)
	//! into a VARIANT. The inverse of 'variant_to_parquet_variant'.
	static ScalarFunction GetBytesToVariantFunction();
	static LogicalType TransformTypedValueRecursive(const LogicalType &type);

private:
	//! Whether the schema has been analyzed or explicitly specified
	bool is_analyzed = false;
	ShreddingType analyzed_shredding_type;
};

} // namespace duckdb
