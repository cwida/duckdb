#include "catch.hpp"
#include "duckdb/common/type_visitor.hpp"

using namespace duckdb;

TEST_CASE("TypeVisitor preserves child order and visits replaced children before their parent", "[type_visitor]") {
	const child_list_t<LogicalType> children {
	    {"a", LogicalType::INTEGER}, {"b", LogicalType::LIST(LogicalType::VARCHAR)}, {"c", LogicalType::BOOLEAN}};
	const duckdb::vector<LogicalType> types {LogicalType::STRUCT(children), LogicalType::TUPLE(children),
	                                         LogicalType::UNION(children)};
	for (const auto &type : types) {
		duckdb::vector<LogicalTypeId> visited;
		auto result = TypeVisitor::VisitReplace(type, [&](const LogicalType &child) {
			visited.push_back(child.id());
			return child.id() == LogicalTypeId::INTEGER ? LogicalType(LogicalType::BIGINT) : child;
		});
		const duckdb::vector<LogicalTypeId> expected {LogicalTypeId::INTEGER, LogicalTypeId::VARCHAR,
		                                              LogicalTypeId::LIST, LogicalTypeId::BOOLEAN, type.id()};
		REQUIRE(visited == expected);
		const auto &result_children =
		    type.id() == LogicalTypeId::UNION ? UnionType::CopyMemberTypes(result) : StructType::GetChildTypes(result);
		const auto &original_children =
		    type.id() == LogicalTypeId::UNION ? UnionType::CopyMemberTypes(type) : StructType::GetChildTypes(type);
		REQUIRE(result_children.size() == children.size());
		for (idx_t i = 0; i < children.size(); i++) {
			REQUIRE(result_children[i].first == original_children[i].first);
			REQUIRE(result_children[i].second == (i == 0 ? LogicalType(LogicalType::BIGINT) : children[i].second));
		}
	}
}

TEST_CASE("TypeVisitor Contains visits children in order and short circuits", "[type_visitor]") {
	const auto type =
	    LogicalType::STRUCT({{"a", LogicalType::MAP(LogicalType::INTEGER, LogicalType::LIST(LogicalType::VARCHAR))},
	                         {"b", LogicalType::ARRAY(LogicalType::BOOLEAN, 3)}});
	duckdb::vector<LogicalTypeId> visited;
	REQUIRE(TypeVisitor::Contains(type, [&](const LogicalType &child) {
		visited.push_back(child.id());
		return child.id() == LogicalTypeId::VARCHAR;
	}));
	const duckdb::vector<LogicalTypeId> expected {LogicalTypeId::STRUCT, LogicalTypeId::MAP, LogicalTypeId::INTEGER,
	                                              LogicalTypeId::LIST, LogicalTypeId::VARCHAR};
	REQUIRE(visited == expected);
	REQUIRE(TypeVisitor::Contains(type, LogicalTypeId::BOOLEAN));
	REQUIRE_FALSE(TypeVisitor::Contains(type, LogicalTypeId::DOUBLE));
}

TEST_CASE("TypeVisitor handles deep types without recursion", "[type_visitor]") {
	const idx_t depth = 10000;
	duckdb::vector<LogicalType> input {LogicalType::INTEGER};
	for (idx_t i = 0; i < depth; i++) {
		input.push_back(LogicalType::STRUCT({{"child", input.back()}}));
	}
	REQUIRE(TypeVisitor::Contains(input.back(), LogicalTypeId::INTEGER));
	REQUIRE_FALSE(TypeVisitor::Contains(input.back(), LogicalTypeId::VARCHAR));

	duckdb::vector<LogicalType> output;
	auto result = TypeVisitor::VisitReplace(input.back(), [&](const LogicalType &type) {
		output.push_back(type.id() == LogicalTypeId::INTEGER ? LogicalType(LogicalType::BIGINT) : type);
		return output.back();
	});
	REQUIRE(output.size() == depth + 1);
	REQUIRE(TypeVisitor::Contains(result, LogicalTypeId::BIGINT));
	REQUIRE_FALSE(TypeVisitor::Contains(result, LogicalTypeId::INTEGER));

	// Retain the children until their parents are destroyed to avoid recursive destruction.
	result = LogicalType::SQLNULL;
	while (!output.empty()) {
		output.pop_back();
	}
	while (!input.empty()) {
		input.pop_back();
	}
}
