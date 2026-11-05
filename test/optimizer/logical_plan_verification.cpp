#include "catch.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/planner/logical_plan_verifier.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/main/database.hpp"
#include "test_helpers.hpp"

using namespace duckdb;

class VerificationExtensionOperator : public LogicalExtensionOperator {
public:
	VerificationExtensionOperator(string name_p, bool verify_types_p, vector<ColumnBinding> bindings_p,
	                              vector<LogicalType> resolved_types_p, vector<TableIndex> table_indexes_p = {},
	                              vector<unique_ptr<Expression>> expressions_p = {})
	    : LogicalExtensionOperator(std::move(expressions_p)), name(std::move(name_p)), verify_types(verify_types_p),
	      bindings(std::move(bindings_p)), resolved_types(std::move(resolved_types_p)),
	      table_indexes(std::move(table_indexes_p)) {
	}

	vector<ColumnBinding> GetColumnBindings() override {
		return bindings;
	}

	vector<TableIndex> GetTableIndex() const override {
		return table_indexes;
	}

	optional_ptr<const string> GetTypeBindingVerificationIdentifier() const noexcept override {
		if (!verify_types) {
			return nullptr;
		}
		return name;
	}

	string GetExtensionName() const override {
		return name;
	}

	PhysicalOperator &CreatePlan(ClientContext &, PhysicalPlanGenerator &) override {
		throw NotImplementedException("Synthetic verification operator cannot create a physical plan");
	}

protected:
	void ResolveTypes() override {
		types = resolved_types;
	}

private:
	string name;
	bool verify_types;
	vector<ColumnBinding> bindings;
	vector<LogicalType> resolved_types;
	vector<TableIndex> table_indexes;
};

class UnnamedVerificationExtensionOperator : public LogicalExtensionOperator {
public:
	UnnamedVerificationExtensionOperator(string identifier_p, vector<ColumnBinding> bindings_p,
	                                     vector<LogicalType> resolved_types_p)
	    : identifier(std::move(identifier_p)), bindings(std::move(bindings_p)),
	      resolved_types(std::move(resolved_types_p)) {
	}

	vector<ColumnBinding> GetColumnBindings() override {
		return bindings;
	}

	optional_ptr<const string> GetTypeBindingVerificationIdentifier() const noexcept override {
		return identifier;
	}

	PhysicalOperator &CreatePlan(ClientContext &, PhysicalPlanGenerator &) override {
		throw NotImplementedException("Synthetic verification operator cannot create a physical plan");
	}

protected:
	void ResolveTypes() override {
		types = resolved_types;
	}

private:
	string identifier;
	vector<ColumnBinding> bindings;
	vector<LogicalType> resolved_types;
};

class IncompleteTypeLeaf : public LogicalOperator {
public:
	IncompleteTypeLeaf(TableIndex table_index_p, LogicalType type_p)
	    : LogicalOperator(LogicalOperatorType::LOGICAL_DUMMY_SCAN), table_index(table_index_p),
	      type(std::move(type_p)) {
	}

	vector<ColumnBinding> GetColumnBindings() override {
		return {ColumnBinding(table_index, ProjectionIndex(0))};
	}

	vector<TableIndex> GetTableIndex() const override {
		return {table_index};
	}

protected:
	void ResolveTypes() override {
		types = {type};
	}

private:
	TableIndex table_index;
	LogicalType type;
};

static unique_ptr<VerificationExtensionOperator> TypedLeaf(TableIndex table_index, LogicalType type,
                                                           bool verify_types = true) {
	auto binding = ColumnBinding(table_index, ProjectionIndex(0));
	return make_uniq<VerificationExtensionOperator>("typed_verification_leaf", verify_types,
	                                                vector<ColumnBinding> {binding}, vector<LogicalType> {type},
	                                                vector<TableIndex> {table_index});
}

static unique_ptr<LogicalProjection> ReferenceProjection(TableIndex table_index, ColumnBinding binding,
                                                         LogicalType type, unique_ptr<LogicalOperator> child,
                                                         const string &alias = string()) {
	vector<unique_ptr<Expression>> expressions;
	if (alias.empty()) {
		expressions.push_back(make_uniq<BoundColumnRefExpression>(type, binding));
	} else {
		expressions.push_back(make_uniq<BoundColumnRefExpression>(Identifier(alias), type, binding));
	}
	auto result = make_uniq<LogicalProjection>(table_index, std::move(expressions));
	result->children.push_back(std::move(child));
	return result;
}

static unique_ptr<VerificationExtensionOperator> ExpressionLeaf(TableIndex table_index, ColumnBinding binding,
                                                                const string &alias) {
	vector<unique_ptr<Expression>> expressions;
	expressions.push_back(make_uniq<BoundColumnRefExpression>(Identifier(alias), LogicalType::INTEGER, binding));
	auto output_binding = ColumnBinding(table_index, ProjectionIndex(0));
	return make_uniq<VerificationExtensionOperator>(
	    "typed_verification_expression_leaf", true, vector<ColumnBinding> {output_binding},
	    vector<LogicalType> {LogicalType::INTEGER}, vector<TableIndex> {table_index}, std::move(expressions));
}

static unique_ptr<VerificationExtensionOperator> ExpressionPassThrough(TableIndex child_index,
                                                                       ColumnBinding expression_binding,
                                                                       LogicalType expression_type,
                                                                       const string &alias) {
	auto child_binding = ColumnBinding(child_index, ProjectionIndex(0));
	vector<unique_ptr<Expression>> expressions;
	expressions.push_back(
	    make_uniq<BoundColumnRefExpression>(Identifier(alias), std::move(expression_type), expression_binding));
	auto result = make_uniq<VerificationExtensionOperator>(
	    "typed_verification_expression_passthrough", true, vector<ColumnBinding> {child_binding},
	    vector<LogicalType> {LogicalType::INTEGER}, vector<TableIndex> {}, std::move(expressions));
	result->children.push_back(TypedLeaf(child_index, LogicalType::INTEGER));
	return result;
}

static unique_ptr<VerificationExtensionOperator> MalformedLeaf(TableIndex table_index) {
	return make_uniq<VerificationExtensionOperator>(
	    "typed_verification_malformed_leaf", true,
	    vector<ColumnBinding> {ColumnBinding(table_index, ProjectionIndex(0)),
	                           ColumnBinding(table_index, ProjectionIndex(1))},
	    vector<LogicalType> {LogicalType::INTEGER}, vector<TableIndex> {table_index});
}

static unique_ptr<VerificationExtensionOperator> InvalidPassThrough(TableIndex child_index, TableIndex invalid_index,
                                                                    const string &alias);

static const Value &GetFact(const LogicalPlanVerificationIssue &issue, const string &name) {
	for (auto &fact : issue.facts) {
		if (fact.first == name) {
			return fact.second;
		}
	}
	throw InternalException("Missing logical plan verification issue fact");
}

