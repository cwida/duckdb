#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "fast_float/fast_float.h"
#include "fmt/format.h"

//----------------------------------------------------------------------------------------------------------------------
// Internals
//----------------------------------------------------------------------------------------------------------------------
namespace duckdb {

static inline VertexXY BSwap(const VertexXY &v) {
	VertexXY result;
	result.x = BSwap(v.x);
	result.y = BSwap(v.y);
	return result;
}

static inline VertexXYZ BSwap(const VertexXYZ &v) {
	VertexXYZ result;
	result.x = BSwap(v.x);
	result.y = BSwap(v.y);
	result.z = BSwap(v.z);
	return result;
}

static inline VertexXYM BSwap(const VertexXYM &v) {
	VertexXYM result;
	result.x = BSwap(v.x);
	result.y = BSwap(v.y);
	result.m = BSwap(v.m);
	return result;
}

static inline VertexXYZM BSwap(const VertexXYZM &v) {
	VertexXYZM result;
	result.x = BSwap(v.x);
	result.y = BSwap(v.y);
	result.z = BSwap(v.z);
	result.m = BSwap(v.m);
	return result;
}

namespace {

class BlobWriter {
public:
	template <class T>
	void Write(const T &value) {
		auto le_value = BSwapIfBE(value);
		auto ptr = reinterpret_cast<const char *>(&le_value);
		buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
	}

	template <class T>
	struct Reserved {
		size_t offset;
		T value;
	};

	template <class T>
	Reserved<T> Reserve() {
		auto offset = buffer.size();
		buffer.resize(buffer.size() + sizeof(T));
		return {offset, T()};
	}

	template <class T>
	void Write(const Reserved<T> &reserved) {
		if (reserved.offset + sizeof(T) > buffer.size()) {
			throw InternalException("Write out of bounds in BinaryWriter");
		}
		auto le_value = BSwapIfBE(reserved.value);
		auto ptr = reinterpret_cast<const char *>(&le_value);
		// We've reserved 0 bytes, so we can safely memcpy
		memcpy(buffer.data() + reserved.offset, ptr, sizeof(T));
	}

	const vector<char> &GetBuffer() const {
		return buffer;
	}

	void Clear() {
		buffer.clear();
	}

private:
	vector<char> buffer;
};

class FixedSizeBlobWriter {
public:
	FixedSizeBlobWriter(char *data, uint32_t size) : beg(data), pos(data), end(data + size) {
	}

	template <class T>
	void Write(const T &value) {
		if (pos + sizeof(T) > end) {
			throw InvalidInputException("Writing beyond end of binary data at position %zu", pos - beg);
		}
		auto le_value = BSwapIfBE(value);
		memcpy(pos, &le_value, sizeof(T));
		pos += sizeof(T);
	}

	void Write(const char *data, size_t size) {
		if (size == 0) {
			return; // nothing to write
		}
		if (pos + size > end) {
			throw InvalidInputException("Writing beyond end of binary data at position %zu", pos - beg);
		}
		memcpy(pos, data, size);
		pos += size;
	}

	void Skip(size_t size) {
		if (pos + size > end) {
			throw InvalidInputException("Skipping beyond end of binary data at position %zu", pos - beg);
		}
		pos += size;
	}

	size_t GetPosition() const {
		return static_cast<idx_t>(pos - beg);
	}

private:
	const char *beg;
	char *pos;
	const char *end;
};

class BlobReader {
public:
	BlobReader(const char *data, uint32_t size) : beg(data), pos(data), end(data + size) {
	}

	template <class T>
	T Read(const bool le) {
		if (le) {
			return Read<T, true>();
		} else {
			return Read<T, false>();
		}
	}

	template <class T, bool LE = true>
	T Read() {
		if (pos + sizeof(T) > end) {
			throw InvalidInputException("Unexpected end of binary data at position %zu", pos - beg);
		}
		T value;
		memcpy(&value, pos, sizeof(T));
		value = LE ? BSwapIfBE(value) : BSwapIfLE(value);
		pos += sizeof(T);
		return value;
	}

	void Skip(size_t size) {
		if (pos + size > end) {
			throw InvalidInputException("Skipping beyond end of binary data at position %zu", pos - beg);
		}
		pos += size;
	}

	const char *Reserve(size_t size) {
		if (pos + size > end) {
			throw InvalidInputException("Reserving beyond end of binary data at position %zu", pos - beg);
		}
		auto current_pos = pos;
		pos += size;
		return current_pos;
	}

	size_t GetPosition() const {
		return static_cast<idx_t>(pos - beg);
	}

	const char *GetDataPtr() const {
		return pos;
	}

	bool IsAtEnd() const {
		return pos >= end;
	}

	void Reset() {
		pos = beg;
	}

private:
	const char *beg;
	const char *pos;
	const char *end;
};

class TextWriter {
public:
	void Write(const char *str) {
		buffer.insert(buffer.end(), str, str + strlen(str));
	}
	void Write(char c) {
		buffer.push_back(c);
	}
	void Write(double value) {
		auto start = buffer.size();
		duckdb_fmt::format_to(std::back_inserter(buffer), "{}", value);
		// Remove trailing ".0" (e.g. "10.0" -> "10"), but only when the number
		// is in fixed notation. Scientific notation like "1e+20" must not be
		// touched — stripping the trailing '0' would corrupt the exponent.
		bool has_exponent = false;
		for (auto i = start; i < buffer.size(); i++) {
			if (buffer[i] == 'e' || buffer[i] == 'E') {
				has_exponent = true;
				break;
			}
		}
		if (!has_exponent && buffer.back() == '0') {
			buffer.pop_back();
			if (buffer.back() == '.') {
				buffer.pop_back();
			}
		}
	}
	const vector<char> &GetBuffer() const {
		return buffer;
	}

private:
	vector<char> buffer;
};

class TextReader {
public:
	TextReader(const char *text, const uint32_t size) : beg(text), pos(text), end(text + size) {
	}

	bool TryMatch(const char *str) {
		auto ptr = pos;
		while (*str && pos < end && StringUtil::CharacterToLower(*pos) == StringUtil::CharacterToLower(*str)) {
			pos++;
			str++;
		}
		if (*str == '\0') {
			SkipWhitespace(); // remove trailing whitespace
			return true;      // matched
		}
		pos = ptr;    // reset position
		return false; // not matched
	}

	bool TryMatch(char c) {
		if (pos < end && StringUtil::CharacterToLower(*pos) == StringUtil::CharacterToLower(c)) {
			pos++;
			SkipWhitespace(); // remove trailing whitespace
			return true;      // matched
		}
		return false; // not matched
	}

	bool Match(const char *str) {
		if (TryMatch(str)) {
			return true;
		}
		// Check if this would go EOF
		if (pos + strlen(str) >= end) {
			return SetError("Expected '%s' but got end of input", str);
		}
		return SetError("Expected '%s' but got '%c'", str, *pos);
	}

	bool Match(char c) {
		if (TryMatch(c)) {
			return true;
		}
		if (pos >= end) {
			return SetError("Expected '%c' but got end of input", c);
		}
		return SetError("Expected '%c' but got '%c'", c, *pos);
	}

	bool MatchNumber(double &num) {
		// Now use fast_float to parse the number
		const auto res = duckdb_fast_float::from_chars(pos, end, num);
		if (res.ec != std::errc()) {
			return SetError("Expected number");
		}

		pos = res.ptr; // update position to the end of the parsed number

		SkipWhitespace(); // remove trailing whitespace
		return true;
	}

	idx_t GetPosition() const {
		return static_cast<idx_t>(pos - beg);
	}

	bool IsAtEnd() {
		SkipWhitespace();
		return pos >= end;
	}

	void Reset() {
		pos = beg;
	}

	//! Records a parse error at the current position, always returns false
	template <class... ARGS>
	bool SetError(const char *raw_msg, ARGS... args) {
		error_message = StringUtil::Format(raw_msg, args...);
		error_offset = UnsafeNumericCast<idx_t>(pos - beg);
		return false;
	}

	string GetErrorMessage() const {
		return StringUtil::Format("Failed to parse geometry: %s at offset %lu", error_message, error_offset);
	}

	InvalidInputException MakeError() const {
		if (query_location.IsValid()) {
			// point at the specific byte within the WKT literal where parsing failed
			const QueryLocation expr_location(query_location.Start() + error_offset, 0);
			return InvalidInputException(Exception::InitializeExtraInfo(expr_location), GetErrorMessage());
		}
		return InvalidInputException(GetErrorMessage());
	}

	void SetQueryLocation(QueryLocation location) {
		query_location = location;
	}

	void SkipWhitespace() {
		while (pos < end && StringUtil::CharacterIsSpace(*pos)) {
			pos++;
		}
	}

private:
	const char *beg;
	const char *pos;
	const char *end;
	QueryLocation query_location;
	string error_message;
	idx_t error_offset = 0;
};

//! Returns false if the text is not valid WKT, the error is recorded in the reader
bool FromStringRecursive(TextReader &reader, BlobWriter &writer, uint32_t depth, bool parent_has_z, bool parent_has_m) {
	if (depth == Geometry::MAX_RECURSION_DEPTH) {
		return reader.SetError("Geometry string exceeds maximum recursion depth of %d", Geometry::MAX_RECURSION_DEPTH);
	}

	// Skip leading whitespace
	reader.SkipWhitespace();

	// EWKT dialect (ignore SRID if present)
	if (reader.TryMatch("SRID")) {
		if (!reader.Match('=')) {
			return false;
		}
		double srid;
		if (!reader.MatchNumber(srid)) {
			return false;
		}
		if (!reader.Match(';')) {
			return false;
		}
	}

	GeometryType type = GeometryType::INVALID;

	if (reader.TryMatch("point")) {
		type = GeometryType::POINT;
	} else if (reader.TryMatch("linestring")) {
		type = GeometryType::LINESTRING;
	} else if (reader.TryMatch("polygon")) {
		type = GeometryType::POLYGON;
	} else if (reader.TryMatch("multipoint")) {
		type = GeometryType::MULTIPOINT;
	} else if (reader.TryMatch("multilinestring")) {
		type = GeometryType::MULTILINESTRING;
	} else if (reader.TryMatch("multipolygon")) {
		type = GeometryType::MULTIPOLYGON;
	} else if (reader.TryMatch("geometrycollection")) {
		type = GeometryType::GEOMETRYCOLLECTION;
	} else {
		return reader.SetError("Unknown geometry type");
	}

	const auto has_z = reader.TryMatch("z");
	const auto has_m = reader.TryMatch("m");

	const auto is_empty = reader.TryMatch("empty");

	if ((depth != 0) && ((parent_has_z != has_z) || (parent_has_m != has_m))) {
		return reader.SetError("Geometry has inconsistent Z/M dimensions");
	}

	// How many dimensions does this geometry have?
	const uint32_t dims = 2 + (has_z ? 1 : 0) + (has_m ? 1 : 0);

	// WKB type
	const auto meta = static_cast<uint32_t>(type) + (has_z ? 1000 : 0) + (has_m ? 2000 : 0);
	// Write the geometry type and vertex type
	writer.Write<uint8_t>(1); // LE Byte Order
	writer.Write<uint32_t>(meta);

	switch (type) {
	case GeometryType::POINT: {
		if (is_empty) {
			for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
				// Write NaN for each dimension, if point is empty
				writer.Write<double>(std::numeric_limits<double>::quiet_NaN());
			}
		} else {
			if (!reader.Match('(')) {
				return false;
			}
			for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
				double value;
				if (!reader.MatchNumber(value)) {
					return false;
				}
				writer.Write<double>(value);
			}
			if (!reader.Match(')')) {
				return false;
			}
		}
	} break;
	case GeometryType::LINESTRING: {
		if (is_empty) {
			writer.Write<uint32_t>(0); // No vertices in empty linestring
			break;
		}
		auto vert_count = writer.Reserve<uint32_t>();
		if (!reader.Match('(')) {
			return false;
		}
		do {
			for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
				double value;
				if (!reader.MatchNumber(value)) {
					return false;
				}
				writer.Write<double>(value);
			}
			vert_count.value++;
		} while (reader.TryMatch(','));
		if (!reader.Match(')')) {
			return false;
		}
		writer.Write(vert_count);
	} break;
	case GeometryType::POLYGON: {
		if (is_empty) {
			writer.Write<uint32_t>(0);
			break; // No rings in empty polygon
		}
		auto ring_count = writer.Reserve<uint32_t>();
		if (!reader.Match('(')) {
			return false;
		}
		do {
			auto vert_count = writer.Reserve<uint32_t>();
			if (!reader.Match('(')) {
				return false;
			}
			do {
				for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
					double value;
					if (!reader.MatchNumber(value)) {
						return false;
					}
					writer.Write<double>(value);
				}
				vert_count.value++;
			} while (reader.TryMatch(','));
			if (!reader.Match(')')) {
				return false;
			}
			writer.Write(vert_count);
			ring_count.value++;
		} while (reader.TryMatch(','));
		if (!reader.Match(')')) {
			return false;
		}
		writer.Write(ring_count);
	} break;
	case GeometryType::MULTIPOINT: {
		if (is_empty) {
			writer.Write<uint32_t>(0); // No points in empty multipoint
			break;
		}
		auto part_count = writer.Reserve<uint32_t>();
		if (!reader.Match('(')) {
			return false;
		}
		do {
			bool has_paren = reader.TryMatch('(');

			const auto part_meta = static_cast<uint32_t>(GeometryType::POINT) + (has_z ? 1000 : 0) + (has_m ? 2000 : 0);
			writer.Write<uint8_t>(1);
			writer.Write<uint32_t>(part_meta);

			if (reader.TryMatch("EMPTY")) {
				for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
					// Write NaN for each dimension, if point is empty
					writer.Write<double>(std::numeric_limits<double>::quiet_NaN());
				}
			} else {
				for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
					double value;
					if (!reader.MatchNumber(value)) {
						return false;
					}
					writer.Write<double>(value);
				}
			}
			if (has_paren) {
				if (!reader.Match(')')) { // Match the closing parenthesis if it was opened
					return false;
				}
			}
			part_count.value++;
		} while (reader.TryMatch(','));
		if (!reader.Match(')')) {
			return false;
		}
		writer.Write(part_count);
	} break;
	case GeometryType::MULTILINESTRING: {
		if (is_empty) {
			writer.Write<uint32_t>(0);
			return true; // No linestrings in empty multilinestring
		}
		auto part_count = writer.Reserve<uint32_t>();
		if (!reader.Match('(')) {
			return false;
		}
		do {
			const auto part_meta =
			    static_cast<uint32_t>(GeometryType::LINESTRING) + (has_z ? 1000 : 0) + (has_m ? 2000 : 0);
			writer.Write<uint8_t>(1);
			writer.Write<uint32_t>(part_meta);

			if (reader.TryMatch("EMPTY")) {
				writer.Write<uint32_t>(0); // No vertices in empty linestring
				part_count.value++;
			} else {
				auto vert_count = writer.Reserve<uint32_t>();
				if (!reader.Match('(')) {
					return false;
				}
				do {
					for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
						double value;
						if (!reader.MatchNumber(value)) {
							return false;
						}
						writer.Write<double>(value);
					}
					vert_count.value++;
				} while (reader.TryMatch(','));
				if (!reader.Match(')')) {
					return false;
				}
				writer.Write(vert_count);
				part_count.value++;
			}
		} while (reader.TryMatch(','));
		if (!reader.Match(')')) {
			return false;
		}
		writer.Write(part_count);
	} break;
	case GeometryType::MULTIPOLYGON: {
		if (is_empty) {
			writer.Write<uint32_t>(0); // No polygons in empty multipolygon
			break;
		}
		auto part_count = writer.Reserve<uint32_t>();
		if (!reader.Match('(')) {
			return false;
		}
		do {
			const auto part_meta =
			    static_cast<uint32_t>(GeometryType::POLYGON) + (has_z ? 1000 : 0) + (has_m ? 2000 : 0);
			writer.Write<uint8_t>(1);
			writer.Write<uint32_t>(part_meta);

			if (reader.TryMatch("EMPTY")) {
				writer.Write<uint32_t>(0); // No rings in empty polygon
				part_count.value++;
			} else {
				auto ring_count = writer.Reserve<uint32_t>();
				if (!reader.Match('(')) {
					return false;
				}
				do {
					auto vert_count = writer.Reserve<uint32_t>();
					if (!reader.Match('(')) {
						return false;
					}
					do {
						for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
							double value;
							if (!reader.MatchNumber(value)) {
								return false;
							}
							writer.Write<double>(value);
						}
						vert_count.value++;
					} while (reader.TryMatch(','));
					if (!reader.Match(')')) {
						return false;
					}
					writer.Write(vert_count);
					ring_count.value++;
				} while (reader.TryMatch(','));
				if (!reader.Match(')')) {
					return false;
				}
				writer.Write(ring_count);
				part_count.value++;
			}
		} while (reader.TryMatch(','));
		if (!reader.Match(')')) {
			return false;
		}
		writer.Write(part_count);
	} break;
	case GeometryType::GEOMETRYCOLLECTION: {
		if (is_empty) {
			writer.Write<uint32_t>(0); // No geometries in empty geometry collection
			break;
		}
		auto part_count = writer.Reserve<uint32_t>();
		if (!reader.Match('(')) {
			return false;
		}
		do {
			// Recursively parse the geometry inside the collection
			if (!FromStringRecursive(reader, writer, depth + 1, has_z, has_m)) {
				return false;
			}
			part_count.value++;
		} while (reader.TryMatch(','));
		if (!reader.Match(')')) {
			return false;
		}
		writer.Write(part_count);
	} break;
	default:
		return reader.SetError("Unknown geometry type %d", static_cast<int>(type));
	}
	return true;
}

