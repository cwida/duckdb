// Arrow scans of schemas whose field names are NULL, which the C data interface allows for an unnamed field.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb.h"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"
#include "duckdb/common/arrow/schema_metadata.hpp"
#include "test_helpers.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace duckdb;

namespace arrow_null_field_names_test {

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
	std::vector<int8_t> type_ids;
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

FieldPtr Int32(const char *name, std::vector<int32_t> values) {
	auto field = MakeField("i", name, int64_t(values.size()), {});
	field->data = std::move(values);
	SetBuffers(*field, {nullptr, field->data.data()});
	return field;
}

FieldPtr Struct(const char *name, std::vector<FieldPtr> children) {
	auto length = children[0]->array.length;
	auto field = MakeField("+s", name, length, std::move(children));
	SetBuffers(*field, {nullptr});
	return field;
}

FieldPtr List(const char *name, std::vector<int32_t> offsets, FieldPtr child) {
	std::vector<FieldPtr> children;
	children.push_back(std::move(child));
	auto field = MakeField("+l", name, int64_t(offsets.size()) - 1, std::move(children));
	field->data = std::move(offsets);
	SetBuffers(*field, {nullptr, field->data.data()});
	return field;
}

FieldPtr Dictionary(const char *name, std::vector<int32_t> indices, FieldPtr values) {
	auto field = Int32(name, std::move(indices));
	field->dictionary = std::move(values);
	field->schema.dictionary = &field->dictionary->schema;
	field->array.dictionary = &field->dictionary->array;
	return field;
}

FieldPtr SparseUnion(const char *name, std::vector<int8_t> type_ids, std::vector<FieldPtr> members) {
	auto field = MakeField("+us:0,1", name, int64_t(type_ids.size()), std::move(members));
	field->type_ids = std::move(type_ids);
	SetBuffers(*field, {field->type_ids.data()});
	return field;
}

FieldPtr RunEnd(const char *name, FieldPtr run_ends, FieldPtr values) {
	auto length = int64_t(run_ends->data.back());
	std::vector<FieldPtr> children;
	children.push_back(std::move(run_ends));
	children.push_back(std::move(values));
	auto field = MakeField("+r", name, length, std::move(children));
	SetBuffers(*field, {});
	return field;
}

FieldPtr Record(std::vector<FieldPtr> columns) {
	return Struct("", std::move(columns));
}

// Serves one record batch and keeps the column names the scan asks the producer for.
class RecordScanFactory : public ArrowScanFactory {
public:
	explicit RecordScanFactory(Field &record_p) : record(record_p) {
	}
	void GetSchema(ArrowSchema &out) override {
		out = record.schema;
	}
	unique_ptr<ArrowArrayStreamWrapper> ProduceStream(ArrowStreamParameters &parameters) override {
		projected_columns = parameters.projected_columns.columns;
		auto schema = record.schema;
		auto array = record.array;
		auto result = make_uniq<ArrowArrayStreamWrapper>();
		AdbcError error {};
		if (duckdb_adbc::BatchToArrayStream(&array, &schema, &result->arrow_array_stream, &error) != ADBC_STATUS_OK) {
			throw InternalException("BatchToArrayStream failed");
		}
		return result;
	}

	Field &record;
	vector<string> projected_columns;
};

// Renders a value without casting it, since casts of a struct or union with duplicate member names fail or misread.
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

string RenderError(const ErrorData &error) {
	return "error: " + Exception::ExceptionTypeToString(error.Type()) + ": " + error.RawMessage();
}

// The column names and types the scan reports, the names it asks the producer for and the rows, or the typed error.
string ScanShape(Field &record) {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("SET memory_limit = '4GB'"));
	auto factory = make_shared_ptr<RecordScanFactory>(record);
	string shape;
	try {
		auto result = con.TableFunction("arrow_scan", {}, {}, factory)->Execute();
		if (result->HasError()) {
			return RenderError(result->GetErrorObject());
		}
		auto names = IdentifiersToStrings(result->GetNames());
		for (idx_t i = 0; i < names.size(); i++) {
			shape += names[i] + " " + result->GetTypes()[i].ToString() + "\n";
		}
		shape += "producer:";
		for (auto &column : factory->projected_columns) {
			shape += " \"" + column + "\"";
		}
		shape += "\n";
		for (auto chunk = result->Fetch(); chunk; chunk = result->Fetch()) {
			for (idx_t row = 0; row < chunk->size(); row++) {
				for (idx_t column = 0; column < chunk->ColumnCount(); column++) {
					shape += (column ? "\t" : "") + RenderValue(chunk->GetValue(column, row));
				}
				shape += "\n";
			}
		}
	} catch (std::exception &ex) {
		return RenderError(ErrorData(ex));
	}
	return shape;
}

