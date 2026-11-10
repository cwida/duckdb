#include "duckdb/common/bswap.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar/compressed_materialization_functions.hpp"
#include "duckdb/function/scalar/compressed_materialization_utils.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

namespace {

// An internally stored non-empty POINT-XY geometry is always exactly these sizes:
// 1 byte little-endian byte order + 4 byte WKB type/meta (= 1) + 2 doubles (X, Y)
constexpr auto POINT_XY_HEADER_SIZE = sizeof(uint8_t) + sizeof(uint32_t);
constexpr auto POINT_XY_COORD_SIZE = sizeof(double) + sizeof(double);
constexpr auto POINT_XY_BLOB_SIZE = POINT_XY_HEADER_SIZE + POINT_XY_COORD_SIZE;

uhugeint_t GeometryPointCompress(const string_t &input) {
	// The statistics that lead to this compression can come from a file, so verify that this is a POINT-XY
	const auto data = const_data_ptr_cast(input.GetData());
	if (input.GetSize() != POINT_XY_BLOB_SIZE || Load<uint8_t>(data) != 1 ||
	    Load<uint32_t>(data + sizeof(uint8_t)) != 1) {
		throw InvalidInputException("Expected only 2D POINT geometries based on the column statistics, but "
		                            "encountered a different geometry - the statistics might be corrupt");
	}

	// The 16 coordinate bytes (X, Y) follow the fixed 5-byte WKB header (byte order + type)
	const auto coords = const_data_ptr_cast(input.GetData()) + POINT_XY_HEADER_SIZE;

	// Pack the coordinates into a UHUGEINT, reversing the byte order so that an unsigned integer
	// comparison of the result matches the lexicographic (memcmp) byte comparison.
	uhugeint_t result;
	const auto result_ptr = data_ptr_cast(&result);
	for (idx_t i = 0; i < POINT_XY_COORD_SIZE; i++) {
		result_ptr[i] = coords[POINT_XY_COORD_SIZE - 1 - i];
	}
	return BSwapIfBE(result);
}

void GeometryPointCompressFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, uhugeint_t>(args.data[0], result, GeometryPointCompress,
	                                             FunctionErrors::CAN_THROW_RUNTIME_ERROR);
}

void GeometryPointDecompress(const uhugeint_t &input, const data_ptr_t output) {
	const auto le_input = BSwapIfBE(input);
	const auto le_input_ptr = const_data_ptr_cast(&le_input);

	// Reconstruct the fixed POINT-XY WKB header: little-endian byte order (1) + WKB type POINT (1)
	Store<uint8_t>(1, output);
	Store<uint32_t>(1, output + sizeof(uint8_t));

	// Restore the coordinate bytes (inverse of the byte reversal in GeometryPointCompress)
	const auto coords = output + POINT_XY_HEADER_SIZE;
	for (idx_t i = 0; i < POINT_XY_COORD_SIZE; i++) {
		coords[POINT_XY_COORD_SIZE - 1 - i] = le_input_ptr[i];
	}
}

void GeometryPointDecompressFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 1);
	const auto count = args.size();

	UnifiedVectorFormat input_data;
	args.data[0].ToUnifiedFormat(input_data);
	const auto input = UnifiedVectorFormat::GetData<uhugeint_t>(input_data);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetDataMutable<string_t>(result);
	auto &result_mask = FlatVector::ValidityMutable(result);

	for (idx_t i = 0; i < count; i++) {
		const auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			result_mask.SetInvalid(i);
			continue;
		}
		auto blob = StringVector::EmptyString(result, POINT_XY_BLOB_SIZE);
		GeometryPointDecompress(input[idx], data_ptr_cast(blob.GetDataWriteable()));
		blob.Finalize();
		result_data[i] = blob;
	}
}

void CMGeometryPointCompressSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                      const BoundScalarFunction &function) {
	serializer.WriteProperty(100, "arguments", function.GetArguments());
	serializer.WriteProperty(101, "return_type", function.GetReturnType());
}

unique_ptr<FunctionData> CMGeometryPointCompressDeserialize(Deserializer &deserializer, BoundScalarFunction &function) {
	function.GetArguments() = deserializer.ReadProperty<vector<LogicalType>>(100, "arguments");
	function.SetReturnType(deserializer.ReadProperty<LogicalType>(101, "return_type"));
	function.SetFunctionCallback(GeometryPointCompressFunction);
	return nullptr;
}

void CMGeometryPointDecompressSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                        const BoundScalarFunction &function) {
	serializer.WriteProperty(100, "arguments", function.GetArguments());
	serializer.WriteProperty(101, "return_type", function.GetReturnType());
}

unique_ptr<FunctionData> CMGeometryPointDecompressDeserialize(Deserializer &deserializer,
                                                              BoundScalarFunction &function) {
	function.GetArguments() = deserializer.ReadProperty<vector<LogicalType>>(100, "arguments");
	function.SetReturnType(deserializer.ReadProperty<LogicalType>(101, "return_type"));
	function.SetFunctionCallback(GeometryPointDecompressFunction);
	return nullptr;
}

} // namespace

ScalarFunction CMGeometryPointCompressFun::GetFunction() {
	ScalarFunction result(Identifier("__internal_compress_geometry_point"), {LogicalType::GEOMETRY()},
	                      LogicalType::UHUGEINT, GeometryPointCompressFunction, CMUtils::Bind);
	result.GetSignature().GetParameter(0).SetName("value");
	result.SetSerializeCallback(CMGeometryPointCompressSerialize);
	result.SetDeserializeCallback(CMGeometryPointCompressDeserialize);
	result.SetErrorMode(FunctionErrors::CAN_THROW_RUNTIME_ERROR);
	return result;
}

ScalarFunction CMGeometryPointDecompressFun::GetFunction() {
	ScalarFunction result(Identifier("__internal_decompress_geometry_point"), {LogicalType::UHUGEINT},
	                      LogicalType::GEOMETRY(), GeometryPointDecompressFunction, CMUtils::Bind);
	result.GetSignature().GetParameter(0).SetName("value");
	result.SetSerializeCallback(CMGeometryPointDecompressSerialize);
	result.SetDeserializeCallback(CMGeometryPointDecompressDeserialize);
	result.SetErrorMode(FunctionErrors::CANNOT_ERROR);
	return result;
}

ScalarFunction InternalCompressGeometryPointFun::GetFunction() {
	return CMGeometryPointCompressFun::GetFunction();
}

ScalarFunction InternalDecompressGeometryPointFun::GetFunction() {
	return CMGeometryPointDecompressFun::GetFunction();
}

CMExpressionType CMUtils::GetGeometryType(const BoundScalarFunction &function) {
	if (function.GetDeserializeCallback() == CMGeometryPointCompressDeserialize) {
		return CMExpressionType::COMPRESS;
	}
	if (function.GetDeserializeCallback() == CMGeometryPointDecompressDeserialize) {
		return CMExpressionType::DECOMPRESS;
	}
	return CMExpressionType::NONE;
}

} // namespace duckdb
