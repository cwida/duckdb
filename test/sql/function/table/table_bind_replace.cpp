#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "test_helpers.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/common/enums/joinref_type.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

using namespace duckdb;

// This function demonstrates/tests how the TableFunction::bind_replace works.
// The bind_replace_demo function has two params: depth and name. It generates custom plan recursively by using
// bind_replace to replace its plan with a CROSS PRODUCT of two calls to itself, with the depth reduced by one. When the
// base case is reached, a regular bind is performed, allowing the table function to be called normally.
struct BindReplaceDemoFun {
	struct CustomFunctionData : public TableFunctionData {
		int64_t current_depth;
		string current_name;
		bool done = false;
	};

	static duckdb::unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                             duckdb::vector<LogicalType> &return_types,
	                                             duckdb::vector<Identifier> &names) {
		auto result = make_uniq<BindReplaceDemoFun::CustomFunctionData>();

		result->current_depth = input.inputs[0].GetValue<int64_t>();
		result->current_name = input.inputs[1].ToString();

		return_types.emplace_back(LogicalType::BIGINT);
		names.emplace_back("depth_" + result->current_name);

		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("col_" + result->current_name);

		return std::move(result);
	}

	static duckdb::unique_ptr<TableRef> BindReplace(ClientContext &context, TableFunctionBindInput &input) {
		auto result = make_uniq<BindReplaceDemoFun::CustomFunctionData>();

		auto depth = input.inputs[0].GetValue<int64_t>();
		auto name = input.inputs[1].ToString();

		// While depth > 0, we will replace the plan with a CROSS JOIN between to sub-calls to the same function
		// resulting in a recursively bound query plan that will eventually result in the regular bind being called.
		if (depth > 0) {
			auto join_node = make_uniq<JoinRef>(JoinRefType::CROSS);

			// Construct LHS TableFunctionRef
			duckdb::vector<duckdb::unique_ptr<ParsedExpression>> left_children;
			left_children.push_back(ConstantExpression::Integer(depth - 1));
			left_children.push_back(ConstantExpression::String(name + "L"));
			auto tf_ref_left = make_uniq<TableFunctionRef>();
			tf_ref_left->alias = Identifier("inner_table_" + name + "L");
			tf_ref_left->function = make_uniq<FunctionExpression>("bind_replace_demo", std::move(left_children));
			join_node->left = std::move(tf_ref_left);

			// Construct RHS TableFunctionRef
			duckdb::vector<duckdb::unique_ptr<ParsedExpression>> right_children;
			right_children.push_back(ConstantExpression::Integer(depth - 1));
			right_children.push_back(ConstantExpression::String(name + "R"));
			auto tf_ref_right = make_uniq<TableFunctionRef>();
			tf_ref_right->alias = Identifier("inner_table_" + name + "R");
			tf_ref_right->function = make_uniq<FunctionExpression>("bind_replace_demo", std::move(right_children));
			join_node->right = std::move(tf_ref_right);

			return std::move(join_node);

		} else {
			// Recursion base case: instead of the bind replace, we return nullptr to indicate this time we do want to
			// do a regular bind phase
			return nullptr;
		}
	}

	static void Function(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
		auto &state = (BindReplaceDemoFun::CustomFunctionData &)*data.bind_data;

		if (!state.done) {
			output.data[0].Append(Value(state.current_depth));
			output.data[1].Append(Value(state.current_name));
			output.SetChildCardinality(1);
			state.done = true;
		} else {
			output.SetChildCardinality(0);
		}
	}

	static void Register(Connection &con) {
		// Create our test TableFunction
		con.BeginTransaction();
		auto &client_context = *con.context;
		auto &catalog = Catalog::GetSystemCatalog(client_context);
		TableFunction bind_replace_demo("bind_replace_demo", {LogicalType::BIGINT, LogicalType::VARCHAR},
		                                BindReplaceDemoFun::Function, BindReplaceDemoFun::Bind);
		bind_replace_demo.bind_replace = BindReplaceDemoFun::BindReplace;
		CreateTableFunctionInfo bind_replace_demo_info(bind_replace_demo);
		catalog.CreateTableFunction(*con.context, bind_replace_demo_info);
		con.Commit();
	}
};

// Simpler function that is effectively an alias for range()
struct BindReplaceDemoFun2 {
	struct CustomFunctionData : public TableFunctionData {
		bool done = false;
	};

	static duckdb::unique_ptr<TableRef> BindReplace(ClientContext &context, TableFunctionBindInput &input) {
		auto result = make_uniq<BindReplaceDemoFun2::CustomFunctionData>();

		auto value = input.inputs[0].GetValue<int64_t>();
		if (value < 0) {
			// Note: we are returning a nullptr in a table function without bind, this will fail
			return nullptr;
		}

		duckdb::vector<duckdb::unique_ptr<ParsedExpression>> children;
		children.push_back(ConstantExpression::FromValue(Value(value)));
		auto tf_ref = make_uniq<TableFunctionRef>();
		tf_ref->function = make_uniq<FunctionExpression>("range", std::move(children));

		return std::move(tf_ref);
	}

	static void Register(Connection &con) {
		// Create our test TableFunction
		con.BeginTransaction();
		auto &client_context = *con.context;
		auto &catalog = Catalog::GetSystemCatalog(client_context);
		TableFunction bind_replace_demo("bind_replace_demo2", {LogicalType::BIGINT}, nullptr, nullptr);
		bind_replace_demo.bind_replace = BindReplaceDemoFun2::BindReplace;
		CreateTableFunctionInfo bind_replace_demo_info(bind_replace_demo);
		catalog.CreateTableFunction(*con.context, bind_replace_demo_info);
		con.Commit();
	}
};

TEST_CASE("Table function with both bind and bindreplace", "[tablefunction]") {
	DuckDB db(nullptr);
	Connection con(db);
	BindReplaceDemoFun::Register(con);

	auto result = con.Query("DESCRIBE SELECT * FROM bind_replace_demo(2, 'hello_');");
	REQUIRE(result->RowCount() == 8);
	REQUIRE(CHECK_COLUMN(result, 0,
	                     {"depth_hello_LL", "col_hello_LL", "depth_hello_LR", "col_hello_LR", "depth_hello_RL",
	                      "col_hello_RL", "depth_hello_RR", "col_hello_RR"}));

	auto result2 = con.Query("SELECT depth_hello_LL, col_hello_LL FROM bind_replace_demo(2, 'hello_');");
	REQUIRE(result2->RowCount() == 1);
	REQUIRE(CHECK_COLUMN(result2, 0, {0}));
	REQUIRE(CHECK_COLUMN(result2, 1, {"hello_LL"}));
}

TEST_CASE("Table function with only bindreplace", "[tablefunction]") {
	DuckDB db(nullptr);
	Connection con(db);
	BindReplaceDemoFun2::Register(con);

	// Positive numbers simply will return the results from the range() call that was returned in the bind replace
	auto result = con.Query("SELECT * FROM bind_replace_demo2(3);");
	REQUIRE(result->RowCount() == 3);
	REQUIRE(CHECK_COLUMN(result, 0, {0, 1, 2}));

	// Negative numbers will not work: we have specified a bind replace, but no bind so returning a nullptr is not
	// allowed
	auto expect_err = con.Query("SELECT * FROM bind_replace_demo2(-3);");
	REQUIRE_THROWS(expect_err->Fetch());
	REQUIRE(expect_err->HasError());
	REQUIRE(StringUtil::Contains(expect_err->GetError(), "nullptr"));
}