static void RequireInvariant(const LogicalPlanVerificationIssue &issue, const string &invariant,
                             const LogicalPlanVerificationPath &path) {
	REQUIRE(issue.code == LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
	REQUIRE(issue.phase == LogicalPlanVerificationPhase::VERIFY);
	REQUIRE(issue.path == path);
	REQUIRE(GetFact(issue, "invariant") == Value(invariant));
}

static unique_ptr<LogicalExpressionGet> IntegerExpressionGet(TableIndex table_index) {
	vector<vector<unique_ptr<Expression>>> rows;
	vector<unique_ptr<Expression>> row;
	row.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(42)));
	rows.push_back(std::move(row));
	return make_uniq<LogicalExpressionGet>(table_index, vector<LogicalType> {LogicalType::INTEGER}, std::move(rows));
}

TEST_CASE("Logical plan verification rejects unsafe logical operator slots", "[logical_plan_verification]") {
	SECTION("null child slot") {
		vector<unique_ptr<Expression>> expressions;
		expressions.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(42)));
		auto plan = make_uniq<LogicalProjection>(TableIndex(1), std::move(expressions));
		plan->children.push_back(nullptr);

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		RequireInvariant(result.GetIssues()[0], "null_operator_child",
		                 LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                              {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0}}});
		REQUIRE(GetFact(result.GetIssues()[0], "child_index") == Value::UBIGINT(0));
	}

	SECTION("null operator expression slot") {
		vector<unique_ptr<Expression>> expressions;
		expressions.push_back(nullptr);
		auto plan = make_uniq<LogicalProjection>(TableIndex(2), std::move(expressions));
		plan->children.push_back(make_uniq<LogicalDummyScan>(TableIndex(3)));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		RequireInvariant(
		    result.GetIssues()[0], "null_operator_expression",
		    LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                 {{LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}});
		REQUIRE(GetFact(result.GetIssues()[0], "expression_index") == Value::UBIGINT(0));
	}
}

TEST_CASE("Logical plan verification retains valid values and filter projection", "[logical_plan_verification]") {
	auto values = IntegerExpressionGet(TableIndex(19));
	values->children.push_back(make_uniq<LogicalDummyScan>(TableIndex(20)));
	auto filter = make_uniq<LogicalFilter>(make_uniq<BoundConstantExpression>(Value::BOOLEAN(true)));
	filter->projection_map.push_back(ProjectionIndex(0));
	filter->children.push_back(std::move(values));
	REQUIRE(LogicalPlanVerifier::VerifyAlways(*filter).IsSuccess());
	REQUIRE(filter->types == vector<LogicalType> {LogicalType::INTEGER});
}

TEST_CASE("Logical plan verification accepts typed extension operators", "[logical_plan_verification]") {
	SECTION("typed leaf") {
		auto child_index = TableIndex(10);
		auto plan = ReferenceProjection(TableIndex(11), ColumnBinding(child_index, ProjectionIndex(0)),
		                                LogicalType::INTEGER, TypedLeaf(child_index, LogicalType::INTEGER));
		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.IsSuccess());
	}

	SECTION("typed pass-through") {
		auto child_index = TableIndex(20);
		auto binding = ColumnBinding(child_index, ProjectionIndex(0));
		vector<unique_ptr<Expression>> expressions;
		expressions.push_back(make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, binding));
		auto pass_through = make_uniq<VerificationExtensionOperator>(
		    "typed_verification_passthrough", true, vector<ColumnBinding> {binding},
		    vector<LogicalType> {LogicalType::INTEGER}, vector<TableIndex> {}, std::move(expressions));
		pass_through->children.push_back(TypedLeaf(child_index, LogicalType::INTEGER));
		auto plan = ReferenceProjection(TableIndex(21), binding, LogicalType::INTEGER, std::move(pass_through));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.IsSuccess());
	}

	SECTION("built-in plan") {
		auto child_index = TableIndex(30);
		auto child = make_uniq<LogicalDummyScan>(child_index);
		auto plan = ReferenceProjection(TableIndex(31), ColumnBinding(child_index, ProjectionIndex(0)),
		                                LogicalType::INTEGER, std::move(child));
		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.IsSuccess());
	}
}

TEST_CASE("Logical plan verification reports exact binding and type paths", "[logical_plan_verification]") {
	SECTION("invalid extension expression binding") {
		auto child_index = TableIndex(40);
		auto child_binding = ColumnBinding(child_index, ProjectionIndex(0));
		auto invalid_binding = ColumnBinding(TableIndex(400), ProjectionIndex(3));
		vector<unique_ptr<Expression>> expressions;
		expressions.push_back(make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, invalid_binding));
		auto pass_through = make_uniq<VerificationExtensionOperator>(
		    "typed_verification_invalid_binding", true, vector<ColumnBinding> {child_binding},
		    vector<LogicalType> {LogicalType::INTEGER}, vector<TableIndex> {}, std::move(expressions));
		pass_through->children.push_back(TypedLeaf(child_index, LogicalType::INTEGER));
		auto plan = ReferenceProjection(TableIndex(41), child_binding, LogicalType::INTEGER, std::move(pass_through));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		auto &issue = result.GetIssues()[0];
		REQUIRE(issue.code == LogicalPlanVerificationIssueCode::INVALID_BINDING);
		REQUIRE(issue.path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0},
		                                      {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}});
		REQUIRE(GetFact(issue, "table_index") == Value::UBIGINT(400));
		REQUIRE(GetFact(issue, "column_index") == Value::UBIGINT(3));
	}

	SECTION("parent reference type mismatch") {
		auto child_index = TableIndex(50);
		auto plan = ReferenceProjection(TableIndex(51), ColumnBinding(child_index, ProjectionIndex(0)),
		                                LogicalType::VARCHAR, TypedLeaf(child_index, LogicalType::INTEGER));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		auto &issue = result.GetIssues()[0];
		REQUIRE(issue.code == LogicalPlanVerificationIssueCode::TYPE_MISMATCH);
		REQUIRE(issue.path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}});
		REQUIRE(issue.construct->type == LogicalPlanVerificationConstructType::BINDING_TYPE_MISMATCH);
		REQUIRE(issue.construct->type_mismatch->expected_type == LogicalType::INTEGER);
		REQUIRE(issue.construct->type_mismatch->actual_type == LogicalType::VARCHAR);
	}

	SECTION("nested expression binding") {
		auto child_index = TableIndex(55);
		auto child_binding = ColumnBinding(child_index, ProjectionIndex(0));
		auto invalid_binding = ColumnBinding(TableIndex(550), ProjectionIndex(0));
		vector<unique_ptr<Expression>> expressions;
		expressions.push_back(make_uniq<BoundConjunctionExpression>(
		    ExpressionType::CONJUNCTION_AND, make_uniq<BoundColumnRefExpression>(LogicalType::BOOLEAN, invalid_binding),
		    make_uniq<BoundConstantExpression>(Value::BOOLEAN(true))));
		auto pass_through = make_uniq<VerificationExtensionOperator>(
		    "typed_verification_nested_binding", true, vector<ColumnBinding> {child_binding},
		    vector<LogicalType> {LogicalType::INTEGER}, vector<TableIndex> {}, std::move(expressions));
		pass_through->children.push_back(TypedLeaf(child_index, LogicalType::INTEGER));
		auto plan = ReferenceProjection(TableIndex(56), child_binding, LogicalType::INTEGER, std::move(pass_through));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0},
		                                      {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0},
		                                      {LogicalPlanVerificationPathComponentType::EXPRESSION_CHILD, 0}}});
	}
}