void ToStringRecursive(BlobReader &reader, TextWriter &writer, idx_t depth, bool parent_has_z, bool parent_has_m) {
	if (depth == Geometry::MAX_RECURSION_DEPTH) {
		throw InvalidInputException("Geometry exceeds maximum recursion depth of %d", Geometry::MAX_RECURSION_DEPTH);
	}

	// Read the byte order (should always be 1 for little-endian)
	auto byte_order = reader.Read<uint8_t>();
	if (byte_order != 1) {
		throw InvalidInputException("Unsupported byte order %d in WKB", byte_order);
	}

	const auto meta = reader.Read<uint32_t>();
	const auto type = static_cast<GeometryType>((meta & 0x0000FFFF) % 1000);
	const auto flag = (meta & 0x0000FFFF) / 1000;
	const auto has_z = (flag & 0x01) != 0;
	const auto has_m = (flag & 0x02) != 0;

	if ((depth != 0) && ((parent_has_z != has_z) || (parent_has_m != has_m))) {
		throw InvalidInputException("Geometry has inconsistent Z/M dimensions, starting at position %zu",
		                            reader.GetPosition());
	}

	const uint32_t dims = 2 + (has_z ? 1 : 0) + (has_m ? 1 : 0);
	const auto flag_str = has_z ? (has_m ? " ZM " : " Z ") : (has_m ? " M " : " ");

	switch (type) {
	case GeometryType::POINT: {
		writer.Write("POINT");
		writer.Write(flag_str);

		double vert[4] = {0, 0, 0, 0};
		auto all_nan = true;
		for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
			vert[d_idx] = reader.Read<double>();
			all_nan &= std::isnan(vert[d_idx]);
		}
		if (all_nan) {
			writer.Write("EMPTY");
			return;
		}
		writer.Write('(');
		for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
			if (d_idx > 0) {
				writer.Write(' ');
			}
			writer.Write(vert[d_idx]);
		}
		writer.Write(')');
	} break;
	case GeometryType::LINESTRING: {
		writer.Write("LINESTRING");
		;
		writer.Write(flag_str);
		const auto vert_count = reader.Read<uint32_t>();
		if (vert_count == 0) {
			writer.Write("EMPTY");
			return;
		}
		writer.Write('(');
		for (uint32_t vert_idx = 0; vert_idx < vert_count; vert_idx++) {
			if (vert_idx > 0) {
				writer.Write(", ");
			}
			for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
				if (d_idx > 0) {
					writer.Write(' ');
				}
				auto value = reader.Read<double>();
				writer.Write(value);
			}
		}
		writer.Write(')');
	} break;
	case GeometryType::POLYGON: {
		writer.Write("POLYGON");
		writer.Write(flag_str);
		const auto ring_count = reader.Read<uint32_t>();
		if (ring_count == 0) {
			writer.Write("EMPTY");
			return;
		}
		writer.Write('(');
		for (uint32_t ring_idx = 0; ring_idx < ring_count; ring_idx++) {
			if (ring_idx > 0) {
				writer.Write(", ");
			}
			const auto vert_count = reader.Read<uint32_t>();
			if (vert_count == 0) {
				writer.Write("EMPTY");
				continue;
			}
			writer.Write('(');
			for (uint32_t vert_idx = 0; vert_idx < vert_count; vert_idx++) {
				if (vert_idx > 0) {
					writer.Write(", ");
				}
				for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
					if (d_idx > 0) {
						writer.Write(' ');
					}
					auto value = reader.Read<double>();
					writer.Write(value);
				}
			}
			writer.Write(')');
		}
		writer.Write(')');
	} break;
	case GeometryType::MULTIPOINT: {
		writer.Write("MULTIPOINT");
		writer.Write(flag_str);
		const auto part_count = reader.Read<uint32_t>();
		if (part_count == 0) {
			writer.Write("EMPTY");
			return;
		}
		writer.Write('(');
		for (uint32_t part_idx = 0; part_idx < part_count; part_idx++) {
			const auto part_byte_order = reader.Read<uint8_t>();
			if (part_byte_order != 1) {
				throw InvalidInputException("Unsupported byte order %d in WKB", part_byte_order);
			}
			const auto part_meta = reader.Read<uint32_t>();
			const auto part_type = static_cast<GeometryType>((part_meta & 0x0000FFFF) % 1000);
			const auto part_flag = (part_meta & 0x0000FFFF) / 1000;
			const auto part_has_z = (part_flag & 0x01) != 0;
			const auto part_has_m = (part_flag & 0x02) != 0;

			if (part_type != GeometryType::POINT) {
				throw InvalidInputException("Expected POINT in MULTIPOINT but got %d", static_cast<int>(part_type));
			}

			if ((has_z != part_has_z) || (has_m != part_has_m)) {
				throw InvalidInputException(
				    "Geometry has inconsistent Z/M dimensions in MULTIPOINT, starting at position %zu",
				    reader.GetPosition());
			}
			if (part_idx > 0) {
				writer.Write(", ");
			}
			double vert[4] = {0, 0, 0, 0};
			auto all_nan = true;
			for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
				vert[d_idx] = reader.Read<double>();
				all_nan &= std::isnan(vert[d_idx]);
			}
			if (all_nan) {
				writer.Write("EMPTY");
				continue;
			}
			// writer.Write('(');
			for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
				if (d_idx > 0) {
					writer.Write(' ');
				}
				writer.Write(vert[d_idx]);
			}
			// writer.Write(')');
		}
		writer.Write(')');

	} break;
	case GeometryType::MULTILINESTRING: {
		writer.Write("MULTILINESTRING");
		writer.Write(flag_str);
		const auto part_count = reader.Read<uint32_t>();
		if (part_count == 0) {
			writer.Write("EMPTY");
			return;
		}
		writer.Write('(');
		for (uint32_t part_idx = 0; part_idx < part_count; part_idx++) {
			const auto part_byte_order = reader.Read<uint8_t>();
			if (part_byte_order != 1) {
				throw InvalidInputException("Unsupported byte order %d in WKB", part_byte_order);
			}
			const auto part_meta = reader.Read<uint32_t>();
			const auto part_type = static_cast<GeometryType>((part_meta & 0x0000FFFF) % 1000);
			const auto part_flag = (part_meta & 0x0000FFFF) / 1000;
			const auto part_has_z = (part_flag & 0x01) != 0;
			const auto part_has_m = (part_flag & 0x02) != 0;

			if (part_type != GeometryType::LINESTRING) {
				throw InvalidInputException("Expected LINESTRING in MULTILINESTRING but got %d",
				                            static_cast<int>(part_type));
			}
			if ((has_z != part_has_z) || (has_m != part_has_m)) {
				throw InvalidInputException(
				    "Geometry has inconsistent Z/M dimensions in MULTILINESTRING, starting at position %zu",
				    reader.GetPosition());
			}
			if (part_idx > 0) {
				writer.Write(", ");
			}
			const auto vert_count = reader.Read<uint32_t>();
			if (vert_count == 0) {
				writer.Write("EMPTY");
				continue;
			}
			writer.Write('(');
			for (uint32_t vert_idx = 0; vert_idx < vert_count; vert_idx++) {
				if (vert_idx > 0) {
					writer.Write(", ");
				}
				for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
					if (d_idx > 0) {
						writer.Write(' ');
					}
					auto value = reader.Read<double>();
					writer.Write(value);
				}
			}
			writer.Write(')');
		}
		writer.Write(')');
	} break;
	case GeometryType::MULTIPOLYGON: {
		writer.Write("MULTIPOLYGON");
		writer.Write(flag_str);
		const auto part_count = reader.Read<uint32_t>();
		if (part_count == 0) {
			writer.Write("EMPTY");
			return;
		}
		writer.Write('(');
		for (uint32_t part_idx = 0; part_idx < part_count; part_idx++) {
			if (part_idx > 0) {
				writer.Write(", ");
			}

			const auto part_byte_order = reader.Read<uint8_t>();
			if (part_byte_order != 1) {
				throw InvalidInputException("Unsupported byte order %d in WKB", part_byte_order);
			}
			const auto part_meta = reader.Read<uint32_t>();
			const auto part_type = static_cast<GeometryType>((part_meta & 0x0000FFFF) % 1000);
			const auto part_flag = (part_meta & 0x0000FFFF) / 1000;
			const auto part_has_z = (part_flag & 0x01) != 0;
			const auto part_has_m = (part_flag & 0x02) != 0;
			if (part_type != GeometryType::POLYGON) {
				throw InvalidInputException("Expected POLYGON in MULTIPOLYGON but got %d", static_cast<int>(part_type));
			}
			if ((has_z != part_has_z) || (has_m != part_has_m)) {
				throw InvalidInputException(
				    "Geometry has inconsistent Z/M dimensions in MULTIPOLYGON, starting at position %zu",
				    reader.GetPosition());
			}

			const auto ring_count = reader.Read<uint32_t>();
			if (ring_count == 0) {
				writer.Write("EMPTY");
				continue;
			}
			writer.Write('(');
			for (uint32_t ring_idx = 0; ring_idx < ring_count; ring_idx++) {
				if (ring_idx > 0) {
					writer.Write(", ");
				}
				const auto vert_count = reader.Read<uint32_t>();
				if (vert_count == 0) {
					writer.Write("EMPTY");
					continue;
				}
				writer.Write('(');
				for (uint32_t vert_idx = 0; vert_idx < vert_count; vert_idx++) {
					if (vert_idx > 0) {
						writer.Write(", ");
					}
					for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
						if (d_idx > 0) {
							writer.Write(' ');
						}
						auto value = reader.Read<double>();
						writer.Write(value);
					}
				}
				writer.Write(')');
			}
			writer.Write(')');
		}
		writer.Write(')');
	} break;
	case GeometryType::GEOMETRYCOLLECTION: {
		writer.Write("GEOMETRYCOLLECTION");
		writer.Write(flag_str);
		const auto part_count = reader.Read<uint32_t>();
		if (part_count == 0) {
			writer.Write("EMPTY");
			return;
		}
		writer.Write('(');
		for (uint32_t part_idx = 0; part_idx < part_count; part_idx++) {
			if (part_idx > 0) {
				writer.Write(", ");
			}
			// Recursively parse the geometry inside the collection
			ToStringRecursive(reader, writer, depth + 1, has_z, has_m);
		}
		writer.Write(')');
	} break;
	default:
		throw InvalidInputException("Unsupported geometry type %d in WKB", static_cast<int>(type));
	}
}

struct WKBAnalysis {
	uint32_t size = 0;
	bool any_be = false;
	bool any_z = false;
	bool any_m = false;
	bool any_unknown = false;
	bool any_ewkb = false;
	bool has_trailing_data = false;
};

//! A collection whose parts are being analyzed
struct WKBAnalysisFrame {
	//! The number of parts that remain to be analyzed
	uint32_t parts_remaining;
	//! The type the parts must have, or 0 if they can have any type
	uint32_t part_type;
};

