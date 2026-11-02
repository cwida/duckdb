#include "catch.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/planner/bound_parameter_map.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "test_helpers.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension/linked_extension_registry.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include <cmath>
#include <cstring>
#include "bound_expression_sql_export_test_helpers.hpp"

using namespace duckdb;

namespace bound_expression_sql_export_test {

static void RequireFunctionIssue(const ExportResult &result, LogicalPlanVerificationIssueCode code,
                                 const LogicalPlanVerificationPath &path, const Identifier &catalog,
                                 const Identifier &schema, const Identifier &name, const vector<LogicalType> &arguments,
                                 const LogicalType &return_type) {
	RequireIssue(result, code, path);
	auto &issue = result.GetIssues()[0];
	REQUIRE(issue.construct);
	REQUIRE(issue.construct->type == LogicalPlanVerificationConstructType::FUNCTION);
	REQUIRE(issue.construct->function);
	auto &identity = *issue.construct->function;
	REQUIRE(identity.catalog == catalog.GetIdentifierName());
	REQUIRE(identity.schema == schema.GetIdentifierName());
	REQUIRE(identity.name == name.GetIdentifierName());
	REQUIRE(identity.arguments == arguments);
	REQUIRE(identity.return_type == return_type);
}

struct SyntheticSQLSource {
	BoundExpressionSQLExportContext context;
	string from_clause;
};

static SyntheticSQLSource CreateSyntheticSQLSource(const Expression &expression) {
	vector<SQLBindingEntry> entries;
	CollectSQLBindings(expression, entries);
	REQUIRE_FALSE(entries.empty());

	SyntheticSQLSource result;
	result.context.resolve_binding = [entries](const ColumnBinding &binding) -> optional<ResolvedSQLColumnReference> {
		for (auto &entry : entries) {
			if (entry.binding == binding) {
				return ResolvedSQLColumnReference {{Identifier("v"), entry.name}, entry.type};
			}
		}
		return {};
	};

	string first_row;
	string second_row;
	string column_names;
	for (idx_t entry_idx = 0; entry_idx < entries.size(); entry_idx++) {
		auto &entry = entries[entry_idx];
		if (entry_idx > 0) {
			first_row += ", ";
			second_row += ", ";
			column_names += ", ";
		}
		if (entry.type.id() == LogicalTypeId::BOOLEAN) {
			first_row += "TRUE";
			second_row += "TRUE";
		} else if (entry.type.id() == LogicalTypeId::VARCHAR) {
			first_row += "CAST('a' AS VARCHAR)";
			second_row += "CAST('b' AS VARCHAR)";
		} else {
			REQUIRE(entry.type.IsNumeric());
			first_row += "CAST(1 AS " + entry.type.ToString() + ")";
			second_row += "CAST(2 AS " + entry.type.ToString() + ")";
		}
		column_names += entry.name.GetIdentifierName();
	}
	result.from_clause = " FROM (VALUES (" + first_row + "), (" + second_row + ")) AS v(" + column_names + ")";
	return result;
}

class OpaqueSQLFunctionData : public FunctionData {
public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<OpaqueSQLFunctionData>();
	}

	bool Equals(const FunctionData &) const override {
		return true;
	}
};

static unique_ptr<FunctionData> BindOpaqueSQLFunction(BindScalarFunctionInput &) {
	return make_uniq<OpaqueSQLFunctionData>();
}

static unique_ptr<FunctionData> BindOpaqueSQLAggregate(BindAggregateFunctionInput &) {
	return make_uniq<OpaqueSQLFunctionData>();
}

static unique_ptr<FunctionData> BindDropLastScalarArgument(BindScalarFunctionInput &input) {
	input.GetBoundFunction().GetArguments().pop_back();
	input.GetArguments().pop_back();
	return make_uniq<OpaqueSQLFunctionData>();
}

static unique_ptr<FunctionData> BindDropLastAggregateArgument(BindAggregateFunctionInput &input) {
	input.GetBoundFunction().GetArguments().pop_back();
	return make_uniq<OpaqueSQLFunctionData>();
}

static unique_ptr<ParsedExpression> PositionalSQLUnbind(FunctionUnbindInput &input) {
	return make_uniq<FunctionExpression>(input.expression.Function().GetQualifiedName(), std::move(input.children));
}

static unique_ptr<ParsedExpression> DeclineSQLUnbind(FunctionUnbindInput &) {
	return nullptr;
}

struct SubtractOperation {
	template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(LEFT_TYPE left, RIGHT_TYPE right) {
		return left - right;
	}
};

template <int64_t FINAL_OFFSET>
struct SyntheticSumOperation {
	static bool IgnoreNull() {
		return true;
	}

	static void Initialize(int64_t &state) {
		state = 0;
	}