TEST_CASE("Logical plan verification structures malformed expression bindings", "[logical_plan_verification]") {
	auto VerifyInvalidBinding = [](unique_ptr<LogicalOperator> plan, const LogicalPlanVerificationPath &expected_path,
	                               const ColumnBinding &expected_binding) {
		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		auto &issue = result.GetIssues()[0];
		REQUIRE(issue.code == LogicalPlanVerificationIssueCode::INVALID_BINDING);
		REQUIRE(issue.path == expected_path);
		REQUIRE(GetFact(issue, "table_index") == Value::UBIGINT(expected_binding.table_index.index));
		REQUIRE(GetFact(issue, "column_index") == Value::UBIGINT(expected_binding.column_index.GetIndexUnsafe()));
		REQUIRE(GetFact(issue, "table_index_valid") == Value::BOOLEAN(expected_binding.table_index.IsValid()));
		REQUIRE(GetFact(issue, "column_index_valid") == Value::BOOLEAN(expected_binding.column_index.IsValid()));
	};

	SECTION("root invalid table component") {
		auto binding = ColumnBinding(TableIndex(), ProjectionIndex(0));
		auto plan = ExpressionPassThrough(TableIndex(401), binding, LogicalType::INTEGER, "root_invalid_table");
		VerifyInvalidBinding(
		    std::move(plan),
		    LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                 {{LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}},
		    binding);
	}

	SECTION("root invalid column component") {
		auto binding = ColumnBinding(TableIndex(402), ProjectionIndex());
		auto plan = ExpressionPassThrough(TableIndex(402), binding, LogicalType::INTEGER, "root_invalid_column");
		VerifyInvalidBinding(
		    std::move(plan),
		    LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                 {{LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}},
		    binding);
	}

	SECTION("root invalid table and column components") {
		auto binding = ColumnBinding(TableIndex(), ProjectionIndex());
		auto plan = ExpressionPassThrough(TableIndex(403), binding, LogicalType::INTEGER, "root_invalid_both");
		VerifyInvalidBinding(
		    std::move(plan),
		    LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                 {{LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}},
		    binding);
	}

	SECTION("nested invalid table component") {
		auto child_index = TableIndex(404);
		auto binding = ColumnBinding(TableIndex(), ProjectionIndex(0));
		auto pass_through = ExpressionPassThrough(child_index, binding, LogicalType::INTEGER, "nested_invalid_table");
		auto plan = ReferenceProjection(TableIndex(405), ColumnBinding(child_index, ProjectionIndex(0)),
		                                LogicalType::INTEGER, std::move(pass_through));
		VerifyInvalidBinding(
		    std::move(plan),
		    LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                 {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0},
		                                  {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}},
		    binding);
	}

	SECTION("nested invalid column component") {
		auto child_index = TableIndex(406);
		auto binding = ColumnBinding(child_index, ProjectionIndex());
		auto pass_through = ExpressionPassThrough(child_index, binding, LogicalType::INTEGER, "nested_invalid_column");
		auto plan = ReferenceProjection(TableIndex(407), ColumnBinding(child_index, ProjectionIndex(0)),
		                                LogicalType::INTEGER, std::move(pass_through));
		VerifyInvalidBinding(
		    std::move(plan),
		    LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                 {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0},
		                                  {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}},
		    binding);
	}

	SECTION("nested invalid table and column components") {
		auto child_index = TableIndex(408);
		auto binding = ColumnBinding(TableIndex(), ProjectionIndex());
		auto pass_through = ExpressionPassThrough(child_index, binding, LogicalType::INTEGER, "nested_invalid_both");
		auto plan = ReferenceProjection(TableIndex(409), ColumnBinding(child_index, ProjectionIndex(0)),
		                                LogicalType::INTEGER, std::move(pass_through));
		VerifyInvalidBinding(
		    std::move(plan),
		    LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                 {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0},
		                                  {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}},
		    binding);
	}
}

TEST_CASE("Logical plan verification isolates extension leaf input scope", "[logical_plan_verification]") {
	auto left_index = TableIndex(57);
	auto right_index = TableIndex(58);
	auto sibling_binding = ColumnBinding(left_index, ProjectionIndex(0));

	SECTION("root binary operator") {
		auto plan = make_uniq<LogicalCrossProduct>(TypedLeaf(left_index, LogicalType::INTEGER),
		                                           ExpressionLeaf(right_index, sibling_binding, "sibling_reference"));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::INVALID_BINDING);
		REQUIRE(result.GetIssues()[0].path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 1},
		                                      {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}});
	}

	SECTION("nested binary operator") {
		auto cross_product =
		    make_uniq<LogicalCrossProduct>(TypedLeaf(left_index, LogicalType::INTEGER),
		                                   ExpressionLeaf(right_index, sibling_binding, "nested_sibling_reference"));
		auto plan = ReferenceProjection(TableIndex(59), ColumnBinding(right_index, ProjectionIndex(0)),
		                                LogicalType::INTEGER, std::move(cross_product));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::INVALID_BINDING);
		REQUIRE(result.GetIssues()[0].path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0},
		                                      {LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 1},
		                                      {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}});
	}
}