WKBAnalysis AnalyzeWKB(BlobReader &reader) {
	WKBAnalysis result;
	// Collection children are complete WKB objects embedded in the root object.
	vector<WKBAnalysisFrame> collections;
	bool is_root = true;
	bool root_has_z = false;
	bool root_has_m = false;

	while (is_root || !collections.empty()) {
		uint32_t required_type = 0;
		if (!is_root) {
			auto &collection = collections.back();
			if (collection.parts_remaining == 0) {
				collections.pop_back();
				continue;
			}
			collection.parts_remaining--;
			required_type = collection.part_type;
			if (collections.size() >= Geometry::MAX_RECURSION_DEPTH) {
				throw InvalidInputException("Geometry exceeds maximum recursion depth of %d",
				                            Geometry::MAX_RECURSION_DEPTH);
			}
		}
		const auto le = reader.Read<uint8_t>() == 1;

		const auto meta = reader.Read<uint32_t>(le);
		const auto type_id = (meta & 0x0000FFFF) % 1000;
		const auto flag_id = (meta & 0x0000FFFF) / 1000;
		if (flag_id > 3) {
			throw InvalidInputException("Unsupported geometry flag %d in WKB", flag_id);
		}

		// Extended WKB detection
		const auto has_extz = (meta & 0x80000000) != 0;
		const auto has_extm = (meta & 0x40000000) != 0;
		const auto has_srid = (meta & 0x20000000) != 0;

		const auto has_z = ((flag_id & 0x01) != 0) || has_extz;
		const auto has_m = ((flag_id & 0x02) != 0) || has_extm;

		if (is_root) {
			root_has_z = has_z;
			root_has_m = has_m;
			is_root = false;
		} else {
			if (has_z != root_has_z || has_m != root_has_m) {
				throw InvalidInputException("Geometry has inconsistent Z/M dimensions, starting at position %zu",
				                            reader.GetPosition());
			}
			if (required_type != 0 && type_id != required_type) {
				throw InvalidInputException("Unexpected geometry type %d in a collection of type %d at position %zu",
				                            type_id, required_type + 3, reader.GetPosition());
			}
		}

		if (has_srid) {
			result.any_ewkb = true;
			reader.Skip(sizeof(uint32_t)); // Skip SRID
			                               // Do not include SRID in the size
		}

		if (has_extz || has_extm || has_srid) {
			// EWKB flags are set
			result.any_ewkb = true;
		}

		const auto v_size = (2 + (has_z ? 1 : 0) + (has_m ? 1 : 0)) * sizeof(double);

		result.any_z |= has_z;
		result.any_m |= has_m;
		result.any_be |= !le;

		result.size += sizeof(uint8_t) + sizeof(uint32_t); // Byte order + type/meta

		switch (type_id) {
		case 1: { // POINT
			reader.Skip(v_size);
			result.size += v_size;
		} break;
		case 2: { // LINESTRING
			const auto vert_count = reader.Read<uint32_t>(le);
			reader.Skip(vert_count * v_size);
			result.size += sizeof(uint32_t) + vert_count * v_size;
		} break;
		case 3: { // POLYGON
			const auto ring_count = reader.Read<uint32_t>(le);
			result.size += sizeof(uint32_t);
			for (uint32_t ring_idx = 0; ring_idx < ring_count; ring_idx++) {
				const auto vert_count = reader.Read<uint32_t>(le);
				reader.Skip(vert_count * v_size);
				result.size += sizeof(uint32_t) + vert_count * v_size;
			}
		} break;
		case 4:   // MULTIPOINT
		case 5:   // MULTILINESTRING
		case 6:   // MULTIPOLYGON
		case 7: { // GEOMETRYCOLLECTION
			const auto part_count = reader.Read<uint32_t>(le);
			// MULTIPOINT, MULTILINESTRING and MULTIPOLYGON contain POINT, LINESTRING and POLYGON parts
			collections.push_back({part_count, type_id == 7 ? 0 : type_id - 3});
			result.size += sizeof(uint32_t); // part count
		} break;
		default: {
			result.any_unknown = true;
			return result;
		}
		}
	}
	result.has_trailing_data = !reader.IsAtEnd();
	return result;
}

void ConvertWKB(BlobReader &reader, FixedSizeBlobWriter &writer) {
	while (!reader.IsAtEnd()) {
		const auto le = reader.Read<uint8_t>() == 1;
		const auto meta = reader.Read<uint32_t>(le);
		const auto type_id = (meta & 0x0000FFFF) % 1000;
		const auto flag_id = (meta & 0x0000FFFF) / 1000;

		// Extended WKB detection
		const auto has_extz = (meta & 0x80000000) != 0;
		const auto has_extm = (meta & 0x40000000) != 0;
		const auto has_srid = (meta & 0x20000000) != 0;

		const auto has_z = ((flag_id & 0x01) != 0) || has_extz;
		const auto has_m = ((flag_id & 0x02) != 0) || has_extm;

		if (has_srid) {
			reader.Skip(sizeof(uint32_t)); // Skip SRID
		}

		const auto v_width = static_cast<uint32_t>((2 + (has_z ? 1 : 0) + (has_m ? 1 : 0)));

		writer.Write<uint8_t>(1);                                          // Always write LE
		writer.Write<uint32_t>(type_id + (1000 * has_z) + (2000 * has_m)); // Write meta

		switch (type_id) {
		case 1: { // POINT
			for (uint32_t d_idx = 0; d_idx < v_width; d_idx++) {
				auto value = reader.Read<double>(le);
				writer.Write<double>(value);
			}
		} break;
		case 2: { // LINESTRING
			const auto vert_count = reader.Read<uint32_t>(le);
			writer.Write<uint32_t>(vert_count);
			for (uint32_t vert_idx = 0; vert_idx < vert_count; vert_idx++) {
				for (uint32_t d_idx = 0; d_idx < v_width; d_idx++) {
					auto value = reader.Read<double>(le);
					writer.Write<double>(value);
				}
			}
		} break;
		case 3: { // POLYGON
			const auto ring_count = reader.Read<uint32_t>(le);
			writer.Write<uint32_t>(ring_count);
			for (uint32_t ring_idx = 0; ring_idx < ring_count; ring_idx++) {
				const auto vert_count = reader.Read<uint32_t>(le);
				writer.Write<uint32_t>(vert_count);
				for (uint32_t vert_idx = 0; vert_idx < vert_count; vert_idx++) {
					for (uint32_t d_idx = 0; d_idx < v_width; d_idx++) {
						auto value = reader.Read<double>(le);
						writer.Write<double>(value);
					}
				}
			}
		} break;
		case 4:   // MULTIPOINT
		case 5:   // MULTILINESTRING
		case 6:   // MULTIPOLYGON
		case 7: { // GEOMETRYCOLLECTION
			const auto part_count = reader.Read<uint32_t>(le);
			writer.Write<uint32_t>(part_count);
		} break;
		default:
			D_ASSERT(false);
			break;
		}
	}
}

} // namespace

} // namespace duckdb

//----------------------------------------------------------------------------------------------------------------------
// Public interface
//----------------------------------------------------------------------------------------------------------------------
namespace duckdb {
constexpr const idx_t Geometry::MAX_RECURSION_DEPTH;

bool Geometry::FromBinary(const string_t &wkb, string_t &result, StringHeap &heap, bool strict) {
	BlobReader reader(wkb.GetData(), static_cast<uint32_t>(wkb.GetSize()));

	WKBAnalysis analysis;
	try {
		analysis = AnalyzeWKB(reader);
	} catch (InvalidInputException &) {
		// Truncated input (e.g. a collection declaring more children than are present) must
		// uphold the non-strict contract: report failure instead of throwing.
		if (strict) {
			throw;
		}
		return false;
	}
	if (analysis.has_trailing_data) {
		if (strict) {
			throw InvalidInputException("Unexpected trailing data at position %zu", reader.GetPosition());
		}
		return false;
	}
	if (analysis.any_unknown) {
		if (strict) {
			throw InvalidInputException("Unsupported geometry type in WKB");
		}
		return false;
	}

	if (analysis.any_be || analysis.any_ewkb) {
		reader.Reset();
		// Make a new WKB with all LE
		auto blob = heap.EmptyString(analysis.size);
		FixedSizeBlobWriter writer(blob.GetDataWriteable(), static_cast<uint32_t>(blob.GetSize()));
		ConvertWKB(reader, writer);
		blob.Finalize();
		result = blob;
		return true;
	}

	// Copy the WKB as-is
	result = heap.AddBlob(wkb.GetData(), wkb.GetSize());
	return true;
}

bool Geometry::FromBinary(const Vector &source, Vector &result, idx_t count, bool strict) {
	auto &heap = StringVector::GetStringHeap(result);
	if (strict) {
		UnaryExecutor::Execute<string_t, string_t>(source, result, count, [&](const string_t &wkb) {
			string_t geom;
			FromBinary(wkb, geom, heap, true);
			return geom;
		});
		return true;
	}

	auto all_ok = true;
	UnaryExecutor::Execute<string_t, string_t>(source, result, count, [&](const string_t &wkb) -> optional<string_t> {
		string_t geom;
		if (!FromBinary(wkb, geom, heap, false)) {
			all_ok = false;
			return nullopt;
		}
		return geom;
	});
	return all_ok;
}

void Geometry::ToBinary(const Vector &source, Vector &result) {
	// We are currently using WKB internally, so just copy as-is!
	result.Reinterpret(source);
}

//! Returns false if the text is not valid WKT, the error is recorded in the reader
static bool TryParseWKT(TextReader &reader, string_t &result, StringHeap &heap, bool &trailing_text) {
	BlobWriter writer;
	trailing_text = false;
	if (!FromStringRecursive(reader, writer, 0, false, false)) {
		return false;
	}
	// Check whether reader has consumed over all meaningful characters.
	if (!reader.IsAtEnd()) {
		trailing_text = true;
		return reader.SetError("Unexpected trailing text");
	}
	const auto &buffer = writer.GetBuffer();
	result = heap.AddBlob(buffer.data(), buffer.size());
	return true;
}

bool Geometry::TryFromString(const string_t &wkt_text, string_t &result, StringHeap &heap, string &error_message) {
	TextReader reader(wkt_text.GetData(), static_cast<uint32_t>(wkt_text.GetSize()));
	bool trailing_text;
	if (!TryParseWKT(reader, result, heap, trailing_text)) {
		error_message = reader.GetErrorMessage();
		return false;
	}
	return true;
}

bool Geometry::FromString(const string_t &wkt_text, string_t &result, StringHeap &heap, bool strict,
                          QueryLocation query_location) {
	TextReader reader(wkt_text.GetData(), static_cast<uint32_t>(wkt_text.GetSize()));
	reader.SetQueryLocation(query_location);
	bool trailing_text;
	if (TryParseWKT(reader, result, heap, trailing_text)) {
		return true;
	}
	if (trailing_text && !strict) {
		return false;
	}
	throw reader.MakeError();
}

bool Geometry::FromString(const string_t &wkt_text, string_t &result, Vector &result_vector, bool strict) {
	return FromString(wkt_text, result, StringVector::GetStringHeap(result_vector), strict, optional_idx::Invalid());
}

string_t Geometry::ToString(StringHeap &heap, const string_t &geom) {
	BlobReader reader(geom.GetData(), static_cast<uint32_t>(geom.GetSize()));
	TextWriter writer;

	ToStringRecursive(reader, writer, 0, false, false);

	// Convert the buffer to string_t
	const auto &buffer = writer.GetBuffer();
	return heap.AddString(buffer.data(), buffer.size());
}

//! Read and validate the byte order and type of a WKB geometry
static pair<GeometryType, VertexType> ReadGeometryHeader(BlobReader &reader) {
	// Read the byte order (should always be 1 for little-endian)
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

	const auto geom_type = static_cast<GeometryType>(type_id);
	const auto vert_type = static_cast<VertexType>(flag_id);

	return {geom_type, vert_type};
}

pair<GeometryType, VertexType> Geometry::GetType(const string_t &wkb) {
	BlobReader reader(wkb.GetData(), static_cast<uint32_t>(wkb.GetSize()));
	return ReadGeometryHeader(reader);
}

//----------------------------------------------------------------------------------------------------------------------
// Geometry visitor
//----------------------------------------------------------------------------------------------------------------------

namespace {

//! Receives the parts and vertices of a WKB geometry from VisitGeometry. Each part is entered and left with its index
//! within its parent (0 for the root) and its number of children: vertices, rings or parts. An empty point has no
//! vertex. Implementations should be final and passed to VisitGeometry as their own type, so that the calls are direct
//! and can be inlined.
class GeometryVisitor {
public:
	virtual ~GeometryVisitor() = default;

	virtual void EnterPoint(idx_t part_idx, idx_t vert_count) {
	}
	virtual void LeavePoint(idx_t part_idx, idx_t vert_count) {
	}
	virtual void EnterLineString(idx_t part_idx, idx_t vert_count) {
	}
	virtual void LeaveLineString(idx_t part_idx, idx_t vert_count) {
	}
	virtual void EnterPolygon(idx_t part_idx, idx_t ring_count) {
	}
	virtual void LeavePolygon(idx_t part_idx, idx_t ring_count) {
	}
	virtual void EnterPolygonRing(idx_t ring_idx, idx_t vert_count) {
	}
	virtual void LeavePolygonRing(idx_t ring_idx, idx_t vert_count) {
	}
	virtual void EnterMultiPoint(idx_t part_idx, idx_t part_count) {
	}
	virtual void LeaveMultiPoint(idx_t part_idx, idx_t part_count) {
	}
	virtual void EnterMultiLineString(idx_t part_idx, idx_t part_count) {
	}
	virtual void LeaveMultiLineString(idx_t part_idx, idx_t part_count) {
	}
	virtual void EnterMultiPolygon(idx_t part_idx, idx_t part_count) {
	}
	virtual void LeaveMultiPolygon(idx_t part_idx, idx_t part_count) {
	}
	virtual void EnterGeometryCollection(idx_t part_idx, idx_t part_count) {
	}
	virtual void LeaveGeometryCollection(idx_t part_idx, idx_t part_count) {
	}

