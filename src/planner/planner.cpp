#include "duckdb/planner/planner.hpp"

#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/joinside.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/planner/subquery/flatten_dependent_join.hpp"
#include "duckdb/planner/subquery/recursive_dependent_join_planner.hpp"
#include "duckdb/planner/operator/logical_dependent_join.hpp"
#include "duckdb/planner/operator/logical_trigger.hpp"
#include "duckdb/planner/operator_extension.hpp"
#include "duckdb/planner/planner_extension.hpp"
#include "duckdb/planner/logical_plan_verifier.hpp"
#include "duckdb/planner/operator/logical_explain.hpp"
#include "duckdb/optimizer/optimizer.hpp"

namespace duckdb {

Planner::Planner(ClientContext &context) : binder(Binder::CreateBinder(context)), context(context) {
}

static void RenderSQLExplain(unique_ptr<LogicalOperator> &op, ClientContext &context, Binder &binder) {
	if (op->type == LogicalOperatorType::LOGICAL_PREPARE || op->type == LogicalOperatorType::LOGICAL_EXECUTE) {
		for (auto &child : op->children) {
			RenderSQLExplain(child, context, binder);
		}
	} else if (op->type == LogicalOperatorType::LOGICAL_EXPLAIN) {
		auto &explain = op->Cast<LogicalExplain>();
		if (explain.explain_type == ExplainType::EXPLAIN_SQL) {
			op = explain.CreateSQLResult(context, binder.GenerateTableIndex());
		}
	}
}

void Planner::Optimize() {
	auto &profiler = QueryProfiler::Get(context);
#ifdef DEBUG
	plan->Verify(context);
#endif
	bool optimize = Settings::Get<EnableOptimizerSetting>(context);
	if (Settings::Get<DebugDisableOptimizerSetting>(context)) {
		// verify disable optimizer - disable EXCEPT for explain, otherwise every single EXPLAIN query breaks
		if (plan->type != LogicalOperatorType::LOGICAL_EXPLAIN) {
			optimize = false;
		}
	}
	if (plan->RequireOptimizer()) {
		{
			auto optimizer_timer = profiler.StartTimer<MetricOptimizerTotalTime>();
			Optimizer optimizer(*binder, context);
			if (optimize) {
				plan = optimizer.Optimize(std::move(plan));
			} else {
				plan = optimizer.LowerMandatoryAggregateRewrites(std::move(plan));
			}
			D_ASSERT(plan);
		}
#ifdef DEBUG
		plan->Verify(context);
#endif
	}
	RenderSQLExplain(plan, context, *binder);
}

// Pre-decorrelation pass: replace LogicalTrigger with LogicalDependentJoin so the standard
// FlattenDependentJoins machinery can decorrelate the trigger body.
static void RewriteTriggersToDependent(Binder &binder, LogicalOperator &op) {
	for (auto &child : op.children) {
		if (child) {
			RewriteTriggersToDependent(binder, *child);
		}
	}
	for (idx_t i = 0; i < op.children.size(); i++) {
		if (!op.children[i] || op.children[i]->type != LogicalOperatorType::LOGICAL_TRIGGER) {
			continue;
		}
		auto &trig = op.children[i]->Cast<LogicalTrigger>();
		auto dep_join = make_uniq<LogicalDependentJoin>(JoinType::INNER);
		dep_join->correlated_columns = std::move(trig.correlated_columns);
		// Trigger bodies have side effects and must fire once per row. Dedup on a synthetic per-row
		// row_number() key instead of the NEW columns (mirrors PerformDuplicateElimination's
		// perform_delim=false path). otherwise rows with identical NEW values would underfire.
		auto binding = ColumnBinding(binder.GenerateTableIndex(), ProjectionIndex(0));
		CorrelatedColumnInfo info(binding, LogicalType::BIGINT, "delim_index", 0);
		dep_join->correlated_columns.AddColumn(std::move(info));
		dep_join->correlated_columns.SetDelimIndexToZero();
		dep_join->perform_delim = false;
		dep_join->children.push_back(std::move(trig.children[0]));
		dep_join->children.push_back(std::move(trig.children[1]));
		op.children[i] = std::move(dep_join);
	}
}

static void CheckTreeDepth(const LogicalOperator &op, idx_t max_depth, idx_t depth = 0) {
	if (depth >= max_depth) {
		throw ParserException("Maximum tree depth of %lld exceeded in logical planner", max_depth);
	}
	for (auto &child : op.children) {
		CheckTreeDepth(*child, max_depth, depth + 1);
	}
}

#ifdef D_ASSERT_IS_ENABLED
static bool ContainsDependentJoin(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_DEPENDENT_JOIN) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsDependentJoin(*child)) {
			return true;
		}
	}
	return false;
}