TEST_CASE("Logical plan verification rejects malformed extension output and duplicate indexes",
          "[logical_plan_verification]") {
	SECTION("output arity") {
		auto child_index = TableIndex(60);
		auto malformed = make_uniq<VerificationExtensionOperator>(
		    "typed_verification_malformed", true,
		    vector<ColumnBinding> {ColumnBinding(child_index, ProjectionIndex(0)),
		                           ColumnBinding(child_index, ProjectionIndex(1))},
		    vector<LogicalType> {LogicalType::INTEGER}, vector<TableIndex> {child_index});
		auto plan = ReferenceProjection(TableIndex(61), ColumnBinding(child_index, ProjectionIndex(0)),
		                                LogicalType::INTEGER, std::move(malformed));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		auto &issue = result.GetIssues()[0];
		REQUIRE(issue.code == LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT);
		REQUIRE(issue.path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0}}});
		REQUIRE(issue.construct->identifier == "typed_verification_malformed");
		REQUIRE(GetFact(issue, "binding_count") == Value::UBIGINT(2));
		REQUIRE(GetFact(issue, "type_count") == Value::UBIGINT(1));
	}

	SECTION("duplicate table index") {
		auto duplicate_index = TableIndex(70);
		auto plan = make_uniq<LogicalCrossProduct>(TypedLeaf(duplicate_index, LogicalType::INTEGER),
		                                           TypedLeaf(duplicate_index, LogicalType::INTEGER));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		auto &issue = result.GetIssues()[0];
		REQUIRE(issue.code == LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
		REQUIRE(issue.path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 1}}});
		REQUIRE(GetFact(issue, "invariant") == Value("duplicate_table_index"));
		REQUIRE(GetFact(issue, "table_index") == Value::UBIGINT(70));
	}

	SECTION("parent duplicate table index") {
		auto duplicate_index = TableIndex(71);
		auto binding = ColumnBinding(duplicate_index, ProjectionIndex(0));
		auto plan = make_uniq<VerificationExtensionOperator>(
		    "typed_verification_parent_index", true, vector<ColumnBinding> {binding},
		    vector<LogicalType> {LogicalType::INTEGER}, vector<TableIndex> {duplicate_index});
		plan->children.push_back(TypedLeaf(duplicate_index, LogicalType::INTEGER));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
		REQUIRE(result.GetIssues()[0].path == LogicalPlanVerificationPath {});
	}

	SECTION("duplicate issues are suppressed") {
		auto duplicate_index = TableIndex(75);
		auto plan = make_uniq<VerificationExtensionOperator>(
		    "typed_verification_repeated_index", true,
		    vector<ColumnBinding> {ColumnBinding(duplicate_index, ProjectionIndex(0))},
		    vector<LogicalType> {LogicalType::INTEGER},
		    vector<TableIndex> {duplicate_index, duplicate_index, duplicate_index});

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].path == LogicalPlanVerificationPath {});
	}

	SECTION("invalid root output type") {
		auto plan = TypedLeaf(TableIndex(76), LogicalType::INVALID);

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT);
		REQUIRE(result.GetIssues()[0].path == LogicalPlanVerificationPath {});
		REQUIRE(GetFact(result.GetIssues()[0], "invalid_type_index") == Value::UBIGINT(0));
	}

	SECTION("arity and invalid output type") {
		auto table_index = TableIndex(761);
		auto plan = make_uniq<VerificationExtensionOperator>(
		    "typed_verification_malformed_type", true,
		    vector<ColumnBinding> {ColumnBinding(table_index, ProjectionIndex(0)),
		                           ColumnBinding(table_index, ProjectionIndex(1))},
		    vector<LogicalType> {LogicalType::INVALID}, vector<TableIndex> {table_index});

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 2);
		REQUIRE(GetFact(result.GetIssues()[0], "binding_count") == Value::UBIGINT(2));
		REQUIRE(GetFact(result.GetIssues()[1], "invalid_type_index") == Value::UBIGINT(0));
	}

	SECTION("invalid child output type") {
		auto child_index = TableIndex(77);
		auto plan = ReferenceProjection(TableIndex(78), ColumnBinding(child_index, ProjectionIndex(0)),
		                                LogicalType::INTEGER, TypedLeaf(child_index, LogicalType::INVALID));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT);
		REQUIRE(result.GetIssues()[0].path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0}}});
	}

	SECTION("invalid root output table index") {
		auto binding = ColumnBinding(TableIndex(), ProjectionIndex(0));
		auto plan = make_uniq<VerificationExtensionOperator>("typed_verification_invalid_table_index", true,
		                                                     vector<ColumnBinding> {binding},
		                                                     vector<LogicalType> {LogicalType::INTEGER});

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT);
		REQUIRE(result.GetIssues()[0].path == LogicalPlanVerificationPath {});
		REQUIRE(GetFact(result.GetIssues()[0], "invalid_binding_index") == Value::UBIGINT(0));
		REQUIRE(GetFact(result.GetIssues()[0], "table_index_valid") == Value::BOOLEAN(false));
		REQUIRE(GetFact(result.GetIssues()[0], "column_index_valid") == Value::BOOLEAN(true));
	}

	SECTION("invalid parent-consumed output table index") {
		auto binding = ColumnBinding(TableIndex(), ProjectionIndex(0));
		auto child = make_uniq<VerificationExtensionOperator>("typed_verification_invalid_table_index", true,
		                                                      vector<ColumnBinding> {binding},
		                                                      vector<LogicalType> {LogicalType::INTEGER});
		auto plan = ReferenceProjection(TableIndex(781), binding, LogicalType::INTEGER, std::move(child));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT);
		REQUIRE(result.GetIssues()[0].path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0}}});
	}

	SECTION("invalid root output column index") {
		auto binding = ColumnBinding(TableIndex(782), ProjectionIndex());
		auto plan = make_uniq<VerificationExtensionOperator>("typed_verification_invalid_column_index", true,
		                                                     vector<ColumnBinding> {binding},
		                                                     vector<LogicalType> {LogicalType::INTEGER});

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT);
		REQUIRE(result.GetIssues()[0].path == LogicalPlanVerificationPath {});
		REQUIRE(GetFact(result.GetIssues()[0], "invalid_binding_index") == Value::UBIGINT(0));
		REQUIRE(GetFact(result.GetIssues()[0], "table_index_valid") == Value::BOOLEAN(true));
		REQUIRE(GetFact(result.GetIssues()[0], "column_index_valid") == Value::BOOLEAN(false));
	}

	SECTION("invalid parent-consumed output column index") {
		auto binding = ColumnBinding(TableIndex(783), ProjectionIndex());
		auto child = make_uniq<VerificationExtensionOperator>("typed_verification_invalid_column_index", true,
		                                                      vector<ColumnBinding> {binding},
		                                                      vector<LogicalType> {LogicalType::INTEGER});
		auto plan = ReferenceProjection(TableIndex(784), binding, LogicalType::INTEGER, std::move(child));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT);
		REQUIRE(result.GetIssues()[0].path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0}}});
	}

	SECTION("duplicate output binding with the same type") {
		auto binding = ColumnBinding(TableIndex(785), ProjectionIndex(0));
		auto plan = make_uniq<VerificationExtensionOperator>(
		    "typed_verification_duplicate_binding_same_type", true, vector<ColumnBinding> {binding, binding},
		    vector<LogicalType> {LogicalType::INTEGER, LogicalType::INTEGER});

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT);
		REQUIRE(result.GetIssues()[0].path == LogicalPlanVerificationPath {});
		REQUIRE(GetFact(result.GetIssues()[0], "first_binding_index") == Value::UBIGINT(0));
		REQUIRE(GetFact(result.GetIssues()[0], "duplicate_binding_index") == Value::UBIGINT(1));
		REQUIRE(GetFact(result.GetIssues()[0], "types_equal") == Value::BOOLEAN(true));
	}

	SECTION("duplicate output binding with conflicting types") {
		auto binding = ColumnBinding(TableIndex(786), ProjectionIndex(0));
		auto plan = make_uniq<VerificationExtensionOperator>(
		    "typed_verification_duplicate_binding_conflicting_type", true, vector<ColumnBinding> {binding, binding},
		    vector<LogicalType> {LogicalType::INTEGER, LogicalType::VARCHAR});

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT);
		REQUIRE(GetFact(result.GetIssues()[0], "types_equal") == Value::BOOLEAN(false));
	}

	SECTION("parent cannot consume duplicate output binding with the same type") {
		auto binding = ColumnBinding(TableIndex(787), ProjectionIndex(0));
		auto child = make_uniq<VerificationExtensionOperator>(
		    "typed_verification_duplicate_binding_same_type", true, vector<ColumnBinding> {binding, binding},
		    vector<LogicalType> {LogicalType::INTEGER, LogicalType::INTEGER});
		auto plan = ReferenceProjection(TableIndex(788), binding, LogicalType::INTEGER, std::move(child));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT);
		REQUIRE(result.GetIssues()[0].path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0}}});
	}

	SECTION("parent cannot consume duplicate output binding with conflicting types") {
		auto binding = ColumnBinding(TableIndex(789), ProjectionIndex(0));
		auto child = make_uniq<VerificationExtensionOperator>(
		    "typed_verification_duplicate_binding_conflicting_type", true, vector<ColumnBinding> {binding, binding},
		    vector<LogicalType> {LogicalType::INTEGER, LogicalType::VARCHAR});
		auto plan = ReferenceProjection(TableIndex(790), binding, LogicalType::INTEGER, std::move(child));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT);
		REQUIRE(result.GetIssues()[0].path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0}}});
	}

	SECTION("invalid root table-index ownership") {
		auto binding = ColumnBinding(TableIndex(793), ProjectionIndex(0));
		auto plan = make_uniq<VerificationExtensionOperator>(
		    "typed_verification_invalid_table_ownership", true, vector<ColumnBinding> {binding},
		    vector<LogicalType> {LogicalType::INTEGER}, vector<TableIndex> {TableIndex()});

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
		REQUIRE(result.GetIssues()[0].path == LogicalPlanVerificationPath {});
		REQUIRE(GetFact(result.GetIssues()[0], "invariant") == Value("invalid_table_index"));
		REQUIRE(GetFact(result.GetIssues()[0], "table_index_ordinal") == Value::UBIGINT(0));
		REQUIRE(GetFact(result.GetIssues()[0], "table_index_valid") == Value::BOOLEAN(false));
	}

	SECTION("invalid nested table-index ownership") {
		auto binding = ColumnBinding(TableIndex(794), ProjectionIndex(0));
		auto child = make_uniq<VerificationExtensionOperator>(
		    "typed_verification_invalid_table_ownership", true, vector<ColumnBinding> {binding},
		    vector<LogicalType> {LogicalType::INTEGER}, vector<TableIndex> {TableIndex()});
		auto plan = ReferenceProjection(TableIndex(795), binding, LogicalType::INTEGER, std::move(child));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
		REQUIRE(result.GetIssues()[0].path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0}}});
	}

	SECTION("unnamed malformed extension") {
		auto table_index = TableIndex(79);
		auto plan = make_uniq<UnnamedVerificationExtensionOperator>(
		    "non_serializable_verification_extension",
		    vector<ColumnBinding> {ColumnBinding(table_index, ProjectionIndex(0)),
		                           ColumnBinding(table_index, ProjectionIndex(1))},
		    vector<LogicalType> {LogicalType::INTEGER});

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT);
		REQUIRE(result.GetIssues()[0].construct->identifier == "non_serializable_verification_extension");
	}

	SECTION("malformed extensions retain distinct identities") {
		auto first = make_uniq<VerificationExtensionOperator>(
		    "first_malformed_extension", true,
		    vector<ColumnBinding> {ColumnBinding(TableIndex(791), ProjectionIndex(0))}, vector<LogicalType> {});
		auto second = make_uniq<VerificationExtensionOperator>(
		    "second_malformed_extension", true,
		    vector<ColumnBinding> {ColumnBinding(TableIndex(792), ProjectionIndex(0))}, vector<LogicalType> {});

		auto first_result = LogicalPlanVerifier::VerifyAlways(*first);
		auto second_result = LogicalPlanVerifier::VerifyAlways(*second);
		REQUIRE(first_result.IsValid());
		REQUIRE(second_result.IsValid());
		REQUIRE(first_result.GetIssues().size() == 1);
		REQUIRE(second_result.GetIssues().size() == 1);
		REQUIRE(first_result.GetIssues()[0].construct->identifier == "first_malformed_extension");
		REQUIRE(second_result.GetIssues()[0].construct->identifier == "second_malformed_extension");
		REQUIRE_FALSE(first_result.GetIssues()[0] == second_result.GetIssues()[0]);
	}

	SECTION("malformed extension result owns its identity") {
		auto plan = make_uniq<VerificationExtensionOperator>(
		    "owned_verification_identifier", true,
		    vector<ColumnBinding> {ColumnBinding(TableIndex(796), ProjectionIndex(0))}, vector<LogicalType> {});

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		plan.reset();
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		REQUIRE(result.GetIssues()[0].construct->identifier == "owned_verification_identifier");
	}

	SECTION("independent malformed children") {
		auto plan = make_uniq<LogicalCrossProduct>(MalformedLeaf(TableIndex(180)), MalformedLeaf(TableIndex(181)));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 2);
		REQUIRE(result.GetIssues()[0].path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0}}});
		REQUIRE(result.GetIssues()[1].path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 1}}});
	}

	SECTION("malformed binding and duplicate defects") {
		auto duplicate_index = TableIndex(182);
		auto plan = make_uniq<LogicalCrossProduct>(
		    MalformedLeaf(duplicate_index),
		    ExpressionLeaf(duplicate_index, ColumnBinding(TableIndex(999), ProjectionIndex(0)), "invalid_binding"));

		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 3);
		REQUIRE(result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT);
		REQUIRE(result.GetIssues()[1].code == LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
		REQUIRE(result.GetIssues()[2].code == LogicalPlanVerificationIssueCode::INVALID_BINDING);
		REQUIRE(result.GetIssues()[2].path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 1},
		                                      {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}});
	}
}