	virtual void OnVertexXY(const VertexXY &vertex) = 0;
	virtual void OnVertexXYZ(const VertexXYZ &vertex) = 0;
	virtual void OnVertexXYM(const VertexXYM &vertex) = 0;
	virtual void OnVertexXYZM(const VertexXYZM &vertex) = 0;
};

//! Walks the parts of a WKB geometry whose vertices are all of type VERTEX_TYPE
template <class VERTEX_TYPE, class VISITOR>
class GeometryWalker {
public:
	//! Visit a geometry of the given type, whose header has already been read
	static void Visit(BlobReader &reader, GeometryType type, idx_t part_idx, VISITOR &visitor, idx_t depth) {
		switch (type) {
		case GeometryType::POINT:
			VisitPoint(reader, part_idx, visitor);
			break;
		case GeometryType::LINESTRING:
			VisitLineString(reader, part_idx, visitor);
			break;
		case GeometryType::POLYGON:
			VisitPolygon(reader, part_idx, visitor);
			break;
		case GeometryType::MULTIPOINT: {
			const auto part_count = reader.Read<uint32_t>();
			visitor.EnterMultiPoint(part_idx, part_count);
			for (uint32_t i = 0; i < part_count; i++) {
				ReadPartHeader(reader, GeometryType::POINT);
				VisitPoint(reader, i, visitor);
			}
			visitor.LeaveMultiPoint(part_idx, part_count);
		} break;
		case GeometryType::MULTILINESTRING: {
			const auto part_count = reader.Read<uint32_t>();
			visitor.EnterMultiLineString(part_idx, part_count);
			for (uint32_t i = 0; i < part_count; i++) {
				ReadPartHeader(reader, GeometryType::LINESTRING);
				VisitLineString(reader, i, visitor);
			}
			visitor.LeaveMultiLineString(part_idx, part_count);
		} break;
		case GeometryType::MULTIPOLYGON: {
			const auto part_count = reader.Read<uint32_t>();
			visitor.EnterMultiPolygon(part_idx, part_count);
			for (uint32_t i = 0; i < part_count; i++) {
				ReadPartHeader(reader, GeometryType::POLYGON);
				VisitPolygon(reader, i, visitor);
			}
			visitor.LeaveMultiPolygon(part_idx, part_count);
		} break;
		case GeometryType::GEOMETRYCOLLECTION: {
			if (depth >= Geometry::MAX_RECURSION_DEPTH) {
				throw InvalidInputException("Geometry exceeds maximum recursion depth of %d",
				                            Geometry::MAX_RECURSION_DEPTH);
			}
			const auto part_count = reader.Read<uint32_t>();
			visitor.EnterGeometryCollection(part_idx, part_count);
			for (uint32_t i = 0; i < part_count; i++) {
				const auto part_type = ReadPartHeader(reader);
				Visit(reader, part_type, i, visitor, depth + 1);
			}
			visitor.LeaveGeometryCollection(part_idx, part_count);
		} break;
		default:
			throw InvalidInputException("Unsupported geometry type %d in WKB", static_cast<int>(type));
		}
	}

private:
	static VERTEX_TYPE LoadVertex(const_data_ptr_t ptr) {
#if DUCKDB_IS_BIG_ENDIAN
		double be_buffer[VERTEX_TYPE::WIDTH];
		for (idx_t i = 0; i < VERTEX_TYPE::WIDTH; i++) {
			be_buffer[i] = LoadLE<double>(ptr + i * sizeof(double));
		}
		return Load<VERTEX_TYPE>(reinterpret_cast<const_data_ptr_t>(be_buffer));
#else
		return Load<VERTEX_TYPE>(ptr);
#endif
	}

	static void OnVertex(VISITOR &visitor, const VertexXY &vertex) {
		visitor.OnVertexXY(vertex);
	}
	static void OnVertex(VISITOR &visitor, const VertexXYZ &vertex) {
		visitor.OnVertexXYZ(vertex);
	}
	static void OnVertex(VISITOR &visitor, const VertexXYM &vertex) {
		visitor.OnVertexXYM(vertex);
	}
	static void OnVertex(VISITOR &visitor, const VertexXYZM &vertex) {
		visitor.OnVertexXYZM(vertex);
	}

	static void VisitVertices(BlobReader &reader, uint32_t vert_count, VISITOR &visitor) {
		// Issue a single .Reserve() for all vertices, to minimize bounds checking overhead
		const auto ptr = const_data_ptr_cast(reader.Reserve(vert_count * sizeof(VERTEX_TYPE)));
		for (uint32_t vert_idx = 0; vert_idx < vert_count; vert_idx++) {
			OnVertex(visitor, LoadVertex(ptr + vert_idx * sizeof(VERTEX_TYPE)));
		}
	}

	static void VisitPoint(BlobReader &reader, idx_t part_idx, VISITOR &visitor) {
		const auto vertex = LoadVertex(const_data_ptr_cast(reader.Reserve(sizeof(VERTEX_TYPE))));
		// An empty point has all-NaN ordinates
		const idx_t vert_count = vertex.AllNan() ? 0 : 1;
		visitor.EnterPoint(part_idx, vert_count);
		if (vert_count != 0) {
			OnVertex(visitor, vertex);
		}
		visitor.LeavePoint(part_idx, vert_count);
	}

	static void VisitLineString(BlobReader &reader, idx_t part_idx, VISITOR &visitor) {
		const auto vert_count = reader.Read<uint32_t>();
		visitor.EnterLineString(part_idx, vert_count);
		VisitVertices(reader, vert_count, visitor);
		visitor.LeaveLineString(part_idx, vert_count);
	}

	static void VisitPolygon(BlobReader &reader, idx_t part_idx, VISITOR &visitor) {
		const auto ring_count = reader.Read<uint32_t>();
		visitor.EnterPolygon(part_idx, ring_count);
		for (uint32_t ring_idx = 0; ring_idx < ring_count; ring_idx++) {
			const auto vert_count = reader.Read<uint32_t>();
			visitor.EnterPolygonRing(ring_idx, vert_count);
			VisitVertices(reader, vert_count, visitor);
			visitor.LeavePolygonRing(ring_idx, vert_count);
		}
		visitor.LeavePolygon(part_idx, ring_count);
	}

	//! Read the header of a collection part, which must have the same vertex type as the root
	static GeometryType ReadPartHeader(BlobReader &reader) {
		const auto header = ReadGeometryHeader(reader);
		if (header.second != VERTEX_TYPE::TYPE) {
			throw InvalidInputException("Geometry has inconsistent Z/M dimensions, starting at position %zu",
			                            reader.GetPosition());
		}
		return header.first;
	}

	static void ReadPartHeader(BlobReader &reader, GeometryType expected_type) {
		const auto type = ReadPartHeader(reader);
		if (type != expected_type) {
			throw InvalidInputException("Unexpected geometry type %d in a collection of type %d at position %zu",
			                            static_cast<int>(type), static_cast<int>(expected_type) + 3,
			                            reader.GetPosition());
		}
	}
};

//! Walk a WKB geometry, calling the visitor for each of its parts and vertices
template <class VISITOR>
void VisitGeometry(const string_t &wkb, VISITOR &visitor) {
	BlobReader reader(wkb.GetData(), static_cast<uint32_t>(wkb.GetSize()));
	const auto header = ReadGeometryHeader(reader);
	switch (header.second) {
	case VertexType::XY:
		GeometryWalker<VertexXY, VISITOR>::Visit(reader, header.first, 0, visitor, 0);
		break;
	case VertexType::XYZ:
		GeometryWalker<VertexXYZ, VISITOR>::Visit(reader, header.first, 0, visitor, 0);
		break;
	case VertexType::XYM:
		GeometryWalker<VertexXYM, VISITOR>::Visit(reader, header.first, 0, visitor, 0);
		break;
	case VertexType::XYZM:
		GeometryWalker<VertexXYZM, VISITOR>::Visit(reader, header.first, 0, visitor, 0);
		break;
	default:
		throw InvalidInputException("Unsupported vertex type %d in WKB", static_cast<int>(header.second));
	}
	if (!reader.IsAtEnd()) {
		throw InvalidInputException("Unexpected trailing data at position %zu", reader.GetPosition());
	}
}

} // namespace

//----------------------------------------------------------------------------------------------------------------------
// Geodetic (GEOGRAPHY) coverage extents
//----------------------------------------------------------------------------------------------------------------------
// GEOGRAPHY edges are geodesics (great-circle arcs), which can bulge poleward past their endpoints, and polygon rings
// can enclose a pole. The geodetic extent therefore bounds what the geometry covers, not just its coordinates.
// (i.e., we cant just accumulate the min/max of all the coordinates individually)
//
// For example, the edge from A (60W, 45N) to B (60E, 45N), drawn in lon/lat:
//
//     63N             _.--''''''--._
//               _.-''                ''-._         geodesic: great-circle arc, bulges poleward
//           _.-'                          '-._
//     45N  A*--------------------------------*B    planar: straight line in lon/lat
//          60W              0               60E
//
// The geodesic reaches ~63.4N at longitude 0, so a latitude range over the endpoints alone (45N) would miss it.
// Longitude is monotone along any edge spanning less than half a circle, so an edge covers the short arc between its
// endpoints' longitudes. The vertices alone may not: LINESTRING(0 10, 170 10, -170 10, 0 10) has vertices within the
// arc from 0 east to -170, but its last edge runs from -170 east to 0.
//
// Here's a mini-glossary to help understanding the following code:
// - Longitude / Latitude: degrees east in [-180, 180] and degrees north in [-90, 90].
// - Great circle: a circle on the sphere whose center is the sphere's center, e.g. the equator.
// - Geodesic: the shortest path between two points on the sphere, an arc of the great circle through both.
// - Meridian: a line of constant longitude, running from pole to pole.
// - Antipodal: diametrically opposite, e.g. the two poles. Every great circle through one point also passes through
//   its antipode, so the geodesic between them is ambiguous.
// - Apex: the northernmost or southernmost point of a great circle.
// - Bulge: the part of an edge that reaches beyond the latitudes of its endpoints, towards a pole.
// - Winding: the total signed longitude change around a ring, about +-360 degrees if the ring encircles a pole.
// - Shell / hole: the first ring of a polygon and the remaining ones. The interior lies to the left of the direction
//   of travel along the ring.

namespace {

constexpr double DEG_TO_RAD = 0.017453292519943295;

//! Outward padding for computed apex latitudes, in degrees
constexpr double APEX_EPSILON = 1e-9;

//! Edges spanning within this many degrees of half a circle of longitude are ambiguous
constexpr double HALF_CIRCLE_EPSILON = 1e-9;

//! Tolerance for dimensionless quantities on the unit sphere (sines and cosines)
constexpr double UNIT_EPSILON = 1e-14;

struct Vec2 {
	double x;
	double y;

	double Cross(const Vec2 &o) const {
		return x * o.y - y * o.x;
	}
};

struct Vec3 {
	double x;
	double y;
	double z;

	Vec3 Cross(const Vec3 &o) const {
		return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
	}
	double Dot(const Vec3 &o) const {
		return x * o.x + y * o.y + z * o.z;
	}
};

//! A point on the sphere, in degrees
struct LonLat {
	double lon;
	double lat;

	//! Signed eastward longitude difference in degrees, in [-180, 180]
	static double LonDelta(double from, double to) {
		double d = to - from;
		if (d > 180.0) {
			d -= 360.0;
		} else if (d < -180.0) {
			d += 360.0;
		}
		return d;
	}

	Vec3 ToCartesian() const {
		const double phi = lat * DEG_TO_RAD;
		const double lam = lon * DEG_TO_RAD;
		return {std::cos(phi) * std::cos(lam), std::cos(phi) * std::sin(lam), std::sin(phi)};
	}

	//! Initial (east, north) tangent of the geodesic from this point towards `to`, unnormalized
	Vec2 DirectionTo(const LonLat &to) const {
		const double phi_from = lat * DEG_TO_RAD;
		const double phi_to = to.lat * DEG_TO_RAD;
		const double dl = LonDelta(lon, to.lon) * DEG_TO_RAD;
		return {std::sin(dl) * std::cos(phi_to),
		        std::cos(phi_from) * std::sin(phi_to) - std::sin(phi_from) * std::cos(phi_to) * std::cos(dl)};
	}
};

//! A great-circle arc from p1 to p2, spanning less than half a circle
struct GeodesicEdge {
	LonLat p1;
	LonLat p2;

	//! Signed eastward longitude change from p1 to p2, in degrees, in [-180, 180]
	double GetLonDelta() const {
		return LonLat::LonDelta(p1.lon, p2.lon);
	}

	//! Whether `lon` lies within the longitudes the edge passes through
	bool ContainsLon(double lon) const {
		const double delta = GetLonDelta();
		const double offset = LonLat::LonDelta(p1.lon, lon);
		return delta > 0 ? (offset >= 0 && offset <= delta) : (offset <= 0 && offset >= delta);
	}

	//! Latitude range reached by the edge, including any poleward bulge past its endpoints.
	//! Returns false if the edge is ambiguous (endpoints nearly antipodal).
	bool TryGetLatRange(double &min_lat, double &max_lat) const {
		min_lat = MinValue(p1.lat, p2.lat);
		max_lat = MaxValue(p1.lat, p2.lat);

		// The endpoints only bound a planar edge.
		// On the sphere it can bulge poleward, which is what the rest of this function computes.

		const double delta = GetLonDelta();

		if (!(std::fabs(delta) < 180.0 - HALF_CIRCLE_EPSILON)) {
			// Spans (nearly) half a circle of longitude, or NaN
			return false;
		}
		if (delta == 0.0) {
			// This is a meridian arc, i.e. same longitude, i.e. no bulge
			return true;
		}

		const Vec3 c1 = p1.ToCartesian();
		const Vec3 c2 = p2.ToCartesian();
		const Vec3 n = c1.Cross(c2);
		const double nxy = std::hypot(n.x, n.y);
		const double nlen = std::hypot(nxy, n.z);

		if (!(nlen > UNIT_EPSILON)) {
			// Coincident endpoints (e.g. a pole under two longitudes) are fine, antipodal ones are ambiguous
			return c1.Dot(c2) > 0;
		}
		if (std::fabs(n.z) <= UNIT_EPSILON * nlen) {
			// The great circle runs (nearly) through the poles, where all of its longitude change happens, so the edge
			// crosses a pole iff it spans about half a circle. The shorter arc crosses the pole nearer to the
			// endpoints.
			if (std::fabs(delta) > 90.0) {
				const double lat_sum = p1.lat + p2.lat;
				if (lat_sum > 0) {
					max_lat = 90.0;
				} else if (lat_sum < 0) {
					min_lat = -90.0;
				} else {
					return false;
				}
			}
			return true;
		}
		// Northernmost point of the great circle, atan2 stays well-conditioned near the pole, unlike asin(nxy / nlen)
		const double apex_lat = std::atan2(nxy, std::fabs(n.z)) / DEG_TO_RAD;
		const double apex_lon = std::atan2(-n.y * n.z, -n.x * n.z) / DEG_TO_RAD;

		if (ContainsLon(apex_lon) && apex_lat > max_lat) {
			max_lat = apex_lat;
		}

		// The southernmost point is antipodal to the northernmost
		const double south_apex_lon = apex_lon > 0 ? apex_lon - 180.0 : apex_lon + 180.0;

		if (ContainsLon(south_apex_lon) && -apex_lat < min_lat) {
			min_lat = -apex_lat;
		}
		return true;
	}
};

//! Side of the boundary that a ring's interior lies on at its highest point. SOUTH is the bounded, pole-free side.
enum class InteriorSide : int8_t { UNKNOWN, SOUTH, NORTH };

//! Coverage state of a linestring or polygon ring, accumulated vertex by vertex
class GeodeticRingState {
public:
	//! Add the next vertex, extending the extent with the edge from the previous one
	void AddVertex(GeometryExtent &extent, const LonLat &vertex) {
		if (vertex_count == 0) {
			first_vertex = vertex;
		} else {
			AddEdge(extent, last_vertex, vertex);
			if (vertex_count == 1) {
				second_vertex = vertex;
			}
		}
		last_vertex = vertex;
		vertex_count++;
	}

