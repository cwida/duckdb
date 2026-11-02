#include "json_geojson.hpp"

#include "duckdb/common/bswap.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "json_functions.hpp"

#include <cmath>
#include <limits>

namespace duckdb {

namespace {

//===--------------------------------------------------------------------===//
// WKB cursors
//===--------------------------------------------------------------------===//
class WKBReader {
public:
	WKBReader(const char *data, const idx_t size) : pos(data), end(data + size) {
	}

	template <class T>
	T Read() {
		if (pos + sizeof(T) > end) {
			throw InvalidInputException("Unexpected end of WKB data while converting to GeoJSON");
		}
		T value;
		memcpy(&value, pos, sizeof(T));
		pos += sizeof(T);
		return BSwapIfBE(value);
	}

private:
	const char *pos;
	const char *end;
};

class WKBWriter {
public:
	template <class T>
	void Write(const T &value) {
		const auto le_value = BSwapIfBE(value);
		const auto ptr = reinterpret_cast<const char *>(&le_value);
		buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
	}

	const vector<char> &GetBuffer() const {
		return buffer;
	}

private:
	vector<char> buffer;
};

//! Header of a single WKB geometry: the byte order byte plus the type/dimension word
struct WKBHeader {
	GeometryType type;
	bool has_z;
	bool has_m;
};

WKBHeader ReadHeader(WKBReader &reader) {
	const auto byte_order = reader.Read<uint8_t>();
	if (byte_order != 1) {
		throw InvalidInputException("Unsupported byte order %d in WKB", byte_order);
	}
	const auto meta = reader.Read<uint32_t>();
	const auto type_id = (meta & 0x0000FFFF) % 1000;
	const auto flag_id = (meta & 0x0000FFFF) / 1000;
	if (type_id < 1 || type_id > 7) {
		throw InvalidInputException("Unsupported geometry type %d in WKB", type_id);
	}
	if (flag_id > 3) {
		throw InvalidInputException("Unsupported geometry flag %d in WKB", flag_id);
	}
	WKBHeader header;
	header.type = static_cast<GeometryType>(type_id);
	header.has_z = (flag_id & 0x01) != 0;
	header.has_m = (flag_id & 0x02) != 0;
	return header;
}

void WriteHeader(WKBWriter &writer, const GeometryType type, const bool has_z) {
	writer.Write<uint8_t>(1);
	writer.Write<uint32_t>(static_cast<uint32_t>(type) + (has_z ? 1000 : 0));
}

//===--------------------------------------------------------------------===//
// GeoJSON type names
//===--------------------------------------------------------------------===//
const char *GeometryTypeToGeoJSONName(const GeometryType type) {
	switch (type) {
	case GeometryType::POINT:
		return "Point";
	case GeometryType::LINESTRING:
		return "LineString";
	case GeometryType::POLYGON:
		return "Polygon";
	case GeometryType::MULTIPOINT:
		return "MultiPoint";
	case GeometryType::MULTILINESTRING:
		return "MultiLineString";
	case GeometryType::MULTIPOLYGON:
		return "MultiPolygon";
	case GeometryType::GEOMETRYCOLLECTION:
		return "GeometryCollection";
	default:
		throw InvalidInputException("Unsupported geometry type in WKB");
	}
}

bool GeoJSONNameToGeometryType(const char *name, const size_t len, GeometryType &result) {
	for (idx_t i = 1; i <= 7; i++) {
		const auto candidate = static_cast<GeometryType>(i);
		const auto candidate_name = GeometryTypeToGeoJSONName(candidate);
		if (strlen(candidate_name) == len && memcmp(candidate_name, name, len) == 0) {
			result = candidate;
			return true;
		}
	}
	return false;
}

//===--------------------------------------------------------------------===//
// WKB -> GeoJSON
//===--------------------------------------------------------------------===//
struct Position {
	double x;
	double y;
	double z;
	//! An empty point is stored as all-NaN ordinates, and has no GeoJSON representation
	bool is_empty;
};

Position ReadPosition(WKBReader &reader, const WKBHeader &header) {
	Position result;
	result.x = reader.Read<double>();
	result.y = reader.Read<double>();
	result.z = header.has_z ? reader.Read<double>() : 0;
	if (header.has_m) {
		reader.Read<double>(); // GeoJSON has no M dimension
	}
	result.is_empty = std::isnan(result.x) && std::isnan(result.y);
	return result;
}

void AppendOrdinates(yyjson_mut_doc *doc, yyjson_mut_val *arr, const Position &pos, const bool has_z) {
	yyjson_mut_arr_add_real(doc, arr, pos.x);
	yyjson_mut_arr_add_real(doc, arr, pos.y);
	if (has_z) {
		yyjson_mut_arr_add_real(doc, arr, pos.z);
	}
}

//! Read a length-prefixed vertex run, appending one position array per vertex
void ReadVertexArray(WKBReader &reader, const WKBHeader &header, yyjson_mut_doc *doc, yyjson_mut_val *arr) {
	const auto vert_count = reader.Read<uint32_t>();
	for (uint32_t i = 0; i < vert_count; i++) {
		const auto pos = ReadPosition(reader, header);
		auto coord = yyjson_mut_arr(doc);
		AppendOrdinates(doc, coord, pos, header.has_z);
		yyjson_mut_arr_append(arr, coord);
	}
}

void ToGeoJSONRecursive(WKBReader &reader, yyjson_mut_doc *doc, yyjson_mut_val *obj, const idx_t depth) {
	if (depth == Geometry::MAX_RECURSION_DEPTH) {
		throw InvalidInputException("Geometry exceeds maximum recursion depth of %d", Geometry::MAX_RECURSION_DEPTH);
	}

	const auto header = ReadHeader(reader);
	yyjson_mut_obj_add_str(doc, obj, "type", GeometryTypeToGeoJSONName(header.type));

	if (header.type == GeometryType::GEOMETRYCOLLECTION) {
		auto geometries = yyjson_mut_arr(doc);
		yyjson_mut_obj_add_val(doc, obj, "geometries", geometries);
		const auto geom_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < geom_count; i++) {
			auto child = yyjson_mut_obj(doc);
			ToGeoJSONRecursive(reader, doc, child, depth + 1);
			yyjson_mut_arr_append(geometries, child);
		}
		return;
	}