static bool VerifyCanonicalComparisonJoins(const LogicalOperator &plan) {
	if (plan.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	    plan.type == LogicalOperatorType::LOGICAL_DELIM_JOIN || plan.type == LogicalOperatorType::LOGICAL_ASOF_JOIN) {
		auto &join = plan.Cast<LogicalComparisonJoin>();
		unordered_set<TableIndex> left_bindings;
		unordered_set<TableIndex> right_bindings;
		LogicalJoin::GetTableReferences(*join.children[0], left_bindings);
		LogicalJoin::GetTableReferences(*join.children[1], right_bindings);
		for (auto &condition : join.conditions) {
			if (!condition.IsComparison()) {
				continue;
			}
			auto left_side = JoinSide::GetCurrentJoinSide(condition.GetLHS(), left_bindings, right_bindings);
			auto right_side = JoinSide::GetCurrentJoinSide(condition.GetRHS(), left_bindings, right_bindings);
			// A condition side can be independent of both children, for example a constant or an outer reference.
			// It is non-canonical only when it requires the opposite child or both children.
			if (left_side == JoinSide::RIGHT || left_side == JoinSide::BOTH || right_side == JoinSide::LEFT ||
			    right_side == JoinSide::BOTH) {
				return false;
			}
		}
	}
	for (auto &child : plan.children) {
		if (!VerifyCanonicalComparisonJoins(*child)) {
			return false;
		}
	}
	return true;
}

static bool VerifyPlannedExpressions(const LogicalOperator &plan) {
	bool valid = true;
	LogicalOperatorVisitor::EnumerateExpressions(plan, [&](const unique_ptr<Expression> *expression) {
		if ((*expression)->HasSubquery()) {
			valid = false;
		}
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    **expression, [&](const BoundColumnRefExpression &column_ref) { valid &= column_ref.Depth() == 0; });
	});
	if (!valid) {
		return false;
	}
	for (auto &child : plan.children) {
		if (!VerifyPlannedExpressions(*child)) {
			return false;
		}
	}
	return true;
}
#endif

static void RunPreBindExtensions(ClientContext &context, Binder &binder, SQLStatement &statement) {
	for (auto &planner_extension : PlannerExtension::Iterate(context)) {
		if (planner_extension.pre_bind_function) {
			PlannerExtensionInput input {context, binder, planner_extension.planner_info.get()};
			planner_extension.pre_bind_function(input, statement);
		}
	}
}

static void RunPostBindExtensions(ClientContext &context, Binder &binder, BoundStatement &statement) {
	for (auto &planner_extension : PlannerExtension::Iterate(context)) {
		if (planner_extension.post_bind_function) {
			PlannerExtensionInput input {context, binder, planner_extension.planner_info.get()};
			planner_extension.post_bind_function(input, statement);
		}
	}
}

void Planner::CreatePlan(SQLStatement &statement) {
	auto &profiler = QueryProfiler::Get(context);
	auto parameter_count = statement.named_param_map.size();

	BoundParameterMap bound_parameters(parameter_data);

	// first bind the tables and columns to the catalog
	bool parameters_resolved = true;
	try {
		auto binding_timer = profiler.StartTimer<MetricPlannerBindingTime>();
		binder->SetParameters(bound_parameters);
		RunPreBindExtensions(context, *binder, statement);
		auto bound_statement = binder->Bind(statement);
		binding_timer.EndTimer();

		RunPostBindExtensions(context, *binder, bound_statement);

		this->names = bound_statement.names;
		this->types = bound_statement.types;
		this->plan = std::move(bound_statement.plan);
	} catch (const std::exception &ex) {
		ErrorData error(ex);
		this->plan = nullptr;
		if (error.Type() == ExceptionType::PARAMETER_NOT_RESOLVED) {
			// parameter types could not be resolved
			this->names = {"unknown"};
			this->types = {LogicalTypeId::UNKNOWN};
			parameters_resolved = false;
		} else if (error.Type() != ExceptionType::INVALID) {
			// different exception type - try operator_extensions
			for (auto &extension_op : OperatorExtension::Iterate(context)) {
				auto bound_statement =
				    extension_op->Bind(context, *this->binder, extension_op->operator_info.get(), statement);
				if (bound_statement.plan != nullptr) {
					RunPostBindExtensions(context, *this->binder, bound_statement);

					this->names = bound_statement.names;
					this->types = bound_statement.types;
					this->plan = std::move(bound_statement.plan);
					break;
				}
			}
			if (!this->plan) {
				throw;
			}
		} else {
			throw;
		}
	}
	if (this->plan) {
		auto max_tree_depth = Settings::Get<MaxExpressionDepthSetting>(context);
		CheckTreeDepth(*plan, max_tree_depth);

		// A LogicalPlanStatement carries an already planned (and possibly optimized) plan
		if (statement.type != StatementType::LOGICAL_PLAN_STATEMENT) {
			RewriteTriggersToDependent(*this->binder, *this->plan);
			RecursiveDependentJoinPlanner::Plan(*this->binder, this->plan);
			this->plan = FlattenDependentJoins::DecorrelateIndependent(*this->binder, std::move(this->plan));
		}
		D_ASSERT(!ContainsDependentJoin(*this->plan));
		D_ASSERT(VerifyPlannedExpressions(*this->plan));
		D_ASSERT(VerifyCanonicalComparisonJoins(*this->plan));
	}
	this->properties = binder->GetStatementProperties();
	this->properties.parameter_count = parameter_count;
	properties.bound_all_parameters = !bound_parameters.rebind && parameters_resolved;

	Planner::VerifyPlan(context, plan, bound_parameters.GetParametersPtr());

	// set up a map of parameter number -> value entries
	for (auto &kv : bound_parameters.GetParameters()) {
		auto &identifier = kv.first;
		auto &param = kv.second;
		// check if the type of the parameter could be resolved
		if (!param->return_type.IsValid()) {
			properties.bound_all_parameters = false;
			continue;
		}
		param->SetValue(Value(param->return_type));
		value_map[identifier] = param;
	}
}

