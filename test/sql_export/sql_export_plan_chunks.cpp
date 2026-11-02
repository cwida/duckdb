#include "sql_export_test_helpers.hpp"
#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include <stdexcept>
#include <type_traits>
#include "logical_plan_sql_export_test_helpers.hpp"

using namespace duckdb;

namespace logical_plan_sql_export_test {

class SQLExportOpaqueProjection : public SQLExportExtensionOperator {
public:
	explicit SQLExportOpaqueProjection(vector<unique_ptr<Expression>> physical_expressions_p)
	    : SQLExportExtensionOperator(
	          "sql_export_opaque_projection",
	          {ColumnBinding(TableIndex(2001), ProjectionIndex(0)), ColumnBinding(TableIndex(2001), ProjectionIndex(1)),
	           ColumnBinding(TableIndex(2001), ProjectionIndex(2))},
	          {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT}, {TableIndex(2001)}),
	      physical_expressions(std::move(physical_expressions_p)) {
	}

	PhysicalOperator &CreatePlan(ClientContext &, PhysicalPlanGenerator &planner) override {
		auto &child = planner.CreatePlan(*children[0]);
		vector<unique_ptr<Expression>> projection;
		projection.push_back(make_uniq<BoundReferenceExpression>(LogicalType::BIGINT, 0));
		for (auto &expression : physical_expressions) {
			projection.push_back(expression->Copy());
		}
		auto &result = planner.Make<PhysicalProjection>(types, std::move(projection), estimated_cardinality);
		result.children.push_back(child);
		return result;
	}

private:
	vector<unique_ptr<Expression>> physical_expressions;
};

class SQLExportValuesCounter : public ClientContextState {
public:
	int64_t value = 0;
};

void SQLExportValuesNext(DataChunk &args, ExpressionState &state, Vector &result) {
	auto counter = state.GetContext().registered_state->Get<SQLExportValuesCounter>("sql_export_values_counter");
	auto writer = FlatVector::Writer<int64_t>(result, args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		writer.WriteValue(++counter->value);
	}
}

void SQLExportValuesPeek(DataChunk &args, ExpressionState &state, Vector &result) {
	auto counter = state.GetContext().registered_state->Get<SQLExportValuesCounter>("sql_export_values_counter");
	auto writer = FlatVector::Writer<int64_t>(result, args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		writer.WriteValue(counter->value);
	}
}

static unique_ptr<LogicalOperator> OrderValues(unique_ptr<LogicalOperator> child) {
	vector<BoundOrderByNode> orders;
	auto bindings = child->GetColumnBindings();
	for (idx_t i = 0; i < bindings.size(); i++) {
		orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST,
		                    make_uniq<BoundColumnRefExpression>(child->types[i], bindings[i]));
	}
	auto order = make_uniq<LogicalOrder>(std::move(orders));
	order->children.push_back(std::move(child));
	order->ResolveOperatorTypes();
	return std::move(order);
}