TEST_CASE("Logical plan verification preserves setting and legacy extension behavior", "[logical_plan_verification]") {
	DuckDB db;
	Connection connection(db);
	auto child_index = TableIndex(80);
	auto invalid_binding = ColumnBinding(TableIndex(800), ProjectionIndex(0));
	auto invalid_plan = ReferenceProjection(TableIndex(81), invalid_binding, LogicalType::INTEGER,
	                                        TypedLeaf(child_index, LogicalType::INTEGER));

	REQUIRE_NO_FAIL(connection.Query("SET debug_verify_column_bindings=false"));
	invalid_plan->types = {LogicalType::VARCHAR};
	REQUIRE_NOTHROW(LogicalPlanVerifier::Verify(*connection.context, *invalid_plan));
	REQUIRE(invalid_plan->types == vector<LogicalType> {LogicalType::VARCHAR});
	REQUIRE(LogicalPlanVerifier::VerifyAlways(*invalid_plan).HasError());

	REQUIRE_NO_FAIL(connection.Query("SET debug_verify_column_bindings=true"));
#ifndef DUCKDB_CRASH_ON_ASSERT
	REQUIRE_THROWS_AS(LogicalPlanVerifier::Verify(*connection.context, *invalid_plan), InternalException);
#endif

	auto valid_plan = ReferenceProjection(TableIndex(82), ColumnBinding(child_index, ProjectionIndex(0)),
	                                      LogicalType::INTEGER, TypedLeaf(child_index, LogicalType::INTEGER));
	REQUIRE_NOTHROW(LogicalPlanVerifier::Verify(*connection.context, *valid_plan));

	vector<unique_ptr<Expression>> malformed_expressions;
	malformed_expressions.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(42)));
	auto malformed_plan = make_uniq<LogicalProjection>(TableIndex(83), std::move(malformed_expressions));
	malformed_plan->children.push_back(nullptr);
	REQUIRE_NO_FAIL(connection.Query("SET debug_verify_column_bindings=false"));
	REQUIRE_NOTHROW(LogicalPlanVerifier::Verify(*connection.context, *malformed_plan));
	REQUIRE_NO_FAIL(connection.Query("SET debug_verify_column_bindings=true"));
