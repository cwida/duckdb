// Arrow scans of a NULL STRUCT row whose FIXED_SIZE_BINARY child has its own NULL bitmap.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"

#include <string>
#include <vector>

using namespace duckdb;

namespace arrow_struct_fixed_size_binary_test {

void ReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void ReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

// Sets the bits of the given rows to NULL in a validity bitmap that starts all valid.
std::vector<uint8_t> NullBitmap(idx_t length, const std::vector<idx_t> &null_rows) {
	std::vector<uint8_t> validity((length + 7) / 8 + 1, 0xFF);
	for (auto row : null_rows) {
		validity[row / 8] &= uint8_t(~(1u << (row % 8)));
	}
	return validity;
}

// utf8 child with the given values, NULL for the given rows.
struct StringChild {
	explicit StringChild(const char *name_p, const std::vector<string> &values, const std::vector<idx_t> &null_rows)
	    : name(name_p), validity(NullBitmap(idx_t(values.size()), null_rows)) {
		offsets.push_back(0);
		for (auto &value : values) {
			data += value;
			offsets.push_back(int32_t(data.size()));
		}
		schema.format = "u";
		schema.name = name.c_str();
		schema.flags = 2;
		schema.release = ReleaseSchema;
		buffers[0] = null_rows.empty() ? nullptr : validity.data();
		buffers[1] = offsets.data();
		buffers[2] = data.data();
		array.length = int64_t(values.size());
		array.null_count = int64_t(null_rows.size());
		array.n_buffers = 3;
		array.buffers = buffers;
		array.release = ReleaseArray;
	}
	StringChild(const StringChild &) = delete;

	string name;
	std::vector<uint8_t> validity;
	std::vector<int32_t> offsets;
	string data;
	const void *buffers[3];
	ArrowSchema schema {};
	ArrowArray array {};
};

// fixed_size_binary(width) child holding `values`, NULL for the given rows. Rows that are NULL still occupy
// `width` bytes of the data buffer, as the Arrow format requires.
struct FixedSizeBinaryChild {
	FixedSizeBinaryChild(const char *name_p, idx_t width, const std::vector<std::vector<uint8_t>> &values,
	                     const std::vector<idx_t> &null_rows)
	    : name(name_p), validity(NullBitmap(idx_t(values.size()), null_rows)), format("w:" + to_string(width)) {
		for (auto &value : values) {
			D_ASSERT(value.size() == width);
			data.insert(data.end(), value.begin(), value.end());
		}
		schema.format = format.c_str();
		schema.name = name.c_str();
		schema.flags = 2;
		schema.release = ReleaseSchema;
		buffers[0] = null_rows.empty() ? nullptr : validity.data();
		buffers[1] = data.data();
		array.length = int64_t(values.size());
		array.null_count = int64_t(null_rows.size());
		array.n_buffers = 2;
		array.buffers = buffers;
		array.release = ReleaseArray;
	}
	FixedSizeBinaryChild(const FixedSizeBinaryChild &) = delete;