	auto coords = yyjson_mut_arr(doc);
	yyjson_mut_obj_add_val(doc, obj, "coordinates", coords);

	switch (header.type) {
	case GeometryType::POINT: {
		const auto pos = ReadPosition(reader, header);
		if (!pos.is_empty) {
			AppendOrdinates(doc, coords, pos, header.has_z);
		}
	} break;
	case GeometryType::LINESTRING:
		ReadVertexArray(reader, header, doc, coords);
		break;
	case GeometryType::POLYGON: {
		const auto ring_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < ring_count; i++) {
			auto ring = yyjson_mut_arr(doc);
			ReadVertexArray(reader, header, doc, ring);
			yyjson_mut_arr_append(coords, ring);
		}
	} break;
	case GeometryType::MULTIPOINT: {
		// GeoJSON stores MultiPoint coordinates as a flat array of positions
		const auto part_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < part_count; i++) {
			const auto part = ReadHeader(reader);
			const auto pos = ReadPosition(reader, part);
			if (pos.is_empty) {
				continue;
			}
			auto coord = yyjson_mut_arr(doc);
			AppendOrdinates(doc, coord, pos, part.has_z);
			yyjson_mut_arr_append(coords, coord);
		}
	} break;
	case GeometryType::MULTILINESTRING: {
		const auto part_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < part_count; i++) {
			const auto part = ReadHeader(reader);
			auto line = yyjson_mut_arr(doc);
			ReadVertexArray(reader, part, doc, line);
			yyjson_mut_arr_append(coords, line);
		}
	} break;
	case GeometryType::MULTIPOLYGON: {
		const auto part_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < part_count; i++) {
			const auto part = ReadHeader(reader);
			auto poly = yyjson_mut_arr(doc);
			const auto ring_count = reader.Read<uint32_t>();
			for (uint32_t j = 0; j < ring_count; j++) {
				auto ring = yyjson_mut_arr(doc);
				ReadVertexArray(reader, part, doc, ring);
				yyjson_mut_arr_append(poly, ring);
			}
			yyjson_mut_arr_append(coords, poly);
		}
	} break;
	default:
		throw InvalidInputException("Unsupported geometry type in WKB");
	}
}