	template <class INPUT_TYPE, class STATE_TYPE, class OP>
	static void Operation(STATE_TYPE &state, const INPUT_TYPE &input, AggregateUnaryInput &) {
		state += input;
	}

	template <class INPUT_TYPE, class STATE_TYPE, class OP>
	static void ConstantOperation(STATE_TYPE &state, const INPUT_TYPE &input, AggregateUnaryInput &, idx_t count) {
		state += input * static_cast<INPUT_TYPE>(count);
	}

	template <class STATE_TYPE, class OP>
	static void Combine(const STATE_TYPE &source, STATE_TYPE &target, AggregateInputData &) {
		target += source;
	}

	template <class RESULT_TYPE, class STATE_TYPE>
	static void Finalize(STATE_TYPE &state, RESULT_TYPE &target, AggregateFinalizeData &) {
		target = static_cast<RESULT_TYPE>(state + FINAL_OFFSET);
	}
};

static AggregateFunction SyntheticSum(const Identifier &name, int64_t offset) {
	auto result = offset == 0 ? AggregateFunction::UnaryAggregate<int64_t, int32_t, int64_t, SyntheticSumOperation<0>>(
	                                LogicalType::INTEGER, LogicalType::BIGINT)
	                          : AggregateFunction::UnaryAggregate<int64_t, int32_t, int64_t, SyntheticSumOperation<1>>(
	                                LogicalType::INTEGER, LogicalType::BIGINT);
	result.SetName(name);
	return result;
}

static Value EvaluateAggregate(const BoundAggregateExpression &expression,
                               const Value &input_value = Value::INTEGER(7)) {
	auto &function = expression.Function();
	AggregateStateInput state_input(function, expression.BindInfo().get());
	auto state = make_unsafe_uniq_array_uninitialized<data_t>(function.GetStateSizeCallback()(state_input));
	data_ptr_t state_ptr = state.get();
	function.GetStateInitCallback()(state_input, &state_ptr, 1);
	Vector input(input_value, count_t(1));
	Vector state_vector(Value::POINTER(CastPointerToValue(state.get())), count_t(1));
	ArenaAllocator allocator(Allocator::DefaultAllocator());
	AggregateInputData update_input(function, expression.BindInfo().get(), allocator);
	function.GetStateUpdateCallback()(&input, update_input, 1, state_vector, 1);
	Vector result(expression.GetReturnType());
	AggregateFinalizeInputData finalize_input(function, expression.BindInfo().get(), allocator);
	function.GetStateFinalizeCallback()(state_vector, finalize_input, result, 1, 0);
	return result.GetValue(0);
}