TEST_CASE("Logical plan SQL export copies owned chunk data and selected columns",
          "[sql_export][logical_plan_sql_export][chunk_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	for (idx_t rows : {0, 5, 4097}) {
		for (bool binary : {false, true}) {
			CAPTURE(rows, binary);
			auto result = connection.Query("SELECT i::SMALLINT, CASE WHEN i%3=0 THEN NULL ELSE [i,NULL] END xs, "
			                               "CASE WHEN i%2=0 THEN NULL ELSE 'a''b' END s FROM range(" +
			                               to_string(rows) + ") t(i)");
			REQUIRE_NO_FAIL(*result);
			connection.BeginTransaction();
			auto get = make_uniq<LogicalColumnDataGet>(TableIndex(1001), result->GetTypes(), result->TakeCollection());
			get->SetColumnIds({2, 0, 2, 1});
			unique_ptr<LogicalOperator> plan = std::move(get);
			if (binary) {
				plan = plan->Copy(*connection.context);
				plan->ResolveOperatorTypes();
			}
			auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
			REQUIRE(exported.IsSuccess());
			auto text = exported.GetValue().query->ToString();
			auto statement = make_uniq<SelectStatement>();
			statement->node = std::move(exported.GetValue().query);
			REQUIRE_NO_FAIL(connection.Query("PRAGMA disable_optimizer"));
			auto direct = connection.Query(make_uniq<LogicalPlanStatement>(std::move(plan)));
			REQUIRE_NO_FAIL(connection.Query("PRAGMA enable_optimizer"));
			auto generated = connection.Query(text);
			auto ast = connection.Query(std::move(statement));
			REQUIRE_NO_FAIL(*direct);
			REQUIRE_NO_FAIL(*generated);
			REQUIRE_NO_FAIL(*ast);
			REQUIRE(direct->RowCount() == rows);
			REQUIRE(generated->GetTypes() == direct->GetTypes());
			REQUIRE(ast->GetTypes() == direct->GetTypes());
			REQUIRE(SQLExportRows(*generated, true) == SQLExportRows(*direct, true));
			REQUIRE(SQLExportRows(*ast, true) == SQLExportRows(*direct, true));
			connection.Rollback();
		}
	}
	auto result = connection.Query("SELECT 42::BIGINT");
	REQUIRE_NO_FAIL(*result);
	connection.BeginTransaction();
	auto borrowed = make_uniq<LogicalColumnDataGet>(TableIndex(1002), result->GetTypes(), result->Collection());
	auto exported = LogicalPlanSQLExporter::Export(*connection.context, *borrowed);
	RequirePlanExportIssue(exported, LogicalPlanVerificationIssueCode::UNSUPPORTED_SOURCE);
	REQUIRE(exported.GetIssues()[0].construct->function->name == "logical_source");
	REQUIRE(exported.GetIssues()[0].facts.size() == 1);
	REQUIRE((exported.GetIssues()[0].facts[0] == pair<string, Value> {"guard", Value("borrowed_chunk_collection")}));
	auto native = connection.Query(make_uniq<LogicalPlanStatement>(std::move(borrowed)));
	REQUIRE_NO_FAIL(*native);
	REQUIRE(native->Collection().GetValue(0, 0) == Value::BIGINT(42));
	connection.Rollback();
}

