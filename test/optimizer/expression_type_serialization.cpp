#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/planner.hpp"

using namespace duckdb;

namespace {

static vector<string> GetCollations(const LogicalType &type) {
	vector<string> result;
	TypeVisitor::Contains(type, [&](const LogicalType &child) {
		if (child.id() == LogicalTypeId::VARCHAR) {
			result.push_back(StringType::GetCollation(child));
		}
		return false;
	});
	return result;
}

static void RequireExpressionType(const Expression &original, const Expression &copy) {
	REQUIRE(original.GetReturnType() == copy.GetReturnType());
	REQUIRE(GetCollations(original.GetReturnType()) == GetCollations(copy.GetReturnType()));
	REQUIRE(original.GetAlias() == copy.GetAlias());
}

static unique_ptr<Expression> CopyExpression(ClientContext &context, const Expression &expression,
                                             const StorageCompatibility &compatibility) {
	MemoryStream stream(Allocator::Get(context));
	SerializationOptions options;
	options.storage_compatibility = compatibility;
	BinarySerializer::Serialize(expression, stream, options);
	stream.Rewind();
	bound_parameter_map_t parameters;
	return BinaryDeserializer::Deserialize<Expression>(stream, context, parameters);
}

static void CheckExpressionTypes(ClientContext &context, const Expression &expression) {
	INFO("expression " << expression.ToString());
	RequireExpressionType(expression, *expression.Copy());
	for (const auto &compatibility :
	     {StorageCompatibility::FromIndex(StorageVersion::V1_5_0), StorageCompatibility::Latest()}) {
		auto copy = CopyExpression(context, expression, compatibility);
		RequireExpressionType(expression, *copy);
	}
	ExpressionIterator::EnumerateChildren(expression,
	                                      [&](const Expression &child) { CheckExpressionTypes(context, child); });
}

static void CheckPlanExpressionTypes(ClientContext &context, const LogicalOperator &plan) {
	for (auto &expression : plan.expressions) {
		CheckExpressionTypes(context, *expression);
	}
	for (auto &child : plan.children) {
		CheckPlanExpressionTypes(context, *child);
	}
}

struct ReplacementBindData : FunctionData {
	explicit ReplacementBindData(bool replace_p) : replace(replace_p) {
	}
	bool replace;
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ReplacementBindData>(replace);
	}
	bool Equals(const FunctionData &other) const override {
		return replace == other.Cast<ReplacementBindData>().replace;
	}
};

} // namespace

TEST_CASE("Expression copies preserve collations through binary serialization", "[serialization][expression_types]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE values_to_copy(v VARCHAR)"));
	connection.BeginTransaction();
	for (const auto &expression :
	     {"'A' COLLATE nocase", "NULL::VARCHAR COLLATE nocase", "concat(v,'') COLLATE nocase",
	      "concat(v,'') COLLATE \"binary\"", "v COLLATE nocase.noaccent", "[[v COLLATE nocase]]",
	      "[v COLLATE nocase,NULL]::VARCHAR[2]", "{'s': v COLLATE nocase}", "map([v COLLATE nocase], [v])",
	      "union_value(s := v COLLATE nocase)", "min(v COLLATE nocase)", "first_value(v COLLATE nocase) OVER ()"}) {
		CAPTURE(expression);
		Parser parser(*connection.context);
		parser.ParseQuery(string("SELECT ") + expression + " AS result FROM values_to_copy");
		Planner planner(*connection.context);
		planner.CreatePlan(std::move(parser.statements[0]));
		CheckPlanExpressionTypes(*connection.context, *planner.plan);
	}
	connection.Rollback();
}