// Requires that a schema scans the same whether its unnamed fields carry a NULL or an empty name, and returns that.
string UnnamedShape(const std::function<FieldPtr(const char *unnamed)> &make) {
	auto with_null = make(nullptr);
	auto with_empty = make("");
	auto shape = ScanShape(*with_empty);
	REQUIRE(ScanShape(*with_null) == shape);
	return shape;
}

// The column type the C API v1 import reports after converting the schema and one array.
string CApiColumnType(Field &record, idx_t column) {
	duckdb_database db;
	duckdb_connection con;
	REQUIRE(duckdb_open(nullptr, &db) == DuckDBSuccess);
	REQUIRE(duckdb_connect(db, &con) == DuckDBSuccess);
	string type_text;
	duckdb_arrow_converted_schema converted = nullptr;
	auto error = duckdb_schema_from_arrow(con, &record.schema, &converted);
	if (error) {
		type_text = "error: " + ErrorData(duckdb_error_data_message(error)).RawMessage();
		duckdb_destroy_error_data(&error);
	} else {
		auto array = record.array;
		duckdb_data_chunk chunk = nullptr;
		error = duckdb_data_chunk_from_arrow(con, &array, converted, &chunk);
		REQUIRE(error == nullptr);
		auto type = duckdb_vector_get_column_type(duckdb_data_chunk_get_vector(chunk, column));
		type_text = reinterpret_cast<LogicalType *>(type)->ToString();
		duckdb_destroy_logical_type(&type);
		duckdb_destroy_data_chunk(&chunk);
		duckdb_destroy_arrow_converted_schema(&converted);
	}
	duckdb_disconnect(&con);
	duckdb_close(&db);
	return type_text;
}

} // namespace arrow_null_field_names_test

TEST_CASE("Arrow scan of top level fields with a NULL name", "[arrow]") {
	using namespace arrow_null_field_names_test;
	auto shape = UnnamedShape([](const char *unnamed) {
		std::vector<FieldPtr> columns;
		columns.push_back(Int32(unnamed, {1, 2}));
		columns.push_back(Int32("v0", {3, 4}));
		columns.push_back(Int32(unnamed, {5, 6}));
		return Record(std::move(columns));
	});
	// an unnamed column is named after its position, then deduplicated; the producer gets the empty name
	REQUIRE(shape == "v0 INTEGER\nv0_1 INTEGER\nv2 INTEGER\nproducer: \"\" \"v0\" \"\"\n1\t3\t5\n2\t4\t6\n");
}

TEST_CASE("Arrow scan of struct children with a NULL name", "[arrow]") {
	using namespace arrow_null_field_names_test;
	SECTION("all children unnamed make a TUPLE") {
		auto shape = UnnamedShape([](const char *unnamed) {
			std::vector<FieldPtr> children;
			children.push_back(Int32(unnamed, {1, 2}));
			children.push_back(Int32(unnamed, {3, 4}));
			std::vector<FieldPtr> columns;
			columns.push_back(Struct("s", std::move(children)));
			return Record(std::move(columns));
		});
		REQUIRE(shape == "s TUPLE(INTEGER, INTEGER)\nproducer: \"s\"\n(1, 3)\n(2, 4)\n");
	}
	SECTION("one unnamed child beside a named one") {
		auto shape = UnnamedShape([](const char *unnamed) {
			std::vector<FieldPtr> children;
			children.push_back(Int32(unnamed, {1, 2}));
			children.push_back(Int32("b", {3, 4}));
			std::vector<FieldPtr> columns;
			columns.push_back(Struct("s", std::move(children)));
			return Record(std::move(columns));
		});
		REQUIRE(shape == "s STRUCT(\"\" INTEGER, b INTEGER)\nproducer: \"s\"\n(1, 3)\n(2, 4)\n");
	}
	SECTION("an unnamed struct column with unnamed children") {
		auto shape = UnnamedShape([](const char *unnamed) {
			std::vector<FieldPtr> children;
			children.push_back(Int32(unnamed, {1, 2}));
			std::vector<FieldPtr> columns;
			columns.push_back(Struct(unnamed, std::move(children)));
			return Record(std::move(columns));
		});
		REQUIRE(shape == "v0 TUPLE(INTEGER)\nproducer: \"\"\n(1)\n(2)\n");
	}
}

TEST_CASE("Arrow scan of a list of structs with NULL names", "[arrow]") {
	using namespace arrow_null_field_names_test;
	auto shape = UnnamedShape([](const char *unnamed) {
		std::vector<FieldPtr> children;
		children.push_back(Int32(unnamed, {1, 2, 3}));
		std::vector<FieldPtr> columns;
		columns.push_back(List("l", {0, 1, 3}, Struct(unnamed, std::move(children))));
		return Record(std::move(columns));
	});
	REQUIRE(shape == "l TUPLE(INTEGER)[]\nproducer: \"l\"\n[(1)]\n[(2), (3)]\n");
}