TEST_CASE("Incremental scalar registration preserves live SQL identity",
          "[sql_export][bound_expression_sql_export][extension]") {
	DuckDB source_db;
	DuckDB target_db;
	Connection source(source_db);
	Connection target(target_db);
	const Identifier name("incremental_sql_identity");
	auto make_function = [&](const LogicalType &type) {
		auto function = ScalarFunction(name, {type}, type, ScalarFunction::NopFunction);
		function.SetCatalogName(Identifier("unowned_catalog"));
		function.SetSchemaName(Identifier("unowned_schema"));
		return function;
	};
	auto register_functions = [&](DuckDB &db) {
		ExtensionLoader loader(*db.instance, "incremental_sql_identity_extension");
		loader.RegisterFunction(make_function(LogicalType::INTEGER));
		auto initial = loader.GetFunction(name).functions.GetFunctionByOffset(0);
		loader.AddFunctionOverload(make_function(LogicalType::BIGINT));
		ScalarFunctionSet additions(name);
		additions.AddFunction(make_function(LogicalType::SMALLINT));
		auto renamed = make_function(LogicalType::VARCHAR);
		renamed.SetName(Identifier("noncanonical_name"));
		additions.AddFunction(std::move(renamed));
		loader.AddFunctionOverload(std::move(additions));
		auto &functions = loader.GetFunction(name).functions;
		REQUIRE(functions.Size() == 4);
		REQUIRE(functions.GetFunctionByOffset(0) == initial);
		idx_t index = 0;
		for (auto &type : {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::SMALLINT, LogicalType::VARCHAR}) {
			auto &definition = functions.GetFunctionByOffset(index++);
			REQUIRE(definition->GetSignature().GetParameter(0).GetType() == type);
			REQUIRE(definition->GetReturnType() == type);
			REQUIRE(definition->GetName() == name);
			REQUIRE(definition->GetCatalogName() == Identifier::SystemCatalog());
			REQUIRE(definition->GetSchemaName() == Identifier::DefaultSchema());
		}
	};
	register_functions(source_db);
	register_functions(target_db);
	source.BeginTransaction();
	BoundExpressionSQLExportContext context;
	for (auto &value : {Value::INTEGER(7), Value::BIGINT(8), Value::SMALLINT(9), Value("ten")}) {
		auto sql =
		    "SELECT " + name.GetIdentifierName() + "(" + value.ToSQLString() + "::" + value.type().ToString() + ")";
		auto plan = BindExportQuery(source, sql);
		auto expression = FindExpression(*plan, [&](const Expression &candidate) {
			return candidate.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
			       candidate.Cast<BoundFunctionExpression>().Function().GetName() == name;
		});
		REQUIRE(expression);
		auto &function = expression->Cast<BoundFunctionExpression>().Function();
		REQUIRE(function.GetLogicalArguments() == vector<LogicalType> {value.type()});
		REQUIRE(function.GetLogicalReturnType() == value.type());
		REQUIRE(ExpressionExecutor::EvaluateScalar(*source.context, *expression) == value);
		auto exported = BoundExpressionSQLExporter::Export(*expression, context);
		REQUIRE(exported.IsValid());
		REQUIRE(exported.IsSuccess());
		REQUIRE(exported.GetValue());
		REQUIRE(exported.GetValue()->Cast<FunctionExpression>().GetQualifiedName() ==
		        QualifiedName(Identifier::SystemCatalog(), Identifier::DefaultSchema(), name));
		plan.reset();
		auto rebound = target.Query("SELECT " + exported.GetValue()->ToString());
		REQUIRE_NO_FAIL(*rebound);
		REQUIRE(rebound->GetTypes() == vector<LogicalType> {value.type()});
		REQUIRE(rebound->RowCount() == 1);
		REQUIRE(rebound->Collection().GetValue(0, 0) == value);
	}
	auto standalone = make_function(LogicalType::INTEGER);
	standalone.SetCatalogName(Identifier::SystemCatalog());
	standalone.SetSchemaName(Identifier::DefaultSchema());
	vector<unique_ptr<Expression>> children;
	children.push_back(Constant(Value::INTEGER(-7)));
	auto bound = standalone.Bind(*source.context, std::move(children));
	REQUIRE(ExpressionExecutor::EvaluateScalar(*source.context, *bound) == Value::INTEGER(-7));
	RequireRoundTrip(target, *bound, context, string(), "incremental_sql_identity(-7::INTEGER)");
	source.Rollback();
}