//===--------------------------------------------------------------------===//
// GeoJSON -> WKB
//===--------------------------------------------------------------------===//
//! A number, or a number kept as its text (YYJSON_READ_NUMBER_AS_RAW, when a column of the scan is a DECIMAL)
static bool TryGetOrdinate(yyjson_val *val, double &result) {
	if (yyjson_is_num(val)) {
		result = yyjson_get_num(val);
		return true;
	}
	yyjson_val number;
	if (yyjson_is_raw(val) && JSONCommon::ReadRawNumber(val, number)) {
		result = yyjson_get_num(&number);
		return true;
	}
	return false;
}

//! Whether any position in this coordinate tree carries a third ordinate
bool CoordinatesHaveZ(yyjson_val *coords, const idx_t depth) {
	if (depth == Geometry::MAX_RECURSION_DEPTH || !yyjson_is_arr(coords)) {
		return false;
	}
	size_t idx, max;
	yyjson_val *child;
	yyjson_arr_foreach(coords, idx, max, child) {
		if (yyjson_is_num(child) || yyjson_is_raw(child)) {
			// This array is a position rather than a nesting level
			return yyjson_arr_size(coords) >= 3;
		}
		if (CoordinatesHaveZ(child, depth + 1)) {
			return true;
		}
	}
	return false;
}

//! GeoJSON allows mixing 2D and 3D positions, but a geometry must have uniform dimensions, so a single
//! 3D position promotes the whole geometry to XYZ (missing ordinates are padded with 0)
bool GeometryHasZ(yyjson_val *val, const idx_t depth) {
	if (depth == Geometry::MAX_RECURSION_DEPTH || !yyjson_is_obj(val)) {
		return false;
	}
	auto geometries = yyjson_obj_get(val, "geometries");
	if (geometries && yyjson_is_arr(geometries)) {
		size_t idx, max;
		yyjson_val *child;
		yyjson_arr_foreach(geometries, idx, max, child) {
			if (GeometryHasZ(child, depth + 1)) {
				return true;
			}
		}
		return false;
	}
	return CoordinatesHaveZ(yyjson_obj_get(val, "coordinates"), depth);
}

void WritePosition(WKBWriter &writer, yyjson_val *pos, const bool has_z, const char *context) {
	if (!yyjson_is_arr(pos) || yyjson_arr_size(pos) < 2) {
		throw InvalidInputException("Invalid GeoJSON: %s position must be an array of at least 2 numbers", context);
	}
	const auto dims = has_z ? 3u : 2u;
	double ordinates[3] = {0, 0, 0};
	size_t idx, max;
	yyjson_val *child;
	yyjson_arr_foreach(pos, idx, max, child) {
		if (idx >= dims) {
			break;
		}
		if (!TryGetOrdinate(child, ordinates[idx])) {
			throw InvalidInputException("Invalid GeoJSON: %s position must contain only numbers", context);
		}
	}
	writer.Write(ordinates[0]);
	writer.Write(ordinates[1]);
	if (has_z) {
		writer.Write(ordinates[2]);
	}
}

void WriteVertexArray(WKBWriter &writer, yyjson_val *arr, const bool has_z, const char *context) {
	if (!yyjson_is_arr(arr)) {
		throw InvalidInputException("Invalid GeoJSON: %s coordinates are nested too shallowly", context);
	}
	writer.Write<uint32_t>(NumericCast<uint32_t>(yyjson_arr_size(arr)));
	size_t idx, max;
	yyjson_val *child;
	yyjson_arr_foreach(arr, idx, max, child) {
		WritePosition(writer, child, has_z, context);
	}
}