TEST_CASE("Arrow scan of dictionaries whose value type has a NULL name", "[arrow]") {
	using namespace arrow_null_field_names_test;
	SECTION("a dictionary of integers") {
		auto shape = UnnamedShape([](const char *unnamed) {
			std::vector<FieldPtr> columns;
			columns.push_back(Dictionary("d", {1, 0, 1}, Int32(unnamed, {10, 20})));
			return Record(std::move(columns));
		});
		REQUIRE(shape == "d INTEGER\nproducer: \"d\"\n20\n10\n20\n");
	}
	SECTION("a dictionary of structs with an unnamed child") {
		auto shape = UnnamedShape([](const char *unnamed) {
			std::vector<FieldPtr> children;
			children.push_back(Int32(unnamed, {10, 20}));
			std::vector<FieldPtr> columns;
			columns.push_back(Dictionary("d", {1, 0, 1}, Struct(unnamed, std::move(children))));
			return Record(std::move(columns));
		});
		REQUIRE(shape == "d TUPLE(INTEGER)\nproducer: \"d\"\n(20)\n(10)\n(20)\n");
	}
}

TEST_CASE("Arrow scan of union members with a NULL name", "[arrow]") {
	using namespace arrow_null_field_names_test;
	auto make = [](const char *first, const char *second) {
		std::vector<FieldPtr> members;
		members.push_back(Int32(first, {1, 2}));
		members.push_back(Int32(second, {3, 4}));
		std::vector<FieldPtr> columns;
		columns.push_back(SparseUnion("u", {0, 1}, std::move(members)));
		return Record(std::move(columns));
	};
	SECTION("one unnamed member beside a named one") {
		auto shape = UnnamedShape([&](const char *unnamed) { return make(unnamed, "b"); });
		REQUIRE(shape == "u UNION(\"\" INTEGER, b INTEGER)\nproducer: \"u\"\n0:1\n1:4\n");
	}
	SECTION("all members unnamed") {
		auto shape = UnnamedShape([&](const char *unnamed) { return make(unnamed, unnamed); });
		REQUIRE(shape == "u UNION(\"\" INTEGER, \"\" INTEGER)\nproducer: \"u\"\n0:1\n1:4\n");
	}
}

TEST_CASE("Arrow scan of a run end encoded column whose children have NULL names", "[arrow]") {
	using namespace arrow_null_field_names_test;
	auto shape = UnnamedShape([](const char *unnamed) {
		std::vector<FieldPtr> columns;
		columns.push_back(RunEnd("r", Int32(unnamed, {2, 3}), Int32(unnamed, {7, 8})));
		return Record(std::move(columns));
	});
	REQUIRE(shape == "r INTEGER\nproducer: \"r\"\n7\n7\n8\n");
}

TEST_CASE("Arrow scan of a variant whose storage fields have NULL names", "[arrow]") {
	using namespace arrow_null_field_names_test;
	auto tag = ArrowSchemaMetadata::ArrowCanonicalType("arrow.parquet.variant").SerializeMetadata();
	auto shape = UnnamedShape([&](const char *unnamed) {
		std::vector<FieldPtr> children;
		children.push_back(Int32(unnamed, {1}));
		children.push_back(Int32("value", {2}));
		children[0]->schema.format = "z";
		children[1]->schema.format = "z";
		std::vector<FieldPtr> columns;
		columns.push_back(Struct("v", std::move(children)));
		columns[0]->schema.metadata = tag.get();
		return Record(std::move(columns));
	});
	// the storage fields are found by name, so an unnamed one is neither metadata nor value
	REQUIRE(shape == "error: Invalid Input: arrow.parquet.variant column has an unexpected or duplicate field '' "
	                 "(expected 'metadata' and 'value')");
}