TEST_CASE("Bound expression SQL export supports represented catalog bind state",
          "[sql_export][bound_expression_sql_export]") {
	DuckDB db;
	Connection connection(db);
	ExtensionLoader loader(*db.instance, "synthetic_sql_export_bind_extension");
	const Identifier schema_name("synthetic_sql_export_bind_schema");
	loader.UseDedicatedSchemaForExtension(schema_name);
	loader.RegisterFunction(ScalarFunction(Identifier("ordinary_bound_scalar"),
	                                       {LogicalType::INTEGER, LogicalType::INTEGER}, LogicalType::INTEGER,
	                                       ScalarFunction::NopFunction, BindOpaqueSQLFunction));
	loader.RegisterFunction(ScalarFunction(Identifier("lost_scalar_argument"),
	                                       {LogicalType::INTEGER, LogicalType::INTEGER}, LogicalType::INTEGER,
	                                       ScalarFunction::NopFunction, BindDropLastScalarArgument));
	auto ordinary_sum = SyntheticSum(Identifier("ordinary_bound_sum"), 0);
	ordinary_sum.SetBindCallback(BindOpaqueSQLAggregate);
	loader.RegisterFunction(std::move(ordinary_sum));
	auto lost_sum = SyntheticSum(Identifier("lost_sum_argument"), 0);
	lost_sum.GetSignature().AddParameter(LogicalType::INTEGER);
	lost_sum.SetBindCallback(BindDropLastAggregateArgument);
	loader.RegisterFunction(std::move(lost_sum));
	loader.RefreshSearchPath(*connection.context);
	connection.BeginTransaction();

	auto bind_scalar = [&](const string &name) {
		auto plan = BindExportQuery(connection, "SELECT " + schema_name.GetIdentifierName() + "." + name +
		                                            "(CAST(7 AS INTEGER), CAST(99 AS INTEGER))");
		auto expression = FindExpression(*plan, [&](const Expression &candidate) {
			return candidate.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
			       candidate.Cast<BoundFunctionExpression>().Function().GetName() == name;
		});
		REQUIRE(expression);
		return expression->Copy();
	};
	auto bind_aggregate = [&](const string &name, const string &arguments = "CAST(7 AS INTEGER)") {
		auto plan = BindExportQuery(connection,
		                            "SELECT " + schema_name.GetIdentifierName() + "." + name + "(" + arguments + ")");
		auto expression = FindExpression(*plan, [&](const Expression &candidate) {
			return candidate.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE &&
			       candidate.Cast<BoundAggregateExpression>().Function().GetName() == name;
		});
		REQUIRE(expression);
		return expression->Copy();
	};

	BoundExpressionSQLExportContext context;
	LogicalPlanVerificationPath root_path;
	root_path.root = LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION;

	vector<pair<string, unique_ptr<Expression>>> represented;
	represented.emplace_back("ordinary_bound_scalar", bind_scalar("ordinary_bound_scalar"));
	represented.emplace_back("ordinary_bound_sum", bind_aggregate("ordinary_bound_sum"));
	for (auto &entry : represented) {
		CAPTURE(entry.first);
		auto &expression = *entry.second;
		const bool scalar = expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION;
		REQUIRE((scalar ? expression.Cast<BoundFunctionExpression>().BindInfo()
		                : expression.Cast<BoundAggregateExpression>().BindInfo()));
		auto result = BoundExpressionSQLExporter::Export(expression, context);
		REQUIRE(result.IsSuccess());
		REQUIRE(BoundExpressionSQLExporter::Export(*expression.Copy(), context).IsSuccess());
		REQUIRE(result.GetValue()->Cast<FunctionExpression>().GetQualifiedName() ==
		        QualifiedName(Identifier::SystemCatalog(), schema_name, Identifier(entry.first)));
		const string oracle = schema_name.GetIdentifierName() + "." + entry.first + (scalar ? "(7, 99)" : "(7)");
		auto serialized = BinaryRoundTrip(*connection.context, expression);
		RequireRoundTrip(connection, *serialized, context, string(), oracle);
		RequireRoundTrip(connection, expression, context, string(), oracle);
	}

	auto lost_scalar = bind_scalar("lost_scalar_argument");
	REQUIRE(lost_scalar->Cast<BoundFunctionExpression>().GetChildren().size() == 1);
	RequireIssue(BoundExpressionSQLExporter::Export(*lost_scalar, context),
	             LogicalPlanVerificationIssueCode::UNSUPPORTED_FUNCTION, root_path);
	auto lost_aggregate = bind_aggregate("lost_sum_argument", "CAST(7 AS INTEGER), CAST(99 AS INTEGER)");
	REQUIRE(lost_aggregate->Cast<BoundAggregateExpression>().GetChildren().size() == 1);
	RequireIssue(BoundExpressionSQLExporter::Export(*lost_aggregate, context),
	             LogicalPlanVerificationIssueCode::UNSUPPORTED_FUNCTION, root_path);
	// Diagnostics retain the logical signature even when a standalone binder removes children.
	auto standalone = SyntheticSum(Identifier("standalone_lost_argument"), 0);
	standalone.GetSignature().AddParameter(LogicalType::INTEGER);
	standalone.SetBindCallback(BindDropLastAggregateArgument);
	standalone.SetCatalogName(Identifier::SystemCatalog());
	standalone.SetSchemaName(schema_name);
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(Constant(Value::INTEGER(7)));
	arguments.push_back(Constant(Value::INTEGER(99)));
	auto standalone_bound = standalone.Bind(*connection.context, std::move(arguments));
	RequireFunctionIssue(BoundExpressionSQLExporter::Export(*standalone_bound, context),
	                     LogicalPlanVerificationIssueCode::UNSUPPORTED_FUNCTION, root_path, Identifier::SystemCatalog(),
	                     schema_name, standalone.GetName(), {LogicalType::INTEGER, LogicalType::INTEGER},
	                     LogicalType::BIGINT);
	connection.Rollback();
}