	string name;
	std::vector<uint8_t> validity;
	string format;
	std::vector<uint8_t> data;
	const void *buffers[2];
	ArrowSchema schema {};
	ArrowArray array {};
};

// STRUCT column over the two children, NULL for the given rows.
template <class FIRST, class SECOND>
struct StructColumn {
	StructColumn(FIRST &first, SECOND &second, idx_t length, const std::vector<idx_t> &null_rows)
	    : validity(NullBitmap(length, null_rows)) {
		schema_children[0] = &first.schema;
		schema_children[1] = &second.schema;
		schema.format = "+s";
		schema.name = "c";
		schema.flags = 2;
		schema.n_children = 2;
		schema.children = schema_children;
		schema.release = ReleaseSchema;
		buffers[0] = null_rows.empty() ? nullptr : validity.data();
		array_children[0] = &first.array;
		array_children[1] = &second.array;
		array.length = int64_t(length);
		array.null_count = int64_t(null_rows.size());
		array.n_buffers = 1;
		array.buffers = buffers;
		array.n_children = 2;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	StructColumn(const StructColumn &) = delete;

	std::vector<uint8_t> validity;
	ArrowSchema *schema_children[2];
	ArrowArray *array_children[2];
	const void *buffers[1];
	ArrowSchema schema {};
	ArrowArray array {};
};

// Runs the query over the scan of the column, as the view arrow_view, and checks that it returns expected.
bool QueryMatches(ArrowSchema &col_schema, ArrowArray &col_array, const string &query, const string &expected) {
	ArrowSchema *schema_children[1] = {&col_schema};
	ArrowSchema record_schema {};
	record_schema.format = "+s";
	record_schema.n_children = 1;
	record_schema.children = schema_children;
	record_schema.release = ReleaseSchema;

	ArrowArray *array_children[1] = {&col_array};
	const void *buffers[1] = {nullptr};
	ArrowArray record_array {};
	record_array.length = col_array.length;
	record_array.n_buffers = 1;
	record_array.buffers = buffers;
	record_array.n_children = 1;
	record_array.children = array_children;
	record_array.release = ReleaseArray;

	ArrowArrayStream stream {};
	AdbcError err {};
	if (duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &stream, &err) != ADBC_STATUS_OK) {
		return false;
	}
	DuckDB db(nullptr);
	Connection con(db);
	con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream))->CreateView("arrow_view");
	auto result = con.Query(query);
	auto expected_result = con.Query(expected);
	stream.release = nullptr;
	if (result->HasError() || expected_result->HasError()) {
		return false;
	}
	return result->Equals(*expected_result, false);
}

// The 16 bytes of the two non-NULL uuid values. A NULL row still occupies width bytes of the data buffer, and
// holds a placeholder there.
static const uint8_t UUID_A_BYTES[16] = {0x6f, 0x96, 0x19, 0xff, 0x8b, 0x86, 0xd0, 0x11,
                                         0xb4, 0x2d, 0x00, 0xc0, 0x4f, 0xc9, 0x64, 0xff};
static const uint8_t UUID_C_BYTES[16] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                                         0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

std::vector<uint8_t> Uuid(const uint8_t (&bytes)[16]) {
	return std::vector<uint8_t>(bytes, bytes + 16);
}

std::vector<uint8_t> Placeholder(idx_t width) {
	return std::vector<uint8_t>(width, 0);
}

} // namespace arrow_struct_fixed_size_binary_test

TEST_CASE("Arrow scan of a NULL STRUCT row over a FIXED_SIZE_BINARY child with NULLs", "[arrow]") {
	using namespace arrow_struct_fixed_size_binary_test;
	// Row 0 is the NULL struct, row 2 has a NULL uuid of its own. The NULL bitmap of the uuid child is what
	// makes the conversion re-set the child validity, discarding the NULL inherited from the struct.
	StringChild names("name", {"", "a", "b", "c"}, {});
	FixedSizeBinaryChild uuids("uuid", 16, {Placeholder(16), Uuid(UUID_A_BYTES), Placeholder(16), Uuid(UUID_C_BYTES)},
	                           {2});
	StructColumn<StringChild, FixedSizeBinaryChild> structs(names, uuids, 4, {0});

	// The NULL struct reads as NULL in both children, and every other row keeps its own values.
	REQUIRE(QueryMatches(structs.schema, structs.array,
	                     "SELECT COALESCE(c.name, '-') || '|' || COALESCE(hex(c.uuid), '-') FROM arrow_view",
	                     "SELECT * FROM (VALUES ('-|-'), ('a|6F9619FF8B86D011B42D00C04FC964FF'), ('b|-'), "
	                     "('c|0123456789ABCDEF0123456789ABCDEF')) t(a)"));
	REQUIRE(QueryMatches(structs.schema, structs.array, "SELECT count(*) FROM arrow_view WHERE c IS NULL",
	                     "SELECT 1::BIGINT"));
	REQUIRE(QueryMatches(structs.schema, structs.array, "SELECT count(*) FROM arrow_view WHERE c.uuid IS NULL",
	                     "SELECT 2::BIGINT"));
}
