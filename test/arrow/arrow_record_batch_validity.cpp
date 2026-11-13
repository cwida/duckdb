// Arrow record batches whose top level struct array carries a validity bitmap of its own, read through arrow_scan and
// both C API v1 readers.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb.h"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace duckdb;

namespace arrow_record_batch_validity_test {

void ReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void ReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

// A hand built Arrow field and its array, owning its children, its dictionary and its buffers.
struct Field {
	ArrowSchema schema {};
	ArrowArray array {};
	std::vector<std::unique_ptr<Field>> children;
	std::unique_ptr<Field> dictionary;
	std::vector<int32_t> data;
	std::vector<uint8_t> bytes;
	std::vector<uint8_t> validity;
	std::vector<ArrowSchema *> schema_children;
	std::vector<ArrowArray *> array_children;
	std::vector<const void *> buffers;
};
using FieldPtr = std::unique_ptr<Field>;

FieldPtr MakeField(const char *format, const char *name, int64_t length, std::vector<FieldPtr> children) {
	auto field = std::unique_ptr<Field>(new Field());
	field->schema.format = format;
	field->schema.name = name;
	field->schema.flags = ARROW_FLAG_NULLABLE;
	field->schema.release = ReleaseSchema;
	field->array.length = length;
	field->array.release = ReleaseArray;
	field->children = std::move(children);
	for (auto &child : field->children) {
		field->schema_children.push_back(&child->schema);
		field->array_children.push_back(&child->array);
	}
	field->schema.n_children = int64_t(field->children.size());
	field->schema.children = field->schema_children.empty() ? nullptr : field->schema_children.data();
	field->array.n_children = int64_t(field->children.size());
	field->array.children = field->array_children.empty() ? nullptr : field->array_children.data();
	return field;
}

void SetBuffers(Field &field, std::vector<const void *> buffers) {
	field.buffers = std::move(buffers);
	field.array.n_buffers = int64_t(field.buffers.size());
	field.array.buffers = field.buffers.data();
}

// Clears the validity bits of the given physical slots; a field without null slots keeps no bitmap.
void SetNulls(Field &field, idx_t slots, const std::vector<idx_t> &null_slots) {
	if (null_slots.empty()) {
		return;
	}
	field.validity.assign((slots + 7) / 8, 0xFF);
	for (auto slot : null_slots) {
		field.validity[slot / 8] &= uint8_t(~(1u << (slot % 8)));
	}
	field.buffers[0] = field.validity.data();
	field.array.null_count = int64_t(null_slots.size());
}

FieldPtr Int32(const char *name, std::vector<int32_t> values, const std::vector<idx_t> &null_slots = {}) {
	auto field = MakeField("i", name, int64_t(values.size()), {});
	field->data = std::move(values);
	SetBuffers(*field, {nullptr, field->data.data()});
	SetNulls(*field, field->data.size(), null_slots);
	return field;
}

// Two byte values, the first byte of each is given.
FieldPtr FixedBinary(const char *name, const std::vector<uint8_t> &first, const std::vector<idx_t> &null_slots = {}) {
	auto field = MakeField("w:2", name, int64_t(first.size()), {});
	for (auto byte : first) {
		field->bytes.push_back(byte);
		field->bytes.push_back('!');
	}
	SetBuffers(*field, {nullptr, field->bytes.data()});
	SetNulls(*field, first.size(), null_slots);
	return field;
}

FieldPtr Utf8(const char *name, const std::vector<string> &values) {
	auto field = MakeField("u", name, int64_t(values.size()), {});
	field->data.push_back(0);
	for (auto &value : values) {
		field->bytes.insert(field->bytes.end(), value.begin(), value.end());
		field->data.push_back(int32_t(field->bytes.size()));
	}
	SetBuffers(*field, {nullptr, field->data.data(), field->bytes.data()});
	return field;
}

FieldPtr Sliced(FieldPtr field, int64_t offset) {
	field->array.offset = offset;
	field->array.length -= offset;
	return field;
}

FieldPtr Struct(const char *name, std::vector<FieldPtr> children, const std::vector<idx_t> &null_slots = {},
                int64_t offset = 0) {
	auto slots = children[0]->array.length;
	auto field = MakeField("+s", name, slots - offset, std::move(children));
	field->array.offset = offset;
	SetBuffers(*field, {nullptr});
	SetNulls(*field, idx_t(slots), null_slots);
	// The null count covers only the slots from the offset on.
	field->array.null_count =
	    std::count_if(null_slots.begin(), null_slots.end(), [&](idx_t slot) { return int64_t(slot) >= offset; });
	return field;
}

FieldPtr List(const char *name, std::vector<int32_t> offsets, FieldPtr child,
              const std::vector<idx_t> &null_slots = {}) {
	auto slots = offsets.size() - 1;
	std::vector<FieldPtr> children;
	children.push_back(std::move(child));
	auto field = MakeField("+l", name, int64_t(slots), std::move(children));
	field->data = std::move(offsets);
	SetBuffers(*field, {nullptr, field->data.data()});
	SetNulls(*field, slots, null_slots);
	return field;
}

// Two elements per slot.
FieldPtr FixedSizeList(const char *name, FieldPtr child) {
	auto slots = child->array.length / 2;
	std::vector<FieldPtr> children;
	children.push_back(std::move(child));
	auto field = MakeField("+w:2", name, slots, std::move(children));
	SetBuffers(*field, {nullptr});
	return field;
}

FieldPtr Dictionary(const char *name, std::vector<int32_t> indices, FieldPtr values,
                    const std::vector<idx_t> &null_slots = {}) {
	auto field = Int32(name, std::move(indices), null_slots);
	field->dictionary = std::move(values);
	field->schema.dictionary = &field->dictionary->schema;
	field->array.dictionary = &field->dictionary->array;
	return field;
}

FieldPtr SparseUnion(const char *name, std::vector<int8_t> type_ids, std::vector<FieldPtr> members) {
	auto field = MakeField("+us:0,1", name, int64_t(type_ids.size()), std::move(members));
	for (auto id : type_ids) {
		field->bytes.push_back(uint8_t(id));
	}
	SetBuffers(*field, {field->bytes.data()});
	return field;
}

FieldPtr RunEnd(const char *name, FieldPtr run_ends, FieldPtr values) {
	run_ends->schema.name = "run_ends";
	values->schema.name = "values";
	auto length = int64_t(run_ends->data.back());
	std::vector<FieldPtr> children;
	children.push_back(std::move(run_ends));
	children.push_back(std::move(values));
	auto field = MakeField("+r", name, length, std::move(children));
	SetBuffers(*field, {});
	return field;
}

FieldPtr Record(std::vector<FieldPtr> columns, const std::vector<idx_t> &null_slots = {}, int64_t offset = 0) {
	return Struct("", std::move(columns), null_slots, offset);
}

string RenderValue(const Value &value) {
	if (value.IsNull()) {
		return "NULL";
	}
	auto &type = value.type();
	if (type.id() == LogicalTypeId::UNION) {
		return to_string(UnionValue::GetTag(value)) + ":" + RenderValue(UnionValue::GetValue(value));
	}
	if (StructType::IsStruct(type) || type.id() == LogicalTypeId::LIST) {
		auto &children = StructType::IsStruct(type) ? StructValue::GetChildren(value) : ListValue::GetChildren(value);
		string text;
		for (auto &child : children) {
			text += (text.empty() ? "" : ", ") + RenderValue(child);
		}
		return StructType::IsStruct(type) ? "(" + text + ")" : "[" + text + "]";
	}
	return value.ToString();
}

// Requires that a NULL struct row is NULL in every child, also inside lists and arrays, which only debug builds verify.
void RequireNullStructChildrenNull(Vector &vector, idx_t count) {
	auto &type = vector.GetType();
	if (type.id() == LogicalTypeId::LIST) {
		vector.Flatten();
		Vector child(ListType::GetChildType(type));
		child.Reference(ListVector::GetChild(vector));
		RequireNullStructChildrenNull(child, ListVector::GetListSize(vector));
		return;
	}
	if (type.id() == LogicalTypeId::ARRAY) {
		vector.Flatten();
		Vector child(ArrayType::GetChildType(type));
		child.Reference(ArrayVector::GetChild(vector));
		RequireNullStructChildrenNull(child, count * ArrayType::GetSize(type));
		return;
	}
	if (type.InternalType() != PhysicalType::STRUCT) {
		return;
	}
	vector.Flatten();
	for (auto &child : StructVector::GetEntries(vector)) {
		Vector flat_child(child.GetType());
		flat_child.Reference(child);
		flat_child.Flatten();
		for (idx_t row = 0; row < count; row++) {
			if (FlatVector::IsNull(vector, row)) {
				INFO("row " << row << " of child " << child.GetType().ToString());
				REQUIRE(FlatVector::IsNull(flat_child, row));
			}
		}
		RequireNullStructChildrenNull(flat_child, count);
	}
}

void RenderChunk(DataChunk &chunk, std::vector<string> &rows) {
	chunk.Verify();
	for (auto &column : chunk.data) {
		Vector copy(column.GetType());
		copy.Reference(column);
		RequireNullStructChildrenNull(copy, chunk.size());
	}
	for (idx_t row = 0; row < chunk.size(); row++) {
		string text;
		for (idx_t column = 0; column < chunk.ColumnCount(); column++) {
			text += (column ? "\t" : "") + RenderValue(chunk.GetValue(column, row));
		}
		rows.push_back(text);
	}
}

// Each reader returns the rows it reads, or a single line with its error.
std::vector<string> ScanArrowScan(Field &record) {
	auto schema = record.schema;
	auto array = record.array;
	ArrowArrayStream stream {};
	AdbcError error {};
	REQUIRE(duckdb_adbc::BatchToArrayStream(&array, &schema, &stream, &error) == ADBC_STATUS_OK);
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET memory_limit = '4GB'"));
	auto result = con.TableFunction("arrow_scan", {}, {}, ArrowTestHelper::ConstructArrowScan(stream))->Execute();
	if (result->HasError()) {
		return {"error: " + result->GetError()};
	}
	std::vector<string> rows;
	for (auto chunk = result->Fetch(); chunk; chunk = result->Fetch()) {
		RenderChunk(*chunk, rows);
	}
	if (result->HasError()) {
		return {"error: " + result->GetError()};
	}
	return rows;
}

struct CDatabase {
	CDatabase() {
		REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
		REQUIRE(duckdb_connect(db, &con) == DuckDBSuccess);
		REQUIRE(duckdb_query(con, "SET memory_limit = '4GB'", nullptr) == DuckDBSuccess);
	}
	~CDatabase() {
		duckdb_disconnect(&con);
		duckdb_close(&db);
	}
	duckdb_database db = nullptr;
	duckdb_connection con = nullptr;
};

std::vector<string> ScanCApiArrayScan(Field &record) {
	CDatabase c;
	auto schema = record.schema;
	auto array = record.array;
	duckdb_arrow_stream stream = nullptr;
	REQUIRE(duckdb_arrow_array_scan(c.con, "batch", reinterpret_cast<duckdb_arrow_schema>(&schema),
	                                reinterpret_cast<duckdb_arrow_array>(&array), &stream) == DuckDBSuccess);
	duckdb_result result;
	std::vector<string> rows;
	if (duckdb_query(c.con, "SELECT * FROM batch", &result) != DuckDBSuccess) {
		rows.push_back(string("error: ") + duckdb_result_error(&result));
	} else {
		while (auto chunk = duckdb_fetch_chunk(result)) {
			RenderChunk(*reinterpret_cast<DataChunk *>(chunk), rows);
			duckdb_destroy_data_chunk(&chunk);
		}
	}
	duckdb_destroy_result(&result);
	duckdb_destroy_arrow_stream(&stream);
	return rows;
}

std::vector<string> ScanCApiDataChunk(Field &record) {
	CDatabase c;
	duckdb_arrow_converted_schema converted = nullptr;
	auto schema = record.schema;
	REQUIRE(duckdb_schema_from_arrow(c.con, &schema, &converted) == nullptr);
	auto array = record.array;
	duckdb_data_chunk chunk = nullptr;
	std::vector<string> rows;
	auto error = duckdb_data_chunk_from_arrow(c.con, &array, converted, &chunk);
	if (error) {
		rows.push_back(string("error: ") + duckdb_error_data_message(error));
		duckdb_destroy_error_data(&error);
	} else {
		RenderChunk(*reinterpret_cast<DataChunk *>(chunk), rows);
		duckdb_destroy_data_chunk(&chunk);
	}
	duckdb_destroy_arrow_converted_schema(&converted);
	return rows;
}

void RequireRows(Field &record, const std::vector<string> &expected) {
	SECTION("arrow_scan") {
		REQUIRE(ScanArrowScan(record) == expected);
	}
	SECTION("duckdb_arrow_array_scan") {
		REQUIRE(ScanCApiArrayScan(record) == expected);
	}
	SECTION("duckdb_data_chunk_from_arrow") {
		REQUIRE(ScanCApiDataChunk(record) == expected);
	}
}

void RequireError(Field &record, const string &text) {
	SECTION("arrow_scan") {
		auto rows = ScanArrowScan(record);
		REQUIRE(rows.size() == 1);
		REQUIRE_THAT(rows[0], Catch::Contains("error: ") && Catch::Contains(text));
	}
	SECTION("duckdb_arrow_array_scan") {
		auto rows = ScanCApiArrayScan(record);
		REQUIRE(rows.size() == 1);
		REQUIRE_THAT(rows[0], Catch::Contains("error: ") && Catch::Contains(text));
	}
	SECTION("duckdb_data_chunk_from_arrow") {
		auto rows = ScanCApiDataChunk(record);
		REQUIRE(rows.size() == 1);
		REQUIRE_THAT(rows[0], Catch::Contains("error: ") && Catch::Contains(text));
	}
}

// Eleven columns over six slots, one of each kind a conversion handles on its own. The struct column is NULL at slot 3
// and has a child of each kind that rebuilds or folds the struct's NULL rows.
FieldPtr AllKinds(const std::vector<idx_t> &batch_null_slots, int64_t batch_offset) {
	auto union_members = []() {
		std::vector<FieldPtr> members;
		members.push_back(Int32("i", {1, 2, 3, 4, 5, 6}));
		members.push_back(Int32("j", {10, 20, 30, 40, 50, 60}));
		return members;
	};
	std::vector<FieldPtr> columns;
	columns.push_back(Int32("a", {1, 2, 3, 4, 5, 6}, {4}));
	columns.push_back(Utf8("v", {"a", "b", "c", "d", "e", "f"}));
	columns.push_back(FixedBinary("w", {'a', 'b', 'c', 'd', 'e', 'f'}, {4}));
	{
		std::vector<FieldPtr> members;
		members.push_back(Int32("x", {10, 20, 30, 40, 50, 60}, {5}));
		members.push_back(FixedBinary("w", {'a', 'b', 'c', 'd', 'e', 'f'}, {5}));
		members.push_back(RunEnd("r", Int32("", {2, 4, 6}), Int32("", {7, 8, 9}, {0})));
		members.push_back(SparseUnion("u", {0, 1, 0, 1, 0, 1}, union_members()));
		members.push_back(Dictionary("d", {0, 1, 0, 1, 0, 1}, Int32("", {100, 200}), {4}));
		std::vector<FieldPtr> nested;
		nested.push_back(FixedBinary("w", {'a', 'b', 'c', 'd', 'e', 'f'}, {5}));
		members.push_back(Struct("t", std::move(nested)));
		columns.push_back(Struct("s", std::move(members), {3}));
	}
	columns.push_back(List("l", {0, 1, 2, 3, 4, 5, 6}, Int32("", {1, 2, 3, 4, 5, 6}), {5}));
	columns.push_back(Dictionary("d", {0, 1, 0, 1, 0, 1}, Int32("", {100, 200}), {4}));
	columns.push_back(RunEnd("r", Int32("", {2, 4, 6}), Int32("", {7, 8, 9}, {1})));
	columns.push_back(SparseUnion("u", {0, 1, 0, 1, 0, 1}, union_members()));
	columns.push_back(Sliced(Int32("o", {0, 1, 2, 3, 4, 5, 6}, {6}), 1));
	columns.push_back(FixedSizeList("f", Int32("", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, {7})));
	{
		std::vector<FieldPtr> item;
		item.push_back(FixedBinary("w", {'a', 'b', 'c', 'd', 'e', 'f'}, {4}));
		columns.push_back(List("ls", {0, 1, 2, 3, 4, 5, 6}, Struct("item", std::move(item), {2})));
	}
	return Record(std::move(columns), batch_null_slots, batch_offset);
}

// The rows of AllKinds by slot, when the batch marks none NULL.
const std::vector<string> ALL_KINDS_ROWS = {
    "1\ta\ta!\t(10, a!, NULL, 0:1, 100, (a!))\t[1]\t100\t7\t0:1\t1\t[1, 2]\t[(a!)]",
    "2\tb\tb!\t(20, b!, NULL, 1:20, 200, (b!))\t[2]\t200\t7\t1:20\t2\t[3, 4]\t[(b!)]",
    "3\tc\tc!\t(30, c!, 8, 0:3, 100, (c!))\t[3]\t100\tNULL\t0:3\t3\t[5, 6]\t[NULL]",
    "4\td\td!\tNULL\t[4]\t200\tNULL\t1:40\t4\t[7, NULL]\t[(d!)]",
    "NULL\te\tNULL\t(50, e!, 9, 0:5, NULL, (e!))\t[5]\tNULL\t9\t0:5\t5\t[9, 10]\t[(NULL)]",
    "6\tf\tf!\t(NULL, NULL, 9, 1:60, 200, (NULL))\tNULL\t200\t9\t1:60\tNULL\t[11, 12]\t[(f!)]",
};
const string NULL_ROW = "NULL\tNULL\tNULL\tNULL\tNULL\tNULL\tNULL\tNULL\tNULL\tNULL\tNULL";

} // namespace arrow_record_batch_validity_test