#ifndef DUCKDB_CRASH_ON_ASSERT
	REQUIRE_THROWS_AS(LogicalPlanVerifier::Verify(*connection.context, *malformed_plan), InternalException);
#endif

	auto legacy_index = TableIndex(90);
	auto legacy_plan = ReferenceProjection(TableIndex(91), ColumnBinding(legacy_index, ProjectionIndex(0)),
	                                       LogicalType::VARCHAR, TypedLeaf(legacy_index, LogicalType::INTEGER, false));
	auto legacy_result = LogicalPlanVerifier::VerifyAlways(*legacy_plan);
	REQUIRE(legacy_result.IsValid());
	REQUIRE(legacy_result.IsSuccess());

	auto identifier_index = TableIndex(901);
	auto unidentified_opt_in = make_uniq<VerificationExtensionOperator>(
	    "", true, vector<ColumnBinding> {ColumnBinding(identifier_index, ProjectionIndex(0))},
	    vector<LogicalType> {LogicalType::INTEGER});
	auto unidentified_opt_in_result = LogicalPlanVerifier::VerifyAlways(*unidentified_opt_in);
	REQUIRE(unidentified_opt_in_result.IsValid());
	REQUIRE(unidentified_opt_in_result.GetIssues().size() == 1);
	REQUIRE(unidentified_opt_in_result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
	REQUIRE(unidentified_opt_in_result.GetIssues()[0].path == LogicalPlanVerificationPath {});
	REQUIRE(GetFact(unidentified_opt_in_result.GetIssues()[0], "invariant") ==
	        Value("missing_type_binding_verification_identifier"));
	auto unidentified_legacy = make_uniq<VerificationExtensionOperator>(
	    "", false, vector<ColumnBinding> {ColumnBinding(identifier_index, ProjectionIndex(0))},
	    vector<LogicalType> {LogicalType::INTEGER});
	auto unidentified_legacy_result = LogicalPlanVerifier::VerifyAlways(*unidentified_legacy);
	REQUIRE(unidentified_legacy_result.IsValid());
	REQUIRE(unidentified_legacy_result.IsSuccess());

#ifndef DUCKDB_CRASH_ON_ASSERT
	auto child_failure = InvalidPassThrough(TableIndex(92), TableIndex(920), "legacy_child_first");
	auto mixed_failure = ReferenceProjection(TableIndex(93), ColumnBinding(TableIndex(930), ProjectionIndex(0)),
	                                         LogicalType::INTEGER, std::move(child_failure), "canonical_parent_first");
	try {
		LogicalPlanVerifier::Verify(*connection.context, *mixed_failure);
		FAIL("Expected enabled verification to reject invalid bindings");
	} catch (InternalException &exception) {
		REQUIRE(StringUtil::Contains(exception.what(), "legacy_child_first"));
		REQUIRE_FALSE(StringUtil::Contains(exception.what(), "canonical_parent_first"));
	}
#endif
}

static unique_ptr<VerificationExtensionOperator> InvalidPassThrough(TableIndex child_index, TableIndex invalid_index,
                                                                    const string &alias) {
	return ExpressionPassThrough(child_index, ColumnBinding(invalid_index, ProjectionIndex(0)), LogicalType::INTEGER,
	                             alias);
}

TEST_CASE("Logical plan verification does not rewrite expressions", "[logical_plan_verification]") {
	auto child_index = TableIndex(938);
	auto binding = ColumnBinding(child_index, ProjectionIndex(0));
	auto plan = ReferenceProjection(TableIndex(939), binding, LogicalType::INTEGER,
	                                TypedLeaf(child_index, LogicalType::INTEGER));
	REQUIRE(plan->expressions[0]->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF);

	auto result = LogicalPlanVerifier::VerifyAlways(*plan);
	REQUIRE(result.IsValid());
	REQUIRE(result.IsSuccess());
	REQUIRE(plan->expressions[0]->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF);
	REQUIRE(plan->expressions[0]->Cast<BoundColumnRefExpression>().Binding() == binding);
}

TEST_CASE("Opted-in extensions use standard column binding resolution", "[logical_plan_verification]") {
	auto child_index = TableIndex(937);
	auto binding = ColumnBinding(child_index, ProjectionIndex(0));
	auto plan = ExpressionPassThrough(child_index, binding, LogicalType::INTEGER, "standard_resolution");
	plan->ResolveOperatorTypes();
	REQUIRE(plan->expressions[0]->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF);

	ColumnBindingResolver resolver;
	REQUIRE_NOTHROW(resolver.VisitOperator(*plan));
	REQUIRE(plan->expressions[0]->GetExpressionClass() == ExpressionClass::BOUND_REF);
	REQUIRE(plan->expressions[0]->GetReturnType() == LogicalType::INTEGER);
}

TEST_CASE("Normal column binding resolution preserves matching incomplete types", "[logical_plan_verification]") {
	auto ResolveMatchingType = [](LogicalType type) {
		auto child_index = TableIndex(939);
		auto child = make_uniq<IncompleteTypeLeaf>(child_index, type);
		auto plan = ReferenceProjection(TableIndex(940), ColumnBinding(child_index, ProjectionIndex(0)), type,
		                                std::move(child));
		plan->ResolveOperatorTypes();
		REQUIRE(plan->expressions[0]->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF);

		ColumnBindingResolver resolver;
		REQUIRE_NOTHROW(resolver.VisitOperator(*plan));
		REQUIRE(plan->expressions[0]->GetExpressionClass() == ExpressionClass::BOUND_REF);
		REQUIRE(plan->expressions[0]->GetReturnType() == type);
	};

	SECTION("ANY") {
		ResolveMatchingType(LogicalType::ANY);
	}
	SECTION("TEMPLATE") {
		ResolveMatchingType(LogicalType::TEMPLATE("T"));
	}
	SECTION("nested incomplete type") {
		ResolveMatchingType(LogicalType::LIST(LogicalType::ANY));
	}
}