TEST_CASE("Standalone function binding does not autoload catalog collisions",
          "[sql_export][bound_expression_sql_export][logical_plan_verification][dont_link]") {
	for (auto &linked : LinkedExtensionRegistry::Get()) {
		if (linked.name == "core_functions") {
			SUCCEED("core_functions is linked into this binary, nothing to autoload");
			return;
		}
	}
	auto extension_directory = TestJoinPath(TestDirectoryPath(), "stage02_standalone_bind_extensions");
	TestDeleteDirectory(extension_directory);
	TestCreateDirectory(extension_directory);
	DBConfig config;
	config.SetOptionByName("extension_directory", extension_directory);
	config.SetOptionByName("autoload_known_extensions", true);
	config.SetOptionByName("autoinstall_known_extensions", false);
	DuckDB db(nullptr, &config);
	Connection connection(db);

	auto require_core_functions_absent = [&]() {
		auto extension_state = connection.Query(
		    "SELECT installed, loaded FROM duckdb_extensions() WHERE extension_name = 'core_functions'");
		REQUIRE_FALSE(extension_state->HasError());
		REQUIRE(extension_state->RowCount() == 1);
		REQUIRE(extension_state->Collection().GetValue(0, 0) == Value::BOOLEAN(false));
		REQUIRE(extension_state->Collection().GetValue(1, 0) == Value::BOOLEAN(false));
		auto count_if = connection.Query("SELECT count(*) FROM duckdb_functions() WHERE function_name = 'count_if'");
		REQUIRE_FALSE(count_if->HasError());
		REQUIRE(count_if->Collection().GetValue(0, 0) == Value::BIGINT(0));
	};
	require_core_functions_absent();
	connection.BeginTransaction();

	BoundExpressionSQLExportContext context;

	ScalarFunction standalone_scalar(Identifier("count_if"), {LogicalType::INTEGER}, LogicalType::INTEGER,
	                                 ScalarFunction::NopFunction);
	standalone_scalar.SetCatalogName(Identifier::SystemCatalog());
	standalone_scalar.SetSchemaName(Identifier::DefaultSchema());
	vector<unique_ptr<Expression>> scalar_children;
	scalar_children.push_back(Constant(Value::INTEGER(7)));
	auto scalar = standalone_scalar.Bind(*connection.context, std::move(scalar_children));
	REQUIRE(ExpressionExecutor::EvaluateScalar(*connection.context, *scalar) == Value::INTEGER(7));

	auto standalone_aggregate = SyntheticSum(Identifier("count_if"), 0);
	standalone_aggregate.SetCatalogName(Identifier::SystemCatalog());
	standalone_aggregate.SetSchemaName(Identifier::DefaultSchema());
	vector<unique_ptr<Expression>> aggregate_children;
	aggregate_children.push_back(Constant(Value::INTEGER(7)));
	auto aggregate = standalone_aggregate.Bind(*connection.context, std::move(aggregate_children));
	REQUIRE(aggregate->Function().GetName() == "count_if");
	REQUIRE(EvaluateAggregate(*aggregate) == Value::BIGINT(7));

	connection.Commit();
	require_core_functions_absent();
}

TEST_CASE("Bound expression SQL export canonicalizes native function definitions",
          "[sql_export][bound_expression_sql_export]") {
	DuckDB db;
	Connection connection(db);
	connection.BeginTransaction();
	BoundExpressionSQLExportContext context;

	auto &catalog = Catalog::GetSystemCatalog(*connection.context);
	auto &abs_entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(
	    *connection.context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), Identifier("abs")));
	auto abs_definition = abs_entry.functions.GetFunctionByArguments(*connection.context, {LogicalType::INTEGER});
	FunctionBinder function_binder(*connection.context);
	auto bind_scalar_definition = [&](shared_ptr<const ScalarFunction> definition) {
		vector<unique_ptr<Expression>> children;
		children.push_back(Constant(Value::INTEGER(-7)));
		return function_binder.BindScalarFunction(std::move(definition), std::move(children));
	};
	vector<unique_ptr<Expression>> copied_children;
	copied_children.push_back(Constant(Value::INTEGER(-7)));
	auto copied_scalar = abs_definition->Bind(*connection.context, std::move(copied_children));
	RequireRoundTrip(connection, *copied_scalar, context, string(), "abs(-7::INTEGER)");
	for (bool has_catalog : {false, true}) {
		for (bool has_schema : {false, true}) {
			if (has_catalog && has_schema) {
				continue;
			}
			auto definition = make_shared_ptr<ScalarFunction>(*abs_definition);
			definition->SetCatalogName(has_catalog ? Identifier::SystemCatalog() : Identifier());
			definition->SetSchemaName(has_schema ? Identifier::DefaultSchema() : Identifier());
			auto bound = bind_scalar_definition(std::move(definition));
			auto exported = BoundExpressionSQLExporter::Export(*bound, context);
			REQUIRE(exported.IsSuccess());
			auto &name = exported.GetValue()->Cast<FunctionExpression>().GetQualifiedName();
			REQUIRE(name.Name() == Identifier("abs"));
			REQUIRE(name.Catalog() == Identifier::SystemCatalog());
			REQUIRE(name.Schema() == Identifier::DefaultSchema());
			RequireRoundTrip(connection, *bound->Copy(), context, string(), "abs(-7::INTEGER)");
		}
	}

	ScalarFunctionSet scalar_set(Identifier("abs"));
	scalar_set.functions.push_back(abs_definition);
	scalar_set.ApplyToFunctions([](ScalarFunction &) {});
	REQUIRE(scalar_set.GetFunctionByOffset(0) != abs_definition);
	auto applied_scalar = bind_scalar_definition(scalar_set.GetFunctionByOffset(0));
	REQUIRE(applied_scalar->Cast<BoundFunctionExpression>().Function().GetDefinition() ==
	        scalar_set.GetFunctionByOffset(0));
	REQUIRE(ExpressionExecutor::EvaluateScalar(*connection.context, *applied_scalar) == Value::INTEGER(7));
	RequireRoundTrip(connection, *applied_scalar, context, string(), "abs(-7::INTEGER)");

	auto &sum_entry = catalog.GetEntry<AggregateFunctionCatalogEntry>(
	    *connection.context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), Identifier("sum")));
	auto sum_definition = sum_entry.functions.GetFunctionByArguments(*connection.context, {LogicalType::INTEGER});
	auto bind_aggregate_definition = [&](shared_ptr<const AggregateFunction> definition) {
		vector<unique_ptr<Expression>> children;
		children.push_back(Constant(Value::INTEGER(7)));
		return function_binder.BindAggregateFunction(std::move(definition), std::move(children));
	};
	for (bool has_catalog : {false, true}) {
		for (bool has_schema : {false, true}) {
			if (has_catalog && has_schema) {
				continue;
			}
			auto definition = make_shared_ptr<AggregateFunction>(*sum_definition);
			definition->SetCatalogName(has_catalog ? Identifier::SystemCatalog() : Identifier());
			definition->SetSchemaName(has_schema ? Identifier::DefaultSchema() : Identifier());
			auto bound = bind_aggregate_definition(std::move(definition));
			RequireRoundTrip(connection, *bound->Copy(), context, string(), "sum(7::INTEGER)");
		}
	}
	vector<unique_ptr<Expression>> copied_aggregate_children;
	copied_aggregate_children.push_back(Constant(Value::INTEGER(7)));
	auto copied_aggregate = sum_definition->Bind(*connection.context, std::move(copied_aggregate_children));
	REQUIRE(EvaluateAggregate(*copied_aggregate) == Value::HUGEINT(hugeint_t(7)));
	RequireRoundTrip(connection, *copied_aggregate, context, string(), "sum(7::INTEGER)");

	AggregateFunctionSet aggregate_set(Identifier("sum"));
	aggregate_set.functions.push_back(sum_definition);
	aggregate_set.ApplyToFunctions([](AggregateFunction &) {});
	REQUIRE(aggregate_set.GetFunctionByOffset(0) != sum_definition);
	auto applied_aggregate = bind_aggregate_definition(aggregate_set.GetFunctionByOffset(0));
	REQUIRE(applied_aggregate->Function().GetDefinition() == aggregate_set.GetFunctionByOffset(0));
	REQUIRE(EvaluateAggregate(*applied_aggregate) == Value::HUGEINT(hugeint_t(7)));
	RequireRoundTrip(connection, *applied_aggregate, context, string(), "sum(7::INTEGER)");
	connection.Rollback();
}

