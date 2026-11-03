
#include "parquet_geometry.hpp"

#include <stdlib.h>
#include <string.h>
#include <unordered_set>
#include <utility>
#include <vector>

#include "column_reader.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/scalar/geometry_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "reader/expression_column_reader.hpp"
#include "yyjson.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "reader/string_column_reader.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/setting_info.hpp"
#include "duckdb/planner/expression.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_types.h"

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT

//------------------------------------------------------------------------------
// GeoParquetFileMetadata
//------------------------------------------------------------------------------
unique_ptr<GeoParquetFileMetadata> GeoParquetFileMetadata::TryRead(const duckdb_parquet::FileMetaData &file_meta_data,
                                                                   ClientContext &context) {
	// Conversion not enabled, or spatial is not loaded!
	if (!IsGeoParquetConversionEnabled(context)) {
		return nullptr;
	}

	for (auto &kv : file_meta_data.key_value_metadata) {
		if (kv.key == "geo") {
			const auto geo_metadata = yyjson_read(kv.value.c_str(), kv.value.size(), 0);
			if (!geo_metadata) {
				// Could not parse the JSON
				return nullptr;
			}

			try {
				// Check the root object
				const auto root = yyjson_doc_get_root(geo_metadata);
				if (!yyjson_is_obj(root)) {
					throw InvalidInputException("Geoparquet metadata is not an object");
				}

				// We dont actually care about the version for now, as we only support V1+native
				auto result = make_uniq<GeoParquetFileMetadata>(GeoParquetVersion::BOTH);

				// Check and parse the version
				const auto version_val = yyjson_obj_get(root, "version");
				if (!yyjson_is_str(version_val)) {
					throw InvalidInputException("Geoparquet metadata does not have a version");
				}

				auto version = yyjson_get_str(version_val);
				if (StringUtil::StartsWith(version, "3")) {
					// Guard against a breaking future 3.0 version
					throw InvalidInputException("Geoparquet version %s is not supported", version);
				}

				// Check and parse the geometry columns
				const auto columns_val = yyjson_obj_get(root, "columns");
				if (!yyjson_is_obj(columns_val)) {
					throw InvalidInputException("Geoparquet metadata does not have a columns object");
				}

				// Iterate over all geometry columns
				yyjson_obj_iter iter = yyjson_obj_iter_with(columns_val);
				yyjson_val *column_key;

				while ((column_key = yyjson_obj_iter_next(&iter))) {
					const auto column_val = yyjson_obj_iter_get_val(column_key);
					const auto column_name = yyjson_get_str(column_key);

					auto &column = result->geometry_columns[column_name];

					if (!yyjson_is_obj(column_val)) {
						throw InvalidInputException("Geoparquet column '%s' is not an object", column_name);
					}

					// Parse the encoding
					const auto encoding_val = yyjson_obj_get(column_val, "encoding");
					if (!yyjson_is_str(encoding_val)) {
						throw InvalidInputException("Geoparquet column '%s' does not have an encoding", column_name);
					}
					const auto encoding_str = yyjson_get_str(encoding_val);
					if (strcmp(encoding_str, "WKB") == 0) {
						column.geometry_encoding = GeoParquetColumnEncoding::WKB;
					} else if (strcmp(encoding_str, "point") == 0) {
						column.geometry_encoding = GeoParquetColumnEncoding::POINT;
					} else if (strcmp(encoding_str, "linestring") == 0) {
						column.geometry_encoding = GeoParquetColumnEncoding::LINESTRING;
					} else if (strcmp(encoding_str, "polygon") == 0) {
						column.geometry_encoding = GeoParquetColumnEncoding::POLYGON;
					} else if (strcmp(encoding_str, "multipoint") == 0) {
						column.geometry_encoding = GeoParquetColumnEncoding::MULTIPOINT;
					} else if (strcmp(encoding_str, "multilinestring") == 0) {
						column.geometry_encoding = GeoParquetColumnEncoding::MULTILINESTRING;
					} else if (strcmp(encoding_str, "multipolygon") == 0) {
						column.geometry_encoding = GeoParquetColumnEncoding::MULTIPOLYGON;
					} else {
						throw InvalidInputException("Geoparquet column '%s' has an unsupported encoding", column_name);
					}

					// Parse the geometry types
					const auto geometry_types_val = yyjson_obj_get(column_val, "geometry_types");
					if (!yyjson_is_arr(geometry_types_val)) {
						throw InvalidInputException("Geoparquet column '%s' does not have geometry types", column_name);
					}

					// Parse the CRS
					const auto crs_val = yyjson_obj_get(column_val, "crs");
					if (crs_val && !yyjson_is_null(crs_val)) {
						// Parse the CRS
						if (!yyjson_is_obj(crs_val)) {
							throw InvalidInputException("Geoparquet column '%s' has invalid CRS", column_name);
						}

						// Move the CRS as a JSON string
						size_t crs_json_len = 0;
						const auto crs_json = yyjson_val_write(crs_val, 0, &crs_json_len);
						column.projjson = string(crs_json, crs_json_len);

						// Free the temporary CRS JSON string
						free(crs_json);
					} else if (crs_val && yyjson_is_null(crs_val)) {
						// If CRS is null, do nothing
					} else {
						// Otherwise, if no CRS, default to OGC:CRS84
						auto crs = CoordinateReferenceSystem::TryConvert(context, "OGC:CRS84",
						                                                 CoordinateReferenceSystemType::PROJJSON);
						if (crs) {
							column.projjson = crs->GetDefinition();
						}
					}

					// TODO: Parse the bounding box, other metadata that might be useful.
					// (Only encoding and geometry types are required to be present)
				}

				// Return the result
				// Make sure to free the JSON document
				yyjson_doc_free(geo_metadata);
				return result;

			} catch (...) {
				// Make sure to free the JSON document in case of an exception
				yyjson_doc_free(geo_metadata);
				throw;
			}
		}
	}
	return nullptr;
}

