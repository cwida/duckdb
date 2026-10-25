// Arrow scans of sparse unions whose format string maps type ids to members in a non-identity order.

#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"

#include <string>
#include <vector>

using namespace duckdb;

namespace arrow_union_type_ids_test {

void ReleaseSchema(ArrowSchema *s) {
	s->release = nullptr;
}
void ReleaseArray(ArrowArray *a) {
	a->release = nullptr;
}

// Sparse UNION(i int32, s utf8) with the given format string - member i holds 100 + j and member s holds "s<j>"
struct UnionColumn {
	UnionColumn(const char *format, const std::vector<int8_t> &type_ids_p) : type_ids(type_ids_p) {
		auto slots = type_ids.size();
		int_values.resize(slots);
		string_offsets.push_back(0);
		for (idx_t j = 0; j < slots; j++) {
			int_values[j] = int32_t(100 + j);
			string_data += "s" + std::to_string(j);
			string_offsets.push_back(int32_t(string_data.size()));
		}
		int_schema.format = "i";
		int_schema.name = "i";
		int_schema.flags = 2;
		int_schema.release = ReleaseSchema;
		string_schema.format = "u";
		string_schema.name = "s";
		string_schema.flags = 2;
		string_schema.release = ReleaseSchema;
		schema_children[0] = &int_schema;
		schema_children[1] = &string_schema;
		schema.format = format;
		schema.name = "u";
		schema.n_children = 2;
		schema.children = schema_children;
		schema.release = ReleaseSchema;

		int_buffers[0] = nullptr;
		int_buffers[1] = int_values.data();
		int_array.length = int64_t(slots);
		int_array.n_buffers = 2;
		int_array.buffers = int_buffers;
		int_array.release = ReleaseArray;
		string_buffers[0] = nullptr;
		string_buffers[1] = string_offsets.data();
		string_buffers[2] = string_data.data();
		string_array.length = int64_t(slots);
		string_array.n_buffers = 3;
		string_array.buffers = string_buffers;
		string_array.release = ReleaseArray;
		array_children[0] = &int_array;
		array_children[1] = &string_array;
		buffers[0] = type_ids.data();
		array.length = int64_t(slots);
		array.n_buffers = 1;
		array.buffers = buffers;
		array.n_children = 2;
		array.children = array_children;
		array.release = ReleaseArray;
	}
	UnionColumn(const UnionColumn &) = delete;

	std::vector<int8_t> type_ids;
	std::vector<int32_t> int_values;
	std::vector<int32_t> string_offsets;
	std::string string_data;
	const void *int_buffers[2];
	const void *string_buffers[3];
	const void *buffers[1];
	ArrowSchema *schema_children[2];
	ArrowArray *array_children[2];
	ArrowSchema int_schema {};
	ArrowSchema string_schema {};
	ArrowSchema schema {};
	ArrowArray int_array {};
	ArrowArray string_array {};
	ArrowArray array {};
};

// Records whether the scan took ownership of the stream, which happens once it is produced
class OwnershipTrackingFactory : public ArrowStreamTestFactory {
public:
	explicit OwnershipTrackingFactory(ArrowArrayStream &stream) : ArrowStreamTestFactory(stream) {
	}
	unique_ptr<ArrowArrayStreamWrapper> ProduceStream(ArrowStreamParameters &parameters) override {
		produced = true;
		return ArrowStreamTestFactory::ProduceStream(parameters);
	}

	bool produced = false;
};

// Scans the column and compares it with the expected query; returns the error message, or an empty string on a match
string ScanUnion(UnionColumn &col, const string &expected_query) {
	ArrowSchema *schema_children[1] = {&col.schema};
	ArrowSchema record_schema {};
	record_schema.format = "+s";
	record_schema.n_children = 1;
	record_schema.children = schema_children;
	record_schema.release = ReleaseSchema;

	ArrowArray *array_children[1] = {&col.array};
	const void *buffers[1] = {nullptr};
	ArrowArray record_array {};
	record_array.length = col.array.length;
	record_array.n_buffers = 1;
	record_array.buffers = buffers;
	record_array.n_children = 1;
	record_array.children = array_children;
	record_array.release = ReleaseArray;

	ArrowArrayStream stream {};
	AdbcError err {};
	REQUIRE(duckdb_adbc::BatchToArrayStream(&record_array, &record_schema, &stream, &err) == ADBC_STATUS_OK);
	DuckDB db(nullptr);
	Connection con(db);
	auto factory = make_shared_ptr<OwnershipTrackingFactory>(stream);
	string error;
	try {
		auto arrow_scan = con.TableFunction("arrow_scan", {}, {}, factory);
		auto result = arrow_scan->Project("union_tag(u)::VARCHAR AS tag, u::VARCHAR AS v")->Execute();
		if (result->HasError()) {
			error = result->GetError();
		} else if (!result->Equals(*con.Query(expected_query))) {
			error = "result mismatch";
		}
	} catch (std::exception &ex) {
		error = ErrorData(ex).Message();
	}
	if (!factory->produced) {
		stream.release(&stream);
	}
	return error;
}

} // namespace arrow_union_type_ids_test

TEST_CASE("Arrow scan of a sparse union with non-identity type ids", "[arrow]") {
	const string expected = "SELECT * FROM (VALUES ('s', 's0'), ('i', '101'), ('i', '102'), ('s', 's3')) t(tag, v)";

	// type id 5 selects member i and type id 9 selects member s
	arrow_union_type_ids_test::UnionColumn sparse_ids("+us:5,9", {9, 5, 5, 9});
	REQUIRE(arrow_union_type_ids_test::ScanUnion(sparse_ids, expected).empty());

	// type id 1 selects member i and type id 0 selects member s
	arrow_union_type_ids_test::UnionColumn swapped_ids("+us:1,0", {0, 1, 1, 0});
	REQUIRE(arrow_union_type_ids_test::ScanUnion(swapped_ids, expected).empty());
}

TEST_CASE("Arrow scan of a sparse union with invalid type ids", "[arrow]") {
	// type id 7 is not listed in the format string
	arrow_union_type_ids_test::UnionColumn unknown_id("+us:5,9", {9, 7, 5, 9});
	REQUIRE(StringUtil::Contains(arrow_union_type_ids_test::ScanUnion(unknown_id, "SELECT 1"),
	                             "type id 7 does not match any union member"));

	const char *invalid_formats[] = {"+us:5", "+us:5,9,11", "+us:5,5", "+us:5,-1", "+us:5,x", "+us:5,128"};
	for (auto format : invalid_formats) {
		INFO(format);
		arrow_union_type_ids_test::UnionColumn col(format, {5, 5, 5, 5});
		REQUIRE(StringUtil::Contains(arrow_union_type_ids_test::ScanUnion(col, "SELECT 1"), "Arrow union format"));
	}
}
