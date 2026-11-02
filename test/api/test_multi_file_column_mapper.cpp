#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/multi_file/base_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_column_mapper.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/multi_file/multi_file_states.hpp"
#include "duckdb/parallel/async_result.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"

using namespace duckdb;

namespace {
class MappingTestReader : public BaseFileReader {
public:
	MappingTestReader() : BaseFileReader(OpenFileInfo("mapping-test")) {
	}
	bool TryInitializeScan(ClientContext &, GlobalTableFunctionState &, LocalTableFunctionState &) override {
		throw InternalException("Mapping test does not scan");
	}
	AsyncResult Scan(ClientContext &, GlobalTableFunctionState &, LocalTableFunctionState &, DataChunk &) override {
		throw InternalException("Mapping test does not scan");
	}
	string GetReaderType() const override {
		return "mapping test";
	}
};

MultiFileColumnDefinition Field(const string &name, const LogicalType &type, int32_t id) {
	MultiFileColumnDefinition result(name, type);
	result.identifier = Value::INTEGER(id);
	return result;
}
} // namespace

TEST_CASE("Field-id mapping requires a match or an explicit default", "[multifile]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.BeginTransaction();
	auto reader = make_shared_ptr<MappingTestReader>();
	MultiFileReaderData reader_data(reader);
	MultiFileReader multi_file_reader;
	SimpleMultiFileList files({OpenFileInfo("mapping-test")});
	vector<MultiFileColumnDefinition> columns;
	columns.push_back(Field("required_field", LogicalType::INTEGER, 42));
	vector<ColumnIndex> indexes {ColumnIndex(0)};
	virtual_column_map_t virtual_columns;
	MultiFileColumnMapper mapper(*con.context, multi_file_reader, reader_data, columns, indexes, nullptr, files,
	                             virtual_columns);

	SECTION("Missing field without a default raises a recoverable error") {
		REQUIRE_THROWS_AS(mapper.CreateMapping(MultiFileColumnMappingMode::BY_FIELD_ID), InvalidInputException);
		REQUIRE_THROWS_WITH(
		    mapper.CreateMapping(MultiFileColumnMappingMode::BY_FIELD_ID),
		    Catch::Contains("(field id 42) is missing from the file schema and has no default expression"));
	}
	SECTION("Matching field does not need a default") {
		reader->columns.push_back(Field("file_field", LogicalType::INTEGER, 42));
		REQUIRE(mapper.CreateMapping(MultiFileColumnMappingMode::BY_FIELD_ID) == ReaderInitializeType::INITIALIZED);
		REQUIRE(reader->column_ids.size() == 1);
	}
	SECTION("Explicit NULL and non-NULL defaults are accepted") {
		auto value = GENERATE(Value(LogicalType::INTEGER), Value::INTEGER(7));
		columns[0].default_expression = ConstantExpression::FromValue(value);
		REQUIRE(mapper.CreateMapping(MultiFileColumnMappingMode::BY_FIELD_ID) == ReaderInitializeType::INITIALIZED);
		REQUIRE(reader_data.expressions.size() == 1);
		REQUIRE(ValueOperations::NotDistinctFrom(reader_data.expressions[0]->Cast<BoundConstantExpression>().GetValue(),
		                                         value));
	}
	SECTION("Missing MAP key without a default raises a recoverable error") {
		auto map_type = LogicalType::MAP(LogicalType::INTEGER, LogicalType::INTEGER);
		columns[0] = Field("m", map_type, 1);
		columns[0].children.push_back(Field("key", LogicalType::INTEGER, 42));
		columns[0].children.push_back(Field("value", LogicalType::INTEGER, 43));
		auto local_map = Field("m", map_type, 1);
		MultiFileColumnDefinition key_value("key_value", ListType::GetChildType(map_type));
		key_value.children.push_back(Field("key", LogicalType::INTEGER, 99));
		key_value.children.push_back(Field("value", LogicalType::INTEGER, 43));
		local_map.children.push_back(key_value);
		reader->columns.push_back(local_map);
		REQUIRE_THROWS_AS(mapper.CreateMapping(MultiFileColumnMappingMode::BY_FIELD_ID), InvalidInputException);
		REQUIRE_THROWS_WITH(
		    mapper.CreateMapping(MultiFileColumnMappingMode::BY_FIELD_ID),
		    Catch::Contains("(field id 42) is missing from the file schema and has no default expression"));
	}
	con.Rollback();
	REQUIRE_NO_FAIL(con.Query("SELECT 42"));
}