TEST_CASE("Logical plan verification structures incomplete expression types", "[logical_plan_verification]") {
	auto VerifyIncompleteType = [](LogicalType expression_type, const string &alias) {
		auto child_index = TableIndex(940);
		auto plan = ExpressionPassThrough(child_index, ColumnBinding(child_index, ProjectionIndex(0)),
		                                  std::move(expression_type), alias);
		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		auto &issue = result.GetIssues()[0];
		REQUIRE(issue.code == LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
		REQUIRE(issue.path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}});
		REQUIRE(GetFact(issue, "invariant") == Value("incomplete_binding_type"));
		REQUIRE(GetFact(issue, "expected_type_complete") == Value::BOOLEAN(true));
		REQUIRE(GetFact(issue, "actual_type_complete") == Value::BOOLEAN(false));
	};

	SECTION("ANY") {
		VerifyIncompleteType(LogicalType::ANY, "incomplete_any");
	}
	SECTION("TEMPLATE") {
		VerifyIncompleteType(LogicalType::TEMPLATE("T"), "incomplete_template");
	}
	SECTION("nested incomplete type") {
		VerifyIncompleteType(LogicalType::LIST(LogicalType::ANY), "incomplete_nested");
	}
	SECTION("incomplete expected type") {
		auto child_index = TableIndex(941);
		auto child = make_uniq<IncompleteTypeLeaf>(child_index, LogicalType::ANY);
		auto plan = ReferenceProjection(TableIndex(942), ColumnBinding(child_index, ProjectionIndex(0)),
		                                LogicalType::INTEGER, std::move(child));
		auto result = LogicalPlanVerifier::VerifyAlways(*plan);
		REQUIRE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 1);
		auto &issue = result.GetIssues()[0];
		REQUIRE(issue.code == LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT);
		REQUIRE(issue.path ==
		        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
		                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}});
		REQUIRE(GetFact(issue, "invariant") == Value("incomplete_binding_type"));
		REQUIRE(GetFact(issue, "expected_type_complete") == Value::BOOLEAN(false));
		REQUIRE(GetFact(issue, "actual_type_complete") == Value::BOOLEAN(true));
	}
}

static unique_ptr<LogicalOperator> MultipleIssuePlan(const string &left_alias, const string &right_alias) {
	return make_uniq<LogicalCrossProduct>(InvalidPassThrough(TableIndex(100), TableIndex(1000), left_alias),
	                                      InvalidPassThrough(TableIndex(101), TableIndex(1001), right_alias));
}

TEST_CASE("Logical plan verification results have deterministic structural identity", "[logical_plan_verification]") {
	auto first_plan = MultipleIssuePlan("first_left", "first_right");
	auto second_plan = MultipleIssuePlan("second_left", "second_right");
	auto first = LogicalPlanVerifier::VerifyAlways(*first_plan);
	auto second = LogicalPlanVerifier::VerifyAlways(*second_plan);

	REQUIRE(first.IsValid());
	REQUIRE(second.IsValid());
	REQUIRE(first.GetIssues().size() == 2);
	REQUIRE(first.GetIssues() == second.GetIssues());
	REQUIRE(first.GetIssues()[0].path ==
	        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
	                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0},
	                                      {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}});
	REQUIRE(first.GetIssues()[1].path ==
	        LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
	                                     {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 1},
	                                      {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0}}});
	REQUIRE(first.GetIssues()[0].message != second.GetIssues()[0].message);

	auto message_only_change = first.GetIssues()[0];
	message_only_change.message = "Different explanatory text";
	REQUIRE(message_only_change == first.GetIssues()[0]);
}