static void ConvertCRS(ClientContext &context, GeoParquetColumnMetadata &column, const string &column_name,
                       const CoordinateReferenceSystem &crs, GeoParquetVersion version) {
	// This is the default in GeoParquet
	if (StringUtil::CIEquals("OGC:CRS84", crs.GetIdentifier())) {
		// Dont write the default
		return;
	}

	if (crs.GetType() == CoordinateReferenceSystemType::PROJJSON) {
		// If already PROJJSON, just write it directly
		column.projjson = crs.GetDefinition();
		return;
	}

	const auto lookup = CoordinateReferenceSystem::TryConvert(context, crs, CoordinateReferenceSystemType::PROJJSON);

	if (lookup) {
		// Successfully converted to PROJJSON
		column.projjson = lookup->GetDefinition();
		return;
	}

	if (version != GeoParquetVersion::V2) {
		throw InvalidInputException("Cannot write GeoParquet V1 metadata for column '%s': GeoParquet V1 only "
		                            "supports PROJJSON CRS definitions",
		                            column_name);
	}

	// Fall back to writing the original CRS definition
	column.projjson = crs.GetDefinition();
}

void GeoParquetFileMetadata::AddGeoParquetStats(ClientContext &context, const string &column_name,
                                                const LogicalType &type, const GeometryStatsData &stats,
                                                GeoParquetVersion version) {
	// Lock the metadata
	lock_guard<mutex> glock(write_lock);

	const auto it = geometry_columns.find(column_name);
	if (it == geometry_columns.end()) {
		// Add a new column to the metadata
		auto &column = geometry_columns[column_name];

		// Attempt to convert the CRS to PROJJSON
		if (GeoType::HasCRS(type)) {
			ConvertCRS(context, column, column_name, GeoType::GetCRS(type), version);
		}

		// Merge the stats
		column.stats.Merge(stats);
		column.insertion_index = geometry_columns.size() - 1;

	} else {
		it->second.stats.Merge(stats);
	}
}