TEST_CASE("Deserialized bind-expression replacements retain result annotations", "[serialization][expression_types]") {
	DuckDB db(nullptr);
	Connection connection(db);
	ExtensionLoader loader(*db.instance, "expression_type_copy");
	ScalarFunction function("copy_string", {LogicalType::VARCHAR}, LogicalType::VARCHAR, ScalarFunction::NopFunction);
	function.SetBindCallback(
	    [](BindScalarFunctionInput &) -> unique_ptr<FunctionData> { return make_uniq<ReplacementBindData>(false); });
	function.SetSerializeCallback([](Serializer &, const optional_ptr<FunctionData>, const BoundScalarFunction &) {});
	function.SetDeserializeCallback([](Deserializer &, BoundScalarFunction &) -> unique_ptr<FunctionData> {
		return make_uniq<ReplacementBindData>(true);
	});
	function.SetBindExpressionCallback([](FunctionBindExpressionInput &input) -> unique_ptr<Expression> {
		if (!input.bind_data->Cast<ReplacementBindData>().replace) {
			return nullptr;
		}
		if (input.children[0]->Cast<BoundConstantExpression>().GetValue() == Value("42")) {
			return make_uniq<BoundConstantExpression>(Value::INTEGER(42));
		}
		return input.children[0]->Copy();
	});
	loader.RegisterFunction(std::move(function));
	connection.BeginTransaction();
	Parser parser(*connection.context);
	parser.ParseQuery("SELECT copy_string('A') COLLATE nocase AS annotated");
	Planner planner(*connection.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	auto &expression = *planner.plan->expressions[0];
	REQUIRE(expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION);
	CheckExpressionTypes(*connection.context, expression);
	auto copy = CopyExpression(*connection.context, expression, StorageCompatibility::Latest());
	REQUIRE(copy->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT);
	REQUIRE(copy->Cast<BoundConstantExpression>().GetValue() == Value("A"));
	parser.statements.clear();
	parser.ParseQuery("SELECT copy_string('42') COLLATE nocase AS annotated");
	Planner cast_planner(*connection.context);
	cast_planner.CreatePlan(std::move(parser.statements[0]));
	auto &cast_expression = *cast_planner.plan->expressions[0];
	CheckExpressionTypes(*connection.context, cast_expression);
	auto cast_copy = CopyExpression(*connection.context, cast_expression, StorageCompatibility::Latest());
	REQUIRE(cast_copy->GetExpressionType() == ExpressionType::OPERATOR_CAST);

	connection.Rollback();
}

TEST_CASE("Expression rewrites preserve compatible result annotations", "[expression_rewriter][expression_types]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query(
	    "CREATE TABLE rewrite_values(v VARCHAR); INSERT INTO rewrite_values VALUES ('A'),('a'),(NULL)"));
	connection.BeginTransaction();
	for (const auto &expression :
	     {"(v || '') COLLATE nocase", "('' || v) COLLATE nocase", "replace(v,'x','x') COLLATE nocase",
	      "CASE WHEN true THEN v ELSE NULL END COLLATE nocase", "CASE WHEN false THEN NULL ELSE v END COLLATE nocase",
	      "coalesce(v,NULL) COLLATE nocase", "least(v,v) COLLATE nocase", "greatest(v,v) COLLATE nocase",
	      "('A' || '') COLLATE nocase", "CASE WHEN true THEN [[v]] ELSE [['A' COLLATE nocase]] END",
	      "CAST(12.5 AS DECIMAL(9,2))+CAST(0 AS DECIMAL(9,2))"}) {
		CAPTURE(expression);
		Parser parser(*connection.context);
		parser.ParseQuery(string("SELECT ") + expression + " AS result FROM rewrite_values");
		Planner planner(*connection.context);
		planner.CreatePlan(std::move(parser.statements[0]));
		planner.plan->ResolveOperatorTypes();
		auto type = planner.plan->types[0];
		Optimizer optimizer(*planner.binder, *connection.context);
		auto plan = optimizer.Optimize(std::move(planner.plan));
		plan->ResolveOperatorTypes();
		REQUIRE(plan->types[0] == type);
		REQUIRE(GetCollations(plan->types[0]) == GetCollations(type));
		std::function<void(LogicalOperator &)> check_rewrites = [&](LogicalOperator &op) {
			LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expr) {
				ExpressionIterator::VisitExpression<BoundFunctionExpression>(
				    **expr, [&](const BoundFunctionExpression &function) {
					    REQUIRE(function.Function().GetName() != "||");
					    REQUIRE(function.Function().GetName() != "replace");
				    });
			});
			for (auto &child : op.children) {
				check_rewrites(*child);
			}
		};
		check_rewrites(*plan);
	}
	connection.Rollback();
}

TEST_CASE("Constant mutations synchronize result types", "[expression_types]") {
	BoundConstantExpression expression(Value("A"));
	Expression &base = expression;
	auto type = LogicalType::VARCHAR_COLLATION("nocase");
	base.SetReturnType(type);
	REQUIRE(expression.GetReturnType().EqualsIncludingCollation(type));
	REQUIRE(expression.GetValue().type().EqualsIncludingCollation(type));
	expression.SetValue(Value::BIGINT(42));
	REQUIRE(expression.GetReturnType() == LogicalType::BIGINT);
	REQUIRE(expression.GetValue() == Value::BIGINT(42));
	auto value = expression.TakeValue();
	REQUIRE(value == Value::BIGINT(42));
	expression.SetValue(Value(type));
	REQUIRE(expression.GetValue().IsNull());
	REQUIRE(expression.GetReturnType().EqualsIncludingCollation(type));
}