TEST_CASE("Bound expression SQL export preserves qualified operator function identity",
          "[sql_export][bound_expression_sql_export]") {
	DuckDB db;
	Connection connection(db);
	ExtensionLoader loader(*db.instance, "synthetic_operator_extension");
	loader.UseDedicatedSchemaForExtension(Identifier("synthetic_operator_schema"));
	loader.RegisterFunction(
	    ScalarFunction(Identifier("+"), {LogicalType::INTEGER, LogicalType::INTEGER}, LogicalType::INTEGER,
	                   ScalarFunction::BinaryFunction<int32_t, int32_t, int32_t, SubtractOperation>));
	loader.RefreshSearchPath(*connection.context);
	connection.BeginTransaction();

	auto plan =
	    BindExportQuery(connection, "SELECT synthetic_operator_schema.\"+\"(CAST(7 AS INTEGER), CAST(2 AS INTEGER))");
	auto expression = FindExpression(*plan, [](const Expression &candidate) {
		return candidate.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
		       candidate.Cast<BoundFunctionExpression>().Function().GetName() == "+";
	});
	REQUIRE(expression);
	auto operator_expression = expression->Copy();
	auto &operator_function = operator_expression->Cast<BoundFunctionExpression>();
	operator_function.IsOperatorMutable() = true;
	REQUIRE(ExpressionExecutor::EvaluateScalar(*connection.context, operator_function) == Value::INTEGER(5));

	BoundExpressionSQLExportContext context;
	auto result = BoundExpressionSQLExporter::Export(operator_function, context);
	REQUIRE(result.IsSuccess());
	auto &parsed = result.GetValue()->Cast<FunctionExpression>();
	auto &definition = operator_function.Function().GetDefinition();
	REQUIRE(definition);
	REQUIRE_FALSE(parsed.IsOperator());
	REQUIRE(parsed.GetQualifiedName().Catalog() == definition->GetCatalogName());
	REQUIRE(parsed.GetQualifiedName().Schema() == definition->GetSchemaName());
	REQUIRE(parsed.GetQualifiedName().Name() == definition->GetName());

	auto rebound = connection.Query("SELECT " + parsed.ToString());
	REQUIRE_FALSE(rebound->HasError());
	REQUIRE(rebound->GetTypes() == vector<LogicalType> {LogicalType::INTEGER});
	REQUIRE(rebound->Collection().GetValue(0, 0) == Value::INTEGER(5));
	connection.Rollback();
}