TEST_CASE("Owned chunk export preserves observable execution groups",
          "[sql_export][logical_plan_sql_export][chunk_sql_export]") {
	for (idx_t count : vector<idx_t> {1, STANDARD_VECTOR_SIZE}) {
		for (bool combined : {false, true}) {
			if (count == STANDARD_VECTOR_SIZE && !combined) {
				continue;
			}
			CAPTURE(count, combined);
			DuckDB db(nullptr);
			Connection connection(db);
			REQUIRE_NO_FAIL(connection.Query("SET threads=1; CREATE TABLE chunk_export_input(x BIGINT)"));
			auto counter = make_shared_ptr<SQLExportValuesCounter>();
			connection.context->registered_state->Insert("sql_export_values_counter", counter);
			ExtensionLoader loader(*db.instance, "chunk_export_counter");
			ScalarFunction next("sql_export_values_next", {}, LogicalType::BIGINT, SQLExportValuesNext);
			next.SetVolatile();
			loader.RegisterFunction(std::move(next));
			ScalarFunction peek("sql_export_values_peek", {}, LogicalType::BIGINT, SQLExportValuesPeek);
			peek.SetVolatile();
			loader.RegisterFunction(std::move(peek));
			auto first = connection.Query("SELECT * FROM range(" + to_string(combined ? count : count * 2) + ")");
			REQUIRE_NO_FAIL(*first);
			auto collection = first->TakeCollection();
			if (combined) {
				auto second =
				    connection.Query("SELECT * FROM range(" + to_string(count) + "," + to_string(count * 2) + ")");
				REQUIRE_NO_FAIL(*second);
				auto other = second->TakeCollection();
				collection->Combine(*other);
			}
			REQUIRE(collection->ChunkCount() ==
			        (combined ? 2 : (count * 2 + STANDARD_VECTOR_SIZE - 1) / STANDARD_VECTOR_SIZE));
			connection.BeginTransaction();
			auto plan = OptimizeLogicalPlanExportQuery(
			    connection, "SELECT x,sql_export_values_next(),sql_export_values_peek() FROM chunk_export_input");
			auto &projection = plan->Cast<LogicalProjection>();
			auto table_index = projection.children[0]->Cast<LogicalGet>().table_index;
			projection.children[0] = make_uniq<LogicalColumnDataGet>(
			    table_index, vector<LogicalType> {LogicalType::BIGINT}, std::move(collection));
			plan->ResolveOperatorTypes();
			auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
			if (combined && count == 1) {
				REQUIRE(exported.HasError());
				plan = OrderValues(std::move(plan));
				auto ordered = LogicalPlanSQLExporter::Export(*connection.context, *plan);
				REQUIRE(ordered.HasError());
				REQUIRE(*ordered.GetIssues()[0].construct ==
				        LogicalPlanVerificationConstructIdentity::ExportFeature("chunk_consumer_evaluation"));
			}
			REQUIRE_NO_FAIL(connection.Query("PRAGMA disable_optimizer"));
			counter->value = 0;
			auto native = connection.Query(make_uniq<LogicalPlanStatement>(std::move(plan)));
			REQUIRE_NO_FAIL(*native);
			REQUIRE(native->RowCount() == count * 2);
			REQUIRE(native->Collection().GetValue(2, 0) ==
			        Value::BIGINT(combined ? count : MinValue<idx_t>(count * 2, STANDARD_VECTOR_SIZE)));
			connection.Rollback();
			REQUIRE_NO_FAIL(connection.Query("PRAGMA enable_optimizer"));
			if (combined && count == 1) {
				REQUIRE(exported.HasError());
				REQUIRE(*exported.GetIssues()[0].construct ==
				        LogicalPlanVerificationConstructIdentity::ExportFeature("chunk_consumer_evaluation"));
				continue;
			}
			REQUIRE(exported.IsSuccess());
			auto text = exported.GetValue().query->ToString();
			auto ast = make_uniq<SelectStatement>();
			ast->node = std::move(exported.GetValue().query);
			for (bool direct_ast : {false, true}) {
				counter->value = 0;
				auto generated = direct_ast ? connection.Query(std::move(ast)) : connection.Query(text);
				REQUIRE_NO_FAIL(*generated);
				REQUIRE(generated->RowCount() == native->RowCount());
				REQUIRE(SQLExportRows(*generated, true) == SQLExportRows(*native, true));
			}
		}
	}
}

