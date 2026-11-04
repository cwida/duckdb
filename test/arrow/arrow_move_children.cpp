#include "catch.hpp"

#include "arrow/arrow_test_helper.hpp"
#include "duckdb/common/adbc/single_batch_array_stream.hpp"

using namespace duckdb;

static void EmptyRelease(ArrowArray *array) {
	for (int64_t i = 0; i < array->n_children; i++) {
		auto child = array->children[i];
		if (child->release) {
			child->release(child);
		}
	}
	array->release = nullptr;
}

template <class T>
void AssertExpectedResult(ArrowSchema *schema, ArrowArrayWrapper &array, T expected_value, bool is_null = false) {
	ArrowArrayStream stream;
	stream.release = nullptr;

	ArrowArray struct_array;
	struct_array.n_children = 1;
	ArrowArray *children[1];
	struct_array.children = (ArrowArray **)&children;
	struct_array.children[0] = &array.arrow_array;
	struct_array.length = array.arrow_array.length;
	struct_array.release = EmptyRelease;
	struct_array.offset = 0;

	AdbcError unused;
	(void)duckdb_adbc::BatchToArrayStream(&struct_array, schema, &stream, &unused);

	DuckDB db(nullptr);
	Connection conn(db);
	auto params = ArrowTestHelper::ConstructArrowScan(stream);

	auto result = ArrowTestHelper::ScanArrowObject(conn, params);
	unique_ptr<DataChunk> chunk;
	while (true) {
		chunk = result->Fetch();
		if (!chunk) {
			break;
		}
		REQUIRE(chunk->ColumnCount() == 1);
		REQUIRE(chunk->size() == STANDARD_VECTOR_SIZE);
		auto &vec = chunk->data[0];
		for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
			auto value = vec.GetValue(i);
			auto expected = Value(expected_value);
			if (is_null) {
				REQUIRE(value.IsNull());
			} else {
				REQUIRE(value == expected);
			}
		}
	}
	if (schema->release) {
		schema->release(schema);
	}
}

vector<ArrowArrayWrapper> FetchChildrenFromArray(shared_ptr<ArrowArrayWrapper> parent) {
	D_ASSERT(parent->arrow_array.release);
	vector<ArrowArrayWrapper> children;
	children.resize(parent->arrow_array.n_children);
	for (int64_t i = 0; i < parent->arrow_array.n_children; i++) {
		auto child = parent->arrow_array.children[i];
		auto &wrapper = children[i];
		wrapper.arrow_array = *child;
		// Unset the 'release' method to null for the child inside the parent
		// to indicate that it has been moved
		child->release = nullptr;
	}
	// Release the parent, should not affect the children
	parent->arrow_array.release(&parent->arrow_array);
	return children;
}

// https://arrow.apache.org/docs/format/CDataInterface.html#moving-child-arrays
TEST_CASE("Test move children", "[arrow]") {
	auto query = StringUtil::Format("select 'a', 'this is a long string', 42, true, NULL from range(%d);",
	                                STANDARD_VECTOR_SIZE * 2);

	// Create the stream that will produce arrow arrays
	DuckDB db(nullptr);
	Connection conn(db);
	auto initial_result = conn.Query(query);
	auto client_properties = conn.context->GetClientProperties();
	auto types = initial_result->GetTypes();
	auto names = IdentifiersToStrings(initial_result->GetNames());

	// Scan every column
	ArrowStreamParameters parameters;
	for (idx_t idx = 0; idx < names.size(); idx++) {
		auto col_idx = idx;
		auto &name = names[idx];
		if (col_idx != COLUMN_IDENTIFIER_ROW_ID) {
			parameters.projected_columns.projection_map[idx] = name;
			parameters.projected_columns.columns.emplace_back(name);
		}
	}
	auto res_names = names;
	auto res_types = types;
	auto res_properties = initial_result->client_properties;

	// Create a test factory and produce a stream from it
	auto factory = ArrowTestFactory(std::move(types), std::move(names), std::move(initial_result), client_properties,
	                                *conn.context);
	auto stream = factory.ProduceStream(parameters);

	// For every array, extract the children and scan them
	while (true) {
		auto chunk = stream->GetNextChunk();
		if (!chunk || !chunk->arrow_array.release) {
			break;
		}
		auto children = FetchChildrenFromArray(std::move(chunk));
		D_ASSERT(children.size() == 5);
		for (idx_t i = 0; i < children.size(); i++) {
			ArrowSchema schema;
			vector<LogicalType> single_type {res_types[i]};
			vector<string> single_name {res_names[i]};
			ArrowConverter::ToArrowSchema(&schema, single_type, single_name, res_properties);

			if (i == 0) {
				AssertExpectedResult<string>(&schema, children[i], "a");
			} else if (i == 1) {
				AssertExpectedResult<string>(&schema, children[i], "this is a long string");
			} else if (i == 2) {
				AssertExpectedResult<int32_t>(&schema, children[i], 42);
			} else if (i == 3) {
				AssertExpectedResult<bool>(&schema, children[i], true);
			} else if (i == 4) {
				AssertExpectedResult<int32_t>(&schema, children[i], 0, true);
			} else {
				// Not possible
				REQUIRE(false);
			}
			if (schema.release) {
				schema.release(&schema);
			}
		}
	}
}