	//! Close the ring, adding the closing edge unless the last vertex already repeats the first (+180 and -180 are the
	//! same longitude). The closing vertex's successor is the second vertex.
	void CloseRing(GeometryExtent &extent) {
		if (vertex_count < 2) {
			return;
		}
		if (LonLat::LonDelta(last_vertex.lon, first_vertex.lon) != 0.0 || last_vertex.lat != first_vertex.lat) {
			AddEdge(extent, last_vertex, first_vertex);
		}
		ResolvePending(second_vertex);
	}

	//! Apply the ring state to the extent. A shell's interior lies on the left of the travel direction (as in S2),
	//! with the orientation read off at the ring's highest point, since a planar shoelace fails when edges bulge.
	//! Holes never extend the interior, so only their edges contribute.
	void Finish(GeometryExtent &extent, bool is_shell) const {
		if (ambiguous) {
			ExtendFullGlobe(extent);
			return;
		}
		if (!is_shell) {
			return;
		}
		// The winding thresholds lie halfway between whole turns
		if (std::fabs(winding) >= 540.0) {
			// Wound around the globe more than once: not a simple ring
			ExtendFullGlobe(extent);
			return;
		}
		if (std::fabs(winding) >= 180.0) {
			// Wound around the globe once: east encloses the north pole, west the south pole
			extent.x_min = -180.0;
			extent.x_max = 180.0;
			if (winding > 0) {
				extent.y_max = MaxValue(extent.y_max, 90.0);
			} else {
				extent.y_min = MinValue(extent.y_min, -90.0);
			}
			return;
		}
		if (top_conflict || top_side != InteriorSide::SOUTH || top_lat >= 90.0) {
			// Clockwise (the interior is the complement), or the orientation is undefined (cusp, degenerate, on a pole)
			ExtendFullGlobe(extent);
		}
		// Otherwise counterclockwise: the interior lies within the boundary's extent
	}

private:
	//! Add the edge prev -> cur, extending the extent with its longitude and latitude ranges
	void AddEdge(GeometryExtent &extent, const LonLat &prev, const LonLat &cur) {
		const GeodesicEdge edge = {prev, cur};
		double min_lat = 0;
		double max_lat = 0;
		if (!edge.TryGetLatRange(min_lat, max_lat)) {
			ambiguous = true;
			return;
		}
		const double delta = edge.GetLonDelta();
		winding += delta;
		ResolvePending(cur);
		if (delta > 0) {
			extent.ExtendXArc(prev.lon, cur.lon);
		} else if (delta < 0) {
			extent.ExtendXArc(cur.lon, prev.lon);
		}
		// The endpoints are already in the extent, and computed apexes beyond them are padded outward
		if (min_lat < MinValue(prev.lat, cur.lat)) {
			extent.y_min = MinValue(extent.y_min, MaxValue(min_lat - APEX_EPSILON, -90.0));
		}
		if (max_lat > MaxValue(prev.lat, cur.lat)) {
			extent.y_max = MaxValue(extent.y_max, MinValue(max_lat + APEX_EPSILON, 90.0));
			// A smooth highest point: traveling east puts the interior (on the left) north of it
			SubmitTop(max_lat, delta > 0 ? InteriorSide::NORTH : InteriorSide::SOUTH);
		}
		if (cur.lat >= top_lat) {
			// A vertex's interior side needs the next vertex, so it is kept pending until the following edge
			pending = true;
			pending_vertex = cur;
			pending_prev = prev;
		}
	}

	//! Submit the pending vertex's interior side, now that its successor is known
	void ResolvePending(const LonLat &next) {
		if (!pending) {
			return;
		}
		const auto &v = pending_vertex;
		const Vec2 to_next = v.DirectionTo(next);
		const Vec2 to_prev = v.DirectionTo(pending_prev);
		const bool next_descends = to_next.y < -UNIT_EPSILON;
		const bool prev_descends = to_prev.y < -UNIT_EPSILON;
		if (next_descends && prev_descends) {
			// A corner: traversed prev -> v -> next with both edges leaving v heading south, the interior (on the left)
			// is pole-free iff the turn at v is a left turn
			const double cross = to_next.Cross(to_prev);
			SubmitTop(v.lat,
			          cross > 0 ? InteriorSide::SOUTH : (cross < 0 ? InteriorSide::NORTH : InteriorSide::UNKNOWN));
		} else {
			// An edge that does not descend from v peaks at or past v, possibly higher by less than the rounding of its
			// apex. The boundary is smooth there, so as at an apex, traveling east puts the interior north of it.
			if (!next_descends) {
				SubmitTop(v.lat, SideOfTravel(to_next.x));
			}
			if (!prev_descends) {
				SubmitTop(v.lat, SideOfTravel(-to_prev.x));
			}
		}
		pending = false;
	}

	//! Interior side at a smooth boundary point, given the eastward component of the travel direction there
	static InteriorSide SideOfTravel(double east) {
		if (east > UNIT_EPSILON) {
			return InteriorSide::NORTH;
		}
		if (east < -UNIT_EPSILON) {
			return InteriorSide::SOUTH;
		}
		return InteriorSide::UNKNOWN;
	}

	//! Record the interior side at a boundary point at `lat`, if it is (one of) the highest
	void SubmitTop(double lat, InteriorSide side) {
		if (lat > top_lat) {
			top_lat = lat;
			top_side = side;
			top_conflict = side == InteriorSide::UNKNOWN;
		} else if (lat == top_lat && side != top_side) {
			top_conflict = true;
		}
	}

	//! Extend the extent to cover the entire globe
	static void ExtendFullGlobe(GeometryExtent &extent) {
		extent.x_min = -180.0;
		extent.x_max = 180.0;
		extent.y_min = MinValue(extent.y_min, -90.0);
		extent.y_max = MaxValue(extent.y_max, 90.0);
	}

private:
	//! Number of vertices added so far, and the first, second and last of them
	idx_t vertex_count = 0;
	LonLat first_vertex = {0, 0};
	LonLat second_vertex = {0, 0};
	LonLat last_vertex = {0, 0};
	//! Accumulated signed longitude change in degrees, ~+-360 for a ring that winds around a pole
	double winding = 0;
	//! Highest latitude reached by the boundary (vertices and edge apexes)
	double top_lat = -std::numeric_limits<double>::infinity();
	//! Side of the boundary the interior lies on at top_lat
	InteriorSide top_side = InteriorSide::UNKNOWN;
	//! The boundary reaches top_lat with conflicting interior sides (a cusp or degenerate ring)
	bool top_conflict = false;
	//! A highest vertex whose interior side awaits the next vertex
	bool pending = false;
	LonLat pending_vertex = {0, 0};
	LonLat pending_prev = {0, 0};
	//! Saw a (nearly) antipodal edge, whose geodesic is ambiguous and may pass arbitrarily close to a pole
	bool ambiguous = false;
};

} // namespace

//----------------------------------------------------------------------------------------------------------------------
// Extents
//----------------------------------------------------------------------------------------------------------------------

namespace {

//! Extends an extent with the vertices of a geometry, counting them and whether any part is empty
class ExtentVisitor : public GeometryVisitor {
public:
	explicit ExtentVisitor(GeometryExtent &extent) : extent(extent) {
	}

	uint32_t GetVertexCount() const {
		return vertex_count;
	}
	bool HasAnyEmpty() const {
		return has_any_empty;
	}

	void EnterPoint(idx_t part_idx, idx_t vert_count) override {
		MarkIfEmpty(vert_count);
	}
	void EnterLineString(idx_t part_idx, idx_t vert_count) override {
		MarkIfEmpty(vert_count);
	}
	void EnterPolygon(idx_t part_idx, idx_t ring_count) override {
		MarkIfEmpty(ring_count);
	}
	void EnterPolygonRing(idx_t ring_idx, idx_t vert_count) override {
		MarkIfEmpty(vert_count);
	}
	void EnterMultiPoint(idx_t part_idx, idx_t part_count) override {
		MarkIfEmpty(part_count);
	}
	void EnterMultiLineString(idx_t part_idx, idx_t part_count) override {
		MarkIfEmpty(part_count);
	}
	void EnterMultiPolygon(idx_t part_idx, idx_t part_count) override {
		MarkIfEmpty(part_count);
	}
	void EnterGeometryCollection(idx_t part_idx, idx_t part_count) override {
		MarkIfEmpty(part_count);
	}

protected:
	void MarkIfEmpty(idx_t child_count) {
		if (child_count == 0) {
			has_any_empty = true;
		}
	}

protected:
	GeometryExtent &extent;
	uint32_t vertex_count = 0;
	bool has_any_empty = false;
};

//! Extends the extent with every vertex
class PlanarExtentVisitor final : public ExtentVisitor {
public:
	explicit PlanarExtentVisitor(GeometryExtent &extent) : ExtentVisitor(extent) {
	}

	void OnVertexXY(const VertexXY &vertex) override {
		AddVertex(vertex);
	}
	void OnVertexXYZ(const VertexXYZ &vertex) override {
		AddVertex(vertex);
	}
	void OnVertexXYM(const VertexXYM &vertex) override {
		AddVertex(vertex);
	}
	void OnVertexXYZM(const VertexXYZM &vertex) override {
		AddVertex(vertex);
	}

private:
	template <class VERTEX_TYPE>
	void AddVertex(const VERTEX_TYPE &vertex) {
		extent.Extend(vertex, GeometryModel::PLANAR);
		vertex_count++;
	}
};

//! Extends the extent with the coverage of every vertex, geodesic edge, and polygon interior
class GeodeticExtentVisitor final : public ExtentVisitor {
public:
	explicit GeodeticExtentVisitor(GeometryExtent &extent) : ExtentVisitor(extent) {
	}

	void EnterLineString(idx_t part_idx, idx_t vert_count) override {
		ExtentVisitor::EnterLineString(part_idx, vert_count);
		if (vert_count > 0) {
			ring = GeodeticRingState();
			in_ring = true;
		}
	}
	void LeaveLineString(idx_t part_idx, idx_t vert_count) override {
		if (in_ring) {
			ring.Finish(extent, false);
			in_ring = false;
		}
	}
	void EnterPolygon(idx_t part_idx, idx_t ring_count) override {
		ExtentVisitor::EnterPolygon(part_idx, ring_count);
		is_shell = true;
	}
	void EnterPolygonRing(idx_t ring_idx, idx_t vert_count) override {
		ExtentVisitor::EnterPolygonRing(ring_idx, vert_count);
		if (vert_count > 0) {
			ring = GeodeticRingState();
			in_ring = true;
		}
	}
	void LeavePolygonRing(idx_t ring_idx, idx_t vert_count) override {
		if (in_ring) {
			ring.CloseRing(extent);
			ring.Finish(extent, is_shell);
			in_ring = false;
			// The first non-empty ring is the shell, the rest are holes
			is_shell = false;
		}
	}

	void OnVertexXY(const VertexXY &vertex) override {
		AddVertex(vertex);
	}
	void OnVertexXYZ(const VertexXYZ &vertex) override {
		AddVertex(vertex);
	}
	void OnVertexXYM(const VertexXYM &vertex) override {
		AddVertex(vertex);
	}
	void OnVertexXYZM(const VertexXYZM &vertex) override {
		AddVertex(vertex);
	}

private:
	template <class VERTEX_TYPE>
	void AddVertex(const VERTEX_TYPE &vertex) {
		extent.Extend(vertex, GeometryModel::GEODETIC);
		vertex_count++;
		if (in_ring) {
			ring.AddVertex(extent, {vertex.x, vertex.y});
		}
	}

private:
	//! State of the current linestring or polygon ring
	GeodeticRingState ring;
	bool in_ring = false;
	//! Whether the next non-empty ring of the current polygon is its shell
	bool is_shell = false;
};

template <class VISITOR>
uint32_t GetExtentInternal(const string_t &wkb, GeometryExtent &extent, bool &has_any_empty) {
	VISITOR visitor(extent);
	VisitGeometry(wkb, visitor);
	if (visitor.HasAnyEmpty()) {
		has_any_empty = true;
	}
	return visitor.GetVertexCount();
}

} // namespace

uint32_t Geometry::GetExtent(const string_t &wkb, GeometryExtent &extent, GeometryModel model) {
	bool has_any_empty = false;
	return GetExtent(wkb, extent, has_any_empty, model);
}

uint32_t Geometry::GetExtent(const string_t &wkb, GeometryExtent &extent, bool &has_any_empty, GeometryModel model) {
	if (model == GeometryModel::GEODETIC) {
		return GetExtentInternal<GeodeticExtentVisitor>(wkb, extent, has_any_empty);
	}
	return GetExtentInternal<PlanarExtentVisitor>(wkb, extent, has_any_empty);
}

bool Geometry::IsValidGeography(const string_t &wkb) {
	// Coordinates must lie within X in [-180, 180] and Y in [-90, 90], so the planar extent suffices
	auto extent = GeometryExtent::Empty();
	const auto vertex_count = GetExtent(wkb, extent, GeometryModel::PLANAR);
	if (vertex_count == 0) {
		return true;
	}
	// A non-finite X or Y (e.g. NaN) leaves the axis unset, and the geodetic math requires finite values
	if (!extent.HasX() || !extent.HasY()) {
		return false;
	}
	return extent.x_min >= -180.0 && extent.x_max <= 180.0 && extent.y_min >= -90.0 && extent.y_max <= 90.0;
}

//----------------------------------------------------------------------------------------------------------------------
// Shredding
//----------------------------------------------------------------------------------------------------------------------

template <class V = VertexXY>
static void ToPoints(const Vector &source_vec, Vector &target_vec, idx_t row_count) {
	const auto geom_data = source_vec.Values<string_t>();
	auto vert_writer = FlatVector::Writer<typename V::STRUCT_TYPE>(target_vec, row_count);

	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		auto geom_entry = geom_data[row_idx];
		if (!geom_entry.IsValid()) {
			vert_writer.WriteNull();
			continue;
		}

		const auto &blob = geom_entry.GetValue();
		const auto blob_data = blob.GetData();
		const auto blob_size = blob.GetSize();

		BlobReader reader(blob_data, static_cast<uint32_t>(blob_size));

		// Skip byte order and type/meta
		reader.Skip(sizeof(uint8_t) + sizeof(uint32_t));

		vert_writer.ForEach([&](auto &child_writer) { child_writer.WriteValue(reader.Read<double>()); });
	}
}