TEST_CASE("Owned chunk SQL export retains delivered rows before conversion errors",
          "[sql_export][logical_plan_sql_export][chunk_sql_export]") {
	for (idx_t count : vector<idx_t> {STANDARD_VECTOR_SIZE, STANDARD_VECTOR_SIZE + STANDARD_VECTOR_SIZE / 2}) {
		DuckDB db(nullptr);
		Connection connection(db);
		REQUIRE_NO_FAIL(
		    connection.Query("SET threads=1; SET max_streaming_buffer_size='1b'; CREATE TABLE chunk_input(x VARCHAR)"));
		auto good = connection.Query("SELECT '1'::VARCHAR FROM range(" + to_string(count) + ")");
		auto bad = connection.Query("SELECT 'bad'::VARCHAR FROM range(" + to_string(count) + ")");
		REQUIRE_NO_FAIL(*good);
		REQUIRE_NO_FAIL(*bad);
		auto collection = good->TakeCollection();
		auto second = bad->TakeCollection();
		collection->Combine(*second);
		connection.BeginTransaction();
		auto plan = OptimizeLogicalPlanExportQuery(connection, "SELECT x::INTEGER FROM chunk_input");
		auto &projection = plan->Cast<LogicalProjection>();
		auto table_index = projection.children[0]->Cast<LogicalGet>().table_index;
		projection.children[0] = make_uniq<LogicalColumnDataGet>(
		    table_index, vector<LogicalType> {LogicalType::VARCHAR}, std::move(collection));
		plan->ResolveOperatorTypes();
		auto pure = LogicalPlanSQLExporter::Export(*connection.context, *projection.children[0]);
		REQUIRE(pure.IsSuccess());
		auto pure_result = connection.Query(pure.GetValue().query->ToString());
		REQUIRE_NO_FAIL(*pure_result);
		REQUIRE(pure_result->RowCount() == count * 2);
		REQUIRE(pure_result->Collection().GetValue(0, count - 1) == Value("1"));
		REQUIRE(pure_result->Collection().GetValue(0, count) == Value("bad"));
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
		if (count == STANDARD_VECTOR_SIZE) {
			REQUIRE(exported.IsSuccess());
		} else {
			REQUIRE(exported.HasError());
			REQUIRE(*exported.GetIssues()[0].construct ==
			        LogicalPlanVerificationConstructIdentity::ExportFeature("chunk_consumer_evaluation"));
		}
		QueryParameters parameters;
		auto drain = [&](unique_ptr<QueryResult> result) {
			unique_ptr<QueryResultStream<>> stream;
			if (!result->HasError()) {
				stream = make_uniq<QueryResultStream<>>(std::move(result));
			}

			idx_t rows = 0;
			while (stream && !stream->HasError()) {
				auto chunk = stream->Fetch();
				if (!chunk) {
					break;
				}
				for (idx_t row = 0; row < chunk->size(); row++) {
					REQUIRE(chunk->GetValue(0, row) == Value::INTEGER(1));
				}
				rows += chunk->size();
			}
			REQUIRE((stream ? stream->HasError() : result->HasError()));
			REQUIRE(
			    StringUtil::Contains((stream ? stream->GetError() : result->GetError()), "Could not convert string"));
			return rows;
		};
		REQUIRE_NO_FAIL(connection.Query("PRAGMA disable_optimizer"));
		auto native_rows = drain(
		    SubmitSQLExportResult(*connection.context, make_uniq<LogicalPlanStatement>(std::move(plan)), parameters));
		connection.Rollback();
		REQUIRE_NO_FAIL(connection.Query("PRAGMA enable_optimizer"));
		if (count == STANDARD_VECTOR_SIZE) {
			auto text_rows =
			    drain(SubmitSQLExportResult(*connection.context, exported.GetValue().query->ToString(), parameters));
			auto statement = make_uniq<SelectStatement>();
			statement->node = std::move(exported.GetValue().query);
			auto ast_rows = drain(SubmitSQLExportResult(*connection.context, std::move(statement), parameters));
			REQUIRE(text_rows == native_rows);
			REQUIRE(ast_rows == native_rows);
		} else {
			REQUIRE(native_rows == count);
		}
		REQUIRE_NO_FAIL(connection.Query("SELECT 42"));
	}
}