TEST_CASE("Arrow record batch: a row the batch marks NULL is NULL in every column", "[arrow]") {
	using namespace arrow_record_batch_validity_test;
	auto record = AllKinds({1, 2}, 0);
	auto expected = ALL_KINDS_ROWS;
	expected[1] = NULL_ROW;
	expected[2] = NULL_ROW;
	RequireRows(*record, expected);
}

TEST_CASE("Arrow record batch: a sliced batch applies its offset to its bitmap and to every column", "[arrow]") {
	using namespace arrow_record_batch_validity_test;
	auto record = AllKinds({1, 2}, 2);
	RequireRows(*record, {NULL_ROW, ALL_KINDS_ROWS[3], ALL_KINDS_ROWS[4], ALL_KINDS_ROWS[5]});
}

TEST_CASE("Arrow record batch: a batch whose bitmap marks no row NULL", "[arrow]") {
	using namespace arrow_record_batch_validity_test;
	SECTION("no bitmap") {
		auto record = AllKinds({}, 0);
		RequireRows(*record, ALL_KINDS_ROWS);
	}
	SECTION("a null count of zero leaves the bitmap unread") {
		auto record = AllKinds({1, 2}, 0);
		record->array.null_count = 0;
		RequireRows(*record, ALL_KINDS_ROWS);
	}
	SECTION("a sliced batch without a bitmap") {
		auto record = AllKinds({}, 2);
		RequireRows(*record, {ALL_KINDS_ROWS[2], ALL_KINDS_ROWS[3], ALL_KINDS_ROWS[4], ALL_KINDS_ROWS[5]});
	}
}