void FromGeoJSONRecursive(yyjson_val *val, WKBWriter &writer, const bool has_z, const idx_t depth) {
	if (depth == Geometry::MAX_RECURSION_DEPTH) {
		throw InvalidInputException("GeoJSON geometry exceeds maximum recursion depth of %d",
		                            Geometry::MAX_RECURSION_DEPTH);
	}
	if (!yyjson_is_obj(val)) {
		throw InvalidInputException("Invalid GeoJSON: geometry must be an object");
	}
	auto type_val = yyjson_obj_get(val, "type");
	if (!type_val || !yyjson_is_str(type_val)) {
		throw InvalidInputException("Invalid GeoJSON: geometry is missing a \"type\" string");
	}
	GeometryType type;
	if (!GeoJSONNameToGeometryType(yyjson_get_str(type_val), yyjson_get_len(type_val), type)) {
		throw InvalidInputException("Invalid GeoJSON: unknown geometry type \"%s\"", yyjson_get_str(type_val));
	}
	const auto context = GeometryTypeToGeoJSONName(type);
	WriteHeader(writer, type, has_z);

	if (type == GeometryType::GEOMETRYCOLLECTION) {
		auto geometries = yyjson_obj_get(val, "geometries");
		if (!geometries || !yyjson_is_arr(geometries)) {
			throw InvalidInputException("Invalid GeoJSON: GeometryCollection requires a \"geometries\" array");
		}
		writer.Write<uint32_t>(NumericCast<uint32_t>(yyjson_arr_size(geometries)));
		size_t idx, max;
		yyjson_val *child;
		yyjson_arr_foreach(geometries, idx, max, child) {
			FromGeoJSONRecursive(child, writer, has_z, depth + 1);
		}
		return;
	}

	auto coords = yyjson_obj_get(val, "coordinates");
	if (!coords || !yyjson_is_arr(coords)) {
		throw InvalidInputException("Invalid GeoJSON: %s requires a \"coordinates\" array", context);
	}

	size_t idx, max;
	yyjson_val *child;
	switch (type) {
	case GeometryType::POINT:
		if (yyjson_arr_size(coords) == 0) {
			// An empty point is stored as all-NaN ordinates
			const auto nan = std::numeric_limits<double>::quiet_NaN();
			writer.Write(nan);
			writer.Write(nan);
			if (has_z) {
				writer.Write(nan);
			}
		} else {
			WritePosition(writer, coords, has_z, context);
		}
		break;
	case GeometryType::LINESTRING:
		WriteVertexArray(writer, coords, has_z, context);
		break;
	case GeometryType::POLYGON:
		writer.Write<uint32_t>(NumericCast<uint32_t>(yyjson_arr_size(coords)));
		yyjson_arr_foreach(coords, idx, max, child) {
			WriteVertexArray(writer, child, has_z, context);
		}
		break;
	case GeometryType::MULTIPOINT:
		writer.Write<uint32_t>(NumericCast<uint32_t>(yyjson_arr_size(coords)));
		yyjson_arr_foreach(coords, idx, max, child) {
			WriteHeader(writer, GeometryType::POINT, has_z);
			WritePosition(writer, child, has_z, context);
		}
		break;
	case GeometryType::MULTILINESTRING:
		writer.Write<uint32_t>(NumericCast<uint32_t>(yyjson_arr_size(coords)));
		yyjson_arr_foreach(coords, idx, max, child) {
			WriteHeader(writer, GeometryType::LINESTRING, has_z);
			WriteVertexArray(writer, child, has_z, context);
		}
		break;
	case GeometryType::MULTIPOLYGON:
		writer.Write<uint32_t>(NumericCast<uint32_t>(yyjson_arr_size(coords)));
		yyjson_arr_foreach(coords, idx, max, child) {
			WriteHeader(writer, GeometryType::POLYGON, has_z);
			if (!yyjson_is_arr(child)) {
				throw InvalidInputException("Invalid GeoJSON: %s coordinates are nested too shallowly", context);
			}
			writer.Write<uint32_t>(NumericCast<uint32_t>(yyjson_arr_size(child)));
			size_t ring_idx, ring_max;
			yyjson_val *ring;
			yyjson_arr_foreach(child, ring_idx, ring_max, ring) {
				WriteVertexArray(writer, ring, has_z, context);
			}
		}
		break;
	default:
		throw InvalidInputException("Invalid GeoJSON: unsupported geometry type \"%s\"", context);
	}
}

} // namespace