TEST_CASE("SQL export recovers calls from supported binary compatibility targets",
          "[sql_export][bound_expression_sql_export][serialization]") {
	DuckDB db;
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE compatibility_values(i INTEGER)"));
	REQUIRE_NO_FAIL(connection.Query("INSERT INTO compatibility_values VALUES (1), (2)"));
	connection.BeginTransaction();
	for (auto call : {"abs(i)", "log2(i)", "sum(i)", "quantile_cont(i, 0.5)"}) {
		INFO(call);
		auto plan = BindExportQuery(connection, string("SELECT ") + call + " FROM compatibility_values");
		auto expression = FindExpression(*plan, [](const Expression &candidate) {
			return candidate.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE ||
			       (candidate.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
			        candidate.GetExpressionType() == ExpressionType::BOUND_FUNCTION);
		});
		REQUIRE(expression);
		auto source = CreateSyntheticSQLSource(*expression);
		auto current =
		    BinaryRoundTrip(*connection.context, *expression, StorageCompatibility::FromIndex(StorageVersion::V2_0_0));
		LogicalPlanVerificationPath current_path;
		current_path.root = LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION;
		RequireRoundTrip(connection, *current, source.context, source.from_clause,
		                 StringUtil::Replace(call, "(i", "(v.exported_0"));
		auto legacy =
		    BinaryRoundTrip(*connection.context, *expression, StorageCompatibility::FromIndex(StorageVersion::V1_5_0));
		REQUIRE(legacy->GetReturnType() == expression->GetReturnType());
		LogicalPlanVerificationPath path;
		path.root = LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION;
		REQUIRE(BoundExpressionSQLExporter::Export(*legacy, source.context).IsSuccess());
	}
	connection.Rollback();
}

TEST_CASE("Bound expression SQL export owns named arguments after source destruction",
          "[sql_export][bound_expression_sql_export][struct_insert_sql_export]") {
	DuckDB db;
	Connection connection(db);
	connection.BeginTransaction();
	const string query = "SELECT struct_insert({'a': 1}, \"named field\" := 2)";
	auto plan = BindExportQuery(connection, query);
	auto expression = FindExpression(*plan, [](const Expression &candidate) {
		return candidate.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
		       candidate.Cast<BoundFunctionExpression>().Function().GetName() == "struct_insert";
	});
	REQUIRE(expression);
	auto expected = connection.Query(query);
	REQUIRE_NO_FAIL(*expected);
	BoundExpressionSQLExportContext context;
	context.client_context = connection.context.get();
	auto exported = BoundExpressionSQLExporter::Export(*expression, context);
	REQUIRE(exported.IsSuccess());
	auto retained_ast = exported.GetValue()->Copy();
	exported.GetValue().reset();
	plan.reset();
	expression = nullptr;
	auto retained = connection.Query("SELECT " + retained_ast->ToString());
	REQUIRE_NO_FAIL(*retained);
	REQUIRE(retained->GetTypes() == expected->GetTypes());
	REQUIRE(retained->Equals(*expected, false));
	connection.Rollback();
}

TEST_CASE("Bound expression SQL export uses scalar unbind callbacks",
          "[sql_export][bound_expression_sql_export][struct_insert_sql_export]") {
	DuckDB db;
	Connection connection(db);
	connection.BeginTransaction();
	auto &catalog = Catalog::GetSystemCatalog(*connection.context);
	auto bind_definition = [&](const Identifier &name, const vector<LogicalType> &types,
	                           scalar_function_unbind_t callback) {
		auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(
		    *connection.context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), name));
		auto definition =
		    make_shared_ptr<ScalarFunction>(*entry.functions.GetFunctionByArguments(*connection.context, types));
		definition->SetUnbindCallback(callback);
		vector<unique_ptr<Expression>> children;
		for (auto &type : types) {
			children.push_back(Constant(type == LogicalType::INTEGER ? Value::INTEGER(-7) : Value::DOUBLE(2)));
		}
		FunctionBinder binder(*connection.context);
		return binder.BindScalarFunction(std::move(definition), std::move(children));
	};

	auto positional = bind_definition(Identifier("abs"), {LogicalType::INTEGER}, PositionalSQLUnbind);
	auto positional_result = BoundExpressionSQLExporter::Export(*positional, {});
	REQUIRE(positional_result.IsSuccess());
	auto &positional_call = positional_result.GetValue()->Cast<FunctionExpression>();
	REQUIRE(positional_call.GetArguments().size() == 1);
	REQUIRE_FALSE(positional_call.GetArguments()[0].HasName());
	REQUIRE_NO_FAIL(connection.Query("SELECT " + positional_call.ToString()));

	auto missing = bind_definition(Identifier("abs"), {LogicalType::INTEGER}, DeclineSQLUnbind);
	auto missing_result = BoundExpressionSQLExporter::Export(*missing, {});
	REQUIRE(missing_result.HasError());
	REQUIRE(missing_result.GetIssues()[0].code == LogicalPlanVerificationIssueCode::UNSUPPORTED_FUNCTION);

	connection.Rollback();
}