template <class V = VertexXY>
static void FromPoints(const Vector &source_vec, Vector &target_vec, idx_t row_count, idx_t result_offset) {
	auto vert_iter = source_vec.Values<typename V::STRUCT_TYPE>();
	auto result_data = FlatVector::Writer<string_t>(target_vec, row_count, result_offset);
	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		const auto vert_entry = vert_iter[row_idx];
		if (!vert_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}

		// byte order + type/meta + vertex data
		const auto blob_size = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(V);
		auto &blob = result_data.WriteEmptyString(blob_size);
		const auto blob_data = blob.GetDataWriteable();

		FixedSizeBlobWriter writer(blob_data, static_cast<uint32_t>(blob_size));

		const auto meta = static_cast<uint32_t>(GeometryType::POINT) + (V::HAS_Z ? 1000 : 0) + (V::HAS_M ? 2000 : 0);

		writer.Write<uint8_t>(1);     // Little-endian
		writer.Write<uint32_t>(meta); // Type/meta

		// Write vertex data
		vert_entry.ForEach([&](const auto &v) { writer.Write<double>(v.GetValueUnsafe()); });

		blob.Finalize();
	}
}

template <class V = VertexXY>
static void ToLineStrings(const Vector &source_vec, Vector &target_vec, idx_t row_count) {
	auto geom_data = source_vec.Values<string_t>();
	auto list_writer = FlatVector::Writer<VectorListType<typename V::STRUCT_TYPE>>(target_vec, row_count);

	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		auto geom_entry = geom_data[row_idx];
		if (!geom_entry.IsValid()) {
			list_writer.WriteNull();
			continue;
		}

		const auto &blob = geom_entry.GetValue();
		BlobReader reader(blob.GetData(), static_cast<uint32_t>(blob.GetSize()));

		// Skip byte order and type/meta
		reader.Skip(sizeof(uint8_t) + sizeof(uint32_t));
		const auto vert_count = reader.Read<uint32_t>();

		for (auto &vert_writer : list_writer.WriteList(vert_count)) {
			vert_writer.ForEach([&](auto &dim_writer) { dim_writer.WriteValue(reader.Read<double>()); });
		}
	}
}

template <class V = VertexXY>
static void FromLineStrings(const Vector &source_vec, Vector &target_vec, idx_t row_count, idx_t result_offset) {
	auto line_iter = source_vec.Values<VectorListType<typename V::STRUCT_TYPE>>();
	auto result_data = FlatVector::Writer<string_t>(target_vec, row_count, result_offset);

	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		const auto line_entry = line_iter[row_idx];
		if (!line_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}

		const auto vert_count = line_entry.GetListLength();

		// byte order + type/meta + vertex count + vertex data
		const auto blob_size = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t) + vert_count * sizeof(V);
		auto &blob = result_data.WriteEmptyString(blob_size);
		FixedSizeBlobWriter writer(blob.GetDataWriteable(), static_cast<uint32_t>(blob_size));

		const auto meta =
		    static_cast<uint32_t>(GeometryType::LINESTRING) + (V::HAS_Z ? 1000 : 0) + (V::HAS_M ? 2000 : 0);

		writer.Write<uint8_t>(1);
		writer.Write<uint32_t>(meta);
		writer.Write<uint32_t>(UnsafeNumericCast<uint32_t>(vert_count));

		for (const auto vert_entry : line_entry.GetChildValues()) {
			vert_entry.ForEach([&](const auto &v) { writer.Write<double>(v.GetValueUnsafe()); });
		}

		blob.Finalize();
	}
}

template <class V = VertexXY>
static void ToPolygons(const Vector &source_vec, Vector &target_vec, idx_t row_count) {
	auto geom_data = source_vec.Values<string_t>();
	auto poly_writer =
	    FlatVector::Writer<VectorListType<VectorListType<typename V::STRUCT_TYPE>>>(target_vec, row_count);
	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		auto geom_entry = geom_data[row_idx];
		if (!geom_entry.IsValid()) {
			poly_writer.WriteNull();
			continue;
		}
		const auto &blob = geom_entry.GetValue();
		BlobReader reader(blob.GetData(), static_cast<uint32_t>(blob.GetSize()));
		reader.Skip(sizeof(uint8_t) + sizeof(uint32_t));
		const auto ring_count = reader.Read<uint32_t>();
		for (auto &ring_writer : poly_writer.WriteList(ring_count)) {
			const auto vert_count = reader.Read<uint32_t>();
			for (auto &vert_writer : ring_writer.WriteList(vert_count)) {
				vert_writer.ForEach([&](auto &dim_writer) { dim_writer.WriteValue(reader.Read<double>()); });
			}
		}
	}
}

template <class V = VertexXY>
static void FromPolygons(const Vector &source_vec, Vector &target_vec, idx_t row_count, idx_t result_offset) {
	auto poly_iter = source_vec.Values<VectorListType<VectorListType<typename V::STRUCT_TYPE>>>();
	auto result_data = FlatVector::Writer<string_t>(target_vec, row_count, result_offset);

	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		const auto poly_entry = poly_iter[row_idx];
		if (!poly_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}

		const auto ring_count = poly_entry.GetListLength();

		// byte order + type/meta + ring count
		idx_t blob_size = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t);
		for (const auto ring_entry : poly_entry.GetChildValues()) {
			blob_size += sizeof(uint32_t) + ring_entry.GetListLength() * sizeof(V);
		}
		auto &blob = result_data.WriteEmptyString(blob_size);
		FixedSizeBlobWriter writer(blob.GetDataWriteable(), static_cast<uint32_t>(blob_size));

		const auto meta = static_cast<uint32_t>(GeometryType::POLYGON) + (V::HAS_Z ? 1000 : 0) + (V::HAS_M ? 2000 : 0);

		writer.Write<uint8_t>(1);
		writer.Write<uint32_t>(meta);
		writer.Write<uint32_t>(UnsafeNumericCast<uint32_t>(ring_count));

		for (const auto ring_entry : poly_entry.GetChildValues()) {
			writer.Write<uint32_t>(UnsafeNumericCast<uint32_t>(ring_entry.GetListLength()));
			for (const auto vert_entry : ring_entry.GetChildValues()) {
				vert_entry.ForEach([&](const auto &v) { writer.Write<double>(v.GetValueUnsafe()); });
			}
		}

		blob.Finalize();
	}
}

template <class V = VertexXY>
static void ToMultiPoints(const Vector &source_vec, Vector &target_vec, idx_t row_count) {
	auto geom_data = source_vec.Values<string_t>();
	auto mult_writer = FlatVector::Writer<VectorListType<typename V::STRUCT_TYPE>>(target_vec, row_count);
	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		auto geom_value = geom_data[row_idx];
		if (!geom_value.IsValid()) {
			mult_writer.WriteNull();
			continue;
		}
		const auto &blob = geom_value.GetValue();
		BlobReader reader(blob.GetData(), static_cast<uint32_t>(blob.GetSize()));
		reader.Skip(sizeof(uint8_t) + sizeof(uint32_t));
		const auto part_count = reader.Read<uint32_t>();
		for (auto &vert_writer : mult_writer.WriteList(part_count)) {
			reader.Skip(sizeof(uint8_t) + sizeof(uint32_t));
			vert_writer.ForEach([&](auto &dim_writer) { dim_writer.WriteValue(reader.Read<double>()); });
		}
	}
}

template <class V = VertexXY>
static void FromMultiPoints(const Vector &source_vec, Vector &target_vec, idx_t row_count, idx_t result_offset) {
	auto mult_iter = source_vec.Values<VectorListType<typename V::STRUCT_TYPE>>();
	auto result_data = FlatVector::Writer<string_t>(target_vec, row_count, result_offset);

	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		const auto mult_entry = mult_iter[row_idx];
		if (!mult_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}

		const auto part_count = mult_entry.GetListLength();

		// byte order + type/meta + part count + (point header + vertex) per part
		const auto blob_size = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t) +
		                       part_count * (sizeof(uint8_t) + sizeof(uint32_t) + sizeof(V));

		auto &blob = result_data.WriteEmptyString(blob_size);
		FixedSizeBlobWriter writer(blob.GetDataWriteable(), static_cast<uint32_t>(blob_size));

		const auto meta =
		    static_cast<uint32_t>(GeometryType::MULTIPOINT) + (V::HAS_Z ? 1000 : 0) + (V::HAS_M ? 2000 : 0);

		writer.Write<uint8_t>(1);
		writer.Write<uint32_t>(meta);
		writer.Write<uint32_t>(UnsafeNumericCast<uint32_t>(part_count));

		const auto point_meta =
		    static_cast<uint32_t>(GeometryType::POINT) + (V::HAS_Z ? 1000 : 0) + (V::HAS_M ? 2000 : 0);
		for (const auto vert_entry : mult_entry.GetChildValues()) {
			writer.Write<uint8_t>(1);
			writer.Write<uint32_t>(point_meta);
			vert_entry.ForEach([&](const auto &v) { writer.Write<double>(v.GetValueUnsafe()); });
		}

		blob.Finalize();
	}
}

template <class V = VertexXY>
static void ToMultiLineStrings(const Vector &source_vec, Vector &target_vec, idx_t row_count) {
	source_vec.Flatten();
	const auto geom_data = FlatVector::GetData<string_t>(source_vec);
	auto mult_writer =
	    FlatVector::Writer<VectorListType<VectorListType<typename V::STRUCT_TYPE>>>(target_vec, row_count);
	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		if (FlatVector::IsNull(source_vec, row_idx)) {
			mult_writer.WriteNull();
			continue;
		}
		const auto &blob = geom_data[row_idx];
		BlobReader reader(blob.GetData(), static_cast<uint32_t>(blob.GetSize()));
		reader.Skip(sizeof(uint8_t) + sizeof(uint32_t));
		const auto line_count = reader.Read<uint32_t>();
		for (auto &line_writer : mult_writer.WriteList(line_count)) {
			reader.Skip(sizeof(uint8_t) + sizeof(uint32_t));
			const auto vert_count = reader.Read<uint32_t>();
			for (auto &vert_writer : line_writer.WriteList(vert_count)) {
				vert_writer.ForEach([&](auto &dim_writer) { dim_writer.WriteValue(reader.Read<double>()); });
			}
		}
	}
}

template <class V = VertexXY>
static void FromMultiLineStrings(const Vector &source_vec, Vector &target_vec, idx_t row_count, idx_t result_offset) {
	auto mult_iter = source_vec.Values<VectorListType<VectorListType<typename V::STRUCT_TYPE>>>();
	auto result_data = FlatVector::Writer<string_t>(target_vec, row_count, result_offset);

	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		const auto mult_entry = mult_iter[row_idx];
		if (!mult_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}

		const auto line_count = mult_entry.GetListLength();

		// byte order + type/meta + line count
		idx_t blob_size = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t);
		for (const auto line_entry : mult_entry.GetChildValues()) {
			// line header (byte order + type/meta + vertex count) + vertex data
			blob_size += sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t) + line_entry.GetListLength() * sizeof(V);
		}

		auto &blob = result_data.WriteEmptyString(blob_size);
		FixedSizeBlobWriter writer(blob.GetDataWriteable(), static_cast<uint32_t>(blob_size));

		const auto meta =
		    static_cast<uint32_t>(GeometryType::MULTILINESTRING) + (V::HAS_Z ? 1000 : 0) + (V::HAS_M ? 2000 : 0);

		writer.Write<uint8_t>(1);
		writer.Write<uint32_t>(meta);
		writer.Write<uint32_t>(UnsafeNumericCast<uint32_t>(line_count));

		const auto line_meta =
		    static_cast<uint32_t>(GeometryType::LINESTRING) + (V::HAS_Z ? 1000 : 0) + (V::HAS_M ? 2000 : 0);
		for (const auto line_entry : mult_entry.GetChildValues()) {
			writer.Write<uint8_t>(1);
			writer.Write<uint32_t>(line_meta);
			writer.Write<uint32_t>(UnsafeNumericCast<uint32_t>(line_entry.GetListLength()));
			for (const auto vert_entry : line_entry.GetChildValues()) {
				vert_entry.ForEach([&](const auto &v) { writer.Write<double>(v.GetValueUnsafe()); });
			}
		}

		blob.Finalize();
	}
}

template <class V = VertexXY>
static void ToMultiPolygons(const Vector &source_vec, Vector &target_vec, idx_t row_count) {
	source_vec.Flatten();
	const auto geom_data = FlatVector::GetData<string_t>(source_vec);
	auto mult_writer = FlatVector::Writer<VectorListType<VectorListType<VectorListType<typename V::STRUCT_TYPE>>>>(
	    target_vec, row_count);
	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		if (FlatVector::IsNull(source_vec, row_idx)) {
			mult_writer.WriteNull();
			continue;
		}
		const auto &blob = geom_data[row_idx];
		BlobReader reader(blob.GetData(), static_cast<uint32_t>(blob.GetSize()));
		reader.Skip(sizeof(uint8_t) + sizeof(uint32_t));
		const auto poly_count = reader.Read<uint32_t>();
		for (auto &poly_writer : mult_writer.WriteList(poly_count)) {
			reader.Skip(sizeof(uint8_t) + sizeof(uint32_t));
			const auto ring_count = reader.Read<uint32_t>();
			for (auto &ring_writer : poly_writer.WriteList(ring_count)) {
				const auto vert_count = reader.Read<uint32_t>();
				for (auto &vert_writer : ring_writer.WriteList(vert_count)) {
					vert_writer.ForEach([&](auto &dim_writer) { dim_writer.WriteValue(reader.Read<double>()); });
				}
			}
		}
	}
}

template <class V = VertexXY>
static void FromMultiPolygons(const Vector &source_vec, Vector &target_vec, idx_t row_count, idx_t result_offset) {
	auto mult_iter = source_vec.Values<VectorListType<VectorListType<VectorListType<typename V::STRUCT_TYPE>>>>();
	auto result_data = FlatVector::Writer<string_t>(target_vec, row_count, result_offset);

	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		const auto mult_entry = mult_iter[row_idx];
		if (!mult_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}

		const auto poly_count = mult_entry.GetListLength();

		// byte order + type/meta + polygon count
		idx_t blob_size = sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t);
		for (const auto poly_entry : mult_entry.GetChildValues()) {
			// polygon header (byte order + type/meta + ring count)
			blob_size += sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t);
			for (const auto ring_entry : poly_entry.GetChildValues()) {
				blob_size += sizeof(uint32_t) + ring_entry.GetListLength() * sizeof(V);
			}
		}

		auto &blob = result_data.WriteEmptyString(blob_size);
		FixedSizeBlobWriter writer(blob.GetDataWriteable(), static_cast<uint32_t>(blob_size));

		const auto meta =
		    static_cast<uint32_t>(GeometryType::MULTIPOLYGON) + (V::HAS_Z ? 1000 : 0) + (V::HAS_M ? 2000 : 0);
		writer.Write<uint8_t>(1);
		writer.Write<uint32_t>(meta);
		writer.Write<uint32_t>(UnsafeNumericCast<uint32_t>(poly_count));

		const auto poly_meta =
		    static_cast<uint32_t>(GeometryType::POLYGON) + (V::HAS_Z ? 1000 : 0) + (V::HAS_M ? 2000 : 0);
		for (const auto poly_entry : mult_entry.GetChildValues()) {
			writer.Write<uint8_t>(1);
			writer.Write<uint32_t>(poly_meta);
			writer.Write<uint32_t>(UnsafeNumericCast<uint32_t>(poly_entry.GetListLength()));

			for (const auto ring_entry : poly_entry.GetChildValues()) {
				writer.Write<uint32_t>(UnsafeNumericCast<uint32_t>(ring_entry.GetListLength()));
				for (const auto vert_entry : ring_entry.GetChildValues()) {
					vert_entry.ForEach([&](const auto &v) { writer.Write<double>(v.GetValueUnsafe()); });
				}
			}
		}

		blob.Finalize();
	}
}