//===--------------------------------------------------------------------===//
// Public interface
//===--------------------------------------------------------------------===//
yyjson_mut_val *JSONGeometry::ToGeoJSON(const string_t &wkb, yyjson_mut_doc *doc) {
	WKBReader reader(wkb.GetData(), wkb.GetSize());
	auto obj = yyjson_mut_obj(doc);
	ToGeoJSONRecursive(reader, doc, obj, 0);
	return obj;
}

bool JSONGeometry::FromGeoJSON(yyjson_val *val, string_t &result, Vector &result_vector, string &error) {
	WKBWriter writer;
	try {
		FromGeoJSONRecursive(val, writer, GeometryHasZ(val, 0), 0);
		const auto &buffer = writer.GetBuffer();
		const string_t wkb(buffer.data(), NumericCast<uint32_t>(buffer.size()));
		// Let the core geometry code validate and normalize what we produced
		return Geometry::FromBinary(wkb, result, StringVector::GetStringHeap(result_vector), true);
	} catch (std::exception &ex) {
		ErrorData error_data(ex);
		error = error_data.RawMessage();
		return false;
	}
}

bool JSONGeometry::IsGeoJSONGeometry(yyjson_val *val) {
	if (!yyjson_is_obj(val)) {
		return false;
	}
	auto type_val = yyjson_obj_get(val, "type");
	if (!type_val || !yyjson_is_str(type_val)) {
		return false;
	}
	GeometryType type;
	if (!GeoJSONNameToGeometryType(yyjson_get_str(type_val), yyjson_get_len(type_val), type)) {
		return false;
	}
	const auto member = yyjson_obj_get(val, type == GeometryType::GEOMETRYCOLLECTION ? "geometries" : "coordinates");
	return member && yyjson_is_arr(member);
}

//===--------------------------------------------------------------------===//
// Scalar functions
//===--------------------------------------------------------------------===//
static void AsGeoJSONFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();
	auto doc = JSONCommon::CreateDocument(alc);

	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &wkb) {
		return JSONCommon::WriteVal<yyjson_mut_val>(JSONGeometry::ToGeoJSON(wkb, doc), alc);
	});

	JSONAllocator::AddBuffer(result, alc);
}

static void GeomFromGeoJSONFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();

	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](const string_t &input) {
		auto doc = JSONCommon::ReadDocument(input, JSONCommon::READ_FLAG, alc);
		string_t geom;
		string error;
		if (!JSONGeometry::FromGeoJSON(doc->root, geom, result, error)) {
			throw InvalidInputException(error);
		}
		return geom;
	});
}

ScalarFunctionSet JSONFunctions::GetAsGeoJSONFunction() {
	ScalarFunctionSet set("st_asgeojson");
	ScalarFunction fun({}, LogicalType::JSON(), AsGeoJSONFunction, nullptr, nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("geom", LogicalType::GEOMETRY());
	fun.SetFallible();
	set.AddFunction(fun);
	return set;
}

ScalarFunctionSet JSONFunctions::GetGeomFromGeoJSONFunction() {
	ScalarFunctionSet set("st_geomfromgeojson");
	for (const auto &input_type : vector<LogicalType> {LogicalType::VARCHAR, LogicalType::JSON()}) {
		ScalarFunction fun({}, LogicalType::GEOMETRY(), GeomFromGeoJSONFunction, nullptr, nullptr,
		                   JSONFunctionLocalState::Init);
		fun.GetSignature().AddParameter("geojson", input_type);
		fun.SetFallible();
		set.AddFunction(fun);
	}
	return set;
}

} // namespace duckdb