TEST_CASE("Owned chunk SQL export requires an explicit consumer evaluation contract",
          "[sql_export][logical_plan_sql_export][chunk_sql_export]") {
	for (bool combined : {false, true}) {
		DuckDB db(nullptr);
		Connection connection(db);
		REQUIRE_NO_FAIL(connection.Query("SET threads=1"));
		auto counter = make_shared_ptr<SQLExportValuesCounter>();
		connection.context->registered_state->Insert("sql_export_values_counter", counter);
		ExtensionLoader loader(*db.instance, "opaque_chunk_export_counter");
		ScalarFunction next("sql_export_values_next", {}, LogicalType::BIGINT, SQLExportValuesNext);
		next.SetVolatile();
		loader.RegisterFunction(std::move(next));
		ScalarFunction peek("sql_export_values_peek", {}, LogicalType::BIGINT, SQLExportValuesPeek);
		peek.SetVolatile();
		loader.RegisterFunction(std::move(peek));
		auto first = connection.Query(combined ? "SELECT 0::BIGINT" : "SELECT * FROM range(2)");
		REQUIRE_NO_FAIL(*first);
		auto collection = first->TakeCollection();
		if (combined) {
			auto second = connection.Query("SELECT 1::BIGINT");
			REQUIRE_NO_FAIL(*second);
			auto part = second->TakeCollection();
			collection->Combine(*part);
		}
		connection.BeginTransaction();
		auto seed =
		    OptimizeLogicalPlanExportQuery(connection, "SELECT sql_export_values_next(),sql_export_values_peek()");
		auto op = make_uniq<SQLExportOpaqueProjection>(std::move(seed->expressions));
		op->children.push_back(make_uniq<LogicalColumnDataGet>(
		    TableIndex(2000), vector<LogicalType> {LogicalType::BIGINT}, std::move(collection)));
		op->ResolveOperatorTypes();
		REQUIRE(op->expressions.empty());
		idx_t callbacks = 0;
		op->export_sql = [&](SQLExportExtensionOperator &input, LogicalPlanSQLExportContext &context,
		                     const LogicalPlanVerificationPath &path) -> PlanExportResult {
			auto child = context.ExportChild(*input.children[0], LogicalPlanSQLExportHelpers::PlanChildPath(path, 0));
			if (child.HasError()) {
				return PlanExportResult::Failure(child.GetIssues());
			}
			callbacks++;
			REQUIRE(input.expressions.empty());
			auto binding_context =
			    LogicalPlanSQLExportHelpers::CreateBindingContext(context.GetClientContext(), {child.GetValue()});
			auto select = make_uniq<SelectNode>();
			auto column = binding_context.resolve_binding(ColumnBinding(TableIndex(2000), ProjectionIndex(0)));
			REQUIRE(column);
			select->select_list.push_back(make_uniq<ColumnRefExpression>(column->names));
			for (auto name : {"sql_export_values_next", "sql_export_values_peek"}) {
				vector<unique_ptr<ParsedExpression>> arguments;
				select->select_list.push_back(make_uniq<FunctionExpression>(Identifier(name), std::move(arguments)));
			}
			select->from_table = LogicalPlanSQLExportHelpers::CreateSubquery(std::move(child.GetValue()));
			return input.ExportQuery(std::move(select), path);
		};
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *op);
		if (combined) {
			REQUIRE(exported.HasError());
			REQUIRE(*exported.GetIssues()[0].construct ==
			        LogicalPlanVerificationConstructIdentity::ExportFeature("chunk_consumer_evaluation"));
			REQUIRE(callbacks == 0);
		} else {
			REQUIRE(exported.IsSuccess());
			REQUIRE(callbacks == 1);
		}
		REQUIRE_NO_FAIL(connection.Query("PRAGMA disable_optimizer"));
		counter->value = 0;
		auto native = connection.Query(make_uniq<LogicalPlanStatement>(std::move(op)));
		REQUIRE_NO_FAIL(*native);
		REQUIRE(native->RowCount() == 2);
		REQUIRE(native->Collection().GetValue(2, 0) == Value::BIGINT(combined ? 1 : 2));
		connection.Rollback();
		REQUIRE_NO_FAIL(connection.Query("PRAGMA enable_optimizer"));
		if (!combined) {
			auto text = exported.GetValue().query->ToString();
			auto ast = make_uniq<SelectStatement>();
			ast->node = std::move(exported.GetValue().query);
			for (bool direct_ast : {false, true}) {
				counter->value = 0;
				auto generated = direct_ast ? connection.Query(std::move(ast)) : connection.Query(text);
				REQUIRE_NO_FAIL(*generated);
				REQUIRE(SQLExportRows(*generated, true) == SQLExportRows(*native, true));
			}
		}
	}
}