TEST_CASE("Arrow record batch: an unknown null count reads the bitmap", "[arrow]") {
	using namespace arrow_record_batch_validity_test;
	auto record = AllKinds({1, 2}, 0);
	record->array.null_count = -1;
	auto expected = ALL_KINDS_ROWS;
	expected[1] = NULL_ROW;
	expected[2] = NULL_ROW;
	RequireRows(*record, expected);
}

TEST_CASE("Arrow record batch: columns must hold the batch's offset and length", "[arrow]") {
	using namespace arrow_record_batch_validity_test;
	SECTION("a sliced batch over columns of its own length is rejected") {
		auto record = AllKinds({1, 2}, 0);
		record->array.offset = 2;
		RequireError(*record, "shorter than");
	}
	SECTION("an offset whose sum with the length overflows is rejected") {
		auto record = AllKinds({}, 0);
		record->array.offset = NumericLimits<int64_t>::Maximum();
		RequireError(*record, "shorter than");
	}
	SECTION("an overflowing offset is rejected before the bitmap is read") {
		auto record = AllKinds({0}, 0);
		record->array.offset = NumericLimits<int64_t>::Maximum();
		RequireError(*record, "shorter than");
	}
	SECTION("a negative offset is rejected") {
		auto record = AllKinds({}, 0);
		record->array.offset = -1;
		RequireError(*record, "shorter than");
	}
	SECTION("columns longer than the batch are read from its offset") {
		auto record = AllKinds({1, 2}, 1);
		record->array.length = 3;
		RequireRows(*record, {NULL_ROW, NULL_ROW, ALL_KINDS_ROWS[3]});
	}
}