TEST_CASE("SQL export retains bound alias names across copies and renamed inputs",
          "[sql_export][bound_expression_sql_export][alias_sql_export]") {
	DuckDB db;
	Connection connection(db);
	connection.BeginTransaction();
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE names(\"MiXeD' name\" INTEGER)"));
	REQUIRE_NO_FAIL(connection.Query("INSERT INTO names VALUES (1),(NULL),(1)"));
	auto plan = BindExportQuery(connection, "SELECT alias(\"MiXeD' name\") FROM names");
	auto expression = FindExpression(*plan, [](const Expression &candidate) {
		return candidate.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
		       candidate.Cast<BoundFunctionExpression>().Function().GetName() == "alias";
	});
	REQUIRE(expression);
	auto renamed = expression->Copy();
	renamed->Cast<BoundFunctionExpression>().GetChildren()[0]->SetAlias(Identifier("discarded display name"));
	for (bool binary : {false, true}) {
		auto copy = binary ? BinaryRoundTrip(*connection.context, *renamed) : renamed->Copy();
		auto &function = copy->Cast<BoundFunctionExpression>();
		auto binding = function.GetChildren()[0]->Cast<BoundColumnRefExpression>().Binding();
		auto context = ResolveBinding(binding, {Identifier("renamed")}, LogicalType::INTEGER);
		const string from = " FROM (SELECT \"MiXeD' name\" AS renamed FROM names)";
		RequireRoundTrip(connection, function, context, from, "'MiXeD'' name'");
		function.SetAlias(Identifier("explicit"));
		RequireRoundTrip(connection, function, context, from, "'explicit'");
	}
	connection.Rollback();
}

TEST_CASE("Function deserialization restores enclosing context after callback exceptions",
          "[sql_export][serialization]") {
	DuckDB db;
	Connection connection(db);
	ExtensionLoader loader(*db.instance, "sql_export_deserialization_context");
	ScalarFunction function("throwing_deserializer", {LogicalType::INTEGER}, LogicalType::INTEGER,
	                        ScalarFunction::NopFunction);
	function.SetSerializeCallback([](Serializer &, const optional_ptr<FunctionData>, const BoundScalarFunction &) {});
	function.SetDeserializeCallback([](Deserializer &deserializer, BoundScalarFunction &) -> unique_ptr<FunctionData> {
		REQUIRE(deserializer.Get<const LogicalType &>() == LogicalType::INTEGER);
		auto &children = deserializer.Get<const const_expression_list_t &>();
		REQUIRE(children.size() == 1);
		REQUIRE(children[0].get().GetReturnType() == LogicalType::INTEGER);
		throw InvalidInputException("Synthetic deserialization failure");
	});
	loader.RegisterFunction(std::move(function));
	connection.BeginTransaction();
	auto plan = BindExportQuery(connection, "SELECT throwing_deserializer(7::INTEGER)");
	auto expression = FindExpression(*plan, [](const Expression &candidate) {
		return candidate.GetExpressionType() == ExpressionType::BOUND_FUNCTION;
	});
	REQUIRE(expression);
	MemoryStream stream(Allocator::Get(*connection.context));
	BinarySerializer::Serialize(*expression, stream);
	stream.Rewind();
	BinaryDeserializer deserializer(stream);
	bound_parameter_map_t parameters;
	deserializer.Set<ClientContext &>(*connection.context);
	deserializer.Set<bound_parameter_map_t &>(parameters);
	const LogicalType enclosing_type = LogicalType::VARCHAR;
	auto enclosing_child = Constant(Value::BIGINT(42));
	const const_expression_list_t enclosing_children {*enclosing_child};
	deserializer.Set<const LogicalType &>(enclosing_type);
	deserializer.Set<const const_expression_list_t &>(enclosing_children);
	REQUIRE_THROWS_AS(deserializer.Deserialize<Expression>(), InvalidInputException);
	REQUIRE(&deserializer.Get<const LogicalType &>() == &enclosing_type);
	REQUIRE(&deserializer.Get<const const_expression_list_t &>() == &enclosing_children);
	connection.Rollback();
}

} // namespace bound_expression_sql_export_test