template <class V = VertexXY>
static void ToVectorizedFormatInternal(const Vector &source, Vector &target, idx_t count, GeometryType geom_type) {
	switch (geom_type) {
	case GeometryType::POINT:
		ToPoints<V>(source, target, count);
		break;
	case GeometryType::LINESTRING:
		ToLineStrings<V>(source, target, count);
		break;
	case GeometryType::POLYGON:
		ToPolygons<V>(source, target, count);
		break;
	case GeometryType::MULTIPOINT:
		ToMultiPoints<V>(source, target, count);
		break;
	case GeometryType::MULTILINESTRING:
		ToMultiLineStrings<V>(source, target, count);
		break;
	case GeometryType::MULTIPOLYGON:
		ToMultiPolygons<V>(source, target, count);
		break;
	default:
		throw NotImplementedException("Unsupported geometry type %d", static_cast<int>(geom_type));
	}
}

void Geometry::ToVectorizedFormat(const Vector &source, Vector &target, idx_t count, GeometryType geom_type,
                                  VertexType vert_type) {
	switch (vert_type) {
	case VertexType::XY:
		ToVectorizedFormatInternal<VertexXY>(source, target, count, geom_type);
		break;
	case VertexType::XYZ:
		ToVectorizedFormatInternal<VertexXYZ>(source, target, count, geom_type);
		break;
	case VertexType::XYM:
		ToVectorizedFormatInternal<VertexXYM>(source, target, count, geom_type);
		break;
	case VertexType::XYZM:
		ToVectorizedFormatInternal<VertexXYZM>(source, target, count, geom_type);
		break;
	default:
		throw InvalidInputException("Unsupported vertex type %d", static_cast<int>(vert_type));
	}
}

template <class V = VertexXY>
static void FromVectorizedFormatInternal(const Vector &source, Vector &target, idx_t count, GeometryType geom_type,
                                         idx_t result_offset) {
	switch (geom_type) {
	case GeometryType::POINT:
		FromPoints<V>(source, target, count, result_offset);
		break;
	case GeometryType::LINESTRING:
		FromLineStrings<V>(source, target, count, result_offset);
		break;
	case GeometryType::POLYGON:
		FromPolygons<V>(source, target, count, result_offset);
		break;
	case GeometryType::MULTIPOINT:
		FromMultiPoints<V>(source, target, count, result_offset);
		break;
	case GeometryType::MULTILINESTRING:
		FromMultiLineStrings<V>(source, target, count, result_offset);
		break;
	case GeometryType::MULTIPOLYGON:
		FromMultiPolygons<V>(source, target, count, result_offset);
		break;
	default:
		throw NotImplementedException("Unsupported geometry type %d", static_cast<int>(geom_type));
	}
}

void Geometry::FromVectorizedFormat(const Vector &source, Vector &target, idx_t count, GeometryType geom_type,
                                    VertexType vert_type, idx_t result_offset) {
	switch (vert_type) {
	case VertexType::XY:
		FromVectorizedFormatInternal<VertexXY>(source, target, count, geom_type, result_offset);
		break;
	case VertexType::XYZ:
		FromVectorizedFormatInternal<VertexXYZ>(source, target, count, geom_type, result_offset);
		break;
	case VertexType::XYM:
		FromVectorizedFormatInternal<VertexXYM>(source, target, count, geom_type, result_offset);
		break;
	case VertexType::XYZM:
		FromVectorizedFormatInternal<VertexXYZM>(source, target, count, geom_type, result_offset);
		break;
	default:
		throw InvalidInputException("Unsupported vertex type %d", static_cast<int>(vert_type));
	}
}

static LogicalType GetVectorizedTypeInternal(GeometryType geom_type, LogicalType vertex_type) {
	switch (geom_type) {
	case GeometryType::POINT:
		return vertex_type;
	case GeometryType::LINESTRING:
		return LogicalType::LIST(vertex_type);
	case GeometryType::POLYGON:
		return LogicalType::LIST(LogicalType::LIST(vertex_type));
	case GeometryType::MULTIPOINT:
		return LogicalType::LIST(vertex_type);
	case GeometryType::MULTILINESTRING:
		return LogicalType::LIST(LogicalType::LIST(vertex_type));
	case GeometryType::MULTIPOLYGON:
		return LogicalType::LIST(LogicalType::LIST(LogicalType::LIST(vertex_type)));
	case GeometryType::GEOMETRYCOLLECTION:
		throw NotImplementedException("GEOMETRYCOLLECTION vectorized type not implemented");
	default:
		throw InvalidInputException("Unsupported geometry type %d", static_cast<int>(geom_type));
	}
}

pair<GeometryType, VertexType> Geometry::GetSpecializedType(GeometryStorageType type) {
	switch (type) {
	case GeometryStorageType::POINT_XY:
		return {GeometryType::POINT, VertexType::XY};
	case GeometryStorageType::POINT_XYZ:
		return {GeometryType::POINT, VertexType::XYZ};
	case GeometryStorageType::POINT_XYM:
		return {GeometryType::POINT, VertexType::XYM};
	case GeometryStorageType::POINT_XYZM:
		return {GeometryType::POINT, VertexType::XYZM};
	case GeometryStorageType::LINESTRING_XY:
		return {GeometryType::LINESTRING, VertexType::XY};
	case GeometryStorageType::LINESTRING_XYZ:
		return {GeometryType::LINESTRING, VertexType::XYZ};
	case GeometryStorageType::LINESTRING_XYM:
		return {GeometryType::LINESTRING, VertexType::XYM};
	case GeometryStorageType::LINESTRING_XYZM:
		return {GeometryType::LINESTRING, VertexType::XYZM};
	case GeometryStorageType::POLYGON_XY:
		return {GeometryType::POLYGON, VertexType::XY};
	case GeometryStorageType::POLYGON_XYZ:
		return {GeometryType::POLYGON, VertexType::XYZ};
	case GeometryStorageType::POLYGON_XYM:
		return {GeometryType::POLYGON, VertexType::XYM};
	case GeometryStorageType::POLYGON_XYZM:
		return {GeometryType::POLYGON, VertexType::XYZM};
	case GeometryStorageType::MULTIPOINT_XY:
		return {GeometryType::MULTIPOINT, VertexType::XY};
	case GeometryStorageType::MULTIPOINT_XYZ:
		return {GeometryType::MULTIPOINT, VertexType::XYZ};
	case GeometryStorageType::MULTIPOINT_XYM:
		return {GeometryType::MULTIPOINT, VertexType::XYM};
	case GeometryStorageType::MULTIPOINT_XYZM:
		return {GeometryType::MULTIPOINT, VertexType::XYZM};
	case GeometryStorageType::MULTILINESTRING_XY:
		return {GeometryType::MULTILINESTRING, VertexType::XY};
	case GeometryStorageType::MULTILINESTRING_XYZ:
		return {GeometryType::MULTILINESTRING, VertexType::XYZ};
	case GeometryStorageType::MULTILINESTRING_XYM:
		return {GeometryType::MULTILINESTRING, VertexType::XYM};
	case GeometryStorageType::MULTILINESTRING_XYZM:
		return {GeometryType::MULTILINESTRING, VertexType::XYZM};
	case GeometryStorageType::MULTIPOLYGON_XY:
		return {GeometryType::MULTIPOLYGON, VertexType::XY};
	case GeometryStorageType::MULTIPOLYGON_XYZ:
		return {GeometryType::MULTIPOLYGON, VertexType::XYZ};
	case GeometryStorageType::MULTIPOLYGON_XYM:
		return {GeometryType::MULTIPOLYGON, VertexType::XYM};
	case GeometryStorageType::MULTIPOLYGON_XYZM:
		return {GeometryType::MULTIPOLYGON, VertexType::XYZM};
	default:
		throw InvalidInputException("Unsupported GeometryStorageType %d", static_cast<int>(type));
	}
}

LogicalType Geometry::GetVectorizedType(GeometryStorageType type) {
	if (type == GeometryStorageType::WKB) {
		return LogicalType::GEOMETRY();
	}

	if (type == GeometryStorageType::SPATIAL) {
		return GetSpatialGeometryType();
	}

	const auto types = GetSpecializedType(type);

	return GetVectorizedType(types.first, types.second);
}

LogicalType Geometry::GetVectorizedType(GeometryType geom_type, VertexType vert_type) {
	switch (vert_type) {
	case VertexType::XY: {
		auto vert = LogicalType::STRUCT({{"x", LogicalType::DOUBLE}, {"y", LogicalType::DOUBLE}});
		return GetVectorizedTypeInternal(geom_type, std::move(vert));
	}
	case VertexType::XYZ: {
		auto vert =
		    LogicalType::STRUCT({{"x", LogicalType::DOUBLE}, {"y", LogicalType::DOUBLE}, {"z", LogicalType::DOUBLE}});
		return GetVectorizedTypeInternal(geom_type, std::move(vert));
	}
	case VertexType::XYM: {
		auto vert =
		    LogicalType::STRUCT({{"x", LogicalType::DOUBLE}, {"y", LogicalType::DOUBLE}, {"m", LogicalType::DOUBLE}});
		return GetVectorizedTypeInternal(geom_type, std::move(vert));
	}
	case VertexType::XYZM: {
		auto vert = LogicalType::STRUCT({{"x", LogicalType::DOUBLE},
		                                 {"y", LogicalType::DOUBLE},
		                                 {"z", LogicalType::DOUBLE},
		                                 {"m", LogicalType::DOUBLE}});
		return GetVectorizedTypeInternal(geom_type, std::move(vert));
	}
	default:
		throw InvalidInputException("Unsupported vertex type %d", static_cast<int>(vert_type));
	}
}

//----------------------------------------------------------------------------------------------------------------------
// LEGACY GEOMETRY CONVERSION
//----------------------------------------------------------------------------------------------------------------------

static uint32_t FromLegacyGeometryRequiredSize(BlobReader &reader) {
	reader.Skip(sizeof(uint8_t)); // type
	const auto flags = reader.Read<uint8_t>();
	reader.Skip(sizeof(uint16_t));
	reader.Skip(sizeof(uint32_t)); // padding

	// Parse flags
	const auto has_z = (flags & 0x01) != 0;
	const auto has_m = (flags & 0x02) != 0;
	const auto has_bbox = (flags & 0x04) != 0;

	const auto format_v1 = (flags & 0x40) != 0;
	const auto format_v0 = (flags & 0x80) != 0;

	if (format_v1 || format_v0) {
		// Unsupported version, throw an error
		throw InvalidInputException("Cannot import an unknown version of the spatial geometry format!");
	}

	if (has_bbox) {
		// Skip past bbox if present
		reader.Skip(sizeof(float) * 2 * (2 + has_z + has_m));
	}

	// Create root geometry
	const auto vert_width = (2 + has_z + has_m) * sizeof(double);

	uint32_t total_size = 0;
	while (!reader.IsAtEnd()) {
		const auto type = static_cast<GeometryType>(reader.Read<uint32_t>() + 1);
		const auto size = reader.Read<uint32_t>();

		// Endianness + type
		total_size += sizeof(uint8_t) + sizeof(uint32_t);

		switch (type) {
		case GeometryType::POINT: {
			// Points have a fixed size
			reader.Skip(size * vert_width);
			total_size += vert_width;
		} break;
		case GeometryType::LINESTRING: {
			reader.Skip(size * vert_width);
			total_size += sizeof(uint32_t) + (size * vert_width);
		} break;
		case GeometryType::POLYGON: {
			total_size += sizeof(uint32_t); // ring count
			auto ring_reader = reader;
			reader.Skip(size * sizeof(uint32_t) + (size % 2) * sizeof(uint32_t));
			for (uint32_t ring_idx = 0; ring_idx < size; ring_idx++) {
				const auto ring_size = ring_reader.Read<uint32_t>();
				reader.Skip(vert_width * ring_size);
				total_size += sizeof(uint32_t) + ring_size * vert_width;
			}
		} break;
		case GeometryType::MULTIPOINT:
		case GeometryType::MULTILINESTRING:
		case GeometryType::MULTIPOLYGON:
		case GeometryType::GEOMETRYCOLLECTION: {
			total_size += sizeof(uint32_t); // item count
		} break;
		default:
			throw InvalidInputException("Unsupported geometry type in legacy geometry!");
		}
	}
	return total_size;
}

static void FromLegacyGeometryConversion(BlobReader &reader, FixedSizeBlobWriter &writer) {
	reader.Skip(sizeof(uint8_t)); // type
	const auto flags = reader.Read<uint8_t>();
	reader.Skip(sizeof(uint16_t));
	reader.Skip(sizeof(uint32_t)); // padding

	// Parse flags
	const auto has_z = (flags & 0x01) != 0;
	const auto has_m = (flags & 0x02) != 0;
	const auto has_bbox = (flags & 0x04) != 0;

	const auto format_v1 = (flags & 0x40) != 0;
	const auto format_v0 = (flags & 0x80) != 0;

	if (format_v1 || format_v0) {
		// Unsupported version, throw an error
		throw InvalidInputException("Cannot import an unknown version of the spatial geometry format!");
	}

	if (has_bbox) {
		// Skip past bbox if present
		reader.Skip(sizeof(float) * 2 * (2 + has_z + has_m));
	}

	// Create root geometry
	const auto vert_width = (2 + has_z + has_m) * sizeof(double);

	while (!reader.IsAtEnd()) {
		const auto type = static_cast<GeometryType>(reader.Read<uint32_t>() + 1);
		const auto size = reader.Read<uint32_t>();

		// Write endianness + type
		const auto meta = static_cast<uint32_t>(type) + (has_z ? 1 : 0) * 1000 + (has_m ? 2 : 0) * 1000;

		writer.Write<uint8_t>(1); // little endian
		writer.Write<uint32_t>(meta);

		switch (type) {
		case GeometryType::POINT: {
			if (size == 0) {
				constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
				constexpr double empty[4] = {nan, nan, nan, nan};
				writer.Write(reinterpret_cast<const char *>(empty), vert_width);
			} else {
				const auto vert_data = reader.Reserve(vert_width);
				writer.Write(vert_data, vert_width);
			}
		} break;
		case GeometryType::LINESTRING: {
			writer.Write<uint32_t>(size);

			const auto vert_size = vert_width * size;
			const auto vert_data = reader.Reserve(vert_size);
			writer.Write(vert_data, vert_size);
		} break;
		case GeometryType::POLYGON: {
			writer.Write<uint32_t>(size); // ring count
			auto ring_reader = reader;
			reader.Skip(size * sizeof(uint32_t) + (size % 2) * sizeof(uint32_t));
			for (uint32_t ring_idx = 0; ring_idx < size; ring_idx++) {
				const auto ring_size = ring_reader.Read<uint32_t>();
				writer.Write<uint32_t>(ring_size);

				const auto vert_size = vert_width * ring_size;
				const auto vert_data = reader.Reserve(vert_size);

				writer.Write(vert_data, vert_size);
			}
		} break;
		case GeometryType::MULTIPOINT:
		case GeometryType::MULTILINESTRING:
		case GeometryType::MULTIPOLYGON:
		case GeometryType::GEOMETRYCOLLECTION: {
			writer.Write<uint32_t>(size); // item count
		} break;
		default:
			throw InvalidInputException("Unsupported geometry type in legacy geometry!");
		}
	}
}

