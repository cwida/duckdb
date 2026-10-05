// Decodes Parquet VARIANT bytes for duckdb_parquet_variant_bytes_to_json (see parquet_extension.cpp).
// The `arrow.parquet.variant` Arrow extension itself is registered by core.

#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/types/variant/parquet_variant_iterator.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "yyjson.hpp"

#include <cstring>
#include <cstdlib>

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT

string ParquetVariantBytesToJson(const_data_ptr_t metadata, idx_t metadata_len, const_data_ptr_t value,
                                 idx_t value_len) {
	Vector combined(LogicalType::BLOB, 1);
	{
		auto writer = FlatVector::Writer<string_t>(combined, 1);
		auto &blob = writer.WriteEmptyString(metadata_len + value_len);
		memcpy(blob.GetDataWriteable(), metadata, metadata_len);
		memcpy(blob.GetDataWriteable() + metadata_len, value, value_len);
		blob.Finalize();
	}

	Vector variant(LogicalType::VARIANT(), 1);
	ParquetVariantConversion::ConvertBinary(combined, variant, 1);

	VariantIterator iterator(variant);
	if (!iterator.RowIsValid(0)) {
		return "null";
	}
	auto node = iterator.Root(0);
	if (node.IsNull()) {
		return "null";
	}

	auto doc = yyjson_mut_doc_new(nullptr);
	auto json_val = VariantCasts::ConvertVariantToJSON(doc, node);
	if (!json_val) {
		yyjson_mut_doc_free(doc);
		throw InternalException("Failed to convert VARIANT value to JSON");
	}
	size_t len = 0;
	char *json_cstr = yyjson_mut_val_write_opts(json_val, 0, nullptr, &len, nullptr);
	yyjson_mut_doc_free(doc);
	if (!json_cstr) {
		throw InternalException("Failed to serialize VARIANT value to JSON");
	}
	string result(json_cstr, len);
	free(json_cstr);
	return result;
}

} // namespace duckdb