TEST_CASE("Arrow scan of duplicate field names", "[arrow]") {
	using namespace arrow_null_field_names_test;
	SECTION("top level names are deduplicated") {
		std::vector<FieldPtr> columns;
		columns.push_back(Int32("a", {1, 2}));
		columns.push_back(Int32("a", {3, 4}));
		columns.push_back(Int32("A", {5, 6}));
		auto record = Record(std::move(columns));
		REQUIRE(ScanShape(*record) ==
		        "a INTEGER\na_1 INTEGER\nA_2 INTEGER\nproducer: \"a\" \"a\" \"A\"\n1\t3\t5\n2\t4\t6\n");
	}
	SECTION("duplicate struct children are kept") {
		std::vector<FieldPtr> children;
		children.push_back(Int32("a", {1, 2}));
		children.push_back(Int32("a", {3, 4}));
		std::vector<FieldPtr> columns;
		columns.push_back(Struct("s", std::move(children)));
		auto record = Record(std::move(columns));
		REQUIRE(ScanShape(*record) == "s STRUCT(a INTEGER, a INTEGER)\nproducer: \"s\"\n(1, 3)\n(2, 4)\n");
	}
	SECTION("two unnamed struct children beside a named one are kept") {
		auto shape = UnnamedShape([](const char *unnamed) {
			std::vector<FieldPtr> children;
			children.push_back(Int32(unnamed, {1, 2}));
			children.push_back(Int32(unnamed, {3, 4}));
			children.push_back(Int32("b", {5, 6}));
			std::vector<FieldPtr> columns;
			columns.push_back(Struct("s", std::move(children)));
			return Record(std::move(columns));
		});
		REQUIRE(shape == "s STRUCT(\"\" INTEGER, \"\" INTEGER, b INTEGER)\nproducer: \"s\"\n(1, 3, 5)\n(2, 4, 6)\n");
	}
	SECTION("duplicate union members are kept") {
		std::vector<FieldPtr> members;
		members.push_back(Int32("a", {1, 2}));
		members.push_back(Int32("a", {3, 4}));
		std::vector<FieldPtr> columns;
		columns.push_back(SparseUnion("u", {0, 1}, std::move(members)));
		auto record = Record(std::move(columns));
		REQUIRE(ScanShape(*record) == "u UNION(a INTEGER, a INTEGER)\nproducer: \"u\"\n0:1\n1:4\n");
	}
}

TEST_CASE("Arrow scan of a field with a NULL format", "[arrow]") {
	using namespace arrow_null_field_names_test;
	SECTION("a top level field") {
		std::vector<FieldPtr> columns;
		columns.push_back(Int32("a", {1, 2}));
		columns[0]->schema.format = nullptr;
		auto record = Record(std::move(columns));
		REQUIRE(ScanShape(*record) == "error: Invalid Input: Arrow schema of field \"a\" has no format string");
	}
	SECTION("an unnamed struct child") {
		std::vector<FieldPtr> children;
		children.push_back(Int32(nullptr, {1, 2}));
		children[0]->schema.format = nullptr;
		std::vector<FieldPtr> columns;
		columns.push_back(Struct("s", std::move(children)));
		auto record = Record(std::move(columns));
		REQUIRE(ScanShape(*record) == "error: Invalid Input: Arrow schema of field \"\" has no format string");
	}
	SECTION("a dictionary value type") {
		std::vector<FieldPtr> columns;
		columns.push_back(Dictionary("d", {0}, Int32(nullptr, {10})));
		columns[0]->dictionary->schema.format = nullptr;
		auto record = Record(std::move(columns));
		REQUIRE(ScanShape(*record) == "error: Invalid Input: Arrow schema of field \"\" has no format string");
	}
}

TEST_CASE("C API import of Arrow schemas with NULL names", "[arrow][capi]") {
	using namespace arrow_null_field_names_test;
	SECTION("a NULL top level name and NULL struct child names") {
		std::vector<FieldPtr> children;
		children.push_back(Int32(nullptr, {1, 2}));
		children.push_back(Int32("b", {3, 4}));
		std::vector<FieldPtr> columns;
		columns.push_back(Int32(nullptr, {5, 6}));
		columns.push_back(Struct(nullptr, std::move(children)));
		auto record = Record(std::move(columns));
		REQUIRE(CApiColumnType(*record, 0) == "INTEGER");
		REQUIRE(CApiColumnType(*record, 1) == "STRUCT(\"\" INTEGER, b INTEGER)");
	}
	SECTION("a list of structs and a dictionary of structs with NULL names") {
		std::vector<FieldPtr> list_children;
		list_children.push_back(Int32(nullptr, {1, 2, 3}));
		std::vector<FieldPtr> dictionary_children;
		dictionary_children.push_back(Int32(nullptr, {10, 20}));
		std::vector<FieldPtr> columns;
		columns.push_back(List(nullptr, {0, 1, 3}, Struct(nullptr, std::move(list_children))));
		columns.push_back(Dictionary(nullptr, {1, 0}, Struct(nullptr, std::move(dictionary_children))));
		auto record = Record(std::move(columns));
		REQUIRE(CApiColumnType(*record, 0) == "TUPLE(INTEGER)[]");
		REQUIRE(CApiColumnType(*record, 1) == "TUPLE(INTEGER)");
	}
	SECTION("a NULL format is an error") {
		std::vector<FieldPtr> columns;
		columns.push_back(Int32(nullptr, {1, 2}));
		columns[0]->schema.format = nullptr;
		auto record = Record(std::move(columns));
		REQUIRE(CApiColumnType(*record, 0) == "error: Arrow schema of field \"\" has no format string");
	}
}