void Geometry::FromSpatialGeometry(const string_t &source, string_t &target, Vector &vector) {
	BlobReader reader(source.GetData(), static_cast<uint32_t>(source.GetSize()));
	const auto required_size = FromLegacyGeometryRequiredSize(reader);

	reader.Reset();

	auto blob = StringVector::EmptyString(vector, required_size);
	const auto blob_data = blob.GetDataWriteable();
	FixedSizeBlobWriter writer(blob_data, required_size);

	FromLegacyGeometryConversion(reader, writer);

	blob.Finalize();
	target = blob;
}

void Geometry::FromSpatialGeometry(const Vector &source_vec, Vector &target_vec, idx_t count, idx_t result_offset) {
	auto entries = source_vec.Values<string_t>();
	auto target_data = FlatVector::GetDataMutable<string_t>(target_vec);

	auto &target_mask = FlatVector::ValidityMutable(target_vec);

	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		auto entry = entries[row_idx];
		const auto res_idx = result_offset + row_idx;

		if (!entry.IsValid()) {
			target_mask.EnsureWritable();
			target_mask.SetInvalid(res_idx);
			continue;
		}

		const auto &source = entry.GetValue();
		BlobReader reader(source.GetData(), static_cast<uint32_t>(source.GetSize()));
		const auto required_size = FromLegacyGeometryRequiredSize(reader);

		reader.Reset();

		auto blob = StringVector::EmptyString(target_vec, required_size);
		const auto blob_data = blob.GetDataWriteable();
		FixedSizeBlobWriter writer(blob_data, required_size);

		FromLegacyGeometryConversion(reader, writer);
		blob.Finalize();
		target_data[res_idx] = blob;
	}
}

void Geometry::FromSpatialGeometry(const string_t &source, string &target) {
	BlobReader reader(source.GetData(), static_cast<uint32_t>(source.GetSize()));
	const auto required_size = FromLegacyGeometryRequiredSize(reader);

	reader.Reset();

	target.resize(required_size);
	FixedSizeBlobWriter writer(&target[0], required_size);

	FromLegacyGeometryConversion(reader, writer);
}

namespace {

struct ToSpatialGeometryState {
	GeometryExtent extent = GeometryExtent::Empty();
	BlobReader reader = BlobReader(nullptr, 0);
	GeometryType root_type = GeometryType::INVALID;
	bool root_hasz = false;
	bool root_hasm = false;
	bool root_bbox = false;
	uint32_t required_size = 0;
};

} // namespace

template <class V = VertexXY>
static void ToSpatialGeometryAnalyzeInternal(GeometryType type, ToSpatialGeometryState &state) {
	state.required_size += sizeof(uint32_t) + sizeof(uint32_t); // type + size

	switch (type) {
	case GeometryType::POINT: {
		auto vert = state.reader.Read<V>();
		if (vert.AllNan()) {
			// Empty point, skip
			return;
		}

		state.extent.Extend(vert);
		state.required_size += sizeof(V);

	} break;
	case GeometryType::LINESTRING: {
		const auto vert_count = state.reader.Read<uint32_t>();
		for (uint32_t vert_idx = 0; vert_idx < vert_count; vert_idx++) {
			auto vert = state.reader.Read<V>();

			state.extent.Extend(vert);
			state.required_size += sizeof(V);
		}

	} break;
	case GeometryType::POLYGON: {
		const auto ring_count = state.reader.Read<uint32_t>();
		for (uint32_t ring_idx = 0; ring_idx < ring_count; ring_idx++) {
			state.required_size += sizeof(uint32_t); // ring count

			const auto vert_count = state.reader.Read<uint32_t>();
			for (uint32_t vert_idx = 0; vert_idx < vert_count; vert_idx++) {
				auto vert = state.reader.Read<V>();

				state.extent.Extend(vert);
				state.required_size += sizeof(V);
			}
		}

		if (ring_count % 2 == 1) {
			state.required_size += sizeof(uint32_t); // padding for odd ring count
		}

	} break;
	case GeometryType::MULTIPOINT:
	case GeometryType::MULTILINESTRING:
	case GeometryType::MULTIPOLYGON:
	case GeometryType::GEOMETRYCOLLECTION: {
		state.reader.Skip(sizeof(uint32_t)); // item count
	} break;
	default:
		throw InvalidInputException("Unsupported geometry type in legacy geometry!");
	}
}

static void ToSpatialGeometryAnalyze(ToSpatialGeometryState &state) {
	auto &reader = state.reader;

	while (!reader.IsAtEnd()) {
		const auto le = reader.Read<uint8_t>();
		if (le != 1) {
			throw InvalidInputException(
			    "Only little-endian legacy geometries are supported for conversion to spatial format!");
		}

		const auto type_id = reader.Read<uint32_t>();
		const auto flag = type_id / 1000;
		const auto type = static_cast<GeometryType>(type_id % 1000);
		const auto has_z = (flag & 0x01) != 0;
		const auto has_m = (flag & 0x02) != 0;

		if (state.root_type == GeometryType::INVALID) {
			state.root_type = type;
			state.root_hasz = has_z;
			state.root_hasm = has_m;
		} else {
			if (state.root_hasz != has_z) {
				throw InvalidInputException("All geometries in a spatial blob must have the same Z presence");
			}
			if (state.root_hasm != has_m) {
				throw InvalidInputException("All geometries in a spatial blob must have the same M presence");
			}
		}

		if (has_z && has_m) {
			ToSpatialGeometryAnalyzeInternal<VertexXYZM>(type, state);
		} else if (has_z) {
			ToSpatialGeometryAnalyzeInternal<VertexXYZ>(type, state);
		} else if (has_m) {
			ToSpatialGeometryAnalyzeInternal<VertexXYM>(type, state);
		} else {
			ToSpatialGeometryAnalyzeInternal<VertexXY>(type, state);
		}
	}

	// Always require 8 bytes for header
	state.required_size += sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint32_t);

	if (state.root_type != GeometryType::POINT) {
		state.root_bbox = true;
		state.required_size += sizeof(float) * (2 + state.root_hasz + state.root_hasm) * 2; // bbox size
	} else {
		state.root_bbox = false;
	}
}

static float DoubleToFloatDown(double d) {
	if (d > static_cast<double>(std::numeric_limits<float>::max())) {
		return std::numeric_limits<float>::max();
	}
	if (d <= static_cast<double>(std::numeric_limits<float>::lowest())) {
		return std::numeric_limits<float>::lowest();
	}

	auto f = static_cast<float>(d);
	if (static_cast<double>(f) <= d) {
		return f;
	}
	return std::nextafter(f, std::numeric_limits<float>::lowest());
}

static float DoubleToFloatUp(double d) {
	if (d >= static_cast<double>(std::numeric_limits<float>::max())) {
		return std::numeric_limits<float>::max();
	}
	if (d < static_cast<double>(std::numeric_limits<float>::lowest())) {
		return std::numeric_limits<float>::lowest();
	}

	auto f = static_cast<float>(d);
	if (static_cast<double>(f) >= d) {
		return f;
	}
	return std::nextafter(f, std::numeric_limits<float>::max());
}

static void ToSpatialGeometryConvert(ToSpatialGeometryState &state, FixedSizeBlobWriter &writer) {
	auto &reader = state.reader;

	uint8_t flags = 0;
	if (state.root_hasz) {
		flags |= 0x01;
	}
	if (state.root_hasm) {
		flags |= 0x02;
	}
	if (state.root_bbox) {
		flags |= 0x04;
	}

	writer.Write<uint8_t>(static_cast<uint8_t>(state.root_type) - 1); // legacy format is 0-based, subtract 1
	writer.Write<uint8_t>(flags);
	writer.Write<uint16_t>(0); // reserved
	writer.Write<uint32_t>(0); // padding

	if (state.root_bbox) {
		// Write bbox
		writer.Write<float>(DoubleToFloatDown(state.extent.x_min));
		writer.Write<float>(DoubleToFloatDown(state.extent.y_min));
		writer.Write<float>(DoubleToFloatUp(state.extent.x_max));
		writer.Write<float>(DoubleToFloatUp(state.extent.y_max));

		if (state.root_hasz) {
			writer.Write<float>(DoubleToFloatDown(state.extent.z_min));
			writer.Write<float>(DoubleToFloatUp(state.extent.z_max));
		}
		if (state.root_hasm) {
			writer.Write<float>(DoubleToFloatDown(state.extent.m_min));
			writer.Write<float>(DoubleToFloatUp(state.extent.m_max));
		}
	}

	const auto vert_width = (2 + state.root_hasz + state.root_hasm) * sizeof(double);

	while (!reader.IsAtEnd()) {
		const auto le = reader.Read<uint8_t>();

		if (le != 1) {
			throw InvalidInputException(
			    "Only little-endian geometries are supported for conversion to spatial format!");
		}

		const auto type_id = reader.Read<uint32_t>();
		const auto type = static_cast<GeometryType>(type_id % 1000);

		writer.Write<uint32_t>(static_cast<uint32_t>(type) - 1); // spatial format is 0-based, subtract 1

		switch (type) {
		case GeometryType::POINT: {
			const auto vert_data = reader.Reserve(vert_width);

			constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
			double empty[4] = {nan, nan, nan, nan};

			memcpy(empty, vert_data, vert_width); // copy vertex data, will overwrite leading dimensions if present
			auto is_empty = true;
			for (auto &d : empty) {
				if (!std::isnan(d)) {
					is_empty = false;
					break;
				}
			}
			if (is_empty) {
				writer.Write<uint32_t>(0); // empty point has 0 vertices
			} else {
				writer.Write<uint32_t>(1); // point always has 1 vertex
				writer.Write(vert_data, vert_width);
			}
		} break;
		case GeometryType::LINESTRING: {
			const auto vert_count = reader.Read<uint32_t>();
			writer.Write<uint32_t>(vert_count);

			const auto vert_size = vert_width * vert_count;
			const auto vert_data = reader.Reserve(vert_size);
			writer.Write(vert_data, vert_size);
		} break;
		case GeometryType::POLYGON: {
			const auto ring_count = reader.Read<uint32_t>();
			writer.Write<uint32_t>(ring_count); // ring count
			auto ring_writer = writer;
			writer.Skip(ring_count * sizeof(uint32_t) + (ring_count % 2) * sizeof(uint32_t));

			for (uint32_t ring_idx = 0; ring_idx < ring_count; ring_idx++) {
				const auto ring_size = reader.Read<uint32_t>();
				ring_writer.Write<uint32_t>(ring_size);

				const auto vert_size = vert_width * ring_size;
				const auto vert_data = reader.Reserve(vert_size);
				writer.Write(vert_data, vert_size);
			}

			if (ring_count % 2 == 1) {
				ring_writer.Write<uint32_t>(0); // padding for odd ring count
			}
		} break;
		case GeometryType::MULTIPOINT:
		case GeometryType::MULTILINESTRING:
		case GeometryType::MULTIPOLYGON:
		case GeometryType::GEOMETRYCOLLECTION: {
			const auto item_count = reader.Read<uint32_t>();
			writer.Write<uint32_t>(item_count); // item count
		} break;
		case GeometryType::INVALID:
			throw InvalidInputException("Unsupported geometry type in legacy geometry!");
		}
	}
}

void Geometry::ToSpatialGeometry(const string_t &source, string_t &target, Vector &vector) {
	ToSpatialGeometryState state;
	state.reader = BlobReader(source.GetData(), static_cast<uint32_t>(source.GetSize()));

	ToSpatialGeometryAnalyze(state);

	state.reader.Reset();

	auto blob = StringVector::EmptyString(vector, state.required_size);
	const auto blob_data = blob.GetDataWriteable();
	FixedSizeBlobWriter writer(blob_data, state.required_size);

	ToSpatialGeometryConvert(state, writer);

	blob.Finalize();
	target = blob;
}

void Geometry::ToSpatialGeometry(const Vector &source, Vector &target, idx_t count) {
	UnaryExecutor::Execute<string_t, string_t>(source, target, count, [&](const string_t &source) {
		string_t result;
		ToSpatialGeometry(source, result, target);
		return result;
	});
}

void Geometry::ToSpatialGeometry(const string_t &source, string &target) {
	ToSpatialGeometryState state;
	state.reader = BlobReader(source.GetData(), static_cast<uint32_t>(source.GetSize()));

	ToSpatialGeometryAnalyze(state);

	state.reader.Reset();

	target.resize(state.required_size);
	FixedSizeBlobWriter writer(&target[0], state.required_size);

	ToSpatialGeometryConvert(state, writer);
}

void Geometry::ToVectorizedFormat(const Vector &source, Vector &target, idx_t count, GeometryStorageType type) {
	if (type == GeometryStorageType::SPATIAL) {
		ToSpatialGeometry(source, target, count);
		return;
	}

	const auto types = GetSpecializedType(type);
	ToVectorizedFormat(source, target, count, types.first, types.second);
}

void Geometry::FromVectorizedFormat(const Vector &source, Vector &target, idx_t count, GeometryStorageType type,
                                    idx_t result_offset) {
	if (type == GeometryStorageType::SPATIAL) {
		FromSpatialGeometry(source, target, count, result_offset);
		return;
	}

	const auto types = GetSpecializedType(type);
	FromVectorizedFormat(source, target, count, types.first, types.second, result_offset);
}

LogicalType Geometry::GetSpatialGeometryType() {
	return LogicalType(LogicalTypeId::BLOB).WithAlias("GEOMETRY");
}

bool Geometry::IsSpatialGeometryType(const LogicalType &type) {
	return type.id() == LogicalTypeId::BLOB && type.HasAlias() && type.GetAlias() == "GEOMETRY";
}

} // namespace duckdb