shared_ptr<PreparedStatementData> Planner::PrepareSQLStatement(unique_ptr<SQLStatement> statement) {
	auto copied_statement = statement->Copy();
	// create a plan of the underlying statement
	// set PREPARE binding mode so that $params without supplied values create placeholder slots
	// instead of falling back to user variables (user variables serve as defaults at EXECUTE time)
	binder->SetBindingMode(BindingMode::PREPARE);
	CreatePlan(std::move(statement));
	// now create the logical prepare
	auto prepared_data = make_shared_ptr<PreparedStatementData>(copied_statement->type);
	prepared_data->unbound_statement = std::move(copied_statement);
	prepared_data->names = names;
	prepared_data->types = types;
	prepared_data->value_map = std::move(value_map);
	prepared_data->properties = properties;
	return prepared_data;
}

void Planner::CreatePlan(unique_ptr<SQLStatement> statement) {
	D_ASSERT(statement);
	Optimizer optimizer(*binder, context);
	optimizer.OptimizeStatement(statement);

	switch (statement->type) {
	case StatementType::SELECT_STATEMENT:
	case StatementType::INSERT_STATEMENT:
	case StatementType::COPY_STATEMENT:
	case StatementType::DELETE_STATEMENT:
	case StatementType::UPDATE_STATEMENT:
	case StatementType::CREATE_STATEMENT:
	case StatementType::DROP_STATEMENT:
	case StatementType::ALTER_STATEMENT:
	case StatementType::TRANSACTION_STATEMENT:
	case StatementType::EXPLAIN_STATEMENT:
	case StatementType::VACUUM_STATEMENT:
	case StatementType::RELATION_STATEMENT:
	case StatementType::CALL_STATEMENT:
	case StatementType::EXPORT_STATEMENT:
	case StatementType::PRAGMA_STATEMENT:
	case StatementType::SET_STATEMENT:
	case StatementType::LOAD_STATEMENT:
	case StatementType::EXTENSION_STATEMENT:
	case StatementType::PREPARE_STATEMENT:
	case StatementType::EXECUTE_STATEMENT:
	case StatementType::LOGICAL_PLAN_STATEMENT:
	case StatementType::ATTACH_STATEMENT:
	case StatementType::DETACH_STATEMENT:
	case StatementType::COPY_DATABASE_STATEMENT:
	case StatementType::UPDATE_EXTENSIONS_STATEMENT:
	case StatementType::MERGE_INTO_STATEMENT:
	case StatementType::CONNECT_STATEMENT:
	case StatementType::DISCONNECT_STATEMENT:
	case StatementType::EXTERNAL_RESOURCE_STATEMENT:
		CreatePlan(*statement);
		break;
	default:
		throw NotImplementedException("Cannot plan statement of type %s!", StatementTypeToString(statement->type));
	}
}

static bool OperatorSupportsSerialization(LogicalOperator &op) {
	for (auto &child : op.children) {
		if (!OperatorSupportsSerialization(*child)) {
			return false;
		}
	}
	return op.SupportSerialization();
}

void Planner::VerifyPlan(ClientContext &context, unique_ptr<LogicalOperator> &op,
                         optional_ptr<bound_parameter_map_t> map) {
	if (!op) {
		return;
	}
	// verify the column bindings of the plan
	LogicalPlanVerifier::Verify(context, *op);
	if (!Settings::Get<DebugVerifySerializerSetting>(context)) {
		return;
	}
	//! SELECT only for now
	if (!OperatorSupportsSerialization(*op)) {
		return;
	}

	auto &config = DBConfig::GetConfig(context);
	// format (de)serialization of this operator
	try {
		MemoryStream stream(Allocator::Get(context));

		SerializationOptions options;
		if (config.options.storage_compatibility.manually_set) {
			// Override the default of 'latest' if this was manually set (for testing, mostly)
			options.storage_compatibility = config.options.storage_compatibility;
		} else {
			options.storage_compatibility = StorageCompatibility::Latest();
		}

		BinarySerializer::Serialize(*op, stream, options);
		stream.Rewind();
		bound_parameter_map_t parameters;
		auto new_plan = BinaryDeserializer::Deserialize<LogicalOperator>(stream, context, parameters);

		if (map) {
			*map = std::move(parameters);
		}
		op = std::move(new_plan);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		switch (error.Type()) {
		case ExceptionType::NOT_IMPLEMENTED: // NOLINT: explicitly allowing these errors (for now)
			break;                           // pass
		default:
			throw;
		}
	}
}

} // namespace duckdb