TEST_CASE("Owned chunk SQL export accounts for intrinsic SINGLE join errors",
          "[sql_export][logical_plan_sql_export][chunk_sql_export]") {
	for (idx_t count : vector<idx_t> {STANDARD_VECTOR_SIZE, STANDARD_VECTOR_SIZE + STANDARD_VECTOR_SIZE / 2}) {
		for (bool combined : {false, true}) {
			for (bool error_on_multiple : {false, true}) {
				CAPTURE(count, combined, error_on_multiple);
				DuckDB db(nullptr);
				Connection connection(db);
				// Isolate source chunk boundaries from join-result coalescing.
				REQUIRE_NO_FAIL(connection.Query(
				    "SET threads=1; SET max_streaming_buffer_size='1b'; SET enable_caching_operators=false"));
				REQUIRE_NO_FAIL(connection.Query(string("SET scalar_subquery_error_on_multiple_rows=") +
				                                 (error_on_multiple ? "true" : "false")));
				auto first = connection.Query(combined ? "SELECT 0::BIGINT FROM range(" + to_string(count) + ")"
				                                       : "SELECT (i=" + to_string(count) + ")::BIGINT FROM range(" +
				                                             to_string(count + 1) + ")t(i)");
				REQUIRE_NO_FAIL(*first);
				auto left = first->TakeCollection();
				if (combined) {
					auto last = connection.Query("SELECT 1::BIGINT");
					REQUIRE_NO_FAIL(*last);
					auto part = last->TakeCollection();
					left->Combine(*part);
				}
				auto right = connection.Query("SELECT x::BIGINT FROM (VALUES(0),(1),(1))t(x)");
				REQUIRE_NO_FAIL(*right);
				connection.BeginTransaction();
				auto join = make_uniq<LogicalComparisonJoin>(JoinType::SINGLE);
				join->children.push_back(make_uniq<LogicalColumnDataGet>(
				    TableIndex(1001), vector<LogicalType> {LogicalType::BIGINT}, std::move(left)));
				join->children.push_back(make_uniq<LogicalColumnDataGet>(
				    TableIndex(1002), vector<LogicalType> {LogicalType::BIGINT}, right->TakeCollection()));
				join->conditions.emplace_back(
				    make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT,
				                                        ColumnBinding(TableIndex(1001), ProjectionIndex(0))),
				    make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT,
				                                        ColumnBinding(TableIndex(1002), ProjectionIndex(0))),
				    ExpressionType::COMPARE_EQUAL);
				join->ResolveOperatorTypes();
				REQUIRE_FALSE(join->conditions[0].GetLHS().CanThrow());
				REQUIRE_FALSE(join->conditions[0].GetRHS().CanThrow());
				auto exported = LogicalPlanSQLExporter::Export(*connection.context, *join);
				bool sensitive = combined && count != STANDARD_VECTOR_SIZE && error_on_multiple;
				if (sensitive) {
					REQUIRE(exported.HasError());
					REQUIRE(*exported.GetIssues()[0].construct ==
					        LogicalPlanVerificationConstructIdentity::ExportFeature("chunk_consumer_evaluation"));
				} else {
					REQUIRE(exported.IsSuccess());
				}
				QueryParameters parameters;
				auto drain = [&](unique_ptr<QueryResult> result) {
					unique_ptr<QueryResultStream<>> stream;
					if (!result->HasError()) {
						stream = make_uniq<QueryResultStream<>>(std::move(result));
					}

					idx_t rows = 0;
					while (stream && !stream->HasError()) {
						auto chunk = stream->Fetch();
						if (!chunk) {
							break;
						}
						bool equal = true;
						for (idx_t i = 0; i < chunk->size(); i++) {
							auto expected = Value::BIGINT(rows + i == count ? 1 : 0);
							equal &= chunk->GetValue(0, i) == expected && chunk->GetValue(1, i) == expected;
						}
						REQUIRE(equal);
						rows += chunk->size();
					}
					REQUIRE((stream ? stream->HasError() : result->HasError()) == error_on_multiple);
					if (error_on_multiple) {
						REQUIRE((stream ? stream->GetErrorType() : result->GetErrorType()) ==
						        ExceptionType::INVALID_INPUT);
						REQUIRE(StringUtil::Contains((stream ? stream->GetError() : result->GetError()),
						                             "More than one row returned"));
					} else {
						REQUIRE(rows == count + 1);
					}
					return rows;
				};
				REQUIRE_NO_FAIL(connection.Query("PRAGMA disable_optimizer"));
				auto native_rows = drain(SubmitSQLExportResult(
				    *connection.context, make_uniq<LogicalPlanStatement>(std::move(join)), parameters));
				connection.Rollback();
				REQUIRE_NO_FAIL(connection.Query("PRAGMA enable_optimizer"));
				if (sensitive) {
					REQUIRE(native_rows == count);
				} else {
					auto text_rows = drain(
					    SubmitSQLExportResult(*connection.context, exported.GetValue().query->ToString(), parameters));
					auto statement = make_uniq<SelectStatement>();
					statement->node = std::move(exported.GetValue().query);
					auto ast_rows = drain(SubmitSQLExportResult(*connection.context, std::move(statement), parameters));
					REQUIRE(text_rows == native_rows);
					REQUIRE(ast_rows == native_rows);
				}
				REQUIRE_NO_FAIL(connection.Query("SELECT 42"));
			}
		}
	}
}