void GeoParquetFileMetadata::Write(duckdb_parquet::FileMetaData &file_meta_data) {
	// GeoParquet does not support M or ZM coordinates. So remove any columns that have them.
	unordered_set<string> invalid_columns;
	for (auto &column : geometry_columns) {
		if (column.second.stats.extent.HasM()) {
			invalid_columns.insert(column.first);
		}
	}
	for (auto &col_name : invalid_columns) {
		geometry_columns.erase(col_name);
	}
	// No columns remaining, nothing to write
	if (geometry_columns.empty()) {
		return;
	}

	// Find the primary geometry column
	const auto &random_first_column = *geometry_columns.begin();
	auto primary_geometry_column = random_first_column.first;
	auto primary_insertion_index = random_first_column.second.insertion_index;

	for (auto &column : geometry_columns) {
		if (column.second.insertion_index < primary_insertion_index) {
			primary_insertion_index = column.second.insertion_index;
			primary_geometry_column = column.first;
		}
	}

	yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
	yyjson_mut_val *root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);

	// Add the version
	switch (version) {
	case GeoParquetVersion::V1:
	case GeoParquetVersion::BOTH:
		yyjson_mut_obj_add_strcpy(doc, root, "version", "1.0.0");
		break;
	case GeoParquetVersion::V2:
		yyjson_mut_obj_add_strcpy(doc, root, "version", "2.0.0");
		break;
	case GeoParquetVersion::NONE:
	default:
		// Should never happen, we should not be writing anything
		yyjson_mut_doc_free(doc);
		throw InternalException("GeoParquetVersion::NONE should not write metadata");
	}

	// Add the primary column
	yyjson_mut_obj_add_strncpy(doc, root, "primary_column", primary_geometry_column.c_str(),
	                           primary_geometry_column.size());

	// Add the columns
	const auto json_columns = yyjson_mut_obj_add_obj(doc, root, "columns");

	for (auto &column : geometry_columns) {
		const auto column_json = yyjson_mut_obj_add_obj(doc, json_columns, column.first.c_str());
		yyjson_mut_obj_add_str(doc, column_json, "encoding", "WKB");
		const auto geometry_types = yyjson_mut_obj_add_arr(doc, column_json, "geometry_types");

		for (auto &type_name : column.second.stats.types.ToString(false)) {
			yyjson_mut_arr_add_strcpy(doc, geometry_types, type_name.c_str());
		}

		const auto &bbox = column.second.stats.extent;

		if (bbox.HasXY()) {
			const auto bbox_arr = yyjson_mut_obj_add_arr(doc, column_json, "bbox");

			if (!column.second.stats.extent.HasZ()) {
				yyjson_mut_arr_add_real(doc, bbox_arr, bbox.x_min);
				yyjson_mut_arr_add_real(doc, bbox_arr, bbox.y_min);
				yyjson_mut_arr_add_real(doc, bbox_arr, bbox.x_max);
				yyjson_mut_arr_add_real(doc, bbox_arr, bbox.y_max);
			} else {
				yyjson_mut_arr_add_real(doc, bbox_arr, bbox.x_min);
				yyjson_mut_arr_add_real(doc, bbox_arr, bbox.y_min);
				yyjson_mut_arr_add_real(doc, bbox_arr, bbox.z_min);
				yyjson_mut_arr_add_real(doc, bbox_arr, bbox.x_max);
				yyjson_mut_arr_add_real(doc, bbox_arr, bbox.y_max);
				yyjson_mut_arr_add_real(doc, bbox_arr, bbox.z_max);
			}
		}

		// If the CRS is present, add it
		if (!column.second.projjson.empty()) {
			const auto crs_doc = yyjson_read(column.second.projjson.c_str(), column.second.projjson.size(), 0);
			if (crs_doc) {
				const auto crs_root = yyjson_doc_get_root(crs_doc);
				const auto crs_val = yyjson_val_mut_copy(doc, crs_root);
				const auto crs_key = yyjson_mut_strcpy(doc, "crs");
				yyjson_mut_obj_add(column_json, crs_key, crs_val);
				yyjson_doc_free(crs_doc);
			} else {
				// Failed to parse the CRS JSON. If this is geoparquet v2, we support other CRS formats
				if (version != GeoParquetVersion::V2) {
					// Skip adding the CRS
					throw SerializationException("Failed to parse CRS PROJJSON for column '%s'", column.first.c_str());
				}
			}
		}
	}

	yyjson_write_err err;
	size_t len;
	char *json = yyjson_mut_write_opts(doc, 0, nullptr, &len, &err);
	if (!json) {
		yyjson_mut_doc_free(doc);
		throw SerializationException("Failed to write JSON string: %s", err.msg);
	}

	// Create a string from the JSON
	file_meta_data.key_value_metadata.emplace_back();
	auto &kv = file_meta_data.key_value_metadata.back();
	kv.__set_key("geo");
	kv.__set_value(string(json, len));

	// Free the JSON and the document
	free(json);
	yyjson_mut_doc_free(doc);

	file_meta_data.__isset.key_value_metadata = true;
}

bool GeoParquetFileMetadata::IsGeometryColumn(const string &column_name) const {
	return geometry_columns.find(column_name) != geometry_columns.end();
}

bool GeoParquetFileMetadata::IsGeoParquetConversionEnabled(const ClientContext &context) {
	bool geoparquet_enabled = false;
	context.TryGetCurrentSetting("enable_geoparquet_conversion", geoparquet_enabled);
	return geoparquet_enabled;
}

const unordered_map<string, GeoParquetColumnMetadata> &GeoParquetFileMetadata::GetColumnMeta() const {
	return geometry_columns;
}

optional_ptr<const GeoParquetColumnMetadata> GeoParquetFileMetadata::GetColumnMeta(const string &column_name) const {
	const auto it = geometry_columns.find(column_name);
	if (it == geometry_columns.end()) {
		return nullptr;
	}
	return &it->second;
}

unique_ptr<ColumnReader> GeometryColumnReader::Create(const ParquetReader &reader, const ParquetColumnSchema &schema,
                                                      ClientContext &context) {
	D_ASSERT(schema.type.id() == LogicalTypeId::GEOMETRY);
	D_ASSERT(schema.children.size() == 1 && schema.children[0].type.id() == LogicalTypeId::BLOB);

	// Make a string reader for the underlying WKB data
	auto string_reader = make_uniq<StringColumnReader>(reader, schema.children[0]);

	// Wrap the string reader in a geometry reader
	auto args = vector<unique_ptr<Expression>>();
	auto ref = make_uniq_base<Expression, BoundReferenceExpression>(LogicalTypeId::BLOB, 0);
	args.push_back(std::move(ref));

	// TODO: Pass the actual target type here so we get the CRS information too
	auto func = StGeomfromwkbFun::GetFunction();
	func.SetName("ST_GeomFromWKB");
	auto read_expr = func.Bind(context, std::move(args));
	auto type_expr = BoundCastExpression::AddDefaultCastToType(std::move(read_expr), schema.type);
	vector<unique_ptr<ColumnReader>> expression_children;
	expression_children.push_back(std::move(string_reader));
	return make_uniq<ExpressionColumnReader>(context, std::move(expression_children), std::move(type_expr), schema);
}

} // namespace duckdb