TEST_CASE("Arrow record batch: NULL rows past the first vector of a sliced batch", "[arrow]") {
	using namespace arrow_record_batch_validity_test;
	constexpr idx_t SLOTS = 3000;
	constexpr int64_t OFFSET = 5;
	const std::vector<idx_t> batch_null_slots {6, 2052, 2053, 2999};
	std::vector<int32_t> values;
	std::vector<int32_t> indices;
	for (idx_t slot = 0; slot < SLOTS; slot++) {
		values.push_back(int32_t(slot));
		indices.push_back(int32_t(slot % 2));
	}
	std::vector<FieldPtr> columns;
	columns.push_back(Int32("a", values, {2054}));
	columns.push_back(Dictionary("d", indices, Int32("", {100, 200}), {2055}));
	auto record = Record(std::move(columns), batch_null_slots, OFFSET);
	std::vector<string> expected;
	for (idx_t slot = OFFSET; slot < SLOTS; slot++) {
		auto batch_null = std::find(batch_null_slots.begin(), batch_null_slots.end(), slot) != batch_null_slots.end();
		auto a = batch_null || slot == 2054 ? "NULL" : to_string(slot);
		auto d = batch_null || slot == 2055 ? "NULL" : to_string(slot % 2 ? 200 : 100);
		expected.push_back(a + "\t" + d);
	}
	RequireRows(*record, expected);
}

TEST_CASE("Arrow record batch: a dictionary column without its dictionary is rejected", "[arrow]") {
	using namespace arrow_record_batch_validity_test;
	std::vector<FieldPtr> columns;
	columns.push_back(Dictionary("d", {0, 1, 0}, Int32("", {100, 200})));
	auto record = Record(std::move(columns));
	record->children[0]->array.dictionary = nullptr;
	RequireError(*record, "has no dictionary");
}

TEST_CASE("Arrow record batch: a batch without columns checks its offset before its bitmap", "[arrow][capi]") {
	using namespace arrow_record_batch_validity_test;
	auto record = MakeField("+s", "", 1, {});
	SetBuffers(*record, {nullptr});
	SetNulls(*record, 1, {0});
	record->array.offset = NumericLimits<int64_t>::Maximum();
	auto rows = ScanCApiDataChunk(*record);
	REQUIRE(rows.size() == 1);
	REQUIRE_THAT(rows[0], Catch::Contains("out of range"));
}