TEST_CASE("Logical plan verification result records enforce their structural contract", "[logical_plan_verification]") {
	LogicalPlanVerificationPath full_plan_path {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
	                                            {{LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0},
	                                             {LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 1},
	                                             {LogicalPlanVerificationPathComponentType::EXPRESSION_CHILD, 2}}};
	REQUIRE(full_plan_path.IsValid());
	REQUIRE(LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION,
	                                     {{LogicalPlanVerificationPathComponentType::EXPRESSION_CHILD, 0}}}
	            .IsValid());
	REQUIRE_FALSE(LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
	                                           {{LogicalPlanVerificationPathComponentType::OPERATOR_EXPRESSION, 0},
	                                            {LogicalPlanVerificationPathComponentType::OPERATOR_CHILD, 0}}}
	                  .IsValid());

	LogicalPlanVerificationIssue malformed_extension;
	malformed_extension.code = LogicalPlanVerificationIssueCode::MALFORMED_EXTENSION_RESULT;
	malformed_extension.construct = LogicalPlanVerificationConstructIdentity::Extension("synthetic_extension");
	REQUIRE(malformed_extension.IsValid());

	LogicalPlanVerificationIssue unsupported_extension;
	unsupported_extension.code = LogicalPlanVerificationIssueCode::UNSUPPORTED_EXTENSION;
	unsupported_extension.construct = LogicalPlanVerificationConstructIdentity::Extension("synthetic_extension");
	REQUIRE_FALSE(unsupported_extension.IsValid());
	unsupported_extension.path = full_plan_path;
	REQUIRE(unsupported_extension.IsValid());

	LogicalPlanVerificationIssue invalid_type_mismatch;
	invalid_type_mismatch.code = LogicalPlanVerificationIssueCode::TYPE_MISMATCH;
	invalid_type_mismatch.path = full_plan_path;
	invalid_type_mismatch.construct =
	    LogicalPlanVerificationConstructIdentity::Expression(ExpressionClass::BOUND_COLUMN_REF);
	REQUIRE_FALSE(invalid_type_mismatch.IsValid());
	invalid_type_mismatch.construct =
	    LogicalPlanVerificationConstructIdentity::BindingTypeMismatch(LogicalType::INTEGER, LogicalType::VARCHAR);
	REQUIRE(invalid_type_mismatch.IsValid());
	invalid_type_mismatch.code = static_cast<LogicalPlanVerificationIssueCode>(255);
	REQUIRE_FALSE(invalid_type_mismatch.IsValid());
	invalid_type_mismatch.code = LogicalPlanVerificationIssueCode::TYPE_MISMATCH;
	invalid_type_mismatch.phase = static_cast<LogicalPlanVerificationPhase>(255);
	REQUIRE_FALSE(invalid_type_mismatch.IsValid());
	invalid_type_mismatch.phase = LogicalPlanVerificationPhase::VERIFY;

	auto incomplete_mismatch =
	    LogicalPlanVerificationConstructIdentity::BindingTypeMismatch(LogicalType::ANY, LogicalType::INTEGER);
	REQUIRE_FALSE(incomplete_mismatch.IsValid());
	auto incomplete_actual_mismatch =
	    LogicalPlanVerificationConstructIdentity::BindingTypeMismatch(LogicalType::INTEGER, LogicalType::ANY);
	REQUIRE_FALSE(incomplete_actual_mismatch.IsValid());
	REQUIRE_FALSE(LogicalPlanVerificationConstructIdentity::LogicalTypeValue(LogicalType::ANY).IsValid());
	REQUIRE_FALSE(LogicalPlanVerificationConstructIdentity::LogicalTypeValue(LogicalType::TEMPLATE("T")).IsValid());
	auto incomplete_nested_type =
	    LogicalPlanVerificationConstructIdentity::LogicalTypeValue(LogicalType::LIST(LogicalType::ANY));
	REQUIRE_FALSE(incomplete_nested_type.IsValid());
	LogicalPlanVerificationFunctionIdentity incomplete_argument_function;
	incomplete_argument_function.name = "incomplete_argument";
	incomplete_argument_function.arguments = {LogicalType::TEMPLATE("T")};
	incomplete_argument_function.return_type = LogicalType::INTEGER;
	REQUIRE_FALSE(incomplete_argument_function.IsValid());
	LogicalPlanVerificationFunctionIdentity incomplete_return_function;
	incomplete_return_function.name = "incomplete_return";
	incomplete_return_function.arguments = {LogicalType::INTEGER};
	incomplete_return_function.return_type = LogicalType::ANY;
	REQUIRE_FALSE(incomplete_return_function.IsValid());

	REQUIRE_FALSE(LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Failure({}).IsValid());
	REQUIRE(LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Failure({invalid_type_mismatch}).IsValid());

	LogicalPlanVerificationIssue filter_issue;
	filter_issue.path = full_plan_path;
	filter_issue.construct =
	    LogicalPlanVerificationConstructIdentity::LogicalOperator(LogicalOperatorType::LOGICAL_FILTER);
	filter_issue.facts = {{"z_fact", Value::INTEGER(2)}, {"a_fact", Value::INTEGER(1)}};
	filter_issue.message = "filter message";
	LogicalPlanVerificationIssue projection_issue;
	projection_issue.path = full_plan_path;
	projection_issue.construct =
	    LogicalPlanVerificationConstructIdentity::LogicalOperator(LogicalOperatorType::LOGICAL_PROJECTION);
	projection_issue.facts = {{"a_fact", Value::INTEGER(2)}};
	projection_issue.message = "projection message";
	auto other_filter_issue = filter_issue;
	other_filter_issue.facts = {{"a_fact", Value::INTEGER(1)}, {"z_fact", Value::INTEGER(3)}};
	other_filter_issue.message = "other filter message";
	auto duplicate_filter_issue = filter_issue;
	duplicate_filter_issue.message = "different filter message";
	auto null_fact_issue = projection_issue;
	null_fact_issue.facts = {{"nullable_fact", Value(LogicalType::INTEGER)}};
	null_fact_issue.message = "null fact message";
	auto duplicate_null_fact_issue = null_fact_issue;
	duplicate_null_fact_issue.message = "different null fact message";
	auto pointer_fact_issue = projection_issue;
	pointer_fact_issue.facts = {{"pointer_fact", Value::POINTER(42)}};
	REQUIRE_FALSE(
	    LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Failure({pointer_fact_issue}).IsValid());
	auto stable_scalar_fact_issue = projection_issue;
	stable_scalar_fact_issue.facts = {{"bit_fact", Value::BIT("101")},
	                                  {"date_fact", Value::DATE(2026, 8, 28)},
	                                  {"decimal_fact", Value::DECIMAL(int64_t(12345), 7, 2)},
	                                  {"integer_fact", Value::INTEGER(42)},
	                                  {"null_fact", Value(LogicalType::VARCHAR)},
	                                  {"sqlnull_fact", Value(LogicalType::SQLNULL)},
	                                  {"string_fact", Value("stable")}};
	REQUIRE(
	    LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Failure({stable_scalar_fact_issue}).IsValid());

	auto RequireInvalidFact = [&](const string &name, Value value) {
		auto invalid_fact_issue = projection_issue;
		invalid_fact_issue.facts = {{name, std::move(value)}};
		auto result = LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Failure(
		    {projection_issue, invalid_fact_issue});
		REQUIRE_FALSE(result.IsValid());
		REQUIRE(result.GetIssues().size() == 2);
	};
	RequireInvalidFact("nested_fact", Value::LIST(LogicalType::INTEGER, {Value::INTEGER(1)}));
	RequireInvalidFact("any_fact", Value(LogicalType::ANY));
	RequireInvalidFact("template_fact", Value(LogicalType::TEMPLATE("T")));
	RequireInvalidFact("type_fact", Value::TYPE(LogicalType::INTEGER));
	RequireInvalidFact("validity_fact", Value(LogicalType(LogicalTypeId::VALIDITY)));
	RequireInvalidFact("lambda_fact", Value(LogicalType::LAMBDA));

	auto invalid_enum_issue = projection_issue;
	invalid_enum_issue.code = static_cast<LogicalPlanVerificationIssueCode>(255);
	auto invalid_enum_result =
	    LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Failure({projection_issue, invalid_enum_issue});
	REQUIRE_FALSE(invalid_enum_result.IsValid());
	REQUIRE(invalid_enum_result.GetIssues().size() == 2);

	auto invalid_path_issue = projection_issue;
	invalid_path_issue.path =
	    LogicalPlanVerificationPath {LogicalPlanVerificationPathRoot::LOGICAL_PLAN,
	                                 {{static_cast<LogicalPlanVerificationPathComponentType>(255), 0}}};
	auto invalid_path_result =
	    LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Failure({projection_issue, invalid_path_issue});
	REQUIRE_FALSE(invalid_path_result.IsValid());
	REQUIRE(invalid_path_result.GetIssues().size() == 2);

	auto invalid_construct_issue = projection_issue;
	invalid_construct_issue.construct = LogicalPlanVerificationConstructIdentity::Extension("");
	auto invalid_construct_result = LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Failure(
	    {projection_issue, invalid_construct_issue});
	REQUIRE_FALSE(invalid_construct_result.IsValid());
	REQUIRE(invalid_construct_result.GetIssues().size() == 2);

	auto invalid_fact_result =
	    LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Failure({projection_issue, pointer_fact_issue});
	REQUIRE_FALSE(invalid_fact_result.IsValid());
	REQUIRE(invalid_fact_result.GetIssues().size() == 2);

	auto forward = LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Failure(
	    {filter_issue, null_fact_issue, projection_issue, other_filter_issue, duplicate_filter_issue,
	     duplicate_null_fact_issue});
	auto reverse = LogicalPlanVerificationResult<LogicalPlanVerificationSuccess>::Failure(
	    {duplicate_null_fact_issue, other_filter_issue, projection_issue, filter_issue});
	REQUIRE(forward.IsValid());
	REQUIRE(reverse.IsValid());
	REQUIRE(forward.GetIssues() == reverse.GetIssues());
	REQUIRE(forward.GetIssues().size() == 4);
	for (auto &issue : forward.GetIssues()) {
		REQUIRE(std::is_sorted(issue.facts.begin(), issue.facts.end(),
		                       [](const pair<string, Value> &left, const pair<string, Value> &right) {
			                       return left.first < right.first;
		                       }));
	}
}