TEST_CASE("Owned chunk SQL export preserves selected rows after a cross product",
          "[sql_export][logical_plan_sql_export][chunk_sql_export]") {
	for (bool combined : {false, true}) {
		for (idx_t consumer = 0; consumer < 3; consumer++) {
			CAPTURE(combined, consumer);
			DuckDB db(nullptr);
			Connection connection(db);
			REQUIRE_NO_FAIL(connection.Query("SET threads=1; SET disabled_optimizers='compressed_materialization'"));
			auto first = connection.Query(combined ? "SELECT 0::BIGINT" : "SELECT * FROM range(2)");
			REQUIRE_NO_FAIL(*first);
			auto left = first->TakeCollection();
			if (combined) {
				auto last = connection.Query("SELECT 1::BIGINT");
				REQUIRE_NO_FAIL(*last);
				auto part = last->TakeCollection();
				left->Combine(*part);
			}
			auto right = connection.Query("SELECT range+10 FROM range(2)");
			REQUIRE_NO_FAIL(*right);
			connection.BeginTransaction();
			unique_ptr<LogicalOperator> plan = make_uniq<LogicalCrossProduct>(
			    make_uniq<LogicalColumnDataGet>(TableIndex(1001), vector<LogicalType> {LogicalType::BIGINT},
			                                    std::move(left)),
			    make_uniq<LogicalColumnDataGet>(TableIndex(1002), vector<LogicalType> {LogicalType::BIGINT},
			                                    right->TakeCollection()));
			plan->ResolveOperatorTypes();
			if (consumer == 2) {
				plan = OrderValues(std::move(plan));
			}
			if (consumer != 0) {
				auto limit = make_uniq<LogicalLimit>(BoundLimitNode::ConstantValue(2), BoundLimitNode());
				limit->children.push_back(std::move(plan));
				plan = std::move(limit);
				plan->ResolveOperatorTypes();
			}
			auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
			bool sensitive = combined && consumer == 1;
			if (sensitive) {
				REQUIRE(exported.HasError());
				REQUIRE(*exported.GetIssues()[0].construct ==
				        LogicalPlanVerificationConstructIdentity::ExportFeature("chunk_consumer_evaluation"));
			} else {
				REQUIRE(exported.IsSuccess());
			}
			REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=true"));
			auto native = connection.Query(make_uniq<LogicalPlanStatement>(std::move(plan)));
			REQUIRE_NO_FAIL(*native);
			REQUIRE(native->RowCount() == (consumer == 0 ? 4 : 2));
			if (sensitive) {
				REQUIRE(CHECK_COLUMN(native, 0, {0, 0}));
				REQUIRE(CHECK_COLUMN(native, 1, {10, 11}));
			}
			REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=false"));
			if (!sensitive) {
				auto generated = connection.Query(exported.GetValue().query->ToString());
				auto statement = make_uniq<SelectStatement>();
				statement->node = std::move(exported.GetValue().query);
				auto ast = connection.Query(std::move(statement));
				REQUIRE_NO_FAIL(*generated);
				REQUIRE_NO_FAIL(*ast);
				REQUIRE(generated->GetTypes() == native->GetTypes());
				REQUIRE(ast->GetTypes() == native->GetTypes());
				REQUIRE(SQLExportRows(*generated, consumer != 0) == SQLExportRows(*native, consumer != 0));
				REQUIRE(SQLExportRows(*ast, consumer != 0) == SQLExportRows(*native, consumer != 0));
			}
			connection.Rollback();
		}
	}
}

} // namespace logical_plan_sql_export_test
